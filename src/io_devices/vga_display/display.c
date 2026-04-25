#include <SDL2/SDL.h>
#include <string.h>
#include "display.h"
#include "../../io.h"
#include "../../interrupt.h"
#include "../../vm.h"

static SDL_Window *window = NULL;
static SDL_Renderer *renderer = NULL;
static SDL_Texture *texture = NULL;
static uint8_t g_mouse_buttons;

int vga_display_init(void) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
        return -1;

    window = SDL_CreateWindow(
        "VM Display", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, FB_WIDTH, FB_HEIGHT, SDL_WINDOW_SHOWN);

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    texture = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, FB_WIDTH, FB_HEIGHT);

    return 0;
}

static void ps2_kbd_push(VM *vm, uint8_t c) {
    (void)vm_ps2_kbd_enqueue(vm, c);
}

static void ps2_mouse_push(VM *vm, uint8_t c) {
    (void)vm_ps2_mouse_enqueue(vm, c);
}

static int sdl_scancode_to_ps2_set1(SDL_Scancode scancode, uint8_t *prefix, uint8_t *code) {
    *prefix = 0u;
    switch (scancode) {
        case SDL_SCANCODE_ESCAPE: *code = 0x01u; return 1;
        case SDL_SCANCODE_1: *code = 0x02u; return 1;
        case SDL_SCANCODE_2: *code = 0x03u; return 1;
        case SDL_SCANCODE_3: *code = 0x04u; return 1;
        case SDL_SCANCODE_4: *code = 0x05u; return 1;
        case SDL_SCANCODE_5: *code = 0x06u; return 1;
        case SDL_SCANCODE_6: *code = 0x07u; return 1;
        case SDL_SCANCODE_7: *code = 0x08u; return 1;
        case SDL_SCANCODE_8: *code = 0x09u; return 1;
        case SDL_SCANCODE_9: *code = 0x0Au; return 1;
        case SDL_SCANCODE_0: *code = 0x0Bu; return 1;
        case SDL_SCANCODE_MINUS: *code = 0x0Cu; return 1;
        case SDL_SCANCODE_EQUALS: *code = 0x0Du; return 1;
        case SDL_SCANCODE_BACKSPACE: *code = 0x0Eu; return 1;
        case SDL_SCANCODE_TAB: *code = 0x0Fu; return 1;
        case SDL_SCANCODE_Q: *code = 0x10u; return 1;
        case SDL_SCANCODE_W: *code = 0x11u; return 1;
        case SDL_SCANCODE_E: *code = 0x12u; return 1;
        case SDL_SCANCODE_R: *code = 0x13u; return 1;
        case SDL_SCANCODE_T: *code = 0x14u; return 1;
        case SDL_SCANCODE_Y: *code = 0x15u; return 1;
        case SDL_SCANCODE_U: *code = 0x16u; return 1;
        case SDL_SCANCODE_I: *code = 0x17u; return 1;
        case SDL_SCANCODE_O: *code = 0x18u; return 1;
        case SDL_SCANCODE_P: *code = 0x19u; return 1;
        case SDL_SCANCODE_LEFTBRACKET: *code = 0x1Au; return 1;
        case SDL_SCANCODE_RIGHTBRACKET: *code = 0x1Bu; return 1;
        case SDL_SCANCODE_RETURN: *code = 0x1Cu; return 1;
        case SDL_SCANCODE_LCTRL: *code = 0x1Du; return 1;
        case SDL_SCANCODE_A: *code = 0x1Eu; return 1;
        case SDL_SCANCODE_S: *code = 0x1Fu; return 1;
        case SDL_SCANCODE_D: *code = 0x20u; return 1;
        case SDL_SCANCODE_F: *code = 0x21u; return 1;
        case SDL_SCANCODE_G: *code = 0x22u; return 1;
        case SDL_SCANCODE_H: *code = 0x23u; return 1;
        case SDL_SCANCODE_J: *code = 0x24u; return 1;
        case SDL_SCANCODE_K: *code = 0x25u; return 1;
        case SDL_SCANCODE_L: *code = 0x26u; return 1;
        case SDL_SCANCODE_SEMICOLON: *code = 0x27u; return 1;
        case SDL_SCANCODE_APOSTROPHE: *code = 0x28u; return 1;
        case SDL_SCANCODE_GRAVE: *code = 0x29u; return 1;
        case SDL_SCANCODE_LSHIFT: *code = 0x2Au; return 1;
        case SDL_SCANCODE_BACKSLASH: *code = 0x2Bu; return 1;
        case SDL_SCANCODE_Z: *code = 0x2Cu; return 1;
        case SDL_SCANCODE_X: *code = 0x2Du; return 1;
        case SDL_SCANCODE_C: *code = 0x2Eu; return 1;
        case SDL_SCANCODE_V: *code = 0x2Fu; return 1;
        case SDL_SCANCODE_B: *code = 0x30u; return 1;
        case SDL_SCANCODE_N: *code = 0x31u; return 1;
        case SDL_SCANCODE_M: *code = 0x32u; return 1;
        case SDL_SCANCODE_COMMA: *code = 0x33u; return 1;
        case SDL_SCANCODE_PERIOD: *code = 0x34u; return 1;
        case SDL_SCANCODE_SLASH: *code = 0x35u; return 1;
        case SDL_SCANCODE_RSHIFT: *code = 0x36u; return 1;
        case SDL_SCANCODE_LALT: *code = 0x38u; return 1;
        case SDL_SCANCODE_SPACE: *code = 0x39u; return 1;
        case SDL_SCANCODE_CAPSLOCK: *code = 0x3Au; return 1;
        case SDL_SCANCODE_KP_ENTER: *code = 0x1Cu; return 1;
        case SDL_SCANCODE_RCTRL: *prefix = 0xE0u; *code = 0x1Du; return 1;
        case SDL_SCANCODE_RALT: *prefix = 0xE0u; *code = 0x38u; return 1;
        case SDL_SCANCODE_UP: *prefix = 0xE0u; *code = 0x48u; return 1;
        case SDL_SCANCODE_LEFT: *prefix = 0xE0u; *code = 0x4Bu; return 1;
        case SDL_SCANCODE_RIGHT: *prefix = 0xE0u; *code = 0x4Du; return 1;
        case SDL_SCANCODE_DOWN: *prefix = 0xE0u; *code = 0x50u; return 1;
        case SDL_SCANCODE_DELETE: *prefix = 0xE0u; *code = 0x53u; return 1;
        default:
            return 0;
    }
}

