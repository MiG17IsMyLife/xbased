/*
 * Chihiro media board ports 0x4000-0x40FF (lpc.h), as a game finds them
 * after SEGABOOT.
 *
 * How the game reaches the board:
 *   0x4004  pick a board register (REG_* below)
 *   0x4000  read or write that register
 * One register, SADDR, is itself a pointer into the board's address space
 * (REG_SADDR_PTR sets it, REG_SADDR_DATA reads or writes the word there),
 * which is how the DIMM mailbox is reached:
 *   1. the game writes a command into the mailbox (REG_MBOX + 0x20..0x3C)
 *   2. it rings the doorbell (REG_MBOX_RING)
 *   3. the board puts the reply at REG_MBOX + 0x00..0x1C and, 2 ms later,
 *      sets bit 0 of port 0x40E0 and raises IRQ 10
 *   4. the game reads the reply and clears the bit
 *
 * The reply comes from the media board's real firmware on the emulated
 * V850 CPU (asic/, when NC_CHIHIRO_ASIC names a flash dump) or from the
 * built-in model (media.c). The Net DIMM registers are in lpc_net.c.
 *
 * Adapts the Net DIMM host interface of Tovarichtch's xemu
 * hw/xbox/chihiro/chihiro.c (98fbd75), LGPL-2.0-or-later.
 * Copyright (c) 2013 espes, 2018-2021 Matt Borgerson,
 * and 2026 Réda Chérif-Touil.
 */
#include "native/dev/dev.h"
#include "native/dev/chihiro/lpc.h"
#include "native/dev/chihiro/media.h"
#include "native/core/irq.h"

#include "lpc_int.h"
#include "trace.h"
#include "asic/chihiro-asic.h"

#include <stdlib.h>
#include <string.h>

/* Port offsets from 0x4000 */
#define LPC_DATA       0x00    /* the selected register's value */
#define LPC_ADDR       0x04    /* which register is selected */
#define LPC_RESET      0x08
#define DIMM_BASE_LO   0x1E    /* reads "XB" */
#define XBAM_0         0x20    /* reads "AM" */
#define XBAM_1         0x22
#define XBAM_2         0x24
#define SCRATCH        0x26
#define BB_STATUS      0x84
#define IRQ10_STATUS   0xE0    /* which replies are waiting (ChihiroLpc.e0) */
#define IRQ10_ARM      0xE1
#define IRQ10_DEASSERT 0xE2
#define CHIP_REVISION  0xF0
#define DIMM_SIZE      0xF4

/* Board registers, selected through port 0x4004 */
#define REG_SADDR_DATA 0xA0000000u   /* the word at the SADDR pointer */
#define REG_SADDR_PTR  0xA0000020u
#define REG_DMA_STATUS 0xA0000040u
#define REG_DMA_CTRL   0xA0001E60u
#define REG_ASIC_CPU   0x80000140u   /* the V850's reset latch: bit 0 = running */
#define REG_PCI_STAT   0x80000160u
#define REG_PCI_STAT2  0x80000164u
#define REG_MBOX       0x84000000u   /* +0x00-0x1C the reply, +0x20-0x3C the command */
#define REG_MBOX_RING  0x84000040u   /* write bit 0: the doorbell */
#define FW_UPLOAD_BASE 0x84800000u   /* where SEGABOOT uploads firmware for the V850 */
#define FW_UPLOAD_SIZE 0x00018000u

ChihiroLpc chihiro_lpc;
pthread_mutex_t chihiro_lpc_lock = PTHREAD_MUTEX_INITIALIZER;

/* A Type-1 board has no SADDR mailbox: the game finds the DIMM base at
 * ports 0x401E/0x4020 and writes its command to a sector of mbcom:, the
 * reply coming in the sector before it (ctx_ac[r].xbe 0x000fa350 and
 * 0x000fa700; xemu's chihiro_mbcom_process answers the same way). */
