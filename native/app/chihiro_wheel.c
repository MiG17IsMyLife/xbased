/*
 * Chihiro host wheel (chihiro_wheel.h).
 */
#include "native/app/chihiro_wheel.h"

#include "native/app/ui/input_map.h"
#include "native/app/wheel_ffb.h"
#include "native/core/nc.h"
#include "native/dev/chihiro_board.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { FFB_MAX_ATTEMPTS = 4 };      /* tries to start force feedback, 1 s apart */

static const char *(*g_option)(const char *name);
static ImCabinet g_cabinet;
static bool (*g_wait_for_previous)(void);

static struct {
    SDL_Joystick *js;               /* the wheel */
    SDL_Joystick *shifter;          /* the separate shifter, if any */
    bool tried;                     /* opening was attempted this run */
    int axes[IM_CAB_WHEEL_AXES];    /* steering/gas/brake: axis index, -1 none */
    int btn[IM_CAB_INPUTS];         /* each cabinet button: button index, -1 none */
    int gear[6];                    /* gears 1-6: shifter button index, -1 none */
    unsigned ffb_attempts;
    Uint64 ffb_retry_at, shifter_retry_at;
    int16_t rest[IM_CAB_WHEEL_MAX_AXES];           /* each axis's value at open */
    ImPedalDirection direction[IM_CAB_WHEEL_AXES]; /* which way a pedal moves */
    bool trace;
} g_wheel;

void chihiro_wheel_setup(const char *(*option)(const char *name), bool (*wait_for_previous)(void))
{
    g_option = option;
    g_wait_for_previous = wait_for_previous;
}

void chihiro_wheel_set_game(const char *key)
{
    g_cabinet = im_cabinet_of(key);
}

static const char *opt_value(const char *name)
{
    return g_option ? g_option(name) : NULL;
}

static int opt_int(const char *name, int dflt)
{
    const char *v = opt_value(name);
    return v ? atoi(v) : dflt;
}

void chihiro_wheel_open(void)
{
    if (g_wheel.js) {
        return;
    }
    if (g_wait_for_previous && g_wait_for_previous()) {
        return;     /* chihiro_wheel_poll tries again later */
    }
    const bool first = !g_wheel.tried;     /* log only the first attempt */
    g_wheel.tried = true;
    g_wheel.trace = getenv("NC_TRACE_WHEEL") != NULL;
    for (int i = 0; i < (int)SDL_arraysize(g_wheel.gear); i++) {
        char key[32];
        snprintf(key, sizeof(key), "chihiro.wheel.gear%d", i + 1);
        const int number = opt_int(key, 0);
        g_wheel.gear[i] = number > 0 ? number - 1 : -1;
    }
    const char *want = opt_value("chihiro.wheel");
    if (want && !strcmp(want, "off")) {
        if (first) {
            nc_log("xbased-app", "chihiro: wheel off; the controller drives");
        }
        return;
    }
    g_wheel.js = im_cabinet_open_wheel(want);
    if (!g_wheel.js) {
        if (first) {
            nc_log("xbased-app", "chihiro: no wheel found%s%s; the controller drives", want ? " named " : "",
                   want ? want : "");
        }
        return;
    }
    const int axes = SDL_GetNumJoystickAxes(g_wheel.js), buttons = SDL_GetNumJoystickButtons(g_wheel.js);
    for (int i = 0; i < IM_CAB_WHEEL_AXES; i++) {
        const ImCabinetAxis *axis = &im_cabinet_axes[i];
        const int index = opt_int(axis->option, axis->default_axis);
        g_wheel.axes[i] = index >= 0 && index < axes && index < (int)SDL_arraysize(g_wheel.rest) ? index : -1;
        if (axis->invert_option) {
            g_wheel.direction[i] = im_cabinet_pedal_direction(opt_value(axis->invert_option));
        }
    }
    for (int i = 0; i < IM_CAB_INPUTS; i++) {
        const ImCabinetControl *control = &im_cabinet_controls[i];
        const int number = control->wheel_button && im_cabinet_has(g_cabinet, i)
                               ? opt_int(control->wheel_button, im_cabinet_wheel_default(g_cabinet, i))
                               : 0;
        g_wheel.btn[i] = number > 0 && number <= buttons ? number - 1 : -1;
    }
    /* Each axis's resting value, for pedals that rest at a non-zero value. */
    for (int a = 0; a < axes && a < (int)SDL_arraysize(g_wheel.rest); a++) {
        g_wheel.rest[a] = SDL_GetJoystickAxis(g_wheel.js, a);
    }
    nc_log("xbased-app", "chihiro: wheel \"%s\" (%d axes, %d buttons, %d hats): steering axis %d, gas %d, brake %d",
           SDL_GetJoystickName(g_wheel.js), axes, buttons, SDL_GetNumJoystickHats(g_wheel.js),
           g_wheel.axes[IM_CAB_WHEEL_STEER], g_wheel.axes[IM_CAB_WHEEL_ACCEL], g_wheel.axes[IM_CAB_WHEEL_BRAKE]);
}

