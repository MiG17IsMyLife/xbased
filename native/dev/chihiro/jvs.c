/*
 * xbased native core — the JVS I/O board (jvs.h).
 */
#include "jvs.h"

#include <string.h>

enum {
    STATUS_OK = 1,
    REPORT_OK = 1,
    REPORT_PARAM = 2,
    BROADCAST = 0xFF,
    HOST = 0x00,
};

static const char k_id[] = "SEGA ENTERPRISES,LTD.;I/O BD JVS;837-13551 ;Ver1.00;98/10";

/* 2 players x 13 switches, 2 coin slots, 8 x 16-bit analog, 6 output banks, backup */
static const uint8_t k_features[] = {
    0x01, 2, 13, 0,
    0x02, 2, 0, 0,
    0x03, 8, 16, 0,
    0x12, 6, 0, 0,
    0x15, 0, 0, 0,
    0x00,
};

void nc_jvs_init(NcJvs *j)
{
    void (*output)(void *, const uint8_t[4]) = j->output;
    void *opaque = j->opaque;
    memset(j, 0, sizeof(*j));
    j->output = output;
    j->opaque = opaque;
    j->sense = 3;
    for (int i = 0; i < 8; i++) {
        j->analog[i] = 0x8000;
    }
}

typedef struct Out {
    uint8_t *b;
    int n, cap;
} Out;

static void put(Out *o, uint8_t v)
{
    if (o->b && o->n < o->cap) {
        o->b[o->n++] = v;
    }
}

static void put16(Out *o, uint16_t v)
{
    put(o, (uint8_t)(v >> 8));
    put(o, (uint8_t)v);
}

static void put_zeros(Out *o, int n)
{
    for (int i = 0; i < n; i++) {
        put(o, 0);
    }
}

static int clip(int want, int left)
{
    return want > left ? left : want;
}

static void set_outputs(NcJvs *j, const uint8_t *banks, int count)
{
    uint8_t now[4] = {0};
    memcpy(now, banks, (size_t)clip(count, 4));
    if (memcmp(now, j->outputs, sizeof(now))) {
        memcpy(j->outputs, now, sizeof(now));
        if (j->output) {
            j->output(j->opaque, j->outputs);
        }
    }
}

