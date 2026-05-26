#ifndef VM_ETHER_BACKEND_H
#define VM_ETHER_BACKEND_H

#include <stdint.h>

/* Abstract ethernet backend — pluggable implementations (null, udp, etc.) */
typedef struct ether_backend {
    int  (*init)(void *state, const uint8_t mac[6]);
    int  (*send)(void *state, const uint8_t *frame, uint32_t len);
    int  (*recv)(void *state, uint8_t *frame, uint32_t max);
    void (*poll)(void *state);
    void (*close)(void *state);
    void *state;
} ether_backend_t;

/* Built-in backends */
int ether_backend_null_create(ether_backend_t *out);
int ether_backend_udp_create(ether_backend_t *out, uint16_t bind_port, uint16_t peer_port);
int ether_backend_nat_create(ether_backend_t *out);

#endif
