/*
 * xbased native core — the few QEMU helpers the network board's files
 * (from Tovarichtch/xemu, by Réda Chérif-Touil, LGPL-2.1-or-later) use.
 */
#ifndef NATIVE_DEV_CHIHIRO_NET_NB_COMPAT_H
#define NATIVE_DEV_CHIHIRO_NET_NB_COMPAT_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

void nc_log(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
uint64_t nc_ke_now_ns(void);

static inline uint32_t ldn_le_p(const void *p, int size)
{
    const uint8_t *b = p;
    switch (size) {
    case 1:
        return b[0];
    case 2:
        return (uint32_t)b[0] | (uint32_t)b[1] << 8;
    default:
        return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    }
}

static inline void stn_le_p(void *p, int size, uint32_t v)
{
    uint8_t *b = p;
    for (int i = 0; i < size; i++) {
        b[i] = (uint8_t)(v >> (8 * i));
    }
}

/* NC_TRACE_CHIHIRO: the board's console and its rare commands */
bool nb_trace_on(void);
#define CHIHIRO_LOGF(cat, fmt, ...)                                    \
    do {                                                               \
        if (nb_trace_on()) {                                           \
            fprintf(stderr, "xbased-chihiro-net: " fmt, ##__VA_ARGS__); \
        }                                                              \
    } while (0)

#endif
