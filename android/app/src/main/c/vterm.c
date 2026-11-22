/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright © 2026 The TokTok team.
 */

#define _XOPEN_SOURCE 700

#include "vterm.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define MAX_PARAMS 16
#define TAB_WIDTH 8
#define REPLACEMENT_CHAR 0xfffdu

#define DEFAULT_ATTR (VTERM_DEFAULT_COLOR | (VTERM_DEFAULT_COLOR << 9))

typedef enum Parser_State {
    STATE_GROUND,
    STATE_ESCAPE,
    STATE_CHARSET,
    STATE_CSI,
} Parser_State;

typedef struct Cell {
    uint32_t ch[VTERM_CELL_CHARS];
    uint32_t attr;
} Cell;

struct Vterm {
    int rows;
    int cols;
    Cell *cells;

    int row;
    int col;
    /* After writing the last column, the next character goes on the next line (xenl). */
    bool wrap_pending;
    /* Whether writing the last column wraps (DECAWM). ncurses turns it off for the last cell. */
    bool autowrap;
    bool cursor_visible;
    /* The scrolling region, both rows included. */
    int top;
    int bottom;
    uint32_t attr;
    /* Position of the last character written, so combining characters can attach to it. last_col
     * is -1 if anything else came after it. */
    int last_row;
    int last_col;

    Parser_State state;
    int params[MAX_PARAMS];
    int param_count;
    bool private_mode;

    uint32_t utf8_char;
    int utf8_left;
};

static Cell *cell_at(const Vterm *term, int row, int col)
{
    return &term->cells[row * term->cols + col];
}

/* Returns an erased cell: blank, in the current colours (bce). */
static Cell blank_cell(const Vterm *term)
{
    const Cell cell = {{' '}, term->attr & 0x3ffffu};
    return cell;
}

static void erase(Vterm *term, int row, int from_col, int to_col)
{
    const Cell blank = blank_cell(term);

    for (int col = from_col; col < to_col; ++col) {
        *cell_at(term, row, col) = blank;
    }
}

static void erase_rows(Vterm *term, int from_row, int to_row)
{
    for (int row = from_row; row < to_row; ++row) {
        erase(term, row, 0, term->cols);
    }
}

static void scroll_up(Vterm *term)
{
    memmove(cell_at(term, term->top, 0), cell_at(term, term->top + 1, 0),
            (size_t)(term->bottom - term->top) * (size_t)term->cols * sizeof(Cell));
    erase(term, term->bottom, 0, term->cols);
}

static void scroll_down(Vterm *term)
{
    memmove(cell_at(term, term->top + 1, 0), cell_at(term, term->top, 0),
            (size_t)(term->bottom - term->top) * (size_t)term->cols * sizeof(Cell));
    erase(term, term->top, 0, term->cols);
}

static void line_feed(Vterm *term)
{
    term->wrap_pending = false;

    if (term->row == term->bottom) {
        scroll_up(term);
    } else if (term->row < term->rows - 1) {
        ++term->row;
    }
}

static void reverse_line_feed(Vterm *term)
{
    term->wrap_pending = false;

    if (term->row == term->top) {
        scroll_down(term);
    } else if (term->row > 0) {
        --term->row;
    }
}

static int clamp(int value, int low, int high)
{
    return value < low ? low : value > high ? high : value;
}

static void move_to(Vterm *term, int row, int col)
{
    term->row = clamp(row, 0, term->rows - 1);
    term->col = clamp(col, 0, term->cols - 1);
    term->wrap_pending = false;
}

static void set_char(Cell *cell, uint32_t ch)
{
    memset(cell->ch, 0, sizeof(cell->ch));
    cell->ch[0] = ch;
}

