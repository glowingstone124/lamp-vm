#include "serial_console.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../io.h"
#include "../../runtime_stats.h"


#define SERIAL_COLS 80
#define SERIAL_ROWS 30
#define BITMAP_GLYPH_W 8
#define BITMAP_GLYPH_H 8
#define CELL_W 9
#define CELL_H 18
#define METRICS_H 20
#define TERM_X 0
#define TERM_Y METRICS_H
#define SERIAL_WIDTH (SERIAL_COLS * CELL_W)
#define SERIAL_HEIGHT (METRICS_H + SERIAL_ROWS * CELL_H)
#define ASCII_MIN 32
#define ASCII_MAX 126
#define FONT_PIXELS (BITMAP_GLYPH_W * BITMAP_GLYPH_H)
#define FONT_GLYPHS (ASCII_MAX - ASCII_MIN + 1)

enum {
    ANSI_NORMAL,
    ANSI_ESCAPE,
    ANSI_CSI
};

typedef struct {
    uint8_t ch;
    uint8_t fg;
    uint8_t bg;
} serial_cell_t;

static SDL_Window *g_window;
static SDL_Renderer *g_renderer;
static SDL_Texture *g_texture;
static uint32_t *g_pixels;
static serial_cell_t g_cells[SERIAL_ROWS][SERIAL_COLS];
static int g_cursor_x;
static int g_cursor_y;
static uint8_t g_fg = 7u;
static uint8_t g_bg = 0u;
static int g_bold;
static int g_ansi_state;
static uint32_t g_ansi_values[8];
static uint32_t g_ansi_count;
static int g_dirty;
static int g_has_focus;
static char g_runtime_status[112];


static const uint32_t g_palette[16] = {
    0xFF0B1020u, 0xFFF87171u, 0xFF4ADE80u, 0xFFFBBF24u,
    0xFF60A5FAu, 0xFFC084FCu, 0xFF22D3EEu, 0xFFCBD5E1u,
    0xFF64748Bu, 0xFFFCA5A5u, 0xFF86EFACu, 0xFFFDE68Au,
    0xFF93C5FDu, 0xFFD8B4FEu, 0xFF67E8F9u, 0xFFF8FAFCu
};

/* Shared with the guest framebuffer console; entries are 0/1 pixels. */
static const int g_font[FONT_GLYPHS * FONT_PIXELS] = {
#include "../../../kernel/src/console_font_8x8_data.inc"
};

static void serial_clear_row(int row) {
    for (int col = 0; col < SERIAL_COLS; col++) {
        g_cells[row][col].ch = (uint8_t)' ';
        g_cells[row][col].fg = g_fg;
        g_cells[row][col].bg = g_bg;
    }
}

static void serial_clear(void) {
    for (int row = 0; row < SERIAL_ROWS; row++) {
        serial_clear_row(row);
    }
    g_cursor_x = 0;
    g_cursor_y = 0;
    g_dirty = 1;
}

static void serial_scroll(void) {
    memmove(&g_cells[0][0], &g_cells[1][0],
            sizeof(g_cells[0]) * (SERIAL_ROWS - 1));
    serial_clear_row(SERIAL_ROWS - 1);
    g_cursor_y = SERIAL_ROWS - 1;
    g_dirty = 1;
}

static void serial_newline(void) {
    g_cursor_x = 0;
    g_cursor_y++;
    if (g_cursor_y >= SERIAL_ROWS) {
        serial_scroll();
    }
}

static void serial_apply_sgr(uint32_t code) {
    if (code == 0u) {
        g_fg = 7u;
        g_bg = 0u;
        g_bold = 0;
    } else if (code == 1u) {
        g_bold = 1;
        if (g_fg < 8u) g_fg = (uint8_t)(g_fg + 8u);
    } else if (code == 22u) {
        g_bold = 0;
        if (g_fg >= 8u) g_fg = (uint8_t)(g_fg - 8u);
    } else if (code >= 30u && code <= 37u) {
        g_fg = (uint8_t)(code - 30u + (g_bold ? 8u : 0u));
    } else if (code >= 40u && code <= 47u) {
        g_bg = (uint8_t)(code - 40u);
    } else if (code >= 90u && code <= 97u) {
        g_fg = (uint8_t)(code - 90u + 8u);
    } else if (code >= 100u && code <= 107u) {
        g_bg = (uint8_t)(code - 100u + 8u);
    }
}

