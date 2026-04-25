#ifndef VM_FRAME_H
#define VM_FRAME_H
#pragma once
#include "../../vm.h"
typedef struct {
    char ch;
    uint8_t attr;
} Cell;
typedef struct {
    int x1, y1, x2, y2;
    int dirty;
} DirtyRect;

void init_screen(void);

void scroll_up(void);

void put_char_with_attr(char c, char attr);

void flush_to_vga(void);

void render_vga_screen(void);

void flush_screen_dirty(void);

void render_screen_dirty(void);

void enable_raw_mode(void);

void disable_raw_mode(void);

int get_key_nonblocking(void);

void vm_handle_keyboard(VM *vm);

void flush_screen_final(void);

#endif // VM_FRAME_H
