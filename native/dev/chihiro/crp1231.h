/*
 * xbased native core — Maximum Tune's Sanwa CRP-1231 card reader-printer,
 * on the SC's first UART. A card is a 69-byte file.
 */
#ifndef NATIVE_DEV_CHIHIRO_CRP1231_H
#define NATIVE_DEV_CHIHIRO_CRP1231_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NC_CARD_BYTES 69

typedef struct NcCardReader {
    uint8_t in[260];            /* the host's frame being gathered */
    int in_len, in_want;
    uint8_t out[128];           /* bytes on their way to the host */
    int out_len, out_pos;
    uint8_t last[80];           /* the answer to the last command, for ENQ */
    int last_len;
    bool fresh;                 /* that answer hasn't been asked for yet */
    uint8_t last_cmd, last_param;
    uint8_t card[NC_CARD_BYTES];
    int where;                  /* where the card is (crp1231.c) */
    bool written;               /* the card has data on it */
    char path[512];             /* its file, "" for a blank not yet written */
    bool waiting;               /* a take-in waits for a card */
    bool cleaning;
    uint64_t clean_start_ns;
    /* Persists the stripe and fills in the path when issuing a new card. */
    bool (*save_card)(char *path, size_t cap, const uint8_t *data);
    void (*note)(const char *what);
} NcCardReader;

void nc_card_reader_init(NcCardReader *r);
bool nc_card_reader_can_offer(const NcCardReader *r);
/* Puts the card in path ("" for a blank) into an empty reader. */
bool nc_card_reader_offer(NcCardReader *r, const char *path);
/* Only a card still at the entry slot can be pulled back by the player. */
bool nc_card_reader_take_back(NcCardReader *r);
/* serial line: a byte from the host / the next byte to it */
void nc_card_reader_rx(NcCardReader *r, uint8_t byte);
bool nc_card_reader_tx(NcCardReader *r, uint8_t *byte);

#endif
