#include "ether_backend.h"
#include <stdlib.h>

typedef struct { int dummy; } null_state_t;

static int null_init(void *state, const uint8_t mac[6]) {
    (void)state; (void)mac; return 0;
}
static int null_send(void *state, const uint8_t *frame, uint32_t len) {
    (void)state; (void)frame; (void)len; return 0;
}
static int null_recv(void *state, uint8_t *frame, uint32_t max) {
    (void)state; (void)frame; (void)max; return 0;
}
static void null_poll(void *state) { (void)state; }
static void null_close(void *state) { free(state); }

int ether_backend_null_create(ether_backend_t *out) {
    null_state_t *s = calloc(1, sizeof(*s));
    if (!s) return -1;
    out->init  = null_init;
    out->send  = null_send;
    out->recv  = null_recv;
    out->poll  = null_poll;
    out->close = null_close;
    out->state = s;
    return 0;
}