/* Clears the half of a double-width character that a write at col leaves behind. */
static void break_wide_char(Vterm *term, int col)
{
    if (cell_at(term, term->row, col)->ch[0] == VTERM_WIDE_TAIL && col > 0) {
        set_char(cell_at(term, term->row, col - 1), ' ');
    }

    if (col + 1 < term->cols && cell_at(term, term->row, col + 1)->ch[0] == VTERM_WIDE_TAIL) {
        set_char(cell_at(term, term->row, col + 1), ' ');
    }
}

/* Adds a combining character to the last character written, if it came right before. */
static void add_combining(Vterm *term, uint32_t ch)
{
    if (term->last_col < 0) {
        return;
    }

    uint32_t *chars = cell_at(term, term->last_row, term->last_col)->ch;

    for (int i = 1; i < VTERM_CELL_CHARS; ++i) {
        if (chars[i] == 0) {
            chars[i] = ch;
            return;
        }
    }
}

static void put_char(Vterm *term, uint32_t ch)
{
    int width = wcwidth((wchar_t)ch);

    if (width == 0) {
        add_combining(term, ch);
        return;
    }

    if (width < 0 || width > term->cols) {
        ch = REPLACEMENT_CHAR;
        width = 1;
    }

    if (term->wrap_pending || term->col + width > term->cols) {
        if (term->autowrap) {
            term->col = 0;
            line_feed(term);
        } else {
            term->col = term->cols - width;
        }
    }

    break_wide_char(term, term->col);
    Cell *cell = cell_at(term, term->row, term->col);
    set_char(cell, ch);
    cell->attr = term->attr;
    term->last_row = term->row;
    term->last_col = term->col;

    if (width == 2) {
        break_wide_char(term, term->col + 1);
        set_char(&cell[1], VTERM_WIDE_TAIL);
        cell[1].attr = term->attr;
    }

    term->col += width;

    if (term->col == term->cols) {
        term->col = term->cols - 1;
        term->wrap_pending = term->autowrap;
    }
}

static void control(Vterm *term, uint8_t byte)
{
    term->last_col = -1;

    switch (byte) {
        case '\b':
            if (term->col > 0) {
                --term->col;
            }

            term->wrap_pending = false;
            break;

        case '\t':
            term->col = clamp((term->col / TAB_WIDTH + 1) * TAB_WIDTH, 0, term->cols - 1);
            term->wrap_pending = false;
            break;

        case '\n':
        case '\v':
        case '\f':
            line_feed(term);
            break;

        case '\r':
            term->col = 0;
            term->wrap_pending = false;
            break;

        case 0x1b:
            term->state = STATE_ESCAPE;
            break;

        default:
            break;  // The bell and everything else is ignored.
    }
}

/* Returns the nth parameter, or fallback if it's missing or 0. */
static int param(const Vterm *term, int n, int fallback)
{
    return n < term->param_count && term->params[n] != 0 ? term->params[n] : fallback;
}

static uint32_t with_fg(uint32_t attr, uint32_t color)
{
    return (attr & ~0x1ffu) | color;
}

static uint32_t with_bg(uint32_t attr, uint32_t color)
{
    return (attr & ~(0x1ffu << 9)) | (color << 9);
}

