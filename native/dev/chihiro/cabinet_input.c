/*
 * Chihiro cabinet controls (cabinet_input.h): merges the controller and
 * the wheel, then sets the JVS I/O board's switches and analog channels
 * the way Maximum Tune's cabinet wires them.
 *
 * JVS layout used by Maximum Tune:
 *   system switches   0x80 TEST
 *   player 1 byte 0   0x80 START, 0x40 SERVICE
 *   player 1 byte 1   gear lever bits (k_gate), 0x02 VIEW, 0x01 PERSPECTIVE
 *   analog 0, 1, 2    steering, gas, brake
 *   coin slot 0       counts coins
 */
#include "native/dev/chihiro/cabinet_input.h"
#include "native/dev/chihiro/board.h"

#include "board_int.h"

#include <pthread.h>
#include <time.h>

/* The gear lever's four switches for each gear (0 neutral, 1-6):
 * 0x80 up, 0x40 down, 0x20 left, 0x10 right. */
static const uint8_t k_gate[7] = {0x00, 0xA0, 0x60, 0x80, 0x40, 0x90, 0x50};

/* Written by the UI thread, read by the USB thread. */
static pthread_mutex_t g_input_lock = PTHREAD_MUTEX_INITIALIZER;
static NcChihiroInput g_pad_in = {.steering = 0x8000}, g_wheel_in;
static bool g_wheel;                /* the wheel's axes replace the pad's */

static int64_t g_test_at_ns;        /* host time TEST was last seen held */

static int64_t host_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

void nc_chihiro_board_input(const NcChihiroInput *in)
{
    if (!nc_chihiro_board_active()) {
        return;
    }
    pthread_mutex_lock(&g_input_lock);
    g_pad_in = *in;
    pthread_mutex_unlock(&g_input_lock);
}

void nc_chihiro_board_wheel(bool axes, const NcChihiroInput *in)
{
    if (!nc_chihiro_board_active()) {
        return;
    }
    pthread_mutex_lock(&g_input_lock);
    g_wheel = axes;
    g_wheel_in = *in;
    pthread_mutex_unlock(&g_input_lock);
}

bool nc_chihiro_board_test_pressed(void)
{
    const int64_t at = __atomic_load_n(&g_test_at_ns, __ATOMIC_ACQUIRE);
    return nc_chihiro_board_active() && at && host_ns() - at < 3000000000LL;
}

/* The pad and wheel merged: the wheel's axes when it has them, its
 * H shifter when it has one, and any button either presses. */
static NcChihiroInput merged_input(void)
{
    pthread_mutex_lock(&g_input_lock);
    NcChihiroInput in = g_pad_in;
    const NcChihiroInput *w = &g_wheel_in;
    if (g_wheel) {
        in.steering = w->steering;
        in.gas = w->gas;
        in.brake = w->brake;
    }
    if (w->h_shifter) {
        in.h_shifter = true;
        in.gear = w->gear;
    }
    in.test |= w->test;
    in.service |= w->service;
    in.coin |= w->coin;
    in.start |= w->start;
    in.view |= w->view;
    in.perspective |= w->perspective;
    in.shift_up |= w->shift_up;
    in.shift_down |= w->shift_down;
    in.card |= w->card;
    pthread_mutex_unlock(&g_input_lock);
    return in;
}

/* A pedal's travel (0-0xFFFF) as the JVS analog value Maximum Tune wants.
 * The game reads full pedal at 320 of the channel's 1023 steps (10-bit
 * value in the top bits), so 95% travel already gives full pedal. */
static uint16_t wmmt_pedal(uint16_t travel)
{
    const uint32_t saturate = UINT16_MAX * 95u / 100u;
    return MIN(travel, saturate) * (320u << 6) / saturate;
}

bool chihiro_input_poll(NcJvs *j)
{
    const NcChihiroInput merged = merged_input();
    const NcChihiroInput *in = &merged;

    /* Remember the last time TEST was held (nc_chihiro_board_test_pressed). */
    if (in->test) {
        __atomic_store_n(&g_test_at_ns, host_ns(), __ATOMIC_RELEASE);
    }

    /* Sequential shifting: each press of shift up/down moves one gear.
     * The buttons' previous state finds the moment they go down. */
    static int gear;
    static bool up_before, down_before, coin_before, card_before;
    if (!in->h_shifter && in->shift_up && !up_before && gear < 6) {
        gear++;
    }
    if (!in->h_shifter && in->shift_down && !down_before && gear > 0) {
        gear--;
    }
    up_before = in->shift_up;
    down_before = in->shift_down;

    j->analog[0] = in->steering;
    j->analog[1] = wmmt_pedal(in->gas);
    j->analog[2] = wmmt_pedal(in->brake);
    /* The drive board reports the wheel's position back to the game,
     * 10 bits: 0 left .. 0x3FF right. */
    if (nc_chihiro_drive && nc_chihiro_drive->wheel) {
        nc_chihiro_drive->wheel(in->steering >> 6);
    }
    j->system_switches = in->test ? 0x80 : 0x00;
    j->player_switches[0][0] = (in->start ? 0x80 : 0) | (in->service ? 0x40 : 0);
    const uint8_t gate = k_gate[in->h_shifter ? (in->gear <= 6 ? in->gear : 0) : gear];
    j->player_switches[0][1] = gate | (in->view ? 0x02 : 0) | (in->perspective ? 0x01 : 0);
    if (in->coin && !coin_before) {
        j->coin_count[0]++;
    }
    coin_before = in->coin;

    const bool card_pressed = in->card && !card_before;
    card_before = in->card;
    return card_pressed;
}
