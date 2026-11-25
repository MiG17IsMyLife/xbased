/*
 * xbased native core — the V257 steering board (v257.h).
 * Adapted from Tovarichtch/xemu's chihiro-driveboard-v257.c (98fbd75).
 * Copyright (c) 2026 Réda Chérif-Touil. SPDX-License-Identifier: LGPL-2.0-or-later
 *
 * Host frame: FF FF, target (16-bit LE), spring, pad, damping, pad, push,
 * pad. Each frame is answered with 'H' and the wheel position. State
 * reports are sent twice: E00 self-test done, C01 motor up, C06 motor off.
 */
#include "v257.h"
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

/* moves the next message onto the line once it is empty */
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
        answer(v);                      /* answered, the forces not taken */
        drain(v);
        return;
    }
    v->spring = (uint8_t)clamp7f(f[4] * 29 / 16);
    v->damping = (uint8_t)clamp7f(f[6] * 2);
    /* the push, a signed byte: negative toward a lower position */
    const int push = clamp7f(((f[8] & 0x80) ? (int)(uint8_t)~f[8] : -(int)f[8]) * 2);
    if (!push) {
        v->push_ticks = 0;
    } else if (!v->push_ticks || (push > 0) != (v->push > 0)) {
        v->push_ticks = PUSH_TICKS;
    }
    v->push = (int8_t)push;
    /* a target too far from the one it holds is refused */
    const uint16_t target = (uint16_t)(f[2] | f[3] << 8);
    const int16_t step = (int16_t)(target - v->target);
    if (target <= 0x3FF && step > -0x70 && step < 0x70) {
        v->target = target;
    }
    if (v->state == POWERED) {
        answer(v);                      /* the wheel's position, the game's ack */
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
