#include "ether.h"
#include "ether_backend.h"
#include "ether_trace.h"
#include "../iommu/iommu_mmio_register.h"
#include "../pcie/pcie.h"
#include "../../interrupt.h"
#include "../../memory.h"
#include "../../runtime_log.h"
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
    PciFunction *pci_function;
    int irq;
    int active;
} ether_state_t;

#define ETHER_MMIO_BASE 0x00750000u

static void ether_pump_rx(VM *vm, ether_state_t *e) {
    uint64_t dma_addr = 0u;
    if (!e || !e->active) return;

    if (e->backend.poll) e->backend.poll(e->backend.state);
    if (e->rx_len != 0 || e->rx_lo == 0) return;

    uint8_t frame[ETHER_MTU];
    int n = 0;
    if (e->backend.recv) n = e->backend.recv(e->backend.state, frame, ETHER_MTU);
    if (n <= 0) return;
    if (ether_trace_enabled()) {
        fprintf(stderr, "[ether] pump frame len=%d rx_lo=0x%08x\n", n, e->rx_lo);
    }

    uint32_t len = (uint32_t)n;
    if (len > ETHER_MTU) len = ETHER_MTU;
    if (!vm_iommu_translate_dma_ex(vm, IOMMU_DEV_ETHER, e->rx_lo, len, IOMMU_DMA_WRITE, &dma_addr)) {
        fprintf(stderr, "[ether] IOMMU reject rx iova=0x%08x len=%u\n", e->rx_lo, len);
        return;
    }
    if (dma_addr >= (uint64_t)vm->memory_size || len > ((uint64_t)vm->memory_size - dma_addr)) {
        fprintf(stderr, "[ether] RX DMA violation pa=0x%llx len=%u\n",
                (unsigned long long)dma_addr, len);
        return;
    }
    memcpy(&vm->memory[(size_t)dma_addr], frame, len);
    vm_ram_mark_written(vm, (uint32_t)dma_addr, len);
    e->rx_len = len;
    e->status |= ETHER_STATUS_RX_READY;
    if (e->pci_function) {
        pci_notify_irq(vm, e->pci_function);
    }
}

static uint32_t ether_reg_read32(VM *vm, ether_state_t *e, uint32_t off) {
    if (!e) return 0u;
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

static void ether_reg_write32(VM *vm, ether_state_t *e, uint32_t off, uint32_t val) {
    if (!e) return;
    switch (off) {
    case ETHER_OFF_TX_LEN:
        /* TX: copy frame from guest memory, send via backend */
        if (val > 0 && val <= ETHER_MTU && e->backend.send) {
            uint64_t dma_addr = 0u;
            uint8_t frame[ETHER_MTU];
            if (!vm_iommu_translate_dma_ex(vm, IOMMU_DEV_ETHER, e->tx_lo, val, IOMMU_DMA_READ, &dma_addr)) {
                fprintf(stderr, "[ether] IOMMU reject tx iova=0x%08x len=%u\n", e->tx_lo, val);
                break;
            }
            if (dma_addr >= (uint64_t)vm->memory_size || val > ((uint64_t)vm->memory_size - dma_addr)) {
                fprintf(stderr, "[ether] TX DMA violation pa=0x%llx len=%u\n",
                        (unsigned long long)dma_addr, val);
                break;
            }
            memcpy(frame, &vm->memory[(size_t)dma_addr], val);
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
        if (e->pci_function) {
            e->pci_function->status &= (uint16_t)~PCI_STATUS_INTX;
        }
        break;
    default:
        break;
    }
}

/* ---- Legacy fixed-MMIO compatibility window ---- */
static uint32_t ether_mmio_read32(VM *vm, uint32_t addr) {
    return ether_reg_read32(vm, (ether_state_t *)vm->ether, addr - ETHER_MMIO_BASE);
}

static void ether_mmio_write32(VM *vm, uint32_t addr, uint32_t val) {
    ether_reg_write32(vm, (ether_state_t *)vm->ether, addr - ETHER_MMIO_BASE, val);
}

/* ---- PCI BAR0 window ---- */
static uint32_t ether_pci_bar_read32(VM *vm, PciFunction *f,
                                    uint32_t bar_index, uint32_t offset) {
    (void)bar_index;
    return ether_reg_read32(vm, (ether_state_t *)f->cookie, offset);
}

static void ether_pci_bar_write32(VM *vm, PciFunction *f,
                                  uint32_t bar_index, uint32_t offset, uint32_t value) {
    (void)bar_index;
    ether_reg_write32(vm, (ether_state_t *)f->cookie, offset, value);
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
        if (vm->mmio_page_map_ready != 0u) {
            vm_mmio_mark_range(vm, dev.start, dev.end);
        }
        VM_RUNTIME_LOG("Registered VM Ether to MMIO ID %d (MAC=%02x:%02x:%02x:%02x:%02x:%02x)\n",
                       vm->mmio_count, e->mac[0], e->mac[1], e->mac[2],
                       e->mac[3], e->mac[4], e->mac[5]);
    }

    PciFunction *pci_fn = pci_register_function(vm, 1u, 0u,
                                                 LAMP_PCI_VENDOR_ID, ETHER_PCI_DEVICE_ID,
                                                 PCI_CLASS_NETWORK, PCI_SUBCLASS_ETHERNET, 0u);
    if (pci_fn) {
        pci_configure_bar(vm, pci_fn, 0u, ETHER_PCI_BAR_SIZE, 0u, 0u,
                          ether_pci_bar_read32, ether_pci_bar_write32,
                          NULL, e);
        (void)pci_add_pm_capability(pci_fn);
        (void)pci_add_msi_capability(pci_fn);
        (void)pci_add_express_capability(pci_fn, 0u);
        pci_set_irq_pin(pci_fn, 1u, INT_ETHER);
        e->pci_function = pci_fn;
        VM_RUNTIME_LOG("Registered PCI Ethernet at 00:01.0 device=0x%04x\n",
                       ETHER_PCI_DEVICE_ID);
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
