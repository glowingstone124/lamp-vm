//
// Created by Max Wang on 2025/12/30.
//

#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../interrupt.h"
#include "../../vm.h"
#include "../../io.h"
#include "frame.h"
struct termios orig_termios;

static int console_trace_enabled(void) {
    static int initialized;
    static int enabled;
    if (!initialized) {
        enabled = getenv("LAMP_CONSOLE_TRACE") ? 1 : 0;
        initialized = 1;
    }
    return enabled;
}

void enable_raw_mode(void) {
    tcgetattr(STDIN_FILENO, &orig_termios);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

void disable_raw_mode(void) { tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios); }

int get_key_nonblocking(void) {
    char c;
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags >= 0 && (flags & O_NONBLOCK) == 0) {
        (void)fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    }
    int n = read(STDIN_FILENO, &c, 1);
    if (n == 1)
        return c;
    return -1;
}

void vm_handle_keyboard(VM *vm) {
    for (;;) {
        int c = get_key_nonblocking();
        if (c == -1) {
            break;
        }
        if (c == '\r') {
            c = '\n';
        }
        if (console_trace_enabled()) {
            fprintf(stderr, "[console stdin] rx=0x%02x\n", (unsigned)(uint8_t)c);
        }
        (void)vm_serial_rx_enqueue(vm, (uint8_t)c);
    }
}
