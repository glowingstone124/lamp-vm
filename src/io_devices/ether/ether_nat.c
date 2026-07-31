/* ether_nat.c — User-mode NAT backend
 *
 * Guest network: 10.0.2.0/24
 *   Guest IP:   10.0.2.15
 *   Gateway:    10.0.2.2  (this host)
 *   DNS:        10.0.2.3  (proxied to 8.8.8.8)
 *
 * Supports: ARP, ICMP echo, UDP forwarding, TCP connect-forward.
 * Works cross-platform without root — uses regular POSIX sockets.
 */
#include "ether_backend.h"
#include "ether_trace.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#define NAT_MTU       2048
#define NAT_TCP_MAX   32
#define NAT_UDP_MAX   16
#define NAT_TCP_MSS   1460
#define NAT_RX_QUEUE_LEN 16

enum {
    NAT_CONNECT_TIMEOUT_MS = 1000,
    NAT_SEND_TIMEOUT_MS = 1000,
    NAT_IMMEDIATE_RX_WAIT_MS = 100
};

/* Ethernet header */
typedef struct __attribute__((packed)) {
    uint8_t dst[6];
    uint8_t src[6];
    uint16_t ethertype;
} eth_hdr_t;

/* ARP header */
typedef struct __attribute__((packed)) {
    uint16_t htype, ptype;
    uint8_t hlen, plen;
    uint16_t op;
    uint8_t sha[6];
    uint32_t spa;
    uint8_t tha[6];
    uint32_t tpa;
} arp_pkt_t;

/* ICMP echo */
typedef struct __attribute__((packed)) {
    uint8_t type, code;
    uint16_t csum, id, seq;
} icmp_echo_t;

enum { NAT_TCP_CLOSED, NAT_TCP_SYNSENT, NAT_TCP_CONNECTED, NAT_TCP_CLOSING };

typedef struct {
    int fd;
    uint8_t state;
    uint32_t guest_ip;
    uint16_t guest_port;
    uint16_t host_port;
    uint32_t remote_ip;
    uint16_t remote_port;
    uint32_t guest_seq, remote_seq;
    uint32_t guest_ack, remote_ack;
    uint8_t guest_mac[6];
    uint8_t tx_buf[NAT_MTU];
    int tx_len;
    uint8_t rx_buf[NAT_MTU];
    int rx_len;
} tcp_conn_t;

typedef struct {
    int fd;
    uint32_t guest_ip;
    uint16_t guest_port;
    uint16_t host_port;
    uint32_t remote_ip;
    uint16_t remote_port;
    uint8_t guest_mac[6];
    uint8_t rx_buf[NAT_MTU];
    int rx_len;
    struct sockaddr_in rx_from;
    socklen_t rx_from_len;
} udp_map_t;

typedef struct {
    uint16_t len;
    uint8_t data[NAT_MTU];
} nat_rx_frame_t;

typedef struct {
    uint8_t mac[6];
    nat_rx_frame_t rx_queue[NAT_RX_QUEUE_LEN];
    uint32_t rx_head;
    uint32_t rx_tail;
    uint32_t rx_count;

    tcp_conn_t tcp[NAT_TCP_MAX];
    udp_map_t udp[NAT_UDP_MAX];

    /* gateway info */
    uint32_t gw_ip, guest_ip, dns_ip, netmask;
    uint8_t gw_mac[6];
} nat_state_t;

/* ---- checksum ---- */
static uint32_t cksum_add(const void *data, int len, uint32_t sum) {
    const uint8_t *p = (const uint8_t *) data;
    while (len > 1) {
        uint16_t w;
        memcpy(&w, p, sizeof(w));
        sum += w;
        p += 2;
        len -= 2;
    }

    if (len > 0) {
        uint16_t w = 0;
        memcpy(&w, p, sizeof(w));
        sum += w;
    }
    return sum;
}

static uint16_t cksum_finish(uint32_t sum) {
    while (sum >> 16) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return (uint16_t) ~sum;
}

static uint16_t cksum(const void *data, int len, uint32_t start) {
    return cksum_finish(cksum_add(data, len, start));
}

static uint16_t ip_cksum(const uint8_t *iphdr) {
    const struct ip *ip = (struct ip *) iphdr;
    return cksum(iphdr, ip->ip_hl * 4, 0);
}

typedef struct __attribute__((packed)) {
    uint32_t src;
    uint32_t dst;
    uint8_t zero;
    uint8_t proto;
    uint16_t len;
} pseudo_hdr_t;

