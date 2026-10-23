/*
 * gamepad_actions.c -- see gamepad_actions.h for the full rationale (Fase 47).
 *
 * All engine facts below are cited from:
 * - decompiled/libGangster2_armeabi/ghidra/out_ghidra.c (CHudManager::load,
 *   CHudManager::update, VirtualButton::update/processTouch/
 *   processTouchRelease, OnFoot/Driving/Flying/Sniper handleVirtualButton)
 * - decompiled/disasm/full_libGangster2.so.md (`002bfb24
 *   <VirtualButton::processTouch(long)>`, `002bfa8c
 *   <VirtualButton::processTouchRelease()>`, `002b5ec4
 *   <CHudManager::update(double)>`)
 * - syms.txt (`nm -D`: every symbol resolved here is a real dynamic export)
 * - decompiled/apk_jadx/.../GameGLSurfaceView.java:64-88 (nativeOnTouch
 *   action codes 1=down / 2=move / 0=up and (action,x,y,pointerId,0,0) order)
 */

#include "utils/gamepad_actions.h"
#include "utils/logger.h"

#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>

#include <so_util/so_util.h>

extern so_module so_mod;

/* Engine touch space. MUST match ENGINE_W/ENGINE_H in source/main.c: the
 * synthesized touches go through the same nativeOnTouch() entry point as
 * the real touchscreen relay, so they live in the same coordinate space. */
#define GA_ENGINE_W 960
#define GA_ENGINE_H 480

/* Pointer ids for synthesized touches. The real-finger relay in main.c
 * owns slots 0..4 (MAX_TOUCH_SLOTS); these start well clear of that range
 * so a physical button can never steal or corrupt a real finger's touch
 * point inside TouchScreenBase (which keys points by id). One id per
 * mapping entry -- Triangle fires two entries (enter-car + enter-shop) and
 * each needs its own simultaneous touch point. */
#define GA_SLOT_BASE 16

/* HudElement vtable slots, as used by CHudManager::update() itself when it
 * dispatches a real touch to a widget (see `002b5ec4`): +0x14 is the
 * interactable/visible predicate consulted before dispatch (draw2d() uses
 * the same slot, so it tracks visibility, including the nearCar/nearShop/
 * nearCover-gated buttons), +0x1c is getTouchRegion() filling the
 * +0x10..0x1c design-space rect. Calling through the vtable (not a fixed
 * symbol) also keeps subclass overrides working if any of these members
 * ever turn out to be a ToggleButton/AnimatedButton instead of a plain
 * VirtualButton. */
#define GA_VT_GATE   (0x14 / 4)
#define GA_VT_REGION (0x1c / 4)

typedef int (*gate_fn)(void *);
typedef void (*region_fn)(float *, void *);
typedef void *(*getinstance_fn)(void);
typedef void (*getscale_fn)(void *, float *, float *);
typedef void *(*menu_fx_fn)(void *);
typedef int (*menu_depth_fn)(void *);
typedef bool (*menu_clearing_fn)(void *);
typedef bool (*menu_state_fn)(void *, const char *);
typedef void *(*menu_controller_fn)(void *, int);
typedef void (*menu_focus_fn)(void *);
typedef void (*menu_input_fn)(void *, int, int);
typedef void (*unregister_draw_fn)(void *, void *);
typedef bool (*camera_active_fn)(void *, int);

/* CHudManager::s_hudManager is a plain `CHudManager*` global (BSS) --
 * resolving its symbol gives the address of the pointer, not the instance. */
typedef void *CHudManagerPtr;

static gamepad_touch_fn s_touch = NULL;
static CHudManagerPtr *s_hudManagerAddr = NULL;
static getinstance_fn s_getInstance = NULL;
static getscale_fn s_getScale = NULL;
static int s_ready = 0;
static getinstance_fn s_getMenuManager;
static menu_fx_fn s_getMenuFX;
static menu_depth_fn s_getMenuDepth;
static menu_clearing_fn s_isClearingFocus;
static menu_state_fn s_isCurrentMenuState;
static menu_controller_fn s_getController;
static menu_focus_fn s_setFocusDefault;
static menu_input_fn s_updateMenuInput;
static void *s_menuFX;
static unsigned s_menuStickFlags;
static unregister_draw_fn s_unregisterHUDDraw;
static void **s_cameraManagerAddr;
static getinstance_fn s_getScriptManager;
static menu_clearing_fn s_consumedCursors, s_isInCinematic;
static camera_active_fn s_isActiveCamera;
static menu_focus_fn s_resetCameraTouch;
static void camera_release(int recenter);
typedef void *(*get_vehicle_fn)(const void *);
typedef void (*add_cash_fn)(void *, int, int);
typedef bool (*helicopter_fn)(void *);
typedef void (*raise_event_fn)(void *, const void *);
typedef void (*acceleration_fn)(void *, float, float, float);
typedef void *(*get_gamepad_fn)(void *, unsigned);
static void **s_playerAddr, **s_eventManagerAddr;
static get_vehicle_fn s_getVehicle;
static add_cash_fn s_addCash;
static helicopter_fn s_isHelicopter;
static menu_clearing_fn s_isWalkHUD, s_isFlyHUD, s_controlLocked, s_controlTransition;
static raise_event_fn s_raiseEvent;
static acceleration_fn s_baseAcceleration;
static menu_clearing_fn s_accelerometerEnabled;
static get_gamepad_fn s_getNativeGamepad;
static void *s_yawVtable, *s_heightVtable, *s_buttonVtable, *s_moveVtable, *s_releaseVtable;
static int *s_orientation;
static unsigned char *s_isInMainMenu;
typedef const float *(*rotation_get_fn)(const void *);
typedef void (*rotation_set_fn)(void *, const float *);
typedef void (*quaternion_mul_fn)(float *, const float *, const float *);
typedef float *(*quaternion_normalize_fn)(float *);
typedef float (*camera_fov_fn)(const void *);
typedef void (*camera_float_fn)(void *, float);
static rotation_get_fn s_getRotation;
static rotation_set_fn s_setRotation;
static quaternion_mul_fn s_multiplyRotation;
static quaternion_normalize_fn s_normalizeRotation;
static camera_fov_fn s_getFOV;
static camera_float_fn s_zoom, s_incVerticalAngle;
static int s_nativeReady, s_flying, s_sniping, s_sniperFire;
static uint64_t s_sniperTime;
static int s_sniperMoving;
static float s_sniperAngle, s_sniperIntensity;
static uint32_t s_buttons;
static float s_flightX, s_flightY, s_flightYaw;
static unsigned s_cheatStep;
static uint64_t s_cheatStepTime, s_cheatDeadline;
static int s_cheatSquareHeld;

/* One representative CHudManager member offset per VirtualButton instance,
 * grouped by EvVButton::ButtonType::Type id. Taken straight from
 * CHudManager::load() (out_ghidra.c, `CHudManager::load()`): each line is
 * `VirtualButton::VirtualButton(sprite, <frame>, <type>, true)` stored at
 * the cited member offset. Several ids own more than one instance (one per
 * HUD skin/mode) -- the +0x14 gate in pad_press() picks the currently
 * interactable one at press time, so all of them must be listed here:
 *
 *   off    frame  type   meaning
 *   0x30   0x71   2      enter/exit car (driving/flying skins)
 *   0x34   0x70   0      attack (driving/flying skins)
 *   0x38   0x72   4      vehicle special A (driving/flying skins)
 *   0x3c   0x73   5      vehicle special B (driving/flying skins)
 *   0x40   0xb6   4      vehicle special A (alt skin)
 *   0x44   0xb7   5      vehicle special B (alt skin)
 *   0x48   2      2      enter/exit car (alt skin)
 *   0x4c   0x77   3      enter shop (on-foot only)
 *   0x50   3      0      attack (alt skin)
 *   0x5c   0x6a   2      enter/exit car (alt skin)
 *   0x60   0x6b   0      attack (alt skin)
 *   0x64   0xb5   2      enter/exit car (alt skin)
 *   0x68   0xb4   0      attack (alt skin)
 *   0x6c   0xb9   0xe    take cover (on-foot + sniper)
 *   0x70   0xcc   0xf    sprint (on-foot only)
 */