static void serial_put_byte(uint8_t c) {
    if (g_ansi_state == ANSI_ESCAPE) {
        if (c == (uint8_t)'[') {
            g_ansi_state = ANSI_CSI;
            g_ansi_count = 0u;
            memset(g_ansi_values, 0, sizeof(g_ansi_values));
        } else {
            g_ansi_state = ANSI_NORMAL;
        }
        return;
    }
    if (g_ansi_state == ANSI_CSI) {
        if (c >= (uint8_t)'0' && c <= (uint8_t)'9') {
            g_ansi_values[g_ansi_count] = g_ansi_values[g_ansi_count] * 10u +
                                          (uint32_t)(c - (uint8_t)'0');
            return;
        }
        if (c == (uint8_t)';') {
            if (g_ansi_count + 1u < 8u) g_ansi_count++;
            return;
        }
        if (c == (uint8_t)'m') {
            for (uint32_t i = 0u; i <= g_ansi_count; i++) {
                serial_apply_sgr(g_ansi_values[i]);
            }
        } else if (c == (uint8_t)'J') {
            /* Clear-screen is the common behavior for ESC[J and ESC[2J. */
            serial_clear();
        } else if (c == (uint8_t)'H' || c == (uint8_t)'f') {
            uint32_t row = g_ansi_values[0] ? g_ansi_values[0] : 1u;
            uint32_t col = g_ansi_values[1] ? g_ansi_values[1] : 1u;
            g_cursor_y = (int)(row > SERIAL_ROWS ? SERIAL_ROWS - 1u : row - 1u);
            g_cursor_x = (int)(col > SERIAL_COLS ? SERIAL_COLS - 1u : col - 1u);
        } else if (c == (uint8_t)'K') {
            int first = g_ansi_values[0] == 2u ? 0 : g_cursor_x;
            int last = g_ansi_values[0] == 1u ? g_cursor_x + 1 : SERIAL_COLS;
            for (int col = first; col < last; col++) {
                g_cells[g_cursor_y][col].ch = (uint8_t)' ';
                g_cells[g_cursor_y][col].fg = g_fg;
                g_cells[g_cursor_y][col].bg = g_bg;
            }
            g_dirty = 1;
        } else if (c == (uint8_t)'A') {
            int amount = (int)(g_ansi_values[0] ? g_ansi_values[0] : 1u);
            g_cursor_y = g_cursor_y > amount ? g_cursor_y - amount : 0;
        } else if (c == (uint8_t)'B') {
            int amount = (int)(g_ansi_values[0] ? g_ansi_values[0] : 1u);
            g_cursor_y += amount;
            if (g_cursor_y >= SERIAL_ROWS) g_cursor_y = SERIAL_ROWS - 1;
        } else if (c == (uint8_t)'C') {
            int amount = (int)(g_ansi_values[0] ? g_ansi_values[0] : 1u);
            g_cursor_x += amount;
            if (g_cursor_x >= SERIAL_COLS) g_cursor_x = SERIAL_COLS - 1;
        } else if (c == (uint8_t)'D') {
            int amount = (int)(g_ansi_values[0] ? g_ansi_values[0] : 1u);
            g_cursor_x = g_cursor_x > amount ? g_cursor_x - amount : 0;
        } else if (c == (uint8_t)'G') {
            uint32_t col = g_ansi_values[0] ? g_ansi_values[0] : 1u;
            g_cursor_x = (int)(col > SERIAL_COLS ? SERIAL_COLS - 1u : col - 1u);
        }
        g_ansi_state = ANSI_NORMAL;
        return;
    }

    if (c == 0x1Bu) {
        g_ansi_state = ANSI_ESCAPE;
        return;
    }
    if (c == (uint8_t)'\r') {
        g_cursor_x = 0;
        return;
    }
    if (c == (uint8_t)'\n') {
        serial_newline();
        g_dirty = 1;
        return;
    }
    if (c == (uint8_t)'\b' || c == 0x7Fu) {
        if (g_cursor_x > 0) {
            g_cursor_x--;
            g_cells[g_cursor_y][g_cursor_x].ch = (uint8_t)' ';
            g_cells[g_cursor_y][g_cursor_x].fg = g_fg;
            g_cells[g_cursor_y][g_cursor_x].bg = g_bg;
            g_dirty = 1;
        }
        return;
    }
    if (c == (uint8_t)'\t') {
        int spaces = 4 - (g_cursor_x & 3);
        while (spaces-- > 0) serial_put_byte((uint8_t)' ');
        return;
    }
    if (c < ASCII_MIN || c > ASCII_MAX) {
        return;
    }

    g_cells[g_cursor_y][g_cursor_x].ch = c;
    g_cells[g_cursor_y][g_cursor_x].fg = g_fg;
    g_cells[g_cursor_y][g_cursor_x].bg = g_bg;
    g_cursor_x++;
    if (g_cursor_x >= SERIAL_COLS) serial_newline();
    g_dirty = 1;
}

