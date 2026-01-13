#include <SDL2/SDL.h>
#include "display.h"
#include "../../vm.h"

static SDL_Window *window = NULL;
static SDL_Renderer *renderer = NULL;
static SDL_Texture *texture = NULL;

int vga_display_init(void) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
        return -1;

    window = SDL_CreateWindow(
        "VM Display", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, FB_WIDTH, FB_HEIGHT, 0);

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    texture = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, FB_WIDTH, FB_HEIGHT);

    return 0;
}

void display_update(VM *vm) {
    printf("first 16 pixels:");
    for (int i = 0; i < 16; i++) {
        printf(" %08x", ((uint32_t *)vm->fb)[i]);
    }
    printf("\n");

    printf("flushing texture, first pixel = %08x\n", ((uint32_t *)vm->fb)[0]);
    SDL_UpdateTexture(texture, NULL, vm->fb, FB_WIDTH * FB_BPP);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
    printf("flushed\n");
}

void display_poll_events(VM *vm) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) {
            vm->halted = 1;
        }
    }
}

void display_shutdown() {
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
}
