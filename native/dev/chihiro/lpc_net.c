/*
 * Chihiro Net DIMM: the network side of the media board's ports (lpc.h).
 *
 * The game exchanges 8-word commands and replies with the Net DIMM
 * through three board registers:
 *   REG_NET_CMD    write the command a word at a time; reading it polls
 *                  the command slot and restarts the word count
 *   REG_SHARED_ST  reading it is the doorbell: "the command is complete"
 *   REG_NET_RESP   read the reply a word at a time; writing it says
 *                  "reply taken"
 * A ready reply sets bit 2 of port 0x40E0 and raises IRQ 10.
 *
 * Who answers:
 *   link play   the network board's real firmware (net/chihiro-netboard.c)
 *   otherwise   the built-in model (media.c), through a small queue that
 *               sends each reply when its delay is over
 */
#include "native/dev/chihiro/lpc.h"
#include "native/dev/chihiro/media.h"
#include "native/core/irq.h"
#include "native/core/nc.h"

#include "lpc_int.h"
#include "net/chihiro-netboard.h"

#include <stdatomic.h>
#include <string.h>

#define REG_SHARED_ST  0x90000000u
#define REG_NET_RESP   0x91000000u
#define REG_NET_CMD    0x91000200u

static atomic_bool g_board;         /* the network board firmware runs */

/* ---------------------------------------------------------- built-in */

/* Sends the earliest due reply, once the game has taken the last one. */
static void net_publish(uint64_t now)
{
    if (chihiro_lpc.net.response[0] || (chihiro_lpc.e0 & 0x04)) {
        return;
    }
    NetCompletion *ready = NULL;
    for (unsigned i = 0; i < NET_QUEUE_SIZE; i++) {
        NetCompletion *entry = &chihiro_lpc.net_queue[i];
        if (entry->due_ns && entry->due_ns <= now && (!ready || entry->due_ns < ready->due_ns)) {
            ready = entry;
        }
    }
    if (ready) {
        memcpy(chihiro_lpc.net.response, ready->reply, sizeof(chihiro_lpc.net.response));
        ready->due_ns = 0;
        chihiro_lpc.net_resp_i = 0;
        chihiro_lpc.e0 |= 0x04;
        chihiro_lpc_trace("network reply %08x %08x (#%u)", ready->reply[0], ready->reply[1]);
        chihiro_lpc_irq_pulse();
    }
}

/* The doorbell: answers the game's command with the built-in model and
 * queues the reply. The command slot is then free again. */
static void net_ring(void)
{
    uint32_t *command = chihiro_lpc.net.command;
    if (!command[0]) {
        return;
    }
    for (unsigned i = 0; i < NET_QUEUE_SIZE; i++) {
        NetCompletion *entry = &chihiro_lpc.net_queue[i];
        if (!entry->due_ns) {
            uint32_t delay_ms;
            chihiro_lpc_trace("network command %08x %08x (#%u)", command[0], command[1]);
            if (nc_chihiro_net_exchange(chihiro_lpc.dimm_factor, command, entry->reply, &delay_ms)) {
                entry->due_ns = nc_ke_now_ns() + (uint64_t)delay_ms * 1000000u;
            } else {
                chihiro_lpc_unknown("Net DIMM command", command[0], command[1]);
            }
            command[0] = 0;
            return;
        }
    }
}

void chihiro_lpc_net_frame(uint64_t now)
{
    if (!g_board) {
        net_publish(now);
    }
}

/* ---------------------------------------------------------- network board */

/* The network board firmware has a reply for the game. */
static void board_rang(void)
{
    pthread_mutex_lock(&chihiro_lpc_lock);
    chihiro_lpc.e0 |= 0x04;
    chihiro_lpc_trace("network board rang (#%u)", 0, 0);
    nc_irq_set(MEDIA_IRQ, true);
    pthread_mutex_unlock(&chihiro_lpc_lock);
}

