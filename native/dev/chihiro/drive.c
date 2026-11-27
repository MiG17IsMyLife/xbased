/*
 * Chihiro drive board: which game uses which board (drive.h).
 */
#include "drive.h"

#include <string.h>

static const struct {
    char game_id[5];
    const NcChihiroDrive *drive;
} k_games[] = {
    {"SBHQ", &nc_chihiro_drive_v257},   /* Wangan Midnight Maximum Tune */
    {"SBKD", &nc_chihiro_drive_v257},   /* Wangan Midnight Maximum Tune 2 */
};

const NcChihiroDrive *nc_chihiro_drive_for(const char *game_id)
{
    for (unsigned i = 0; game_id && i < sizeof(k_games) / sizeof(*k_games); i++) {
        if (!strncmp(game_id, k_games[i].game_id, 4)) {
            return k_games[i].drive;
        }
    }
    return NULL;
}
