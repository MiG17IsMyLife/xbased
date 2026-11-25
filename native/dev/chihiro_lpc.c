/*
 * xbased native core — the Chihiro media board's LPC ports (0x4000-0x40FF)
 * as a game finds them after SegaBoot: an indirect register window, the
 * SADDR window into the board, and IRQ 10, shared by the DIMM and Net DIMM
 * mailboxes. The Net DIMM host interface adapts Tovarichtch's xemu
 * hw/xbox/chihiro/chihiro.c (98fbd75), LGPL-2.0-or-later.
 * Copyright (c) 2013 espes, 2018-2021 Matt Borgerson,
 * and 2026 Réda Chérif-Touil.
 */
#include "native/dev/dev.h"
#include "native/dev/chihiro_board.h"
#include "native/core/irq.h"
#include "chihiro/net/chihiro-netboard.h"
#include "chihiro/asic/chihiro-asic.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define LPC_DATA       0x00    /* 0x4000: the register at the address */
#define LPC_ADDR       0x04    /* 0x4004: the register's address */
#define LPC_RESET      0x08
#define DIMM_BASE_LO   0x1E    /* "XB" for the game */
#define XBAM_0         0x20    /* "AM" */
#define XBAM_1         0x22
#define XBAM_2         0x24
#define SCRATCH        0x26
#define BB_STATUS      0x84
#define IRQ10_STATUS   0xE0    /* bit 0: a message from the board */
#define IRQ10_ARM      0xE1
#define IRQ10_DEASSERT 0xE2
#define CHIP_REVISION  0xF0
#define DIMM_SIZE      0xF4

#define REG_SADDR_DATA 0xA0000000u   /* the word at the SADDR pointer */
#define REG_SADDR_PTR  0xA0000020u
#define REG_DMA_STATUS 0xA0000040u
#define REG_DMA_CTRL   0xA0001E60u
#define REG_ASIC_CPU   0x80000140u   /* the V850's latch: bit 0 released */
#define REG_PCI_STAT   0x80000160u
#define REG_PCI_STAT2  0x80000164u
#define REG_MBOX       0x84000000u   /* 0x00-0x1C the answer, 0x20-0x3C the command */
#define REG_MBOX_RING  0x84000040u
#define FW_UPLOAD_BASE 0x84800000u
#define FW_UPLOAD_SIZE 0x00018000u
#define REG_SHARED_ST  0x90000000u
/* The Net DIMM's response and command word streams. */
#define REG_NET_RESP   0x91000000u
#define REG_NET_CMD    0x91000200u

#define MEDIA_IRQ 10
#define NET_QUEUE_SIZE 8
#define NET_SECTOR_SIZE 512
#define NET_WINDOW_SIZE (1u << 20)

/* The LPC port streams and mbcom sectors address the same dual-port RAM. */
typedef struct NetMemory {
    uint32_t response[8];
    uint8_t response_data[NET_SECTOR_SIZE - 32];
    uint32_t command[8];
    uint8_t command_data[NET_WINDOW_SIZE - NET_SECTOR_SIZE - 32];
} NetMemory;

_Static_assert(sizeof(NetMemory) == NET_WINDOW_SIZE, "Net DIMM window size");

typedef struct NetCompletion {
    uint64_t due_ns;
    uint32_t reply[8];
} NetCompletion;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static struct {
    uint32_t reg_addr;      /* 0x4004 */
    uint32_t saddr;         /* the SADDR pointer, auto-incremented */
    uint32_t dma_status;
    bool dma_active;
    uint32_t asic_cpu;      /* SegaBoot released the V850: 1 */
    uint32_t cmd[8];
    uint32_t resp[8];
    uint16_t scratch;
    uint8_t e0;
    bool e1_armed;
    unsigned dimm_factor;   /* 0 128 MB .. 3 1 GB */
    uint64_t raise_at_ns;   /* a message for the game, raised then (0: none) */
    NetMemory net;
    unsigned net_cmd_i, net_resp_i;
    NetCompletion net_queue[NET_QUEUE_SIZE];
} g_lpc;

static unsigned g_unknown;
static atomic_bool g_board;
static bool g_asic;
static uint8_t g_fw_upload[FW_UPLOAD_SIZE];
static uint32_t g_fw_upload_size;

/* NC_TRACE_CHIHIRO: the media board's messages and IRQ 10 */
static int g_trace = -1;
static void trace(const char *fmt, uint32_t a, uint32_t b)
{
    static unsigned n;
    if (g_trace < 0) {
        g_trace = getenv("NC_TRACE_CHIHIRO") != NULL;
    }
    if (g_trace > 0 && (n++ < 300 || n % 10000 == 0)) {
        nc_log("xbased-chihiro-lpc", fmt, a, b, n);
    }
}