static const int offs_attack[]    = { 0x34, 0x50, 0x60, 0x68 };
static const int offs_entercar[]  = { 0x30, 0x48, 0x5c, 0x64 };
static const int offs_veh_a[]     = { 0x38, 0x40 };
static const int offs_veh_b[]     = { 0x3c, 0x44 };
static const int offs_shop[]      = { 0x4c };
static const int offs_cover[]     = { 0x6c };
static const int offs_sprint[]    = { 0x70 };

struct pad_action {
    uint32_t mask;
    const char *name;
    const int *offs;
    int noffs;
    int slot;
    int active;
    int x;
    int y;
};

/* Physical mapping.
 * - On foot: Cross=sprint, Triangle=enter-car/shop, Square=cover, Circle=attack.
 * - In vehicle: Cross=accelerate, Circle=brake, Triangle=exit-car.
 *   (L/R triggers also act as brake/accelerate for driving convenience).
 * Each entry gates on its button's visibility (e.g. attack is visible on foot
 * and hidden in cars; accelerate is visible in cars and hidden on foot). */
static struct pad_action s_actions[] = {
    { SCE_CTRL_CROSS,    "attack",     offs_attack,   4, GA_SLOT_BASE + 0, 0, 0, 0 },
    { SCE_CTRL_CROSS,    "accelerate", offs_veh_a,    2, GA_SLOT_BASE + 1, 0, 0, 0 },
    { SCE_CTRL_CROSS,    "sprint",     offs_sprint,   1, GA_SLOT_BASE + 2, 0, 0, 0 },
    { SCE_CTRL_CIRCLE,   "brake",      offs_veh_b,    2, GA_SLOT_BASE + 3, 0, 0, 0 },
    { SCE_CTRL_TRIANGLE, "enter-car",  offs_entercar, 4, GA_SLOT_BASE + 4, 0, 0, 0 },
    { SCE_CTRL_TRIANGLE, "enter-shop", offs_shop,     1, GA_SLOT_BASE + 5, 0, 0, 0 },
    { SCE_CTRL_SQUARE,   "take-cover", offs_cover,    1, GA_SLOT_BASE + 6, 0, 0, 0 },
    { SCE_CTRL_RTRIGGER, "accelerate", offs_veh_a,    2, GA_SLOT_BASE + 7, 0, 0, 0 },
    { SCE_CTRL_LTRIGGER, "brake",      offs_veh_b,    2, GA_SLOT_BASE + 8, 0, 0, 0 },
};

#define GA_NACTIONS (sizeof(s_actions) / sizeof(s_actions[0]))

/*
 * Fase 51: the on-foot/sniper movement widget. CHudManager::load()
 * (out_ghidra.c) allocates exactly ONE `AnalogStick` (0x8c bytes) and stores
 * it at this offset -- `AnalogStick::AnalogStick(this_00, sprite, 0, 1)`,
 * i.e. frame 0 for the fixed base graphic and frame 1 for the moving knob.
 * It derives from the same `HudElement` base as VirtualButton (its ctor
 * calls `HudElement::HudElement(...)` first), so the same vtable gate
 * (+0x14) and getTouchRegion (+0x1c) mechanism `pad_press()` already uses
 * applies here unchanged -- confirmed by reading the ctor itself, which
 * fills this object's own +0x10..0x1c with a design-space rect computed
 * from the base sprite frame's module dimensions, the exact same field
 * layout VirtualButton uses for its touch region.
 *
 * `AnalogStick::processTouch()` (out_ghidra.c) does NOT hit-test the touch
 * position against the base's region on every call -- it looks up the
 * ALREADY-CAPTURED pointer by id in `TouchScreenBase::s_touchScreenBase` and
 * accumulates the delta between consecutive samples. That is standard touch
 * capture: once a DOWN lands inside the widget's region, subsequent MOVEs
 * for the same pointer id keep going to it regardless of how far the touch
 * point drifts -- exactly like a real thumb dragging a joystick knob past
 * its drawn base circle. So the down position only needs to land inside the
 * region once; everything from there is a plain drag.
 *
 */
#define GA_OFF_ANALOGSTICK 0x28
#define GA_OFF_WHEEL       0x2c
#define GA_OFF_SLIDECONTROL 0x54
#define GA_OFF_SLIDECONTROL2 0x58

/* Deadzone as a fraction of the raw analog range (SceCtrlData.lx/ly are
 * 0..255, 128 = centered) -- keeps a resting stick (which never sits at
 * exactly 128 on real hardware) from dribbling a tiny constant drag. */
#define GA_STICK_DEADZONE 24

struct stick_state {
    int slot;
    int active;
    int cx, cy;      /* base center, engine touch pixels (fixed while active) */
    float rx, ry;    /* max knob travel per axis, engine touch pixels */
    int last_x, last_y;
};

static struct stick_state s_moveStick  = { GA_SLOT_BASE + 9, 0, 0, 0, 0.0f, 0.0f, 0, 0 };
static struct stick_state s_wheelStick = { GA_SLOT_BASE + 10, 0, 0, 0, 0.0f, 0.0f, 0, 0 };
static struct stick_state s_cameraStick = { GA_SLOT_BASE + 11, 0, 0, 0, 0.0f, 0.0f, 0, 0 };
static void *s_cameraScreen, *s_cameraObject;
static uint64_t s_cameraTime;


/*
 * Fase 52 (crash confirmed on hardware, debug_local_052.log +
 * gangstarmiamivindication-psp2core-1789879707-...psp2dmp): pressing Cross
 * to skip the intro video, then having ANY physical button/stick register a
 * fresh edge on the very next frame, data-aborts inside the ENGINE's own
 * `Application::GetScreenDimensions(int&, int&) const`
 * (`_ZNK11Application19GetScreenDimensionsERiS0_`) -- `ldr r0, [r3, #16]`
 * with r3 == 0, called from `Application::GetScreenScaleFactors()`, called
 * from this file's own `pad_press()`/`stick_region()`. The crash PC/disasm
 * pin the field exactly: `r3 = *(Application* + 0x179bc)`, NULL until some
 * point during the engine's own heavy `Application::PostInit()` work
 * (historically observed running inside the SECOND call to nativeRender --
 * port_progress.md Fase 13/48). `CHudManager::s_hudManager` (the `hud` gate
 * below) is already non-NULL by then -- CHudManager is constructed earlier
 * in `Application::Init()` -- so that check alone does not protect this
 * call. Every physical-input path in this file goes through
 * `Application::GetScreenScaleFactors()` to convert a widget's design-space
 * rect to touch pixels, so this one guard, called first, protects all of
 * them (button taps AND the movement stick).
 */
#define GA_SCREENDIM_FIELD_OFF 0x179bc

static int screen_scale_ready(void) {
    void *app = s_getInstance();
    if (!app)
        return 0;
    return *(void **)((char *)app + GA_SCREENDIM_FIELD_OFF) != NULL;
}

static void pad_miss_diag(const struct pad_action *a, const char *reason) {
    static uint64_t last_us;
    uint64_t now = sceKernelGetProcessTimeWide();
    if (last_us && now - last_us < 5000000)
        return;
    last_us = now;
    l_note("[input] pad %s unavailable: %s", a->name, reason);
}

