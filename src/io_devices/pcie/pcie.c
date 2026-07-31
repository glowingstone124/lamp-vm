//
// PCIe root complex implementation: ECAM decode, Type 0 configuration
// header emulation, BAR size-probe/relocation, capability list (PM/MSI/
// Express), and MSI-to-INTC interrupt delivery.
//
#include "pcie.h"
#include "../../runtime_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../interrupt.h"
#include "../../panic.h"

#define PCIE_MAX_BAR_WINDOWS 16u

/* One MMIO_Device shadow per registered BAR; start/end are updated in place
 * whenever the guest (re)programs the BAR base or toggles Command.MEM_ENABLE. */
typedef struct {
    MMIO_Device mmio;
    uint8_t dev;
    uint8_t func;
    uint8_t bar_index;
    uint8_t in_use;
} PciBarWindow;

typedef struct {
    PciFunction functions[PCI_ECAM_DEV_COUNT][PCI_ECAM_FUNC_COUNT];
    PciBarWindow bar_windows[PCIE_MAX_BAR_WINDOWS];
    uint32_t bar_window_count;
} PcieState;

static PciFunction *pci_get_function(PcieState *st, uint32_t dev, uint32_t func) {
    if (dev >= PCI_ECAM_DEV_COUNT || func >= PCI_ECAM_FUNC_COUNT) {
        return NULL;
    }
    return &st->functions[dev][func];
}

/* Recomputes one BAR's decoded address window from its current base/size and
 * the function's Command.MEM_ENABLE bit (real hardware only decodes BAR
 * address ranges while memory space is enabled). */
static void pci_update_bar_window(VM *vm, PciFunction *f, uint32_t bar_index) {
    PcieState *st = (PcieState *)vm->pcie;
    if (!st) {
        return;
    }
    for (uint32_t i = 0; i < st->bar_window_count; i++) {
        PciBarWindow *w = &st->bar_windows[i];
        if (!w->in_use || w->dev != f->dev || w->func != f->func || w->bar_index != bar_index) {
            continue;
        }
        PciBarInfo *bar = &f->bars[bar_index];
        int active = (bar->size != 0u) && (bar->base != 0u) &&
                     ((f->command & PCI_COMMAND_MEM_ENABLE) != 0u);
        if (active) {
            w->mmio.start = bar->base;
            w->mmio.end = bar->base + bar->size - 1u;
        } else {
            w->mmio.start = 0xFFFFFFFFu;
            w->mmio.end = 0x00000000u;
        }
        return;
    }
}

static void pci_update_all_bar_windows(VM *vm, PciFunction *f) {
    for (uint32_t i = 0; i < PCI_MAX_BARS; i++) {
        if (f->bars[i].size != 0u) {
            pci_update_bar_window(vm, f, i);
        }
    }
}

static uint32_t pci_bar_window_read32(VM *vm, uint32_t addr) {
    PcieState *st = (PcieState *)vm->pcie;
    if (!st) {
        return 0xFFFFFFFFu;
    }
    for (uint32_t i = 0; i < st->bar_window_count; i++) {
        PciBarWindow *w = &st->bar_windows[i];
        if (w->in_use && addr >= w->mmio.start && addr <= w->mmio.end) {
            PciFunction *f = &st->functions[w->dev][w->func];
            uint32_t offset = addr - w->mmio.start;
            if (f->bar_read32) {
                return f->bar_read32(vm, f, w->bar_index, offset);
            }
            return 0u;
        }
    }
    fprintf(stderr, "[pcie] read32 miss inside BAR window at 0x%08x\n", addr);
    return 0xFFFFFFFFu;
}

