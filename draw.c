#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include "draw.h"

static FT_Library library;
static FT_Face face;

void font_init(const char *pattern)
{
	FcPattern *request = FcNameParse((const FcChar8 *)pattern), *match;
	FcResult result;
	FcChar8 *file;
	int index = 0;
	if (!request || !FcConfigSubstitute(NULL, request, FcMatchPattern))
		goto failed;
	FcDefaultSubstitute(request);
	match = FcFontMatch(NULL, request, &result);
	FcPatternDestroy(request);
	if (!match || FcPatternGetString(match, FC_FILE, 0, &file) != FcResultMatch)
		goto failed;
	FcPatternGetInteger(match, FC_INDEX, 0, &index);
	if (FT_Init_FreeType(&library) || FT_New_Face(library, (char *)file, index, &face))
		goto failed;
	FcPatternDestroy(match);
	return;
failed:
	fprintf(stderr, "ludus: cannot load font matching '%s'\n", pattern);
	exit(EXIT_FAILURE);
}

void font_finish(void)
{
	FT_Done_Face(face);
	FT_Done_FreeType(library);
	FcFini();
}

void fill(struct canvas c, int x, int y, int w, int h, uint32_t color)
{
	int i, j;
	for (j = y < 0 ? 0 : y; j < y + h && j < c.height; ++j)
		for (i = x < 0 ? 0 : x; i < x + w && i < c.width; ++i)
			c.pixels[(size_t)j * c.width + i] = color;
}

static uint32_t blend(uint32_t bg, uint32_t fg, unsigned alpha)
{
	uint32_t result = 0;
	int shift;
	for (shift = 0; shift <= 16; shift += 8) {
		unsigned a = (bg >> shift) & 255, b = (fg >> shift) & 255;
		result |= ((a * (255 - alpha) + b * alpha) / 255) << shift;
	}
	return result;
}

static int layout(struct canvas c, int x, int baseline, int left, int right,
                  int size, uint32_t color, const char *s, size_t remaining)
{
	if (FT_Set_Pixel_Sizes(face, 0, size)) return 0;
	while (remaining && (!c.pixels || x < right)) {
		FcChar32 cp;
		FT_GlyphSlot glyph = face->glyph;
		int n = FcUtf8ToUcs4((const FcChar8 *)s, &cp, remaining > 4 ? 4 : (int)remaining);
		if (n < 1) { cp = 0xfffd; n = 1; }
		s += n;
		remaining -= n;
		if (FT_Load_Char(face, cp, FT_LOAD_NO_BITMAP)) continue;
		if (c.pixels && !FT_Render_Glyph(glyph, FT_RENDER_MODE_NORMAL)) {
			FT_Bitmap *bm = &glyph->bitmap;
			for (unsigned row = 0; row < bm->rows; ++row) {
				int y = baseline - glyph->bitmap_top + (int)row;
				if (y < 0 || y >= c.height) continue;
				for (unsigned col = 0; col < bm->width; ++col) {
					int px = x + glyph->bitmap_left + (int)col;
					if (px < left || px < 0 || px >= right || px >= c.width) continue;
					uint32_t *dst = c.pixels + (size_t)y * c.width + px;
					*dst = blend(*dst, color, bm->buffer[row * bm->pitch + col]);
				}
			}
		}
		x += glyph->advance.x >> 6;
	}
	return x;
}

void text(struct canvas c, int x, int baseline, int left, int right,
          int size, uint32_t color, const char *s)
{
	layout(c, x, baseline, left, right, size, color, s, strlen(s));
}
int text_width(int size, const char *s)
{
	return layout((struct canvas){0}, 0, 0, 0, 0, size, 0, s, strlen(s));
}

/* Measure a UTF-8 prefix in the current face. Always break between glyphs. */
static size_t fit_prefix(const char *s, size_t len, int width)
{
	size_t pos = 0;
	while (pos < len) {
		FcChar32 cp;
		int n = FcUtf8ToUcs4((const FcChar8 *)s + pos, &cp, len - pos > 4 ? 4 : (int)(len - pos));
		if (n < 1) { cp = 0xfffd; n = 1; }
		if (!FT_Load_Char(face, cp, FT_LOAD_NO_BITMAP)) width -= face->glyph->advance.x >> 6;
		if (width < 0) break;
		pos += n;
	}
	return pos;
}

void wrapped_title(struct canvas c, int x, int baseline, int width, int size,
                   int lines, uint32_t color, const char *s)
{
	size_t len = strlen(s);
	if (FT_Set_Pixel_Sizes(face, 0, size)) return;
	for (int line = 0; line < lines && len; ++line) {
		while (len && *s == ' ') { ++s; --len; }
		size_t fit = fit_prefix(s, len, width), take = fit;
		int ellipsis = fit < len && line == lines - 1;
		if (ellipsis) {
			take = fit_prefix(s, len, width - text_width(size, "…"));
		} else if (fit < len && s[fit] != ' ') {
			while (take && s[take - 1] != ' ') --take;
			if (!take) take = fit;
		}
		size_t ink = take;
		while (ink && s[ink - 1] == ' ') --ink;
		int end = layout(c, x, baseline, x, x + width, size, color, s, ink);
		if (ellipsis) text(c, end, baseline, x, x + width, size, color, "…");
		if (!take || ellipsis) break;
		s += take;
		len -= take;
		baseline += size + 3;
	}
}
