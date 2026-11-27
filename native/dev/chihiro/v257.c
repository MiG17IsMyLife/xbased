/*
 * Chihiro drive board: Namco V257 STR PCB (v257.h).
 * Adapted from Tovarichtch/xemu's chihiro-driveboard-v257.c (98fbd75).
 * Copyright (c) 2026 Réda Chérif-Touil. SPDX-License-Identifier: LGPL-2.0-or-later
 *
 * Game -> board, 10 bytes:
 *   FF FF, target (16-bit little-endian), spring, 0, damping, 0, push, 0
 * Board -> game, 3 bytes:
 *   'H' + wheel position (16-bit big-endian)   the answer to each frame
 *   "E00"  self-test done                      state reports, each sent twice
 *   "C01"  motor powered up
 *   "C06"  motor off
 *
 * States (the motor relay is a JVS output the game switches):
 *   BOOTING  -relay on->  SELF_TEST  -3.15 s->  TESTED
 *   OFF      -relay on->  POWERING   -3.15 s->  POWERED
 *   relay off in TESTED or POWERED: back to OFF
 * Forces are applied only while POWERED.
 */
#include "v257.h"
#include "drive.h"
#include "native/dev/chihiro_board.h"

#include <string.h>

enum { BOOTING, SELF_TEST, TESTED, OFF, POWERING, POWERED };

#define POWER_TICKS 197     /* 3.15 s of 16 ms ticks */
#define PUSH_TICKS 6        /* a push lasts about 99 ms */
#define CENTRE 0x1FF

void nc_v257_init(NcV257 *v)
{
    memset(v, 0, sizeof(*v));
    v->state = BOOTING;
    v->target = CENTRE;
}

static void send(NcV257 *v, const uint8_t msg[3], bool twice)
{
    if (twice) {
        /* a state report replaces answers still queued */
        while (v->head != v->tail && v->queue[(v->tail + 3) % 4][0] == 'H') {
            v->tail = (v->tail + 3) % 4;
        }
    }
    const int next = (v->tail + 1) % 4;
    if (next == v->head) {
        return;
    }
    memcpy(v->queue[v->tail], msg, 3);
    v->twice[v->tail] = twice;
    v->tail = next;
}

static void report(NcV257 *v, const char *code)
{
    send(v, (const uint8_t *)code, true);
}

static void answer(NcV257 *v)
{
    const uint8_t msg[3] = {'H', (uint8_t)(v->wheel >> 8), (uint8_t)v->wheel};
    send(v, msg, false);
}

/* Moves the next queued message onto the line once the line is empty. */
static void drain(NcV257 *v)
{
    if (v->head == v->tail || v->line_head != v->line_tail) {
        return;
    }
    for (int k = 0; k < (v->twice[v->head] ? 2 : 1); k++) {
        for (int i = 0; i < 3; i++) {
            v->line[v->line_tail] = v->queue[v->head][i];
            v->line_tail = (v->line_tail + 1) % 8;
        }
    }
    v->head = (v->head + 1) % 4;
}

/* No force: hold the centre with no spring. */
static void rest(NcV257 *v)
{
    v->target = CENTRE;
    v->spring = v->damping = 0;
    v->push = 0;
    v->push_ticks = 0;
}

static void motor_off(NcV257 *v, bool tell)
{
    if (tell) {
        report(v, "C06");
    }
    rest(v);
    v->state = OFF;
}

void nc_v257_tick(NcV257 *v)
{
    switch (v->state) {
    case BOOTING:
    case OFF:
        if (v->state == OFF) {
            rest(v);
        }
        if (v->motor) {
            v->state = v->state == BOOTING ? SELF_TEST : POWERING;
            v->countdown = POWER_TICKS;
        }
        break;
    case SELF_TEST:
        if (!v->motor) {
            v->state = BOOTING;
        } else if (--v->countdown <= 0) {
            report(v, "E00");
            v->state = TESTED;
        }
        break;
    case POWERING:
        if (!v->motor) {
            motor_off(v, false);
        } else if (--v->countdown <= 0) {
            report(v, "C01");
            v->state = POWERED;
        }
        break;
    case TESTED:
    case POWERED:
        if (!v->motor) {
            motor_off(v, true);
        }
        break;
    }
    if (v->push_ticks > 0 && --v->push_ticks == 0) {
        v->push = 0;
    }
    drain(v);
}