static void pci_bar_window_write32(VM *vm, uint32_t addr, uint32_t val) {
    PcieState *st = (PcieState *)vm->pcie;
    if (!st) {
        return;
    }
    for (uint32_t i = 0; i < st->bar_window_count; i++) {
        PciBarWindow *w = &st->bar_windows[i];
        if (w->in_use && addr >= w->mmio.start && addr <= w->mmio.end) {
            PciFunction *f = &st->functions[w->dev][w->func];
            uint32_t offset = addr - w->mmio.start;
            if (f->bar_write32) {
                f->bar_write32(vm, f, w->bar_index, offset, val);
            }
            return;
        }
    }
    fprintf(stderr, "[pcie] write32 miss inside BAR window at 0x%08x\n", addr);
}

static uint32_t pci_bar_read_reg(const PciFunction *f, uint32_t bar_index) {
    const PciBarInfo *bar = &f->bars[bar_index];
    if (bar->size == 0u) {
        return 0u; /* unimplemented BAR reads as all-zero */
    }
    uint32_t type_bits = (bar->is64 ? PCI_BAR_MEM_TYPE64 : 0u) | (bar->prefetchable ? PCI_BAR_PREFETCHABLE : 0u);
    uint32_t mask = ~(bar->size - 1u);
    return (bar->base & mask) | type_bits;
}

/*
 * Standard BAR sizing trick: guest writes all-1s, the low `size`-width bits
 * are hardwired to 0 (they are not real storage), so reading back yields the
 * size mask. No separate "sizing mode" bookkeeping is required -- the same
 * mask-on-write logic serves both sizing and real base assignment.
 */
static void pci_bar_write_reg(VM *vm, PciFunction *f, uint32_t bar_index, uint32_t value) {
    PciBarInfo *bar = &f->bars[bar_index];
    if (bar->size == 0u) {
        return; /* unimplemented BAR, writes ignored */
    }
    uint32_t mask = ~(bar->size - 1u);
    uint32_t new_base = value & mask;
    if (new_base != bar->base) {
        bar->base = new_base;
        pci_update_bar_window(vm, f, bar_index);
        if (f->bar_relocated) {
            f->bar_relocated(vm, f, bar_index, new_base);
        }
    }
}

static uint32_t pci_cap_read_dword(const PciFunction *f, uint32_t abs_offset) {
    if (abs_offset < PCI_CFG_CAP_START) {
        return 0u;
    }
    uint32_t rel = abs_offset - PCI_CFG_CAP_START;
    if (rel + 4u > sizeof(f->cap_space)) {
        return 0u;
    }
    uint32_t v;
    memcpy(&v, &f->cap_space[rel], sizeof(v));
    return v;
}

static void pci_cap_write_dword(PciFunction *f, uint32_t abs_offset, uint32_t value) {
    if (abs_offset < PCI_CFG_CAP_START) {
        return;
    }
    uint32_t rel = abs_offset - PCI_CFG_CAP_START;
    if (rel + 4u > sizeof(f->cap_space)) {
        return;
    }

    if (f->msi_cap_offset != 0u && abs_offset >= f->msi_cap_offset &&
        abs_offset < (uint32_t)f->msi_cap_offset + PCI_MSI_CAP_SIZE) {
        uint32_t local = abs_offset - f->msi_cap_offset;
        uint32_t cur;
        memcpy(&cur, &f->cap_space[rel], sizeof(cur));
        uint32_t updated = cur;
        if (local == 0x0u) {
            /* Only Message Control bit0 (MSI Enable) is writable; id/next/rest of msg-ctrl stay fixed. */
            uint32_t msg_ctrl = (cur >> 16) & 0xFFFFu;
            uint32_t new_enable = (value >> 16) & 0x0001u;
            msg_ctrl = (msg_ctrl & ~0x0001u) | new_enable;
            updated = (cur & 0x0000FFFFu) | (msg_ctrl << 16);
        } else if (local == 0x4u) {
            updated = value; /* Message Address Low */
        } else if (local == 0x8u) {
            updated = value; /* Message Address High */
        } else if (local == 0xCu) {
            updated = value & 0x0000FFFFu; /* Message Data (low16 only, high16 reserved=0) */
        }
        memcpy(&f->cap_space[rel], &updated, sizeof(updated));
        return;
    }

    if (f->express_cap_offset != 0u && abs_offset >= f->express_cap_offset &&
        abs_offset < (uint32_t)f->express_cap_offset + PCI_EXPRESS_CAP_SIZE) {
        uint32_t local = abs_offset - f->express_cap_offset;
        if (local == 0x8u || local == 0x10u) {
            /* Device Control / Link Control (low16, RW) with Status (high16, RO) preserved. */
            uint32_t cur;
            memcpy(&cur, &f->cap_space[rel], sizeof(cur));
            uint32_t updated = (cur & 0xFFFF0000u) | (value & 0x0000FFFFu);
            memcpy(&f->cap_space[rel], &updated, sizeof(updated));
        }
        /* Capability header / Device Cap / Link Cap / Status dwords are read-only. */
        return;
    }

    /* Generic/unknown capability payload (e.g. a future vendor-specific cap): plain RW storage. */
    memcpy(&f->cap_space[rel], &value, sizeof(value));
}

