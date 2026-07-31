//
// PCIe root complex: ECAM configuration space, BAR assignment/relocation,
// capability list (Power Management / MSI / PCI Express), and MSI delivery
// onto the existing 256-vector INTC.
//
#ifndef VM_PCIE_H
#define VM_PCIE_H

#include "../../vm.h"
#include "../../../include/lampvm/device_abi.h"

/* ---- Type 0 (endpoint) configuration header offsets ---- */
#define PCI_CFG_VENDOR_ID     0x00u
#define PCI_CFG_DEVICE_ID     0x02u
#define PCI_CFG_COMMAND       0x04u
#define PCI_CFG_STATUS        0x06u
#define PCI_CFG_REVISION_ID   0x08u
#define PCI_CFG_PROG_IF       0x09u
#define PCI_CFG_SUBCLASS      0x0Au
#define PCI_CFG_CLASS_CODE    0x0Bu
#define PCI_CFG_CACHE_LINE    0x0Cu
#define PCI_CFG_LATENCY_TIMER 0x0Du
#define PCI_CFG_HEADER_TYPE   0x0Eu
#define PCI_CFG_BIST          0x0Fu
#define PCI_CFG_BAR0          0x10u
#define PCI_CFG_SUBSYS_VENDOR 0x2Cu
#define PCI_CFG_SUBSYS_ID     0x2Eu
#define PCI_CFG_EXPROM_BASE   0x30u
#define PCI_CFG_CAP_PTR       0x34u
#define PCI_CFG_INT_LINE      0x3Cu
#define PCI_CFG_INT_PIN       0x3Du
#define PCI_CFG_MIN_GNT       0x3Eu
#define PCI_CFG_MAX_LAT       0x3Fu
#define PCI_CFG_CAP_START     0x40u

#define PCI_HEADER_TYPE_MULTIFUNC 0x80u

#define PCI_COMMAND_IO_ENABLE    0x0001u
#define PCI_COMMAND_MEM_ENABLE   0x0002u
#define PCI_COMMAND_BUS_MASTER   0x0004u
#define PCI_COMMAND_INTX_DISABLE 0x0400u

#define PCI_STATUS_INTX      0x0008u
#define PCI_STATUS_CAP_LIST  0x0010u

#define PCI_BAR_MEM_TYPE64      0x04u
#define PCI_BAR_PREFETCHABLE    0x08u

#define PCI_CAP_ID_PM      0x01u
#define PCI_CAP_ID_MSI     0x05u
#define PCI_CAP_ID_EXPRESS 0x10u

#define PCI_PM_CAP_SIZE      0x08u
#define PCI_MSI_CAP_SIZE     0x10u
#define PCI_EXPRESS_CAP_SIZE 0x14u

#define PCI_MAX_BARS 6u

/* Class codes used by devices this VM currently ships or plans to ship. */
#define PCI_CLASS_BRIDGE       0x06u
#define PCI_SUBCLASS_HOST      0x00u
#define PCI_CLASS_NETWORK      0x02u
#define PCI_SUBCLASS_ETHERNET  0x00u
#define PCI_CLASS_DISPLAY      0x03u
#define PCI_SUBCLASS_VGA       0x00u
#define PCI_CLASS_MULTIMEDIA   0x04u
#define PCI_SUBCLASS_AUDIO     0x01u
#define PCI_CLASS_SERIAL_BUS   0x0Cu
#define PCI_SUBCLASS_USB       0x03u
#define PCI_PROGIF_EHCI        0x20u

/*
 * Self-assigned vendor ID for this project's virtual devices. This is not a
 * PCI-SIG registered vendor ID; it only needs to be internally consistent
 * since lamp-vm never talks to real-world PCI hardware/drivers.
 */
/*
 * MSI address/data convention used by this VM (documented in docs/pci.md):
 *   Message Address bits [11:4] select the destination core id.
 *   Message Data bits [7:0] select the INTC vector to raise on that core.
 * This mirrors the spirit of the x86 MSI address/data split (destination
 * APIC id in the address, vector in the data) without depending on any
 * x86-specific semantics.
 */