#define TYPE1_DIMM_BASE 0x01000000u     /* ports 0x4020:0x401E */
#define TYPE1_MAILBOX   0x900000u       /* mbcom: offset of the reply sector */
#define TYPE1_SECTOR    512u            /* the command sector follows it */

static bool g_type1;                /* a Type-1 (FPGA) board, not a Type-3 */
static uint8_t g_type1_box[2 * TYPE1_SECTOR];   /* the reply sector, then the command's */

static unsigned g_unknown;          /* accesses logged as not modelled */
static bool g_asic;                 /* the real media board firmware runs */
static uint8_t g_fw_upload[FW_UPLOAD_SIZE];
static uint32_t g_fw_upload_size;

/* ---------------------------------------------------------- helpers */

void chihiro_lpc_trace(const char *fmt, uint32_t a, uint32_t b)
{
    /* The first 300 lines, then one in 10,000. */
    static unsigned n;
    if (chihiro_trace_on() && (n++ < 300 || n % 10000 == 0)) {
        nc_log("xbased-chihiro-lpc", fmt, a, b, n);
    }
}

void chihiro_lpc_unknown(const char *what, uint32_t a, uint32_t v)
{
    if (g_unknown++ < 100) {
        nc_log("xbased-chihiro-lpc", "%s %08x (%08x): not modelled", what, a, v);
    }
}

void chihiro_lpc_irq_pulse(void)
{
    nc_irq_set(MEDIA_IRQ, false);
    nc_irq_set(MEDIA_IRQ, true);
}

/* Drops IRQ 10 unless some reply is still waiting. */
static void irq_drop_if_idle(void)
{
    if (!chihiro_lpc.e0) {
        nc_irq_set(MEDIA_IRQ, false);
    }
}

/* A DIMM reply is in resp: raise IRQ 10 for it in 2 ms. */
static void raise_message(void)
{
    chihiro_lpc.raise_at_ns = nc_ke_now_ns() + 2000000u;
}

/* ---------------------------------------------------------- start, tick */

void nc_chihiro_lpc_reset(unsigned dimm_factor)
{
    pthread_mutex_lock(&chihiro_lpc_lock);
    memset(&chihiro_lpc, 0, sizeof(chihiro_lpc));
    chihiro_lpc.asic_cpu = 1;       /* SEGABOOT has already started the board's CPU */
    chihiro_lpc.dimm_factor = dimm_factor;
    g_fw_upload_size = 0;
    const char *flash = getenv("NC_CHIHIRO_ASIC");
    g_asic = flash && *flash && chihiro_asic_init(flash, dimm_factor);
    if (flash && *flash && !g_asic) {
        nc_log("xbased-chihiro", "could not start the V850 from %s", flash);
    }
    pthread_mutex_unlock(&chihiro_lpc_lock);
}

void nc_chihiro_lpc_frame(void)
{
    pthread_mutex_lock(&chihiro_lpc_lock);
    /* The V850 firmware can also send a message on its own. */
    uint32_t message[8];
    if (g_asic && !chihiro_lpc.resp[0] && chihiro_asic_tick(message)) {
        memcpy(chihiro_lpc.resp, message, sizeof(message));
        raise_message();
    }
    const uint64_t now = nc_ke_now_ns();
    if (chihiro_lpc.raise_at_ns && now >= chihiro_lpc.raise_at_ns) {
        chihiro_lpc.raise_at_ns = 0;
        chihiro_lpc.e0 |= 0x01;
        uint32_t reply[2] = {chihiro_lpc.resp[0], chihiro_lpc.resp[1]};
        if (g_type1) {
            memcpy(reply, g_type1_box, sizeof(reply));
        }
        chihiro_lpc_trace("IRQ 10 raised: reply %08x %08x (#%u)", reply[0], reply[1]);
        chihiro_lpc_irq_pulse();
    }
    chihiro_lpc_net_frame(now);
    pthread_mutex_unlock(&chihiro_lpc_lock);
}

/* ---------------------------------------------------------- DIMM mailbox */

