/*
 * xbased native core — boot and run a title.
 */
#include "native/core/core.h"
#include "hw/xbox/hle/pb/pbmode.h"

void (*nc_crash_hook)(void);
#include "native/dev/dev.h"
#include "native/dev/usb.h"
#include "native/hle/hle_native.h"
#include "native/core/chihiro_boot.h"
#include "native/dev/chihiro_board.h"
#include "native/core/mmio.h"
#include "native/core/mods.h"
#include "native/core/xbox_boot.h"
#include "native/core/xcheat.h"
#include "native/net/net.h"
#include "native/state/save.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <unistd.h>

#define MAIN_STACK_SIZE (256u * 1024u)

static bool core_hook_query(void *opaque, uint32_t eip);
static void core_code_page(void *opaque, uint32_t va);

/* The title can't start: why, for the activity's dialog (NC_FATAL_BOOT). */
static bool boot_failed(const char *why)
{
    nc_hle_set_fatal(NC_FATAL_BOOT, why);
    return false;
}

static void init_xbox_devices(NcVm *vm, bool network_enabled)
{
    nc_dev_nv2a_init();
    nc_dev_nv2a_set_ram(vm->ram_size);
    nc_dev_apu_init(vm);
    nc_dev_xbox_usb_init(vm);
    nc_dev_aci_init(vm);
    nc_net_start(network_enabled);
    nc_dev_nvnet_init(vm);
    /* Register last so the catch-all leaves APU and other devices first. */
    nc_dev_misc_init();
}

/* SEGABOOT, when the media board's flash is there (nc_chihiro_flash_path):
 * its newer build (the flash's second bank, else the first) put on the hard
 * disk and booted as the cabinet does at power on and on every reboot; it
 * boots the game, or its test program. */
static void chihiro_segaboot(const char *dir, NcChihiroBoot *game, const char media_id[17])
{
    char path[1100];
    FILE *f = nc_chihiro_flash_path(dir, path, sizeof(path)) ? fopen(path, "rb") : NULL;
    if (!f) {
        return;
    }
    static uint8_t flash[2u << 20];
    const size_t n = fread(flash, 1, sizeof(flash), f);
    fclose(f);
    const size_t bank = n >= (2u << 20) && !memcmp(flash + (1u << 20), "XBEH", 4) ? (1u << 20) : 0;
    if (memcmp(flash + bank, "XBEH", 4)) {
        nc_log("xbased-native-chihiro", "SEGABOOT: no XBE in the media board's flash");
        return;
    }
    /* the board's serial where its flash keeps it (MBDT), the MEDIA BD ID
     * SEGABOOT hands the game */
    if (n >= 0x100000 && !memcmp(flash + 0xFFE00, "MBDT", 4)) {
        memcpy(flash + 0xFFE10, media_id, 16);
    }
    char host[600];
    snprintf(host, sizeof(host), "%s/Partition1/chihiro-segaboot.xbe", nc_io_hdd_root());
    FILE *o = fopen(host, "wb");
    const bool ok = o && fwrite(flash + bank, 1, 1u << 20, o) == (1u << 20);
    if (o) {
        fclose(o);
    }
    if (!ok) {
        nc_log("xbased-native-chihiro", "SEGABOOT: can't write %s", host);
        return;
    }
    snprintf(game->boot_path, sizeof(game->boot_path), "%s", NC_CHIHIRO_SEGABOOT_XBE);
    nc_io_mount_chihiro_flash(flash, n);
    nc_log("xbased-native-chihiro", "SEGABOOT: the flash's bank %u booted first", bank ? 1u : 0u);
}

/* The board memory's file, a build's own (Ver.A and Ver.B share a game
 * code): the shared file of earlier versions is its start. */