#define PCI_MSI_ADDR_CORE_SHIFT 4u
#define PCI_MSI_ADDR_CORE_MASK  0xFFu

typedef struct PciFunction PciFunction;

/* Device-supplied callbacks for reads/writes that land inside an assigned BAR window. */
typedef uint32_t (*pci_bar_read32_fn)(VM *vm, PciFunction *f, uint32_t bar_index, uint32_t offset);
typedef void (*pci_bar_write32_fn)(VM *vm, PciFunction *f, uint32_t bar_index, uint32_t offset, uint32_t value);
/* Optional notification once the guest (re)programs a BAR's base address. */
typedef void (*pci_bar_relocated_fn)(VM *vm, PciFunction *f, uint32_t bar_index, uint32_t new_base);

typedef struct {
    uint32_t size;          /* 0 = BAR not implemented */
    uint32_t base;          /* current guest-assigned base, 0 = unassigned */
    uint8_t is64;
    uint8_t prefetchable;
} PciBarInfo;

struct PciFunction {
    uint8_t present;
    uint8_t dev;
    uint8_t func;

    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t revision_id;
    uint16_t subsys_vendor;
    uint16_t subsys_id;

    uint16_t command;
    uint16_t status;

    PciBarInfo bars[PCI_MAX_BARS];

    uint8_t cap_space[256 - PCI_CFG_CAP_START]; /* raw capability bytes, offsets relative to PCI_CFG_CAP_START */
    uint8_t cap_used;                            /* bytes already allocated from cap_space */
    uint8_t cap_ptr;                             /* absolute config-space offset of first capability, 0 = none */
    uint8_t last_cap_offset;                     /* absolute offset of most recently added capability, 0 = none yet */
    uint8_t msi_cap_offset;                      /* absolute offset, 0 if function has no MSI capability */
    uint8_t express_cap_offset;

    uint8_t irq_line;
    uint8_t irq_pin; /* 0 = none, 1..4 = INTA#..INTD# */

    void *cookie;
    pci_bar_read32_fn bar_read32;
    pci_bar_write32_fn bar_write32;
    pci_bar_relocated_fn bar_relocated;
};

/* Registers the ECAM MMIO window and the host bridge function (bus0/dev0/func0). */
void register_pcie_mmio(VM *vm);

/*
 * Registers a new endpoint function at (dev, func). Returns NULL if the slot
 * is already populated or out of range. The returned pointer is stable for
 * the lifetime of the VM.
 */
PciFunction *pci_register_function(VM *vm, uint32_t dev, uint32_t func,
                                    uint16_t vendor_id, uint16_t device_id,
                                    uint8_t class_code, uint8_t subclass, uint8_t prog_if);

/* Declares a BAR for a function. Must be called before the guest enumerates the bus. */
void pci_configure_bar(VM *vm, PciFunction *f, uint32_t bar_index, uint32_t size,
                        uint8_t is64, uint8_t prefetchable,
                        pci_bar_read32_fn read32, pci_bar_write32_fn write32,
                        pci_bar_relocated_fn relocated, void *cookie);

/* Adds a capability of `size` bytes to the function's capability chain; returns its absolute config offset. */
uint8_t pci_add_capability(PciFunction *f, uint8_t cap_id, uint8_t size);

/* Convenience helpers that lay out a spec-shaped capability and remember its offset for later access. */
uint8_t pci_add_msi_capability(PciFunction *f);
uint8_t pci_add_express_capability(PciFunction *f, uint8_t device_port_type);
uint8_t pci_add_pm_capability(PciFunction *f);

/* Assigns the function's legacy interrupt pin (1..4 for INTA#..INTD#), routed through INTC on trigger_interrupt(). */
void pci_set_irq_pin(PciFunction *f, uint8_t pin, uint32_t legacy_vector);

/*
 * Raises an interrupt for the function: uses MSI (per the function's
 * programmed Address/Data) if enabled, otherwise falls back to the legacy
 * INTx vector registered via pci_set_irq_pin().
 */
void pci_notify_irq(VM *vm, PciFunction *f);

#endif /* VM_PCIE_H */