uint64_t nc_ke_now_ns(void);

static void unknown(const char *what, uint32_t a, uint32_t v)
{
    if (g_unknown++ < 100) {
        nc_log("xbased-chihiro-lpc", "%s %08x (%08x): not modelled", what, a, v);
    }
}

void nc_chihiro_lpc_reset(unsigned dimm_factor)
{
    pthread_mutex_lock(&g_lock);
    memset(&g_lpc, 0, sizeof(g_lpc));
    g_lpc.asic_cpu = 1;             /* SegaBoot has released the board's CPU */
    g_lpc.dimm_factor = dimm_factor;
    g_fw_upload_size = 0;
    const char *flash = getenv("NC_CHIHIRO_ASIC");
    g_asic = flash && *flash && chihiro_asic_init(flash, dimm_factor);
    if (flash && *flash && !g_asic) {
        nc_log("xbased-chihiro", "could not start the V850 from %s", flash);
    }
    pthread_mutex_unlock(&g_lock);
}

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
    pthread_mutex_lock(&g_lock);
    uint8_t *window = (uint8_t *)&g_lpc.net + off;
    if (write) {
        memcpy(window, buf, len);
    } else {
        memcpy(buf, window, len);
    }
    pthread_mutex_unlock(&g_lock);
    return true;
}

/* A DIMM reply raises IRQ 10 after 2 ms. */
static void raise_message(void)
{
    g_lpc.raise_at_ns = nc_ke_now_ns() + 2000000u;
}

static void irq_pulse(void)
{
    nc_irq_set(MEDIA_IRQ, false);
    nc_irq_set(MEDIA_IRQ, true);
}

static void irq_drop_if_idle(void)
{
    if (!g_lpc.e0) {
        nc_irq_set(MEDIA_IRQ, false);
    }
}

/* Posts the earliest due network reply once the game freed the slot. */
static void net_publish(uint64_t now)
{
    if (g_lpc.net.response[0] || (g_lpc.e0 & 0x04)) {
        return;
    }
    NetCompletion *ready = NULL;
    for (unsigned i = 0; i < NET_QUEUE_SIZE; i++) {
        NetCompletion *entry = &g_lpc.net_queue[i];
        if (entry->due_ns && entry->due_ns <= now && (!ready || entry->due_ns < ready->due_ns)) {
            ready = entry;
        }
    }
    if (ready) {
        memcpy(g_lpc.net.response, ready->reply, sizeof(g_lpc.net.response));
        ready->due_ns = 0;
        g_lpc.net_resp_i = 0;
        g_lpc.e0 |= 0x04;
        trace("network reply %08x %08x (#%u)", ready->reply[0], ready->reply[1]);
        irq_pulse();
    }
}

void nc_chihiro_lpc_frame(void)
{
    pthread_mutex_lock(&g_lock);
    uint32_t message[8];
    if (g_asic && !g_lpc.resp[0] && chihiro_asic_tick(message)) {
        memcpy(g_lpc.resp, message, sizeof(message));
        raise_message();
    }
    const uint64_t now = nc_ke_now_ns();
    if (g_lpc.raise_at_ns && now >= g_lpc.raise_at_ns) {
        g_lpc.raise_at_ns = 0;
        g_lpc.e0 |= 0x01;
        trace("IRQ 10 raised: message %08x %08x (#%u)", g_lpc.resp[0], g_lpc.resp[1]);
        irq_pulse();
    }
    if (!g_board) {
        net_publish(now);
    }
    pthread_mutex_unlock(&g_lock);
}