void nc_v257_motor(NcV257 *v, bool on)
{
    v->motor = on;
}

void nc_v257_wheel(NcV257 *v, uint16_t pos)
{
    v->wheel = pos > 0x3FF ? 0x3FF : pos;
}

static int clamp7f(int x)
{
    return x > 0x7F ? 0x7F : x < -0x7F ? -0x7F : x;
}

void nc_v257_rx(NcV257 *v, uint8_t byte)
{
    if (v->state < OFF) {
        return;                         /* its serial port isn't up yet */
    }
    if (v->frame_len < 2) {             /* FF FF opens a frame, nothing else */
        v->frame_len = byte == 0xFF ? v->frame_len + 1 : 0;
        return;
    }
    v->frame[v->frame_len++] = byte;
    if (v->frame_len < (int)sizeof(v->frame)) {
        return;
    }
    v->frame_len = 0;
    const uint8_t *f = v->frame;
    if (v->state == OFF) {
        answer(v);                      /* answered, but the forces are not used */
        drain(v);
        return;
    }
    v->spring = (uint8_t)clamp7f(f[4] * 29 / 16);
    v->damping = (uint8_t)clamp7f(f[6] * 2);
    /* The push byte is sent inverted: bit 7 set pushes toward a higher position. */
    const int push = clamp7f(((f[8] & 0x80) ? (int)(uint8_t)~f[8] : -(int)f[8]) * 2);
    if (!push) {
        v->push_ticks = 0;
    } else if (!v->push_ticks || (push > 0) != (v->push > 0)) {
        v->push_ticks = PUSH_TICKS;
    }
    v->push = (int8_t)push;
    /* A target too far from the current one is ignored. */
    const uint16_t target = (uint16_t)(f[2] | f[3] << 8);
    const int16_t step = (int16_t)(target - v->target);
    if (target <= 0x3FF && step > -0x70 && step < 0x70) {
        v->target = target;
    }
    if (v->state == POWERED) {
        answer(v);                      /* the wheel's position, as the game's ack */
        drain(v);
    }
}

bool nc_v257_tx(NcV257 *v, uint8_t *byte)
{
    if (v->line_head == v->line_tail) {
        return false;
    }
    *byte = v->line[v->line_head];
    v->line_head = (v->line_head + 1) % 8;
    return true;
}

bool nc_v257_force(const NcV257 *v, NcChihiroForce *force)
{
    if (v->state != POWERED || !v->motor) {
        *force = (NcChihiroForce){0};
        return false;
    }
    *force = (NcChihiroForce){
        .target = v->target,
        .spring = v->spring,
        .damping = v->damping,
        .push = v->push_ticks ? v->push : 0,
    };
    return true;
}

/* ------------------------------------------------------------ as a drive board (drive.h) */

static NcV257 g_v257;

static void drive_init(void)
{
    nc_v257_init(&g_v257);
}

static void drive_tick(void)
{
    nc_v257_tick(&g_v257);
}

/* the motor's relay: JVS output bank 0, bit 7 */
static void drive_outputs(const uint8_t outputs[4])
{
    nc_v257_motor(&g_v257, (outputs[0] & 0x80) != 0);
}

static void drive_wheel(uint16_t position)
{
    nc_v257_wheel(&g_v257, position);
}

static void drive_rx(uint8_t byte)
{
    nc_v257_rx(&g_v257, byte);
}

static bool drive_tx(uint8_t *byte)
{
    return nc_v257_tx(&g_v257, byte);
}

static bool drive_force(NcChihiroForce *force)
{
    return nc_v257_force(&g_v257, force);
}

const NcChihiroDrive nc_chihiro_drive_v257 = {
    .name = "Namco V257 STR PCB",
    .init = drive_init,
    .tick = drive_tick,
    .outputs = drive_outputs,
    .wheel = drive_wheel,
    .rx = drive_rx,
    .tx = drive_tx,
    .force = drive_force,
};
