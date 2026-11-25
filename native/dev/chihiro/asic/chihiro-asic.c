/*
 * Chihiro Type-3 media board ASIC — the machine around the V850 core
 *
 * Copyright (c) 2026 Réda Chérif-Touil
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 * What the firmware sees: every address below was MEASURED by running the
 * firmware on the V850 core and recording each access outside its own memory;
 * an unknown register reads back as zero.
 *
 *   0x00000000-0x00017FFF  the firmware image, reset vector at 0
 *   0x0FFF0000-0x0FFFFFFF  work RAM        (gp = 0x0FFFF000, ep = 0x0FFF0000)
 *   0xFFFF8000-0xFFFFEFFF  internal RAM, where the RTOS keeps its scheduler
 *   0xFFFFF000-0xFFFFFFFF  V850 on-chip peripherals
 *
 *   0x0FE00060  straps. Bits 0-2 pick the board: 0 -> type 4, 1 -> 2, 2 -> 3,
 *               3 -> 3, 4 -> 0, 6 -> 1, anything else -> 0xFF (unsupported).
 *               Read once, at the head of the board init.
 *   0x0FE00000  the board-to-host interrupt register. The reply pump writes 0
 *               there, then sets bit 2 once all 32 answer bytes are in place
 *   0x0FE00020  the DIMM SPD bus: bit 0 is SCL, bit 1 is SDA
 *   0x0FE00024  bit 1 enables the SDA output, 0 releases the line
 *   0x0FE00010  written 1 at the end of the board init
 *   0x0FE00030  the DIMM size, encoded as (size >> 26) - 1
 *   0x0FE000F0  the doorbell the idle loop polls; never rung here: reads 0,
 *               writes dropped
 *   0x0FE000F8  console enable, 0x0FE000FC console buffer pointer
 *   0x0FC00000  where the firmware publishes the DIMM size for the host
 *   0x0FC00004-0x0FC0003C  fifteen words read once and summed at startup
 *   0x0C000000-3F  the host window: 0x0C000020/24/28 take the DIMM window
 *               descriptors; the host link (status at 0x0C000000, vector
 *               0xD0) is not modelled: reads 0, writes dropped
 *   0x0F800024/A0/A4/E0/E4/E8  written once during init
 *   0x0FD00020/24/28  read once
 *   0x0FB78000  a block table, valid when it holds 0x57377521
 *   0x00400004/08/18/20/24/30  the security PIC: three data bits and a clock,
 *               bit-banged. The 0.85 image carries on without it ("Pic is
 *               not alive."); the firmware a game uploads stays at 26 % until
 *               it answers (chihiro-pic.c).
 */
#include "../net/nb_compat.h"
#include "chihiro-asic.h"
#include "chihiro-v850.h"
#include "chihiro-spd.h"
#include "chihiro-des.h"
#include "chihiro-pic.h"

#define ASIC_ROM_SIZE   0x00018000u
#define ASIC_RAM_BASE   0x0FFF0000u
#define ASIC_RAM_SIZE   0x00010000u
#define ASIC_IRAM_BASE  0xFFFF8000u
#define ASIC_IRAM_SIZE  0x00007000u
#define ASIC_PERI_BASE  0xFFFFF000u
#define ASIC_PERI_WORDS 0x400u

/* HEURISTIC: a 33 MHz V850 (no source for the crystal yet), counted in clocks
 * as the core charges them (see chihiro-v850.h). */
#define ASIC_CLOCKS_PER_MS 33000

#define ASIC_IDLE_REGS 40           /* r1-r31 and nine system registers */
#define ASIC_IDLE_LOG  64           /* bytes a repeating turn may change */