static void pad_press(struct pad_action *a) {
    if (!screen_scale_ready()) {
        pad_miss_diag(a, "screen not ready");
        return;
    }

    CHudManagerPtr hud = *s_hudManagerAddr;
    if (!hud) {
        pad_miss_diag(a, "HUD not ready");
        return;
    }

    float fx = 0.0f, fy = 0.0f;
    s_getScale(s_getInstance(), &fx, &fy);
    if (!(fx > 0.0f && fy > 0.0f)) {
        pad_miss_diag(a, "invalid screen scale");
        return;
    }

    for (int i = 0; i < a->noffs; i++) {
        int off = a->offs[i];
        void *button = *(void **)((char *)hud + off);
        if (!button)
            continue;
        void **vt = *(void ***)button;

        /* Same interactable check the engine runs before dispatching a
         * real touch to this widget -- hidden/inapplicable skins (wrong
         * mode, not near car/shop/cover) fail here and are skipped. */
        gate_fn gate = (gate_fn)vt[GA_VT_GATE];
        if (!gate || !gate(button))
            continue;

        float rect[4];
        region_fn region = (region_fn)vt[GA_VT_REGION];
        if (!region)
            continue;
        region(rect, button);
        if (!(rect[2] > rect[0] && rect[3] > rect[1]))
            continue;

        /* Design-space rect center -> engine touch pixels, exactly like
         * VirtualButton::update() scales the rect before hit-testing
         * (`__aeabi_fmul(scale, rect)` on each edge). */
        int x = (int)((rect[0] + rect[2]) * 0.5f * fx);
        int y = (int)((rect[1] + rect[3]) * 0.5f * fy);
        if (x < 0 || x >= GA_ENGINE_W || y < 0 || y >= GA_ENGINE_H)
            continue;

        if (!s_touch(&jni, NULL, 1, x, y, (jlong)a->slot, 0, 0)) return;
        a->active = 1;
        a->x = x;
        a->y = y;
        l_note("[input] pad %s down -> vbutton hud+0x%x @(%d,%d) slot %d",
               a->name, off, x, y, a->slot);
        return; /* first interactable skin wins: one tap, never multi-fire */
    }

    pad_miss_diag(a, "no visible HUD button with a valid touch region");
}

static void pad_release(struct pad_action *a) {
    if (!a->active)
        return;
    s_touch(&jni, NULL, 0, a->x, a->y, (jlong)a->slot, 0, 0);
    a->active = 0;
    l_note("[input] pad %s up (slot %d)", a->name, a->slot);
}

static void release_gameplay_touches(void) {
    camera_release(1);
    for (unsigned i = 0; i < GA_NACTIONS; i++)
        pad_release(&s_actions[i]);
    struct stick_state *sticks[] = { &s_moveStick, &s_wheelStick };
    for (unsigned i = 0; i < sizeof(sticks) / sizeof(sticks[0]); i++) {
        struct stick_state *stick = sticks[i];
        if (stick->active) {
            s_touch(&jni, NULL, 0, stick->last_x, stick->last_y,
                    (jlong)stick->slot, 0, 0);
            stick->active = 0;
        }
    }
}

static void *active_menu_fx(void) {
    if (!screen_scale_ready())
        return NULL;
    void *manager = s_getMenuManager();
    if (!manager)
        return NULL;
    void *fx = s_getMenuFX(manager);
    // Native getDepth dereferences the FX, so the null check must come first.
    if (!fx || s_getMenuDepth(manager) <= 0)
        return NULL;
    // Tutorial is an overlay during play; BlockingTutorial owns modal input.
    return s_isCurrentMenuState(manager, "Tutorial") ? NULL : fx;
}

/* getVehicle may conceal the vehicle in cinematics, so cinematic gating
 * precedes both the on-foot cheat and helicopter checks. +0x179e4 contains
 * the CControlManager pointer in the pinned Miami binary. */
static void *gameplay_player(void) {
    if (!s_ready || !s_nativeReady || s_menuFX || *s_isInMainMenu || !screen_scale_ready()) return NULL;
    void *app = s_getInstance(), *hud = *s_hudManagerAddr;
    void *player = *s_playerAddr;
    void *control = *(void **)((char *)app + 0x179e4);
    if (!hud || !player || !control || s_consumedCursors(s_getMenuManager()) ||
        s_controlLocked(control) || s_controlTransition(control)) return NULL;
    void *script = s_getScriptManager();
    return script && !s_isInCinematic(script) ? player : NULL;
}

static int helicopter_active(void) {
    void *player = gameplay_player();
    if (!player || !s_isFlyHUD(*s_hudManagerAddr)) return 0;
    void *vehicle = s_getVehicle(player);
    return vehicle && s_isHelicopter(vehicle);
}

static uint32_t money_cheat(uint32_t buttons, uint32_t old_buttons) {
    uint32_t pressed = buttons & ~old_buttons;
    if (!(buttons & SCE_CTRL_SQUARE)) s_cheatSquareHeld = 0;
    void *player = gameplay_player();
    if (!player || !s_isWalkHUD(*s_hudManagerAddr) || s_getVehicle(player)) {
        s_cheatStep = 0;
        s_cheatDeadline = 0;
    } else {
        uint64_t now = sceKernelGetProcessTimeWide() / 1000;
        if ((pressed & SCE_CTRL_SQUARE) && s_cheatDeadline && now < s_cheatDeadline) {
            s_cheatDeadline = 0;
            s_cheatSquareHeld = 1;
            s_addCash(player, 50000, 1);
            l_note("[CHEAT] added 50000 cash");
        } else if (pressed & SCE_CTRL_LTRIGGER) {
            s_cheatStep = 1;
            s_cheatStepTime = now;
            s_cheatDeadline = 0;
        } else if (pressed & SCE_CTRL_RTRIGGER) {
            s_cheatStep = s_cheatStep == 1 && now - s_cheatStepTime < 2000 ? 2 : 0;
            s_cheatStepTime = now;
        } else if ((pressed & SCE_CTRL_UP) && s_cheatStep == 2) {
            if (now - s_cheatStepTime < 2000) {
                s_cheatDeadline = now + 5000;
                l_note("[CHEAT] armed: Square adds 50000 cash");
            }
            s_cheatStep = 0;
        }
    }
    return s_cheatSquareHeld ? buttons & ~SCE_CTRL_SQUARE : buttons;
}

static void virtual_button(unsigned state, unsigned button) {
    struct { void *vtable; unsigned type, state, button; } event = {
        s_buttonVtable, 13, state, button
    };
    if (*s_eventManagerAddr) s_raiseEvent(*s_eventManagerAddr, &event);
}

static void sniper_move(float nx, float ny) {
    float intensity = sqrtf(nx * nx + ny * ny);
    if (intensity > 1.0f) intensity = 1.0f;
    if (intensity > 0.0f) {
        s_sniperAngle = -atan2f(nx, ny);
        s_sniperIntensity = intensity;
    } else if (!s_sniperMoving) return;
    struct { void *vtable; unsigned type; float angle, intensity; } event = {
        intensity > 0 ? s_moveVtable : s_releaseVtable,
        intensity > 0 ? 7 : 8, s_sniperAngle, s_sniperIntensity
    };
    if (*s_eventManagerAddr) s_raiseEvent(*s_eventManagerAddr, &event);
    s_sniperMoving = intensity > 0;
}

static void *sniper_player(void) {
    void *player = gameplay_player();
    void *manager = *s_cameraManagerAddr;
    return player && manager && s_isActiveCamera(manager, 4) &&
        *(void **)((char *)manager + 8) ? player : NULL;
}

static void sniper_buttons(uint32_t buttons, uint32_t old_buttons) {
    int sniping = sniper_player() != NULL;
    if (sniping != s_sniping) {
        release_gameplay_touches();
        if (s_sniperFire) virtual_button(3, 0);
        sniper_move(0, 0);
        s_sniperFire = 0;
        s_sniping = sniping;
        s_sniperTime = 0;
        l_note("[input] sniper controls=%d", sniping);
    }
    if (!sniping) return;
    int fire = (buttons & SCE_CTRL_CIRCLE) != 0;
    if (fire) virtual_button(s_sniperFire ? 1 : 0, 0);
    else if (s_sniperFire) virtual_button(2, 0);
    s_sniperFire = fire;
    if ((old_buttons & ~buttons) & SCE_CTRL_TRIANGLE) virtual_button(2, 12);
}