static uint16_t transport_cksum(
    uint32_t src_ip_hbo,
    uint32_t dst_ip_hbo,
    uint8_t proto,
    const void *seg,
    int seg_len
) {
    pseudo_hdr_t pseudo_hdr;
    pseudo_hdr.src = htonl(src_ip_hbo);
    pseudo_hdr.dst = htonl(dst_ip_hbo);
    pseudo_hdr.proto = proto;
    pseudo_hdr.zero = 0;
    pseudo_hdr.len = htons((uint16_t)seg_len);

    uint32_t sum = 0;
    sum = cksum_add(&pseudo_hdr, sizeof(pseudo_hdr), sum);
    sum = cksum_add(seg, seg_len, sum);
    return cksum_finish(sum);
}

/* ---- helpers ---- */
static int nat_set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    return (fl >= 0) ? fcntl(fd, F_SETFL, fl | O_NONBLOCK) : -1;
}

static uint32_t nat_make_ip(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return ((uint32_t) a << 24) | ((uint32_t) b << 16) | ((uint32_t) c << 8) | d;
}

static int nat_rx_enqueue(nat_state_t *ns, const uint8_t *frame, int len) {
    if (!ns || !frame || len <= 0 || len > NAT_MTU) return -1;
    if (ns->rx_count >= NAT_RX_QUEUE_LEN) {
        if (ether_trace_enabled()) {
            fprintf(stderr, "[NAT] RX queue full, dropping frame len=%d\n", len);
        }
        return -1;
    }
    nat_rx_frame_t *slot = &ns->rx_queue[ns->rx_tail];
    memcpy(slot->data, frame, (size_t)len);
    slot->len = (uint16_t)len;
    ns->rx_tail = (ns->rx_tail + 1u) % NAT_RX_QUEUE_LEN;
    ns->rx_count++;
    return 0;
}

static int nat_rx_dequeue(nat_state_t *ns, uint8_t *frame, uint32_t max) {
    if (!ns || !frame || ns->rx_count == 0u) return 0;
    nat_rx_frame_t *slot = &ns->rx_queue[ns->rx_head];
    uint32_t len = slot->len;
    uint32_t copied = len > max ? max : len;
    memcpy(frame, slot->data, copied);
    slot->len = 0u;
    ns->rx_head = (ns->rx_head + 1u) % NAT_RX_QUEUE_LEN;
    ns->rx_count--;
    return (int)copied;
}

/* ---- ARP handler ---- */
static int nat_handle_arp(nat_state_t *ns, const uint8_t *frame, int len,
                          uint8_t *out, int *out_len) {
    const arp_pkt_t *arp = (const arp_pkt_t *) (frame + sizeof(eth_hdr_t));
    if (len < (int) (sizeof(eth_hdr_t) + sizeof(arp_pkt_t))) return 0;
    if (ntohs(arp->op) != 1) return 0; /* only ARP request */
    uint32_t target_ip = ntohl(arp->tpa);
    if (target_ip != ns->gw_ip && target_ip != ns->dns_ip) return 0;

    /* Build reply */
    eth_hdr_t *eth_out = (eth_hdr_t *) out;
    memcpy(eth_out->dst, arp->sha, 6);
    memcpy(eth_out->src, ns->gw_mac, 6);
    eth_out->ethertype = htons(0x0806);

    arp_pkt_t *arp_out = (arp_pkt_t *) (out + sizeof(eth_hdr_t));
    arp_out->htype = htons(1);
    arp_out->ptype = htons(0x0800);
    arp_out->hlen = 6;
    arp_out->plen = 4;
    arp_out->op = htons(2);
    memcpy(arp_out->sha, ns->gw_mac, 6);
    arp_out->spa = arp->tpa;
    memcpy(arp_out->tha, arp->sha, 6);
    arp_out->tpa = arp->spa;
    *out_len = sizeof(eth_hdr_t) + sizeof(arp_pkt_t);
    return 1;
}

