#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "frame.h"

#include <stdlib.h>
#define SCREEN_WIDTH 80
#define SCREEN_HEIGHT 25
#define FLUSH_THRESHOLD 10
int cursor_x = 0;
int cursor_y = 0;
static int refresh_counter = 0;
Cell screen[SCREEN_HEIGHT][SCREEN_WIDTH];
DirtyRect dirty = {0,0,0,0,0};
uint16_t vga_memory[SCREEN_HEIGHT * SCREEN_WIDTH];
#define VGA_ADDR(x,y) ((y)*SCREEN_WIDTH + (x))

void init_screen() {
    for (int y = 0; y < SCREEN_HEIGHT; y++) {
        for (int x = 0; x < SCREEN_WIDTH; x++) {
            screen[y][x].ch = ' ';
            screen[y][x].attr = 0x07;
        }
    }
    cursor_x = 0;
    cursor_y = 0;
    dirty.dirty = 1;
    dirty.x1 = 0; dirty.y1 = 0;
    dirty.x2 = SCREEN_WIDTH-1; dirty.y2 = SCREEN_HEIGHT-1;
}

void scroll_up() {
    memmove(&screen[0][0], &screen[1][0], sizeof(Cell)*(SCREEN_HEIGHT-1)*SCREEN_WIDTH);
    for (int x = 0; x < SCREEN_WIDTH; x++) {
        screen[SCREEN_HEIGHT-1][x].ch = ' ';
        screen[SCREEN_HEIGHT-1][x].attr = 0x07;
    }
    dirty.dirty = 1;
    dirty.x1 = 0; dirty.y1 = 0;
    dirty.x2 = SCREEN_WIDTH-1; dirty.y2 = SCREEN_HEIGHT-1;
    if (cursor_y > 0) cursor_y--;
}

void put_char_with_attr(char c, char attr) {
    if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
        if (cursor_y >= SCREEN_HEIGHT) scroll_up();
    } else {
        screen[cursor_y][cursor_x].ch = c;
        screen[cursor_y][cursor_x].attr = attr;

        if (!dirty.dirty) {
            dirty.x1 = cursor_x; dirty.y1 = cursor_y;
            dirty.x2 = cursor_x; dirty.y2 = cursor_y;
            dirty.dirty = 1;
        } else {
            if (cursor_x < dirty.x1) dirty.x1 = cursor_x;
            if (cursor_x > dirty.x2) dirty.x2 = cursor_x;
            if (cursor_y < dirty.y1) dirty.y1 = cursor_y;
            if (cursor_y > dirty.y2) dirty.y2 = cursor_y;
        }

        cursor_x++;
        if (cursor_x >= SCREEN_WIDTH) {
            cursor_x = 0;
            cursor_y++;
            if (cursor_y >= SCREEN_HEIGHT) scroll_up();
        }
    }

    refresh_counter++;
    if (refresh_counter >= FLUSH_THRESHOLD || c == '\n') {
        render_vga_screen();
    }
}

void flush_to_vga() {
    if (!dirty.dirty) return;
    for (int y = dirty.y1; y <= dirty.y2; y++) {
        for (int x = dirty.x1; x <= dirty.x2; x++) {
            vga_memory[VGA_ADDR(x,y)] = ((uint16_t)screen[y][x].attr << 8) | (uint8_t)screen[y][x].ch;
        }
    }
    dirty.dirty = 0;
}
void clear_screen() {
    system("clear");
}
void render_vga_screen() {
    clear_screen();
    flush_to_vga();

    for (int y = 0; y < SCREEN_HEIGHT; y++) {
        for (int x = 0; x < SCREEN_WIDTH; x++) {
            putchar(vga_memory[VGA_ADDR(x, y)] & 0xFF);
        }
        putchar('\n');
    }
    fflush(stdout);
    refresh_counter = 0;
}
void render_screen_dirty() {
    if (!dirty.dirty) return;
    for (int y = dirty.y1; y <= dirty.y2; y++) {
        for (int x = dirty.x1; x <= dirty.x2; x++) {
            vga_memory[VGA_ADDR(x,y)] = ((uint16_t)screen[y][x].attr << 8) | (uint8_t)screen[y][x].ch;
        }
    }
    dirty.dirty = 0;
}