typedef struct ChihiroAsic {
    V850State cpu;
    uint8_t  rom[ASIC_ROM_SIZE];
    uint8_t  ram[ASIC_RAM_SIZE];
    uint8_t  iram[ASIC_IRAM_SIZE];
    uint32_t peri[ASIC_PERI_WORDS];

    bool     have_rom;
    bool     running;
    bool     released;          /* the host's latch: stays set if the core stops */
    bool     reported;          /* the console has been read out once */
    uint32_t straps;
    ChihiroSpd spd;             /* the DIMM SPD EEPROMs on 0x0FE00020/24 */
    ChihiroPic pic;             /* the security PIC on 0x00400004/08 */
    unsigned dimm_mb;           /* how much DIMM the board carries */
    uint32_t dimm_blocks[16];   /* the block table at 0x0FB78000 */
    /* The shared mailbox: the host's 0x84000000 is the board's 0x0FC00000,
     * 0x84000020 is 0x0FC00020 (the router reads 32 bytes at offset 0x20). */
    uint32_t mbox[16];          /* 0x0FC00000 response, 0x0FC00020 command */
    /* 0x0FE00000: bit 2 set once the whole 32-byte answer is in the window. */
    uint32_t host_irq;
    bool     mbox_answered;     /* a message is posted, not yet taken */
    uint8_t *flash;
    uint32_t flash_size;
    uint64_t  steps;
    int       clocks_to_tick;

    /* The idle loop, recognised while it runs (see asic_idle_turn). */
    uint32_t  idle_pc;          /* the turn's starting point, sampled */
    uint32_t  idle_regs[ASIC_IDLE_REGS];    /* the core there last time */
    uint64_t  idle_cycles;      /* and when, so a turn's length is known */
    /* The bytes the core changed since, with their value there. */
    struct { uint8_t *p; uint8_t was; } idle_log[ASIC_IDLE_LOG];
    int       idle_logged;
    bool      idle_changed;     /* a device, the host, or too many bytes */
    int       idle_sample;      /* steps since the last candidate was taken */
} ChihiroAsic;

/* The registers, each described in the list at the top of this file. */
#define ASIC_MBOX_BASE    0x0FC00000u   /* the shared mailbox, 0x40 bytes */
#define ASIC_HOST_IRQ     0x0FE00000u
#define ASIC_SPD_LINES    0x0FE00020u
#define ASIC_SPD_ENABLE   0x0FE00024u
#define ASIC_STRAPS       0x0FE00060u

/* The security PIC hangs off three lines of a port: 0x00400004 carries the
 * data on bits 0-2 and the clock on bit 3, 0x00400008 says which way they go. */
#define ASIC_PIC_LINES    0x00400004u
#define ASIC_PIC_DIR      0x00400008u

/* The media board flash at 0x00800000: "MBDT" at 0x008FFE00, then the board
 * serial at 0x008FFE10 (flash offsets 0xFFE00, 0xFFE10). */
#define ASIC_FLASH_BASE   0x00800000u
#define ASIC_FLASH_SIZE   0x00200000u

/* The DIMM block table a board type 3 walks: valid when the first word is
 * 0x57377521; from 0x20, one word per pair of banks holds the pair's first
 * 16 KB unit, 0xFFFFFFFF when empty, and the pair runs to `unit`. The init at
 * 0x1256: strap 3 walks four words of up to 0x4000 units (256 MB), others
 * eight of 0x2000 (128 MB). */
#define ASIC_DIMM_BLOCKS  0x0FB78000u
#define ASIC_DIMM_MAGIC   0x57377521u

#define ASIC_MAILBOX_VECTOR 0xB0u
/* The RTOS clock: vector 0x90, the one maskable source the init unmasks
 * (`and 0xFFFFFC05` to 0xFFFFF100) with a real handler (0xB314, then 0xB350
 * wakes the tasks whose delay ran out). HEURISTIC: one tick a millisecond. */
#define ASIC_TICK_VECTOR 0x90u

static ChihiroAsic asic;

/* ------------------------------------------------------------------ memory */

/* The core changes a byte: its value at the idle candidate point is kept,
 * once, so the point can be compared whole (see asic_idle_turn). */
