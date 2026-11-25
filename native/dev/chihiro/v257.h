/*
 * xbased native core — Maximum Tune's steering board, the Namco V257 STR PCB,
 * on the SC's second UART.
 */
#ifndef NATIVE_DEV_CHIHIRO_V257_H
#define NATIVE_DEV_CHIHIRO_V257_H

#include <stdbool.h>
#include <stdint.h>

typedef struct NcV257 {
    int state;
    int countdown;          /* service ticks left powering up */
    bool motor;             /* the cabinet's motor relay (JVS output bank 0, bit 7) */
    uint8_t frame[10];      /* the host's frame being gathered */
    int frame_len;
    /* 3-byte messages, one sent per tick */
    uint8_t queue[4][3];
    bool twice[4];
    int head, tail;
    uint8_t line[8];        /* bytes on their way to the host */
    int line_head, line_tail;
    uint16_t wheel;         /* where the wheel is, 0-0x3FF */
    uint16_t target;        /* the position it is told to hold, 0-0x3FF */
    uint8_t spring, damping; /* 0-0x7F */
    int8_t push;            /* -0x7F-0x7F, toward a higher position */
    int push_ticks;
} NcV257;

void nc_v257_init(NcV257 *v);
/* called every 16 ms */
void nc_v257_tick(NcV257 *v);
void nc_v257_motor(NcV257 *v, bool on);
void nc_v257_wheel(NcV257 *v, uint16_t pos);
/* serial line: a byte from the host / the next byte to it */
void nc_v257_rx(NcV257 *v, uint8_t byte);
bool nc_v257_tx(NcV257 *v, uint8_t *byte);
struct NcChihiroForce;
/* The current force command. False with the motor off. */
bool nc_v257_force(const NcV257 *v, struct NcChihiroForce *force);

#endif