static void chihiro_board_memory(int disc_fd, const NcChihiroBoot *game, char *save, size_t cap)
{
    char shared[600];
    snprintf(shared, sizeof(shared), "%s/chihiro-%s.bin", nc_io_hdd_root(), game->game_id);
    snprintf(save, cap, "%s/chihiro-%s-%08X.bin", nc_io_hdd_root(), game->game_id,
             nc_chihiro_boot_tag(disc_fd, game));
    if (access(save, 0) == 0 || access(shared, 0) != 0) {
        return;
    }
    FILE *from = fopen(shared, "rb"), *to = fopen(save, "wb");
    char buf[4096];
    for (size_t n; from && to && (n = fread(buf, 1, sizeof(buf), from)) > 0;) {
        fwrite(buf, 1, n, to);
    }
    if (from) {
        fclose(from);
    }
    if (to) {
        fclose(to);
    }
    nc_log("xbased-native-chihiro", "board memory: %s starts from %s", save, shared);
}

bool nc_core_boot(NcCore *c, int disc_fd, const NcBootOptions *boot)
{
    memset(c, 0, sizeof(*c));
    char media_id[17] = "";
    c->disc.fd = -1;
    NcChihiroBoot chihiro;
    const char *probe_error = nc_chihiro_boot_probe(disc_fd, boot->chihiro_test, &chihiro);
    if (probe_error) {
        nc_log("xbased-native-chihiro", "%s", probe_error);
        close(disc_fd);
        return boot_failed(probe_error);
    }
    if (boot->chihiro_test && !chihiro.title) {
        if (disc_fd >= 0) {
            close(disc_fd);
        }
        return boot_failed("chihirotest requires an extracted WMMT1 or WMMT2 game folder.");
    }
    const bool loadstate = boot->loadstate[0] != '\0';
    const char *chihiro_dir = chihiro.title ? getenv("NC_CHIHIRO_DIR") : NULL;
    unsigned cabinet = chihiro.title ? nc_chihiro_link_cabinet() : 0;
    char firmware[1200];
    if (cabinet && !nc_chihiro_link_firmware(chihiro_dir, firmware, sizeof(firmware))) {
        nc_log("xbased-chihiro", "link: ver1305.bin unavailable at %s; playing alone", firmware);
        cabinet = 0;
    }
    if (loadstate && cabinet) {
        if (disc_fd >= 0) {
            close(disc_fd);
        }
        return boot_failed("Save states are unavailable in Chihiro link mode.");
    }
    const uint32_t ram = chihiro.title ? NC_CHIHIRO_RAM : nc_xbox_boot_ram(disc_fd, boot);
    /* a savestate's kernel objects are where its boot made them (KERN v3
     * moved the pool's first range) */
    nc_kernel_pool_for(loadstate ? nc_kern_state_kprocess(boot->loadstate) : 0);
    if (!nc_vm_init(&c->vm, ram) || !nc_kernel_init(&c->kernel, &c->vm)) {
        if (disc_fd >= 0) {
            close(disc_fd);
        }
        return boot_failed(nc_vm_why ? nc_vm_why : "The emulated console couldn't be set up.");
    }
    if (boot->relaunch) {
        /* the last title's kept memory (D3D's saved display), where it was */
        nc_kernel_av_saved_set(nc_mm_keep_restore(&c->kernel));
    }
#if defined(__x86_64__)
    const bool real_hw_speed = chihiro.title && boot->chihiro_real_hw_speed;
#else
    const bool real_hw_speed = false;
#endif
    hle_pb_set_real_hw_speed(real_hw_speed);
    if (real_hw_speed) {
        nc_log("xbased-native-chihiro", "real hardware speed enabled (CPU and GPU)");
    }
    nc_ports_set_chihiro(chihiro.title != NULL);
    if (chihiro.title) {
        nc_kernel_set_av_chihiro();
        /* the game's MAIN BD ID is the baseboard's serial, its MEDIA BD ID
         * the media board's */
        char serial[12], main_id[17];
        nc_ex_console_serial(&c->kernel, serial);
        nc_chihiro_board_id(getenv("NC_CHIHIRO_MAIN_ID"), serial, true, main_id);
        nc_chihiro_board_id(getenv("NC_CHIHIRO_MEDIA_ID"), serial, false, media_id);
        setenv("NC_CHIHIRO_MAIN_ID", main_id, 1);   /* the baseboard's EEPROM (nc_chihiro_board_init) */
        nc_log("xbased-native-chihiro", "MAIN BD ID %s, MEDIA BD ID %s", main_id, media_id);
        nc_chihiro_media_init(media_id);
        if (cabinet) {
            if (!nc_chihiro_lpc_load_network(firmware, media_id, cabinet)) {
                cabinet = 0;
            }
        }
        char save[600];
        chihiro_board_memory(disc_fd, &chihiro, save, sizeof(save));
        /* with SEGABOOT its test menu sets the coins (and the game's test
         * menu its costs), kept in the board's memory: not ours over them */
        NcChihiroCoinSettings coins = boot->chihiro_coins;
        char flash[1100];
        if (nc_chihiro_flash_path(chihiro_dir, flash, sizeof(flash))) {
            memset(&coins, 0, sizeof(coins));
        }
        nc_chihiro_board_init(chihiro_dir && *chihiro_dir ? chihiro_dir : "chihiro", save, &coins, cabinet);
    }
    init_xbox_devices(&c->vm, !chihiro.title || cabinet != 0);
    if (chihiro.title && !boot->chihiro_test) {
        chihiro_segaboot(chihiro_dir, &chihiro, media_id);
    }
    if (chihiro.title) {
        nc_io_mount_chihiro_media(chihiro.game_executable, chihiro.test_executable);
        nc_log("xbased-native-chihiro", "direct boot: %s, %s, 128 MiB", chihiro.title, chihiro.executable);
    }
    if (!nc_xbox_boot_xbe(c, disc_fd, boot, chihiro.title ? &chihiro : NULL)) {
        return false;
    }
    if (cabinet) {
        nc_chihiro_lpc_attach_network();
        if (!strcmp(chihiro.game_id, "SBKD") && !strcmp(chihiro.executable, chihiro.game_executable)) {
            nc_kernel_wmmt2_link_fixup(&c->kernel);
        }
    }
    nc_kernel_title_fixups(&c->kernel, &c->xbe.info);
    {
        /* the titles that need the drives' own speed, by id or by the
         * certificate's title name (another edition, another id) */
        char name[41] = "";
        const uint32_t cert = nc_vm_rd32(&c->vm, c->xbe.info.base + 0x118);
        /* the certificate's title name (40 characters at +0xC) whole inside the headers that were loaded */
        if (cert >= c->xbe.info.base && c->xbe.info.sizeof_headers >= 0xC + 82 &&
            cert - c->xbe.info.base <= c->xbe.info.sizeof_headers - (0xC + 82)) {
            for (unsigned i = 0; i < 40; i++) {
                const uint16_t ch = (uint16_t)(nc_vm_rd32(&c->vm, cert + 0xC + 2 * i) & 0xFFFF);
                if (!ch) {
                    break;
                }
                name[i] = ch < 0x80 ? (char)ch : '?';
            }
        }
        nc_io_title_device_time(c->xbe.info.title_id, name);
        nc_dev_nv2a_title_pace(c->xbe.info.title_id, name);
    }

    NcCpuConfig cfg = {
        .membase = c->vm.base,
        .sysop = nc_kernel_sysop,
        .opaque = &c->kernel,
        .hook_query = core_hook_query,
        .code_page = core_code_page,
        .idt_base = c->kernel.idt_va,
        .idt_limit = 0x7FF,
        .gdt_base = c->kernel.gdt_va,
        .gdt_limit = NC_GDT_LIMIT,
        .real_hw_speed = real_hw_speed,
    };
    c->cpu = nc_cpu_create(&cfg, c->kernel.kpcr_va);
    if (!c->cpu) {
        return boot_failed("The emulated CPU couldn't start.");
    }
    nc_mods_bind(&c->vm, c->cpu, &c->xbe.info);

    uint32_t stack = MAIN_STACK_SIZE;
    if (c->xbe.stack_commit > stack) {
        stack = nc_align_up(c->xbe.stack_commit, NC_PAGE);
    }
    /* a cheat file chosen for this title (core/xcheat.c): run before the entry point */
    const uint32_t start = loadstate ? c->xbe.info.entry
                                    : nc_xcheat_install(&c->kernel, &c->vm, c->xbe.info.entry, c->xbe.info.title_id);
    c->boot_thread = nc_kernel_boot_thread(&c->kernel, c->cpu, start, stack);
    if (!c->boot_thread) {
        return boot_failed("The emulated CPU couldn't start.");
    }
    nc_log("xbased-native-frontier", "stage S1 entry %08x, stack %08x-%08x, fs %08x",
           c->xbe.info.entry, c->boot_thread->stack_limit, c->boot_thread->stack_base, c->kernel.kpcr_va);

    /* The HLE GPU layer: its hooks come from the title's signature scan. */
    c->kernel.hle_hook = nc_hle_hook;
    c->kernel.hle_after_sysop = nc_hle_after_sysop;
    nc_hle_boot(&c->kernel, &c->vm, c->cpu, &c->boot_thread->ctx);
    nc_state_attach(c);
    if (loadstate) {
        const char *err = nc_state_load(boot->loadstate);
        if (err) {
            char why[384];
            snprintf(why, sizeof(why), "The savestate couldn't be loaded: %s.", err);
            nc_hle_set_fatal(NC_FATAL_STATE, why);
            return false;
        }
        nc_kernel_loaded_fixups(&c->kernel);
        nc_mods_after_load(&c->kernel, c->kernel.image_path);
        c->restored_state = true;
    }
    return true;
}