static void sniper_aim(uint8_t rx, uint8_t ry, int front_touch_active) {
    uint64_t now = sceKernelGetProcessTimeWide();
    uint64_t elapsed = s_sniperTime ? now - s_sniperTime : 16667;
    s_sniperTime = now;
    if (elapsed > 33333) elapsed = 33333;
    void *player = sniper_player();
    if (!player || front_touch_active) return;
    void *manager = *s_cameraManagerAddr;
    void *camera = *(void **)((char *)manager + 8);
    float dt = elapsed / 1000000.0f;
    float fov = s_getFOV(manager);
    if (!isfinite(fov) || fov < 6.0f || fov > 50.0f) return;
    int zoom = !!(s_buttons & SCE_CTRL_LTRIGGER) - !!(s_buttons & SCE_CTRL_RTRIGGER);
    if (zoom) {
        fov += zoom * 24.0f * dt;
        if (fov < 6.0f) fov = 6.0f;
        if (fov > 50.0f) fov = 50.0f;
        s_zoom(manager, fov);
    }
    int x = (int)rx - 128, y = (int)ry - 128;
    if (abs(x) <= GA_STICK_DEADZONE) x = 0;
    if (abs(y) <= GA_STICK_DEADZONE) y = 0;
    /* FPSCamera::scroll (0x353568): Player+4 is its LevelObject;
     * rotation is current * delta about Z, vertical angle clamps natively. */
    void *character = (char *)player + 4;
    if (x) {
        float yaw = x / 128.0f * 360.0f * dt * fov / GA_ENGINE_W * (3.14159265359f / 180.0f);
        float delta[4] = { 0, 0, sinf(yaw * 0.5f), cosf(yaw * 0.5f) }, result[4];
        s_normalizeRotation(delta);
        s_multiplyRotation(result, s_getRotation(character), delta);
        s_setRotation(character, result);
    }
    if (y) s_incVerticalAngle(camera, y / 128.0f * 360.0f * dt * fov / GA_ENGINE_H);
}

static void switch_weapon(uint32_t buttons, uint32_t old_buttons) {
    void *player = gameplay_player();
    if (!player || s_sniping || !s_isWalkHUD(*s_hudManagerAddr) || s_getVehicle(player)) return;
    uint32_t released = old_buttons & ~buttons;
    if (released & SCE_CTRL_LTRIGGER) virtual_button(2, 8);
    if (released & SCE_CTRL_RTRIGGER) virtual_button(2, 7);
}

static void flight_axis(void *vtable, unsigned type, unsigned direction, float intensity) {
    struct { void *vtable; unsigned type, direction; float intensity; } event = {
        vtable, type, direction, intensity
    };
    if (*s_eventManagerAddr) s_raiseEvent(*s_eventManagerAddr, &event);
}

/* Replace only the GLF accelerometer adapter. Base::moved retains native
 * angle bookkeeping and synchronous event dispatch. The original adapter
 * negates gamepad+0x40/44/48 and ignores its float arguments (0x36889c).
 * Flying's gyro path overwrites rotor commands every physics step; feed
 * sticks through that path rather than writing private helicopter fields. */
static void accelerometer_moved(void *self, float x, float y, float z) {
    if (helicopter_active() && (*s_orientation == 3 || *s_orientation == 1)) {
        float sign = *s_orientation == 3 ? 1.0f : -1.0f;
        s_baseAcceleration(self, sign * (2.0f / 3.0f + 0.5f * s_flightY),
                           sign * 0.5f * s_flightX, 0.0f);
        flight_axis(s_yawVtable, 9, s_flightYaw < 0, fabsf(s_flightYaw));
        /* Both vertical channels need neutral events: the flying handler
         * does not subscribe to stick-release events. */
        flight_axis(s_heightVtable, 11, 1, (s_buttons & SCE_CTRL_RTRIGGER) ? 1.0f : 0.0f);
        flight_axis(s_heightVtable, 11, 0, (s_buttons & SCE_CTRL_LTRIGGER) ? 1.0f : 0.0f);
        return;
    }
    void *input = *(void **)((char *)self + 0x4c);
    if (!input || !s_accelerometerEnabled(input)) return;
    float *pad = s_getNativeGamepad(input, 0);
    if (pad) s_baseAcceleration(self, -pad[0x40 / 4], -pad[0x44 / 4], -pad[0x48 / 4]);
    (void)x; (void)y; (void)z;
}

static void menu_input(unsigned flags) {
    if (!flags || s_isClearingFocus(s_getMenuManager()))
        return;
    void *controller = s_getController(s_menuFX, 0);
    if (!controller)
        return;
    // Pinned SO RenderFX::GetController/SetFocus store the focus at +0x10.
    if (!*(void **)((char *)controller + 0x10))
        s_setFocusDefault(s_menuFX);
    s_updateMenuInput(s_menuFX, (int)flags, 0);
    l_note("[input] native menu flags=0x%x", flags);
}

/* Locates CHudManager+GA_OFF_ANALOGSTICK, runs the same interactable gate
 * VirtualButton uses, and fills *cx/*cy (region center) and *rx/*ry (half
 * the region's width/height -- the knob's usable travel per axis) in engine
 * touch pixels. Returns 0 (out params untouched) if the widget is currently
 * hidden/not interactable (e.g. no AnalogStick in Driving/Flying mode). */
static int stick_region(int *cx, int *cy, float *rx, float *ry) {
    if (!screen_scale_ready())
        return 0;

    CHudManagerPtr hud = *s_hudManagerAddr;
    if (!hud)
        return 0;

    float fx = 0.0f, fy = 0.0f;
    s_getScale(s_getInstance(), &fx, &fy);
    if (!(fx > 0.0f && fy > 0.0f))
        return 0;

    void *stick = *(void **)((char *)hud + GA_OFF_ANALOGSTICK);
    if (!stick)
        return 0;
    void **vt = *(void ***)stick;

    gate_fn gate = (gate_fn)vt[GA_VT_GATE];
    if (!gate || !gate(stick))
        return 0;

    float rect[4];
    region_fn region = (region_fn)vt[GA_VT_REGION];
    if (!region)
        return 0;
    region(rect, stick);
    if (!(rect[2] > rect[0] && rect[3] > rect[1]))
        return 0;

    *cx = (int)((rect[0] + rect[2]) * 0.5f * fx);
    *cy = (int)((rect[1] + rect[3]) * 0.5f * fy);
    *rx = (rect[2] - rect[0]) * 0.5f * fx;
    *ry = (rect[3] - rect[1]) * 0.5f * fy;
    return (*cx >= 0 && *cx < GA_ENGINE_W && *cy >= 0 && *cy < GA_ENGINE_H);
}

/* Locates CHudManager+GA_OFF_WHEEL (or either SlideControl alternate at
 * +0x54/+0x58 if the wheel skin itself is hidden), runs the interactable
 * gate (+0x14), and fills *cx/*cy (region center) and *rx/*ry in engine
 * touch pixels. Returns 1 if interactable, 0 if hidden (e.g. on foot).
 * Fase 58: the second SlideControl at +0x58 is now a candidate too -- the
 * old code only tried +0x54, so a driving skin steering through +0x58 was
 * never found and the wheel "didn't move". */
