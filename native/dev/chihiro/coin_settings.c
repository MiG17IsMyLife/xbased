/*
 * Chihiro settings EEPROM: the coin settings record (baseboard.h).
 *
 * The QC's 256-byte settings EEPROM holds two copies of a 64-byte
 * "ACBU0001" record (Sega's arcade library keeps the operator's coin
 * settings there). Layout of one record:
 *
 *   0x00  "ACBU0001"
 *   0x08  u16  length of the data after the header, in 4-byte words
 *   0x0a  u16  sum of bytes 0x0c-0x3f
 *   0x0c  data; the coin settings start at 0x1c (see COIN_* below)
 */
#include "baseboard.h"

#include <string.h>

enum {
    ACBU_SLOT_SIZE = 64,
    ACBU_HEADER_SIZE = 12,
    ACBU_COIN_OFFSET = 0x1c,
    /* offsets within the coin settings */
    COIN_RATE = 6,                  /* the rate as the test menu numbers it */
    COIN_FREE_PLAY = 7,
    COIN_CREDITS = 8,               /* credits per coin */
    COIN_SECOND_CHUTE_CREDITS = 9,
    COIN_BONUS = 10,
    COIN_DIVISOR = 11,              /* coins per credit */
    COIN_COST = 12,                 /* NC_CH_COST_COUNT prices follow */
};

static uint16_t acbu_checksum(const uint8_t *slot)
{
    uint16_t sum = 0;
    for (unsigned i = ACBU_HEADER_SIZE; i < ACBU_SLOT_SIZE; i++) {
        sum += slot[i];
    }
    return sum;
}

static bool acbu_valid(const uint8_t *slot)
{
    return !memcmp(slot, "ACBU0001", 8) && nc_rd16(slot + 8) == (ACBU_SLOT_SIZE - ACBU_HEADER_SIZE) / 4 &&
           nc_rd16(slot + 10) == acbu_checksum(slot);
}

/* A default record for a board with no saved settings: free play (rate
 * 10), every price 1 credit. The same record goes in both copies. */
void chihiro_coin_settings_init(ChihiroUsbBoard *b)
{
    static const uint8_t coin_settings[] = {
        99, 9, 4, 0, 0, 0, 10, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    };
    uint8_t *s = b->settings;
    memcpy(s, "ACBU0001", 8);
    nc_wr16(s + 8, (ACBU_SLOT_SIZE - ACBU_HEADER_SIZE) / 4);
    s[ACBU_HEADER_SIZE] = s[ACBU_HEADER_SIZE + 1] = 1;
    memcpy(s + ACBU_COIN_OFFSET, coin_settings, sizeof(coin_settings));
    nc_wr16(s + 10, acbu_checksum(s));
    memcpy(s + ACBU_SLOT_SIZE, s, ACBU_SLOT_SIZE);
}

/* Writes the user's coin settings into both copies of the record, if the
 * record is valid. Marks the backup dirty when something changed. */
void chihiro_usb_apply_coins(ChihiroUsbBoard *b, const NcChihiroCoinSettings *coins)
{
    for (unsigned offset = 0; offset < 2 * ACBU_SLOT_SIZE; offset += ACBU_SLOT_SIZE) {
        uint8_t *slot = b->settings + offset;
        if (!acbu_valid(slot)) {
            continue;
        }
        uint8_t *stored = slot + ACBU_COIN_OFFSET;
        uint8_t next[COIN_COST + NC_CH_COST_COUNT];
        memcpy(next, stored, sizeof(next));
        const unsigned rate = coins->rate;
        if (rate >= 1 && rate <= 10) {
            /* The game checks that the rate and the fields it implies agree,
             * so set them all:
             *   1-5   that many coins for 1 credit
             *   6-9   1 coin for 2-5 credits
             *   10    free play */
            next[COIN_RATE] = rate;
            next[COIN_FREE_PLAY] = rate == 10;
            next[COIN_CREDITS] = rate >= 6 && rate <= 9 ? rate - 4 : 1;
            next[COIN_SECOND_CHUTE_CREDITS] = 1;
            next[COIN_BONUS] = 0;
            next[COIN_DIVISOR] = rate <= 5 ? rate : 1;
        }
        for (int i = 0; i < NC_CH_COST_COUNT; i++) {
            if (coins->cost[i] >= 1 && coins->cost[i] <= 9) {
                next[COIN_COST + i] = coins->cost[i];
            }
        }
        if (memcmp(next, stored, sizeof(next))) {
            memcpy(stored, next, sizeof(next));
            nc_wr16(slot + 10, acbu_checksum(slot));
            b->backup_dirty = true;
        }
    }
}
