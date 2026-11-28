/*
 * Chihiro drive board: which cabinet uses which board (drive.h).
 */
#include "drive.h"
#include "board.h"
#include "native/core/chihiro_boot.h"

/* Crazy Taxi's wheel has no motor, only a centring spring: the host wheel
 * gets that spring. */
static bool sprung_force(NcChihiroForce *force)
{
    *force = (NcChihiroForce){.target = 0x200, .spring = 0x60, .damping = 0x10};
    return true;
}

static const NcChihiroDrive k_sprung = {
    .name = "none (a sprung wheel)",
    .force = sprung_force,
};

const NcChihiroDrive *nc_chihiro_drive_for(const char *game_id)
{
    if (!nc_chihiro_game_title(game_id)) {
        return NULL;
    }
    switch (nc_chihiro_cabinet(game_id)) {
    case NC_CHIHIRO_MAXIMUM_TUNE:
        return &nc_chihiro_drive_v257;
    case NC_CHIHIRO_OUTRUN:
        return &nc_chihiro_drive_sega_servo;
    case NC_CHIHIRO_CRAZY_TAXI:
        return &k_sprung;
    }
    return NULL;
}
