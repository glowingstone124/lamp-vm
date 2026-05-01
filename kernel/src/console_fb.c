#include "../include/kernel/console_fb.h"
#include "../include/kernel/platform.h"
#include "../include/kernel/types.h"

#define CELL_W 8
#define CELL_H 8
#define COLS ((int)(FB_WIDTH / CELL_W))
#define ROWS ((int)(FB_HEIGHT / CELL_H))

#define ASCII_MIN 32
#define ASCII_MAX 126
#define FONT_PIXELS 64
#define FONT_GLYPHS (ASCII_MAX - ASCII_MIN + 1)
#define FG_DEFAULT 0x00FFFFFFu
#define BG_DEFAULT 0x00000000u

static int g_cursor_x;
static int g_cursor_y;
static uint32_t g_fg = FG_DEFAULT;
static uint32_t g_bg = BG_DEFAULT;
static volatile uint32_t *const g_fb = (volatile uint32_t *)(uintptr_t)FB_BASE;

/*
 * Imported from toolchain example ascii_fb.c, values are 0/1 pixels.
 * Layout: glyph-major, each glyph is 8x8 = 64 entries.
 */
static int g_font[FONT_GLYPHS * FONT_PIXELS] = {
#include "console_font_8x8_data.inc"
};

static inline void fb_accel_out32(uint32_t addr, uint32_t value) {
    __asm__ volatile("out %0, %1" :: "r"(value), "r"(addr));
}

static void clear_cell(int cx, int cy) {
    int py = cy * CELL_H;
    int px = cx * CELL_W;
    for (int y = 0; y < CELL_H; y++) {
        for (int x = 0; x < CELL_W; x++) {
            g_fb[(py + y) * (int)FB_WIDTH + (px + x)] = g_bg;
        }
    }
}

static void draw_box(int cx, int cy) {
    int py = cy * CELL_H;
    int px = cx * CELL_W;
    for (int y = 0; y < CELL_H; y++) {
        for (int x = 0; x < CELL_W; x++) {
            if (y == 0 || y == (CELL_H - 1) || x == 0 || x == (CELL_W - 1)) {
                g_fb[(py + y) * (int)FB_WIDTH + (px + x)] = g_fg;
            } else {
                g_fb[(py + y) * (int)FB_WIDTH + (px + x)] = g_bg;
            }
        }
    }
}

static void draw_char(int ch, int cx, int cy) {
    int py = cy * CELL_H;
    int px = cx * CELL_W;
    if (ch < ASCII_MIN || ch > ASCII_MAX) {
        draw_box(cx, cy);
        return;
    }

    int glyph_base = (ch - ASCII_MIN) * FONT_PIXELS;
    for (int row = 0; row < CELL_H; row++) {
        int row_base = glyph_base + row * CELL_W;
        for (int col = 0; col < CELL_W; col++) {
            int v = g_font[row_base + col];
            if (v) {
                g_fb[(py + row) * (int)FB_WIDTH + (px + col)] = g_fg;
            } else {
                g_fb[(py + row) * (int)FB_WIDTH + (px + col)] = g_bg;
            }
        }
    }
}

static void scroll_one_line(void) {
    fb_accel_out32(IO_FB_ACCEL_ARG0, g_bg);
    fb_accel_out32(IO_FB_ACCEL_CMD, FB_ACCEL_CMD_SCROLL_UP_8PX);
}

static void newline(void) {
    g_cursor_x = 0;
    g_cursor_y++;
    if (g_cursor_y >= ROWS) {
        scroll_one_line();
        g_cursor_y = ROWS - 1;
    }
}

void console_fb_clear(void) {
    fb_accel_out32(IO_FB_ACCEL_ARG0, g_bg);
    fb_accel_out32(IO_FB_ACCEL_CMD, FB_ACCEL_CMD_CLEAR);
    g_cursor_x = 0;
    g_cursor_y = 0;
}

void console_fb_putc(uint32_t c) {
    if (c == (uint32_t)'\a') {
        return;
    }
    if (c == (uint32_t)'\n') {
        newline();
        return;
    }
    if (c == (uint32_t)'\v') {
        newline();
        return;
    }
    if (c == (uint32_t)'\f') {
        console_fb_clear();
        return;
    }
    if (c == (uint32_t)'\b') {
        if (g_cursor_x > 0) {
            g_cursor_x--;
        } else if (g_cursor_y > 0) {
            g_cursor_y--;
            g_cursor_x = COLS - 1;
        } else {
            return;
        }
        clear_cell(g_cursor_x, g_cursor_y);
        return;
    }
    if (c == (uint32_t)'\r') {
        g_cursor_x = 0;
        return;
    }
    if (c == (uint32_t)'\t') {
        for (int i = 0; i < 4; i++) {
            console_fb_putc((uint32_t)' ');
        }
        return;
    }
    if (c == 0u) {
        clear_cell(g_cursor_x, g_cursor_y);
    } else {
        draw_char((int)c, g_cursor_x, g_cursor_y);
    }

    g_cursor_x++;
    if (g_cursor_x >= COLS) {
        newline();
    }
}

void console_fb_init(void) {
    g_fg = FG_DEFAULT;
    g_bg = BG_DEFAULT;
    g_cursor_x = 0;
    g_cursor_y = 0;
    console_fb_clear();
}

void console_fb_set_colors(uint32_t fg, uint32_t bg) {
    g_fg = fg;
    g_bg = bg;
}

void console_fb_puts(const char *s) {
    if (!s) {
        return;
    }
    const uint8_t *p = (const uint8_t *)s;
    while (*p != 0u) {
        console_fb_putc((uint32_t)*p);
        p++;
    }
}