/* ---- ICMP handler ---- */
static int nat_handle_icmp(nat_state_t *ns, const eth_hdr_t *eth,
                           const uint8_t *iphdr, int ip_len,
                           uint8_t *out, int *out_len) {
    struct ip *ip = (struct ip *) iphdr;
    icmp_echo_t *icmp = (icmp_echo_t *) (iphdr + ip->ip_hl * 4);
    int icmp_len = ip_len - ip->ip_hl * 4;
    if (icmp_len < 8 || icmp->type != 8) return 0; /* only echo request */

    /* Build reply */
    eth_hdr_t *eth_out = (eth_hdr_t *) out;
    memcpy(eth_out->dst, eth->src, 6);
    memcpy(eth_out->src, ns->gw_mac, 6);
    eth_out->ethertype = htons(0x0800);

    struct ip *ip_out = (struct ip *) (out + sizeof(eth_hdr_t));
    int rip_len = sizeof(struct ip) + icmp_len;
    memcpy(ip_out, ip, rip_len);
    ip_out->ip_len = htons((uint16_t)rip_len);
    ip_out->ip_src.s_addr = ip->ip_dst.s_addr;
    ip_out->ip_dst.s_addr = ip->ip_src.s_addr;
    ip_out->ip_sum = 0;
    ip_out->ip_sum = ip_cksum((uint8_t *) ip_out);

    icmp_echo_t *icmp_out = (icmp_echo_t *) ((uint8_t *) ip_out + sizeof(struct ip));
    icmp_out->type = 0;
    icmp_out->csum = 0;
    icmp_out->csum = cksum(icmp_out, icmp_len, 0);

    *out_len = sizeof(eth_hdr_t) + rip_len;
    return 1;
}

/* ---- helpers for TCP/UDP NAT ---- */
static void nat_build_ipv4_base(uint8_t *out,
                                const uint8_t *dst_mac,
                                const uint8_t *src_mac,
                                uint32_t src_ip,
                                uint32_t dst_ip,
                                uint8_t proto,
                                int transport_len) {
    eth_hdr_t *eth_out = (eth_hdr_t *) out;
    memcpy(eth_out->dst, dst_mac, 6);
    memcpy(eth_out->src, src_mac, 6);
    eth_out->ethertype = htons(0x0800);
    struct ip *ip = (struct ip *) (out + sizeof(eth_hdr_t));
    memset(ip, 0, sizeof(*ip));

    ip->ip_hl = 5;
    ip->ip_v = 4;
    ip->ip_len = htons((uint16_t)(sizeof(struct ip) + transport_len));
    ip->ip_ttl = 64;
    ip->ip_p = proto;
    ip->ip_src.s_addr = htonl(src_ip);
    ip->ip_dst.s_addr = htonl(dst_ip);
    ip->ip_sum = 0;
    ip->ip_sum = ip_cksum((uint8_t *) ip);
}

static void nat_build_ip_eth(uint8_t *out,
                             const uint8_t *dst_mac,
                             const uint8_t *src_mac,
                             uint32_t src_ip,
                             uint32_t dst_ip,
                             uint8_t proto,
                             const void *transport,
                             int transport_len) {
    nat_build_ipv4_base(out, dst_mac, src_mac, src_ip, dst_ip, proto, transport_len);
    uint8_t *seg = out + sizeof(eth_hdr_t) + sizeof(struct ip);
    memcpy(seg, transport, (size_t)transport_len);

    if (proto == IPPROTO_TCP && transport_len >= (int)sizeof(struct tcphdr)) {
        struct tcphdr *tcp = (struct tcphdr *)seg;
        tcp->th_sum = 0;
        tcp->th_sum = transport_cksum(src_ip, dst_ip, proto, seg, transport_len);
    } else if (proto == IPPROTO_UDP && transport_len >= (int)sizeof(struct udphdr)) {
        struct udphdr *udp = (struct udphdr *)seg;
        udp->uh_sum = 0;
        udp->uh_sum = transport_cksum(src_ip, dst_ip, proto, seg, transport_len);
    }
}

/* ---- TCP NAT ---- */
static tcp_conn_t *nat_tcp_find_by_guest(nat_state_t *ns, uint16_t guest_port) {
    for (int i = 0; i < NAT_TCP_MAX; i++)
        if (ns->tcp[i].state != NAT_TCP_CLOSED && ns->tcp[i].guest_port == guest_port)
            return &ns->tcp[i];
    return NULL;
}

static tcp_conn_t *nat_tcp_alloc(nat_state_t *ns) {
    for (int i = 0; i < NAT_TCP_MAX; i++)
        if (ns->tcp[i].state == NAT_TCP_CLOSED) return &ns->tcp[i];
    return NULL;
}

static void nat_tcp_close(tcp_conn_t *c) {
    if (c->fd >= 0) close(c->fd);
    memset(c, 0, sizeof(*c));
    c->fd = -1;
    c->state = NAT_TCP_CLOSED;
}

static int nat_tcp_wait_writable(int fd, int timeout_ms) {
    fd_set wfds;
    struct timeval tv;

    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int rc = select(fd + 1, NULL, &wfds, NULL, &tv);
    return (rc > 0 && FD_ISSET(fd, &wfds)) ? 0 : -1;
}

static int nat_tcp_wait_readable(int fd, int timeout_ms) {
    fd_set rfds;
    struct timeval tv;

    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int rc = select(fd + 1, &rfds, NULL, NULL, &tv);
    return (rc > 0 && FD_ISSET(fd, &rfds)) ? 0 : -1;
}

