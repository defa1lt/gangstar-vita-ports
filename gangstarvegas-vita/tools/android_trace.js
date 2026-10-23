'use strict';
// The APK builder prepends GAME_TRACE_PROFILE and TRACE_SINK_SOURCE.
const output = '/sdcard/Android/data/com.gameloft.android.ANMP.GloftGGHM/files/vegas-trace/';
let log, image, native, flushState, finishState, mutexInit, openFile, closeFile;
let stopped = false, installed = false, observer, timer, poller, sequence = 0;
const states = new Map(), listeners = [], attached = new Set();
const started = Date.now();
let budget, budgetMutex;
let garbageTimer;

function collectLater() {
  clearTimeout(garbageTimer);
  garbageTimer = setTimeout(() => Stalker.garbageCollect(), 250);
}

function emit(event, fields = {}) {
  if (!log) return;
  log.write(JSON.stringify({event, ms: Date.now() - started, ...fields}) + '\n');
  log.flush();
}
function createMutex() {
  const p = Memory.alloc(64);
  if (mutexInit(p, ptr(0)) !== 0) throw new Error('pthread_mutex_init failed');
  return p;
}
function beginScope(tid, context) {
  if (stopped) return false;
  let state = states.get(tid);
  if (!state) {
    const path = output + 'calls-' + tid + '.bin';
    const fd = openFile(Memory.allocUtf8String(path), 0x241, 0x180);
    if (fd < 0) throw new Error('Cannot create trace file: ' + path);
    try {
      const memory = Memory.alloc(44 + 65536), lock = createMutex();
      memory.writeByteArray(new Uint8Array(44 + 65536));
      memory.writePointer(lock); memory.add(4).writeS32(fd);
      const range = Process.findRangeByAddress(context.sp);
      if (!range || range.protection.indexOf('r') === -1) throw new Error('No readable stack range');
      memory.add(36).writeU32(range.base.toUInt32());
      memory.add(40).writeU32(range.base.add(range.size).toUInt32());
      state = {memory, lock, fd, depth: 0, path}; states.set(tid, state);
      emit('thread_trace', {tid, path, stack: range.base.toString(), stack_size: range.size});
    } catch (error) { closeFile(fd); throw error; }
  }
  if (state.depth++ === 0) {
    Stalker.follow(tid, {events: {call: true, ret: true}, onEvent: native.record_event, data: state.memory});
  }
  return true;
}
function endScope(tid) {
  const state = states.get(tid);
  if (state && state.depth > 0 && --state.depth === 0) {
    Stalker.unfollow(tid);
    collectLater();
  }
}
function stop(reason) {
  if (stopped) return;
  stopped = true;
  if (budget && budget.add(8).readU32()) reason = 'byte_limit';
  clearTimeout(timer); clearInterval(poller);
  if (observer) observer.detach();
  for (const listener of listeners) listener.detach();
  for (const [tid, state] of states) {
    try { Stalker.unfollow(tid); } catch (error) { emit('cleanup_error', {tid, message: String(error)}); }
    finishState(state.memory); closeFile(state.fd);
    emit('thread_end', {tid, bytes: state.memory.add(12).readU32(), records: state.memory.add(16).readU32(), written_records: state.memory.add(12).readU32() / 128,
      no_context: state.memory.add(20).readU32(), bad_stack: state.memory.add(24).readU32(),
      io_error: state.memory.add(28).readU32(), truncated: reason !== 'first_step_return' || state.memory.add(28).readU32() !== 0});
  }
  emit('trace_end', {reason, bytes: budget ? budget.add(4).readU32() : 0});
  if (log) { log.close(); log = null; }
  collectLater();
  // Keep native callbacks and data alive until process exit; unfollow may still unwind.
}
function attach(address, callbacks) {
  const key = address.toString();
  if (attached.has(key)) return;
  const listener = Interceptor.attach(address, callbacks);
  attached.add(key); listeners.push(listener);
}
function scopeCallbacks(name, finishAfter) {
  return {
    onEnter(args) {
      this.tid = Process.getCurrentThreadId(); this.active = false;
      try { this.active = beginScope(this.tid, this.context); }
      catch (error) { emit('trace_error', {message: String(error)}); setTimeout(() => stop('trace_error'), 0); }
      if (this.active) emit('jni_enter', {name, tid: this.tid, caller: this.returnAddress.toString(),
        registers: [this.context.r0, this.context.r1, this.context.r2, this.context.r3].map(String)});
    },
    onLeave(retval) {
      if (!this.active) return;
      emit('jni_leave', {name, tid: this.tid, result: retval.toString()});
      endScope(this.tid);
      if (finishAfter) setTimeout(() => stop('first_step_return'), 0);
    }
  };
}
function install(module) {
  if (installed || stopped) return;
  installed = true; image = module;
  if (Process.arch !== 'arm' || Process.pointerSize !== 4) throw new Error('ARM32 required');
  budget.add(12).writeU32(image.base.toUInt32()); budget.add(16).writeU32(image.base.add(image.size).toUInt32());
  emit('game_module', {base: image.base.toString(), size: image.size, path: image.path,
    text_sha256: GAME_TRACE_PROFILE.text_sha256});
  for (const module of Process.enumerateModules()) {
    emit('module_map', {name: module.name, base: module.base.toString(), size: module.size});
    if (module.name !== image.name && module.name !== 'linker') Stalker.exclude({base: module.base, size: module.size});
  }
  let jniAttached = 0;
  for (const entry of GAME_TRACE_PROFILE.jni) {
    try {
      attach(image.base.add(entry.offset), scopeCallbacks(entry.name, entry.name.endsWith('GL2JNILib_step')));
      jniAttached++;
    } catch (error) {
      emit('hook_unavailable', {name: entry.name, offset: entry.offset, message: String(error)});
      if (entry.name === 'JNI_OnLoad' || /GL2JNILib_(step|getViewSettings)$/.test(entry.name)) throw error;
    }
  }
  for (const hook of GAME_TRACE_PROFILE.hooks) {
    let count = 0;
    attach(image.base.add(hook.offset), {
      onEnter(args) {
        this.sample = ++count <= 128 || (count & (count - 1)) === 0;
        this.tid = Process.getCurrentThreadId();
        if (this.sample) emit('native_enter', {name: hook.name, tid: this.tid, count,
          caller: this.returnAddress.toString(), args: [args[0], args[1], args[2], args[3]].map(String)});
      },
      onLeave(result) {
        if (this.sample) emit('native_leave', {name: hook.name, tid: this.tid, result: result.toString()});
      }
    });
  }
  if (GAME_TRACE_PROFILE.ctors.length > 1) {
    attach(image.base.add(GAME_TRACE_PROFILE.ctors[0]), {
      onEnter() { this.tid = Process.getCurrentThreadId(); beginScope(this.tid, this.context); emit('constructors_begin', {tid: this.tid}); }
    });
    attach(image.base.add(GAME_TRACE_PROFILE.ctors[GAME_TRACE_PROFILE.ctors.length - 1]), {
      onLeave() { const tid = Process.getCurrentThreadId(); emit('constructors_end', {tid}); endScope(tid); }
    });
  }
  const pthreadCreate = Process.getModuleByName('libc.so').getExportByName('pthread_create');
  attach(pthreadCreate, {
    onEnter(args) {
      const routine = args[2];
      if (routine.compare(image.base) >= 0 && routine.compare(image.base.add(image.size)) < 0) {
        emit('pthread_create', {tid: Process.getCurrentThreadId(), start: routine.toString()});
        attach(routine, scopeCallbacks('pthread_start@' + routine.sub(image.base).toString(), false));
      }
    }
  });
  emit('hooks_installed', {jni: jniAttached, jni_total: GAME_TRACE_PROFILE.jni.length, detailed: GAME_TRACE_PROFILE.hooks.length});
}
rpc.exports = {
  init() {
    try {
      log = new File(output + 'events.jsonl', 'w');
      emit('trace_start', {frida: Frida.version, arch: Process.arch, pid: Process.id, record_bytes: 128,
        duration_ms: 120000, max_bytes: 536870912, coverage: 'ARM32 call/ret in game image; JNI, constructors and game pthread scopes; raw ABI registers, not typed objects'});
      const libc = Process.getModuleByName('libc.so');
      mutexInit = new NativeFunction(libc.getExportByName('pthread_mutex_init'), 'int', ['pointer', 'pointer']);
      openFile = new NativeFunction(libc.getExportByName('open'), 'int', ['pointer', 'int', 'int']);
      closeFile = new NativeFunction(libc.getExportByName('close'), 'int', ['int']);
      budget = Memory.alloc(20); budget.writeByteArray(new Uint8Array(20)); budget.writeU32(536870912); budgetMutex = createMutex();
      native = new CModule(TRACE_SINK_SOURCE, {trace_budget: budget, budget_mutex: budgetMutex,
        pthread_mutex_lock: libc.getExportByName('pthread_mutex_lock'), pthread_mutex_unlock: libc.getExportByName('pthread_mutex_unlock'),
        write: libc.getExportByName('write'), clock_gettime: libc.getExportByName('clock_gettime'), __errno: libc.getExportByName('__errno')});
      flushState = new NativeFunction(native.flush_state, 'void', ['pointer']);
      finishState = new NativeFunction(native.finish_state, 'void', ['pointer']);
      timer = setTimeout(() => stop('time_limit'), 120000);
      poller = setInterval(() => {
        if (stopped) return;
        for (const state of states.values()) flushState(state.memory);
        if (budget.add(8).readU32()) stop('byte_limit');
      }, 1000);
      observer = Process.attachModuleObserver({onAdded(module) {
        if (module.name === 'libGangstar4.so') {
          try { install(module); }
          catch (error) { emit('trace_error', {message: String(error)}); setTimeout(() => stop('trace_error'), 0); }
        }
      }});
    } catch (error) { emit('trace_error', {message: String(error)}); console.error('Vegas trace: ' + error); stop('init_error'); }
  }
};