bool chihiro_wheel_is(SDL_JoystickID id)
{
    return g_wheel.js && SDL_GetJoystickID(g_wheel.js) == id;
}

bool chihiro_wheel_owns(SDL_JoystickID id, bool by_name)
{
    if (chihiro_wheel_is(id) || (g_wheel.shifter && SDL_GetJoystickID(g_wheel.shifter) == id)) {
        return true;
    }
    if (!by_name) {
        return false;
    }
    const char *wheel = opt_value("chihiro.wheel"), *shifter = opt_value("chihiro.shifter");
    return im_cabinet_find_wheel(wheel) == id || (shifter && *shifter && im_cabinet_find_wheel(shifter) == id);
}

void chihiro_wheel_close(void)
{
    nc_wheel_ffb_close();
    if (g_wheel.shifter) {
        SDL_CloseJoystick(g_wheel.shifter);
    }
    if (g_wheel.js) {
        SDL_CloseJoystick(g_wheel.js);
    }
    memset(&g_wheel, 0, sizeof(g_wheel));
    nc_chihiro_board_wheel(false, &(NcChihiroInput){0});
}

/* A pedal's travel, 0 released .. 0xFFFF pressed (0 with no axis set). */
static uint16_t wheel_pedal(int pedal)
{
    const int axis = g_wheel.axes[pedal];
    if (axis < 0) {
        return 0;
    }
    return im_cabinet_pedal_value(SDL_GetJoystickAxis(g_wheel.js, axis), g_wheel.rest[axis], g_wheel.direction[pedal]);
}

/* Reads the H shifter's gear buttons into in: from the separate shifter
 * when chihiro.shifter names one (reopened every second if unplugged),
 * otherwise from the wheel itself. */
static void shifter_poll(NcChihiroInput *in)
{
    const char *name = opt_value("chihiro.shifter");
    SDL_Joystick *device = g_wheel.js;
    if (name && *name) {
        if (g_wheel.shifter && !SDL_JoystickConnected(g_wheel.shifter)) {
            SDL_CloseJoystick(g_wheel.shifter);
            g_wheel.shifter = NULL;
        }
        const Uint64 now = SDL_GetTicks();
        if (!g_wheel.shifter && now >= g_wheel.shifter_retry_at) {
            g_wheel.shifter = im_cabinet_open_wheel(name);
            g_wheel.shifter_retry_at = now + 1000;
        }
        device = g_wheel.shifter;
    }
    if (!device) {
        return;
    }
    const int buttons = SDL_GetNumJoystickButtons(device);
    for (int i = 0; i < (int)SDL_arraysize(g_wheel.gear); i++) {
        const int button = g_wheel.gear[i];
        if (button >= 0 && button < buttons) {
            in->h_shifter = true;           /* any gear button set: H shifter mode */
            if (!in->gear && SDL_GetJoystickButton(device, button)) {
                in->gear = i + 1;
            }
        }
    }
}

/* NC_TRACE_WHEEL: logs axes that moved a lot and buttons that went down,
 * to help find the right axis and button numbers. */
