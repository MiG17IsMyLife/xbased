/*
 * Xbox boot media and XBE selection.
 */
#include "native/core/xbox_boot.h"
#include "native/core/mods.h"
#include "native/hle/hle_native.h"
#include "native/state/save.h"

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>

#define XBE_INIT_LIMIT_DEVKIT_MEMORY 4u
#define XBOX_DEVKIT_RAM (128u << 20)

/* An XBE header's certificate: the title ID and version. */
static bool xbe_title(const uint8_t *h, size_t len, uint32_t *tid, uint32_t *ver)
{
    if (len < 0x180 || memcmp(h, "XBEH", 4)) {
        return false;
    }
    const uint32_t cert = nc_rd32(h + 0x118) - nc_rd32(h + 0x104);
    if (cert > len - 0xB0) {
        return false;
    }
    *tid = nc_rd32(h + cert + 0x08);
    *ver = nc_rd32(h + cert + 0xAC);
    return true;
}

/* An Xbox Live title update of a disc XBE (path "\default.xbe" or
 * "\Device\CdRom0\default.xbe"): as the Xbox kernel does, the file of that
 * name in E:\TDATA\<title ID>\$u boots instead when it is the same title's
 * and no older (the title's updater installs it there, then relaunches the
 * disc). Its path in out; false when there is none. */
static bool title_update(const NcDisc *disc, const char *path, char *out, size_t n)
{
    static const char cd[] = "\\Device\\CdRom0\\";
    const char *rel = !strncasecmp(path, cd, sizeof(cd) - 1)                ? path + sizeof(cd) - 1
                      : path[0] == '\\' && strncasecmp(path, "\\Device\\", 8) ? path + 1
                                                                            : NULL;
    NcDiscFile f;
    uint8_t h[0x1000], u[0x1000];
    uint32_t tid, ver, utid, uver;
    if (!rel || strchr(rel, '\\') || !nc_disc_find(disc, rel, &f) || f.size < sizeof(h) ||
        !nc_disc_read(disc, &f, 0, h, sizeof(h)) || !xbe_title(h, sizeof(h), &tid, &ver)) {
        return false;
    }
    char host[512];
    snprintf(out, n, "\\Device\\Harddisk0\\Partition1\\TDATA\\%08x\\$u\\%s", tid, rel);
    if (!nc_io_host_file(out, host, sizeof(host))) {
        return false;
    }
    FILE *fp = fopen(host, "rb");
    const bool read = fp && fread(u, 1, sizeof(u), fp) == sizeof(u);
    if (fp) {
        fclose(fp);
    }
    if (!read || !xbe_title(u, sizeof(u), &utid, &uver) || utid != tid || uver < ver) {
        nc_log("xbased-native-frontier", "title update %s: not this title's newer version (title %08x version %x, "
               "the disc's %08x %x): the disc's boots", out, read ? utid : 0, read ? uver : 0, tid, ver);
        return false;
    }
    nc_log("xbased-native-frontier", "title update: %s (version %x, the disc's %x) boots for %s", out, uver, ver, path);
    return true;
}

static bool read_disc_xbe_header(int disc_fd, uint8_t *header, size_t size)
{
    const int fd = dup(disc_fd);
    if (fd < 0) {
        return false;
    }
    NcDisc disc;
    if (!nc_disc_open(&disc, fd)) {
        close(fd);
        return false;
    }
    NcDiscFile file;
    const bool read = nc_disc_find(&disc, "default.xbe", &file) && nc_disc_read(&disc, &file, 0, header, size);
    nc_disc_close(&disc);
    return read;
}

uint32_t nc_xbox_boot_ram(int disc_fd, const NcBootOptions *boot)
{
    /* 128 MiB, a debug kit's (the option ram=128, NC_RAM_MB=128): for a title
     * built without XINIT_LIMIT_DEVKIT_MEMORY (the XBE's init flag 4), as a
     * debug kit gives it; betas and debug builds use the extra memory. A
     * retail title marked for 64 MiB gets 64 there too: given 128, Halo's
     * D3D wrote push buffer jumps the GPU can't take and drew nothing.
     * An XBE signed with the debug keys runs on a debug kit only, so it gets
     * a debug kit's memory whatever the option says (unless marked for 64):
     * StarCraft Ghost's v1.256 build (flags 1) ran out of 64 MiB loading its
     * first mission ("out of physical memory", then a write to 0). */
    const char *mb = getenv("NC_RAM_MB");
    const uint32_t ram = mb && atoi(mb) == 128 ? XBOX_DEVKIT_RAM : NC_RAM_RETAIL;
    uint8_t header[0x12C];
    if (disc_fd < 0 || boot->dashboard || !read_disc_xbe_header(disc_fd, header, sizeof(header)) ||
        nc_rd32(header) != HLE_XBE_MAGIC) {
        return ram;
    }
    if (nc_rd32(header + 0x124) & XBE_INIT_LIMIT_DEVKIT_MEMORY) {
        if (ram == XBOX_DEVKIT_RAM) {
            nc_log("xbased-native", "memory: the title is marked for 64 MiB (as a debug kit runs it): 64 MiB");
        }
        return NC_RAM_RETAIL;
    }
    const uint32_t base = nc_rd32(header + 0x104), size = nc_rd32(header + 0x10C);
    const uint32_t encoded_entry = nc_rd32(header + 0x128);
    const uint32_t debug_entry = encoded_entry ^ HLE_XBE_ENTRY_KEY_DEBUG;
    const uint32_t retail_entry = encoded_entry ^ HLE_XBE_ENTRY_KEY_RETAIL;
    if (ram == NC_RAM_RETAIL && debug_entry >= base && debug_entry - base < size && retail_entry - base >= size) {
        nc_log("xbased-native", "memory: a debug kit's XBE (the debug keys), not marked for 64 MiB: 128 MiB");
        return XBOX_DEVKIT_RAM;
    }
    return ram;
}

