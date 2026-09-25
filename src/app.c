/*
 * app.c -- the SDL window, the GL context, and the main loop.
 *
 * WHY SDL, AND WHY AN OpenGL ES CONTEXT
 * -------------------------------------
 * SDL2 (zlib licence) already contains tested X11 and Wayland video
 * backends, fullscreen handling and the game-controller mapping database.
 * Writing raw Xlib and raw libwayland-client backends would mean owning
 * two of everything -- including two fullscreen paths and two input paths
 * -- for a launcher whose job is to get out of the way and run a game.
 *
 * The context requested here is OpenGL ES 2.0, not desktop GL.  That is
 * deliberate and it is what makes the two display servers converge:
 *
 *   - Wayland has no GLX at all; EGL is the only option there.
 *   - On X11, SDL_HINT_VIDEO_X11_FORCE_EGL exists precisely because, as
 *     SDL_hints.h puts it, "By default SDL will use GLX when both are
 *     present."
 *
 * Setting that hint and asking for an ES profile puts X11 and Wayland on
 * the same EGL + GLES2 path, so there is one rendering path that is
 * actually exercised on both rather than one tested path and one hoped-for
 * one.
 */
#include "ph.h"
#include "gfx.h"
#include "text.h"
#include "ui.h"
#include "input.h"
#include "script.h"

#include <stdlib.h>
#include <string.h>

#include <SDL.h>

/* Used when lua/theme.lua is missing entirely, so the launcher still
 * comes up readable instead of black-on-black. */
static void
theme_fallback(struct ph_theme *t)
{
	memset(t, 0, sizeof(*t));
	strlcpy(t->name, "FALLBACK", sizeof(t->name));
	t->bg          = 0x07090bffu;
	t->panel       = 0x0e1318ffu;
	t->panel_alt   = 0x141b22ffu;
	t->frame       = 0x1f2b33ffu;
	t->text        = 0xb9c8d2ffu;
	t->text_dim    = 0x5d6f7cffu;
	t->text_bright = 0xeafff7ffu;
	t->accent      = 0x1affaaffu;
	t->accent2     = 0xff2e88ffu;
	t->danger      = 0xff3b3bffu;
	t->ok          = 0x51ff9bffu;
	t->sel_bg      = 0x1affaa2au;
	t->sel_fg      = 0xffffffffu;
	t->shadow      = 0x000000aau;
	t->scanline = 0.30f; t->curvature = 0.03f; t->vignette = 0.35f;
	t->aberration = 0.6f; t->glow = 0.28f;     t->noise = 0.035f;
}

static void
layout_fallback(struct ph_layout *l)
{
	l->pad = 20; l->top_h = 116; l->bottom_h = 96;
	l->split = 0.70f; l->tile_w = 196; l->tile_gap = 18;
	l->tile_aspect = 1.34f;
	l->font_px = 20; l->font_px_title = 34; l->font_px_small = 15;
	l->anim_speed = 16.0f;
}

/* Multiply every metric by the display scale so the same Lua numbers hold
 * on a HiDPI panel, where the drawable is larger than the window. */
static void
layout_scale(struct ph_layout *l, float s)
{
	if (s <= 1.001f && s >= 0.999f)
		return;
	l->pad             = (int)(l->pad * s);
	l->top_h           = (int)(l->top_h * s);
	l->bottom_h        = (int)(l->bottom_h * s);
	l->tile_w          = (int)(l->tile_w * s);
	l->tile_gap        = (int)(l->tile_gap * s);
	l->font_px         = (int)(l->font_px * s);
	l->font_px_title   = (int)(l->font_px_title * s);
	l->font_px_small   = (int)(l->font_px_small * s);
}

static float
display_scale(SDL_Window *win)
{
	int ww = 0, wh = 0, dw = 0, dh = 0;

	SDL_GetWindowSize(win, &ww, &wh);
	SDL_GL_GetDrawableSize(win, &dw, &dh);
	return (ww > 0) ? (float)dw / (float)ww : 1.0f;
}

