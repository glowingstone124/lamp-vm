#include <SDL3/SDL.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include "display.h"
#include "../../io.h"
#include "../../interrupt.h"
#include "../../vm.h"
#include "../serial/serial_console.h"

static SDL_Window *window = NULL;
static SDL_Renderer *renderer = NULL;
static SDL_Texture *texture = NULL;
static SDL_MouseButtonFlags g_mouse_buttons;
static uint32_t g_display_window_id;
static uint8_t g_pressed_scancodes[SDL_SCANCODE_COUNT];
static uint8_t g_pointer_capture_requested;
static uint8_t g_pointer_captured;
static uint8_t g_capture_hotkey_down;
static float g_mouse_motion_remainder_x;
static float g_mouse_motion_remainder_y;

int display_init(VM *vm, int serial_window_enabled) {
    /* Keep macOS trackpads on the mouse path and apply the user's native
     * acceleration curve in relative mode. Centering is the most reliable
     * continuous-capture mode on macOS. */
    (void)SDL_SetHint(SDL_HINT_TRACKPAD_IS_TOUCH_ONLY, "0");
    (void)SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE, "1");
    (void)SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_MODE_CENTER, "1");
    (void)SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
        return -1;

    window = SDL_CreateWindow(
        "VM Display", FB_WIDTH, FB_HEIGHT,
        SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE);

    if (!window) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return -1;
    }
    g_display_window_id = SDL_GetWindowID(window);
    renderer = SDL_CreateRenderer(window, NULL);
    if (!renderer) {
        display_shutdown();
        return -1;
    }
    /* The VM owns a high-resolution 120 Hz deadline loop. Renderer VSync would
     * add a second blocking clock and make frame time alternate under load. */
    (void)SDL_SetRenderVSync(renderer, 0);
    (void)SDL_SetRenderDrawColor(renderer, 0u, 0u, 0u, 255u);
    if (!SDL_SetRenderLogicalPresentation(
            renderer, FB_WIDTH, FB_HEIGHT, SDL_LOGICAL_PRESENTATION_LETTERBOX)) {
        fprintf(stderr, "Failed to configure VM display presentation: %s\n", SDL_GetError());
        display_shutdown();
        return -1;
    }
    texture = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, FB_WIDTH, FB_HEIGHT);
    if (!texture) {
        display_shutdown();
        return -1;
    }
    if (!SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST)) {
        fprintf(stderr, "Failed to configure VM framebuffer scaling: %s\n", SDL_GetError());
        display_shutdown();
        return -1;
    }
    /* Guest framebuffer pixels are 0x00RRGGBB as well as 0xAARRGGBB.
     * They are always opaque display pixels; the high byte is not alpha. */
    if (!SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE)) {
        fprintf(stderr, "Failed to configure VM framebuffer texture: %s\n", SDL_GetError());
        display_shutdown();
        return -1;
    }

    vm->serial_window_enabled = serial_window_enabled ? 1 : 0;
    memset(g_pressed_scancodes, 0, sizeof(g_pressed_scancodes));
    g_mouse_buttons = 0u;
    /* Keep capture armed before the Serial window is created. On macOS the
     * first trackpad click used to focus an inactive Display window may not be
     * delivered as a mouse-button event even with click-through enabled. The
     * focus-gained event can now enter relative mode immediately.
     * Ctrl+Command+G on macOS (or Ctrl+Alt+G elsewhere) still clears this
     * request until the user clicks Display again. */
    g_pointer_capture_requested = 1u;
    g_pointer_captured = 0u;
    g_capture_hotkey_down = 0u;
    g_mouse_motion_remainder_x = 0.0f;
    g_mouse_motion_remainder_y = 0.0f;
    vm_fb_mark_all_dirty(vm);
    if (vm->serial_window_enabled && serial_console_init() != 0) {
        fprintf(stderr, "Failed to create VM Serial window: %s\n", SDL_GetError());
        vm->serial_window_enabled = 0;
    }
    return 0;
}

static void ps2_kbd_push(VM *vm, uint8_t c) {
    (void)vm_ps2_kbd_enqueue(vm, c);
}

