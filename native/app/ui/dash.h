/*
 * xbased front end — the dashboard (dash.c): the console-style menus
 * before a game: the library, settings, saves, online. Runs until the user
 * leaves; each game it starts runs in a process of its own.
 */
#ifndef NATIVE_APP_UI_DASH_H
#define NATIVE_APP_UI_DASH_H

#include <SDL3/SDL.h>

/* The dashboard in win until the user leaves. exe: this executable (the
 * games' process). */
int dash_run(SDL_Window *win, const char *exe);

/* The in-game menu uses the presenter's overlay or a saved frame (frame_bmp).
 * Returns the chosen action and, for a save or load, its slot. Settings are
 * saved to disk; the optional live callback receives changes as they happen. */
enum { DASH_INGAME_RESUME, DASH_INGAME_SAVE, DASH_INGAME_LOAD, DASH_INGAME_RESTART, DASH_INGAME_QUIT };
struct Config;
/* Full screen in the in-game menu (F11, Alt+Enter): this instead of the
 * window's own, when the game's window is inside a frame (win_frame.h) */
void dash_set_fullscreen_hook(void (*toggle)(void));
void dash_close_gamepads(void);
/* The pointer's place (window pixels) from elsewhere (the game's frame):
 * false, none; NULL, SDL's own. */
void dash_set_pointer_hook(bool (*where)(float *x, float *y));
/* Called as the dashboard reads its input: the run's pump of the session
 * frame's mouse into this window's events, while the in-game menu's loop
 * has the window (win_main.c). */
void dash_set_pump_hook(void (*pump)(void));
/* A notice the in-game menu shows as it next opens (a controller gone). */
void dash_ingame_notice(const char *msg);
int dash_ingame(SDL_Window *win, const char *key, const char *title, bool chihiro, bool link, const char *frame_bmp, int *slot,
                void (*live)(const struct Config *global, const struct Config *game));

#endif
