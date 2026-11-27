/*
 * xbased native core — Chihiro Type-3 DIMM and its Net DIMM command protocol.
 * The game starts with its filesystem already mounted in DIMM memory. The
 * LPC side delivers replies and interrupts on the emulated clock.
 */
#include "native/dev/chihiro_board.h"

#include <string.h>

enum {
    DIMM_CONNECT = 0x0001,
    DIMM_STATUS = 0x0100,
    DIMM_VERSION = 0x0101,
    DIMM_SYSTEM = 0x0102,
    DIMM_SERIAL = 0x0103,
    NET_CAPACITY = 0x0104,
    NET_INTERFACE = 0x0415,
    NET_LINK_STATE = 0x0601,
    NET_PEER_COUNT = 0x0602,
    NET_POLL = 0x0606,
    NET_KEEPALIVE = 0x0607,
    NET_PING = 0x0608,
};

#define DIMM_FIRMWARE 0x0115u
#define NET_FIRMWARE 0x1305u
#define NET_PACKET_LIMIT 1024u

static char g_serial[16];

void nc_chihiro_media_init(const char serial[16])
{
    memcpy(g_serial, serial, sizeof(g_serial));
}

static bool dimm_query(bool network, unsigned dimm_factor, uint16_t operation, uint32_t reply[8])
{
    switch (operation) {
    case DIMM_CONNECT:
        reply[1] = 0x08000000u << dimm_factor;
        return true;
    case DIMM_STATUS:
        reply[1] = 5;       /* the direct boot has already loaded the game */
        reply[2] = 100;
        return true;
    case DIMM_VERSION:
        reply[1] = network ? 0x00010000u | NET_FIRMWARE : 0x00020000u | DIMM_FIRMWARE;
        return true;
    case DIMM_SYSTEM:
        if (network) {
            return false;
        }
        /* the media board's firmware answers 203h; bit 16 would be a
         * development board (SEGABOOT's "Develop mode") */
        reply[1] = 0x0203u;
        return true;
    case DIMM_SERIAL:
        memcpy(reply + 1, g_serial, sizeof(g_serial));
        return true;
    default:
        return false;
    }
}

bool nc_chihiro_media_exchange(unsigned dimm_factor, const uint32_t command[8], uint32_t reply[8])
{
    memset(reply, 0, 8 * sizeof(*reply));
    if (!command[0] || !dimm_query(false, dimm_factor, (uint16_t)(command[0] >> 16), reply)) {
        return false;
    }
    reply[0] = command[0] | 0x80000000u;
    return true;
}

bool nc_chihiro_net_exchange(unsigned dimm_factor, const uint32_t command[8], uint32_t reply[8],
                             uint32_t *delay_ms)
{
    const uint16_t operation = (uint16_t)(command[0] >> 16);
    if (!command[0] || (operation & 0x8000u)) {
        return false;
    }
    memset(reply, 0, 8 * sizeof(*reply));
    *delay_ms = 1;
    if (!dimm_query(true, dimm_factor, operation, reply)) {
        switch (operation) {
        case NET_CAPACITY:
            reply[1] = NET_PACKET_LIMIT;
            break;
        case NET_INTERFACE:     /* no link partner on a standalone cabinet */
            reply[2] = command[2];
            *delay_ms = 1000;
            break;
        case NET_LINK_STATE:
        case NET_PEER_COUNT:
        case NET_KEEPALIVE:
            break;
        case NET_POLL:
            reply[2] = command[2];
            break;
        case NET_PING:
            reply[1] = UINT32_MAX;
            reply[2] = command[2];
            *delay_ms = command[2] & 0xffffu;
            break;
        default:
            return false;
        }
    }
    reply[0] = command[0] | 0x80000000u;
    return true;
}