static int nat_tcp_finish_connect(tcp_conn_t *c, int timeout_ms) {
    int soerr = 0;
    socklen_t len = sizeof(soerr);

    if (c->state == NAT_TCP_CONNECTED) return 0;
    if (c->state != NAT_TCP_SYNSENT) return -1;
    if (nat_tcp_wait_writable(c->fd, timeout_ms) < 0) return -1;
    if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &soerr, &len) < 0) return -1;
    if (soerr != 0) {
        errno = soerr;
        return -1;
    }
    c->state = NAT_TCP_CONNECTED;
    return 0;
}

static int nat_tcp_send_all(tcp_conn_t *c, const uint8_t *payload, int len) {
    int sent_total = 0;

    while (sent_total < len) {
        ssize_t sent;
        if (nat_tcp_wait_writable(c->fd, NAT_SEND_TIMEOUT_MS) < 0) break;
        sent = send(c->fd, payload + sent_total, (size_t)(len - sent_total), 0);
        if (sent > 0) {
            sent_total += (int)sent;
            continue;
        }
        if (sent < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        break;
    }
    return sent_total;
}

static int nat_handle_tcp(nat_state_t *ns, const uint8_t *frame, int len,
                          const eth_hdr_t *eth, const struct ip *ip,
                          uint8_t *out, int *out_len) {
    (void)frame;
    (void)len;
    int ip_hdr_len = ip->ip_hl * 4;
    int tcp_len = ntohs(ip->ip_len) - ip_hdr_len;
    if (tcp_len < 20) return 0;
    struct tcphdr *tcp = (struct tcphdr *) ((const uint8_t *) ip + ip_hdr_len);
    uint16_t guest_port = ntohs(tcp->th_sport);
    uint32_t remote_ip = ntohl(ip->ip_dst.s_addr);
    uint16_t remote_port = ntohs(tcp->th_dport);

    /* Handle SYN: new connection */
    if ((tcp->th_flags & TH_SYN) && !(tcp->th_flags & TH_ACK)) {
        tcp_conn_t *c = nat_tcp_alloc(ns);
        if (!c) return 0; /* table full */

        c->fd = socket(AF_INET, SOCK_STREAM, 0);
        if (c->fd < 0) return 0;
        nat_set_nonblock(c->fd);

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(remote_ip == ns->gw_ip ? nat_make_ip(127, 0, 0, 1) : remote_ip),
        .sin_port = htons(remote_port)
    };
        int rc = connect(c->fd, (struct sockaddr *) &addr, sizeof(addr));
        if (rc < 0 && errno != EINPROGRESS) {
            close(c->fd);
            return 0;
        }

        c->state = NAT_TCP_SYNSENT;
        c->guest_ip = ntohl(ip->ip_src.s_addr);
        c->guest_port = guest_port;
        c->remote_ip = remote_ip;
        c->remote_port = remote_port;
        c->guest_seq = ntohl(tcp->th_seq);
        c->guest_ack = c->guest_seq + 1;
        c->remote_seq = 0x10000000u;
        memcpy(c->guest_mac, eth->src, sizeof(c->guest_mac));
        /* Send SYN-ACK back to guest */
        struct tcphdr synack = {0};
        synack.th_sport = tcp->th_dport;
        synack.th_dport = tcp->th_sport;
        synack.th_seq = htonl(c->remote_seq);
        synack.th_ack = htonl(c->guest_ack);
        synack.th_flags = TH_SYN | TH_ACK;
        synack.th_off = 5;
        synack.th_win = htons(65535);
        nat_build_ip_eth(out, eth->src, ns->gw_mac,
                         remote_ip, c->guest_ip,
                         IPPROTO_TCP, &synack, sizeof(synack));
        struct ip *ip_out = (struct ip *) (out + sizeof(eth_hdr_t));
        struct tcphdr *tcp_out = (struct tcphdr *) (out + sizeof(eth_hdr_t) + sizeof(struct ip));
        uint16_t plen = htons(sizeof(struct tcphdr));
        tcp_out->th_sum = 0;
        tcp_out->th_sum = cksum(tcp_out, sizeof(*tcp_out),
                                cksum(&ip_out->ip_src, 8, IPPROTO_TCP + ntohs(plen)));
        ip_out->ip_len = htons(sizeof(struct ip) + sizeof(struct tcphdr));
        ip_out->ip_sum = 0;
        ip_out->ip_sum = ip_cksum((uint8_t *) ip_out);
        *out_len = sizeof(eth_hdr_t) + sizeof(struct ip) + sizeof(struct tcphdr);
        c->remote_seq++;
        return 1;
    }

    /* Handle data/ACK on existing connection */
    tcp_conn_t *c = nat_tcp_find_by_guest(ns, guest_port);
    if (!c || (c->state != NAT_TCP_CONNECTED && c->state != NAT_TCP_SYNSENT)) return 0;

    if (tcp->th_flags & TH_ACK) {
        c->remote_ack = ntohl(tcp->th_ack);
    }

    if (tcp->th_flags & TH_FIN) {
        /* Send FIN-ACK, close */
        struct tcphdr finack = {0};
        finack.th_sport = htons(c->remote_port);
        finack.th_dport = htons(guest_port);
        finack.th_seq = htonl(c->remote_seq);
        finack.th_ack = htonl(ntohl(tcp->th_seq) + 1);
        finack.th_flags = TH_FIN | TH_ACK;
        finack.th_off = 5;
        finack.th_win = htons(65535);
        nat_build_ip_eth(out, eth->src, ns->gw_mac,
                         c->remote_ip, c->guest_ip,
                         IPPROTO_TCP, &finack, sizeof(finack));
        *out_len = sizeof(eth_hdr_t) + sizeof(struct ip) + sizeof(struct tcphdr);
        nat_tcp_close(c);
        return 1;
    }

    /* Forward data to remote */
    int data_off = tcp->th_off * 4;
    int data_len = tcp_len - data_off;
    if (data_len > 0) {
        const uint8_t *payload = ((const uint8_t *) tcp) + data_off;
        int sent;
        if (nat_tcp_finish_connect(c, NAT_CONNECT_TIMEOUT_MS) < 0) {
            nat_tcp_close(c);
            return 0;
        }
        sent = nat_tcp_send_all(c, payload, data_len);
        if (sent <= 0) {
            nat_tcp_close(c);
            return 0;
        }
        if (ether_trace_enabled()) {
            fprintf(stderr, "[NAT tcp] forward guest_port=%u len=%d sent=%d\n",
                    guest_port, data_len, sent);
        }
        c->guest_ack = ntohl(tcp->th_seq) + (uint32_t)sent;
        if (nat_tcp_wait_readable(c->fd, NAT_IMMEDIATE_RX_WAIT_MS) == 0) {
            c->rx_len = (int) recv(c->fd, c->rx_buf, NAT_TCP_MSS, 0);
            if (c->rx_len > 0) {
                uint8_t segment[NAT_MTU];
                struct tcphdr *push = (struct tcphdr *)segment;
                int seg_len;
                memset(segment, 0, sizeof(struct tcphdr));
                push->th_sport = htons(c->remote_port);
                push->th_dport = htons(guest_port);
                push->th_seq = htonl(c->remote_seq);
                push->th_ack = htonl(c->guest_ack);
                push->th_flags = TH_ACK | TH_PUSH;
                push->th_off = 5;
                push->th_win = htons(65535);
                seg_len = (int)sizeof(struct tcphdr) + c->rx_len;
                memcpy(segment + sizeof(struct tcphdr), c->rx_buf, (size_t)c->rx_len);
                nat_build_ip_eth(out, eth->src, ns->gw_mac,
                                 c->remote_ip, c->guest_ip,
                                 IPPROTO_TCP, segment, seg_len);
                *out_len = sizeof(eth_hdr_t) + sizeof(struct ip) + seg_len;
                if (ether_trace_enabled()) {
                    fprintf(stderr, "[NAT tcp] immediate data guest_port=%u len=%d frame=%d\n",
                            guest_port, c->rx_len, *out_len);
                }
                c->remote_seq += (uint32_t)c->rx_len;
                c->rx_len = 0;
                return 1;
            }
            if (c->rx_len == 0) {
                c->state = NAT_TCP_CLOSING;
            }
        }
        return 0;
    }

    /* Poll remote for response data */
    if (c->rx_len == 0) {
        if (nat_tcp_finish_connect(c, 0) == 0) {
            c->rx_len = (int) recv(c->fd, c->rx_buf, NAT_TCP_MSS, 0);
            if (c->rx_len == 0) {
                c->state = NAT_TCP_CLOSING;
            }
        }
    }
    if (c->rx_len > 0) {
        uint8_t segment[NAT_MTU];
        struct tcphdr *push = (struct tcphdr *)segment;
        int seg_len;
        memset(segment, 0, sizeof(struct tcphdr));
        push->th_sport = htons(c->remote_port);
        push->th_dport = htons(guest_port);
        push->th_seq = htonl(c->remote_seq);
        push->th_ack = htonl(c->guest_ack);
        push->th_flags = TH_ACK | TH_PUSH;
        push->th_off = 5;
        push->th_win = htons(65535);
        seg_len = (int)sizeof(struct tcphdr) + c->rx_len;
        memcpy(segment + sizeof(struct tcphdr), c->rx_buf, (size_t)c->rx_len);
        nat_build_ip_eth(out, eth->src, ns->gw_mac,
                         c->remote_ip, c->guest_ip,
                         IPPROTO_TCP, segment, seg_len);
        *out_len = sizeof(eth_hdr_t) + sizeof(struct ip) + seg_len;
        c->remote_seq += (uint32_t) c->rx_len;
        c->rx_len = 0;
        return 1;
    }
    if (c->rx_len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        nat_tcp_close(c);
        return 0;
    }
    c->rx_len = 0;
    return 0;
}