static void select_graphic_rendition(Vterm *term)
{
    if (term->param_count == 0) {
        term->attr = DEFAULT_ATTR;
        return;
    }

    for (int i = 0; i < term->param_count; ++i) {
        const int p = term->params[i];

        if ((p == 38 || p == 48) && i + 2 < term->param_count && term->params[i + 1] == 5) {
            const uint32_t color = (uint32_t)clamp(term->params[i + 2], 0, 255);
            term->attr = p == 38 ? with_fg(term->attr, color) : with_bg(term->attr, color);
            i += 2;
        } else if (p == 0) {
            term->attr = DEFAULT_ATTR;
        } else if (p == 1) {
            term->attr |= VTERM_BOLD;
        } else if (p == 2) {
            term->attr |= VTERM_DIM;
        } else if (p == 3) {
            term->attr |= VTERM_ITALIC;
        } else if (p == 4) {
            term->attr |= VTERM_UNDERLINE;
        } else if (p == 7) {
            term->attr |= VTERM_REVERSE;
        } else if (p == 22) {
            term->attr &= ~(VTERM_BOLD | VTERM_DIM);
        } else if (p == 23) {
            term->attr &= ~VTERM_ITALIC;
        } else if (p == 24) {
            term->attr &= ~VTERM_UNDERLINE;
        } else if (p == 27) {
            term->attr &= ~VTERM_REVERSE;
        } else if (p >= 30 && p <= 37) {
            term->attr = with_fg(term->attr, (uint32_t)(p - 30));
        } else if (p == 39) {
            term->attr = with_fg(term->attr, VTERM_DEFAULT_COLOR);
        } else if (p >= 40 && p <= 47) {
            term->attr = with_bg(term->attr, (uint32_t)(p - 40));
        } else if (p == 49) {
            term->attr = with_bg(term->attr, VTERM_DEFAULT_COLOR);
        } else if (p >= 90 && p <= 97) {
            term->attr = with_fg(term->attr, (uint32_t)(p - 90 + 8));
        } else if (p >= 100 && p <= 107) {
            term->attr = with_bg(term->attr, (uint32_t)(p - 100 + 8));
        }
    }
}

static void csi_dispatch(Vterm *term, uint8_t final)
{
    if (term->private_mode) {
        if (final == 'h' || final == 'l') {
            const bool set = final == 'h';

            for (int i = 0; i < term->param_count; ++i) {
                if (term->params[i] == 7) {
                    term->autowrap = set;
                } else if (term->params[i] == 25) {
                    term->cursor_visible = set;
                }
            }
        }

        return;
    }

    switch (final) {
        case 'A':
            move_to(term, term->row - param(term, 0, 1), term->col);
            break;

        case 'B':
            move_to(term, term->row + param(term, 0, 1), term->col);
            break;

        case 'C':
            move_to(term, term->row, term->col + param(term, 0, 1));
            break;

        case 'D':
            move_to(term, term->row, term->col - param(term, 0, 1));
            break;

        case 'G':
            move_to(term, term->row, param(term, 0, 1) - 1);
            break;

        case 'd':
            move_to(term, param(term, 0, 1) - 1, term->col);
            break;

        case 'H':
        case 'f':
            move_to(term, param(term, 0, 1) - 1, param(term, 1, 1) - 1);
            break;

        case 'J':
            switch (param(term, 0, 0)) {
                case 0:
                    erase(term, term->row, term->col, term->cols);
                    erase_rows(term, term->row + 1, term->rows);
                    break;

                case 1:
                    erase_rows(term, 0, term->row);
                    erase(term, term->row, 0, term->col + 1);
                    break;

                default:
                    erase_rows(term, 0, term->rows);
                    break;
            }

            break;

        case 'K':
            switch (param(term, 0, 0)) {
                case 0:
                    erase(term, term->row, term->col, term->cols);
                    break;

                case 1:
                    erase(term, term->row, 0, term->col + 1);
                    break;

                default:
                    erase(term, term->row, 0, term->cols);
                    break;
            }

            break;

        case 'X':
            erase(term, term->row, term->col, clamp(term->col + param(term, 0, 1), 0, term->cols));
            break;

        case 'r': {
            const int top = param(term, 0, 1) - 1;
            const int bottom = param(term, 1, term->rows) - 1;

            if (top < bottom && bottom < term->rows) {
                term->top = top;
                term->bottom = bottom;
            } else {
                term->top = 0;
                term->bottom = term->rows - 1;
            }

            move_to(term, 0, 0);
            break;
        }

        case 'm':
            select_graphic_rendition(term);
            break;

        default:
            break;
    }
}

