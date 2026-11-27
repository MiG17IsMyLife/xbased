/*
 * Chihiro game launch: reads boot.id, the file at the root of a Chihiro
 * game that names its executables and its game ID.
 *
 * Chihiro games are run from an extracted game folder (the files of the
 * media board's image), not from a disc image. The folder holds boot.id
 * and the game's XBE files.
 */
#ifndef NATIVE_CORE_CHIHIRO_BOOT_H
#define NATIVE_CORE_CHIHIRO_BOOT_H

#include <stdbool.h>
#include <stddef.h>

#define NC_CHIHIRO_BOOT_MAGIC "BTID"     /* boot.id's first 4 bytes */
#define NC_CHIHIRO_RAM (128u << 20)        /* a Chihiro has 128 MB of RAM */
/* The kernel device path the game's files are mounted at. */
#define NC_CHIHIRO_MBFS "\\Device\\MediaBoard\\Partition0"

typedef struct NcChihiroBoot {
    const char *title;        /* the game's name */
    char executable[32];      /* the XBE to start, relative to the game folder */
    char game_executable[32]; /* boot.id names two XBEs: the game ... */
    char test_executable[32]; /* ... and its test menu ("" if none) */
    char game_id[5];          /* 4-letter game code, as "SBKD" */
    char boot_path[128];      /* an XBE to boot first instead (SEGABOOT), "" for none */
} NcChihiroBoot;

/* Finds the boot.id for path, which may be the game folder, the boot.id
 * itself, or one of the XBEs it names. Writes its path to out. test (may be
 * NULL) is set when path was the test menu's XBE. False if none is found. */
bool nc_chihiro_boot_path(const char *path, char *out, size_t cap, bool *test);

/* For path (as above): the game XBE's path, the game folder (root) and the
 * game ID. Opening the test XBE uses the game's settings, found this way. */
bool nc_chihiro_xbe_path(const char *path, char *out, size_t cap, char *root, size_t root_cap, char game_id[5]);

/* A number that tells builds of a game apart (a hash of the game XBE's
 * first 4 KB). Two versions with the same game ID then keep separate board
 * memory. 0 if the XBE can't be read. */
unsigned nc_chihiro_boot_tag(int boot_fd, const NcChihiroBoot *game);

/* Reads boot.id from fd into game; test picks the test menu's XBE.
 * Returns an error message for a damaged or unsupported boot.id.
 * Returns NULL with game->title NULL when fd isn't a boot.id at all
 * (an Xbox disc, or no disc). */
const char *nc_chihiro_boot_probe(int fd, bool test, NcChihiroBoot *game);

#endif