/* ---- UDP NAT ---- */
static int nat_handle_udp(nat_state_t *ns, const uint8_t *frame, int len,
                          const eth_hdr_t *eth, const struct ip *ip,
                          uint8_t *out, int *out_len) {
    (void)frame;
    (void)len;
    int ip_hdr_len = ip->ip_hl * 4;
    int udp_len = ntohs(ip->ip_len) - ip_hdr_len;
    if (udp_len < 8) return 0;
    struct udphdr *udp = (struct udphdr *) ((const uint8_t *) ip + ip_hdr_len);
    uint16_t guest_port = ntohs(udp->uh_sport);
    uint16_t remote_port = ntohs(udp->uh_dport);
    uint32_t remote_ip = ntohl(ip->ip_dst.s_addr);

    /* Special: redirect DNS to 8.8.8.8:53 */
    uint32_t real_dns = remote_ip;
    uint16_t real_dport = remote_port;
    if (remote_ip == ns->dns_ip && remote_port == 53) {
        real_dns = nat_make_ip(8, 8, 8, 8);
        real_dport = 53;
    }

    /* Find or create UDP mapping */
    udp_map_t *u = NULL;
    for (int i = 0; i < NAT_UDP_MAX; i++) {
        if (ns->udp[i].fd >= 0 && ns->udp[i].guest_port == guest_port) {
            u = &ns->udp[i];
            break;
        }
    }
    if (!u) {
        for (int i = 0; i < NAT_UDP_MAX; i++) {
            if (ns->udp[i].fd < 0) {
                u = &ns->udp[i];
                u->fd = socket(AF_INET, SOCK_DGRAM, 0);
                if (u->fd < 0) return 0;
                nat_set_nonblock(u->fd);
                u->guest_port = guest_port;
                break;
            }
        }
    }
    if (!u) return 0;
    u->guest_ip = ntohl(ip->ip_src.s_addr);
    u->remote_ip = remote_ip;
    u->remote_port = remote_port;
    memcpy(u->guest_mac, eth->src, sizeof(u->guest_mac));

    /* Forward to real destination */
    struct sockaddr_in dst = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(real_dns),
        .sin_port = htons(real_dport)
    };
    int payload_len = udp_len - 8;
    const uint8_t *payload = ((const uint8_t *) udp) + 8;
    sendto(u->fd, payload, payload_len, 0, (struct sockaddr *) &dst, sizeof(dst));

    /* Poll for response */
    if (u->rx_len == 0) {
        u->rx_from_len = sizeof(u->rx_from);
        u->rx_len = (int) recvfrom(u->fd, u->rx_buf, NAT_MTU - 40, 0,
                                   (struct sockaddr *) &u->rx_from, &u->rx_from_len);
    }
    if (u->rx_len > 0) {
        uint8_t segment[NAT_MTU];
        struct udphdr *resp = (struct udphdr *)segment;
        int seg_len = (int)sizeof(struct udphdr) + u->rx_len;
        memset(resp, 0, sizeof(*resp));
        resp->uh_sport = htons(remote_port);
        resp->uh_dport = htons(guest_port);
        resp->uh_ulen = htons((uint16_t)seg_len);
        memcpy(segment + sizeof(struct udphdr), u->rx_buf, (size_t)u->rx_len);
        nat_build_ip_eth(out, eth->src, ns->gw_mac,
                         remote_ip, ntohl(ip->ip_src.s_addr),
                         IPPROTO_UDP, segment, seg_len);
        *out_len = sizeof(eth_hdr_t) + sizeof(struct ip) + seg_len;
        u->rx_len = 0;
        return 1;
    }
    if (u->rx_len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        close(u->fd);
        u->fd = -1;
    }
    u->rx_len = 0;
    return 0;
}