/* The doorbell: answer the command in cmd. */
static void ring(void)
{
    uint32_t msg[8];
    chihiro_lpc_trace("command %08x %08x (#%u)", chihiro_lpc.cmd[0], chihiro_lpc.cmd[1]);
    if (!(chihiro_lpc.asic_cpu & 1)) {
        return;                     /* the board's CPU is held in reset */
    }
    const bool answered = g_asic ? chihiro_asic_mailbox_exchange(chihiro_lpc.cmd, msg)
                                 : nc_chihiro_media_exchange(chihiro_lpc.dimm_factor, chihiro_lpc.cmd, msg);
    if (answered) {
        memcpy(chihiro_lpc.resp, msg, sizeof(msg));
        raise_message();
    } else if (!g_asic && chihiro_lpc.cmd[0] && !(chihiro_lpc.cmd[0] & 0x80000000u)) {
        chihiro_lpc_unknown("DIMM command", chihiro_lpc.cmd[0], chihiro_lpc.cmd[1]);
    }
    memset(chihiro_lpc.cmd, 0, sizeof(chihiro_lpc.cmd));
}

/* A read through the SADDR pointer. */
static uint32_t saddr_read(uint32_t a)
{
    if (a == REG_ASIC_CPU) {
        return chihiro_lpc.asic_cpu;
    }
    if (a == REG_PCI_STAT) {
        return 0;                   /* bit 0 clear: the network device is present */
    }
    if (a == REG_PCI_STAT2) {
        return 1;
    }
    if (a >= REG_MBOX && a <= REG_MBOX + 0x1C) {
        return chihiro_lpc.resp[(a - REG_MBOX) / 4];
    }
    if (a >= REG_MBOX + 0x20 && a <= REG_MBOX + 0x3C) {
        return chihiro_lpc.cmd[(a - REG_MBOX - 0x20) / 4];
    }
    if (a == REG_DMA_CTRL) {
        return 2;
    }
    chihiro_lpc_unknown("SADDR read", a, 0);
    return 0;
}

/* A write through the SADDR pointer. */
static void saddr_write(uint32_t a, uint32_t v)
{
    if (g_asic && a >= FW_UPLOAD_BASE && a <= FW_UPLOAD_BASE + FW_UPLOAD_SIZE - 4) {
        /* Firmware for the V850, written a word at a time from offset 0.
         * It is loaded when the CPU is next started. */
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
            chihiro_lpc.cmd[(a - REG_MBOX - 0x20) / 4] = v;
        } else {
            /* Writing 0 to the reply's first word clears the whole reply. */
            const uint32_t i = (a - REG_MBOX) / 4;
            chihiro_lpc.resp[i] = v;
            if (i == 0 && v == 0) {
                memset(chihiro_lpc.resp, 0, sizeof(chihiro_lpc.resp));
            }
        }
    } else if (a == REG_ASIC_CPU) {
        /* Start (bit 0 set) or stop the board's CPU. */
        const bool releasing = (v & 1) && !chihiro_lpc.asic_cpu;
        chihiro_lpc.asic_cpu = v & 1;
        if (g_asic) {
            if (releasing && g_fw_upload_size) {
                chihiro_asic_load_upload(g_fw_upload, g_fw_upload_size);
                g_fw_upload_size = 0;
            }
            chihiro_asic_set_running(chihiro_lpc.asic_cpu != 0);
        }
        if (!chihiro_lpc.asic_cpu) {
            /* Stopped: anything in the mailbox is lost. */
            memset(chihiro_lpc.cmd, 0, sizeof(chihiro_lpc.cmd));
            memset(chihiro_lpc.resp, 0, sizeof(chihiro_lpc.resp));
            chihiro_lpc.raise_at_ns = 0;
            chihiro_lpc.e0 &= (uint8_t)~0x01;
            irq_drop_if_idle();
        }
    } else if (a == REG_DMA_CTRL) {
        /* ignored */
    } else if (a == REG_MBOX_RING) {
        if (v & 1) {
            ring();
        }
    } else {
        chihiro_lpc_unknown("SADDR write", a, v);
    }
}