bool nc_xbox_boot_xbe(NcCore *c, int disc_fd, const NcBootOptions *boot)
{
    /* The dashboard boots with no disc (the tray empty); a title from the
     * disc, or the dashboard relaunched from one, has it. */
    if (disc_fd >= 0) {
        if (!nc_disc_open(&c->disc, disc_fd)) {
            close(disc_fd);
            c->disc.fd = -1;
            nc_hle_set_fatal(NC_FATAL_BOOT, "The game's file isn't an Xbox disc image.");
            return false;
        }
        nc_mods_install(&c->disc);      /* the mods' files and XBE patches, the cheats */
    } else if (!boot->dashboard && !boot->loadstate[0]) {
        nc_hle_set_fatal(NC_FATAL_BOOT, "The game's file can't be opened.");
        return false;
    }
    /* A relaunch (XLaunchNewImage, NativeCoreActivity starts the core again):
     * the XBE the saved launch data page names, and the page for the title. */
    uint8_t page[NC_LAUNCH_PAGE];
    bool have_page = false;
    char xbe_path[NC_LAUNCH_PAGE] = "\\default.xbe";
    if (boot->dashboard) {
        snprintf(xbe_path, sizeof(xbe_path), "%s", NC_DASHBOARD_PATH);
        snprintf(c->kernel.image_path, sizeof(c->kernel.image_path), "%s", NC_DASHBOARD_PATH);
    }
    if (boot->loadstate[0]) {
        /* a savestate: the XBE it was running (a relaunched title's own, the
         * dashboard's) */
        NcStateInfo info;
        static const char cd[] = "\\Device\\CdRom0";
        static const char hd[] = "\\Device\\Harddisk0\\";
        if (!nc_state_read_info(boot->loadstate, &info)) {
            nc_hle_set_fatal(NC_FATAL_STATE, "the savestate can't be read");
            return false;
        }
        if ((!strncasecmp(info.xbe_path, cd, sizeof(cd) - 1) && info.xbe_path[sizeof(cd) - 1] == '\\') ||
            !strncasecmp(info.xbe_path, hd, sizeof(hd) - 1)) {
            snprintf(xbe_path, sizeof(xbe_path), "%s", info.xbe_path);
            snprintf(c->kernel.image_path, sizeof(c->kernel.image_path), "%s", info.xbe_path);
        }
        nc_log("xbased-native-frontier", "savestate: booting %s for %s", xbe_path, boot->loadstate);
    } else if (boot->relaunch) {
        char file[512];
        nc_kernel_relaunch_file(file, sizeof(file));
        FILE *fp = fopen(file, "rb");
        if (fp) {
            have_page = fread(page, 1, NC_LAUNCH_PAGE, fp) == NC_LAUNCH_PAGE;
            fclose(fp);
            unlink(file);
        }
        if (have_page && nc_kernel_launch_xbe(page, xbe_path, sizeof(xbe_path))) {
            snprintf(c->kernel.image_path, sizeof(c->kernel.image_path), "%s", xbe_path);
            nc_log("xbased-native-frontier", "relaunch: %s (launch type %u)", c->kernel.image_path,
                   nc_rd32(page));
        } else {
            have_page = false;
            snprintf(xbe_path, sizeof(xbe_path), "%s", boot->dashboard ? NC_DASHBOARD_PATH : "\\default.xbe");
            snprintf(c->kernel.image_path, sizeof(c->kernel.image_path), "%s", boot->dashboard ? NC_DASHBOARD_PATH : "");
            nc_log("xbased-native-frontier", "relaunch: no launch data page, booting %s", xbe_path);
        }
    }
    /* a disc XBE with a title update installed: the update (a savestate's
     * memory is the XBE it ran, booted as it was) */
    char update[NC_LAUNCH_PAGE];
    if (!boot->loadstate[0] && disc_fd >= 0 && title_update(&c->disc, xbe_path, update, sizeof(update))) {
        snprintf(xbe_path, sizeof(xbe_path), "%s", update);
        snprintf(c->kernel.image_path, sizeof(c->kernel.image_path), "%s", update);
    }
    if (!nc_xbe_load(&c->vm, &c->disc, xbe_path, &c->kernel, &c->xbe)) {
        char why[256], host[512];
        if (nc_vm_why) {
            nc_hle_set_fatal(NC_FATAL_BOOT, nc_vm_why);
            return false;  /* the host's memory where the XBE goes */
        }
        if (!strcasecmp(xbe_path, NC_DASHBOARD_PATH) && !nc_io_host_file(xbe_path, host, sizeof(host))) {
            nc_hle_set_fatal(NC_FATAL_BOOT,
                             "The dashboard isn't installed: its files (xboxdash.xbe and its folders) go on "
                             "the hard disk's C: drive.");
            return false;
        }
        const char *name = strrchr(xbe_path, '\\');
        snprintf(why, sizeof(why), "The game's program (%s) can't be loaded: it's damaged, or xbased doesn't support "
                 "it yet.", name ? name + 1 : xbe_path);
        nc_hle_set_fatal(NC_FATAL_BOOT, why);
        return false;
    }
    nc_kernel_title_loaded(&c->kernel, &c->disc, &c->xbe.file, c->xbe.fd, c->xbe.info.base,
                           c->xbe.info.sizeof_image);
    if (have_page) {
        nc_kernel_launch_page_restore(&c->kernel, page);
    }
    return true;
}
