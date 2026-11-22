/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright © 2026 The TokTok team.
 */

/*
 * The Android app's terminal emulator. It keeps a grid of cells and understands what ncurses sends
 * for the toxic-android terminfo entry (../../../../toxic.terminfo).
 */
#ifndef TOXIC_ANDROID_VTERM_H
#define TOXIC_ANDROID_VTERM_H

#include <stddef.h>
#include <stdint.h>

/* Colour index for the terminal's default foreground or background. */
#define VTERM_DEFAULT_COLOR 256

/* The cell after a double-width character holds this instead of a character. */
#define VTERM_WIDE_TAIL 0xffffffffu

/* A cell holds a character and up to 4 combining ones after it, like ncurses' cells. */
#define VTERM_CELL_CHARS 5

/* A cell's attributes: foreground and background colour (0-256) and flags. */
#define VTERM_FG(attr) ((attr) & 0x1ffu)
#define VTERM_BG(attr) (((attr) >> 9) & 0x1ffu)
#define VTERM_BOLD (1u << 18)
#define VTERM_DIM (1u << 19)
#define VTERM_ITALIC (1u << 20)
#define VTERM_UNDERLINE (1u << 21)
#define VTERM_REVERSE (1u << 22)

/* The default colours, as 0xAARRGGBB. */
#define VTERM_DEFAULT_FG_RGB 0xffd0d0d0u
#define VTERM_DEFAULT_BG_RGB 0xff000000u

/*
 * How a cell is drawn: its colours as 0xAARRGGBB, with reverse video and the cursor swapping them
 * and dim text half transparent, and its VTERM_BOLD, VTERM_ITALIC and VTERM_UNDERLINE flags.
 */
typedef struct Vterm_Look {
    uint32_t fg;
    uint32_t bg;
    uint32_t style;
} Vterm_Look;

typedef struct Vterm Vterm;

/* Returns NULL if out of memory or rows or cols is less than 1. */
Vterm *vterm_new(int rows, int cols);
void vterm_free(Vterm *term);

/* Keeps the top left of the screen. Returns -1 if out of memory, keeping the old size. */
int vterm_resize(Vterm *term, int rows, int cols);

void vterm_write(Vterm *term, const uint8_t *data, size_t length);

int vterm_rows(const Vterm *term);
int vterm_cols(const Vterm *term);

/*
 * Returns the VTERM_CELL_CHARS characters at row and col (both 0-based): a character (or
 * VTERM_WIDE_TAIL), then its combining characters, then 0s.
 */
const uint32_t *vterm_chars(const Vterm *term, int row, int col);
uint32_t vterm_attr(const Vterm *term, int row, int col);
Vterm_Look vterm_look(const Vterm *term, int row, int col);

int vterm_cursor_row(const Vterm *term);
int vterm_cursor_col(const Vterm *term);

#endif /* TOXIC_ANDROID_VTERM_H */