/* ---------------------------------------------------------- Type-1 mailbox */

void nc_chihiro_lpc_set_type1(bool type1)
{
    pthread_mutex_lock(&chihiro_lpc_lock);
    g_type1 = type1;
    memset(g_type1_box, 0, sizeof(g_type1_box));
    pthread_mutex_unlock(&chihiro_lpc_lock);
    if (type1) {
        nc_log("xbased-chihiro", "media board: Type-1");
    }
}

/* The command sector answered into the reply sector: the same 8 words as
 * a Type-3's mailbox, so media.c answers both. */
static void type1_command(void)
{
    uint32_t command[8], reply[8];
    memcpy(command, g_type1_box + TYPE1_SECTOR, sizeof(command));
    memset(g_type1_box + TYPE1_SECTOR, 0, TYPE1_SECTOR);
    if (!command[0] || (command[0] & 0x80000000u)) {
        return;                     /* the game clearing the sector */
    }
    chihiro_lpc_trace("Type-1 command %08x %08x (#%u)", command[0], command[1]);
    if (!nc_chihiro_media_exchange(chihiro_lpc.dimm_factor, command, reply)) {
        /* What the board does with the others (0x0009 at start) isn't
         * known: done, nothing to report, as xemu answers. */
        chihiro_lpc_unknown("Type-1 command", command[0], command[1]);
        memset(reply, 0, sizeof(reply));
        reply[0] = command[0] | 0x80000000u;
    }
    memset(g_type1_box, 0, TYPE1_SECTOR);
    memcpy(g_type1_box, reply, sizeof(reply));
    raise_message();
}

bool nc_chihiro_lpc_type1_mailbox(uint64_t off, void *buf, uint32_t len, bool write)
{
    if (!g_type1 || off < TYPE1_MAILBOX || off >= TYPE1_MAILBOX + sizeof(g_type1_box)) {
        return false;
    }
    const uint32_t at = (uint32_t)(off - TYPE1_MAILBOX);
    const uint32_t n = len < sizeof(g_type1_box) - at ? len : (uint32_t)sizeof(g_type1_box) - at;
    pthread_mutex_lock(&chihiro_lpc_lock);
    if (!write) {
        memcpy(buf, g_type1_box + at, n);
    } else {
        memcpy(g_type1_box + at, buf, n);
        if (at + n > TYPE1_SECTOR) {
            type1_command();
        }
    }
    pthread_mutex_unlock(&chihiro_lpc_lock);
    return true;
}

/* ---------------------------------------------------------- registers */

static uint32_t reg_read(uint32_t a)
{
    uint32_t v;
    switch (a) {
    case REG_ASIC_CPU:
    case REG_PCI_STAT:
    case REG_PCI_STAT2:
    case REG_DMA_CTRL:
        return saddr_read(a);
    case REG_SADDR_DATA:
        v = saddr_read(chihiro_lpc.saddr);
        chihiro_lpc.saddr += 4;
        return v;
    case REG_SADDR_PTR:
        return chihiro_lpc.saddr;
    case REG_DMA_STATUS:
        return chihiro_lpc.dma_status;
    default:
        if (chihiro_lpc_net_read(a, &v)) {
            return v;
        }
        chihiro_lpc_unknown("register read", a, 0);
        return 0;
    }
}

static void reg_write(uint32_t a, uint32_t v)
{
    switch (a) {
    case REG_SADDR_PTR:
        chihiro_lpc.saddr = v;
        break;
    case REG_DMA_STATUS:
        chihiro_lpc.dma_status = v;
        chihiro_lpc.dma_active = v & 0x80000000u;
        break;
    case REG_DMA_CTRL:
        break;                      /* the game's data is already loaded (direct boot) */
    case REG_SADDR_DATA:
        saddr_write(chihiro_lpc.saddr, v);
        if (chihiro_lpc.dma_active) {
            chihiro_lpc.saddr += 4;
        }
        break;
    default:
        if (!chihiro_lpc_net_write(a, v)) {
            chihiro_lpc_unknown("register write", a, v);
        }
        break;
    }
}