void nc_core_run(NcCore *c)
{
    /* The guest thread's short host waits (a Sleep(0) poll that went on, the
     * idle CPU) end when they should: Linux lets a normal thread's timed
     * wait run 50 us late by default. */
    prctl(PR_SET_TIMERSLACK, 1000UL, 0, 0, 0);
    /* a savestate: the thread it was running, or the scheduler first */
    const NcCpuContext *start = c->restored_state ? nc_kstate_start_context(&c->kernel) : &c->boot_thread->ctx;
    nc_cpu_run(c->cpu, start ? start : &c->boot_thread->ctx);
    nc_dev_ohci_flush_cabinet();
    const NcKernel *k = &c->kernel;
    if (k->stop) {
        nc_log("xbased-native-frontier", "stop: %s", k->stop_reason);
    } else {
        nc_log("xbased-native-frontier", "stop: guest executed HLT");
    }
    unsigned threads = 0;
    for (unsigned i = 0; i < NC_MAX_THREADS; i++) {
        threads += k->threads[i].state != NC_T_FREE;
    }
    const NcKExport *first = nc_kernel_export(k->first_ordinal);
    nc_log("xbased-native-frontier", "summary stage=%s kernel_calls=%llu first_call=%s threads=%u",
           k->first_ordinal ? "S2" : "S1", (unsigned long long)k->calls,
           first ? first->name : "-", threads);
}