/* ---- NAT: forward all TCP connections to check for data ---- */
static void nat_tcp_poll_all(nat_state_t *ns, uint8_t *out, int *out_len) {
    (void)out;
    (void)out_len;
    for (int i = 0; i < NAT_TCP_MAX; i++) {
        tcp_conn_t *c = &ns->tcp[i];
        if (c->state != NAT_TCP_CONNECTED) continue;
        if (c->remote_ack < c->remote_seq) continue;
        if (c->rx_len == 0) {
            c->rx_len = (int) recv(c->fd, c->rx_buf, NAT_TCP_MSS, 0);
            if (c->rx_len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && ether_trace_enabled()) {
                static int eagain_log_budget = 8;
                if (eagain_log_budget > 0) {
                    fprintf(stderr, "[NAT tcp] remote eagain guest_port=%u\n", c->guest_port);
                    eagain_log_budget--;
                }
            }
            if (c->rx_len == 0) {
                if (ether_trace_enabled()) fprintf(stderr, "[NAT tcp] remote eof guest_port=%u\n", c->guest_port);
                c->state = NAT_TCP_CLOSING;
            } else if (c->rx_len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                c->rx_len = 0;
                continue;
            } else if (c->rx_len > 0 && ether_trace_enabled()) {
                fprintf(stderr, "[NAT tcp] remote data guest_port=%u len=%d\n", c->guest_port, c->rx_len);
            }
        }
    }
}