static int wheel_region(int *cx, int *cy, float *rx, float *ry) {
    if (!screen_scale_ready())
        return 0;

    CHudManagerPtr hud = *s_hudManagerAddr;
    if (!hud)
        return 0;

    float fx = 0.0f, fy = 0.0f;
    s_getScale(s_getInstance(), &fx, &fy);
    if (!(fx > 0.0f && fy > 0.0f))
        return 0;

    const int offs[] = { GA_OFF_WHEEL, GA_OFF_SLIDECONTROL, GA_OFF_SLIDECONTROL2 };
    for (int i = 0; i < 3; i++) {
        void *wheel = *(void **)((char *)hud + offs[i]);
        if (!wheel)
            continue;
        void **vt = *(void ***)wheel;

        gate_fn gate = (gate_fn)vt[GA_VT_GATE];
        if (!gate || !gate(wheel))
            continue;

        float rect[4];
        region_fn region = (region_fn)vt[GA_VT_REGION];
        if (!region)
            continue;
        region(rect, wheel);
        if (!(rect[2] > rect[0] && rect[3] > rect[1]))
            continue;

        *cx = (int)((rect[0] + rect[2]) * 0.5f * fx);
        *cy = (int)((rect[1] + rect[3]) * 0.5f * fy);
        *rx = (rect[2] - rect[0]) * 0.5f * fx;
        *ry = (rect[3] - rect[1]) * 0.5f * fy;
        return (*cx >= 0 && *cx < GA_ENGINE_W && *cy >= 0 && *cy < GA_ENGINE_H);
    }
    return 0;
}

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Fase 59: steering input is present but NO wheel/slide candidate passed
 * the lookup -- log WHY, throttled to one line per 5s of wall time (never
 * per frame: logging changes timing under test). Log 058 proved the
 * symptom (DeviceKeyInput:21/22 while driving with zero "wheel down"
 * lines) but not which lookup step fails per skin (null widget? +0x14
 * gate? degenerate rect? out-of-range center?), so this line carries all
 * three per candidate: ptr=0/1 gate=0/1 + the raw design rect. One line
 * per 5s is enough to catch every driving skin the user tries. */
static void wheel_miss_diag(float nx) {
    static uint64_t last_us = 0;
    uint64_t now = sceKernelGetProcessTimeWide();
    if (last_us != 0 && now - last_us < 5000000)
        return;
    last_us = now;

    CHudManagerPtr hud = s_hudManagerAddr ? *s_hudManagerAddr : NULL;
    if (!hud) {
        l_note("[input] wheel miss (nx=%.2f): hud=NULL", nx);
        return;
    }
    float fx = 0.0f, fy = 0.0f;
    if (screen_scale_ready())
        s_getScale(s_getInstance(), &fx, &fy);

    static const int offs[] = { GA_OFF_WHEEL, GA_OFF_SLIDECONTROL, GA_OFF_SLIDECONTROL2 };
    char detail[192];
    int pos = 0;
    for (int i = 0; i < 3 && pos < (int)sizeof(detail) - 32; i++) {
        void *w = *(void **)((char *)hud + offs[i]);
        int has = (w != NULL);
        int gate = 0;
        float rect[4] = { 0, 0, 0, 0 };
        if (has) {
            void **vt = *(void ***)w;
            gate_fn g = (gate_fn)vt[GA_VT_GATE];
            gate = (g && g(w)) ? 1 : 0;
            if (gate) {
                region_fn r = (region_fn)vt[GA_VT_REGION];
                if (r) {
                    r(rect, w);
                    if (!(rect[2] > rect[0] && rect[3] > rect[1]))
                        gate = 2;  /* gate ok, rect degenerate */
                } else {
                    gate = 3;  /* gate ok, no region fn */
                }
            }
        }
        pos += snprintf(detail + pos, sizeof(detail) - (size_t)pos,
                        "%s+0x%x:ptr=%d gate=%d rect=(%.0f,%.0f,%.0f,%.0f)",
                        i ? " " : "", offs[i], has, gate,
                        rect[0], rect[1], rect[2], rect[3]);
    }
    l_note("[input] wheel miss (nx=%.2f, scale=%.2fx%.2f): %s", nx, fx, fy, detail);
}

/* CHudManager::update uses this vector of primary HudElement pointers for
 * hit testing, then sends the first unused touch to HudScreen. Pick an
 * anchor outside every active widget, including status/weapon widgets. */
static int camera_anchor(void *hud, int *x, int *y) {
    void **begin = *(void ***)((char *)hud + 0xd4);
    void **end = *(void ***)((char *)hud + 0xd8);
    if (!begin || (uintptr_t)end < (uintptr_t)begin ||
        (uintptr_t)end - (uintptr_t)begin > 1024) return 0;
    float fx, fy;
    s_getScale(s_getInstance(), &fx, &fy);
    if (!(fx > 0 && fy > 0)) return 0;
    int best = GA_ENGINE_W * GA_ENGINE_W + GA_ENGINE_H * GA_ENGINE_H;
    int found = 0;
    for (int cy = 16; cy < GA_ENGINE_H - 16; cy += 32) {
        for (int cx = 16; cx < GA_ENGINE_W - 16; cx += 32) {
            int distance = (cx - GA_ENGINE_W / 2) * (cx - GA_ENGINE_W / 2) +
                           (cy - GA_ENGINE_H / 2) * (cy - GA_ENGINE_H / 2);
            if (distance >= best) continue;
            int occupied = 0;
            for (void **item = begin; item != end; ++item) {
                void *widget = *item;
                if (!widget) continue;
                void **vt = *(void ***)widget;
                gate_fn gate = (gate_fn)vt[GA_VT_GATE];
                region_fn region = (region_fn)vt[GA_VT_REGION];
                if (!gate || !region || !gate(widget)) continue;
                float rect[4];
                region(rect, widget);
                if (cx >= rect[0] * fx && cx <= rect[2] * fx &&
                    cy >= rect[1] * fy && cy <= rect[3] * fy) {
                    occupied = 1;
                    break;
                }
            }
            if (!occupied) { best = distance; *x = cx; *y = cy; found = 1; }
        }
    }
    return found;
}

static void camera_release(int recenter) {
    if (!s_cameraStick.active) return;
    void *hud = *s_hudManagerAddr;
    void *screen = hud ? *(void **)((char *)hud + 0x90) : NULL;
    void *manager = *s_cameraManagerAddr;
    if (screen == s_cameraScreen) {
        /* Pinned HudScreen drag-release (0x2bdcfc) sets manager+0x24=1.
         * resetTouch (0x2bd534) cancels our gesture before UP, preventing
         * a short physical deflection from triggering native tap actions.
         * Real front fingers always cancel ours BEFORE their native frame. */
        if (recenter && manager && *(void **)((char *)manager + 8) == s_cameraObject)
            *((uint8_t *)manager + 0x24) = 1;
        s_resetCameraTouch(screen);
    }
    s_touch(&jni, NULL, 0, s_cameraStick.last_x, s_cameraStick.last_y,
            (jlong)s_cameraStick.slot, 0, 0);
    s_cameraStick.active = 0;
    s_cameraStick.rx = s_cameraStick.ry = 0.0f;
    s_cameraScreen = s_cameraObject = NULL;
}

