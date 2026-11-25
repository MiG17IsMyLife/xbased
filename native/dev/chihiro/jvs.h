/*
 * xbased native core — the Chihiro cabinet's JVS I/O board (Sega 837-13551).
 */
#ifndef NATIVE_DEV_CHIHIRO_JVS_H
#define NATIVE_DEV_CHIHIRO_JVS_H

#include <stdbool.h>
#include <stdint.h>

#define NC_JVS_SYNC 0xE0

typedef struct NcJvs {
    uint8_t device_id;          /* 0 until the host assigns one */
    uint8_t sense;              /* 3 while unaddressed, 0 after */
    uint8_t resets;             /* a reset takes two in a row */

    uint8_t system_switches;    /* TEST 0x80 */
    uint8_t player_switches[2][2];
    uint16_t coin_count[2];
    uint16_t analog[8];         /* 0x8000 centred */

    uint8_t outputs[4];         /* general-purpose outputs, bank 0 first */
    void (*output)(void *opaque, const uint8_t outputs[4]);
    void *opaque;
} NcJvs;

void nc_jvs_init(NcJvs *j);

/* One unescaped host frame (E0, address, count, data, sum). Writes the
 * unescaped reply to out and returns its length, or 0 for no reply. */
int nc_jvs_frame(NcJvs *j, const uint8_t *in, int len, uint8_t *out, int cap);

#endif
