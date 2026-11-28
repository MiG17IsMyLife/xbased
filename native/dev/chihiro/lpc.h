/*
 * Chihiro media board's I/O ports (0x4000-0x40FF on the LPC bus).
 *
 * This is how a game talks to the media board after SEGABOOT has started
 * it: a small register window, a mailbox for commands and replies, and
 * interrupt line IRQ 10 that tells the game a reply is ready.
 *
 * For link play the network board's real firmware can be run as well
 * (net/chihiro-netboard.c); without it a built-in model answers (media.c).
 */
#ifndef NATIVE_DEV_CHIHIRO_LPC_H
#define NATIVE_DEV_CHIHIRO_LPC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Clears the ports' state. dimm_factor is the DIMM size (see media.h).
 * If NC_CHIHIRO_ASIC names a media board flash dump, its firmware runs. */
void nc_chihiro_lpc_reset(unsigned dimm_factor);

/* type1: show the game a Type-1 media board (FPGA) instead of a Type-3
 * (ASIC); see nc_chihiro_type1_media. Kept across nc_chihiro_lpc_reset. */
void nc_chihiro_lpc_set_type1(bool type1);

/* The Type-1 board's mailbox in mbcom: (off is the byte offset there):
 * the reply sector at 0x900000, the command sector at 0x900200. A command
 * written is answered at once and IRQ 10 raised 2 ms later. False when
 * off/len aren't the mailbox or the board is a Type-3. */
bool nc_chihiro_lpc_type1_mailbox(uint64_t off, void *buf, uint32_t len, bool write);

/* A port read or write by the game. Always handled (returns true). */
bool nc_chihiro_lpc_in(uint16_t port, unsigned bytes, uint32_t *value);
bool nc_chihiro_lpc_out(uint16_t port, unsigned bytes, uint32_t value);

/* Link play: loads the network board firmware (ver1305.bin) and starts
 * it as cabinet 1-4 at IP 10.0.0.<cabinet>. False if it can't. */
bool nc_chihiro_lpc_load_network(const char *firmware, const char serial[16], unsigned cabinet);

/* Connects the loaded network board to the emulator's network. */
void nc_chihiro_lpc_attach_network(void);

/* True while the network board firmware is running. */
bool nc_chihiro_lpc_link_active(void);

/* Call every 1 ms: raises IRQ 10 when a reply is due. */
void nc_chihiro_lpc_frame(void);

#ifdef __cplusplus
}
#endif

#endif
