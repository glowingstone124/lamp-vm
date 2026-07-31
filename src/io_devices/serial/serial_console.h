#ifndef VM_SERIAL_CONSOLE_H
#define VM_SERIAL_CONSOLE_H

#include <SDL3/SDL_events.h>
#include <stdint.h>

#include "../../vm.h"

int serial_console_init(void);
void serial_console_shutdown(void);
uint32_t serial_console_window_id(void);
int serial_console_handle_event(VM *vm, const SDL_Event *event);
void serial_console_update(VM *vm);

#endif