/* Hooked guest addresses: the HLE GPU's functions and device-access sites. */
static bool core_hook_query(void *opaque, uint32_t eip)
{
    return nc_x86mmio_is_site(eip) || nc_hle_hook_query(opaque, eip);
}

/* Code a title writes over: translated code has to go when its guest bytes
 * change, which nothing else here notices (the CPU translates once). Tomb
 * Raider Legend ships parts of its .text as NOPs and loads its levels' and
 * objects' code there as it goes, through the 0x80000000 view, over code it
 * ran before: the old translations ran on, and it died a little further in
 * each time (a call with a size for a pointer, a jump to nowhere). A page of
 * the XBE's executable, read-only sections is watched from its first translated block:
 * write-protected as the dirty tracking protects pages (every view), its
 * first store (hle/dirty.c) drops its translations. A page written over again
 * and again (data beside code) stops being watched. */
#define CODE_GIVEN_UP 2u
#define CODE_HITS_MAX 64u
static NcKernel *g_code_k;
static uint32_t *g_code_va;     /* per physical page: its VA | 1 while watched, CODE_GIVEN_UP */
static uint8_t *g_code_hits;

static bool code_exec_va(NcKernel *k, uint32_t va)
{
    const uint32_t base = k->xbe_base;
    if (!base || nc_vm_phys(k->vm, base + 0x120) == UINT32_MAX) {
        return false;
    }
    const uint32_t nsec = nc_vm_rd32(k->vm, base + 0x11C), secs = nc_vm_rd32(k->vm, base + 0x120);
    for (uint32_t i = 0; i < nsec && i < 256; i++) {
        const uint32_t o = secs + i * 0x38;
        if (nc_vm_phys(k->vm, o + 0x0C) == UINT32_MAX) {
            return false;
        }
        const uint32_t flags = nc_vm_rd32(k->vm, o), sva = nc_vm_rd32(k->vm, o + 4), size = nc_vm_rd32(k->vm, o + 8);
        /* (not a writable one: the XDK's libraries keep their data beside
         * their code there, D3D's and DSOUND's pages written all the time) */
        if ((flags & HLE_XBE_SEC_EXECUTABLE) && !(flags & HLE_XBE_SEC_WRITABLE) &&
            (uint64_t)va + NC_PAGE > sva && va < (uint64_t)sva + size) {
            return true;
        }
    }
    return false;
}

