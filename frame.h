#ifndef VM_FRAME_H
#define VM_FRAME_H
typedef struct {
    char ch;
    uint8_t attr;
} Cell;

void init_screen();
void render_screen();
void put_char_with_attr(char c, char attr);

#endif //VM_FRAME_H
