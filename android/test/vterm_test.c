/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright © 2026 The TokTok team.
 */

/*
 * Draws at random with ncurses for the toxic-android terminal, feeds what ncurses sends to
 * the emulator and checks that every cell matches what ncurses thinks is on the screen.
 *
 * Usage: TERMINFO=<dir with the compiled entry> vterm_test [seed]
 */
#define _XOPEN_SOURCE_EXTENDED 1
#define _XOPEN_SOURCE 700

#include <curses.h>
#include <locale.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../app/src/main/c/vterm.h"

#define ROWS 24
#define COLS 80
#define STEPS 20000
#define PAIRS 64

#define SIDE_ROWS (ROWS - 4)
#define SIDE_COLS 30
#define SIDE_Y 2
#define SIDE_X (COLS - SIDE_COLS)

static FILE *output;
static Vterm *term;
static int failures;

/* Maps an ACS line-drawing character (l, q, k, ...) to the Unicode one ncurses sends with U8#1. */
static uint32_t line_char(wchar_t ch)
{
    static const char from[] = "lqkxjmtuvwn";
    static const uint32_t to[] = {0x250c, 0x2500, 0x2510, 0x2502, 0x2518, 0x2514,
                                  0x251c, 0x2524, 0x2534, 0x252c, 0x253c
                                 };
    const char *found = ch > 0 && ch < 0x80 ? strchr(from, (int)ch) : NULL;
    return found != NULL ? to[found - from] : (uint32_t)ch;
}

/* Feeds the emulator what ncurses wrote since the last call. */
static void flush_output(void)
{
    static long done = 0;
    fflush(output);
    const long end = ftell(output);
    uint8_t buf[4096];
    fseek(output, done, SEEK_SET);

    while (done < end) {
        const size_t want = (size_t)(end - done) < sizeof(buf) ? (size_t)(end - done) : sizeof(buf);
        const size_t got = fread(buf, 1, want, output);
        vterm_write(term, buf, got);
        done += (long)got;
    }

    fseek(output, end, SEEK_SET);
}

static uint32_t expected_attr(attr_t attrs, int pair)
{
    int fg;
    int bg;
    extended_pair_content(pair, &fg, &bg);
    uint32_t attr = (uint32_t)(fg < 0 ? VTERM_DEFAULT_COLOR : fg)
                    | ((uint32_t)(bg < 0 ? VTERM_DEFAULT_COLOR : bg) << 9);
    attr |= (attrs & A_BOLD) ? VTERM_BOLD : 0;
    attr |= (attrs & A_DIM) ? VTERM_DIM : 0;
    attr |= (attrs & A_ITALIC) ? VTERM_ITALIC : 0;
    attr |= (attrs & A_UNDERLINE) ? VTERM_UNDERLINE : 0;
    attr |= (attrs & A_REVERSE) ? VTERM_REVERSE : 0;
    return attr;
}

static void compare(int step)
{
    flush_output();

    for (int row = 0; row < ROWS; ++row) {
        for (int col = 0; col < COLS; ++col) {
            cchar_t cell;
            wchar_t wch[CCHARW_MAX + 1];
            attr_t attrs;
            short short_pair;
            int pair = 0;
            mvwin_wch(curscr, row, col, &cell);
            getcchar(&cell, wch, &attrs, &short_pair, &pair);

            const uint32_t want = (attrs & A_ALTCHARSET) ? line_char(wch[0]) : (uint32_t)wch[0];
            // Ignore bold, dim and italic on a blank, like ncurses does: they don't show.
            const uint32_t hidden = want == ' ' ? VTERM_BOLD | VTERM_DIM | VTERM_ITALIC : 0;
            const uint32_t want_attr = expected_attr(attrs & ~A_ALTCHARSET, pair) & ~hidden;
            const uint32_t *got = vterm_chars(term, row, col);
            const uint32_t got_attr = vterm_attr(term, row, col) & ~hidden;
            bool same = got[0] == want && got_attr == want_attr;

            // Then the combining characters, up to getcchar's 0.
            bool more = true;

            for (int i = 1; i < VTERM_CELL_CHARS; ++i) {
                more = more && i < CCHARW_MAX && wch[i] != 0;
                same = same && got[i] == (more ? (uint32_t)wch[i] : 0);
            }

            if (!same && ++failures <= 10) {
                fprintf(stderr, "step %d, cell %d,%d: want U+%04X U+%04X attr %06x, got U+%04X U+%04X attr %06x\n",
                        step, row, col, want, (uint32_t)wch[1], want_attr, got[0], got[1], got_attr);
            }

            if (wcwidth(wch[0]) == 2) {
                ++col;
            }
        }
    }

    int row;
    int col;
    getyx(newscr, row, col);

    if ((vterm_cursor_row(term) != row || vterm_cursor_col(term) != col) && ++failures <= 10) {
        fprintf(stderr, "step %d: cursor want %d,%d, got %d,%d\n", step, row, col, vterm_cursor_row(term),
                vterm_cursor_col(term));
    }
}