static void asic_store(uint8_t *p, uint8_t v)
{
    int i = 0;

    if (*p == v) {
        return;
    }
    if (!asic.idle_changed) {
        while (i < asic.idle_logged && asic.idle_log[i].p != p) {
            i++;
        }
        if (i == ASIC_IDLE_LOG) {
            asic.idle_changed = true;
        } else if (i == asic.idle_logged) {
            asic.idle_log[i].p = p;
            asic.idle_log[i].was = *p;
            asic.idle_logged++;
        }
    }
    *p = v;
}

static void asic_store32(uint32_t *w, uint32_t val)
{
    uint8_t b[4];

    memcpy(b, &val, sizeof(b));
    for (int i = 0; i < 4; i++) {
        asic_store((uint8_t *)w + i, b[i]);
    }
}

/* The memory an access of size bytes at addr lies in, whole: the V850's
 * loads and stores need not be aligned, so one may start at an array's last
 * byte. */
static uint8_t *asic_map(uint32_t addr, uint32_t size, uint32_t *off)
{
    if (addr <= ASIC_ROM_SIZE - size) { *off = addr; return asic.rom; }
    if (addr >= ASIC_RAM_BASE && addr - ASIC_RAM_BASE <= ASIC_RAM_SIZE - size) {
        *off = addr - ASIC_RAM_BASE; return asic.ram;
    }
    if (addr >= ASIC_IRAM_BASE &&
        addr - ASIC_IRAM_BASE <= ASIC_IRAM_SIZE - size) {
        *off = addr - ASIC_IRAM_BASE; return asic.iram;
    }
    return NULL;
}

/* Fills the block table for the amount of DIMM the board carries, using the
 * geometry the straps select. */
static void asic_build_dimm_blocks(void)
{
    unsigned unit  = (asic.straps & 7) == 3 ? 0x4000u : 0x2000u;
    unsigned pairs = (asic.straps & 7) == 3 ? 4u : 8u;
    uint64_t per_pair = (uint64_t)unit << 14;   /* bytes one pair covers */
    uint64_t left = (uint64_t)asic.dimm_mb << 20;

    memset(asic.dimm_blocks, 0, sizeof(asic.dimm_blocks));
    asic.dimm_blocks[0] = ASIC_DIMM_MAGIC;
    for (unsigned i = 0; i < pairs && i < 8; i++) {
        if (left >= per_pair) { asic.dimm_blocks[8 + i] = 0; left -= per_pair; }
        else                    asic.dimm_blocks[8 + i] = 0xFFFFFFFFu;
    }
}

static uint32_t asic_read(void *opaque, uint32_t addr, int size)
{
    uint32_t off;
    uint8_t *b = asic_map(addr, size, &off);

    if (b) {
        return ldn_le_p(b + off, size);
    }
    if (addr >= ASIC_PERI_BASE) {
        return asic.peri[(addr - ASIC_PERI_BASE) >> 2];
    }
    if (addr >= ASIC_DIMM_BLOCKS && addr < ASIC_DIMM_BLOCKS + 0x40) {
        return asic.dimm_blocks[(addr - ASIC_DIMM_BLOCKS) >> 2];
    }
    if (addr == ASIC_PIC_LINES) {
        /* A read steps the chip's transfer while it has one to give. */
        ChihiroPic was;
        uint32_t v;

        memcpy(&was, &asic.pic, sizeof(was));
        v = chihiro_pic_read_lines(&asic.pic);
        if (memcmp(&was, &asic.pic, sizeof(was)))
            asic.idle_changed = true;
        return v;
    }
    if (addr >= ASIC_FLASH_BASE && addr < ASIC_FLASH_BASE + ASIC_FLASH_SIZE) {
        uint32_t flash_size = 0;
        const uint8_t *flash = asic.flash;
        flash_size = asic.flash_size;
        uint32_t o = addr - ASIC_FLASH_BASE;
        if (!flash || o + (uint32_t)size > flash_size) return 0;
        return ldn_le_p(flash + o, size);
    }
    switch (addr) {
    case ASIC_HOST_IRQ:  return asic.host_irq;
    case ASIC_SPD_LINES: return chihiro_spd_read_lines(&asic.spd);
    case ASIC_STRAPS:    return asic.straps;
    default:
        if (addr >= ASIC_MBOX_BASE && addr < ASIC_MBOX_BASE + 0x40) {
            return asic.mbox[(addr - ASIC_MBOX_BASE) >> 2];
        }
        /* Everything else reads as zero. */
        return 0;
    }
}

