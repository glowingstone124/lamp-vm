#include "ether_backend.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ETHER_MTU 2048
#define UDP_RX_QUEUE_LEN 16

typedef struct {
    uint16_t len;
    uint8_t data[ETHER_MTU];
} udp_rx_frame_t;

typedef struct {
    int fd;
    struct sockaddr_in peer;
    udp_rx_frame_t rx_queue[UDP_RX_QUEUE_LEN];
    uint32_t rx_head;
    uint32_t rx_tail;
    uint32_t rx_count;
} udp_state_t;

static int udp_init(void *state, const uint8_t mac[6]) {
    udp_state_t *s = (udp_state_t *)state;
    (void)mac;
    int flags = fcntl(s->fd, F_GETFL, 0);
    if (flags >= 0) fcntl(s->fd, F_SETFL, flags | O_NONBLOCK);
    s->rx_head = 0u;
    s->rx_tail = 0u;
    s->rx_count = 0u;
    return 0;
}

static int udp_send(void *state, const uint8_t *frame, uint32_t len) {
    udp_state_t *s = (udp_state_t *)state;
    ssize_t n = sendto(s->fd, frame, len, 0,
                       (const struct sockaddr *)&s->peer, sizeof(s->peer));
    return (n == (ssize_t)len) ? 0 : -1;
}

static int udp_recv(void *state, uint8_t *frame, uint32_t max) {
    udp_state_t *s = (udp_state_t *)state;
    if (s->rx_count == 0u) return 0;
    udp_rx_frame_t *slot = &s->rx_queue[s->rx_head];
    uint32_t n = slot->len;
    if (n > max) n = max;
    memcpy(frame, slot->data, n);
    slot->len = 0u;
    s->rx_head = (s->rx_head + 1u) % UDP_RX_QUEUE_LEN;
    s->rx_count--;
    return (int)n;
}

static void udp_poll(void *state) {
    udp_state_t *s = (udp_state_t *)state;
    while (s->rx_count < UDP_RX_QUEUE_LEN) {
        struct sockaddr_in source;
        socklen_t source_len = sizeof(source);
        udp_rx_frame_t *slot = &s->rx_queue[s->rx_tail];
        ssize_t n = recvfrom(s->fd, slot->data, ETHER_MTU, 0,
                             (struct sockaddr *)&source, &source_len);
        if (n <= 0) break;
        if (source.sin_family != AF_INET ||
            source.sin_addr.s_addr != s->peer.sin_addr.s_addr ||
            source.sin_port != s->peer.sin_port) {
            continue;
        }
        slot->len = (uint16_t)n;
        s->rx_tail = (s->rx_tail + 1u) % UDP_RX_QUEUE_LEN;
        s->rx_count++;
    }
}

static void udp_close(void *state) {
    udp_state_t *s = (udp_state_t *)state;
    if (s->fd >= 0) close(s->fd);
    free(s);
}

int ether_backend_udp_create(ether_backend_t *out, uint16_t bind_port, uint16_t peer_port) {
    udp_state_t *s = calloc(1, sizeof(*s));
    if (!s) return -1;

    s->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (s->fd < 0) { perror("ether_udp socket"); free(s); return -1; }

    struct sockaddr_in bind_addr = {0};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind_addr.sin_port = htons(bind_port);

    if (bind(s->fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        perror("ether_udp bind"); close(s->fd); free(s); return -1;
    }

    s->peer.sin_family = AF_INET;
    s->peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    s->peer.sin_port = htons(peer_port);

    out->init  = udp_init;
    out->send  = udp_send;
    out->recv  = udp_recv;
    out->poll  = udp_poll;
    out->close = udp_close;
    out->state = s;
    return 0;
}