static void camera_update(uint8_t rx, uint8_t ry, int front_touch_active) {
    uint64_t now = sceKernelGetProcessTimeWide();
    uint64_t elapsed = s_cameraTime ? now - s_cameraTime : 16667;
    s_cameraTime = now;
    if (elapsed > 33333) elapsed = 33333;
    int x = (int)rx - 128, y = (int)ry - 128;
    if (x >= -GA_STICK_DEADZONE && x <= GA_STICK_DEADZONE) x = 0;
    if (y >= -GA_STICK_DEADZONE && y <= GA_STICK_DEADZONE) y = 0;
    void *hud = *s_hudManagerAddr;
    void *screen = hud ? *(void **)((char *)hud + 0x90) : NULL;
    if (s_menuFX || front_touch_active || (!x && !y) || !screen_scale_ready() ||
        !screen || !*((uint8_t *)screen + 0x58)) {
        camera_release(1);
        return;
    }
    void *manager = *s_cameraManagerAddr;
    void *menu = s_getMenuManager(), *script = s_getScriptManager();
    if (!manager || !menu || !script || s_consumedCursors(menu) || s_isInCinematic(script) ||
        !(s_isActiveCamera(manager, 0) || s_isActiveCamera(manager, 1))) {
        camera_release(1);
        return;
    }
    void *camera = *(void **)((char *)manager + 8);
    if (!camera || (s_cameraStick.active &&
        (screen != s_cameraScreen || camera != s_cameraObject))) {
        camera_release(0);
        return;
    }
    if (!s_cameraStick.active) {
        /* HudScreen::isUsed (0x2bd52c) reads this byte. Wait for any
         * previous real gesture's native release before taking ownership. */
        if (*((uint8_t *)screen + 0x59)) return;
        if (!camera_anchor(hud, &s_cameraStick.last_x, &s_cameraStick.last_y)) return;
        s_cameraScreen = screen;
        s_cameraObject = camera;
        if (!s_touch(&jni, NULL, 1, s_cameraStick.last_x, s_cameraStick.last_y,
                     (jlong)s_cameraStick.slot, 0, 0)) {
            s_cameraScreen = s_cameraObject = NULL;
            return;
        }
        s_cameraStick.active = 1;
        l_note("[input] camera swipe down @(%d,%d) slot %d",
               s_cameraStick.last_x, s_cameraStick.last_y, s_cameraStick.slot);
        return;
    }
    /* Native displacement is current minus previous. Send MOVE every frame,
     * even for zero pixel movement, so subpixel frames never repeat a delta. */
    s_cameraStick.rx += (float)x / 128.0f * 360.0f * elapsed / 1000000.0f;
    s_cameraStick.ry += (float)y / 128.0f * 360.0f * elapsed / 1000000.0f;
    int dx = (int)s_cameraStick.rx, dy = (int)s_cameraStick.ry;
    s_cameraStick.rx -= dx;
    s_cameraStick.ry -= dy;
    int tx = s_cameraStick.last_x + dx, ty = s_cameraStick.last_y + dy;
    if (tx < 16 || tx >= GA_ENGINE_W - 16 || ty < 16 || ty >= GA_ENGINE_H - 16) {
        /* UP gets its own native frame; same-frame DOWN can erase it from
         * the native queue. Re-anchor next frame without return-behind. */
        camera_release(0);
        return;
    }
    s_touch(&jni, NULL, 2, tx, ty, (jlong)s_cameraStick.slot, 0, 0);
    s_cameraStick.last_x = tx;
    s_cameraStick.last_y = ty;
}

void gamepad_stick_update(uint32_t dpad_buttons, uint8_t lx, uint8_t ly,
                          uint8_t rx, uint8_t ry, int front_touch_active) {
    if (!s_ready)
        return;
    if (!s_flying && !s_sniping) camera_update(rx, ry, front_touch_active);
    else camera_release(0);
    if (s_menuFX) {
        unsigned flags = (ly < 64 ? 1u : ly > 192 ? 2u : 0u) |
                         (lx < 64 ? 4u : lx > 192 ? 8u : 0u);
        menu_input(flags & ~s_menuStickFlags);
        s_menuStickFlags = flags;
        return;
    }
    s_menuStickFlags = 0;

    /* Normalize to [-1, 1], deadzone around the 128 center; D-pad overrides
     * a given axis to full deflection when pressed (checked after the
     * analog read so a digital press always wins ties). Screen Y grows
     * downward, so "up" (ny > 0) must SUBTRACT from the touch Y target. */
    int dxRaw = (int)lx - 128, dyRaw = (int)ly - 128;
    float nx = (dxRaw > GA_STICK_DEADZONE || dxRaw < -GA_STICK_DEADZONE) ? (float)dxRaw / 128.0f : 0.0f;
    float ny = (dyRaw > GA_STICK_DEADZONE || dyRaw < -GA_STICK_DEADZONE) ? -(float)dyRaw / 128.0f : 0.0f;
    if (dpad_buttons & SCE_CTRL_LEFT)  nx = -1.0f;
    if (dpad_buttons & SCE_CTRL_RIGHT) nx = 1.0f;
    if (dpad_buttons & SCE_CTRL_UP)    ny = 1.0f;
    if (dpad_buttons & SCE_CTRL_DOWN)  ny = -1.0f;
    if (nx > 1.0f) nx = 1.0f; else if (nx < -1.0f) nx = -1.0f;
    if (ny > 1.0f) ny = 1.0f; else if (ny < -1.0f) ny = -1.0f;

    s_flightX = nx;
    s_flightY = -ny;
    float length = sqrtf(nx * nx + ny * ny);
    if (length > 1.0f) { s_flightX /= length; s_flightY /= length; }
    int yaw = (int)rx - 128;
    s_flightYaw = abs(yaw) > GA_STICK_DEADZONE ? yaw / 128.0f : 0.0f;
    if (s_flying) return;
    if (s_sniping) {
        sniper_move(nx, ny);
        sniper_aim(rx, ry, front_touch_active);
        return;
    }

    int want_active = (nx != 0.0f || ny != 0.0f);

    /* Fase 58: query BOTH widgets up front. On foot the movement AnalogStick
     * is interactable and the Wheel is hidden, in a vehicle it is the other
     * way around -- but during HUD transitions both gates can briefly report
     * interactable, and the old code checked the stick first, so a stale
     * stick gate starved the wheel (user report: cruceta never turned the
     * wheel). Steering (nx) therefore goes to the wheel whenever the wheel
     * is interactable, regardless of what the stick gate says. */
    int scx, scy; float srx, sry;
    int stick_ok = stick_region(&scx, &scy, &srx, &sry);
    int wcx, wcy; float wrx, wry;
    int wheel_ok = wheel_region(&wcx, &wcy, &wrx, &wry);

    /* Fase 59: steering deflection with neither widget interactable --
     * diagnose (throttled) instead of silently dropping it. On foot this
     * stays quiet (the move-stick takes nx); in a vehicle with a hidden
     * stick it pinpoints which wheel-lookup step fails per driving skin. */
    if (nx != 0.0f && !wheel_ok && !stick_ok)
        wheel_miss_diag(nx);

    /* 1. In vehicle: steering wheel / slide control, driven with the real
     * finger gesture -- grab near the top of the wheel rim and curve
     * down-left / down-right like a real steering wheel.
     *
     * Wheel::processTouch() (out_ghidra.c) only reads deltaX =
     * first.x - current.x (dir = sign, amount = |dx| / *(this+0x68), with
     * 0x68 = 0x46 = 70px for full lock -- the Wheel ctor hardcodes it), so
     * the X component below is what actually steers: DOWN lands near the
     * top of the wheel region (cx, cy - 0.7*ry, clamped inside so the DOWN
     * hit-tests onto the widget), then MOVE goes to
     * (down_x + nx*85, down_y + 50*|nx|). +-85px in X clears the 70px full-
     * lock threshold on digital input and stays proportional on the analog
     * stick; the +50px downward curve replicates the real finger path and
     * also exercises the Y axis for the SlideControl steering alternate
     * (its processTouch reads both axes depending on the mode at this+0x68).
     * Pointer-id capture (same as the AnalogStick, Fase 51) keeps routing
     * MOVEs to the wheel once the DOWN lands, so drifting outside the drawn
     * region is fine -- exactly what a real thumb does past the rim. */
    if (wheel_ok && nx != 0.0f) {
        if (s_moveStick.active) {
            s_touch(&jni, NULL, 0, s_moveStick.last_x, s_moveStick.last_y, (jlong)s_moveStick.slot, 0, 0);
            l_note("[input] move-stick up (slot %d)", s_moveStick.slot);
            s_moveStick.active = 0;
        }

        if (!s_wheelStick.active) {
            /* DOWN = grab point near the top of the rim. Stored in cx/cy
             * (fixed for the whole drag); tx/ty below derive from it. */
            s_wheelStick.cx = wcx;
            s_wheelStick.cy = clampi((int)(wcy - wry * 0.7f), 0, GA_ENGINE_H - 1);
            s_wheelStick.rx = wrx;
            s_wheelStick.ry = wry;
            s_wheelStick.last_x = s_wheelStick.cx;
            s_wheelStick.last_y = s_wheelStick.cy;
            if (!s_touch(&jni, NULL, 1, s_wheelStick.cx, s_wheelStick.cy,
                         (jlong)s_wheelStick.slot, 0, 0)) return;
            s_wheelStick.active = 1;
            l_note("[input] wheel down @(%d,%d) slot %d", s_wheelStick.cx, s_wheelStick.cy, s_wheelStick.slot);
        }

        /* nx > 0 (right) -> tx > down_x -> first.x - current.x < 0 -> dir=0 (right)
         * nx < 0 (left)  -> tx < down_x -> first.x - current.x > 0 -> dir=1 (left)
         * ty always grows downward, curving like a real wheel turn. */
        float ax = nx < 0.0f ? -nx : nx;
        int tx = clampi((int)(s_wheelStick.cx + nx * 85.0f), 0, GA_ENGINE_W - 1);
        int ty = clampi((int)(s_wheelStick.cy + ax * 50.0f), 0, GA_ENGINE_H - 1);
        if (tx != s_wheelStick.last_x || ty != s_wheelStick.last_y) {
            s_touch(&jni, NULL, 2, tx, ty, (jlong)s_wheelStick.slot, 0, 0);
            s_wheelStick.last_x = tx;
            s_wheelStick.last_y = ty;
            l_debug("[input] wheel move @(%d,%d) nx=%.2f", tx, ty, nx);
        }
        return;
    }

    /* No steering input (or wheel hidden now): center the wheel if held. */
    if (s_wheelStick.active) {
        s_touch(&jni, NULL, 0, s_wheelStick.last_x, s_wheelStick.last_y, (jlong)s_wheelStick.slot, 0, 0);
        l_note("[input] wheel up (slot %d)", s_wheelStick.slot);
        s_wheelStick.active = 0;
    }

    /* 2. On-foot / sniper: AnalogStick movement (unchanged, Fase 51). */
    if (stick_ok) {
        if (!want_active) {
            if (s_moveStick.active) {
                s_touch(&jni, NULL, 0, s_moveStick.last_x, s_moveStick.last_y, (jlong)s_moveStick.slot, 0, 0);
                l_note("[input] move-stick up (slot %d)", s_moveStick.slot);
                s_moveStick.active = 0;
            }
            return;
        }

        if (!s_moveStick.active) {
            s_moveStick.cx = scx;
            s_moveStick.cy = scy;
            s_moveStick.rx = srx;
            s_moveStick.ry = sry;
            s_moveStick.last_x = scx;
            s_moveStick.last_y = scy;
            if (!s_touch(&jni, NULL, 1, scx, scy, (jlong)s_moveStick.slot, 0, 0)) return;
            s_moveStick.active = 1;
            l_note("[input] move-stick down @(%d,%d) r=(%.0f,%.0f) slot %d",
                   scx, scy, srx, sry, s_moveStick.slot);
        }

        /* A bit past the widget's own drawn radius (1.25x) so a fully-deflected
         * physical stick reliably reaches whatever internal max-drag clamp
         * AnalogStick::processTouch applies, same margin a real thumb dragging
         * past the base graphic would give it. */
        int tx = clampi((int)(s_moveStick.cx + nx * s_moveStick.rx * 1.25f), 0, GA_ENGINE_W - 1);
        int ty = clampi((int)(s_moveStick.cy - ny * s_moveStick.ry * 1.25f), 0, GA_ENGINE_H - 1);
        if (tx != s_moveStick.last_x || ty != s_moveStick.last_y) {
            s_touch(&jni, NULL, 2, tx, ty, (jlong)s_moveStick.slot, 0, 0);
            s_moveStick.last_x = tx;
            s_moveStick.last_y = ty;
            l_debug("[input] move-stick move @(%d,%d)", tx, ty);
        }
        return;
    }

    /* 3. Neither stick nor wheel is interactable: release move-stick if held. */
    if (s_moveStick.active) {
        s_touch(&jni, NULL, 0, s_moveStick.last_x, s_moveStick.last_y, (jlong)s_moveStick.slot, 0, 0);
        l_note("[input] move-stick up (slot %d)", s_moveStick.slot);
        s_moveStick.active = 0;
    }
}

