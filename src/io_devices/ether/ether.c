#include "ether.h"
#include "ether_backend.h"
#include "ether_trace.h"
#include "../../interrupt.h"
#include "../../vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Per-VM ethernet state, stored in vm->ether */
typedef struct {
    ether_backend_t backend;
    uint8_t mac[6];
    uint32_t tx_len;
    uint32_t tx_lo;
    uint32_t rx_len;
    uint32_t rx_lo;
    uint32_t status;
    int irq;
    int active;
} ether_state_t;

#define ETHER_MMIO_BASE 0x00750000u

static void ether_pump_rx(VM *vm, ether_state_t *e) {
    if (!e || !e->active) return;

    if (e->backend.poll) e->backend.poll(e->backend.state);
    if (e->rx_len != 0 || e->rx_lo == 0 || e->rx_lo >= vm->memory_size) return;

    uint8_t frame[ETHER_MTU];
    int n = 0;
    if (e->backend.recv) n = e->backend.recv(e->backend.state, frame, ETHER_MTU);
    if (n <= 0) return;
    if (ether_trace_enabled()) {
        fprintf(stderr, "[ether] pump frame len=%d rx_lo=0x%08x\n", n, e->rx_lo);
    }

    uint32_t len = (uint32_t)n;
    if (len > ETHER_MTU) len = ETHER_MTU;
    if (len > vm->memory_size - e->rx_lo) {
        len = (uint32_t)(vm->memory_size - e->rx_lo);
    }
    memcpy(&vm->memory[e->rx_lo], frame, len);
    e->rx_len = len;
    e->status |= ETHER_STATUS_RX_READY;
}

/* ---- MMIO read handler ---- */
static uint32_t ether_mmio_read32(VM *vm, uint32_t addr) {
    ether_state_t *e = (ether_state_t *)vm->ether;
    if (!e) return 0;
    uint32_t off = addr - ETHER_MMIO_BASE;

    if (off == ETHER_OFF_RX_LEN || off == ETHER_OFF_STATUS) {
        ether_pump_rx(vm, e);
    }

    switch (off) {
    case ETHER_OFF_TX_LO:   return e->tx_lo;
    case ETHER_OFF_RX_LEN:  return e->rx_len;
    case ETHER_OFF_RX_LO:   return e->rx_lo;
    case ETHER_OFF_STATUS:  return e->status;
    case ETHER_OFF_MAC_LO:
        return ((uint32_t)e->mac[0]) | ((uint32_t)e->mac[1] << 8) |
               ((uint32_t)e->mac[2] << 16) | ((uint32_t)e->mac[3] << 24);
    case ETHER_OFF_MAC_HI:
        return ((uint32_t)e->mac[4]) | ((uint32_t)e->mac[5] << 8);
    default: return 0;
    }
}

/* ---- MMIO write handler ---- */
static void ether_mmio_write32(VM *vm, uint32_t addr, uint32_t val) {
    ether_state_t *e = (ether_state_t *)vm->ether;
    if (!e) return;
    uint32_t off = addr - ETHER_MMIO_BASE;

    switch (off) {
    case ETHER_OFF_TX_LEN:
        /* TX: copy frame from guest memory, send via backend */
        if (val > 0 && val <= ETHER_MTU &&
            e->tx_lo < vm->memory_size &&
            val <= vm->memory_size - e->tx_lo &&
            e->backend.send) {
            uint8_t frame[ETHER_MTU];
            memcpy(frame, &vm->memory[e->tx_lo], val);
            e->backend.send(e->backend.state, frame, val);
        }
        break;
    case ETHER_OFF_TX_LO:
        e->tx_lo = val;
        break;
    case ETHER_OFF_RX_LO:
        e->rx_lo = val;
        /* Acknowledge RX: guest has processed the previous packet */
        if (ether_trace_enabled() && e->rx_len != 0) {
            fprintf(stderr, "[ether] ack rx len=%u rx_lo=0x%08x\n", e->rx_len, e->rx_lo);
        }
        e->rx_len = 0;
        e->status &= ~ETHER_STATUS_RX_READY;
        break;
    default:
        break;
    }
}

/* ---- Public API ---- */

int ether_init(VM *vm, ether_backend_t *backend) {
    ether_state_t *e = calloc(1, sizeof(*e));
    if (!e) return -1;

    e->mac[0] = ETHER_MAC_BYTE0; e->mac[1] = ETHER_MAC_BYTE1;
    e->mac[2] = ETHER_MAC_BYTE2; e->mac[3] = ETHER_MAC_BYTE3;
    e->mac[4] = ETHER_MAC_BYTE4; e->mac[5] = ETHER_MAC_BYTE5;
    e->status = ETHER_STATUS_LINK;
    e->irq    = INT_ETHER;
    e->active  = 1;
    memcpy(&e->backend, backend, sizeof(*backend));

    if (e->backend.init && e->backend.init(e->backend.state, e->mac) != 0) {
        if (e->backend.close) e->backend.close(e->backend.state);
        free(e);
        return -1;
    }

    vm->ether = e;

    /* Register MMIO device */
    static MMIO_Device dev;
    dev.start  = ETHER_MMIO_BASE;
    dev.end    = ETHER_MMIO_BASE + ETHER_MMIO_SIZE - 1;
    dev.read32  = ether_mmio_read32;
    dev.write32 = ether_mmio_write32;
    if (vm->mmio_count < MAX_MMIO_DEVICES) {
        vm->mmio_devices[vm->mmio_count++] = &dev;
        printf("Registered VM Ether to MMIO ID %d (MAC=%02x:%02x:%02x:%02x:%02x:%02x)\n",
               vm->mmio_count, e->mac[0],e->mac[1],e->mac[2],e->mac[3],e->mac[4],e->mac[5]);
    }
    return 0;
}

void ether_shutdown(VM *vm) {
    ether_state_t *e = (ether_state_t *)vm->ether;
    if (!e) return;
    e->active = 0;
    if (e->backend.close) e->backend.close(e->backend.state);
    free(e);
    vm->ether = NULL;
}

void ether_poll(VM *vm) {
    ether_state_t *e = (ether_state_t *)vm->ether;
    if (!e || !e->active) return;
    ether_pump_rx(vm, e);
}
