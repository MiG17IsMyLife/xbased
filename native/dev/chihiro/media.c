/*
 * Chihiro media board, built-in model (media.h).
 *
 * The real Type-3 media board keeps the game in DIMM memory. Here the
 * game's files are already mounted when it starts (direct boot), so the
 * model just answers status queries as a board that has finished loading.
 *
 * A command is 8 words. command[0]'s upper 16 bits are the operation;
 * the reply echoes command[0] with bit 31 set, and the answer follows in
 * reply[1..].
 */
#include "native/dev/chihiro/media.h"

#include <string.h>

/* Operations. 0x00xx-0x01xx are DIMM queries both sides answer; the rest
 * are Net DIMM (network) operations. */
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

#define DIMM_FIRMWARE 0x0115u       /* media board firmware version */
#define NET_FIRMWARE 0x1305u        /* network board firmware version (ver1305.bin) */
#define NET_PACKET_LIMIT 1024u

static char g_serial[16];

void nc_chihiro_media_init(const char serial[16])
{
    memcpy(g_serial, serial, sizeof(g_serial));
}

/* The DIMM queries. network: asked through the Net DIMM side. */
static bool dimm_query(bool network, unsigned dimm_factor, uint16_t operation, uint32_t reply[8])
{
    switch (operation) {
    case DIMM_CONNECT:
        reply[1] = 0x08000000u << dimm_factor;  /* memory size: 128 MB << factor */
        return true;
    case DIMM_STATUS:
        reply[1] = 5;               /* 5 = game loaded (direct boot already did it) */
        reply[2] = 100;             /* 100% */
        return true;
    case DIMM_VERSION:
        reply[1] = network ? 0x00010000u | NET_FIRMWARE : 0x00020000u | DIMM_FIRMWARE;
        return true;
    case DIMM_SYSTEM:
        if (network) {
            return false;
        }
        /* The real firmware answers 0x203. Bit 16 would mark a development
         * board (SEGABOOT's "Develop mode"). */
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
        return false;               /* empty, or already a reply */
    }
    memset(reply, 0, 8 * sizeof(*reply));
    *delay_ms = 1;
    if (!dimm_query(true, dimm_factor, operation, reply)) {
        switch (operation) {
        case NET_CAPACITY:
            reply[1] = NET_PACKET_LIMIT;
            break;
        case NET_INTERFACE:
            /* A cabinet playing alone has no link partner: the request
             * is echoed back after 1 s. */
            reply[2] = command[2];
            *delay_ms = 1000;
            break;
        case NET_LINK_STATE:
        case NET_PEER_COUNT:
        case NET_KEEPALIVE:
            break;                  /* all zero: no link, no peers */
        case NET_POLL:
            reply[2] = command[2];
            break;
        case NET_PING:
            /* No one to answer: all ones, sent after the wait the ping
             * asks for (low 16 bits of command[2], in ms). */
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
