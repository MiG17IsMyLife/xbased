/*
 * Chihiro save data: where each game keeps its cards and board memory.
 *
 * Every game and region has its own folder in the Chihiro data folder
 * (the chihiro.data setting):
 *
 *   <data>/SBKD-JPN/cards/card001.bin ...   Maximum Tune 2, Japan
 *   <data>/SBKD-JPN/board-1794A1D4.bin      its board memory, one per build
 *   <data>/SBKD-EXP/...                     Maximum Tune 2, export
 *   <data>/SBHQ-JPN/...                     Maximum Tune 1, Japan
 *
 * Board memory (settings EEPROM + backup SRAM) is kept per build because
 * two versions of a game may lay out their backup differently; cards are
 * kept per game and region, so Ver.A's cards work in Ver.B.
 *
 * Two other card folders are also offered to the player:
 *   unsorted  <data>/cards, where all cards were kept before this split.
 *             Its cards show up in every game; one moves into the game's
 *             own folder the first time that game writes it.
 *   transfer  the previous game's cards (Maximum Tune 1's for Maximum
 *             Tune 2, same region), which the newer game can take over.
 *             cards.c inserts a copy, so the original card stays as it is.
 */
#ifndef NATIVE_DEV_CHIHIRO_STORAGE_H
#define NATIVE_DEV_CHIHIRO_STORAGE_H

#ifdef __cplusplus
extern "C" {
#endif

#define NC_CHIHIRO_PATH_CAP 600

typedef struct NcChihiroStorage {
    char folder[NC_CHIHIRO_PATH_CAP];   /* <data>/<game ID>-<region> */
    char cards[NC_CHIHIRO_PATH_CAP];    /* this game's cards */
    char unsorted[NC_CHIHIRO_PATH_CAP]; /* cards from before the split */
    char transfer[NC_CHIHIRO_PATH_CAP]; /* the previous game's cards, "" none */
    char transfer_label[8];             /* how its cards are shown: "WMMT1" */
    char save[NC_CHIHIRO_PATH_CAP];     /* this build's board memory file */
} NcChihiroStorage;

/*
 * Works out the folders for a game. Creates nothing.
 *   data     the Chihiro data folder
 *   game_id  4-letter game code from boot.id ("SBKD")
 *   region   "JPN", "EXP", or "" if unknown (nc_chihiro_boot_region)
 *   build    a number unique to the build (nc_chihiro_boot_tag)
 */
void nc_chihiro_storage(const char *data, const char *game_id, const char *region, unsigned build,
                        NcChihiroStorage *s);

#ifdef __cplusplus
}
#endif

#endif
