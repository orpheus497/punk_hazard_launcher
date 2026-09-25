/*
 * stb.c -- the single translation unit that instantiates the vendored
 * single-header libraries.
 *
 * stb_truetype and stb_image are header-only: exactly one file in the
 * program must define STB_*_IMPLEMENTATION so the definitions are emitted
 * once.  Keeping that file separate means the Makefile can compile it with
 * -w, so third-party warnings never dilute the -Wall -Wextra -Wshadow
 * -Wcast-qual output from our own code.
 *
 * Licence: stb is dual-licensed MIT / public domain (third_party/stb/LICENSE).
 *
 * SECURITY NOTE, quoted from stb_truetype.h itself:
 *     "NO SECURITY GUARANTEE -- DO NOT USE THIS ON UNTRUSTED FONT FILES
 *      This library does no range checking of the offsets found in the
 *      file, meaning an attacker can use it to read arbitrary memory."
 * punkhazard only ever feeds it the typeface baked into its own .rodata at
 * build time, never a file chosen at runtime, so there is no untrusted
 * font input path.  stb_image, by contrast, IS pointed at user-supplied
 * cover art; that is a deliberate, bounded risk on local files the user
 * placed in their own library directory.
 */
#include "ph.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_GIF
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "gfx.h"

/* Always four channels out: the renderer has exactly one texture format. */
unsigned char *
ph_image_load(const char *path, int *w, int *h)
{
	int comp = 0;

	return stbi_load(path, w, h, &comp, 4);
}

void
ph_image_free(unsigned char *px)
{
	stbi_image_free(px);
}

int
ph_image_write_png(const char *path, int w, int h, const void *px)
{
	return stbi_write_png(path, w, h, 4, px, w * 4);
}
