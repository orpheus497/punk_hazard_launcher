/*
 * input.h -- SDL events to semantic actions.
 *
 * Nothing above this layer knows what a scancode is.  The UI is told
 * "up", not "the user pressed K", which is what lets lua/keys.lua rebind
 * anything without a single change in ui.c.
 */
#ifndef PH_INPUT_H
#define PH_INPUT_H

#include "ph.h"
#include "script.h"

#include <SDL.h>

struct ph_input;

struct ph_input *ph_input_create(struct ph_script *sc);
void ph_input_destroy(struct ph_input *in);

/* Controller hotplug; call for SDL_CONTROLLERDEVICEADDED/REMOVED. */
void ph_input_device_event(struct ph_input *in, const SDL_Event *e);

enum ph_action ph_input_key(struct ph_input *in, SDL_Keycode k);
enum ph_action ph_input_pad(struct ph_input *in, Uint8 button);

/* Analog sticks repeat like a held d-pad; call once per frame. */
enum ph_action ph_input_tick(struct ph_input *in, float dt);

int ph_input_pad_count(const struct ph_input *in);

#endif /* PH_INPUT_H */
