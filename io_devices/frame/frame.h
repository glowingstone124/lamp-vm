#ifndef VM_FRAME_H
#define VM_FRAME_H
typedef struct {
    char ch;
    uint8_t attr;
} Cell;
typedef struct {
    int x1, y1, x2, y2;
    int dirty;
} DirtyRect;

void init_screen();

void scroll_up();

void put_char_with_attr(char c, char attr);

void flush_to_vga();

void render_vga_screen();

void flush_screen_dirty();

void render_screen_dirty();
#endif //VM_FRAME_H