static uint8_t pci_compute_header_type(PcieState *st, uint32_t dev, uint32_t func) {
    if (func != 0u) {
        return 0x00u;
    }
    for (uint32_t i = 1u; i < PCI_ECAM_FUNC_COUNT; i++) {
        if (st->functions[dev][i].present) {
            return PCI_HEADER_TYPE_MULTIFUNC;
        }
    }
    return 0x00u;
}

static uint32_t pcie_ecam_read32(VM *vm, uint32_t addr) {
    PcieState *st = (PcieState *)vm->pcie;
    if (!st) {
        return 0xFFFFFFFFu;
    }
    uint32_t rel = addr - PCIE_ECAM_BASE;
    uint32_t func_index = rel / PCI_ECAM_FUNC_SIZE;
    uint32_t offset = rel % PCI_ECAM_FUNC_SIZE;
    uint32_t dev = func_index / PCI_ECAM_FUNC_COUNT;
    uint32_t func = func_index % PCI_ECAM_FUNC_COUNT;

    PciFunction *f = pci_get_function(st, dev, func);
    if (!f || !f->present) {
        return 0xFFFFFFFFu; /* standard "no device present" response */
    }

    if (offset >= PCI_CFG_CAP_START) {
        return pci_cap_read_dword(f, offset & ~0x3u);
    }

    switch (offset & ~0x3u) {
    case 0x00u:
        return ((uint32_t)f->device_id << 16) | f->vendor_id;
    case 0x04u:
        return ((uint32_t)f->status << 16) | f->command;
    case 0x08u:
        return ((uint32_t)f->class_code << 24) | ((uint32_t)f->subclass << 16) |
               ((uint32_t)f->prog_if << 8) | f->revision_id;
    case 0x0Cu:
        return (uint32_t)pci_compute_header_type(st, dev, func) << 16;
    case 0x10u: case 0x14u: case 0x18u: case 0x1Cu: case 0x20u: case 0x24u:
        return pci_bar_read_reg(f, (offset - 0x10u) / 4u);
    case 0x2Cu:
        return ((uint32_t)f->subsys_id << 16) | f->subsys_vendor;
    case 0x30u:
        return 0u; /* no expansion ROM */
    case 0x34u:
        return f->cap_ptr;
    case 0x3Cu:
        return (uint32_t)f->irq_line | ((uint32_t)f->irq_pin << 8);
    default:
        return 0u;
    }
}

