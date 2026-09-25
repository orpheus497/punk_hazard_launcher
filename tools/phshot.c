/*
 * phshot -- render the launcher UI to a PNG without a display server.
 *
 * This is a test harness, not part of the installed program.  It exists
 * because a renderer you cannot run is a renderer you cannot check: it
 * creates an OpenGL ES 2.0 context on an EGL pbuffer, drives the real
 * gfx/text/ui code exactly as app.c does, and reads the result back.
 *
 * It is the reason src/gfx.c contains no reference to SDL.  The same
 * drawing code that runs in the window runs here, so a broken shader, a
 * mispacked glyph atlas or a layout that falls off the edge shows up in a
 * file instead of only on someone's screen.
 *
 *     ./phshot -o shot.png -w 1600 -h 900 -t hazard
 *
 * Build with:  make shot
 */
#include "ph.h"
#include "gfx.h"
#include "ui.h"
#include "script.h"

#include <stdlib.h>
#include <string.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

static void
theme_fallback(struct ph_theme *t)
{
	memset(t, 0, sizeof(*t));
	strlcpy(t->name, "FALLBACK", sizeof(t->name));
	t->bg = 0x07090bffu; t->panel = 0x0e1318ffu; t->panel_alt = 0x141b22ffu;
	t->frame = 0x1f2b33ffu; t->text = 0xb9c8d2ffu; t->text_dim = 0x5d6f7cffu;
	t->text_bright = 0xeafff7ffu; t->accent = 0x1affaaffu;
	t->accent2 = 0xff2e88ffu; t->danger = 0xff3b3bffu; t->ok = 0x51ff9bffu;
	t->sel_bg = 0x1affaa2au; t->sel_fg = 0xffffffffu; t->shadow = 0x000000aau;
	t->scanline = 0.30f; t->curvature = 0.03f; t->vignette = 0.35f;
	t->aberration = 0.6f; t->glow = 0.28f; t->noise = 0.035f;
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

/*
 * Bring up EGL with no window.  EGL_PLATFORM_SURFACELESS_MESA is tried
 * first because it needs no display at all; a pbuffer on the default
 * display is the fallback.
 */
static int
egl_up(EGLDisplay *dpy_out, EGLSurface *surf_out, EGLContext *ctx_out,
    int w, int h)
{
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display;
	EGLDisplay dpy = EGL_NO_DISPLAY;
	EGLConfig cfg;
	EGLint maj, min, n = 0;
	static const EGLint cfg_attr[] = {
		EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
		EGL_NONE
	};
	static const EGLint ctx_attr[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE
	};
	EGLint pb_attr[] = { EGL_WIDTH, 0, EGL_HEIGHT, 0, EGL_NONE };

	get_platform_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)
	    eglGetProcAddress("eglGetPlatformDisplayEXT");
	if (get_platform_display != NULL)
		dpy = get_platform_display(EGL_PLATFORM_SURFACELESS_MESA,
		    EGL_DEFAULT_DISPLAY, NULL);
	if (dpy == EGL_NO_DISPLAY)
		dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (dpy == EGL_NO_DISPLAY) {
		ph_warn("no EGL display");
		return -1;
	}
	if (!eglInitialize(dpy, &maj, &min)) {
		ph_warn("eglInitialize failed (0x%x)", eglGetError());
		return -1;
	}
	ph_info("EGL %d.%d, vendor %s", maj, min, eglQueryString(dpy, EGL_VENDOR));

	if (!eglBindAPI(EGL_OPENGL_ES_API)) {
		ph_warn("eglBindAPI failed");
		return -1;
	}
	if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1) {
		ph_warn("no suitable EGL config");
		return -1;
	}
	pb_attr[1] = w;
	pb_attr[3] = h;
	*surf_out = eglCreatePbufferSurface(dpy, cfg, pb_attr);
	if (*surf_out == EGL_NO_SURFACE) {
		ph_warn("eglCreatePbufferSurface failed (0x%x)", eglGetError());
		return -1;
	}
	*ctx_out = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
	if (*ctx_out == EGL_NO_CONTEXT) {
		ph_warn("eglCreateContext failed (0x%x)", eglGetError());
		return -1;
	}
	if (!eglMakeCurrent(dpy, *surf_out, *surf_out, *ctx_out)) {
		ph_warn("eglMakeCurrent failed (0x%x)", eglGetError());
		return -1;
	}
	*dpy_out = dpy;
	return 0;
}