/* ---- Main packet handler ---- */
static int nat_handle_frame(nat_state_t *ns, const uint8_t *frame, int len,
                            uint8_t *out, int *out_len) {
    if (len < (int) sizeof(eth_hdr_t)) return 0;
    const eth_hdr_t *eth = (const eth_hdr_t *) frame;
    uint16_t etype = ntohs(eth->ethertype);

    if (etype == 0x0806) return nat_handle_arp(ns, frame, len, out, out_len);
    if (etype != 0x0800 || len < (int) (sizeof(eth_hdr_t) + sizeof(struct ip))) return 0;

    const struct ip *ip = (const struct ip *) (frame + sizeof(eth_hdr_t));
    if (ip->ip_v != 4) return 0;

    /* Route all outgoing IP traffic (not to guest itself) */
    uint32_t dst = ntohl(ip->ip_dst.s_addr);
    if (dst == ns->guest_ip) return 0; /* loopback, ignore */

    if (ip->ip_p == IPPROTO_ICMP)
        return nat_handle_icmp(ns, eth, (const uint8_t *) ip,
                               ntohs(ip->ip_len), out, out_len);
    if (ip->ip_p == IPPROTO_UDP)
        return nat_handle_udp(ns, frame, len, eth, ip, out, out_len);
    if (ip->ip_p == IPPROTO_TCP)
        return nat_handle_tcp(ns, frame, len, eth, ip, out, out_len);

    return 0;
}

/* ---- Backend API ---- */

static int nat_init(void *state, const uint8_t mac[6]) {
    nat_state_t *ns = (nat_state_t *) state;
    memcpy(ns->mac, mac, 6);
    memcpy(ns->gw_mac, mac, 6);
    ns->gw_ip = nat_make_ip(10, 0, 2, 2);
    ns->guest_ip = nat_make_ip(10, 0, 2, 15);
    ns->dns_ip = nat_make_ip(10, 0, 2, 3);
    ns->netmask = nat_make_ip(255, 255, 255, 0);
    ns->rx_head = 0u;
    ns->rx_tail = 0u;
    ns->rx_count = 0u;
    for (int i = 0; i < NAT_TCP_MAX; i++) {
        ns->tcp[i].fd = -1;
        ns->tcp[i].state = NAT_TCP_CLOSED;
    }
    for (int i = 0; i < NAT_UDP_MAX; i++) ns->udp[i].fd = -1;
    return 0;
}

static int nat_send(void *state, const uint8_t *frame, uint32_t len) {
    nat_state_t *ns = (nat_state_t *) state;
    uint8_t reply[NAT_MTU];
    int rlen = 0;
    if (nat_handle_frame(ns, frame, (int) len, reply, &rlen)) {
        return nat_rx_enqueue(ns, reply, rlen);
    }
    return 0;
}

static int nat_recv(void *state, uint8_t *frame, uint32_t max) {
    return nat_rx_dequeue((nat_state_t *)state, frame, max);
}