static void serial_fill_rect(int x, int y, int w, int h, uint32_t color) {
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > SERIAL_WIDTH ? SERIAL_WIDTH : x + w;
    int y1 = y + h > SERIAL_HEIGHT ? SERIAL_HEIGHT : y + h;
    for (int py = y0; py < y1; py++) {
        for (int px = x0; px < x1; px++) {
            g_pixels[py * SERIAL_WIDTH + px] = color;
        }
    }
}

static void serial_draw_bitmap_glyph(uint8_t ch, int px, int py, uint32_t color) {
    int glyph = (int)ch - ASCII_MIN;
    if (glyph < 0 || glyph >= FONT_GLYPHS) return;
    for (int y = 0; y < BITMAP_GLYPH_H; y++) {
        for (int x = 0; x < BITMAP_GLYPH_W; x++) {
            if (g_font[glyph * FONT_PIXELS + y * BITMAP_GLYPH_W + x]) {
                serial_fill_rect(px + x, py + y * 2, 1, 2, color);
            }
        }
    }
}

static void serial_draw_bitmap_text(const char *text, int px, int py, uint32_t color) {
    while (text && *text != '\0') {
        serial_draw_bitmap_glyph((uint8_t)*text++, px, py, color);
        px += CELL_W;
    }
}

static void serial_update_runtime_status(VM *vm) {
    VmRuntimeStats stats;
    char next[sizeof(g_runtime_status)];
    uint64_t uptime_seconds;
    uint64_t uptime_hours;
    uint64_t uptime_minutes;
    uint64_t rate_tenths;
    uint64_t per_core_rate_tenths;
    uint64_t rss_mib;
    uint64_t ram_mib;
    if (!vm) return;

    vm_runtime_stats_sample(vm, &stats);
    uptime_seconds = stats.uptime_ns / 1000000000ull;
    uptime_hours = uptime_seconds / 3600u;
    uptime_minutes = (uptime_seconds / 60u) % 60u;
    rate_tenths = stats.execution_rate_hz / 100000u;
    per_core_rate_tenths = stats.active_core_count != 0u
        ? rate_tenths / stats.active_core_count
        : 0u;
    rss_mib = stats.host_resident_bytes / (1024u * 1024u);
    ram_mib = stats.guest_ram_bytes / (1024u * 1024u);
    (void)snprintf(next, sizeof(next),
                   "CAP %lluM/C | MIPS %llu.%llu/C %llu.%llu/T | RAM %lluM | RSS %lluM | %uC | UP %02llu:%02llu",
                   (unsigned long long)(stats.cpu_frequency_hz / 1000000u),
                   (unsigned long long)(per_core_rate_tenths / 10u),
                   (unsigned long long)(per_core_rate_tenths % 10u),
                   (unsigned long long)(rate_tenths / 10u),
                   (unsigned long long)(rate_tenths % 10u),
                   (unsigned long long)ram_mib,
                   (unsigned long long)rss_mib,
                   stats.active_core_count,
                   (unsigned long long)uptime_hours,
                   (unsigned long long)uptime_minutes);
    if (strcmp(next, g_runtime_status) != 0) {
        (void)snprintf(g_runtime_status, sizeof(g_runtime_status), "%s", next);
        g_dirty = 1;
    }
}