/*
 * Run a game.  The launcher gets out of the way first: fullscreen is
 * dropped (a fullscreen OpenGL window will otherwise sit on top of, or
 * fight with, whatever the game opens) and the window is minimised.
 */
static void
run_game(SDL_Window *win, struct ph_ui *ui, struct ph_config *cfg,
    struct ph_game *g)
{
	struct ph_run_result res;
	char when[48];

	SDL_SetWindowFullscreen(win, 0);
	SDL_MinimizeWindow(win);

	if (ph_launch(g, &res) < 0) {
		ph_ui_toast(ui, "could not launch %s", g->slug);
	} else {
		ph_human_time(when, sizeof(when), res.seconds);
		if (res.status == 0)
			ph_ui_toast(ui, "%s  \xe2\x80\x94  played %s",
			    g->title, when);
		else
			ph_ui_toast(ui, "%s exited with status %d after %s",
			    g->title, res.status, when);
	}

	SDL_RestoreWindow(win);
	SDL_RaiseWindow(win);
	if (cfg->fullscreen)
		SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP);
	/*
	 * Drain input that arrived while the game had focus, so a keypress
	 * meant for the game does not land in the launcher.
	 *
	 * The ranges are deliberately narrow.  A single flush from
	 * SDL_KEYDOWN to SDL_MULTIGESTURE also covers
	 * SDL_CONTROLLERDEVICEADDED/REMOVED, which sit between them in the
	 * event enum -- so a pad unplugged while the game was running would
	 * be dropped here and ph_input would go on holding a dead handle.
	 * Device events are left in the queue.
	 */
	SDL_PumpEvents();
	SDL_FlushEvents(SDL_KEYDOWN, SDL_TEXTINPUT);
	SDL_FlushEvents(SDL_MOUSEMOTION, SDL_MOUSEWHEEL);
	SDL_FlushEvents(SDL_CONTROLLERAXISMOTION, SDL_CONTROLLERBUTTONUP);
}

/* Keys that keep their meaning while the search field has focus.  Every
 * other key is text, which is why this is a whitelist keyed on the
 * keycode rather than a lookup in the user's keymap. */
static enum ph_action
search_mode_action(SDL_Keycode k)
{
	switch (k) {
	case SDLK_ESCAPE:   return PH_ACT_BACK;
	case SDLK_RETURN:
	case SDLK_KP_ENTER: return PH_ACT_LAUNCH;
	case SDLK_UP:       return PH_ACT_UP;
	case SDLK_DOWN:     return PH_ACT_DOWN;
	case SDLK_PAGEUP:   return PH_ACT_PAGE_UP;
	case SDLK_PAGEDOWN: return PH_ACT_PAGE_DOWN;
	case SDLK_HOME:     return PH_ACT_HOME;
	case SDLK_END:      return PH_ACT_END;
	case SDLK_F11:      return PH_ACT_FULLSCREEN;
	default:            return PH_ACT_NONE;
	}
}