static void pcie_ecam_write32(VM *vm, uint32_t addr, uint32_t val) {
    PcieState *st = (PcieState *)vm->pcie;
    if (!st) {
        return;
    }
    uint32_t rel = addr - PCIE_ECAM_BASE;
    uint32_t func_index = rel / PCI_ECAM_FUNC_SIZE;
    uint32_t offset = rel % PCI_ECAM_FUNC_SIZE;
    uint32_t dev = func_index / PCI_ECAM_FUNC_COUNT;
    uint32_t func = func_index % PCI_ECAM_FUNC_COUNT;

    PciFunction *f = pci_get_function(st, dev, func);
    if (!f || !f->present) {
        return; /* writes to an empty slot are dropped, like a real master-abort */
    }

    if (offset >= PCI_CFG_CAP_START) {
        pci_cap_write_dword(f, offset & ~0x3u, val);
        return;
    }

    switch (offset & ~0x3u) {
    case 0x04u:
        f->command = (uint16_t)(val & 0xFFFFu);
        pci_update_all_bar_windows(vm, f);
        break;
    case 0x10u: case 0x14u: case 0x18u: case 0x1Cu: case 0x20u: case 0x24u:
        pci_bar_write_reg(vm, f, (offset - 0x10u) / 4u, val);
        break;
    case 0x3Cu:
        f->irq_line = (uint8_t)(val & 0xFFu);
        break;
    default:
        break; /* revision/class, subsystem ids, and expansion ROM are read-only in this model */
    }
}

void register_pcie_mmio(VM *vm) {
    if (!vm) {
        return;
    }
    PcieState *st = calloc(1, sizeof(PcieState));
    if (!st) {
        panic("Failed to allocate PCIe root complex state", vm);
        return;
    }
    vm->pcie = st;

    static MMIO_Device ecam_dev;
    ecam_dev.start = PCIE_ECAM_BASE;
    ecam_dev.end = PCIE_ECAM_BASE + PCIE_ECAM_SIZE - 1u;
    ecam_dev.read32 = pcie_ecam_read32;
    ecam_dev.write32 = pcie_ecam_write32;

    if (vm->mmio_count < MAX_MMIO_DEVICES) {
        vm->mmio_devices[vm->mmio_count++] = &ecam_dev;
        VM_RUNTIME_LOG("Registered VM PCIe ECAM to MMIO ID %d (base=0x%08x size=0x%x)\n",
                       vm->mmio_count, PCIE_ECAM_BASE, PCIE_ECAM_SIZE);
    }

    PciFunction *hb = pci_register_function(vm, 0u, 0u, LAMP_PCI_VENDOR_ID, 0x0001u,
                                             PCI_CLASS_BRIDGE, PCI_SUBCLASS_HOST, 0x00u);
    if (hb) {
        VM_RUNTIME_LOG("Registered PCIe host bridge at 00:00.0 (vendor=0x%04x device=0x%04x)\n",
                       hb->vendor_id, hb->device_id);
    }
}

PciFunction *pci_register_function(VM *vm, uint32_t dev, uint32_t func,
                                    uint16_t vendor_id, uint16_t device_id,
                                    uint8_t class_code, uint8_t subclass, uint8_t prog_if) {
    if (!vm || !vm->pcie) {
        return NULL;
    }
    PcieState *st = (PcieState *)vm->pcie;
    PciFunction *f = pci_get_function(st, dev, func);
    if (!f || f->present) {
        return NULL; /* out of range or slot already taken */
    }

    memset(f, 0, sizeof(*f));
    f->present = 1u;
    f->dev = (uint8_t)dev;
    f->func = (uint8_t)func;
    f->vendor_id = vendor_id;
    f->device_id = device_id;
    f->class_code = class_code;
    f->subclass = subclass;
    f->prog_if = prog_if;
    f->revision_id = 0x01u;
    f->subsys_vendor = vendor_id;
    f->subsys_id = device_id;
    /* Command starts fully disabled (IO/mem/bus-master off) until the guest's PCI
     * enumeration code enables them, matching real firmware-to-OS handoff. */
    f->command = 0u;
    f->status = 0u;
    return f;
}