static void escape_dispatch(Vterm *term, uint8_t byte)
{
    term->state = STATE_GROUND;

    switch (byte) {
        case '[':
            term->state = STATE_CSI;
            term->param_count = 0;
            term->private_mode = false;
            memset(term->params, 0, sizeof(term->params));
            break;

        case '(':
        case ')':
            term->state = STATE_CHARSET;
            break;

        case 'D':
            line_feed(term);
            break;

        case 'E':
            term->col = 0;
            line_feed(term);
            break;

        case 'M':
            reverse_line_feed(term);
            break;

        default:
            break;
    }
}

static void csi_byte(Vterm *term, uint8_t byte)
{
    if (byte >= '0' && byte <= '9') {
        if (term->param_count == 0) {
            term->param_count = 1;
        }

        int *p = &term->params[term->param_count - 1];

        if (*p < 10000) {
            *p = *p * 10 + (byte - '0');
        }
    } else if (byte == ';') {
        if (term->param_count == 0) {
            term->param_count = 1;
        }

        if (term->param_count < MAX_PARAMS) {
            ++term->param_count;
        }
    } else if (byte == '?' && term->param_count == 0) {
        term->private_mode = true;
    } else if (byte >= 0x40 && byte <= 0x7e) {
        term->state = STATE_GROUND;
        csi_dispatch(term, byte);
    } else if (byte < 0x20) {
        control(term, byte);
    }
}

static void ground_byte(Vterm *term, uint8_t byte)
{
    if (term->utf8_left > 0) {
        if ((byte & 0xc0) == 0x80) {
            term->utf8_char = (term->utf8_char << 6) | (byte & 0x3fu);

            if (--term->utf8_left == 0) {
                const uint32_t ch = term->utf8_char;
                // Surrogates and what's past U+10FFFF aren't characters.
                put_char(term, ch > 0x10ffff || (ch >= 0xd800 && ch <= 0xdfff) ? REPLACEMENT_CHAR : ch);
            }

            return;
        }

        term->utf8_left = 0;
        put_char(term, REPLACEMENT_CHAR);
    }

    if (byte < 0x20) {
        control(term, byte);
    } else if (byte < 0x7f) {
        put_char(term, byte);
    } else if ((byte & 0xe0) == 0xc0) {
        term->utf8_char = byte & 0x1fu;
        term->utf8_left = 1;
    } else if ((byte & 0xf0) == 0xe0) {
        term->utf8_char = byte & 0x0fu;
        term->utf8_left = 2;
    } else if ((byte & 0xf8) == 0xf0) {
        term->utf8_char = byte & 0x07u;
        term->utf8_left = 3;
    } else if (byte != 0x7f) {
        put_char(term, REPLACEMENT_CHAR);
    }
}

void vterm_write(Vterm *term, const uint8_t *data, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        const uint8_t byte = data[i];

        switch (term->state) {
            case STATE_GROUND:
                ground_byte(term, byte);
                break;

            case STATE_ESCAPE:
                escape_dispatch(term, byte);
                break;

            case STATE_CHARSET:
                term->state = STATE_GROUND;
                break;

            case STATE_CSI:
                csi_byte(term, byte);
                break;
        }
    }
}

static Cell *new_cells(int rows, int cols)
{
    Cell *cells = malloc((size_t)rows * (size_t)cols * sizeof(Cell));

    if (cells != NULL) {
        for (int i = 0; i < rows * cols; ++i) {
            set_char(&cells[i], ' ');
            cells[i].attr = DEFAULT_ATTR;
        }
    }

    return cells;
}

Vterm *vterm_new(int rows, int cols)
{
    if (rows < 1 || cols < 1) {
        return NULL;
    }

    Vterm *term = calloc(1, sizeof(Vterm));

    if (term == NULL) {
        return NULL;
    }

    term->cells = new_cells(rows, cols);

    if (term->cells == NULL) {
        free(term);
        return NULL;
    }

    term->rows = rows;
    term->cols = cols;
    term->bottom = rows - 1;
    term->attr = DEFAULT_ATTR;
    term->autowrap = true;
    term->cursor_visible = true;
    term->last_col = -1;
    return term;
}

