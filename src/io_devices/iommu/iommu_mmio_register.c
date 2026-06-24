#include "iommu_mmio_register.h"

#include <stdio.h>

static inline uint32_t lo32_u64(uint64_t v) {
    return (uint32_t)(v & 0xFFFFFFFFu);
}

static inline uint32_t hi32_u64(uint64_t v) {
    return (uint32_t)((v >> 32) & 0xFFFFFFFFu);
}

static inline uint64_t set_lo32_u64(uint64_t oldv, uint32_t lo) {
    return (oldv & 0xFFFFFFFF00000000ull) | (uint64_t)lo;
}

static inline uint64_t set_hi32_u64(uint64_t oldv, uint32_t hi) {
    return (oldv & 0x00000000FFFFFFFFull) | ((uint64_t)hi << 32);
}

static inline uint32_t rd_le32_raw(const uint8_t *p) {
    return ((uint32_t)p[0]) |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static inline int iommu_phys_range_ok(const VM *vm, uint64_t pa, uint64_t len) {
    if (!vm || len == 0u) {
        return 0;
    }
    if (pa >= (uint64_t)vm->memory_size) {
        return 0;
    }
    return (len <= ((uint64_t)vm->memory_size - pa)) ? 1 : 0;
}

static inline void iommu_set_fault_locked(VM *vm, uint32_t dev_id, uint64_t iova, uint32_t len, uint32_t reason) {
    vm->iommu.fault_status = IOMMU_FAULT_VALID | (reason << IOMMU_FAULT_REASON_SHIFT);
    vm->iommu.fault_dev = dev_id;
    vm->iommu.fault_iova = iova;
    vm->iommu.fault_len = len;
}

static inline uint32_t iommu_selected_dev(const VM *vm) {
    uint32_t sel = vm->iommu.selected_dev;
    if (sel >= IOMMU_MAX_DEVICES) {
        return IOMMU_MAX_DEVICES;
    }
    return sel;
}

static int iommu_page_translate_locked(VM *vm,
                                       uint32_t dev_id,
                                       uint64_t iova,
                                       uint64_t len,
                                       uint64_t *pa_out,
                                       uint32_t *reason_out) {
    uint32_t root;
    uint64_t first_pa = 0u;
    uint64_t done = 0u;

    if (!vm || !pa_out || !reason_out) {
        return 0;
    }
    if ((vm->iommu.devices[dev_id].root & 0xFFFu) != 0u) {
        *reason_out = IOMMU_FAULT_REASON_BAD_ROOT;
        return 0;
    }
    root = (uint32_t)vm->iommu.devices[dev_id].root;
    if (!iommu_phys_range_ok(vm, root, 4096u)) {
        *reason_out = IOMMU_FAULT_REASON_BAD_ROOT;
        return 0;
    }

    while (done < len) {
        uint64_t cur_iova64 = iova + done;
        uint32_t cur_iova;
        uint32_t page_off;
        uint32_t chunk;
        uint32_t pde_pa;
        uint32_t pde;
        uint32_t pte_table;
        uint32_t pte_pa;
        uint32_t pte;
        uint64_t cur_pa;

        if (cur_iova64 > 0xFFFFFFFFull) {
            *reason_out = IOMMU_FAULT_REASON_BOUNDS;
            return 0;
        }
        cur_iova = (uint32_t)cur_iova64;
        page_off = cur_iova & 0xFFFu;
        chunk = 4096u - page_off;
        if ((uint64_t)chunk > (len - done)) {
            chunk = (uint32_t)(len - done);
        }

        pde_pa = root + (((cur_iova >> 22) & 0x3FFu) * 4u);
        if (!iommu_phys_range_ok(vm, pde_pa, 4u)) {
            *reason_out = IOMMU_FAULT_REASON_PTABLE_OOB;
            return 0;
        }
        pde = rd_le32_raw(&vm->memory[pde_pa]);
        if ((pde & IOMMU_PTE_P) == 0u) {
            *reason_out = IOMMU_FAULT_REASON_UNMAPPED;
            return 0;
        }

        pte_table = pde & 0xFFFFF000u;
        if (!iommu_phys_range_ok(vm, pte_table, 4096u)) {
            *reason_out = IOMMU_FAULT_REASON_PTABLE_OOB;
            return 0;
        }
        pte_pa = pte_table + (((cur_iova >> 12) & 0x3FFu) * 4u);
        if (!iommu_phys_range_ok(vm, pte_pa, 4u)) {
            *reason_out = IOMMU_FAULT_REASON_PTABLE_OOB;
            return 0;
        }
        pte = rd_le32_raw(&vm->memory[pte_pa]);
        if ((pte & IOMMU_PTE_P) == 0u) {
            *reason_out = IOMMU_FAULT_REASON_UNMAPPED;
            return 0;
        }

        cur_pa = (uint64_t)(pte & 0xFFFFF000u) + page_off;
        if (!iommu_phys_range_ok(vm, cur_pa, chunk)) {
            *reason_out = IOMMU_FAULT_REASON_PA_RANGE;
            return 0;
        }
        if (done == 0u) {
            first_pa = cur_pa;
        } else if (cur_pa != first_pa + done) {
            *reason_out = IOMMU_FAULT_REASON_NONCONTIG;
            return 0;
        }
        done += chunk;
    }

    *pa_out = first_pa;
    return 1;
}

static uint32_t iommu_read32(VM *vm, uint32_t addr) {
    const uint32_t offset = addr - IOMMU_BASE;
    const uint32_t dev = iommu_selected_dev(vm);

    switch (offset) {
        case IOMMU_REG_CAP:
            return (1u << 16) | (IOMMU_MAX_DEVICES & 0xFFu);
        case IOMMU_REG_CTRL:
            return vm->iommu.ctrl;
        case IOMMU_REG_DEVSEL:
            return vm->iommu.selected_dev;
        case IOMMU_REG_DEV_CTRL:
            return (dev < IOMMU_MAX_DEVICES) ? vm->iommu.devices[dev].ctrl : 0u;
        case IOMMU_REG_IOVA_BASE_LO:
            return (dev < IOMMU_MAX_DEVICES) ? lo32_u64(vm->iommu.devices[dev].iova_base) : 0u;
        case IOMMU_REG_IOVA_BASE_HI:
            return (dev < IOMMU_MAX_DEVICES) ? hi32_u64(vm->iommu.devices[dev].iova_base) : 0u;
        case IOMMU_REG_IOVA_SIZE:
            return (dev < IOMMU_MAX_DEVICES) ? vm->iommu.devices[dev].iova_size : 0u;
        case IOMMU_REG_PA_BASE_LO:
            return (dev < IOMMU_MAX_DEVICES) ? lo32_u64(vm->iommu.devices[dev].pa_base) : 0u;
        case IOMMU_REG_PA_BASE_HI:
            return (dev < IOMMU_MAX_DEVICES) ? hi32_u64(vm->iommu.devices[dev].pa_base) : 0u;
        case IOMMU_REG_FAULT_STATUS:
            return vm->iommu.fault_status;
        case IOMMU_REG_FAULT_DEV:
            return vm->iommu.fault_dev;
        case IOMMU_REG_FAULT_IOVA_LO:
            return lo32_u64(vm->iommu.fault_iova);
        case IOMMU_REG_FAULT_IOVA_HI:
            return hi32_u64(vm->iommu.fault_iova);
        case IOMMU_REG_FAULT_LEN:
            return vm->iommu.fault_len;
        case IOMMU_REG_ROOT_LO:
            return (dev < IOMMU_MAX_DEVICES) ? lo32_u64(vm->iommu.devices[dev].root) : 0u;
        case IOMMU_REG_ROOT_HI:
            return (dev < IOMMU_MAX_DEVICES) ? hi32_u64(vm->iommu.devices[dev].root) : 0u;
        default:
            fprintf(stderr, "Unknown IOMMU MMIO register offset: 0x%08x\n", offset);
            return 0u;
    }
}

static void iommu_write32(VM *vm, uint32_t addr, uint32_t value) {
    const uint32_t offset = addr - IOMMU_BASE;
    const uint32_t dev = iommu_selected_dev(vm);

    switch (offset) {
        case IOMMU_REG_CTRL:
            vm->iommu.ctrl = value & IOMMU_CTRL_ENABLE;
            return;
        case IOMMU_REG_DEVSEL:
            vm->iommu.selected_dev = value;
            return;
        case IOMMU_REG_DEV_CTRL:
            if (dev < IOMMU_MAX_DEVICES) {
                vm->iommu.devices[dev].ctrl = value & (IOMMU_DEV_CTRL_ENABLE | IOMMU_DEV_CTRL_PAGED);
            }
            return;
        case IOMMU_REG_IOVA_BASE_LO:
            if (dev < IOMMU_MAX_DEVICES) {
                vm->iommu.devices[dev].iova_base = set_lo32_u64(vm->iommu.devices[dev].iova_base, value);
            }
            return;
        case IOMMU_REG_IOVA_BASE_HI:
            if (dev < IOMMU_MAX_DEVICES) {
                vm->iommu.devices[dev].iova_base = set_hi32_u64(vm->iommu.devices[dev].iova_base, value);
            }
            return;
        case IOMMU_REG_IOVA_SIZE:
            if (dev < IOMMU_MAX_DEVICES) {
                vm->iommu.devices[dev].iova_size = value;
            }
            return;
        case IOMMU_REG_PA_BASE_LO:
            if (dev < IOMMU_MAX_DEVICES) {
                vm->iommu.devices[dev].pa_base = set_lo32_u64(vm->iommu.devices[dev].pa_base, value);
            }
            return;
        case IOMMU_REG_PA_BASE_HI:
            if (dev < IOMMU_MAX_DEVICES) {
                vm->iommu.devices[dev].pa_base = set_hi32_u64(vm->iommu.devices[dev].pa_base, value);
            }
            return;
        case IOMMU_REG_FAULT_STATUS:
            if ((value & IOMMU_FAULT_VALID) != 0u) {
                vm->iommu.fault_status = 0u;
                vm->iommu.fault_dev = 0u;
                vm->iommu.fault_iova = 0u;
                vm->iommu.fault_len = 0u;
            }
            return;
        case IOMMU_REG_ROOT_LO:
            if (dev < IOMMU_MAX_DEVICES) {
                vm->iommu.devices[dev].root = set_lo32_u64(vm->iommu.devices[dev].root, value);
            }
            return;
        case IOMMU_REG_ROOT_HI:
            if (dev < IOMMU_MAX_DEVICES) {
                vm->iommu.devices[dev].root = set_hi32_u64(vm->iommu.devices[dev].root, value);
            }
            return;
        default:
            fprintf(stderr, "Unknown IOMMU MMIO register offset: 0x%08x\n", offset);
            atomic_set_vm_halt(vm, 1);;
            return;
    }
}

int vm_iommu_translate_dma(VM *vm, uint32_t dev_id, uint64_t iova, uint64_t len, uint64_t *pa_out) {
    uint64_t pa = iova;
    uint32_t reason = 0u;
    int ok = 1;

    if (!vm || !pa_out || len == 0u) {
        return 0;
    }

    vm_shared_lock(vm);
    if ((vm->iommu.ctrl & IOMMU_CTRL_ENABLE) != 0u) {
        if (dev_id >= IOMMU_MAX_DEVICES) {
            ok = 0;
            reason = IOMMU_FAULT_REASON_DEV_INVALID;
        } else if ((vm->iommu.devices[dev_id].ctrl & IOMMU_DEV_CTRL_ENABLE) != 0u) {
            if ((vm->iommu.devices[dev_id].ctrl & IOMMU_DEV_CTRL_PAGED) != 0u) {
                ok = iommu_page_translate_locked(vm, dev_id, iova, len, &pa, &reason);
            } else {
                uint64_t base = vm->iommu.devices[dev_id].iova_base;
                uint64_t size = (uint64_t)vm->iommu.devices[dev_id].iova_size;
                uint64_t off = 0u;

                if (size == 0u || iova < base) {
                    ok = 0;
                    reason = IOMMU_FAULT_REASON_UNMAPPED;
                } else {
                    off = iova - base;
                    if (off > size || len > size || off > (size - len)) {
                        ok = 0;
                        reason = IOMMU_FAULT_REASON_BOUNDS;
                    } else {
                        pa = vm->iommu.devices[dev_id].pa_base + off;
                        if (pa >= (uint64_t)vm->memory_size || len > ((uint64_t)vm->memory_size - pa)) {
                            ok = 0;
                            reason = IOMMU_FAULT_REASON_PA_RANGE;
                        }
                    }
                }
            }
            if (ok && ((iova + len - 1u) > 0xFFFFFFFFull || (pa + len - 1u) > 0xFFFFFFFFull)) {
                ok = 0;
                if (reason == 0u) {
                    reason = IOMMU_FAULT_REASON_BOUNDS;
                }
            }
        }

        if (!ok) {
            uint32_t fault_len = (len > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)len;
            iommu_set_fault_locked(vm, dev_id, iova, fault_len, reason);
        }
    }
    vm_shared_unlock(vm);

    if (!ok) {
        return 0;
    }
    *pa_out = pa;
    return 1;
}

void register_iommu_mmio(VM *vm) {
    static MMIO_Device iommu_dev;
    iommu_dev.start = IOMMU_BASE;
    iommu_dev.end = IOMMU_BASE + IOMMU_MMIO_SIZE - 1u;
    iommu_dev.read32 = iommu_read32;
    iommu_dev.write32 = iommu_write32;

    if (vm->mmio_count < MAX_MMIO_DEVICES) {
        vm->mmio_devices[vm->mmio_count++] = &iommu_dev;
        printf("Registered VM IOMMU to MMIO ID %d\n", vm->mmio_count);
    }
}