static void trace_wheel(void)
{
    static int16_t last[SDL_arraysize(g_wheel.rest)];
    static bool down[IM_CAB_WHEEL_MAX_BUTTONS];
    const int axes = SDL_GetNumJoystickAxes(g_wheel.js);
    for (int a = 0; a < axes && a < (int)SDL_arraysize(last); a++) {
        const int16_t v = SDL_GetJoystickAxis(g_wheel.js, a);
        if (abs(v - last[a]) > 6000) {
            nc_log("xbased-app", "chihiro wheel: axis %d = %d (rest %d)", a, v, g_wheel.rest[a]);
            last[a] = v;
        }
    }
    const int buttons = SDL_GetNumJoystickButtons(g_wheel.js);
    for (int b = 0; b < buttons && b < (int)SDL_arraysize(down); b++) {
        const bool d = SDL_GetJoystickButton(g_wheel.js, b);
        if (d && !down[b]) {
            nc_log("xbased-app", "chihiro wheel: button %d", b);
        }
        down[b] = d;
    }
}

void chihiro_wheel_poll(bool focused, bool running)
{
    if (!nc_chihiro_board_active()) {
        return;
    }
    if (!g_wheel.tried) {
        chihiro_wheel_open();
    }
    if (g_wheel.js && !SDL_JoystickConnected(g_wheel.js)) {
        chihiro_wheel_close();          /* unplugged: open it again next frame */
        return;
    }
    NcChihiroInput in = {0};
    shifter_poll(&in);
    if (!g_wheel.js) {
        nc_chihiro_board_wheel(false, &in);     /* only the shifter, if any */
        return;
    }

    /* Force feedback only while the game runs in front. */
    running = focused && running;
    const Uint64 now = SDL_GetTicks();
    if (running && g_wheel.ffb_attempts < FFB_MAX_ATTEMPTS && now >= g_wheel.ffb_retry_at &&
        g_wheel.axes[IM_CAB_WHEEL_STEER] >= 0 && opt_int("chihiro.ffb", 0)) {
        const bool opened = nc_wheel_ffb_open(g_wheel.js,
            opt_int("chihiro.ffb.strength", NC_WHEEL_FFB_STRENGTH_DEFAULT),
            opt_int("chihiro.ffb.deadband", NC_WHEEL_FFB_DEADBAND_DEFAULT), opt_int("chihiro.ffb.invert", 0) != 0);
        g_wheel.ffb_attempts = opened ? FFB_MAX_ATTEMPTS : g_wheel.ffb_attempts + 1;
        g_wheel.ffb_retry_at = now + 1000;
    }
    nc_wheel_ffb_poll(running);

    if (!focused && opt_int("chihiro.focus_input", 0)) {
        /* In the background: wheel centred, pedals and buttons released. */
        nc_chihiro_board_wheel(true, &(NcChihiroInput){.steering = 0x8000});
        return;
    }
    if (g_wheel.trace) {
        trace_wheel();
    }
    const int steer = g_wheel.axes[IM_CAB_WHEEL_STEER];
    in.steering = steer >= 0 ? (uint16_t)(SDL_GetJoystickAxis(g_wheel.js, steer) + 0x8000) : 0x8000;
    in.gas = wheel_pedal(IM_CAB_WHEEL_ACCEL);
    in.brake = wheel_pedal(IM_CAB_WHEEL_BRAKE);
    bool *const outs[IM_CAB_INPUTS] = {
        [IM_CAB_START] = &in.start, [IM_CAB_COIN] = &in.coin, [IM_CAB_TEST] = &in.test, [IM_CAB_SERVICE] = &in.service,
        [IM_CAB_VIEW] = &in.view, [IM_CAB_INTRUDE] = &in.perspective, [IM_CAB_SHIFT_UP] = &in.shift_up,
        [IM_CAB_SHIFT_DOWN] = &in.shift_down, [IM_CAB_CARD] = &in.card,
    };
    for (int i = 0; i < IM_CAB_INPUTS; i++) {
        if (outs[i] && g_wheel.btn[i] >= 0) {
            *outs[i] = SDL_GetJoystickButton(g_wheel.js, g_wheel.btn[i]);
        }
    }
    nc_chihiro_board_wheel(true, &in);
}