static void core_code_page(void *opaque, uint32_t va)
{
    NcKernel *k = opaque;
    const uint32_t pa = nc_vm_phys(k->vm, va);
    if (pa == UINT32_MAX || pa >= k->vm->ram_size) {
        return;
    }
    if (!g_code_va) {
        g_code_va = calloc(k->vm->ram_size / NC_PAGE, sizeof(*g_code_va));
        g_code_hits = calloc(k->vm->ram_size / NC_PAGE, 1);
        if (!g_code_va || !g_code_hits) {
            return;
        }
        g_code_k = k;
    }
    const uint32_t pg = pa / NC_PAGE;
    if (g_code_va[pg] == CODE_GIVEN_UP || !code_exec_va(k, va)) {
        return;
    }
    g_code_va[pg] = (va & ~(NC_PAGE - 1)) | 1u;
    hle_host_mem_test_and_clear(pa & ~(NC_PAGE - 1), NC_PAGE);
}

void nc_core_code_written(uint32_t pa)
{
    if (!g_code_va || !g_code_k || pa >= g_code_k->vm->ram_size) {
        return;
    }
    const uint32_t pg = pa / NC_PAGE, w = g_code_va[pg];
    if (!(w & 1u)) {
        return;
    }
    if (!nc_cpu_on_own_thread(g_code_k->cpu)) {
        static unsigned s_other;        /* (guest stores run on the CPU's thread) */
        if (s_other++ < 4) {
            nc_log("xbased-native", "cpu: code at %08x written from another thread: left as it is", w & ~(NC_PAGE - 1));
        }
        return;
    }
    g_code_va[pg] = 0;
    nc_cpu_invalidate(g_code_k->cpu, w & ~(NC_PAGE - 1), NC_PAGE);
    static unsigned s_logged;
    if (s_logged++ < 8) {
        nc_log("xbased-native", "cpu: code at %08x written over (pa %08x): its translations dropped (first 8 logged)",
               w & ~(NC_PAGE - 1), pa & ~(NC_PAGE - 1));
    }
    if (++g_code_hits[pg] >= CODE_HITS_MAX) {
        g_code_va[pg] = CODE_GIVEN_UP;
        nc_log("xbased-native", "cpu: code page %08x written over %u times: not watched any more",
               w & ~(NC_PAGE - 1), CODE_HITS_MAX);
    }
}

