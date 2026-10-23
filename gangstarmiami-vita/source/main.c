#include "utils/init.h"
#include "utils/glutil.h"
#include "utils/logger.h"
#include "utils/dialog.h"
#include "utils/audio.h"
#include "utils/gamepad_actions.h"
#include "reimpl/gl.h"
#include "reimpl/io.h"
#include "video.h"

#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/clib.h>
#include <psp2/touch.h>
#include <psp2/ctrl.h>
#include <stdlib.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>

int _newlib_heap_size_user = 256 * 1024 * 1024;
int sceUserMainThreadStackSize = 4 * 1024 * 1024;

#ifdef USE_SCELIBC_IO
int sceLibcHeapSize = 16 * 1024 * 1024;
#endif

so_module so_mod;

extern int port_register_abort_handler(void);
extern void port_trace(const char *format, ...);

/*
 * This .so has no JNI_OnLoad and never calls RegisterNatives -- confirmed with
 * `objdump -T` (see PORTING_PLAN.md section 3). It relies entirely on the static
 * `Java_...` symbol naming convention, so every entry point the real Activity/
 * GLSurfaceView/Renderer would have called is resolved and invoked here by hand,
 * following the exact order confirmed by reading the decompiled Java lifecycle
 * (Gangster2.java / GameRenderer.java / GameGLSurfaceView.java).
 */

typedef void (*fn_void_int)(JNIEnv *, jclass, jint);
typedef void (*fn_void_void)(JNIEnv *, jclass);
typedef void (*fn_void_int_int)(JNIEnv *, jclass, jint, jint);
typedef void (*fn_void_touch)(JNIEnv *, jclass, jint, jint, jint, jlong, jint, jint);
typedef void (*fn_void_instance)(JNIEnv *, jobject);
typedef void (*fn_void_info)(JNIEnv *, jclass, jstring, jstring, jstring, jstring);

static fn_void_int      Gangster2_nativeSetPhone;
static fn_void_info     Gangster2_nativeGetInfo;
static fn_void_int_int  Gangster2_nativeResume;
static fn_void_instance GameRenderer_nativeGetJNIEnv;
static fn_void_int      GLResLoader_nativeInit;
static fn_void_int      GLMediaPlayer_nativeInit;
static fn_void_int      Gangster2_nativeInit;
static fn_void_int      GameRenderer_nativeInit;
static fn_void_int_int  GameRenderer_nativeResize;
static fn_void_void     GameRenderer_nativeRender;
static fn_void_touch    GameGLSurfaceView_nativeOnTouch;
static fn_void_int      Gangster2_nativeKeyDown;
static fn_void_int      Gangster2_nativeKeyUp;

/* Native TouchScreenBase has eight slots. UP retires an ID only during
 * nativeRender, so its slot remains reserved until that frame completes. */
static uint32_t touch_active_ids, touch_retiring_ids;
static int relay_touch(JNIEnv *env, jclass cls, jint action, jint x, jint y,
                       jlong id, jint arg1, jint arg2) {
    if (id < 0 || id >= 32) return 0;
    uint32_t bit = 1u << (unsigned)id;
    if (action == 1) {
        if (touch_retiring_ids & bit) return 0;
        if (!(touch_active_ids & bit) &&
            __builtin_popcount(touch_active_ids | touch_retiring_ids) >= 8) return 0;
        touch_active_ids |= bit;
    } else {
        if (!(touch_active_ids & bit)) return 0;
        if (action == 0) {
            touch_active_ids &= ~bit;
            touch_retiring_ids |= bit;
        }
    }
    GameGLSurfaceView_nativeOnTouch(env, cls, action, x, y, id, arg1, arg2);
    return 1;
}

#define RESOLVE(dst, sym) \
    dst = (void *) so_symbol(&so_mod, sym); \
    if (!dst) fatal_error("Could not resolve required symbol: %s", sym);

#define SCREEN_W 960
#define SCREEN_H 544

// The game engine operates internally at 960x480 (hardcoded height 480 in
// Application::Init and GameRenderer_nativeResize). glViewport_soloader (gl.c)
// scales the default framebuffer vertically from 480 to 544 to fill the Vita
// panel. Touch inputs from the Vita panel (960x544) must therefore be mapped to
// the engine's internal space (960x480) so touch hitboxes match visual buttons.
#define ENGINE_W 960
#define ENGINE_H 480
#define MAX_TOUCH_SLOTS 5