void pci_configure_bar(VM *vm, PciFunction *f, uint32_t bar_index, uint32_t size,
                        uint8_t is64, uint8_t prefetchable,
                        pci_bar_read32_fn read32, pci_bar_write32_fn write32,
                        pci_bar_relocated_fn relocated, void *cookie) {
    if (!vm || !f || bar_index >= PCI_MAX_BARS || size == 0u) {
        return;
    }
    if ((size & (size - 1u)) != 0u) {
        fprintf(stderr, "[pcie] BAR size 0x%x is not a power of two\n", size);
        return;
    }

    f->bars[bar_index].size = size;
    f->bars[bar_index].base = 0u;
    f->bars[bar_index].is64 = is64 ? 1u : 0u;
    f->bars[bar_index].prefetchable = prefetchable ? 1u : 0u;
    f->bar_read32 = read32;
    f->bar_write32 = write32;
    f->bar_relocated = relocated;
    f->cookie = cookie;

    PcieState *st = (PcieState *)vm->pcie;
    if (!st || st->bar_window_count >= PCIE_MAX_BAR_WINDOWS) {
        fprintf(stderr, "[pcie] no free BAR window slots\n");
        return;
    }
    PciBarWindow *w = &st->bar_windows[st->bar_window_count++];
    memset(w, 0, sizeof(*w));
    w->dev = f->dev;
    w->func = f->func;
    w->bar_index = (uint8_t)bar_index;
    w->in_use = 1u;
    w->mmio.start = 0xFFFFFFFFu; /* inactive until the guest assigns a base and enables Command.MEM_ENABLE */
    w->mmio.end = 0x00000000u;
    w->mmio.read32 = pci_bar_window_read32;
    w->mmio.write32 = pci_bar_window_write32;

    if (vm->mmio_count < MAX_MMIO_DEVICES) {
        vm->mmio_devices[vm->mmio_count++] = &w->mmio;
    } else {
        fprintf(stderr, "[pcie] MAX_MMIO_DEVICES exhausted while registering BAR window\n");
    }
}

uint8_t pci_add_capability(PciFunction *f, uint8_t cap_id, uint8_t size) {
    if (!f || size < 4u || (size % 4u) != 0u) {
        return 0u;
    }
    if ((uint32_t)f->cap_used + (uint32_t)size > sizeof(f->cap_space)) {
        return 0u;
    }

    uint8_t rel = f->cap_used;
    uint8_t abs_offset = (uint8_t)(PCI_CFG_CAP_START + rel);
    memset(&f->cap_space[rel], 0, size);
    f->cap_space[rel] = cap_id;   /* byte0: capability id */
    f->cap_space[rel + 1] = 0u;   /* byte1: next pointer, patched below when chaining */

    if (f->cap_ptr == 0u) {
        f->cap_ptr = abs_offset;
    } else if (f->last_cap_offset != 0u) {
        uint8_t last_rel = (uint8_t)(f->last_cap_offset - PCI_CFG_CAP_START);
        f->cap_space[last_rel + 1] = abs_offset; /* patch previous cap's "next" byte */
    }
    f->last_cap_offset = abs_offset;
    f->cap_used = (uint8_t)(rel + size);
    f->status |= PCI_STATUS_CAP_LIST;
    return abs_offset;
}

uint8_t pci_add_msi_capability(PciFunction *f) {
    uint8_t offset = pci_add_capability(f, PCI_CAP_ID_MSI, (uint8_t)PCI_MSI_CAP_SIZE);
    if (offset == 0u) {
        return 0u;
    }
    uint8_t rel = (uint8_t)(offset - PCI_CFG_CAP_START);
    uint32_t dword0;
    memcpy(&dword0, &f->cap_space[rel], sizeof(dword0));
    dword0 |= (uint32_t)0x0080u << 16; /* Message Control bit7: 64-bit Address Capable (RO=1) */
    memcpy(&f->cap_space[rel], &dword0, sizeof(dword0));
    f->msi_cap_offset = offset;
    return offset;
}