/* A fault in translated code the core can't take as the guest's: where the
 * guest was, and what it was doing, before the process goes down with it.
 * Faults elsewhere go on to ART's handlers unreported (its null checks). */
static bool fault_report(NcCore *c, uintptr_t pc, uint64_t va, const char *what)
{
    /* FEX maps a pc outside its code buffer to the vCPU's current EIP, not
     * 0: every fault in the process took this for the guest's, ART's null
     * checks on the UI thread too (a Galaxy Tab S9, Orta: the dumps, the UI
     * thread with them, then an ANR). Only translated code's. */
    uint32_t eip;
    if (!nc_cpu_sample_pc(c->cpu, pc, &eip) || !eip) {
        return false;
    }
    nc_log("xbased-native-frontier", "host fault: %s at guest %08llx, eip %08x (host pc %p)", what,
           (unsigned long long)va, eip, (void *)pc);
    nc_kernel_dump_recent(-1);
    nc_kernel_dump_threads(&c->kernel, -1);
    return false;
}

/* bumped from the fault handlers of any thread (atomically), read by the stats */
uint64_t g_nc_faults_callret, g_nc_faults_dirty, g_nc_faults_mmio, g_nc_faults_unaligned;

static struct {
    uint32_t eip, n;
} g_mfs[16];

static void mmio_fault_sample(uint32_t eip)
{
    if (__atomic_load_n(&g_nc_faults_mmio, __ATOMIC_RELAXED) & 63) {
        return;
    }
    unsigned k = 0;
    while (k < 16 && g_mfs[k].n && g_mfs[k].eip != eip) {
        k++;
    }
    if (k < 16) {
        g_mfs[k].eip = eip;
        g_mfs[k].n++;
    }
}

unsigned nc_mmio_fault_top(uint32_t *eips, uint32_t *counts, unsigned max)
{
    unsigned n = 0;
    for (; n < max; n++) {
        int best = -1;
        for (unsigned k = 0; k < 16; k++) {
            if (g_mfs[k].n && (best < 0 || g_mfs[k].n > g_mfs[best].n)) {
                best = (int)k;
            }
        }
        if (best < 0) {
            break;
        }
        eips[n] = g_mfs[best].eip;
        counts[n] = g_mfs[best].n * 64;
        g_mfs[best].n = 0;
    }
    memset(g_mfs, 0, sizeof(g_mfs));
    return n;
}

void nc_mmio_fault_sites(char *buf, size_t len)
{
    size_t o = 0;
    buf[0] = 0;
    for (unsigned r = 0; r < 6 && o + 24 < len; r++) {
        int best = -1;
        for (unsigned k = 0; k < 16; k++) {
            if (g_mfs[k].n && (best < 0 || g_mfs[k].n > g_mfs[best].n)) {
                best = (int)k;
            }
        }
        if (best < 0) {
            break;
        }
        o += (size_t)snprintf(buf + o, len - o, " %08x:%u", g_mfs[best].eip, g_mfs[best].n * 64);
        g_mfs[best].n = 0;
    }
    memset(g_mfs, 0, sizeof(g_mfs));
}

