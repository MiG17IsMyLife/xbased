/*
 * Chihiro debug tracing (trace.h).
 */
#include "trace.h"

#include <stdio.h>
#include <stdlib.h>

bool chihiro_trace_on(void)
{
    static int on = -1;     /* -1 until the environment has been read */
    if (on < 0) {
        on = getenv("NC_TRACE_CHIHIRO") != NULL;
    }
    return on;
}

bool chihiro_trace(unsigned *budget)
{
    return chihiro_trace_on() && *budget && (*budget)--;
}

const char *chihiro_hex(const uint8_t *bytes, unsigned n, char out[CHIHIRO_HEX_CAP])
{
    out[0] = 0;
    for (unsigned i = 0; i < n && i < 24; i++) {
        snprintf(out + 3 * i, 4, " %02x", bytes[i]);
    }
    return out;
}
