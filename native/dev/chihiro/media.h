/*
 * Chihiro media board (built-in model): answers the game's DIMM and
 * Net DIMM commands without running the board's own firmware.
 *
 * On real hardware the media board holds the game in DIMM memory and has a
 * small CPU that answers the game's status queries. xbased loads the game
 * itself (direct boot), so this model only has to give the answers a game
 * expects from a board that has finished loading. lpc.c delivers the
 * commands and replies.
 *
 * When the real media board firmware is available, lpc.c runs it instead
 * (asic/chihiro-asic.c) and this model is not used.
 */
#ifndef NATIVE_DEV_CHIHIRO_MEDIA_H
#define NATIVE_DEV_CHIHIRO_MEDIA_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sets the media board's serial (the game's MEDIA BD ID), 16 characters. */
void nc_chihiro_media_init(const char serial[16]);

/*
 * One DIMM mailbox command. command and reply are 8 words; the upper 16
 * bits of command[0] say which operation. dimm_factor is the memory size:
 * 0 = 128 MB, 1 = 256 MB, 2 = 512 MB, 3 = 1 GB.
 * False for a command this model doesn't know.
 */
bool nc_chihiro_media_exchange(unsigned dimm_factor, const uint32_t command[8], uint32_t reply[8]);

/* One Net DIMM (network side) command. delay_ms receives how long the
 * real board would take to answer. False for an unknown command. */
bool nc_chihiro_net_exchange(unsigned dimm_factor, const uint32_t command[8], uint32_t reply[8],
                             uint32_t *delay_ms);

/* Reads or writes the 1 MB memory window the game shares with the Net
 * DIMM. False if off/len fall outside it. */
bool nc_chihiro_net_window(uint32_t off, void *buf, uint32_t len, bool write);

#ifdef __cplusplus
}
#endif

#endif
