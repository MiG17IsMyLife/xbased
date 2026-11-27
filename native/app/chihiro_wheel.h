/*
 * Chihiro host wheel: a real steering wheel (and optional separate
 * H shifter) on the PC, read each frame and passed to the cabinet as its
 * steering, pedals and buttons. Also starts force feedback (wheel_ffb.c).
 *
 * Settings (each read through the option callback):
 *   chihiro.wheel=NAME             joystick to use (default: "R3", then
 *                                  any "Wheel"); "off" disables it
 *   chihiro.wheel.steer/gas/brake  axis number: X 0, Y 1, Z 2, RX 3, RY 4,
 *                                  RZ 5; -1 none (KittenTools' WMMT3 layout)
 *   chihiro.wheel.<button>=N       1-based button number, 0 none
 *   chihiro.wheel.gear1..gear6=N   H shifter buttons, 0 none
 *   chihiro.shifter=NAME           a separate joystick for the H shifter
 *   chihiro.ffb=1                  force feedback (with .strength,
 *                                  .deadband, .invert)
 *   chihiro.focus_input=1          ignore the wheel while the window is in
 *                                  the background (two cabinets on one PC)
 * NC_TRACE_WHEEL in the environment logs moving axes and pressed buttons.
 *
 * Everything here runs on the SDL window thread.
 */
#ifndef NATIVE_APP_CHIHIRO_WHEEL_H
#define NATIVE_APP_CHIHIRO_WHEEL_H

#include <SDL3/SDL_joystick.h>
#include <stdbool.h>

/*
 * Tells the module how to reach the front end. Call once before anything else.
 *   option              looks up a setting's value, NULL when unset
 *   wait_for_previous   true while the wheel must not be opened yet
 *                       (may be NULL; see win_main.c previous_run_alive)
 */
void chihiro_wheel_setup(const char *(*option)(const char *name), bool (*wait_for_previous)(void));

/* The game's settings key ("CH-SGBZ-..."): picks its cabinet's default
 * wheel buttons (input_map.h im_cabinet_of). Call when a game is loaded. */
void chihiro_wheel_set_game(const char *key);

/* Opens the configured wheel if it isn't open yet. */
void chihiro_wheel_open(void);

/* Stops force feedback, closes the wheel and shifter, and tells the
 * cabinet the wheel is gone. */
void chihiro_wheel_close(void);

/* True if id is the open wheel. */
bool chihiro_wheel_is(SDL_JoystickID id);

/* True if id belongs to the cabinet (the wheel or the shifter), so it must
 * not be used as a player's controller (that would take away the wheel's
 * force feedback). With by_name, a device not opened yet (the wheel waits
 * for the previous run to end) is also recognised by its setting's name. */
bool chihiro_wheel_owns(SDL_JoystickID id, bool by_name);

/* Reads the wheel and passes it to the cabinet. Call once per frame.
 *   focused  the window is in front
 *   running  the emulation is running (not paused, not booting) */
void chihiro_wheel_poll(bool focused, bool running);

#endif
