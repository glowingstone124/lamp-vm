//
// Created by Max Wang on 2026/1/10.
//

#ifndef VM_DISPLAY_H
#define VM_DISPLAY_H
#define SDL_DISABLE_IMMINTRIN_H
#define SDL_DISABLE_MM3DNOW_H
#define SDL_DISABLE_MMINTRIN_H
#define SDL_DISABLE_XMMINTRIN_H
#define SDL_DISABLE_EMMINTRIN_H
#include <SDL2/SDL_render.h>
#include "../../vm.h"
typedef struct {
    uint32_t *vram;
} frame_buffer;
typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
} display;
int vga_display_init(void);
void display_poll_events(VM *vm);
void display_update(VM *vm);
void display_shutdown(void);

#endif // VM_DISPLAY_H