static void serial_render(void) {
    if (!g_dirty || !g_renderer || !g_texture || !g_pixels) return;

    serial_fill_rect(0, 0, SERIAL_WIDTH, METRICS_H, 0xFF101827u);
    serial_draw_bitmap_text(g_runtime_status, 0, 2, 0xFF94A3B8u);

    for (int row = 0; row < SERIAL_ROWS; row++) {
        for (int col = 0; col < SERIAL_COLS; col++) {
            const serial_cell_t *cell = &g_cells[row][col];
            int px = TERM_X + col * CELL_W;
            int py = TERM_Y + row * CELL_H;
            serial_fill_rect(px, py, CELL_W, CELL_H,
                             g_palette[cell->bg & 0x0Fu]);
        }
    }

    for (int row = 0; row < SERIAL_ROWS; row++) {
        for (int col = 0; col < SERIAL_COLS; col++) {
            const serial_cell_t *cell = &g_cells[row][col];
            serial_draw_bitmap_glyph(cell->ch,
                                     TERM_X + col * CELL_W,
                                     TERM_Y + row * CELL_H + 1,
                                     g_palette[cell->fg & 0x0Fu]);
        }
    }

    if (g_cursor_x >= 0 && g_cursor_x < SERIAL_COLS &&
        g_cursor_y >= 0 && g_cursor_y < SERIAL_ROWS) {
        int py = TERM_Y + g_cursor_y * CELL_H + CELL_H - 2;
        int px = TERM_X + g_cursor_x * CELL_W;
        serial_fill_rect(px, py, CELL_W - 1, 2,
                         g_has_focus ? 0xFF38BDF8u : 0xFF64748Bu);
    }
    SDL_UpdateTexture(g_texture, NULL, g_pixels, SERIAL_WIDTH * (int)sizeof(uint32_t));
    SDL_RenderClear(g_renderer);
    SDL_RenderTexture(g_renderer, g_texture, NULL, NULL);
    SDL_RenderPresent(g_renderer);
    g_dirty = 0;
}

static void serial_push(VM *vm, uint8_t c) {
    if (c == (uint8_t)'\r') c = (uint8_t)'\n';
    (void)vm_serial_rx_enqueue(vm, c);
}

static void serial_push_bytes(VM *vm, const char *bytes) {
    if (!bytes) return;
    while (*bytes != '\0') {
        serial_push(vm, (uint8_t)*bytes++);
    }
}

int serial_console_init(void) {
    g_window = SDL_CreateWindow("Lamp VM — Serial Console",
                                SERIAL_WIDTH, SERIAL_HEIGHT,
                                SDL_WINDOW_RESIZABLE |
                                SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!g_window) return -1;

    g_renderer = SDL_CreateRenderer(g_window, NULL);
    if (!g_renderer) {
        serial_console_shutdown();
        return -1;
    }
    /* Presentation is paced by the shared VM display loop. */
    (void)SDL_SetRenderVSync(g_renderer, 0);
    (void)SDL_SetRenderLogicalPresentation(g_renderer,
                                           SERIAL_WIDTH, SERIAL_HEIGHT,
                                           SDL_LOGICAL_PRESENTATION_LETTERBOX);
    g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888,
                                  SDL_TEXTUREACCESS_STREAMING,
                                  SERIAL_WIDTH, SERIAL_HEIGHT);
    g_pixels = calloc((size_t)SERIAL_WIDTH * SERIAL_HEIGHT, sizeof(*g_pixels));
    if (!g_texture || !g_pixels) {
        serial_console_shutdown();
        return -1;
    }
    g_fg = 7u;
    g_bg = 0u;
    g_bold = 0;
    g_ansi_state = ANSI_NORMAL;
    g_has_focus = (SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS) != 0u;
    (void)snprintf(g_runtime_status, sizeof(g_runtime_status),
                   "CAP --M/C | MIPS --.-/C --.-/T | RAM -- | RSS -- | --C | UP --:--");
    serial_clear();
    serial_render();
    SDL_StartTextInput(g_window);
    return 0;
}