static void board_rang(void)
{
    pthread_mutex_lock(&g_lock);
    g_lpc.e0 |= 0x04;
    trace("network board rang (#%u)", 0, 0);
    nc_irq_set(MEDIA_IRQ, true);
    pthread_mutex_unlock(&g_lock);
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

/* Queues the game's Net DIMM command; its reply sets 0x40E0 bit 2. */
static void net_ring(void)
{
    uint32_t *command = g_lpc.net.command;
    if (!command[0]) {
        return;
    }
    for (unsigned i = 0; i < NET_QUEUE_SIZE; i++) {
        NetCompletion *entry = &g_lpc.net_queue[i];
        if (!entry->due_ns) {
            uint32_t delay_ms;
            trace("network command %08x %08x (#%u)", command[0], command[1]);
            if (nc_chihiro_net_exchange(g_lpc.dimm_factor, command, entry->reply, &delay_ms)) {
                entry->due_ns = nc_ke_now_ns() + (uint64_t)delay_ms * 1000000u;
            } else {
                unknown("Net DIMM command", command[0], command[1]);
            }
            command[0] = 0;
            return;
        }
    }
}

static uint32_t saddr_read(uint32_t a)
{
    if (a == REG_ASIC_CPU) {
        return g_lpc.asic_cpu;
    }
    if (a == REG_PCI_STAT) {
        return 0;                   /* the network device present (bit 0 clear) */
    }
    if (a == REG_PCI_STAT2) {
        return 1;
    }
    if (a >= REG_MBOX && a <= REG_MBOX + 0x1C) {
        return g_lpc.resp[(a - REG_MBOX) / 4];
    }
    if (a >= REG_MBOX + 0x20 && a <= REG_MBOX + 0x3C) {
        return g_lpc.cmd[(a - REG_MBOX - 0x20) / 4];
    }
    if (a == REG_DMA_CTRL) {
        return 2;
    }
    unknown("SADDR read", a, 0);
    return 0;
}

static void ring(void)
{
    uint32_t msg[8];
    trace("command %08x %08x (#%u)", g_lpc.cmd[0], g_lpc.cmd[1]);
    if (!(g_lpc.asic_cpu & 1)) {
        return;
    }
    const bool answered = g_asic ? chihiro_asic_mailbox_exchange(g_lpc.cmd, msg)
                                 : nc_chihiro_media_exchange(g_lpc.dimm_factor, g_lpc.cmd, msg);
    if (answered) {
        memcpy(g_lpc.resp, msg, sizeof(msg));
        raise_message();
    } else if (!g_asic && g_lpc.cmd[0] && !(g_lpc.cmd[0] & 0x80000000u)) {
        unknown("DIMM command", g_lpc.cmd[0], g_lpc.cmd[1]);
    }
    memset(g_lpc.cmd, 0, sizeof(g_lpc.cmd));
}

static void saddr_write(uint32_t a, uint32_t v)
{
    if (g_asic && a >= FW_UPLOAD_BASE && a <= FW_UPLOAD_BASE + FW_UPLOAD_SIZE - 4) {
        const uint32_t off = a - FW_UPLOAD_BASE;
        if (!off) {
            memset(g_fw_upload, 0, sizeof(g_fw_upload));
            g_fw_upload_size = 0;
        }
        memcpy(g_fw_upload + off, &v, sizeof(v));
        if (off + 4 > g_fw_upload_size) {
            g_fw_upload_size = off + 4;
        }
    } else if (a >= REG_MBOX && a <= REG_MBOX + 0x3C) {
        if (g_asic) {
            chihiro_asic_host_mailbox_write(a - REG_MBOX, v);
        }
        if (a >= REG_MBOX + 0x20) {
            g_lpc.cmd[(a - REG_MBOX - 0x20) / 4] = v;
        } else {
            const uint32_t i = (a - REG_MBOX) / 4;
            g_lpc.resp[i] = v;
            if (i == 0 && v == 0) {
                memset(g_lpc.resp, 0, sizeof(g_lpc.resp));
            }
        }
    } else if (a == REG_ASIC_CPU) {
        const bool releasing = (v & 1) && !g_lpc.asic_cpu;
        g_lpc.asic_cpu = v & 1;
        if (g_asic) {
            if (releasing && g_fw_upload_size) {
                chihiro_asic_load_upload(g_fw_upload, g_fw_upload_size);
                g_fw_upload_size = 0;
            }
            chihiro_asic_set_running(g_lpc.asic_cpu != 0);
        }
        if (!g_lpc.asic_cpu) {
            memset(g_lpc.cmd, 0, sizeof(g_lpc.cmd));
            memset(g_lpc.resp, 0, sizeof(g_lpc.resp));
            g_lpc.raise_at_ns = 0;
            g_lpc.e0 &= (uint8_t)~0x01;
            irq_drop_if_idle();
        }
    } else if (a == REG_DMA_CTRL) {
        /* ignored */
    } else if (a == REG_MBOX_RING) {
        if (v & 1) {
            ring();
        }
    } else {
        unknown("SADDR write", a, v);
    }
}

static uint32_t reg_read(uint32_t a)
{
    switch (a) {
    case REG_ASIC_CPU:
    case REG_PCI_STAT:
    case REG_PCI_STAT2:
    case REG_DMA_CTRL:
        return saddr_read(a);
    case REG_SADDR_DATA: {
        const uint32_t v = saddr_read(g_lpc.saddr);
        g_lpc.saddr += 4;
        return v;
    }
    case REG_SADDR_PTR:
        return g_lpc.saddr;
    case REG_DMA_STATUS:
        return g_lpc.dma_status;
    case REG_SHARED_ST:             /* the read is also the doorbell */
        if (g_board) {
            chihiro_netboard_host_ring();
        } else {
            net_ring();
        }
        return 1;
    case REG_NET_RESP: {
        uint32_t pkt[8];
        if (g_board) {
            chihiro_netboard_host_response(pkt, sizeof(pkt));
        } else {
            memcpy(pkt, g_lpc.net.response, sizeof(pkt));
        }
        const uint32_t v = pkt[g_lpc.net_resp_i & 7];
        if (g_lpc.net_resp_i < 7) {
            g_lpc.net_resp_i++;
        }
        return v;
    }
    case REG_NET_CMD: {
        g_lpc.net_cmd_i = 0;        /* the slot poll also starts a word stream */
        if (g_board) {
            uint32_t pkt[8];
            chihiro_netboard_host_command_slot(pkt, sizeof(pkt));
            return pkt[0];
        }
        return g_lpc.net.command[0];
    }
    default:
        unknown("register read", a, 0);
        return 0;
    }
}

static void reg_write(uint32_t a, uint32_t v)
{
    switch (a) {
    case REG_SADDR_PTR:
        g_lpc.saddr = v;
        break;
    case REG_DMA_STATUS:
        g_lpc.dma_status = v;
        g_lpc.dma_active = v & 0x80000000u;
        break;
    case REG_DMA_CTRL:
        break;                      /* direct boot already owns the DIMM data */
    case REG_SADDR_DATA:
        saddr_write(g_lpc.saddr, v);
        if (g_lpc.dma_active) {
            g_lpc.saddr += 4;
        }
        break;
    case REG_NET_CMD:
        if (g_lpc.net_cmd_i < 8) {
            g_lpc.net.command[g_lpc.net_cmd_i++] = v;
            if (g_board && g_lpc.net_cmd_i == 8) {
                chihiro_netboard_host_command(g_lpc.net.command, sizeof(g_lpc.net.command));
            }
        }
        break;
    case REG_NET_RESP:              /* the game took the answer */
        memset(g_lpc.net.response, 0, sizeof(g_lpc.net.response));
        g_lpc.net_resp_i = 0;
        g_lpc.net_cmd_i = 0;
        if (g_board) {
            chihiro_netboard_host_release();
        }
        break;
    default:
        unknown("register write", a, v);
        break;
    }
}

bool nc_chihiro_lpc_in(uint16_t port, unsigned bytes, uint32_t *value)
{
    pthread_mutex_lock(&g_lock);
    switch (port - 0x4000) {
    case LPC_DATA:
        *value = reg_read(g_lpc.reg_addr);
        break;
    case LPC_ADDR:
        *value = g_lpc.reg_addr;
        break;
    case DIMM_BASE_LO:
    case XBAM_1:
        *value = 0x4258;            /* "XBAM", as the game reads it after SegaBoot */
        break;
    case XBAM_0:
    case XBAM_2:
        *value = 0x4D41;
        break;
    case SCRATCH:
        *value = g_lpc.scratch;
        break;
    case BB_STATUS:
        *value = 0;
        break;
    case IRQ10_STATUS:
        *value = g_lpc.e0;
        break;
    case CHIP_REVISION:
        *value = 0x0100;            /* Type-3: the ASIC */
        break;
    case DIMM_SIZE:
        *value = g_lpc.dimm_factor | 0x40;  /* the jumpers; 0x40 the Type-3's DIP register */
        break;
    default:
        unknown("port read", port, 0);
        *value = 0;
        break;
    }
    pthread_mutex_unlock(&g_lock);
    return true;
}

bool nc_chihiro_lpc_out(uint16_t port, unsigned bytes, uint32_t value)
{
    pthread_mutex_lock(&g_lock);
    if (port >= 0x40E0 && port <= 0x40E2) {
        trace("port %04x <- %02x (#%u)", port, value);
    }
    switch (port - 0x4000) {
    case LPC_DATA:
        reg_write(g_lpc.reg_addr, value);
        break;
    case LPC_ADDR:
        g_lpc.reg_addr = value;
        break;
    case LPC_RESET:
        break;
    case SCRATCH:
        g_lpc.scratch = (uint16_t)value;
        break;
    case IRQ10_STATUS:              /* the game's acknowledgement: those bits clear */
        if (g_board && (value & 0x04)) {
            chihiro_netboard_host_ack();
        }
        g_lpc.e0 &= (uint8_t)~value;
        irq_drop_if_idle();
        break;
    case IRQ10_ARM:
        /* nonzero arms; 0 acknowledges the message and drops the line */
        if (value) {
            g_lpc.e1_armed = true;
        } else {
            if (g_lpc.e1_armed) {
                g_lpc.e0 &= (uint8_t)~0x01;
            } else {
                g_lpc.scratch &= (uint16_t)~0x0100;
            }
            nc_irq_set(MEDIA_IRQ, false);
            g_lpc.e1_armed = false;
        }
        break;
    case IRQ10_DEASSERT:
        nc_irq_set(MEDIA_IRQ, false);
        break;
    default:
        unknown("port write", port, value);
        break;
    }
    pthread_mutex_unlock(&g_lock);
    return true;
}
