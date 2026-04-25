//
// Created by Max Wang on 2025/12/30.
//

#include <termios.h>
#include <unistd.h>
#include <fcntl.h>

#include "../../interrupt.h"
#include "../../vm.h"
#include "../../io.h"
#include "frame.h"
struct termios orig_termios;

void enable_raw_mode(void) {
    tcgetattr(STDIN_FILENO, &orig_termios);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);
}

void disable_raw_mode(void) { tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios); }

int get_key_nonblocking(void) {
    char c;
    int n = read(STDIN_FILENO, &c, 1);
    if (n == 1)
        return c;
    return -1;
}

void vm_handle_keyboard(VM *vm) {
    int c = get_key_nonblocking();
    if (c != -1) {
        if (c == '\r') {
            c = '\n';
        }
        (void)vm_serial_rx_enqueue(vm, (uint8_t)c);
    }
}
