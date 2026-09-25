/*
 * input.c -- keyboard, mouse and gamepad, normalised into enum ph_action.
 *
 * SDL's game controller layer is used rather than its raw joystick layer:
 * it maps whatever the kernel exposes onto the standard Xbox-style button
 * names through SDL's built-in mapping database, so lua/keys.lua can talk
 * about "a" and "dpup" instead of "button 3 on this particular pad".  On
 * FreeBSD the devices underneath are evdev nodes from the hid/evdev
 * drivers; SDL opens them, so punkhazard does not have to.
 */
#include "input.h"

#include <stdlib.h>
#include <string.h>

#define MAX_PADS 8

/* Below this, a stick is at rest. Xbox-style pads idle around 2-4k. */
#define AXIS_DEADZONE 12000

/* Held-stick repeat, matching a typical keyboard autorepeat. */
#define REPEAT_DELAY  0.38f
#define REPEAT_RATE   0.07f

struct ph_input {
	struct ph_script   *sc;
	SDL_GameController *pads[MAX_PADS];
	int                 npads;

	int    axis_v, axis_h;		/* -1, 0, +1 */
	float  timer;
	int    repeating;
};

struct ph_input *
ph_input_create(struct ph_script *sc)
{
	struct ph_input *in = ph_xcalloc(1, sizeof(*in));
	int i, n;

	in->sc = sc;
	n = SDL_NumJoysticks();
	for (i = 0; i < n && in->npads < MAX_PADS; i++) {
		if (!SDL_IsGameController(i))
			continue;
		if ((in->pads[in->npads] = SDL_GameControllerOpen(i)) != NULL) {
			ph_info("gamepad: %s",
			    SDL_GameControllerName(in->pads[in->npads]));
			in->npads++;
		}
	}
	return in;
}

void
ph_input_destroy(struct ph_input *in)
{
	int i;

	if (in == NULL)
		return;
	for (i = 0; i < in->npads; i++)
		if (in->pads[i] != NULL)
			SDL_GameControllerClose(in->pads[i]);
	free(in);
}

void
ph_input_device_event(struct ph_input *in, const SDL_Event *e)
{
	int i;

	if (e->type == SDL_CONTROLLERDEVICEADDED) {
		if (in->npads >= MAX_PADS || !SDL_IsGameController(e->cdevice.which))
			return;
		in->pads[in->npads] = SDL_GameControllerOpen(e->cdevice.which);
		if (in->pads[in->npads] != NULL) {
			ph_info("gamepad attached: %s",
			    SDL_GameControllerName(in->pads[in->npads]));
			in->npads++;
		}
	} else if (e->type == SDL_CONTROLLERDEVICEREMOVED) {
		for (i = 0; i < in->npads; i++) {
			SDL_Joystick *j;

			if (in->pads[i] == NULL)
				continue;
			j = SDL_GameControllerGetJoystick(in->pads[i]);
			if (j != NULL &&
			    SDL_JoystickInstanceID(j) == e->cdevice.which) {
				SDL_GameControllerClose(in->pads[i]);
				in->pads[i] = in->pads[in->npads - 1];
				in->pads[--in->npads] = NULL;
				ph_info("gamepad detached");
				break;
			}
		}
	}
}

int
ph_input_pad_count(const struct ph_input *in)
{
	return in == NULL ? 0 : in->npads;
}

enum ph_action
ph_input_key(struct ph_input *in, SDL_Keycode k)
{
	const char *name = SDL_GetKeyName(k);

	/*
	 * SDL reports unaccented letter keys in upper case.  Look the name
	 * up as given first, so a keys.lua entry is matched exactly as
	 * written; only then try the other case, so both "Q" and "q" work
	 * in a user's own keymap.
	 */
	enum ph_action a = ph_script_key(in->sc, name);

	if (a == PH_ACT_NONE && name != NULL && name[0] != '\0' &&
	    name[1] == '\0') {
		char alt[2];

		alt[0] = (name[0] >= 'A' && name[0] <= 'Z')
		    ? (char)(name[0] + 32)
		    : (char)((name[0] >= 'a' && name[0] <= 'z')
		        ? name[0] - 32 : name[0]);
		alt[1] = '\0';
		a = ph_script_key(in->sc, alt);
	}
	return a;
}

enum ph_action
ph_input_pad(struct ph_input *in, Uint8 button)
{
	const char *name = SDL_GameControllerGetStringForButton(
	    (SDL_GameControllerButton)button);

	return name != NULL ? ph_script_pad(in->sc, name) : PH_ACT_NONE;
}

/*
 * Analog sticks.  A stick held past the deadzone should behave like a held
 * d-pad: one immediate step, a pause, then a steady repeat.  Polling the
 * axis directly each frame (rather than reacting to SDL_CONTROLLERAXISMOTION
 * events) keeps the logic in one place and avoids the event storm a noisy
 * stick generates.
 */
enum ph_action
ph_input_tick(struct ph_input *in, float dt)
{
	int v = 0, h = 0, i;

	for (i = 0; i < in->npads; i++) {
		Sint16 ay, ax;

		if (in->pads[i] == NULL)
			continue;
		ay = SDL_GameControllerGetAxis(in->pads[i], SDL_CONTROLLER_AXIS_LEFTY);
		ax = SDL_GameControllerGetAxis(in->pads[i], SDL_CONTROLLER_AXIS_LEFTX);
		if (ay < -AXIS_DEADZONE)      v = -1;
		else if (ay > AXIS_DEADZONE)  v = +1;
		if (ax < -AXIS_DEADZONE)      h = -1;
		else if (ax > AXIS_DEADZONE)  h = +1;
	}

	if (v != in->axis_v || h != in->axis_h) {
		in->axis_v = v;
		in->axis_h = h;
		in->timer = REPEAT_DELAY;
		in->repeating = 0;
		if (v != 0)
			return v < 0 ? PH_ACT_UP : PH_ACT_DOWN;
		if (h != 0)
			return h < 0 ? PH_ACT_PAGE_UP : PH_ACT_PAGE_DOWN;
		return PH_ACT_NONE;
	}
	if (v == 0 && h == 0)
		return PH_ACT_NONE;

	if ((in->timer -= dt) <= 0.0f) {
		in->timer = REPEAT_RATE;
		in->repeating = 1;
		if (v != 0)
			return v < 0 ? PH_ACT_UP : PH_ACT_DOWN;
		return h < 0 ? PH_ACT_PAGE_UP : PH_ACT_PAGE_DOWN;
	}
	return PH_ACT_NONE;
}
