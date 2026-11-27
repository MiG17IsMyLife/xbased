/*
 * Chihiro drive board: the board that drives the steering wheel's motor
 * (force feedback).
 *
 * Each racing game uses its own board, with its own protocol on the SC's
 * second serial port and/or the JVS outputs. The game ID picks one
 * (nc_chihiro_drive_for); the rest of the cabinet only calls the
 * functions in NcChihiroDrive, so it doesn't care which board it is.
 *
 * To support a new game's board: write an NcChihiroDrive for it (v257.c
 * is Maximum Tune's) and add the game ID to the table in drive.c.
 * Any function may be NULL if the board doesn't need it.
 */
#ifndef NATIVE_DEV_CHIHIRO_DRIVE_H
#define NATIVE_DEV_CHIHIRO_DRIVE_H

#include <stdbool.h>
#include <stdint.h>

struct NcChihiroForce;

typedef struct NcChihiroDrive {
    const char *name;
    void (*init)(void);
    /* called every 16 ms */
    void (*tick)(void);
    /* the JVS board's outputs changed (the motor relay, lamps) */
    void (*outputs)(const uint8_t outputs[4]);
    /* the player's wheel position, 0 left .. 0x3FF right */
    void (*wheel)(uint16_t position);
    /* the SC's second serial port: rx gets a byte from the game; tx gives
     * the next byte for the game, false when there is none */
    void (*rx)(uint8_t byte);
    bool (*tx)(uint8_t *byte);
    /* the motor's current command (board.h); false when the motor is off */
    bool (*force)(struct NcChihiroForce *force);
    /* true while the board itself holds the wheel (a start-up check, hands
     * off); steering gets the position it holds it at, as the JVS reads it */
    bool (*holding)(uint16_t *steering);
} NcChihiroDrive;

/* The drive board for a game ID from boot.id ("SBKD"). NULL if xbased
 * knows none for it: no force feedback, nothing on the serial port. */
const NcChihiroDrive *nc_chihiro_drive_for(const char *game_id);

/* Maximum Tune 1 and 2: the Namco V257 STR PCB (v257.c) */
extern const NcChihiroDrive nc_chihiro_drive_v257;

/* OutRun 2: Sega's servo motor drive board (sega_servo.c) */
extern const NcChihiroDrive nc_chihiro_drive_sega_servo;

#endif
