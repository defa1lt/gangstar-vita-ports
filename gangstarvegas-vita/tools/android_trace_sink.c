#include <gum/gumstalker.h>

#define BUFFER_SIZE 65536
#define RECORD_SIZE 128

typedef struct {
  guint32 limit, written, full, image_start, image_end;
} TraceBudget;
typedef struct {
  void *lock;
  int fd;
  guint32 used, written, records, no_context, bad_stack, error, stopped;
  guint32 stack_start, stack_end;
  guint8 buffer[BUFFER_SIZE];
} TraceState;
extern TraceBudget trace_budget;
extern guint8 budget_mutex[];
extern int pthread_mutex_lock(void *mutex);
extern int pthread_mutex_unlock(void *mutex);
extern int write(int fd, const void *bytes, unsigned int size);
extern int *__errno(void);
extern int clock_gettime(int clock, void *time);

static void flush_locked(TraceState *state) {
  guint32 wanted = state->used, done = 0;
  if (!wanted) return;
  pthread_mutex_lock(budget_mutex);
  guint32 available = trace_budget.limit - trace_budget.written;
  if (wanted > available) {
    wanted = available - available % RECORD_SIZE;
    trace_budget.full = 1;
    state->stopped = 1;
  }
  while (done < wanted) {
    int result = write(state->fd, state->buffer + done, wanted - done);
    if (result < 0 && *__errno() == 4) continue;
    if (result <= 0) {
      state->error = result < 0 ? *__errno() : 5;
      state->stopped = 1;
      break;
    }
    done += result;
  }
  trace_budget.written += done;
  if (trace_budget.written == trace_budget.limit) trace_budget.full = 1;
  state->written += done;
  state->used = 0;
  pthread_mutex_unlock(budget_mutex);
}

void record_event(const GumEvent *event, GumCpuContext *context, void *data) {
  TraceState *state = data;
  if (event->type != GUM_CALL && event->type != GUM_RET) return;
  guint32 location = (guint32)event->call.location;
  if (location < trace_budget.image_start || location >= trace_budget.image_end) return;
  int saved_errno = *__errno();
  pthread_mutex_lock(state->lock);
  if (!state->stopped) {
    guint32 *record = (guint32 *)(state->buffer + state->used);
    unsigned int i;
    for (i = 0; i < 32; i++) record[i] = 0;
    record[0] = event->type;
    record[1] = location;
    record[2] = (guint32)event->call.target;
    record[3] = event->call.depth;
    gint32 stamp[2];
    if (clock_gettime(1, stamp) == 0) {
      record[11] = (guint32)stamp[0] * 1000 + (guint32)stamp[1] / 1000000;
      record[4] |= 4;
    }
    if (context) {
      record[4] |= 1;
      for (i = 0; i < 4; i++) record[5 + i] = context->r[i];
      record[9] = context->sp;
      record[10] = context->lr;
      const guint8 *vfp = (const guint8 *)&context->v[0];
      for (i = 0; i < 16; i++) {
        const guint8 *word = vfp + i * 4;
        record[12 + i] = word[0] | ((guint32)word[1] << 8) |
                         ((guint32)word[2] << 16) | ((guint32)word[3] << 24);
      }
      if (!(context->sp & 3) && context->sp >= state->stack_start &&
          state->stack_end >= 16 && context->sp <= state->stack_end - 16) {
        const guint32 *stack = (const guint32 *)context->sp;
        for (i = 0; i < 4; i++) record[28 + i] = stack[i];
        record[4] |= 2;
      } else state->bad_stack++;
    } else state->no_context++;
    state->records++;
    state->used += RECORD_SIZE;
    if (state->used == BUFFER_SIZE) flush_locked(state);
  }
  pthread_mutex_unlock(state->lock);
  *__errno() = saved_errno;
}

void flush_state(TraceState *state) {
  int saved_errno = *__errno();
  pthread_mutex_lock(state->lock);
  flush_locked(state);
  pthread_mutex_unlock(state->lock);
  *__errno() = saved_errno;
}

void finish_state(TraceState *state) {
  int saved_errno = *__errno();
  pthread_mutex_lock(state->lock);
  state->stopped = 1;
  flush_locked(state);
  pthread_mutex_unlock(state->lock);
  *__errno() = saved_errno;
}