int
ph_app_run(const struct ph_paths *p)
{
	SDL_Window      *win = NULL;
	SDL_GLContext    ctx = NULL;
	struct ph_script *sc = NULL;
	struct ph_gfx   *gfx = NULL;
	struct ph_ui     *ui = NULL;
	struct ph_input  *in = NULL;
	struct ph_lib     lib;
	struct ph_config  cfg;
	struct ph_theme   theme;
	struct ph_layout  lay;
	Uint64 t_prev;
	float  t_total = 0.0f;
	int    rc = 1, running = 1, skip_text = 0;
	int    drawable_w = 0, drawable_h = 0;

	ph_lib_init(&lib);

	/* --- policy, before anything is created ----------------------- */
	memset(&cfg, 0, sizeof(cfg));
	cfg.win_w = 1280; cfg.win_h = 720; cfg.vsync = 1; cfg.crt = 1;
	strlcpy(cfg.theme, "hazard", sizeof(cfg.theme));
	layout_fallback(&lay);
	theme_fallback(&theme);

	if ((sc = ph_script_open(p)) != NULL) {
		ph_script_config(sc, &cfg);
		ph_script_layout(sc, &lay);
		if (ph_script_theme(sc, cfg.theme, &theme) != 0) {
			ph_warn("theme '%s' not found; using the fallback",
			    cfg.theme);
			theme_fallback(&theme);
		}
	} else {
		ph_warn("no Lua policy loaded; using compiled-in defaults");
	}

	/* --- SDL ------------------------------------------------------ */
	/* Must be set before SDL_Init: the video backend reads it while
	 * choosing between GLX and EGL. */
	SDL_SetHint(SDL_HINT_VIDEO_X11_FORCE_EGL, "1");
	SDL_SetHint(SDL_HINT_VIDEO_HIGHDPI_DISABLED, "0");

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
		ph_warn("SDL_Init: %s", SDL_GetError());
		goto done;
	}

	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);	/* 2D only */

	win = SDL_CreateWindow(PH_NAME,
	    SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
	    cfg.win_w, cfg.win_h,
	    SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
	    (cfg.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
	if (win == NULL) {
		ph_warn("SDL_CreateWindow: %s", SDL_GetError());
		goto done;
	}
	if ((ctx = SDL_GL_CreateContext(win)) == NULL) {
		ph_warn("SDL_GL_CreateContext: %s", SDL_GetError());
		ph_warn("an OpenGL ES 2.0 capable driver is required "
		    "(FreeBSD: pkg install mesa-libs mesa-dri)");
		goto done;
	}
	/* 1 = wait for vblank. Failing is harmless: it just means tearing
	 * or a free-running loop, so it is reported and ignored. */
	if (SDL_GL_SetSwapInterval(cfg.vsync ? 1 : 0) != 0)
		ph_info("swap interval unavailable: %s", SDL_GetError());

	ph_info("video driver: %s", SDL_GetCurrentVideoDriver());

	SDL_GL_GetDrawableSize(win, &drawable_w, &drawable_h);
	layout_scale(&lay, display_scale(win));

	if ((gfx = ph_gfx_create(drawable_w, drawable_h)) == NULL)
		goto done;

	if (ph_lib_scan(&lib, p) != 0)
		ph_warn("could not read %s", p->games);

	if ((ui = ph_ui_create(gfx, sc, &lib, &cfg, &theme, &lay)) == NULL)
		goto done;
	in = ph_input_create(sc);
	SDL_StartTextInput();

	/* --- main loop ------------------------------------------------ */
	t_prev = SDL_GetPerformanceCounter();
	while (running) {
		SDL_Event e;
		float dt;
		Uint64 now;
		enum ph_req req;
		struct ph_game *target;
		enum ph_action pad_act;

		now = SDL_GetPerformanceCounter();
		dt = (float)((double)(now - t_prev) /
		    (double)SDL_GetPerformanceFrequency());
		t_prev = now;
		if (dt > 0.25f)		/* after a launch, or a stall */
			dt = 0.25f;
		t_total += dt;

		while (SDL_PollEvent(&e)) {
			switch (e.type) {
			case SDL_QUIT:
				running = 0;
				break;

			case SDL_WINDOWEVENT:
				if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
				    e.window.event == SDL_WINDOWEVENT_RESIZED) {
					SDL_GL_GetDrawableSize(win,
					    &drawable_w, &drawable_h);
					ph_gfx_resize(gfx, drawable_w, drawable_h);
				}
				break;

			case SDL_KEYDOWN: {
				enum ph_action a;

				/*
				 * Ask the UI directly rather than using a
				 * value cached at the end of the previous
				 * frame: SDL delivers events in batches, so
				 * the '/' that opens the search field and the
				 * first character typed into it can arrive in
				 * the same batch.  With a stale flag that
				 * first character was routed as a binding.
				 */
				skip_text = 0;
				if (ph_ui_is_searching(ui)) {
					if (e.key.keysym.sym == SDLK_BACKSPACE) {
						ph_ui_backspace(ui);
						break;
					}
					a = search_mode_action(e.key.keysym.sym);
					if (a == PH_ACT_NONE)
						break;	/* it is typing */
				} else {
					a = ph_input_key(in, e.key.keysym.sym);
				}
				if (a == PH_ACT_SEARCH)
					skip_text = 1;	/* eat the '/' itself */
				ph_ui_action(ui, a);
				break;
			}

			case SDL_TEXTINPUT:
				if (skip_text)
					skip_text = 0;
				else
					ph_ui_text(ui, e.text.text);
				break;

			case SDL_MOUSEMOTION:
				ph_ui_mouse_move(ui,
				    (int)(e.motion.x * display_scale(win)),
				    (int)(e.motion.y * display_scale(win)));
				break;

			case SDL_MOUSEBUTTONDOWN:
				if (e.button.button == SDL_BUTTON_LEFT)
					ph_ui_mouse_click(ui,
					    (int)(e.button.x * display_scale(win)),
					    (int)(e.button.y * display_scale(win)),
					    e.button.clicks);
				break;

			case SDL_MOUSEWHEEL:
				ph_ui_mouse_wheel(ui, e.wheel.y);
				break;

			case SDL_CONTROLLERBUTTONDOWN:
				ph_ui_action(ui,
				    ph_input_pad(in, e.cbutton.button));
				break;

			case SDL_CONTROLLERDEVICEADDED:
			case SDL_CONTROLLERDEVICEREMOVED:
				ph_input_device_event(in, &e);
				break;

			default:
				break;
			}
		}

		if ((pad_act = ph_input_tick(in, dt)) != PH_ACT_NONE)
			ph_ui_action(ui, pad_act);

		ph_ui_update(ui, dt);

		/* Act on whatever the UI asked for. */
		while ((req = ph_ui_poll(ui, &target)) != PH_REQ_NONE) {
			switch (req) {
			case PH_REQ_QUIT:
				running = 0;
				break;
			case PH_REQ_FULLSCREEN:
				cfg.fullscreen = !cfg.fullscreen;
				SDL_SetWindowFullscreen(win, cfg.fullscreen ?
				    SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
				SDL_GL_GetDrawableSize(win, &drawable_w,
				    &drawable_h);
				ph_gfx_resize(gfx, drawable_w, drawable_h);
				break;
			case PH_REQ_RELOAD:
				/* The GL texture names live in the records
				 * ph_lib_scan is about to discard. */
				ph_ui_release_covers(ui);
				if (ph_lib_scan(&lib, p) == 0) {
					ph_ui_refresh(ui);
					ph_ui_toast(ui, "rescanned: %zu games",
					    lib.n);
				} else {
					ph_ui_toast(ui, "rescan failed");
				}
				break;
			case PH_REQ_LAUNCH:
				if (target != NULL) {
					run_game(win, ui, &cfg, target);
					t_prev = SDL_GetPerformanceCounter();
				}
				break;
			case PH_REQ_NONE:
			default:
				break;
			}
		}

		ph_gfx_frame_begin(gfx, theme.bg);
		ph_ui_draw(ui, t_total);
		ph_gfx_frame_end(gfx, &theme, t_total, cfg.crt);
		SDL_GL_SwapWindow(win);
	}
	rc = 0;

done:
	ph_input_destroy(in);
	ph_ui_destroy(ui);
	ph_gfx_destroy(gfx);
	ph_lib_free(&lib);
	ph_script_close(sc);
	if (ctx != NULL)
		SDL_GL_DeleteContext(ctx);
	if (win != NULL)
		SDL_DestroyWindow(win);
	SDL_Quit();
	return rc;
}