static void log_watched_store(NcCore *c, const void *address, void *ucontext, uintptr_t pc)
{
    const uint64_t gva = nc_vm_guest_of(&c->vm, address);
    const uint32_t gpa = gva != UINT64_MAX ? nc_vm_phys(&c->vm, (uint32_t)gva) : UINT32_MAX;
    if (gpa == UINT32_MAX || !nc_hle_watch_pa(gpa)) {
        return;
    }
    nc_hle_watch_store((uint32_t)gva, gpa, nc_cpu_guest_eip_at(c->cpu, pc), 0);
    static unsigned framed;
    NcFrame *wf = framed < 20 ? nc_cpu_fault_frame(c->cpu, ucontext) : NULL;
    if (wf) {
        /* the stack's words that look like the title's code: return addresses */
        framed++;
        char fr[400];
        int n = 0;
        const uint32_t esp = nc_frame_gpr(wf, NC_ESP);
        for (uint32_t i = 0; i < 160 && n < (int)sizeof(fr) - 12; i++) {
            const uint32_t a = esp + 4 * i;
            if (nc_vm_phys(&c->vm, a) == UINT32_MAX) {
                break;
            }
            const uint32_t w = nc_vm_rd32(&c->vm, a);
            if (w >= 0x11000 && w < 0x160000) {
                n += snprintf(fr + n, sizeof(fr) - (size_t)n, " %x:%08x", i * 4, w);
            }
        }
        nc_log("xbased-native-watch", "  esp %08x stack%s", esp, fr);
    }
}

static bool handle_guest_page_fault(NcCore *c, int sig, siginfo_t *info, void *ucontext, uintptr_t pc)
{
    /* A guest page the guest doesn't have: the title's page tables may map
     * it since the last flush (the access runs again), or else from
     * translated code the title's page-fault handler, if it has one, maps it.
     * An access past the window's ends is the guest's at the wrapped address
     * (Forza after a race: [0xFFFFFFC4 + 0x60], a NULL list entry's field):
     * the title's fault when that page is missing, as on the Xbox. */
    uint64_t va = nc_vm_guest_of(&c->vm, info->si_addr);
    const uint64_t wrapped = va == UINT64_MAX ? nc_vm_guest_of_wrapped(&c->vm, info->si_addr) : UINT64_MAX;
    if (wrapped != UINT64_MAX) {
        va = wrapped;
    }
    if (sig != SIGSEGV || va == UINT64_MAX) {
        return fault_report(c, pc, (uint64_t)(uintptr_t)info->si_addr, "outside the guest");
    }
    /* A store to a page the title made read-only (PAGE_READONLY): its fault,
     * as a missing page's below. Rainbow Six: Lockdown commits the pages of
     * its own swapped memory read-only and takes a store's fault as the page
     * written, which it then writes to its swap file before it lets the page
     * go (#275). */
    const bool ro_store = nc_vm_phys(&c->vm, (uint32_t)va) != UINT32_MAX && wrapped == UINT64_MAX &&
                          nc_vm_guest_ro_has(&c->vm, (uint32_t)va);
    if (nc_vm_phys(&c->vm, (uint32_t)va) != UINT32_MAX && !ro_store) {
        return fault_report(c, pc, va, wrapped != UINT64_MAX ? "wrapped, to a mapped page" : "mapped page");
    }
    if (!ro_store && nc_paging_fault(&c->kernel, (uint32_t)va)) {
        /* mapped now; the wrapped access would only fault again */
        return wrapped == UINT64_MAX || fault_report(c, pc, va, "wrapped, to a page the tables map");
    }
    NcFrame *f = nc_cpu_fault_frame(c->cpu, ucontext);
    if (!f) {
        nc_kernel_call_fault(&c->kernel, (uint32_t)va, ro_store || nc_cpu_fault_is_write(ucontext));
        return false;
    }
    if (wrapped != UINT64_MAX) {
        nc_log("xbased-native-frontier", "guest fault at %08x (wrapped address), eip %08x", (uint32_t)va,
               nc_frame_eip(f));
    }
    if (!nc_kernel_page_fault(&c->kernel, f, (uint32_t)va, ro_store || nc_cpu_fault_is_write(ucontext))) {
        /* no page-fault handler of the title's: an access violation, to the
         * thread's exception handlers (a __try around a probe, a title's own
         * crash handler), as the Xbox kernel raises it */
        const bool write = ro_store || nc_cpu_fault_is_write(ucontext);
        const uint32_t at = nc_frame_eip(f);
        if (!nc_seh_fault(&c->kernel, f, NC_STATUS_ACCESS_VIOLATION, at, 2, write, (uint32_t)va)) {
            return fault_report(c, pc, va, "the title doesn't handle it");
        }
        static unsigned told;
        if (told++ < 4) {
            /* the first few with what led to them: a title that goes on may still be lost */
            nc_log("xbased-native-frontier", "guest access violation: %s of %08x at eip %08x, taken by the title's handlers",
                   write ? "write" : "read", (uint32_t)va, at);
            nc_kernel_dump_recent(-1);
        }
    }
    nc_cpu_fault_resume(c->cpu, ucontext);
    return true;
}