/*
 * Whether no double-width character in the window is cut in half. When something overwrites half
 * of one, ncurses keeps both halves but terminals blank the other half, so ncurses and the screen
 * can be a column apart on that row. That's an ncurses problem, so the test avoids it. ncurses
 * marks the first half with 1 in attr's A_CHARTEXT bits and the second half with 2.
 */
static bool is_whole(WINDOW *win)
{
    for (int row = 0; row < getmaxy(win); ++row) {
        bool expect_second_half = false;

        for (int col = 0; col < getmaxx(win); ++col) {
            cchar_t cell;
            mvwin_wch(win, row, col, &cell);
            const int half = (int)(cell.attr & A_CHARTEXT);

            if ((half >= 2) != expect_second_half) {
                return false;
            }

            expect_second_half = half == 1;
        }

        if (expect_second_half) {
            return false;
        }
    }

    return true;
}

static const wchar_t *const words[] = {
    L"hello", L"toxic", L" ", L"é", L"中文", L"😀", L"Ωmega", L"tab\there", L"x",
    L"cafe\u0301", L"\u0e2a\u0e27\u0e31\u0e2a\u0e14\u0e35", L"\U0001f468\u200d\U0001f469",
    L"a\u0301\u0302\u0303\u0304\u0305",
    L"a long line of text that may well wrap around the edge of the window",
};

/* Draws one random step. The same seed gives the same steps. */
static void random_step(WINDOW *win, unsigned seed)
{
    static const attr_t attrs[] = {A_NORMAL, A_BOLD, A_DIM, A_ITALIC, A_UNDERLINE, A_REVERSE};
    const int rows = getmaxy(win);
    const int cols = getmaxx(win);
    const int row = rand_r(&seed) % rows;
    const int col = rand_r(&seed) % cols;

    switch (rand_r(&seed) % 11) {
        case 0:
        case 1:
        case 2:
            mvwaddwstr(win, row, col, words[rand_r(&seed) % (sizeof(words) / sizeof(words[0]))]);
            break;

        case 3:
            wattr_set(win, attrs[rand_r(&seed) % 6] | attrs[rand_r(&seed) % 6], (short)(rand_r(&seed) % PAIRS),
                      NULL);
            break;

        case 4:
            wmove(win, row, col);
            wclrtoeol(win);
            break;

        case 5:
            wmove(win, row, col);
            wclrtobot(win);
            break;

        case 6:
            if (rand_r(&seed) % 8 == 0) {
                werase(win);
            }

            break;

        case 7:
            wsetscrreg(win, row, row + rand_r(&seed) % (rows - row));
            wscrl(win, rand_r(&seed) % 3 - 1);
            wsetscrreg(win, 0, rows - 1);
            break;

        case 8:
            box(win, 0, 0);
            break;

        case 9:
            mvwhline(win, row, 0, ACS_HLINE, col);
            break;

        default:
            wbkgdset(win, ' ' | COLOR_PAIR(rand_r(&seed) % PAIRS));
            break;
    }
}

int main(int argc, char **argv)
{
    const unsigned seed = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 1;
    srand(seed);
    setlocale(LC_ALL, "C.UTF-8");

    output = tmpfile();
    FILE *input = fopen("/dev/null", "r");
    term = vterm_new(ROWS, COLS);

    if (output == NULL || input == NULL || term == NULL) {
        fprintf(stderr, "setup failed\n");
        return 2;
    }

    SCREEN *screen = newterm("toxic-android", output, input);

    if (screen == NULL) {
        fprintf(stderr, "no toxic-android terminfo entry (set TERMINFO)\n");
        return 2;
    }

    start_color();
    use_default_colors();

    for (int pair = 1; pair < PAIRS; ++pair) {
        init_extended_pair(pair, rand() % 257 - 1, rand() % 257 - 1);
    }

    // A side panel inside the main window, like toxic's.
    WINDOW *side = derwin(stdscr, SIDE_ROWS, SIDE_COLS, SIDE_Y, SIDE_X);
    scrollok(stdscr, TRUE);
    scrollok(side, TRUE);
    syncok(side, TRUE);
    int left_out = 0;

    for (int step = 0; step < STEPS && failures == 0; ++step) {
        const bool on_side = rand() % 3 == 0;
        const unsigned step_seed = (unsigned)rand();

        // Try the step on a copy first, and leave it out if it cuts a character in half.
        WINDOW *copy = dupwin(stdscr);
        WINDOW *copy_side = derwin(copy, SIDE_ROWS, SIDE_COLS, SIDE_Y, SIDE_X);
        scrollok(copy, TRUE);
        scrollok(copy_side, TRUE);
        random_step(on_side ? copy_side : copy, step_seed);
        const bool whole = is_whole(copy);
        delwin(copy_side);
        delwin(copy);

        if (whole) {
            random_step(on_side ? side : stdscr, step_seed);
        } else {
            ++left_out;
        }

        if (rand() % 4 == 0) {
            wnoutrefresh(stdscr);
            doupdate();
            compare(step);
        }
    }

    endwin();
    delscreen(screen);
    vterm_free(term);

    if (failures > 0) {
        fprintf(stderr, "seed %u: %d mismatches\n", seed, failures);
        return 1;
    }

    printf("seed %u: %d steps match (%d left out)\n", seed, STEPS, left_out);
    return 0;
}
