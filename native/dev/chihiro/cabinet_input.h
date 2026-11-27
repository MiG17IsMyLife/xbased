/*
 * Chihiro cabinet controls: the host's controller and wheel, turned into
 * the switches and analog channels the game reads from the JVS I/O board.
 *
 * The front end reports two sources every frame: the controller
 * (nc_chihiro_board_input) and an optional wheel (nc_chihiro_board_wheel).
 * They are merged on the USB thread: the wheel's steering and pedals win
 * when it has them, and a button counts as pressed if either source
 * presses it.
 */
#ifndef NATIVE_DEV_CHIHIRO_CABINET_INPUT_H
#define NATIVE_DEV_CHIHIRO_CABINET_INPUT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The state of the cabinet's controls. */
typedef struct NcChihiroInput {
    uint16_t steering;      /* 0x0000 full left, 0x8000 centre, 0xFFFF full right */
    uint16_t gas, brake;    /* 0 released .. 0xFFFF fully pressed */
    bool test, service, coin, start, view, perspective;
    bool shift_up, shift_down;  /* sequential shifting (ignored with h_shifter) */
    bool h_shifter;         /* true: "gear" selects the gear directly */
    uint8_t gear;           /* with h_shifter: 0 neutral, 1-6 */
    bool card;              /* the card button: insert a card or take it back */
} NcChihiroInput;

/* The controller's state (call from the UI thread, any time). */
void nc_chihiro_board_input(const NcChihiroInput *in);

/* A host wheel's state. With axes true, its steering and pedals replace
 * the controller's; its buttons are always added to the controller's. */
void nc_chihiro_board_wheel(bool axes, const NcChihiroInput *in);

/* True if TEST was held within the last 3 seconds. The front end uses it
 * to reboot into the game's test menu instead of the game. */
bool nc_chihiro_board_test_pressed(void);

#ifdef __cplusplus
}
#endif

#endif