static int ps2_mouse_push(VM *vm, uint8_t c) {
    return vm_ps2_mouse_enqueue(vm, c);
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

static void ps2_mouse_send_packet(VM *vm, int dx, int dy);

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

static void ps2_release_input_state(VM *vm) {
    for (int scancode = 0; scancode < SDL_SCANCODE_COUNT; scancode++) {
        if (g_pressed_scancodes[scancode] == 0u) {
            continue;
        }
        ps2_kbd_send_key(vm, (SDL_Scancode)scancode, 1);
        g_pressed_scancodes[scancode] = 0u;
    }
    if (g_mouse_buttons != 0u) {
        g_mouse_buttons = 0u;
        ps2_mouse_send_packet(vm, 0, 0);
    }
}

static int display_window_has_input_focus(void) {
    return window &&
        (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0u;
}

static void display_apply_pointer_capture(void) {
    float discarded_x;
    float discarded_y;
    int relative_enabled;
    int should_capture;
    if (!window) {
        g_pointer_captured = 0u;
        return;
    }
    relative_enabled = SDL_GetWindowRelativeMouseMode(window) ? 1 : 0;
    should_capture = g_pointer_capture_requested &&
                     display_window_has_input_focus();
    if (should_capture) {
        if (!SDL_GetWindowMouseGrab(window)) {
            (void)SDL_SetWindowMouseGrab(window, true);
        }
        if (relative_enabled) {
            g_pointer_captured = 1u;
            return;
        }
        if (!SDL_SetWindowRelativeMouseMode(window, true)) {
            fprintf(stderr, "Failed to capture VM Display pointer: %s\n",
                    SDL_GetError());
            (void)SDL_SetWindowMouseGrab(window, false);
            g_pointer_captured = 0u;
            return;
        }
        (void)SDL_GetRelativeMouseState(&discarded_x, &discarded_y);
        g_pointer_captured = SDL_GetWindowRelativeMouseMode(window) ? 1u : 0u;
    } else {
        if (relative_enabled) {
            (void)SDL_GetRelativeMouseState(&discarded_x, &discarded_y);
            (void)SDL_SetWindowRelativeMouseMode(window, false);
        }
        if (SDL_GetWindowMouseGrab(window)) {
            (void)SDL_SetWindowMouseGrab(window, false);
        }
        (void)SDL_ShowCursor();
        g_pointer_captured = 0u;
    }
}

static void display_set_pointer_capture(int enabled) {
    g_pointer_capture_requested = enabled ? 1u : 0u;
    display_apply_pointer_capture();
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

        (void)ps2_mouse_push(vm, b0);
        (void)ps2_mouse_push(vm, (uint8_t)((int8_t)pkt_x));
        (void)ps2_mouse_push(vm, (uint8_t)((int8_t)ps2_y));
    } while (rem_x != 0 || rem_y != 0);
}

static void ps2_mouse_flush_motion(VM *vm, float *dx, float *dy) {
    const int whole_x = (int)*dx;
    const int whole_y = (int)*dy;
    *dx -= (float)whole_x;
    *dy -= (float)whole_y;
    if (whole_x != 0 || whole_y != 0) {
        ps2_mouse_send_packet(vm, whole_x, whole_y);
    }
}

static void display_upload_dirty_rows(VM *vm) {
    const size_t row_bytes = (size_t)FB_WIDTH * FB_BPP;
    uint8_t *front = (uint8_t *)vm->fb_front;
    const uint8_t *back = (const uint8_t *)vm->fb;
    size_t run_start = FB_HEIGHT;

    for (size_t row = 0; row <= FB_HEIGHT; row++) {
        const int dirty = (row < FB_HEIGHT) ? vm_fb_take_row_dirty(vm, row) : 0;

        if (dirty) {
            vm_fb_row_lock(vm, row);
            memcpy(front + row * row_bytes, back + row * row_bytes, row_bytes);
            vm_fb_row_unlock(vm, row);
            if (run_start == FB_HEIGHT) {
                run_start = row;
            }
            continue;
        }

        if (run_start != FB_HEIGHT) {
            SDL_Rect rect = {
                .x = 0,
                .y = (int)run_start,
                .w = FB_WIDTH,
                .h = (int)(row - run_start),
            };
            if (!SDL_UpdateTexture(texture, &rect, front + run_start * row_bytes, (int)row_bytes)) {
                fprintf(stderr, "Failed to update VM framebuffer texture: %s\n", SDL_GetError());
                for (size_t retry = run_start; retry < row; retry++) {
                    vm_fb_mark_row_dirty(vm, retry);
                }
            }
            run_start = FB_HEIGHT;
        }
    }
}

void display_update(VM *vm) {
    display_upload_dirty_rows(vm);
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
    if (vm->serial_window_enabled) {
        serial_console_update(vm);
    }
}

static void display_disable_serial_window(VM *vm) {
    uint8_t c;
    serial_console_shutdown();
    vm->serial_window_enabled = 0;
    while (vm_serial_tx_dequeue(vm, &c)) {
        (void)write(STDOUT_FILENO, &c, 1);
    }
}

void display_poll_events(VM *vm) {
    SDL_Event e;
    float motion_dx = g_mouse_motion_remainder_x;
    float motion_dy = g_mouse_motion_remainder_y;
    while (SDL_PollEvent(&e)) {
        if (vm->serial_window_enabled && serial_console_handle_event(vm, &e)) {
            continue;
        }
        switch (e.type) {
            case SDL_EVENT_QUIT:
                atomic_set_vm_halt(vm, 1);
                break;
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                if (e.window.windowID == g_display_window_id) {
                    atomic_set_vm_halt(vm, 1);
                } else if (e.window.windowID == serial_console_window_id()) {
                    display_disable_serial_window(vm);
                }
                break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.windowID != g_display_window_id) {
                    break;
                }
                if (e.key.scancode == SDL_SCANCODE_G &&
                    (e.key.mod & SDL_KMOD_CTRL) != 0u &&
                    (e.key.mod & (SDL_KMOD_ALT | SDL_KMOD_GUI)) != 0u) {
                    if (!g_capture_hotkey_down) {
                        g_capture_hotkey_down = 1u;
                        display_set_pointer_capture(
                            !g_pointer_capture_requested);
                    }
                    break;
                }
                ps2_kbd_send_key(vm, e.key.scancode, 0);
                if ((int)e.key.scancode >= 0 && (int)e.key.scancode < SDL_SCANCODE_COUNT) {
                    g_pressed_scancodes[e.key.scancode] = 1u;
                }
                break;
            case SDL_EVENT_KEY_UP:
                if (e.key.windowID != g_display_window_id) {
                    break;
                }
                if (e.key.scancode == SDL_SCANCODE_G && g_capture_hotkey_down) {
                    g_capture_hotkey_down = 0u;
                    break;
                }
                ps2_kbd_send_key(vm, e.key.scancode, 1);
                if ((int)e.key.scancode >= 0 && (int)e.key.scancode < SDL_SCANCODE_COUNT) {
                    g_pressed_scancodes[e.key.scancode] = 0u;
                }
                break;
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                if (e.window.windowID == g_display_window_id) {
                    display_apply_pointer_capture();
                }
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                if (e.window.windowID == g_display_window_id) {
                    ps2_mouse_flush_motion(vm, &motion_dx, &motion_dy);
                    ps2_release_input_state(vm);
                    g_capture_hotkey_down = 0u;
                    /* Preserve the user's capture request. SDL requires
                     * relative mode to be suspended without focus; the focus
                     * gained path restores it. */
                    display_apply_pointer_capture();
                    motion_dx = 0.0f;
                    motion_dy = 0.0f;
                }
                break;
            case SDL_EVENT_MOUSE_MOTION:
                if (!g_pointer_captured) break;
                /* Some relative-mode backends report no owning window. Once
                 * Display owns relative mode, window id 0 is unambiguous. */
                if (e.motion.windowID != 0u &&
                    e.motion.windowID != g_display_window_id) break;
                motion_dx += e.motion.xrel;
                motion_dy += e.motion.yrel;
                g_mouse_buttons = e.motion.state;
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (e.button.windowID != g_display_window_id) break;
                ps2_mouse_flush_motion(vm, &motion_dx, &motion_dy);
                if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                    !g_pointer_capture_requested) {
                    display_set_pointer_capture(1);
                }
                g_mouse_buttons = SDL_GetMouseState(NULL, NULL);
                ps2_mouse_send_packet(vm, 0, 0);
                break;
            default:
                break;
        }
    }
    if (g_pointer_capture_requested && display_window_has_input_focus() &&
        !SDL_GetWindowRelativeMouseMode(window)) {
        display_apply_pointer_capture();
    }
    g_pointer_captured = window && SDL_GetWindowRelativeMouseMode(window) ?
        1u : 0u;
    ps2_mouse_flush_motion(vm, &motion_dx, &motion_dy);
    g_mouse_motion_remainder_x = motion_dx;
    g_mouse_motion_remainder_y = motion_dy;
}

void display_shutdown(void) {
    display_set_pointer_capture(0);
    serial_console_shutdown();
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    texture = NULL;
    renderer = NULL;
    window = NULL;
    g_display_window_id = 0u;
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}
