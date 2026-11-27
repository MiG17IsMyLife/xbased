/*
 * Chihiro setup helpers (board_setup.h): firmware paths, the link play
 * cabinet number and the board serial numbers.
 */
#include "native/dev/chihiro/board_setup.h"

#include "native/core/nc.h"
#include "net/chihiro-netboard.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The folder the user gave, or "chihiro" (relative to the working folder). */
static const char *folder_or_default(const char *dir)
{
    return dir && *dir ? dir : "chihiro";
}

unsigned nc_chihiro_link_cabinet(void)
{
    const char *link = getenv("NC_CHIHIRO_LINK");
    const int cabinet = link ? atoi(link) : 0;
    return cabinet >= 1 && cabinet <= 4 ? (unsigned)cabinet : 0;
}

bool nc_chihiro_link_firmware(const char *dir, char *path, size_t capacity)
{
    const int n = snprintf(path, capacity, "%s/ver1305.bin", folder_or_default(dir));
    struct stat st;
    return n >= 0 && (size_t)n < capacity && stat(path, &st) == 0 && S_ISREG(st.st_mode) &&
           st.st_size >= CHIHIRO_NETBOARD_MIN_FIRMWARE_BYTES && access(path, R_OK) == 0;
}

bool nc_chihiro_flash_path(const char *dir, char *path, size_t capacity)
{
    static const char *const names[] = {"fpr21042.bin", "fpr21042_m29w160et.bin"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(*names); i++) {
        const int n = snprintf(path, capacity, "%s/%s", folder_or_default(dir), names[i]);
        if (n >= 0 && (size_t)n < capacity && access(path, R_OK) == 0) {
            return true;
        }
    }
    path[0] = 0;
    return false;
}

/*
 * True if id has SEGABOOT's serial format "%%%@-##@########", where
 *   %  a digit or a letter
 *   @  a letter other than I and O
 *   #  a digit
 * and other characters must match exactly. Example: AAEE-01D44744715.
 */
static bool id_form(const char *id)
{
    static const char form[] = "%%%@-##@########";
    if (!id || strlen(id) != sizeof(form) - 1) {
        return false;
    }
    for (int i = 0; form[i]; i++) {
        const char c = id[i];
        const bool digit = c >= '0' && c <= '9', letter = c >= 'A' && c <= 'Z' && c != 'I' && c != 'O';
        const bool ok = form[i] == '#'   ? digit
                        : form[i] == '%' ? digit || letter
                        : form[i] == '@' ? letter
                                         : c == form[i];
        if (!ok) {
            return false;
        }
    }
    return true;
}

void nc_chihiro_board_id(const char *id, const char console_serial[12], bool base, char out[17])
{
    if (id_form(id)) {
        memcpy(out, id, 17);
        return;
    }
    if (id && *id) {
        nc_log("xbased-chihiro", "%s BD ID \"%s\" isn't in SEGABOOT's form: the default", base ? "MAIN" : "MEDIA", id);
    }
    /*
     * Made from the console serial, so it stays the same between runs:
     *   "A" + 2 hex digits + a letter A-F   from a hash of the serial
     *   "-01D"
     *   8 digits of the serial              the first 8 for the baseboard,
     *                                       the last 8 for the media board
     * The two boards use different hash seeds so their IDs differ.
     */
    uint32_t h = base ? 0x811C9DC5u : 0x01000193u;
    for (int i = 0; i < 12; i++) {
        h = (h ^ (uint8_t)console_serial[i]) * 16777619u;
    }
    char digits[9];
    for (int i = 0; i < 8; i++) {
        const char c = console_serial[base ? i : 4 + i];
        digits[i] = c >= '0' && c <= '9' ? c : '0';
    }
    digits[8] = 0;
    snprintf(out, 17, "A%02X%c-01D%s", (unsigned)(h & 0xFF), 'A' + (int)((h >> 8) % 6), digits);
}
