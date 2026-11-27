/*
 * Chihiro drive board for Maximum Tune: the Namco V257 STR PCB, on the
 * SC's second serial port. It powers the wheel's motor and reports the
 * wheel's position back to the game.
 *
 * The game keeps sending 10-byte frames with the force to apply (a target
 * position, spring, damping and a push); the board answers each with the
 * wheel's position. It also reports its own state changes (self-test
 * done, motor on, motor off).
 */
#ifndef NATIVE_DEV_CHIHIRO_V257_H
#define NATIVE_DEV_CHIHIRO_V257_H

#include <stdbool.h>
#include <stdint.h>

typedef struct NcV257 {
    int state;              /* BOOTING .. POWERED (v257.c) */
    int countdown;          /* 16 ms ticks left in self-test or power-up */
    bool motor;             /* the cabinet's motor relay (JVS output bank 0, bit 7) */
    uint8_t frame[10];      /* the game's frame, gathered byte by byte */
    int frame_len;
    /* 3-byte messages waiting to be sent, one per tick (a ring buffer).
     * twice: state reports are sent two times in a row. */
    uint8_t queue[4][3];
    bool twice[4];
    int head, tail;
    uint8_t line[8];        /* bytes being sent to the game (a ring buffer) */
    int line_head, line_tail;
    uint16_t wheel;         /* where the wheel is, 0-0x3FF */
    uint16_t target;        /* the position it is told to hold, 0-0x3FF */
    uint8_t spring, damping; /* 0-0x7F */
    int8_t push;            /* -0x7F-0x7F, positive toward a higher position */
    int push_ticks;         /* ticks left before the push ends */
} NcV257;

/* Power-on state: booting, motor off. */
void nc_v257_init(NcV257 *v);
/* Call every 16 ms. */
void nc_v257_tick(NcV257 *v);
/* The motor relay switched on or off. */
void nc_v257_motor(NcV257 *v, bool on);
/* The player's wheel position, 0-0x3FF. */
void nc_v257_wheel(NcV257 *v, uint16_t pos);
/* The serial line: rx gets a byte from the game; tx gives the next byte
 * for the game, false when there is none. */
void nc_v257_rx(NcV257 *v, uint8_t byte);
bool nc_v257_tx(NcV257 *v, uint8_t *byte);
struct NcChihiroForce;
/* The current force command (board.h). False when the motor is off. */
bool nc_v257_force(const NcV257 *v, struct NcChihiroForce *force);

#endif
