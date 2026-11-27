/*
 * How the parts of the Chihiro baseboard call each other (internal).
 *
 *   board.c          owns the QC and SC devices, starts and runs them
 *   cards.c          card files <-> the card reader
 *   backup.c         settings EEPROM and backup SRAM <-> the save file
 *   cabinet_input.c  host controls -> the JVS I/O board
 *   board_setup.c    file paths and board serials (public only)
 */
#ifndef NATIVE_DEV_CHIHIRO_BOARD_INT_H
#define NATIVE_DEV_CHIHIRO_BOARD_INT_H

#include "baseboard.h"
#include "native/dev/chihiro/storage.h"

/* cards.c: takes the game's card folders and resets the reader. */
void chihiro_cards_init(const NcChihiroStorage *storage);

/* cards.c: the player pressed the card button. Inserts the chosen card,
 * or takes back one still sticking out of the slot. */
void chihiro_cards_button(void);

/* backup.c: loads the settings and SRAM from save (NULL or "": no file). */
void chihiro_backup_load(ChihiroUsbBoard *qc, const char *save);

/* backup.c: link play. Writes the cabinet number (1-4) into the game's
 * backup record now and whenever the game writes the SRAM. */
void chihiro_backup_link(ChihiroUsbBoard *qc, unsigned cabinet);

/* backup.c: saves the settings and SRAM if they changed. */
void chihiro_backup_flush(ChihiroUsbBoard *qc);

/* cabinet_input.c: picks how the cabinet wires its controls for a game
 * ID from boot.id ("SGBZ"); Maximum Tune's when it doesn't know it. */
void chihiro_input_init(const char *game_id);

/* cabinet_input.c: puts the latest controls onto the JVS board. Returns
 * true on the frame the card button goes down. */
bool chihiro_input_poll(NcJvs *jvs);

#endif
