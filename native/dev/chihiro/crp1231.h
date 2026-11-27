/*
 * Chihiro card reader: Maximum Tune's Sanwa CRP-1231 card reader-printer,
 * on the SC's first serial port.
 *
 * It reads and writes the card's 69-byte magnetic stripe and prints on
 * the card's face. A card here is a 69-byte file (cards.c picks which).
 * The card can be:
 *   nowhere       no card
 *   in the slot   inserted by the player, not yet drawn in
 *   inside        drawn in; the game can read and write it
 *   handed back   pushed out to the player at the end of a game
 */
#ifndef NATIVE_DEV_CHIHIRO_CRP1231_H
#define NATIVE_DEV_CHIHIRO_CRP1231_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NC_CARD_BYTES 69
#define NC_CARD_PATH_CAP 1024

typedef struct NcCardReader {
    uint8_t in[260];            /* the game's frame, gathered byte by byte */
    int in_len, in_want;
    uint8_t out[128];           /* bytes waiting to go to the game */
    int out_len, out_pos;
    uint8_t last[80];           /* the answer to the last command, sent on ENQ */
    int last_len;
    bool fresh;                 /* the game hasn't asked for that answer yet */
    uint8_t last_cmd, last_param;
    uint8_t card[NC_CARD_BYTES];
    int where;                  /* where the card is (see the list above) */
    bool written;               /* the card has data on it */
    char path[NC_CARD_PATH_CAP]; /* its file, "" for a blank not yet written */
    bool waiting;               /* the game asked to draw in a card, none yet */
    bool cleaning;
    uint64_t clean_start_ns;
    /* Saves the stripe to path's file. For a blank card path is "": the
     * callback picks a new file and writes its name into path. */
    bool (*save_card)(char *path, size_t cap, const uint8_t *data);
    /* Logs an event ("card out", ...). May be NULL. */
    void (*note)(const char *what);
} NcCardReader;

/* Empties the reader. Keeps the save_card and note callbacks. */
void nc_card_reader_init(NcCardReader *r);
/* True when there is no card, so the player can insert one. */
bool nc_card_reader_can_offer(const NcCardReader *r);
/* Puts the card in path ("" for a blank card) into the slot. False if the
 * reader isn't empty or the file can't be read. */
bool nc_card_reader_offer(NcCardReader *r, const char *path);
/* The player pulls back a card still in the slot. False if there is none. */
bool nc_card_reader_take_back(NcCardReader *r);
/* The serial line: rx gets a byte from the game; tx gives the next byte
 * for the game, false when there is none. */
void nc_card_reader_rx(NcCardReader *r, uint8_t byte);
bool nc_card_reader_tx(NcCardReader *r, uint8_t *byte);

#endif
