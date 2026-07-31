#ifndef LAMP_VM_RUNTIME_LOG_H
#define LAMP_VM_RUNTIME_LOG_H

#include <stdio.h>
#include <stdlib.h>

static inline int vm_runtime_log_enabled(void) {
    const char *value = getenv("LAMP_VM_LOG");
    return value && value[0] != '\0' && value[0] != '0';
}

#define VM_RUNTIME_LOG(...)                         \
    do {                                            \
        if (vm_runtime_log_enabled()) {             \
            fprintf(stderr, __VA_ARGS__);           \
        }                                           \
    } while (0)

#endif