void gamepad_actions_init(gamepad_touch_fn touch) {
    s_touch = touch;
    s_hudManagerAddr = (CHudManagerPtr *) so_symbol(&so_mod, "_ZN11CHudManager12s_hudManagerE");
    s_getInstance = (getinstance_fn) so_symbol(&so_mod, "_ZN11Application11GetInstanceEv");
    s_getScale = (getscale_fn) so_symbol(&so_mod, "_ZNK11Application21GetScreenScaleFactorsERfS0_");

    s_getMenuManager = (getinstance_fn) so_symbol(&so_mod, "_ZN11MenuManager11getInstanceEv");
    s_getMenuFX = (menu_fx_fn) so_symbol(&so_mod, "_ZN11MenuManager9getMenuFXEv");
    s_getMenuDepth = (menu_depth_fn) so_symbol(&so_mod, "_ZNK11MenuManager8getDepthEv");
    s_isClearingFocus = (menu_clearing_fn) so_symbol(&so_mod, "_ZNK11MenuManager15isClearingFocusEv");
    s_isCurrentMenuState = (menu_state_fn) so_symbol(&so_mod, "_ZNK11MenuManager14isCurrentStateEPKc");
    s_getController = (menu_controller_fn) so_symbol(&so_mod, "_ZN8RenderFX13GetControllerEi");
    s_setFocusDefault = (menu_focus_fn) so_symbol(&so_mod, "_ZN6MenuFX15SetFocusDefaultEv");
    s_updateMenuInput = (menu_input_fn) so_symbol(&so_mod, "_ZN8RenderFX11UpdateInputEii");

    s_unregisterHUDDraw = (unregister_draw_fn) so_symbol(&so_mod, "_ZN11Application25UnRegisterForDrawingOnHUDEP9IDrawable");
    s_cameraManagerAddr = (void **) so_symbol(&so_mod, "_ZN13CameraManager10s_instanceE");
    s_getScriptManager = (getinstance_fn) so_symbol(&so_mod, "_ZN13ScriptManager11getInstanceEv");
    s_consumedCursors = (menu_clearing_fn) so_symbol(&so_mod, "_ZNK11MenuManager15consumedCursorsEv");
    s_isInCinematic = (menu_clearing_fn) so_symbol(&so_mod, "_ZNK13ScriptManager13isInCinematicEv");
    s_isActiveCamera = (camera_active_fn) so_symbol(&so_mod, "_ZNK13CameraManager14isActiveCameraEi");
    s_resetCameraTouch = (menu_focus_fn) so_symbol(&so_mod, "_ZN9HudScreen10resetTouchEv");
    if (!s_cameraManagerAddr || !s_getScriptManager || !s_consumedCursors ||
        !s_isInCinematic || !s_isActiveCamera || !s_resetCameraTouch ||
        !s_unregisterHUDDraw || !s_touch || !s_hudManagerAddr || !s_getInstance || !s_getScale ||
        !s_getMenuManager || !s_getMenuFX || !s_getMenuDepth ||
        !s_isClearingFocus || !s_isCurrentMenuState || !s_getController ||
        !s_setFocusDefault || !s_updateMenuInput) {
        l_note("gamepad_actions: could not resolve required symbols -- "
               "physical-button gameplay mapping disabled, touchscreen relay still works");
        s_ready = 0;
        return;
    }
    s_isInMainMenu = (unsigned char *)so_symbol(&so_mod, "_ZN12StateMachine14s_isInMainMenuE");
    s_playerAddr = (void **)so_symbol(&so_mod, "_ZN6Player8s_playerE");
    s_getVehicle = (get_vehicle_fn)so_symbol(&so_mod, "_ZNK6Player10getVehicleEv");
    s_addCash = (add_cash_fn)so_symbol(&so_mod, "_ZN6Player7addCashEii");
    s_isHelicopter = (helicopter_fn)so_symbol(&so_mod, "_ZN17GameObjectManager12isHelicopterEP11LevelObject");
    s_isWalkHUD = (menu_clearing_fn)so_symbol(&so_mod, "_ZNK11CHudManager11isInWalkHudEv");
    s_isFlyHUD = (menu_clearing_fn)so_symbol(&so_mod, "_ZNK11CHudManager10isInFlyHudEv");
    s_controlLocked = (menu_clearing_fn)so_symbol(&so_mod, "_ZNK15CControlManager8isLockedEv");
    s_controlTransition = (menu_clearing_fn)so_symbol(&so_mod, "_ZN15CControlManager14isInTransitionEv");
    s_eventManagerAddr = (void **)so_symbol(&so_mod, "_ZN12EventManager14s_eventManagerE");
    s_raiseEvent = (raise_event_fn)so_symbol(&so_mod, "_ZN12EventManager5raiseERK6IEvent");
    uintptr_t button = so_symbol(&so_mod, "_ZTV15EvVirtualButton");
    s_buttonVtable = button ? (void *)(button + 8) : NULL;
    uintptr_t move = so_symbol(&so_mod, "_ZTV11EvStickMove");
    uintptr_t release = so_symbol(&so_mod, "_ZTV15EvStickReleased");
    s_moveVtable = move ? (void *)(move + 8) : NULL;
    s_releaseVtable = release ? (void *)(release + 8) : NULL;
    uintptr_t yaw = so_symbol(&so_mod, "_ZTV20EvLeftRightStickMove");
    uintptr_t height = so_symbol(&so_mod, "_ZTV17EvUpDownStickMove");
    s_yawVtable = yaw ? (void *)(yaw + 8) : NULL;
    s_heightVtable = height ? (void *)(height + 8) : NULL;
    s_orientation = (int *)so_symbol(&so_mod, "_ZN9GS3DStuff13s_orientationE");
    s_baseAcceleration = (acceleration_fn)so_symbol(&so_mod, "_ZN17AccelerometerBase5movedEfff");
    s_accelerometerEnabled = (menu_clearing_fn)so_symbol(&so_mod, "_ZNK3glf12InputManager22IsAccelerometerEnabledEv");
    s_getNativeGamepad = (get_gamepad_fn)so_symbol(&so_mod, "_ZN3glf12InputManager10GetGamepadEj");
    s_getRotation = (rotation_get_fn)so_symbol(&so_mod, "_ZNK11LevelObject11GetRotationEv");
    s_setRotation = (rotation_set_fn)so_symbol(&so_mod, "_ZN11LevelObject11SetRotationERKN6glitch4core10quaternionE");
    s_multiplyRotation = (quaternion_mul_fn)so_symbol(&so_mod, "_ZNK6glitch4core10quaternionmlERKS1_");
    s_normalizeRotation = (quaternion_normalize_fn)so_symbol(&so_mod, "_ZN6glitch4core10quaternion9normalizeEv");
    s_getFOV = (camera_fov_fn)so_symbol(&so_mod, "_ZNK13CameraManager6getFOVEv");
    s_zoom = (camera_float_fn)so_symbol(&so_mod, "_ZN13CameraManager4zoomEf");
    s_incVerticalAngle = (camera_float_fn)so_symbol(&so_mod, "_ZN9FPSCamera16incVerticalAngleEf");
    uintptr_t moved = so_symbol(&so_mod, "_ZN16AccelerometerGlf5movedEfff");
    s_nativeReady = s_isInMainMenu && s_playerAddr && s_getVehicle && s_addCash && s_isHelicopter &&
        s_isWalkHUD && s_isFlyHUD && s_controlLocked && s_controlTransition &&
        s_moveVtable && s_releaseVtable && s_eventManagerAddr && s_raiseEvent && s_buttonVtable && s_yawVtable && s_heightVtable &&
        s_getRotation && s_setRotation && s_multiplyRotation && s_normalizeRotation &&
        s_getFOV && s_zoom && s_incVerticalAngle && s_orientation && s_baseAcceleration && s_accelerometerEnabled && s_getNativeGamepad && moved;
    if (s_nativeReady) {
        hook_addr(moved, (uintptr_t)accelerometer_moved);
        so_flush_caches(&so_mod);
        l_note("[input] helicopter adapter and money cheat ready");
    } else l_warn("[input] helicopter/cheat symbols missing");
    s_ready = 1;
    l_note("gamepad_actions: ready (native menus, HUD touches and right-stick camera)");
}