bool nc_chihiro_lpc_load_network(const char *firmware, const char serial[16], unsigned cabinet)
{
    const uint32_t ip = 10u | (uint32_t)cabinet << 24;      /* 10.0.0.<cabinet> */
    if (!chihiro_netboard_init(firmware, serial, ip)) {
        nc_log("xbased-chihiro", "link: could not load network board firmware at %s; playing alone", firmware);
        return false;
    }
    chihiro_netboard_set_host_interrupt(board_rang);
    g_board = true;
    nc_log("xbased-chihiro", "link: cabinet %u, 10.0.0.%u, network board firmware %s", cabinet, cabinet, firmware);
    return true;
}

void nc_chihiro_lpc_attach_network(void)
{
    if (g_board) {
        chihiro_netboard_attach_net();
    }
}

bool nc_chihiro_lpc_link_active(void)
{
    return g_board;
}

void chihiro_lpc_net_ack(uint32_t bits)
{
    if (g_board && (bits & 0x04)) {
        chihiro_netboard_host_ack();
    }
}

/* ---------------------------------------------------------- shared memory */

bool nc_chihiro_net_window(uint32_t off, void *buf, uint32_t len, bool write)
{
    if (off > NET_WINDOW_SIZE || len > NET_WINDOW_SIZE - off) {
        return false;
    }
    if (g_board) {
        if (write) {
            chihiro_netboard_host_window_write(off, buf, (int)len);
        } else {
            chihiro_netboard_host_window_read(off, buf, (int)len);
        }
        return true;
    }
    pthread_mutex_lock(&chihiro_lpc_lock);
    uint8_t *window = (uint8_t *)&chihiro_lpc.net + off;
    if (write) {
        memcpy(window, buf, len);
    } else {
        memcpy(buf, window, len);
    }
    pthread_mutex_unlock(&chihiro_lpc_lock);
    return true;
}

/* ---------------------------------------------------------- registers */

bool chihiro_lpc_net_read(uint32_t reg, uint32_t *value)
{
    uint32_t pkt[8];
    switch (reg) {
    case REG_SHARED_ST:
        if (g_board) {
            chihiro_netboard_host_ring();
        } else {
            net_ring();
        }
        *value = 1;
        return true;
    case REG_NET_RESP:
        if (g_board) {
            chihiro_netboard_host_response(pkt, sizeof(pkt));
        } else {
            memcpy(pkt, chihiro_lpc.net.response, sizeof(pkt));
        }
        /* The next word; the last one repeats. */
        *value = pkt[chihiro_lpc.net_resp_i & 7];
        if (chihiro_lpc.net_resp_i < 7) {
            chihiro_lpc.net_resp_i++;
        }
        return true;
    case REG_NET_CMD:
        chihiro_lpc.net_cmd_i = 0;
        if (g_board) {
            chihiro_netboard_host_command_slot(pkt, sizeof(pkt));
            *value = pkt[0];
        } else {
            *value = chihiro_lpc.net.command[0];
        }
        return true;
    default:
        return false;
    }
}

bool chihiro_lpc_net_write(uint32_t reg, uint32_t value)
{
    switch (reg) {
    case REG_NET_CMD:
        if (chihiro_lpc.net_cmd_i < 8) {
            chihiro_lpc.net.command[chihiro_lpc.net_cmd_i++] = value;
            if (g_board && chihiro_lpc.net_cmd_i == 8) {
                chihiro_netboard_host_command(chihiro_lpc.net.command, sizeof(chihiro_lpc.net.command));
            }
        }
        return true;
    case REG_NET_RESP:
        memset(chihiro_lpc.net.response, 0, sizeof(chihiro_lpc.net.response));
        chihiro_lpc.net_resp_i = 0;
        chihiro_lpc.net_cmd_i = 0;
        if (g_board) {
            chihiro_netboard_host_release();
        }
        return true;
    default:
        return false;
    }
}