/* ---------------------------------------------------------- ports */

bool nc_chihiro_lpc_in(uint16_t port, unsigned bytes, uint32_t *value)
{
    pthread_mutex_lock(&chihiro_lpc_lock);
    switch (port - 0x4000) {
    case LPC_DATA:
        *value = reg_read(chihiro_lpc.reg_addr);
        break;
    case LPC_ADDR:
        *value = chihiro_lpc.reg_addr;
        break;
    case DIMM_BASE_LO:
        /* A Type-1 gives the DIMM base here and "XBAM" at 0x4022 only. */
        *value = g_type1 ? (uint16_t)TYPE1_DIMM_BASE : 0x4258;
        break;
    case XBAM_0:
        *value = g_type1 ? (uint16_t)(TYPE1_DIMM_BASE >> 16) : 0x4D41;
        break;
    case XBAM_1:
        *value = 0x4258;            /* "XB": the ports spell "XBAM" after SEGABOOT */
        break;
    case XBAM_2:
        *value = 0x4D41;            /* "AM" */
        break;
    case SCRATCH:
        *value = chihiro_lpc.scratch;
        break;
    case BB_STATUS:
        *value = 0;
        break;
    case IRQ10_STATUS:
        *value = chihiro_lpc.e0;
        break;
    case CHIP_REVISION:
        /* High byte: 1 the ASIC (Type-3), 0 the FPGA (Type-1, revision 1). */
        *value = g_type1 ? 0x0001 : 0x0100;
        break;
    case DIMM_SIZE:
        /* The DIMM size jumpers, plus 0x40 for the Type-3's DIP register. */
        *value = chihiro_lpc.dimm_factor | (g_type1 ? 0 : 0x40);
        break;
    default:
        chihiro_lpc_unknown("port read", port, 0);
        *value = 0;
        break;
    }
    pthread_mutex_unlock(&chihiro_lpc_lock);
    return true;
}

bool nc_chihiro_lpc_out(uint16_t port, unsigned bytes, uint32_t value)
{
    pthread_mutex_lock(&chihiro_lpc_lock);
    if (port >= 0x40E0 && port <= 0x40E2) {
        chihiro_lpc_trace("port %04x <- %02x (#%u)", port, value);
    }
    switch (port - 0x4000) {
    case LPC_DATA:
        reg_write(chihiro_lpc.reg_addr, value);
        break;
    case LPC_ADDR:
        chihiro_lpc.reg_addr = value;
        break;
    case LPC_RESET:
        break;
    case SCRATCH:
        chihiro_lpc.scratch = (uint16_t)value;
        if (g_type1) {
            /* A Type-1 game's IRQ 10 handler writes 0x102 here: taken. */
            chihiro_lpc.e0 &= (uint8_t)~0x01;
            irq_drop_if_idle();
        }
        break;
    case IRQ10_STATUS:
        /* The game acknowledges replies by writing their bits. */
        chihiro_lpc_net_ack(value);
        chihiro_lpc.e0 &= (uint8_t)~value;
        irq_drop_if_idle();
        break;
    case IRQ10_ARM:
        /* Nonzero arms. 0 acknowledges the DIMM reply (if armed) and
         * drops the interrupt line. */
        if (value) {
            chihiro_lpc.e1_armed = true;
        } else {
            if (chihiro_lpc.e1_armed) {
                chihiro_lpc.e0 &= (uint8_t)~0x01;
            } else {
                chihiro_lpc.scratch &= (uint16_t)~0x0100;
            }
            nc_irq_set(MEDIA_IRQ, false);
            chihiro_lpc.e1_armed = false;
        }
        break;
    case IRQ10_DEASSERT:
        nc_irq_set(MEDIA_IRQ, false);
        break;
    default:
        chihiro_lpc_unknown("port write", port, value);
        break;
    }
    pthread_mutex_unlock(&chihiro_lpc_lock);
    return true;
}
