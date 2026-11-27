/*
 * Chihiro debug tracing, shared by the baseboard and the media board.
 *
 * Set the environment variable NC_TRACE_CHIHIRO to log USB, JVS, card
 * reader and media board traffic. Each call site has its own line budget
 * so a busy one can't flood the log.
 */
#ifndef NATIVE_DEV_CHIHIRO_TRACE_H
#define NATIVE_DEV_CHIHIRO_TRACE_H

#include <stdbool.h>
#include <stdint.h>

/* True if NC_TRACE_CHIHIRO is set. */
bool chihiro_trace_on(void);

/* True if tracing is on and *budget lines are left; uses up one. */
bool chihiro_trace(unsigned *budget);

/* Up to 24 bytes as " xx xx ..." for a log line. Returns out. */
#define CHIHIRO_HEX_CAP (3 * 24 + 1)
const char *chihiro_hex(const uint8_t *bytes, unsigned n, char out[CHIHIRO_HEX_CAP]);

#endif