static void ps2_kbd_send_key(VM *vm, SDL_Scancode scancode, int released) {
    uint8_t prefix;
    uint8_t code;
    if (!sdl_scancode_to_ps2_set1(scancode, &prefix, &code)) {
        return;
    }
    if (prefix != 0u) {
        ps2_kbd_push(vm, prefix);
    }
    ps2_kbd_push(vm, released ? (uint8_t)(code | 0x80u) : code);
}

static void ps2_mouse_send_packet(VM *vm, int dx, int dy) {
    int rem_x = dx;
    int rem_y = dy;

    do {
        int pkt_x = rem_x;
        int pkt_y = rem_y;
        int ps2_y;
        uint8_t b0 = 0x08u;
        if (pkt_x > 127) pkt_x = 127;
        if (pkt_x < -127) pkt_x = -127;
        if (pkt_y > 127) pkt_y = 127;
        if (pkt_y < -127) pkt_y = -127;
        rem_x -= pkt_x;
        rem_y -= pkt_y;
        ps2_y = -pkt_y;

        if ((g_mouse_buttons & SDL_BUTTON_LMASK) != 0u) b0 |= 0x01u;
        if ((g_mouse_buttons & SDL_BUTTON_RMASK) != 0u) b0 |= 0x02u;
        if ((g_mouse_buttons & SDL_BUTTON_MMASK) != 0u) b0 |= 0x04u;
        if (pkt_x < 0) b0 |= 0x10u;
        if (ps2_y < 0) b0 |= 0x20u;

        ps2_mouse_push(vm, b0);
        ps2_mouse_push(vm, (uint8_t)((int8_t)pkt_x));
        ps2_mouse_push(vm, (uint8_t)((int8_t)ps2_y));
    } while (rem_x != 0 || rem_y != 0);
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
            case SDL_KEYDOWN:
                ps2_kbd_send_key(vm, e.key.keysym.scancode, 0);
                break;
            case SDL_KEYUP:
                ps2_kbd_send_key(vm, e.key.keysym.scancode, 1);
                break;
            case SDL_MOUSEMOTION:
                if (e.motion.xrel != 0 || e.motion.yrel != 0) {
                    ps2_mouse_send_packet(vm, e.motion.xrel, e.motion.yrel);
                }
                break;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
                g_mouse_buttons = (uint8_t)SDL_GetMouseState(NULL, NULL);
                ps2_mouse_send_packet(vm, 0, 0);
                break;
            default:
                break;
        }
    }
}

void display_shutdown(void) {
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
}