void serial_console_shutdown(void) {
    free(g_pixels);
    g_pixels = NULL;
    if (g_texture) SDL_DestroyTexture(g_texture);
    if (g_renderer) SDL_DestroyRenderer(g_renderer);
    if (g_window) SDL_DestroyWindow(g_window);
    g_texture = NULL;
    g_renderer = NULL;
    g_window = NULL;
}

uint32_t serial_console_window_id(void) {
    return g_window ? SDL_GetWindowID(g_window) : 0u;
}

int serial_console_handle_event(VM *vm, const SDL_Event *event) {
    uint32_t window_id = serial_console_window_id();
    if (!vm || !event || window_id == 0u) return 0;

    if ((event->type == SDL_EVENT_WINDOW_FOCUS_GAINED ||
         event->type == SDL_EVENT_WINDOW_FOCUS_LOST) &&
        event->window.windowID == window_id) {
        if (event->type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
            g_has_focus = 1;
            g_dirty = 1;
        } else {
            g_has_focus = 0;
            g_dirty = 1;
        }
        return 0;
    }
    if (event->type == SDL_EVENT_TEXT_INPUT && event->text.windowID == window_id) {
        serial_push_bytes(vm, event->text.text);
        return 1;
    }
    if ((event->type != SDL_EVENT_KEY_DOWN && event->type != SDL_EVENT_KEY_UP) ||
        event->key.windowID != window_id) {
        return 0;
    }
    if (event->type == SDL_EVENT_KEY_UP) return 1;
    if (event->key.repeat) return 1;

    SDL_Keycode sym = event->key.key;
    SDL_Keymod mod = event->key.mod;
    if ((mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0 && sym == SDLK_V) {
        char *text = SDL_GetClipboardText();
        serial_push_bytes(vm, text);
        SDL_free(text);
        return 1;
    }
    if ((mod & SDL_KMOD_CTRL) != 0 && sym >= SDLK_A && sym <= SDLK_Z) {
        serial_push(vm, (uint8_t)(sym - SDLK_A + 1));
        return 1;
    }
    switch (sym) {
        case SDLK_RETURN:
        case SDLK_KP_ENTER: serial_push(vm, (uint8_t)'\n'); break;
        case SDLK_BACKSPACE: serial_push(vm, 0x7Fu); break;
        case SDLK_TAB: serial_push(vm, (uint8_t)'\t'); break;
        case SDLK_ESCAPE: serial_push(vm, 0x1Bu); break;
        case SDLK_UP: serial_push_bytes(vm, "\x1b[A"); break;
        case SDLK_DOWN: serial_push_bytes(vm, "\x1b[B"); break;
        case SDLK_RIGHT: serial_push_bytes(vm, "\x1b[C"); break;
        case SDLK_LEFT: serial_push_bytes(vm, "\x1b[D"); break;
        case SDLK_HOME: serial_push_bytes(vm, "\x1b[H"); break;
        case SDLK_END: serial_push_bytes(vm, "\x1b[F"); break;
        case SDLK_DELETE: serial_push_bytes(vm, "\x1b[3~"); break;
        default: break;
    }
    return 1;
}

void serial_console_update(VM *vm) {
    uint8_t c;
    int consumed = 0;
    serial_update_runtime_status(vm);
    while (vm_serial_tx_dequeue(vm, &c)) {
        serial_put_byte(c);
        consumed++;
        if (consumed >= 4096) break;
    }
    serial_render();
}