int main() {
    int abort_result = port_register_abort_handler();
    soloader_init_all();
    port_trace("main: Miami 00.22 indexed probes / queued logging, abort handler=0x%08X, stack=%u",
               abort_result, (unsigned)sceUserMainThreadStackSize);
    l_checkpoint(1, "main: soloader_init_all() done");

    RESOLVE(Gangster2_nativeSetPhone, "Java_com_gameloft_android_TBFV_GloftGMHP_ML_Gangster2_nativeSetPhone")
    RESOLVE(Gangster2_nativeGetInfo, "Java_com_gameloft_android_TBFV_GloftGMHP_ML_Gangster2_nativeGetInfo")
    RESOLVE(Gangster2_nativeResume, "Java_com_gameloft_android_TBFV_GloftGMHP_ML_Gangster2_nativeResume")
    RESOLVE(GameRenderer_nativeGetJNIEnv, "Java_com_gameloft_android_TBFV_GloftGMHP_ML_GameRenderer_nativeGetJNIEnv")
    RESOLVE(GLResLoader_nativeInit, "Java_com_gameloft_android_TBFV_GloftGMHP_ML_GLResLoader_nativeInit")
    RESOLVE(GLMediaPlayer_nativeInit, "Java_com_gameloft_android_TBFV_GloftGMHP_ML_GLMediaPlayer_nativeInit")
    RESOLVE(Gangster2_nativeInit, "Java_com_gameloft_android_TBFV_GloftGMHP_ML_Gangster2_nativeInit")
    RESOLVE(GameRenderer_nativeInit, "Java_com_gameloft_android_TBFV_GloftGMHP_ML_GameRenderer_nativeInit")
    RESOLVE(GameRenderer_nativeResize, "Java_com_gameloft_android_TBFV_GloftGMHP_ML_GameRenderer_nativeResize")
    RESOLVE(GameRenderer_nativeRender, "Java_com_gameloft_android_TBFV_GloftGMHP_ML_GameRenderer_nativeRender")
    RESOLVE(GameGLSurfaceView_nativeOnTouch, "Java_com_gameloft_android_TBFV_GloftGMHP_ML_GameGLSurfaceView_nativeOnTouch")
    RESOLVE(Gangster2_nativeKeyDown, "Java_com_gameloft_android_TBFV_GloftGMHP_ML_Gangster2_nativeKeyDown")
    RESOLVE(Gangster2_nativeKeyUp, "Java_com_gameloft_android_TBFV_GloftGMHP_ML_Gangster2_nativeKeyUp")
    l_checkpoint(2, "main: all Java_* symbols resolved");

    // Fase 47: maps physical buttons onto the engine's own on-screen virtual
    // HUD buttons (attack, enter car/shop, take cover, sprint, ...) by
    // synthesizing real touches at the active button's position -- see
    // gamepad_actions.h. Gets the same nativeOnTouch() entry point the
    // touchscreen relay below uses (dedicated pointer ids 16+, real fingers
    // own slots 0-4). Soft-fails (touchscreen relay below still works)
    // if the engine's internal symbols ever move.
    gamepad_actions_init(relay_touch);

    gl_init();
    gl_report_load_stats(0, 0);
    io_report_load_stats(0, 0);
    l_checkpoint(3, "main: gl_init() done");

    // Loads SceAvPlayer so Method_loadMovie (java.c) can actually play
    // intro.m4v instead of skipping straight to completion. Must come after
    // gl_init(): video_play()'s texture allocator maps memory via
    // sceGxmMapMemory, which needs the GXM context vitaGL's init brings up.
    video_init();

    // onCreate(): nativeSetPhone(dm.widthPixels), before any GL/surface work.
    Gangster2_nativeSetPhone(&jni, NULL, ENGINE_W);
    l_checkpoint(4, "main: Gangster2_nativeSetPhone() done");

    jstring country = jni->NewStringUTF(&jni, "US");
    jstring device_id = jni->NewStringUTF(&jni, "000000000000000");
    jstring device = jni->NewStringUTF(&jni, "Sony Ericsson_R800i");
    jstring firmware = jni->NewStringUTF(&jni, "2.3.4");
    Gangster2_nativeGetInfo(&jni, NULL, country, device_id, device, firmware);
    l_note("lifecycle: Gangster2.nativeGetInfo complete");

    // onSurfaceCreated(), in the exact order the real Renderer calls them.
    GameRenderer_nativeGetJNIEnv(&jni, NULL);
    l_checkpoint(5, "main: GameRenderer_nativeGetJNIEnv() done");
    GLResLoader_nativeInit(&jni, NULL, 0);
    l_checkpoint(6, "main: GLResLoader_nativeInit() done");
    GLMediaPlayer_nativeInit(&jni, NULL, 0);
    l_checkpoint(7, "main: GLMediaPlayer_nativeInit() done");
    // Fase 31: no hay VM Java que ejecute GLMediaPlayer.init(), asi que el
    // SoundPool/MediaPlayer nunca se crearia solo. Levantar el backend real
    // (SceAudioOut + vorbis) aqui; es barato y sin el los queries de estado
    // harian que el motor reintentara la radio cada frame (ver audio.c).
    audio_init();
    Gangster2_nativeInit(&jni, NULL, 1); // 1 == demo mode, matches a fresh install's default state
    l_checkpoint(8, "main: Gangster2_nativeInit() done -- most likely place the engine spawns its worker thread(s)");
    GameRenderer_nativeInit(&jni, NULL, 1);
    l_checkpoint(9, "main: GameRenderer_nativeInit() done");

    // onSurfaceChanged(w, h)
    GameRenderer_nativeResize(&jni, NULL, ENGINE_W, ENGINE_H);
    l_checkpoint(10, "main: GameRenderer_nativeResize() done -- entering main loop");

    Gangster2_nativeResume(&jni, NULL, 2, 1);
    l_note("lifecycle: Gangster2.nativeResume(2, 1)");

    SceCtrlPadInputMode mode_before = SCE_CTRL_MODE_DIGITAL;
    SceCtrlPadInputMode mode_after = SCE_CTRL_MODE_DIGITAL;
    int get_before = sceCtrlGetSamplingMode(&mode_before);
    int set_mode = sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    int get_after = sceCtrlGetSamplingMode(&mode_after);
    l_note("[input] sampling before=%d query=%d set=%d after=%d query=%d",
           mode_before, get_before, set_mode, mode_after, get_after);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);

    int touchSlotHwId[MAX_TOUCH_SLOTS] = {-1, -1, -1, -1, -1};
    int touchLastX[MAX_TOUCH_SLOTS] = {-1, -1, -1, -1, -1};
    int touchLastY[MAX_TOUCH_SLOTS] = {-1, -1, -1, -1, -1};

    SceCtrlData pad = { .lx = 128, .ly = 128, .rx = 128, .ry = 128 };
    uint32_t oldButtons = 0;

    // Frame-progress instrumentation (id 22, otherwise unused): distinguishes
    // "stuck inside the Nth nativeRender" (only the 'enter' line appears) from
    // "loop alive but rendering black" (frame N lines keep appearing).
    //
    // This uses l_note(), NOT l_checkpoint()/l_debug(), on purpose: a Release
    // build compiles l_debug out, and a release run is exactly the situation
    // where this question ("is the render loop even turning?") is the first
    // thing worth answering. Volume is bounded -- the first 10 frames in
    // detail, then a time-based heartbeat (at most one line every 5 s) plus a
    // line for any frame whose nativeRender takes > 0.5 s (asset loading shows
    // up as slow frames -- frame 2 took ~7.8 s in logs/debug_local_020.log).
    // A frame-count modulo would go silent for minutes while loading, because
    // a single nativeRender can take seconds; wall-clock time does not.
    int frame_no = 0;
    SceUInt64 last_beat = 0;
    int last_beat_frame = 0;
    SceUInt64 last_phase_report = 0;

    gl_report_load_stats(0, 0);
    io_report_load_stats(0, 0);
    while (1) {
        frame_no++;
        SceUInt64 frame_start = sceKernelGetProcessTimeWide();
        if (frame_no <= 10)
            l_note("[022] main loop: frame %d enter (t=%llu)", frame_no, frame_start);

        SceTouchData touch = {0};
        if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) <= 0) touch.reportNum = 0;

        int seen[MAX_TOUCH_SLOTS] = {0, 0, 0, 0, 0};
        for (int r = 0; r < touch.reportNum && r < MAX_TOUCH_SLOTS; r++) {
            int hwId = touch.report[r].id;
            int x = (int)((touch.report[r].x * ENGINE_W) / 1920);
            int y = (int)((touch.report[r].y * ENGINE_H) / 1088);
            if (x < 0) x = 0;
            else if (x >= ENGINE_W) x = ENGINE_W - 1;
            if (y < 0) y = 0;
            else if (y >= ENGINE_H) y = ENGINE_H - 1;

            int slot = -1;
            for (int s = 0; s < MAX_TOUCH_SLOTS; s++) {
                if (touchSlotHwId[s] == hwId) { slot = s; break; }
            }
            if (slot == -1) {
                for (int s = 0; s < MAX_TOUCH_SLOTS; s++) {
                    if (touchSlotHwId[s] == -1) { slot = s; break; }
                }
                if (slot == -1) continue;
                touchSlotHwId[slot] = hwId;
                touchLastX[slot] = -1;
                touchLastY[slot] = -1;
            }
            seen[slot] = 1;

            if (touchLastX[slot] == -1) {
                if (!relay_touch(&jni, NULL, 1, x, y, slot, 0, 0)) continue;
            } else if (touchLastX[slot] != x || touchLastY[slot] != y) {
                relay_touch(&jni, NULL, 2, x, y, slot, 0, 0);
            }
            touchLastX[slot] = x;
            touchLastY[slot] = y;
        }
        for (int s = 0; s < MAX_TOUCH_SLOTS; s++) {
            if (touchSlotHwId[s] != -1 && !seen[s]) {
                relay_touch(&jni, NULL, 0, touchLastX[s], touchLastY[s], s, 0, 0);
                touchLastX[s] = -1;
                touchLastY[s] = -1;
                touchSlotHwId[s] = -1;
            }
        }

        int pad_result = sceCtrlPeekBufferPositive(0, &pad, 1);
        if (pad_result <= 0)
            pad = (SceCtrlData){ .lx = 128, .ly = 128, .rx = 128, .ry = 128 };
        static unsigned analog_logs;
        static int previous_deflection;
        int deflection = (abs((int)pad.lx - 128) > 24 || abs((int)pad.ly - 128) > 24 ? 1 : 0) |
                         (abs((int)pad.rx - 128) > 24 || abs((int)pad.ry - 128) > 24 ? 2 : 0);
        if (deflection != previous_deflection && analog_logs < 16) {
            l_note("[input] raw analog lx=%u ly=%u rx=%u ry=%u poll=%d",
                   pad.lx, pad.ly, pad.rx, pad.ry, pad_result);
            analog_logs++;
        }
        previous_deflection = deflection;
        // The native Android key path only handles Back/Menu. Directional
        // navigation and confirmation use the real MenuFX input path below.
        if ((pad.buttons & SCE_CTRL_START) && !(oldButtons & SCE_CTRL_START))
            Gangster2_nativeKeyDown(&jni, NULL, 82);
        if (!(pad.buttons & SCE_CTRL_START) && (oldButtons & SCE_CTRL_START))
            Gangster2_nativeKeyUp(&jni, NULL, 82);
        gamepad_actions_update(pad.buttons, oldButtons);
        // Movement uses the native HUD drag path; menus use native navigation.
        gamepad_stick_update(pad.buttons & (SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT),
                              pad.lx, pad.ly, pad.rx, pad.ry, touch.reportNum != 0);
        oldButtons = pad.buttons;

        SceUInt64 input_finished = sceKernelGetProcessTimeWide();
        GameRenderer_nativeRender(&jni, NULL);
        touch_retiring_ids = 0;
        if (frame_no == 1) {
            Gangster2_nativeResume(&jni, NULL, 2, 0);
            l_note("lifecycle: Gangster2.nativeResume(2, 0) after first frame");
        }
        // Fase 31: evitar que el governor baje relojes / suspenda durante
        // cargas largas (el frame 2 y la rafaga de shaders tardan minutos).
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
        SceUInt64 native_finished = sceKernelGetProcessTimeWide();
        SceUInt64 render_us = native_finished - frame_start;
        gl_report_load_stats(frame_no, render_us);
        io_report_load_stats(frame_no, render_us);
        audio_report_frame_stats(frame_no);
        if (frame_no <= 10) {
            l_note("[022] main loop: frame %d returned (%llu us)", frame_no, render_us);
        } else {
            if (render_us > 500000)
                l_note("[022] main loop: frame %d slow render (%llu ms)", frame_no, render_us / 1000);
            SceUInt64 now = sceKernelGetProcessTimeWide();
            if (last_beat == 0) {
                last_beat = now;
                last_beat_frame = frame_no;
            }
            if (now - last_beat > 5000000) {
                // Human-readable heartbeat (2026-09-06): fps measured between
                // beats (not estimated), render cost of this frame, and total
                // GL draws/clears -- one line every 5 s answers "is anything
                // happening?" without reading tea leaves from microsecond
                // timings.
                float fps = (float)(frame_no - last_beat_frame) * 1000000.0f /
                            (float)(now - last_beat);
                l_note("[022] frame %d | %.1f fps | render %.1f ms",
                       frame_no, fps, (float)render_us / 1000.0f);
                last_beat = now;
                last_beat_frame = frame_no;
            }
        }
        SceUInt64 report_finished = sceKernelGetProcessTimeWide();
        gl_swap();
        SceUInt64 swap_finished = sceKernelGetProcessTimeWide();
        if (frame_no > 10 && swap_finished - frame_start >= 100000 &&
            swap_finished - last_phase_report >= 1000000) {
            last_phase_report = swap_finished;
            l_note("[PERF] frame=%d total_ms=%llu input_ms=%llu native_ms=%llu "
                   "report_ms=%llu swap_ms=%llu", frame_no,
                   (unsigned long long)((swap_finished - frame_start) / 1000),
                   (unsigned long long)((input_finished - frame_start) / 1000),
                   (unsigned long long)((native_finished - input_finished) / 1000),
                   (unsigned long long)((report_finished - native_finished) / 1000),
                   (unsigned long long)((swap_finished - report_finished) / 1000));
        }

        // Enable only for an explicit capture: framebuffer writes stall loading.