/* Runs the command at c; returns the bytes it used, 0 to stop parsing. */
static int command(NcJvs *j, const uint8_t *c, int left, Out *o)
{
    /* commands that take a parameter byte need it */
    if (left < 2 && c[0] && strchr("\xF0\xF1\x20\x21\x22\x23\x26\x2E\x32\x33\x34", c[0])) {
        return 0;
    }
    switch (c[0]) {
    case 0xF0:                                  /* reset */
        if (++j->resets >= 2) {
            static const uint8_t off[4];
            j->device_id = 0;
            j->sense = 3;
            j->resets = 0;
            set_outputs(j, off, 4);
        }
        return 2;
    case 0xF1:                                  /* set address */
        if (j->sense) {
            j->device_id = c[1];
            j->sense = 0;
            j->resets = 0;
            put(o, REPORT_OK);
        }
        return 2;
    case 0x10:                                  /* identify */
        put(o, REPORT_OK);
        for (unsigned i = 0; i < sizeof(k_id); i++) {
            put(o, (uint8_t)k_id[i]);
        }
        return 1;
    case 0x11:                                  /* command revision 1.1 */
        put(o, REPORT_OK);
        put(o, 0x11);
        return 1;
    case 0x12:                                  /* JVS revision 2.0 */
        put(o, REPORT_OK);
        put(o, 0x20);
        return 1;
    case 0x13:                                  /* communication version 1.0 */
        put(o, REPORT_OK);
        put(o, 0x10);
        return 1;
    case 0x14:                                  /* features */
        put(o, REPORT_OK);
        for (unsigned i = 0; i < sizeof(k_features); i++) {
            put(o, k_features[i]);
        }
        return 1;
    case 0x15: {                                /* host name */
        int n = 1;
        while (n < left && c[n]) {
            n++;
        }
        put(o, REPORT_OK);
        return n < left ? n + 1 : n;
    }
    case 0x20:                                  /* switches: players, bytes per player */
        if (left < 3) {
            return 0;
        }
        put(o, REPORT_OK);
        put(o, j->system_switches);
        for (int p = 0; p < c[1]; p++) {
            for (int b = 0; b < c[2]; b++) {
                put(o, p < 2 && b < 2 ? j->player_switches[p][b] : 0);
            }
        }
        return 3;
    case 0x21:                                  /* coins */
        put(o, REPORT_OK);
        for (int s = 0; s < c[1]; s++) {
            put16(o, s < 2 ? j->coin_count[s] & 0x3FFF : 0);
        }
        return 2;
    case 0x22:                                  /* analog */
        put(o, REPORT_OK);
        for (int a = 0; a < c[1]; a++) {
            put16(o, a < 8 ? j->analog[a] : 0x8000);
        }
        return 2;
    case 0x23:                                  /* rotary */
        put(o, REPORT_OK);
        put_zeros(o, 2 * c[1]);
        return 2;
    case 0x26:                                  /* general-purpose inputs */
        put(o, REPORT_OK);
        put_zeros(o, c[1]);
        return 2;
    case 0x2E:                                  /* hoppers */
        put(o, REPORT_OK);
        put_zeros(o, 4 * c[1]);
        return 2;
    case 0x2F:                                  /* retransmit */
        put(o, REPORT_OK);
        return 1;
    case 0x30:                                  /* coins decrease */
    case 0x31: {                                /* coins increase */
        if (left < 4) {
            return 0;
        }
        if (c[1] >= 1 && c[1] <= 2) {
            uint16_t *v = &j->coin_count[c[1] - 1];
            const uint16_t n = (uint16_t)(c[2] << 8 | c[3]);
            if (c[0] == 0x31) {
                *v = (uint16_t)(*v + n);
            } else if (*v >= n) {
                *v = (uint16_t)(*v - n);
            }
        }
        put(o, REPORT_OK);
        return 4;
    }
    case 0x32: {                                /* outputs */
        const int n = clip(2 + c[1], left);
        set_outputs(j, c + 2, n - 2);
        put(o, REPORT_OK);
        return n;
    }
    case 0x33:                                  /* analog outputs */
        put(o, REPORT_OK);
        return clip(2 + 2 * c[1], left);
    case 0x34:                                  /* characters */
        put(o, REPORT_OK);
        return clip(2 + c[1], left);
    case 0x36:                                  /* payout decrease */
        put(o, REPORT_OK);
        return clip(4, left);
    case 0x37:                                  /* outputs, second form */
        put(o, REPORT_OK);
        return clip(3, left);
    default:
        /* Unknown (Maximum Tune 2 sends Namco's 70h once): refuse that byte
         * and keep parsing. Stopping here makes the game report Error 11. */
        put(o, REPORT_PARAM);
        return 1;
    }
}

int nc_jvs_frame(NcJvs *j, const uint8_t *in, int len, uint8_t *out, int cap)
{
    if (len < 4 || in[0] != NC_JVS_SYNC || in[2] < 1 || len < 3 + in[2]) {
        return 0;
    }
    const uint8_t target = in[1];
    const uint8_t *data = in + 3;
    const int n = in[2] - 1;
    if (target != BROADCAST && j->device_id && target != j->device_id) {
        return 0;
    }
    /* of the broadcasts, only an address assignment is answered */
    const bool answer = target != BROADCAST || (n >= 1 && data[0] == 0xF1);
    uint8_t report[256];
    Out o = {answer ? report : NULL, 0, (int)sizeof(report)};
    put(&o, STATUS_OK);
    for (int at = 0; at < n;) {
        const int used = command(j, data + at, n - at, &o);
        if (used <= 0) {
            break;
        }
        at += used;
    }
    if (!answer || cap < o.n + 4) {
        return 0;
    }
    out[0] = NC_JVS_SYNC;
    out[1] = HOST;
    out[2] = (uint8_t)(o.n + 1);
    memcpy(out + 3, report, (size_t)o.n);
    uint8_t sum = out[1] + out[2];
    for (int i = 0; i < o.n; i++) {
        sum += report[i];
    }
    out[3 + o.n] = sum;
    return o.n + 4;
}
