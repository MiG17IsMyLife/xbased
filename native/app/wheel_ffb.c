/* Map the V257's centering, viscosity and signed push to native wheel effects.
 * The three effect types follow KittenTools' WMMT3 STR PCB adapter. The
 * Chihiro board has already interpreted the serial frame and motor relay. */
#include "native/app/wheel_ffb.h"
#include "native/core/nc.h"
#include "native/dev/chihiro_board.h"
#include "native/dev/usb.h"

#include <SDL3/SDL_haptic.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_timer.h>

enum {
    EFFECT_DURATION_MS = 150,  /* expires if the frontend stops servicing it */
    EFFECT_REFRESH_MS = 100,
    EFFECT_RETRY_MS = 500,
};

enum { WHEEL_SPRING, WHEEL_DAMPER, WHEEL_PUSH };

typedef struct WheelEffect {
    SDL_HapticEffectType type;
    SDL_HapticEffectID id;
    int level, center;
    Uint64 refresh_at, retry_at;
} WheelEffect;

static WheelEffect g_effects[] = {
    [WHEEL_SPRING] = {.type = SDL_HAPTIC_SPRING, .id = -1},
    [WHEEL_DAMPER] = {.type = SDL_HAPTIC_DAMPER, .id = -1},
    [WHEEL_PUSH] = {.type = SDL_HAPTIC_CONSTANT, .id = -1},
};

static SDL_Haptic *g_haptic;
static Uint32 g_features;
static int g_strength, g_deadband;
static bool g_invert_push;

static void stop_effect(WheelEffect *effect)
{
    if (effect->id >= 0) {
        SDL_StopHapticEffect(g_haptic, effect->id);
        SDL_DestroyHapticEffect(g_haptic, effect->id);
        effect->id = -1;
    }
}

void nc_wheel_ffb_stop(void)
{
    for (int i = 0; i < (int)SDL_arraysize(g_effects); i++) {
        stop_effect(&g_effects[i]);
    }
}

void nc_wheel_ffb_close(void)
{
    if (g_haptic) {
        nc_wheel_ffb_stop();
        SDL_CloseHaptic(g_haptic);
        g_haptic = NULL;
        SDL_QuitSubSystem(SDL_INIT_HAPTIC);
    }
}

bool nc_wheel_ffb_open(SDL_Joystick *joystick, int strength, int deadband, bool invert_push)
{
    if (strength <= 0) {
        return false;
    }
    if (!SDL_InitSubSystem(SDL_INIT_HAPTIC)) {
        nc_log("xbased-app", "chihiro FFB: haptics unavailable: %s", SDL_GetError());
        return false;
    }
    g_haptic = SDL_OpenHapticFromJoystick(joystick);
    if (!g_haptic) {
        nc_log("xbased-app", "chihiro FFB: %s: %s", SDL_GetJoystickName(joystick), SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_HAPTIC);
        return false;
    }
    g_features = SDL_GetHapticFeatures(g_haptic);
    if (!(g_features & (SDL_HAPTIC_SPRING | SDL_HAPTIC_DAMPER | SDL_HAPTIC_CONSTANT)) ||
        SDL_GetNumHapticAxes(g_haptic) < 1) {
        nc_log("xbased-app", "chihiro FFB: %s has no steering force effects", SDL_GetJoystickName(joystick));
        nc_wheel_ffb_close();
        return false;
    }
    for (int i = 0; i < (int)SDL_arraysize(g_effects); i++) {
        g_effects[i].retry_at = 0;
    }
    g_strength = SDL_min(strength, 100);
    g_deadband = SDL_clamp(deadband, 0, 100);
    g_invert_push = invert_push;
    nc_log("xbased-app", "chihiro FFB: %s (%d%%), spring %s, damper %s, constant %s",
           SDL_GetJoystickName(joystick), g_strength, g_features & SDL_HAPTIC_SPRING ? "yes" : "no",
           g_features & SDL_HAPTIC_DAMPER ? "yes" : "no", g_features & SDL_HAPTIC_CONSTANT ? "yes" : "no");
    return true;
}

static SDL_HapticEffect make_effect(SDL_HapticEffectType type, int level, int center)
{
    SDL_HapticEffect data = {0};
    if (type == SDL_HAPTIC_CONSTANT) {
        data.constant.type = type;
        data.constant.direction.type = SDL_HAPTIC_STEERING_AXIS;
        data.constant.length = EFFECT_DURATION_MS;
        data.constant.level = level;
    } else {
        SDL_HapticCondition *condition = &data.condition;
        condition->type = type;
        condition->direction.type = SDL_HAPTIC_STEERING_AXIS;
        condition->length = EFFECT_DURATION_MS;
        condition->center[0] = center;
        condition->left_coeff[0] = condition->right_coeff[0] = level;
        condition->left_sat[0] = condition->right_sat[0] = SDL_MAX_UINT16 * g_strength / 100;
        if (type == SDL_HAPTIC_SPRING) {
            condition->deadband[0] = SDL_MAX_UINT16 * g_deadband / 100;
        }
    }
    return data;
}

static void apply_effect(WheelEffect *effect, int command, int center, Uint64 now)
{
    if (!(g_features & effect->type)) {
        return;
    }
    const int level = command * SDL_MAX_SINT16 * g_strength / (0x7F * 100);
    if (!level) {
        stop_effect(effect);
        return;
    }
    if (now < effect->retry_at) {
        return;
    }
    if (effect->id < 0 || level != effect->level || center != effect->center) {
        const SDL_HapticEffect data = make_effect(effect->type, level, center);
        if (effect->id < 0) {
            effect->id = SDL_CreateHapticEffect(g_haptic, &data);
            if (effect->id < 0) {
                goto fail;
            }
            effect->refresh_at = now;
        } else if (!SDL_UpdateHapticEffect(g_haptic, effect->id, &data)) {
            goto fail;
        }
        effect->level = level;
        effect->center = center;
    }
    if (now >= effect->refresh_at) {
        if (!SDL_RunHapticEffect(g_haptic, effect->id, 1)) {
            goto fail;
        }
        effect->refresh_at = now + EFFECT_REFRESH_MS;
    }
    effect->retry_at = 0;
    return;

fail:
    if (!effect->retry_at) {
        nc_log("xbased-app", "chihiro FFB: effect 0x%x failed: %s", (unsigned)effect->type, SDL_GetError());
    }
    stop_effect(effect);
    effect->retry_at = now + EFFECT_RETRY_MS;
}

void nc_wheel_ffb_poll(bool running)
{
    if (!g_haptic) {
        return;
    }
    NcChihiroForce force;
    if (!running || !nc_dev_ohci_cabinet_force(&force)) {
        nc_wheel_ffb_stop();
        return;
    }
    const Uint64 now = SDL_GetTicks();
    const int center = force.target * (2 * SDL_MAX_SINT16) / 0x3FF - SDL_MAX_SINT16;
    apply_effect(&g_effects[WHEEL_SPRING], force.spring, center, now);
    apply_effect(&g_effects[WHEEL_DAMPER], force.damping, 0, now);
    apply_effect(&g_effects[WHEEL_PUSH], g_invert_push ? -force.push : force.push, 0, now);
}
