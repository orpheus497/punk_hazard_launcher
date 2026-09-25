/*
 * text.h -- typeface rasterisation and text layout.
 *
 * The typeface is Hurmit Nerd Font Mono, baked into the executable by
 * tools/bin2c (see gen/font_*.c) and rasterised at runtime by
 * stb_truetype.  There is no fontconfig lookup, no font path, and no
 * failure mode where the UI starts with no glyphs.
 *
 * Glyphs are cached on demand rather than pre-baked.  A Nerd Font carries
 * on the order of ten thousand glyphs across the Latin ranges and the
 * Private Use Area icon blocks; rasterising all of them at three sizes on
 * startup would cost time and atlas space for icons the UI never draws.
 * Instead each code point is rasterised the first time it is asked for and
 * kept for the life of the font object.
 */
#ifndef PH_TEXT_H
#define PH_TEXT_H

#include "ph.h"
#include "gfx.h"

struct ph_font;

/* A line of wrapped text: a pointer into the original string plus a byte
 * length.  Nothing is copied. */
struct ph_line {
	const char *p;
	int         len;
};

/* `ttf` must outlive the font: the baked arrays are in .rodata, so they do. */
struct ph_font *ph_font_create(const unsigned char *ttf, size_t len, int px);
void  ph_font_destroy(struct ph_font *f);

float ph_font_height(const struct ph_font *f);   /* ascent+descent+linegap */
float ph_font_ascent(const struct ph_font *f);

float ph_text_width(struct ph_font *f, const char *s);
float ph_text_width_n(struct ph_font *f, const char *s, int len);

/* All draw calls take `y` as the TOP of the line box, not the baseline --
 * layout code thinks in boxes.  They return the advanced pen x. */
float ph_text_draw(struct ph_gfx *g, struct ph_font *f, float x, float y,
          const char *s, ph_rgba c);
float ph_text_draw_n(struct ph_gfx *g, struct ph_font *f, float x, float y,
          const char *s, int len, ph_rgba c);
/* Truncates with a single-character ellipsis if it will not fit in maxw. */
float ph_text_draw_clip(struct ph_gfx *g, struct ph_font *f, float x, float y,
          float maxw, const char *s, ph_rgba c);

int   ph_text_wrap(struct ph_font *f, const char *s, float maxw,
          struct ph_line *out, int max_lines);

/* Baked typeface, emitted by tools/bin2c into gen/. */
extern const unsigned char ph_font_regular[];
extern const unsigned long ph_font_regular_len;
extern const unsigned char ph_font_bold[];
extern const unsigned long ph_font_bold_len;

#endif /* PH_TEXT_H */
