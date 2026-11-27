/*
 * Chihiro setup helpers: where the board's firmware files are, the link
 * play cabinet number, and the board serial numbers the game shows.
 */
#ifndef NATIVE_DEV_CHIHIRO_BOARD_SETUP_H
#define NATIVE_DEV_CHIHIRO_BOARD_SETUP_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The link play cabinet number from NC_CHIHIRO_LINK (1-4), or 0 when
 * the cabinet plays alone. */
unsigned nc_chihiro_link_cabinet(void);

/* Path of the network board firmware (dir/ver1305.bin; dir defaults to
 * "chihiro"). True only if the file exists, is readable and big enough. */
bool nc_chihiro_link_firmware(const char *dir, char *path, size_t capacity);

/* Path of the media board's flash dump in dir (fpr21042.bin or
 * fpr21042_m29w160et.bin). It holds SEGABOOT; with it the game boots
 * through SEGABOOT like on real hardware (core.c). False, and path "",
 * when there is none. */
bool nc_chihiro_flash_path(const char *dir, char *path, size_t capacity);

/*
 * A board serial number as the game shows it: the baseboard's "MAIN BD ID"
 * (base true) or the media board's "MEDIA BD ID" (base false).
 *
 * If id (the chihiro.main_id / chihiro.media_id setting) has SEGABOOT's
 * format, like "AAEE-01D44744715", it is used as is. Otherwise a stable
 * one is made up from the Xbox console's 12-digit serial.
 * out receives 16 characters and a terminating zero.
 */
void nc_chihiro_board_id(const char *id, const char console_serial[12], bool base, char out[17]);

#ifdef __cplusplus
}
#endif

#endif
