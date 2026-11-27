/*
 * Chihiro save data folders (storage.h).
 */
#include "native/dev/chihiro/storage.h"

#include <stdio.h>
#include <string.h>

/* Games that can take over an earlier game's cards. */
static const struct {
    char game_id[5];
    char previous[5];
    char label[8];          /* how the earlier game's cards are shown */
} k_transfers[] = {
    {"SBKD", "SBHQ", "WMMT1"},  /* Maximum Tune 2 takes Maximum Tune 1's cards */
};

/* "<game ID>-<region>", or just the game ID when the region is unknown. */
static void game_key(char *out, size_t cap, const char *game_id, const char *region)
{
    if (region && *region) {
        snprintf(out, cap, "%.4s-%s", game_id, region);
    } else {
        snprintf(out, cap, "%.4s", game_id);
    }
}

void nc_chihiro_storage(const char *data, const char *game_id, const char *region, unsigned build,
                        NcChihiroStorage *s)
{
    memset(s, 0, sizeof(*s));
    char key[16];
    game_key(key, sizeof(key), game_id, region);
    snprintf(s->folder, sizeof(s->folder), "%s/%s", data, key);
    snprintf(s->cards, sizeof(s->cards), "%s/cards", s->folder);
    snprintf(s->unsorted, sizeof(s->unsorted), "%s/cards", data);
    snprintf(s->save, sizeof(s->save), "%s/board-%08X.bin", s->folder, build);
    for (unsigned i = 0; i < sizeof(k_transfers) / sizeof(*k_transfers); i++) {
        if (!strncmp(game_id, k_transfers[i].game_id, 4)) {
            char previous[16];
            game_key(previous, sizeof(previous), k_transfers[i].previous, region);
            snprintf(s->transfer, sizeof(s->transfer), "%s/%s/cards", data, previous);
            snprintf(s->transfer_label, sizeof(s->transfer_label), "%s", k_transfers[i].label);
        }
    }
}