void vterm_free(Vterm *term)
{
    if (term != NULL) {
        free(term->cells);
        free(term);
    }
}

int vterm_resize(Vterm *term, int rows, int cols)
{
    if (rows < 1 || cols < 1) {
        return -1;
    }

    Cell *cells = new_cells(rows, cols);

    if (cells == NULL) {
        return -1;
    }

    for (int row = 0; row < rows && row < term->rows; ++row) {
        for (int col = 0; col < cols && col < term->cols; ++col) {
            cells[row * cols + col] = *cell_at(term, row, col);
        }

        // A double-width character cut in half becomes a blank.
        if (cols < term->cols && cell_at(term, row, cols)->ch[0] == VTERM_WIDE_TAIL) {
            set_char(&cells[row * cols + cols - 1], ' ');
        }
    }

    free(term->cells);
    term->cells = cells;
    term->rows = rows;
    term->cols = cols;
    term->top = 0;
    term->bottom = rows - 1;
    term->last_col = -1;
    move_to(term, term->row, term->col);
    return 0;
}

int vterm_rows(const Vterm *term)
{
    return term->rows;
}

int vterm_cols(const Vterm *term)
{
    return term->cols;
}

const uint32_t *vterm_chars(const Vterm *term, int row, int col)
{
    return cell_at(term, row, col)->ch;
}

uint32_t vterm_attr(const Vterm *term, int row, int col)
{
    return cell_at(term, row, col)->attr;
}

/* Returns xterm's colour index as 0xAARRGGBB, or fallback for the default colour. */
static uint32_t rgb(uint32_t index, uint32_t fallback)
{
    static const uint32_t basic[16] = {
        0x000000, 0xcd0000, 0x00cd00, 0xcdcd00, 0x0000ee, 0xcd00cd, 0x00cdcd, 0xe5e5e5,
        0x7f7f7f, 0xff0000, 0x00ff00, 0xffff00, 0x5c5cff, 0xff00ff, 0x00ffff, 0xffffff,
    };
    static const uint32_t levels[6] = {0, 95, 135, 175, 215, 255};

    if (index >= VTERM_DEFAULT_COLOR) {
        return fallback;
    }

    if (index < 16) {
        return 0xff000000u | basic[index];
    }

    if (index < 232) {
        const uint32_t i = index - 16;
        return 0xff000000u | levels[i / 36] << 16 | levels[i / 6 % 6] << 8 | levels[i % 6];
    }

    const uint32_t gray = 8 + 10 * (index - 232);
    return 0xff000000u | gray << 16 | gray << 8 | gray;
}

Vterm_Look vterm_look(const Vterm *term, int row, int col)
{
    const uint32_t attr = cell_at(term, row, col)->attr;
    const bool at_cursor = term->cursor_visible && row == term->row && col == term->col;
    const bool reversed = ((attr & VTERM_REVERSE) != 0) != at_cursor;
    Vterm_Look look = {
        reversed ? rgb(VTERM_BG(attr), VTERM_DEFAULT_BG_RGB) : rgb(VTERM_FG(attr), VTERM_DEFAULT_FG_RGB),
        reversed ? rgb(VTERM_FG(attr), VTERM_DEFAULT_FG_RGB) : rgb(VTERM_BG(attr), VTERM_DEFAULT_BG_RGB),
        attr & (VTERM_BOLD | VTERM_ITALIC | VTERM_UNDERLINE),
    };

    if ((attr & VTERM_DIM) != 0) {
        look.fg = (look.fg & 0x00ffffffu) | 0x99000000u;
    }

    return look;
}

int vterm_cursor_row(const Vterm *term)
{
    return term->row;
}

int vterm_cursor_col(const Vterm *term)
{
    return term->col;
}