static void nat_poll(void *state) {
    nat_state_t *ns = (nat_state_t *) state;
    uint8_t dummy[NAT_MTU];
    int dlen = 0;
    /* Poll remote sockets first, then queue ready frames for the guest. */
    nat_tcp_poll_all(ns, dummy, &dlen);
    for (int i = 0; i < NAT_TCP_MAX && ns->rx_count < NAT_RX_QUEUE_LEN; i++) {
        tcp_conn_t *c = &ns->tcp[i];
        if ((c->state == NAT_TCP_CONNECTED && c->rx_len > 0) || c->state == NAT_TCP_CLOSING) {
            uint8_t segment[NAT_MTU];
            uint8_t frame[NAT_MTU];
            struct tcphdr *push = (struct tcphdr *)segment;
            int copy = (c->state == NAT_TCP_CLOSING) ? 0 : c->rx_len;
            int seg_len;
            int frame_len;
            if ((int)sizeof(struct tcphdr) + copy > NAT_MTU) {
                copy = NAT_MTU - (int)sizeof(struct tcphdr);
            }
            memset(push, 0, sizeof(*push));
            push->th_sport = htons(c->remote_port);
            push->th_dport = htons(c->guest_port);
            push->th_seq = htonl(c->remote_seq);
            push->th_ack = htonl(c->guest_ack);
            push->th_flags = TH_ACK | (copy > 0 ? TH_PUSH : TH_FIN);
            push->th_off = 5;
            push->th_win = htons(65535);
            if (copy > 0) {
                memcpy(segment + sizeof(struct tcphdr), c->rx_buf, (size_t)copy);
            }
            seg_len = (int)sizeof(struct tcphdr) + copy;
            nat_build_ip_eth(frame, c->guest_mac, ns->gw_mac,
                             c->remote_ip, c->guest_ip,
                             IPPROTO_TCP, segment, seg_len);
            frame_len = (int)sizeof(eth_hdr_t) + (int)sizeof(struct ip) + seg_len;
            if (nat_rx_enqueue(ns, frame, frame_len) != 0) break;
            if (ether_trace_enabled()) {
                fprintf(stderr, "[NAT tcp] queue guest_port=%u payload=%d flags=0x%02x frame=%d depth=%u\n",
                        c->guest_port, copy, push->th_flags, frame_len, ns->rx_count);
            }
            c->remote_seq += (uint32_t)copy;
            if (c->state == NAT_TCP_CLOSING) {
                c->remote_seq++;
                nat_tcp_close(c);
            }
            c->rx_len = 0;
        }
    }
    for (int i = 0; i < NAT_UDP_MAX && ns->rx_count < NAT_RX_QUEUE_LEN; i++) {
        udp_map_t *u = &ns->udp[i];
        if (u->fd < 0 || u->guest_port == 0u) continue;
        u->rx_from_len = sizeof(u->rx_from);
        u->rx_len = (int)recvfrom(u->fd, u->rx_buf, NAT_MTU - 64, 0,
                                  (struct sockaddr *)&u->rx_from, &u->rx_from_len);
        if (u->rx_len <= 0) {
            if (u->rx_len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                close(u->fd);
                u->fd = -1;
            }
            u->rx_len = 0;
            continue;
        }

        uint8_t segment[NAT_MTU];
        uint8_t frame[NAT_MTU];
        struct udphdr *resp = (struct udphdr *)segment;
        int seg_len = (int)sizeof(struct udphdr) + u->rx_len;
        int frame_len = (int)sizeof(eth_hdr_t) + (int)sizeof(struct ip) + seg_len;
        memset(resp, 0, sizeof(*resp));
        resp->uh_sport = htons(u->remote_port);
        resp->uh_dport = htons(u->guest_port);
        resp->uh_ulen = htons((uint16_t)seg_len);
        memcpy(segment + sizeof(struct udphdr), u->rx_buf, (size_t)u->rx_len);
        nat_build_ip_eth(frame, u->guest_mac, ns->gw_mac,
                         u->remote_ip, u->guest_ip,
                         IPPROTO_UDP, segment, seg_len);
        (void)nat_rx_enqueue(ns, frame, frame_len);
        u->rx_len = 0;
    }
}

static void nat_close(void *state) {
    nat_state_t *ns = (nat_state_t *) state;
    for (int i = 0; i < NAT_TCP_MAX; i++) nat_tcp_close(&ns->tcp[i]);
    for (int i = 0; i < NAT_UDP_MAX; i++) {
        if (ns->udp[i].fd >= 0) close(ns->udp[i].fd);
    }
    free(ns);
}

int ether_backend_nat_create(ether_backend_t *out) {
    nat_state_t *ns = calloc(1, sizeof(*ns));
    if (!ns) return -1;
    out->init = nat_init;
    out->send = nat_send;
    out->recv = nat_recv;
    out->poll = nat_poll;
    out->close = nat_close;
    out->state = ns;
    return 0;
}
