#include<stdint.h>
#include<stdio.h>
#include "frame.h"

#define SCREEN_WIDTH 80
#define SCREEN_HEIGHT 25

Cell screen[SCREEN_HEIGHT][SCREEN_WIDTH];
int cursor_x = 0;
int cursor_y = 0;

void init_screen() {
    for (int y = 0; y < SCREEN_HEIGHT; y++) {
        for (int x = 0; x < SCREEN_WIDTH; x++) {
            screen[y][x].ch = ' ';
            screen[y][x].attr = 0x07;
        }
    }
    cursor_x = 0;
    cursor_y = 0;
}

void put_char_with_attr(char c, char attr) {
    if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
        if (cursor_y >= SCREEN_HEIGHT) cursor_y = 0;
        return;
    }
    screen[cursor_y][cursor_x].ch = c;
    screen[cursor_y][cursor_x].attr = attr;
    cursor_x++;
    if (cursor_x >= SCREEN_WIDTH) {
        cursor_x = 0;
        cursor_y++;
        if (cursor_y >= SCREEN_HEIGHT) cursor_y = 0;
    }
}


void render_screen() {
    for (int y = 0; y < SCREEN_HEIGHT; y++) {
        for (int x = 0; x < SCREEN_WIDTH; x++) {
            putchar(screen[y][x].ch);
        }
        putchar('\n');
    }
}
