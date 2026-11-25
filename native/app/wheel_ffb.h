/* Force feedback for the PC front end's selected Chihiro wheel. */
#ifndef NATIVE_APP_WHEEL_FFB_H
#define NATIVE_APP_WHEEL_FFB_H

#include <SDL3/SDL_joystick.h>

#define NC_WHEEL_FFB_STRENGTH_DEFAULT 50
#define NC_WHEEL_FFB_DEADBAND_DEFAULT 2

/* These functions run on the SDL window thread. The caller owns the joystick. */
bool nc_wheel_ffb_open(SDL_Joystick *joystick, int strength, int deadband, bool invert_push);
void nc_wheel_ffb_poll(bool running);
void nc_wheel_ffb_stop(void);
void nc_wheel_ffb_close(void);

#endif
