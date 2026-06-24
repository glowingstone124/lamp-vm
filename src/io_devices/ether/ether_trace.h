#ifndef LAMP_VM_ETHER_TRACE_H
#define LAMP_VM_ETHER_TRACE_H

#include <stdlib.h>

static inline int ether_trace_enabled(void) {
    const char *v = getenv("LAMP_NET_TRACE");
    return v && v[0] && v[0] != '0';
}

#endif