void gamepad_actions_update(uint32_t buttons, uint32_t old_buttons) {
    if (!s_ready)
        return;

    /* CHudManager::unregDrawableComponents converts HudElement to its
     * IDrawable subobject at +4. Removing only the drawing registration
     * preserves visibility/input flags. Repeat because mode changes register
     * these controls again; the engine's removal is safe when absent. */
    void *hud = *s_hudManagerAddr;
    void *app = s_getInstance();
    if (hud && app) {
        for (unsigned off = 0x28; off <= 0x70; off += 4) {
            void *widget = *(void **)((char *)hud + off);
            if (widget) s_unregisterHUDDraw(app, (char *)widget + 4);
        }
        for (unsigned off = 0x88; off <= 0x8c; off += 4) {
            void *widget = *(void **)((char *)hud + off);
            if (widget) s_unregisterHUDDraw(app, (char *)widget + 4);
        }
    }

    void *fx = active_menu_fx();
    if (fx && !s_menuFX)
        release_gameplay_touches();
    s_menuFX = fx;
    s_buttons = buttons;
    int flying = helicopter_active();
    if (flying != s_flying) {
        release_gameplay_touches();
        s_flying = flying;
        l_note("[input] helicopter controls=%d orientation=%d", flying,
               s_orientation ? *s_orientation : 0);
    }
    sniper_buttons(buttons, old_buttons);
    buttons = money_cheat(buttons, old_buttons);
    switch_weapon(buttons, old_buttons);
    if (s_menuFX) {
        uint32_t pressed = buttons & ~old_buttons;
        unsigned flags = (pressed & SCE_CTRL_UP ? 1u : 0u) |
                         (pressed & SCE_CTRL_DOWN ? 2u : 0u) |
                         (pressed & SCE_CTRL_LEFT ? 4u : 0u) |
                         (pressed & SCE_CTRL_RIGHT ? 8u : 0u) |
                         (pressed & SCE_CTRL_CROSS ? 16u : 0u);
        menu_input(flags);
        return;
    }

    for (unsigned i = 0; i < GA_NACTIONS; i++) {
        struct pad_action *a = &s_actions[i];
        if (s_sniping || (s_flying && (a->offs == offs_veh_a || a->offs == offs_veh_b))) {
            pad_release(a);
            continue;
        }
        uint32_t mask = a->mask;
        if (a->offs == offs_attack) {
            mask = *s_hudManagerAddr && s_nativeReady && s_isWalkHUD(*s_hudManagerAddr) ?
                SCE_CTRL_CIRCLE : SCE_CTRL_CROSS;
            if (mask != a->mask) { pad_release(a); a->mask = mask; }
        }
        int down = (buttons & mask) != 0;
        int was = (old_buttons & mask) != 0;
        if (down && !was)
            pad_press(a);
        else if (!down && was)
            pad_release(a);
    }
}