int
main(int argc, char **argv)
{
	struct ph_paths  paths;
	struct ph_script *sc;
	struct ph_gfx    *gfx;
	struct ph_ui     *ui;
	struct ph_lib     lib;
	struct ph_config  cfg;
	struct ph_theme   theme;
	struct ph_layout  lay;
	EGLDisplay dpy = EGL_NO_DISPLAY;
	EGLSurface surf = EGL_NO_SURFACE;
	EGLContext ctx = EGL_NO_CONTEXT;
	unsigned char *px;
	const char *out = "shot.png", *want_theme = NULL, *search = NULL;
	int w = 1600, h = 900, crt = -1, select_n = 0, help = 0, tab = 0;
	int opt_n = 0, sort_n = 0, i, f, pw, ph_h;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) out = argv[++i];
		else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) w = atoi(argv[++i]);
		else if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) h = atoi(argv[++i]);
		else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) want_theme = argv[++i];
		else if (strcmp(argv[i], "--search") == 0 && i + 1 < argc) search = argv[++i];
		else if (strcmp(argv[i], "--select") == 0 && i + 1 < argc) select_n = atoi(argv[++i]);
		else if (strcmp(argv[i], "--help-overlay") == 0) help = 1;
		else if (strcmp(argv[i], "--tab") == 0) tab = 1;
		else if (strcmp(argv[i], "--sort") == 0 && i + 1 < argc) sort_n = atoi(argv[++i]);
		else if (strcmp(argv[i], "--option") == 0 && i + 1 < argc) opt_n = atoi(argv[++i]);
		else if (strcmp(argv[i], "--no-crt") == 0) crt = 0;
		else if (strcmp(argv[i], "--crt") == 0) crt = 1;
		else if (strcmp(argv[i], "-v") == 0) ph_verbose_set(1);
		else {
			fprintf(stderr, "usage: phshot [-o out.png] [-w W] [-h H] "
			    "[-t theme] [--search TEXT] [--select N] "
			    "[--tab] [--option N] "
			    "[--help-overlay] [--crt|--no-crt] [-v]\n");
			return 2;
		}
	}
	if (w < 320) w = 320;
	if (h < 240) h = 240;

	if (egl_up(&dpy, &surf, &ctx, w, h) != 0)
		return 1;

	ph_paths_init(&paths);
	ph_lib_init(&lib);

	memset(&cfg, 0, sizeof(cfg));
	cfg.crt = 1;
	strlcpy(cfg.theme, "hazard", sizeof(cfg.theme));
	layout_fallback(&lay);
	theme_fallback(&theme);

	if ((sc = ph_script_open(&paths)) != NULL) {
		ph_script_config(sc, &cfg);
		ph_script_layout(sc, &lay);
		if (want_theme != NULL)
			strlcpy(cfg.theme, want_theme, sizeof(cfg.theme));
		if (ph_script_theme(sc, cfg.theme, &theme) != 0)
			theme_fallback(&theme);
	}
	if (crt >= 0)
		cfg.crt = crt;

	if (ph_lib_scan(&lib, &paths) != 0)
		ph_warn("could not scan %s", paths.games);
	ph_info("library: %zu games", lib.n);

	if ((gfx = ph_gfx_create(w, h)) == NULL)
		return 1;
	if ((ui = ph_ui_create(gfx, sc, &lib, &cfg, &theme, &lay)) == NULL)
		return 1;

	if (search != NULL) {
		ph_ui_action(ui, PH_ACT_SEARCH);
		ph_ui_text(ui, search);
	}
	for (i = 0; i < select_n; i++)
		ph_ui_action(ui, PH_ACT_RIGHT);
	for (i = 0; i < sort_n; i++)
		ph_ui_action(ui, PH_ACT_SORT);
	if (tab) {
		ph_ui_action(ui, PH_ACT_DETAILS);	/* focus the panel */
		for (i = 0; i < opt_n; i++)
			ph_ui_action(ui, PH_ACT_DOWN);
	}
	if (help)
		ph_ui_action(ui, PH_ACT_HELP);

	/*
	 * Draw several frames before capturing.  The first frame does the
	 * glyph rasterisation and lays out nothing that has settled yet;
	 * the scroll easing also needs a few steps to reach its target.
	 * Sixty frames of 1/60s is one second of simulated time.
	 */
	for (f = 0; f < 60; f++) {
		ph_ui_update(ui, 1.0f / 60.0f);
		ph_gfx_frame_begin(gfx, theme.bg);
		ph_ui_draw(ui, 1.25f);		/* fixed time: reproducible noise */
		ph_gfx_frame_end(gfx, &theme, 1.25f, cfg.crt);
	}

	px = ph_gfx_capture(gfx, &pw, &ph_h);
	if (!ph_image_write_png(out, pw, ph_h, px)) {
		ph_warn("could not write %s", out);
		return 1;
	}
	fprintf(stderr, "phshot: wrote %s (%dx%d, theme %s, crt %s)\n",
	    out, pw, ph_h, theme.name, cfg.crt ? "on" : "off");

	free(px);
	ph_ui_destroy(ui);
	ph_gfx_destroy(gfx);
	ph_lib_free(&lib);
	ph_script_close(sc);
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroySurface(dpy, surf);
	eglDestroyContext(dpy, ctx);
	eglTerminate(dpy);
	return 0;
}
