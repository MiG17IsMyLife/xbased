/*
 * Chihiro baseboard: starting it, running it each USB frame, and what the
 * rest of the emulator asks of it.
 *
 * A Sega Chihiro is an Xbox with arcade hardware added. Its "baseboard"
 * puts two USB devices where the Xbox controller ports would be:
 *
 *   QC  talks to the cabinet's JVS I/O board (buttons, pedals, coins) and
 *       holds the board's EEPROM, battery-backed SRAM and clock.
 *   SC  has two serial ports: the card reader on the first, the game's
 *       drive board (the steering wheel's motor controller) on the second.
 *
 * The other parts of the cabinet have their own headers:
 *   cabinet_input.h  host controls -> the cabinet's switches and pedals
 *   cards.h          which card file the player inserts
 *   board_setup.h    file paths and board serial numbers
 *   media.h, lpc.h   the media board (game storage) and network board
 */
#ifndef NATIVE_DEV_CHIHIRO_BOARD_PUBLIC_H
#define NATIVE_DEV_CHIHIRO_BOARD_PUBLIC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct NcUsbDev;

/* The prices the operator can set in the game's coin settings. */
typedef enum NcChihiroCost {
    NC_CH_COST_START,
    NC_CH_COST_CONTINUE,
    NC_CH_COST_NEW_CARD,
    NC_CH_COST_FULL_COURSE,
    NC_CH_COST_RENEW_CARD,
    NC_CH_COST_TRANSFER_CARD,
    NC_CH_COST_COUNT,
} NcChihiroCost;

/* Coin settings to write into the board's settings EEPROM at start.
 * A value of 0 keeps whatever the board already has. */
typedef struct NcChihiroCoinSettings {
    uint8_t rate;                    /* coin rate 1-10, as the test menu numbers them; 10 = free play */
    uint8_t cost[NC_CH_COST_COUNT];  /* credits for each, 1-9 */
} NcChihiroCoinSettings;

/*
 * Starts the baseboard.
 *   dir      folder for the cabinet's files (card files go in dir/cards)
 *   save     file that keeps the settings EEPROM and backup SRAM between
 *            runs; NULL keeps them in memory only
 *   coins    coin settings to apply (see NcChihiroCoinSettings)
 *   cabinet  link play cabinet number 1-4, or 0 for a single cabinet
 *   game_id  the game's 4-letter code from boot.id ("SBKD"); picks the
 *            drive board (drive.h)
 */
void nc_chihiro_board_init(const char *dir, const char *save, const NcChihiroCoinSettings *coins,
                           unsigned cabinet, const char *game_id);

/* True once nc_chihiro_board_init has run (a Chihiro game is loaded). */
bool nc_chihiro_board_active(void);

/* The QC and SC USB devices, for the USB controller to plug in.
 * NULL when the baseboard isn't active. */
struct NcUsbDev *nc_chihiro_board_qc(void);
struct NcUsbDev *nc_chihiro_board_sc(void);

/* Call once per 1 ms USB frame (from the OHCI worker thread). Reads the
 * controls, handles the card button, steps the drive board and now and
 * then saves the backup memory. */
void nc_chihiro_board_frame(void);

/* Writes the backup memory to the save file if the game changed it. */
void nc_chihiro_board_flush(void);

/* What the steering wheel's motor is being told to do, in the cabinet's
 * own units. The host turns this into force feedback (app/wheel_ffb.c). */
typedef struct NcChihiroForce {
    uint16_t target;         /* wheel position to hold: 0 left .. 0x3FF right */
    uint8_t spring, damping; /* 0 .. 0x7F */
    int8_t push;             /* -0x7F .. 0x7F, positive pushes right */
} NcChihiroForce;

/* Fills force with the current motor command. Returns false (and zeroes
 * force) when the motor is off or the game has no drive board.
 * Call on the OHCI worker thread or while holding its lock. */
bool nc_chihiro_board_force(NcChihiroForce *force);

#ifdef __cplusplus
}
#endif

#endif