bool nc_core_fault(NcCore *c, int sig, siginfo_t *info, void *ucontext)
{
    if (sig == SIGILL) {
        return false;
    }
#if defined(__x86_64__)
    const uintptr_t pc = nc_cpu_context_pc(ucontext);
#else
    const uintptr_t pc = ((ucontext_t *)ucontext)->uc_mcontext.pc;
#endif
    if (sig == SIGFPE) {
        /* A guest DIV/IDIV's divide error (fault_win.c): to the title's
         * handlers, as the Xbox kernel hands it (a divisor of zero, or a
         * quotient that doesn't fit: si_code says which); with none, said
         * where it happened rather than leaving the process gone unlogged. */
        NcFrame *df = nc_cpu_fault_frame(c->cpu, ucontext);
        if (df && (nc_kernel_trap(&c->kernel, df, 0, nc_frame_eip(df)) ||
                   nc_seh_fault(&c->kernel, df, info->si_code == NC_FPE_OVERFLOW ? NC_STATUS_INTEGER_OVERFLOW
                                                                                 : NC_STATUS_INTEGER_DIVIDE_BY_ZERO,
                                nc_frame_eip(df), 0, 0, 0))) {
            nc_cpu_fault_resume(c->cpu, ucontext);
            return true;
        }
        return fault_report(c, pc, 0, "divide error");
    }
    if (sig == SIGBUS && nc_cpu_unaligned_fault(c->cpu, info->si_code, ucontext)) {
        /* Doom 3 keeps a critical section at 0x00BFD88E: RtlEnterCriticalSection's
         * lock inc of its LockCount (0x00BFD89E) took the core down */
        if (__atomic_fetch_add(&g_nc_faults_unaligned, 1, __ATOMIC_RELAXED) < 4) {
            nc_log("xbased-native", "unaligned atomic at guest %08llx, eip %08x",
                   (unsigned long long)nc_vm_guest_of(&c->vm, info->si_addr), nc_cpu_guest_eip_at(c->cpu, pc));
        }
        return true;
    }
    if (nc_cpu_callret_fault(c->cpu, (uintptr_t)info->si_addr, ucontext)) {
        __atomic_fetch_add(&g_nc_faults_callret, 1, __ATOMIC_RELAXED);
        return true;
    }
    log_watched_store(c, info->si_addr, ucontext, pc);
    if (nc_hle_dirty_fault(info, ucontext)) {
        __atomic_fetch_add(&g_nc_faults_dirty, 1, __ATOMIC_RELAXED);
        return true;
    }
    if (nc_mmio_fault(&c->vm, info, ucontext)) {
        __atomic_fetch_add(&g_nc_faults_mmio, 1, __ATOMIC_RELAXED);
        mmio_fault_sample(nc_cpu_guest_eip_at(c->cpu, pc));
        /* From translated code: that instruction is hooked from now on. */
        nc_x86mmio_note(nc_cpu_guest_eip_at(c->cpu, pc));
        return true;
    }
    return handle_guest_page_fault(c, sig, info, ucontext, pc);
}