static void asic_write(void *opaque, uint32_t addr, int size, uint32_t val)
{
    uint32_t off;
    uint8_t *b = asic_map(addr, size, &off);

    if (b) {
        if (b == asic.rom) return;              /* the image is read-only */
        for (int i = 0; i < size; i++) {
            asic_store(&b[off + i], (uint8_t)(val >> (8 * i)));
        }
        return;
    }
    if (addr >= ASIC_PERI_BASE) {
        asic_store32(&asic.peri[(addr - ASIC_PERI_BASE) >> 2], val);
        return;
    }
    if (addr >= ASIC_MBOX_BASE && addr < ASIC_MBOX_BASE + 0x40) {
        asic_store32(&asic.mbox[(addr - ASIC_MBOX_BASE) >> 2], val);
        return;
    }
    if (addr == ASIC_PIC_LINES || addr == ASIC_PIC_DIR) {
        ChihiroPic was;

        memcpy(&was, &asic.pic, sizeof(was));
        if (addr == ASIC_PIC_LINES) chihiro_pic_write_lines(&asic.pic, val);
        else                        chihiro_pic_write_dir(&asic.pic, val);
        if (memcmp(&was, &asic.pic, sizeof(was)))
            asic.idle_changed = true;
        return;
    }
    if (addr == ASIC_SPD_LINES || addr == ASIC_SPD_ENABLE) {
        ChihiroSpd was;

        memcpy(&was, &asic.spd, sizeof(was));
        if (addr == ASIC_SPD_LINES) chihiro_spd_write_lines(&asic.spd, val);
        else                    chihiro_spd_write_enable(&asic.spd, val);
        if (memcmp(&was, &asic.spd, sizeof(was)))
            asic.idle_changed = true;
        return;
    }
    if (addr == ASIC_HOST_IRQ) {
        asic_store32(&asic.host_irq, val);
        if ((val & 4) && !asic.mbox_answered) {
            asic.mbox_answered = true;
            asic.idle_changed = true;
        }
        return;
    }
    /* The rest is accepted and dropped. Each one is listed at the top of this
     * file with what the firmware uses it for. */
}

/* ------------------------------------------------------------------ loading */

/* The image starts with its reset vector, a jr (0x0780); a firmware.asic
 * carries a 16-byte header before it. */
static uint32_t asic_body_offset(const uint8_t *img, uint32_t len)
{
    if (len >= 2 && img[0] == 0x80 && img[1] == 0x07) return 0;
    if (len >= 18 && img[16] == 0x80 && img[17] == 0x07) return 16;
    return UINT32_MAX;
}

/* The upload is DES in ECB, eight-byte blocks read little-endian, with one
 * key for every Sega media board of this generation (it decrypts the Ghost
 * Squad and Ollie King uploads and the copy at flash 0x1E4000): the textbook
 * DES example key of FIPS PUB 81. */
#define ASIC_FIRMWARE_KEY 0x0123456789ABCDEFull

static bool asic_load_firmware(const uint8_t *img, uint32_t len,
                               const char *origin)
{
    uint32_t skip = asic_body_offset(img, len);
    uint8_t *plain = NULL;

    if (skip == UINT32_MAX && len >= 24) {
        plain = malloc(len);
        if (!plain) return false;
        memcpy(plain, img, len);
        chihiro_des_decrypt_buffer(plain, len, ASIC_FIRMWARE_KEY);
        skip = asic_body_offset(plain, len);
        if (skip != UINT32_MAX) {
            img = plain;
        } else {
            free(plain);
            plain = NULL;
        }
    }

    if (skip == UINT32_MAX) {
        fprintf(stderr, "Chihiro ASIC: %s holds no V850 reset vector, and it is "
                "not encrypted with the key this board uses either\n", origin);
        return false;
    }
    uint32_t body = len - skip;
    if (body > ASIC_ROM_SIZE) body = ASIC_ROM_SIZE;

    memset(asic.rom, 0xFF, sizeof(asic.rom));
    memcpy(asic.rom, img + skip, body);
    asic.have_rom = true;
    asic.reported = false;
    fprintf(stderr, "Chihiro ASIC: firmware loaded from %s (%u bytes%s%s)\n",
            origin, body, skip ? ", 16-byte header skipped" : "",
            plain ? ", decrypted" : "");
    free(plain);
    return true;
}

