/*
 * Sega Chihiro arcade hardware: the one header the rest of xbased includes.
 *
 * Each part has its own header in native/dev/chihiro/:
 *   board.h          the baseboard (QC and SC USB devices), start and run
 *   cabinet_input.h  the cabinet's controls
 *   cards.h          card files for the card reader
 *   storage.h        each game's folder for its cards and board memory
 *   board_setup.h    firmware paths, link cabinet number, board serials
 *   media.h          the built-in media board model
 *   lpc.h            the media board's I/O ports and the network board
 */
#ifndef NATIVE_DEV_CHIHIRO_BOARD_H
#define NATIVE_DEV_CHIHIRO_BOARD_H

#include "native/dev/chihiro/board.h"
#include "native/dev/chihiro/board_setup.h"
#include "native/dev/chihiro/cabinet_input.h"
#include "native/dev/chihiro/cards.h"
#include "native/dev/chihiro/lpc.h"
#include "native/dev/chihiro/media.h"
#include "native/dev/chihiro/storage.h"

#endif
