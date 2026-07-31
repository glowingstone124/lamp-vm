//
// Created by Max Wang on 2026/1/10.
//

#ifndef VM_DISPLAY_H
#define VM_DISPLAY_H
#include <SDL3/SDL_render.h>
#include "../../vm.h"
typedef struct {
    uint32_t *vram;
} frame_buffer;
typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
} display;
int display_init(VM *vm, int serial_window_enabled);
void display_poll_events(VM *vm);
void display_update(VM *vm);
void display_shutdown(void);

#endif // VM_DISPLAY_H