/* ------------------------------------------------------------------ running */

/* Prints the boot banner the firmware leaves in work RAM, found by content. */
static void asic_report_console(void)
{
    uint32_t best_at = 0, best_len = 0, run_at = 0, run_len = 0;

    for (uint32_t i = 0; i < ASIC_RAM_SIZE; i++) {
        uint8_t c = asic.ram[i];
        bool printable = (c >= 0x20 && c < 0x7F) || c == '\n';
        if (printable) {
            if (!run_len) run_at = i;
            run_len++;
            if (run_len > best_len) { best_len = run_len; best_at = run_at; }
        } else {
            run_len = 0;
        }
    }
    if (best_len < 16) {
        fprintf(stderr, "Chihiro ASIC: the V850 left no console text\n");
        return;
    }
    fprintf(stderr, "Chihiro ASIC: V850 console ---------------------------\n");
    fprintf(stderr, "  ");
    for (uint32_t i = 0; i < best_len; i++) {
        uint8_t c = asic.ram[best_at + i];
        if (c == '\n') fprintf(stderr, "\n  ");
        else           fputc(c, stderr);
    }
    fprintf(stderr, "\nChihiro ASIC: -----------------------------------------\n");
}

/* The core at one point: registers, PSW and system registers. */
static void asic_core_regs(uint32_t *out)
{
    const V850State *c = &asic.cpu;
    const uint32_t sys[] = { c->psw, c->eipc, c->eipsw, c->fepc, c->fepsw,
                             c->ecr, c->ctpc, c->ctpsw, c->ctbp };

    memcpy(out, &c->r[1], 31 * sizeof(uint32_t));
    memcpy(out + 31, sys, sizeof(sys));
}

/* The firmware has no idle instruction: its RTOS walks its task list forever.
 * A turn that brings the board back to the same point in the same state --
 * the same registers, every byte the core changed on the way back to its
 * value there, no device stepped and nothing from the host -- is repeated
 * exactly by the next, so such turns are charged at the board's rate and not
 * run. The tick, a host interrupt or an exchange ends the skip. */
static void asic_idle_turn(uint64_t end)
{
    uint32_t regs[ASIC_IDLE_REGS];
    uint64_t turn, span, jump;
    bool same;

    if (asic.cpu.pc != asic.idle_pc) {
        /* Take a new candidate now and then: whichever point the core is at
         * when it goes idle will come round again a turn later. */
        if (++asic.idle_sample >= 4096) {
            asic.idle_sample = 0;
            asic.idle_pc = asic.cpu.pc;
            asic_core_regs(asic.idle_regs);
            asic.idle_cycles = asic.cpu.cycles;
            asic.idle_logged = 0;
            asic.idle_changed = false;
        }
        return;
    }

    asic_core_regs(regs);
    turn = asic.cpu.cycles - asic.idle_cycles;
    same = !asic.idle_changed && turn != 0 && !asic.cpu.irq_pending &&
           !memcmp(regs, asic.idle_regs, sizeof(regs));
    for (int i = 0; same && i < asic.idle_logged; i++) {
        same = *asic.idle_log[i].p == asic.idle_log[i].was;
    }
    asic.idle_logged = 0;
    asic.idle_changed = false;
    if (!same) {
        memcpy(asic.idle_regs, regs, sizeof(regs));
        asic.idle_cycles = asic.cpu.cycles;
        return;
    }

    /* Whole turns only, never past the next tick or what has been paid. */
    if (asic.cpu.cycles >= end) {
        return;
    }
    span = MIN((uint64_t)MAX(asic.clocks_to_tick, 0), end - asic.cpu.cycles);
    jump = (span / turn) * turn;
    if (jump == 0) {
        return;
    }
    asic.cpu.cycles += jump;
    asic.clocks_to_tick -= (int)jump;
    asic.idle_cycles = asic.cpu.cycles;
}

