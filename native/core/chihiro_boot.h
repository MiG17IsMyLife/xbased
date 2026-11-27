/*
 * Launch information from an extracted WMMT cabinet's boot.id.
 */
#ifndef NATIVE_CORE_CHIHIRO_BOOT_H
#define NATIVE_CORE_CHIHIRO_BOOT_H

#include <stdbool.h>
#include <stddef.h>

#define NC_CHIHIRO_BOOT_MAGIC "BTID"
#define NC_CHIHIRO_RAM (128u << 20)
#define NC_CHIHIRO_MBFS "\\Device\\MediaBoard\\Partition0"

typedef struct NcChihiroBoot {
    const char *title;
    char executable[32];      /* relative to the root of the game media */
    char game_executable[32]; /* boot.id's two: the game's and its test mode's */
    char test_executable[32];
    char game_id[5];          /* boot.id's game code (SBKD) */
    char boot_path[128];      /* the XBE booted first instead (SEGABOOT), "" none */
} NcChihiroBoot;

/* The boot.id of an extracted game, from its folder, boot.id, or an XBE
 * named in it (test: it was the test program's XBE). */
bool nc_chihiro_boot_path(const char *path, char *out, size_t cap, bool *test);

/* The game XBE and ID, also used for settings when opening its test XBE. */
bool nc_chihiro_xbe_path(const char *path, char *out, size_t cap, char *root, size_t root_cap, char game_id[5]);

/* A tag for this build of the game (a hash of its game XBE's headers), so
 * two builds with one game code keep their board memory apart; 0 if unread. */
unsigned nc_chihiro_boot_tag(int boot_fd, const NcChihiroBoot *game);

/* Returns an error for invalid or unsupported boot.id media. For an Xbox
 * disc or an empty tray, returns NULL and leaves title NULL. */
const char *nc_chihiro_boot_probe(int fd, bool test, NcChihiroBoot *game);

#endif