uint8_t pci_add_express_capability(PciFunction *f, uint8_t device_port_type) {
    uint8_t offset = pci_add_capability(f, PCI_CAP_ID_EXPRESS, (uint8_t)PCI_EXPRESS_CAP_SIZE);
    if (offset == 0u) {
        return 0u;
    }
    uint8_t rel = (uint8_t)(offset - PCI_CFG_CAP_START);

    uint32_t dword0;
    memcpy(&dword0, &f->cap_space[rel], sizeof(dword0));
    uint32_t pcie_cap_reg = 0x0002u /* Capability Version = 2 */
                            | ((uint32_t)(device_port_type & 0xFu) << 4);
    dword0 |= pcie_cap_reg << 16;
    memcpy(&f->cap_space[rel], &dword0, sizeof(dword0));

    /* Link Capabilities (offset+0xC): Max Link Speed=1 (2.5GT/s), Max Link Width=1 (x1). */
    uint32_t link_cap = 0x1u | (0x1u << 4);
    memcpy(&f->cap_space[rel + 0xCu], &link_cap, sizeof(link_cap));

    /* Link Status (upper 16 bits of dword @ +0x10): Speed=1, Width=1, Data Link Layer Active=1.
     * There is no physical link to train, so it is reported "up" immediately. */
    uint32_t link_status = (0x1u | (0x1u << 4) | (0x1u << 13)) << 16;
    memcpy(&f->cap_space[rel + 0x10u], &link_status, sizeof(link_status));

    f->express_cap_offset = offset;
    return offset;
}

uint8_t pci_add_pm_capability(PciFunction *f) {
    uint8_t offset = pci_add_capability(f, PCI_CAP_ID_PM, (uint8_t)PCI_PM_CAP_SIZE);
    if (offset == 0u) {
        return 0u;
    }
    uint8_t rel = (uint8_t)(offset - PCI_CFG_CAP_START);
    uint32_t dword0;
    memcpy(&dword0, &f->cap_space[rel], sizeof(dword0));
    dword0 |= (uint32_t)0x0003u << 16; /* PMC bits[2:0]: PM spec version 3, no PME support */
    memcpy(&f->cap_space[rel], &dword0, sizeof(dword0));
    return offset;
}

void pci_set_irq_pin(PciFunction *f, uint8_t pin, uint32_t legacy_vector) {
    if (!f || pin < 1u || pin > 4u) {
        return;
    }
    f->irq_pin = pin;
    f->irq_line = (uint8_t)(legacy_vector & 0xFFu);
}

void pci_notify_irq(VM *vm, PciFunction *f) {
    if (!vm || !f) {
        return;
    }
    if (f->msi_cap_offset != 0u) {
        uint8_t rel = (uint8_t)(f->msi_cap_offset - PCI_CFG_CAP_START);
        uint32_t dword0;
        memcpy(&dword0, &f->cap_space[rel], sizeof(dword0));
        int msi_enabled = (int)((dword0 >> 16) & 0x1u);
        if (msi_enabled) {
            uint32_t addr_lo;
            uint32_t data_dword;
            memcpy(&addr_lo, &f->cap_space[rel + 0x4], sizeof(addr_lo));
            memcpy(&data_dword, &f->cap_space[rel + 0xC], sizeof(data_dword));
            int core_id = (int)((addr_lo >> PCI_MSI_ADDR_CORE_SHIFT) & PCI_MSI_ADDR_CORE_MASK);
            if (core_id < 0 || core_id >= vm->smp_cores) {
                core_id = 0;
            }
            uint32_t vector = data_dword & 0xFFu;
            trigger_interrupt_target(vm, core_id, vector);
            return;
        }
    }
    if (f->irq_pin != 0u && (f->command & PCI_COMMAND_INTX_DISABLE) == 0u) {
        f->status |= PCI_STATUS_INTX;
        trigger_interrupt(vm, f->irq_line);
    }
}