/* Runs the core, feeding it the RTOS clock at the same rate wherever it is
 * called from, and stops early when `until` says so. Returns the steps run. */
static int asic_run(int budget, bool (*until)(void))
{
    int i;

    uint64_t end = asic.cpu.cycles + (uint64_t)budget;

    for (i = 0; asic.cpu.cycles < end; i++) {
        if (asic.clocks_to_tick <= 0) {
            asic.clocks_to_tick += ASIC_CLOCKS_PER_MS;
            v850_raise(&asic.cpu, ASIC_TICK_VECTOR);
        }
        uint64_t before = asic.cpu.cycles;
        if (!v850_step(&asic.cpu)) break;
        asic.clocks_to_tick -= (int)(asic.cpu.cycles - before);
        asic.steps++;
        if (until) {
            /* The cabinet is waiting on this exchange: run it as it comes,
             * with no idle machinery in the way. */
            if (until()) { i++; break; }
        } else {
            asic_idle_turn(end);
        }
    }
    return i;
}

/* The board has posted a message in its window: with bit 31 (Done) the
 * answer to a host command, without it a command of its own, which the
 * acLib runs and replies to (0x0002/0x0003 restart, 0x0004-0x000B host
 * memory access, 0x0302 test report). The host reads it on the interrupt,
 * then clears the window. */
static bool asic_posted(void)
{
    return asic.mbox_answered;
}

/* The firmware clears its command window once it has read a message; the
 * host writes the next one there as soon as it sees it clear. */
static bool asic_taken_or_posted(void)
{
    return asic.mbox[8] == 0 || asic.mbox_answered;
}

/* HEURISTIC: a cabinet's board is up long before the Xbox asks it anything;
 * here it starts when the game releases it, so it is given its boot (about
 * 20 million clocks, 606 ms of board time) in one go, run inline. */
#define ASIC_SELFTEST_BUDGET (ASIC_CLOCKS_PER_MS * 650)

static void asic_run_selftest(void)
{
    uint64_t start = asic.steps, c0 = asic.cpu.cycles;

    if (!asic.have_rom) return;
    asic_run(ASIC_SELFTEST_BUDGET, NULL);
    fprintf(stderr, "Chihiro ASIC: board booted in %llu instructions, "
            "%llu clocks (%.3f per instruction)\n",
            (unsigned long long)(asic.steps - start),
            (unsigned long long)(asic.cpu.cycles - c0),
            (double)(asic.cpu.cycles - c0) / (double)(asic.steps - start));
}

/* One mailbox exchange: the command into 0x0FC00020, vector 0xB0 (the only
 * poster of the input task's semaphore), run until the board posts its next
 * message at 0x0FC00000. The ceiling, in clocks, keeps a silent firmware
 * from hanging. */
#define ASIC_EXCHANGE_BUDGET 1000000

void chihiro_asic_host_mailbox_write(uint32_t offset, uint32_t val)
{
    if (offset >= 0x40) return;
    asic.mbox[offset >> 2] = val;
    asic.idle_changed = true;
}

static bool asic_mailbox_exchange(const uint32_t *cmd8, uint32_t *msg8)
{
    if (!asic.running || !asic.have_rom) return false;

    /* Already written word by word; copied again for any other path. */
    memcpy(&asic.mbox[8], cmd8, 32);
    asic.host_irq = 0;
    v850_raise(&asic.cpu, ASIC_MAILBOX_VECTOR);

    /* A command runs until the board posts its next message; a reply to one
     * of the board's own commands (bit 31), which has no answer, until the
     * board has taken it. */
    if (!asic.mbox_answered) {
        asic_run(ASIC_EXCHANGE_BUDGET, (cmd8[0] & 0x80000000u) ?
                 asic_taken_or_posted : asic_posted);
    }
    /* Nothing yet, or an earlier message still on its way to the host: the
     * board thread hands this one over in turn. */
    if (!asic.mbox_answered) return false;
    memcpy(msg8, &asic.mbox[0], 32);
    asic.mbox_answered = false;     /* taken */
    return true;
}