#define GMV_SHOT_TRIAGE 0
#if GMV_SHOT_TRIAGE
        if (frame_no == 150) {
            char shot[128];
            sceClibSnprintf(shot, sizeof(shot), DATA_PATH "logs/shot_%05d.bmp", frame_no);
            int sr = gl_shot(shot);
            l_note("[022] screenshot %s -> %d", shot, sr);
        }
#endif

        // Fase 49 (2026-09-19): the Android-side `Thread.sleep(33 - elapsed)`
        // this used to mirror (GameRenderer.onDrawFrame(), decompiled Java)
        // is a battery/thermal throttle for phone hardware, not a simulation
        // requirement -- confirmed in the decompiled source: onDrawFrame()
        // measures `System.currentTimeMillis()` itself and only sleeps the
        // *leftover* time, never enforcing a fixed step when a frame runs
        // long. The engine is delta-time driven, not tick-locked.
        //
        // On Vita this cap was pure waste stacked on top of a real limiter
        // that was already there: gl_swap() -> vglSwapBuffers() ->
        // scene_end() calls sceDisplayWaitVblankStartMulti(vsync_interval)
        // (lib/vitaGL/source/gxm.c, vsync_interval=1 by default) every frame,
        // which already blocks this thread until the next 60 Hz vblank. That
        // is the correct place to pace frames -- unlike the old
        // sceKernelDelayThread(33000 - elapsed) here, it can never leave the
        // CPU idle-spinning past the display's actual refresh, and it does
        // not clamp a fast frame down to 30 fps. Removing the extra sleep
        // lets any scene that renders in under 16.6 ms present at a full
        // 60 fps instead of being held to 30; scenes that are already
        // GPU/CPU-bound past 33 ms are unaffected either way.
        //
        // sceKernelPowerTick() above still runs every iteration regardless,
        // so this does not reintroduce the governor/suspend risk from Fase 31.
    }

    sceKernelExitDeleteThread(0);
}
