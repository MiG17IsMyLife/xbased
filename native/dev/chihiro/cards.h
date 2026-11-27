/*
 * Chihiro card files: which card the player puts into the card reader.
 *
 * Maximum Tune saves the player's progress on a magnetic card. Each card
 * is one 69-byte file in <cabinet folder>/cards (an empty file is a blank
 * card). New cards are named card001.bin, card002.bin, ...
 *
 * The player picks a card by file name. An empty name means "the most
 * recently changed card in the folder".
 */
#ifndef NATIVE_DEV_CHIHIRO_CARDS_H
#define NATIVE_DEV_CHIHIRO_CARDS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Room for a card file name, including the terminating zero. */
#define NC_CHIHIRO_CARD_NAME_CAP 456

/* Chooses the card to insert next. NULL, "" or an invalid name select the
 * most recent card. */
void nc_chihiro_card_select(const char *name);

/* Chooses the card from a "chihiro.card" setting: "" (most recent),
 * "file:<name>", or a slot number 1-999 (card<NNN>.bin). */
void nc_chihiro_card_load_setting(const char *value);

/* The chosen card's file name ("" for the most recent). */
void nc_chihiro_card_selected(char name[NC_CHIHIRO_CARD_NAME_CAP]);

/* Replaces name with the next (direction > 0) or previous (direction < 0)
 * card file in name order, or the most recent one (direction 0).
 * name becomes "" when there is none. For the UI's card picker. */
void nc_chihiro_card_step(char name[NC_CHIHIRO_CARD_NAME_CAP], int direction);

#ifdef __cplusplus
}
#endif

#endif
