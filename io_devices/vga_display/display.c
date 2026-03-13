#include <SDL2/SDL.h>
#include <string.h>
#include "display.h"
#include "../../io.h"
#include "../../interrupt.h"
#include "../../vm.h"

static SDL_Window *window = NULL;
static SDL_Renderer *renderer = NULL;
static SDL_Texture *texture = NULL;

int vga_display_init(void) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
        return -1;

    window = SDL_CreateWindow(
        "VM Display", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, FB_WIDTH, FB_HEIGHT, SDL_WINDOW_BORDERLESS);

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    texture = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, FB_WIDTH, FB_HEIGHT);

    SDL_StartTextInput();
    return 0;
}

static void serial_rx_push(VM *vm, uint8_t c) {
    (void)vm_serial_rx_enqueue(vm, c);
}

static void serial_rx_push_normalized(VM *vm, uint8_t c) {
    if (c == (uint8_t)'\r') {
        c = (uint8_t)'\n';
    }
    serial_rx_push(vm, c);
}

void display_update(VM *vm) {
    //printf("first 16 pixels:");
    //for (int i = 0; i < 16; i++) {
    //    printf(" %08x", ((uint32_t *)vm->fb)[i]);
    //}
    //printf("\n");

    uint8_t *front = (uint8_t *)vm->fb_front;
    const uint8_t *back = (const uint8_t *)vm->fb;
    vm_shared_lock(vm);
    memcpy(front, back, FB_SIZE);
    vm_shared_unlock(vm);

    SDL_UpdateTexture(texture, NULL, vm->fb_front, FB_WIDTH * FB_BPP);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
    //printf("flushed\n");
}

void display_poll_events(VM *vm) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_QUIT:
                atomic_set_vm_halt(vm, 1);;
                break;
            case SDL_TEXTINPUT: {
                const char *p = e.text.text;
                while (*p != '\0') {
                    serial_rx_push_normalized(vm, (uint8_t)*p);
                    p++;
                }
                break;
            }
            case SDL_KEYDOWN:
                if ((e.key.keysym.mod & KMOD_CTRL) != 0) {
                    SDL_Keycode sym = e.key.keysym.sym;
                    if (sym >= SDLK_a && sym <= SDLK_z) {
                        uint8_t ctrl = (uint8_t)(sym - SDLK_a + 1);
                        serial_rx_push_normalized(vm, ctrl);
                        break;
                    }
                }
                if (e.key.keysym.sym == SDLK_RETURN ||
                    e.key.keysym.sym == SDLK_KP_ENTER ||
                    e.key.keysym.sym == SDLK_RETURN2) {
                    serial_rx_push_normalized(vm, (uint8_t)'\n');
                } else if (e.key.keysym.sym == SDLK_BACKSPACE) {
                    serial_rx_push(vm, (uint8_t)0x08);
                } else if (e.key.keysym.sym == SDLK_TAB) {
                    serial_rx_push(vm, (uint8_t)'\t');
                }
                break;
            default:
                break;
        }
    }
}

void display_shutdown(void) {
    SDL_StopTextInput();
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
}
