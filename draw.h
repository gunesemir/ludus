#ifndef LUDUS_DRAW_H
#define LUDUS_DRAW_H
#include <stdint.h>

struct canvas { uint32_t *pixels; int width, height; };
void font_init(const char *pattern);
void fill(struct canvas c, int x, int y, int w, int h, uint32_t color);
/* Baseline in pixels. Clip horizontally to [left, right). */
void text(struct canvas c, int x, int baseline, int left, int right,
          int size, uint32_t color, const char *str);
int text_width(int size, const char *str);
#endif
