/*
 * Chihiro baseboard: starts the QC and SC devices and runs them each USB
 * frame (board.h). The work itself is done in the other board files; see
 * board_int.h for which does what.
 */
#include "native/dev/chihiro_board.h"
#include "native/core/chihiro_boot.h"

#include "board_int.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The game's drive board, chosen from its game ID (drive.h). */
const NcChihiroDrive *nc_chihiro_drive;

static ChihiroUsbBoard g_qc, g_sc;
static atomic_bool g_active;
static unsigned g_frames;           /* 1 ms frames since start */

/* The JVS board's output pins changed (lamps, the motor relay): the drive
 * board may need to know. */
static void jvs_outputs(void *opaque, const uint8_t outputs[4])
{
    if (nc_chihiro_drive && nc_chihiro_drive->outputs) {
        nc_chihiro_drive->outputs(outputs);
    }
}

void nc_chihiro_board_init(const NcChihiroStorage *storage, const NcChihiroCoinSettings *coins,
                           unsigned cabinet, const char *game_id)
{
    chihiro_cards_init(storage);
    chihiro_input_init(game_id);
    nc_chihiro_lpc_set_type1(nc_chihiro_type1_media(game_id));
    nc_chihiro_drive = nc_chihiro_drive_for(game_id);
    if (nc_chihiro_drive && nc_chihiro_drive->init) {
        nc_chihiro_drive->init();
    }
    nc_log("xbased-chihiro", "drive board: %s", nc_chihiro_drive ? nc_chihiro_drive->name : "none");

    chihiro_usb_init(&g_qc, true);
    chihiro_usb_init(&g_sc, false);
    g_qc.jvs.output = jvs_outputs;

    /* NC_CHIHIRO_REGION: the cabinet's region (1 Japan, 2 USA, 3 Export),
     * stored with its bitwise complement. */
    const char *region = getenv("NC_CHIHIRO_REGION");
    const int r = region ? atoi(region) : 1;
    g_qc.eeprom[0x1f00] = (uint8_t)(r >= 1 && r <= 3 ? r : 1);
    g_qc.eeprom[0x1f01] = (uint8_t)~g_qc.eeprom[0x1f00];
    /* NC_CHIHIRO_MAIN_ID: the baseboard's serial (the game's MAIN BD ID),
     * at the place Sega's QC EEPROM keeps it. */
    const char *main_id = getenv("NC_CHIHIRO_MAIN_ID");
    if (main_id && strlen(main_id) == 16) {
        memcpy(g_qc.eeprom + 0x1f10, main_id, 16);
    }

    chihiro_backup_load(&g_qc, storage->save);
    chihiro_usb_apply_coins(&g_qc, coins);
    if (cabinet) {
        chihiro_backup_link(&g_qc, cabinet);
    }
    g_frames = 0;
    g_active = true;
    nc_log("xbased-chihiro", "baseboard: QC and SC USB protocol emulation");
}

bool nc_chihiro_board_active(void)
{
    return g_active;
}

NcUsbDev *nc_chihiro_board_qc(void)
{
    return g_active ? &g_qc.usb : NULL;
}

NcUsbDev *nc_chihiro_board_sc(void)
{
    return g_active ? &g_sc.usb : NULL;
}

void nc_chihiro_board_flush(void)
{
    if (g_active) {
        chihiro_backup_flush(&g_qc);
    }
}

bool nc_chihiro_board_force(NcChihiroForce *force)
{
    if (!g_active || !nc_chihiro_drive || !nc_chihiro_drive->force) {
        *force = (NcChihiroForce){0};
        return false;
    }
    return nc_chihiro_drive->force(force);
}

void nc_chihiro_board_frame(void)
{
    if (!g_active) {
        return;
    }
    if (chihiro_input_poll(&g_qc.jvs)) {
        chihiro_cards_button();
    }
    /* The drive board works in 16 ms steps. */
    if (++g_frames % 16 == 0) {
        if (nc_chihiro_drive && nc_chihiro_drive->tick) {
            nc_chihiro_drive->tick();
        }
    }
    /* Save the backup memory every 5 seconds if it changed. */
    if (g_frames % 5000 == 0) {
        nc_chihiro_board_flush();
    }
}