bool chihiro_asic_mailbox_exchange(const uint32_t *cmd8, uint32_t *msg8)
{
    return asic_mailbox_exchange(cmd8, msg8);
}

bool chihiro_asic_tick(uint32_t *msg8)
{
    if (asic.running && !asic.mbox_answered) {
        asic_run(ASIC_CLOCKS_PER_MS, asic_posted);
        if (asic.cpu.illegal) {
            nc_log("xbased-chihiro-asic", "V850 stopped at %08x", asic.cpu.illegal_pc);
            asic.running = false;
        }
        if (!asic.reported && asic.steps > 5000000) {
            asic.reported = true;
            if (nb_trace_on()) asic_report_console();
        }
    }
    if (!asic.mbox_answered) return false;
    memcpy(msg8, asic.mbox, 32);
    asic.mbox_answered = false;
    return true;
}

static void asic_set_running(bool running)
{
    if (running == asic.released) return;
    asic.released = running;

    if (running) {
        if (!asic.have_rom) {
            fprintf(stderr, "Chihiro ASIC: CPU released with no firmware "
                    "loaded — nothing will run\n");
            return;
        }
        memset(asic.ram, 0, sizeof(asic.ram));
        memset(asic.iram, 0, sizeof(asic.iram));
        memset(asic.peri, 0, sizeof(asic.peri));
        v850_reset(&asic.cpu);          /* pc 0, the reset vector */
        asic.steps = 0;
        asic.reported = false;
        memset(asic.mbox, 0, sizeof(asic.mbox));
        asic.mbox_answered = false;
        asic.host_irq = 0;
        asic.clocks_to_tick = ASIC_CLOCKS_PER_MS;
        chihiro_spd_init(&asic.spd, asic.dimm_mb, 0);
        chihiro_pic_reset(&asic.pic);
        asic_build_dimm_blocks();
        fprintf(stderr, "Chihiro ASIC: V850 released from reset\n");
        asic_run_selftest();
    } else {
        fprintf(stderr, "Chihiro ASIC: V850 held in reset after %llu "
                "instructions\n", (unsigned long long)asic.steps);
    }
    asic.running = running;
}


void chihiro_asic_set_running(bool running)
{
    asic_set_running(running);
}

bool chihiro_asic_load_upload(const uint8_t *image, uint32_t size)
{
    return asic_load_firmware(image, size, "the host upload");
}

bool chihiro_asic_init(const char *path, unsigned dimm_factor)
{
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    uint8_t *flash = malloc(ASIC_FLASH_SIZE);
    if (!flash) {
        fclose(file);
        return false;
    }
    const size_t size = fread(flash, 1, ASIC_FLASH_SIZE, file);
    fclose(file);
    if (size != ASIC_FLASH_SIZE) {
        free(flash);
        return false;
    }

    free(asic.flash);
    memset(&asic, 0, sizeof(asic));
    asic.flash = flash;
    asic.flash_size = (uint32_t)size;
    asic.straps = 3;
    asic.dimm_mb = 128u << dimm_factor;
    v850_init(&asic.cpu, &asic, asic_read, asic_write);
    asic.cpu.code = asic.rom;
    asic.cpu.code_size = ASIC_ROM_SIZE;

    const uint32_t offset = 0x1e4000;
    if (!asic_load_firmware(flash + offset, (uint32_t)size - offset, path)) {
        free(asic.flash);
        asic.flash = NULL;
        asic.flash_size = 0;
        return false;
    }
    chihiro_asic_set_running(true);
    return asic.running;
}
