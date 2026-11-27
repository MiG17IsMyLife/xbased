/*
 * Chihiro JVS I/O board (Sega 837-13551).
 *
 * JVS is Sega's arcade I/O standard: a host (here the game, through the
 * QC) sends framed commands on an RS-485 bus, and each I/O board answers
 * with its switches, coin counters and analog channels. This board is
 * the one in Maximum Tune's cabinet.
 *
 * A frame: E0 (sync), target address, byte count, data..., checksum.
 * At power-on the board has no address; the host gives it one with
 * command F1. cabinet_input.c fills in the switch and analog values.
 */
#ifndef NATIVE_DEV_CHIHIRO_JVS_H
#define NATIVE_DEV_CHIHIRO_JVS_H

#include <stdbool.h>
#include <stdint.h>

#define NC_JVS_SYNC 0xE0

typedef struct NcJvs {
    uint8_t device_id;          /* bus address, 0 until the host assigns one */
    uint8_t sense;              /* the sense line: 3 while unaddressed, 0 after */
    uint8_t resets;             /* reset commands seen; it takes two in a row */

    uint8_t system_switches;    /* 0x80 TEST */
    uint8_t player_switches[2][2];
    uint16_t coin_count[2];
    uint16_t analog[8];         /* 0 .. 0xFFFF, 0x8000 centre */

    uint8_t outputs[4];         /* general-purpose outputs, bank 0 first */
    /* Called when the outputs change (lamps, the wheel motor's relay). */
    void (*output)(void *opaque, const uint8_t outputs[4]);
    void *opaque;
} NcJvs;

/* Resets the board to power-on state. Keeps output and opaque. */
void nc_jvs_init(NcJvs *j);

/* Handles one frame from the host, without JVS escape bytes. Writes the
 * reply (also unescaped) to out and returns its length, or 0 when the
 * board doesn't answer (not addressed to it, or a broadcast). */
int nc_jvs_frame(NcJvs *j, const uint8_t *in, int len, uint8_t *out, int cap);

#endif
