#include "mmu_mmio_register.h"
#include "../../runtime_log.h"

#include <stdio.h>
#include <string.h>

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

static inline int mmu_phys_range_ok(const VM *vm, uint32_t pa, uint32_t len) {
    if (!vm || len == 0u) {
        return 0;
    }
    if (pa >= (uint32_t)vm->memory_size) {
        return 0;
    }
    return (len <= ((uint32_t)vm->memory_size - pa)) ? 1 : 0;
}

static inline uint32_t mmu_core_id(VM *vm) {
    VCPU *cpu;
    if (!vm) {
        return 0u;
    }
    cpu = vm_current_cpu(vm);
    if (!cpu || cpu->core_id < 0 || cpu->core_id >= (int)MMU_MAX_CORES) {
        return 0u;
    }
    if (vm->smp_cores > 0 && cpu->core_id >= vm->smp_cores) {
        return 0u;
    }
    return (uint32_t)cpu->core_id;
}

static inline void mmu_set_fault(VM *vm, uint32_t core_id, uint32_t vaddr, uint32_t access, uint32_t reason) {
    if (!vm) {
        return;
    }
    vm->mmu.fault_status[core_id] = MMU_FAULT_VALID | (reason << MMU_FAULT_REASON_SHIFT);
    vm->mmu.fault_addr[core_id] = (uint64_t)vaddr;
    vm->mmu.fault_info[core_id] = access;
}

static inline uint32_t mmu_access_required_perms(uint32_t access) {
    uint32_t required = MMU_PTE_P;
    if ((access & VM_MMU_ACC_WRITE) != 0u) {
        required |= MMU_PTE_W;
    }
    if ((access & VM_MMU_ACC_EXEC) != 0u) {
        required |= MMU_PTE_X;
    }
    if ((access & VM_MMU_ACC_USER) != 0u) {
        required |= MMU_PTE_U;
    }
    return required;
}

static int mmu_walk_translate(VM *vm,
                              uint32_t core_id,
                              uint32_t vaddr,
                              uint32_t access,
                              uint32_t *pa_out,
                              uint32_t *perms_out) {
    uint32_t root;
    uint32_t pde_pa;
    uint32_t pde;
    uint32_t pte_table;
    uint32_t pte_pa;
    uint32_t pte;
    uint32_t perms;
    uint32_t pa;

    if (!vm || !pa_out) {
        return 0;
    }

    root = (uint32_t)(vm->mmu.root[core_id] & 0xFFFFF000ull);
    if ((root & 0xFFFu) != 0u || !mmu_phys_range_ok(vm, root, 4096u)) {
        mmu_set_fault(vm, core_id, vaddr, access, MMU_FAULT_REASON_BAD_ROOT);
        return 0;
    }

    pde_pa = root + (((vaddr >> 22) & 0x3FFu) * 4u);
    if (!mmu_phys_range_ok(vm, pde_pa, 4u)) {
        mmu_set_fault(vm, core_id, vaddr, access, MMU_FAULT_REASON_PTABLE_OOB);
        return 0;
    }
    pde = rd_le32_raw(&vm->memory[pde_pa]);
    if ((pde & MMU_PTE_P) == 0u) {
        mmu_set_fault(vm, core_id, vaddr, access, MMU_FAULT_REASON_NOT_PRESENT);
        return 0;
    }

    pte_table = pde & 0xFFFFF000u;
    if (!mmu_phys_range_ok(vm, pte_table, 4096u)) {
        mmu_set_fault(vm, core_id, vaddr, access, MMU_FAULT_REASON_PTABLE_OOB);
        return 0;
    }

    pte_pa = pte_table + (((vaddr >> 12) & 0x3FFu) * 4u);
    if (!mmu_phys_range_ok(vm, pte_pa, 4u)) {
        mmu_set_fault(vm, core_id, vaddr, access, MMU_FAULT_REASON_PTABLE_OOB);
        return 0;
    }
    pte = rd_le32_raw(&vm->memory[pte_pa]);
    if ((pte & MMU_PTE_P) == 0u) {
        mmu_set_fault(vm, core_id, vaddr, access, MMU_FAULT_REASON_NOT_PRESENT);
        return 0;
    }

    perms = pde & pte;
    if ((access & VM_MMU_ACC_WRITE) != 0u && (perms & MMU_PTE_W) == 0u) {
        mmu_set_fault(vm, core_id, vaddr, access, MMU_FAULT_REASON_PERM);
        return 0;
    }
    if ((access & VM_MMU_ACC_EXEC) != 0u && (perms & MMU_PTE_X) == 0u) {
        mmu_set_fault(vm, core_id, vaddr, access, MMU_FAULT_REASON_PERM);
        return 0;
    }
    if ((access & VM_MMU_ACC_USER) != 0u && (perms & MMU_PTE_U) == 0u) {
        mmu_set_fault(vm, core_id, vaddr, access, MMU_FAULT_REASON_PERM);
        return 0;
    }

    pa = (pte & 0xFFFFF000u) | (vaddr & 0xFFFu);
    *pa_out = pa;
    if (perms_out) {
        *perms_out = perms;
    }
    return 1;
}

void vm_mmu_flush_tlb(VM *vm, uint32_t core_id) {
    if (!vm || !vm->cpus || core_id >= (uint32_t)vm->smp_cores) {
        return;
    }
    memset(vm->cpus[core_id].tlb, 0, sizeof(vm->cpus[core_id].tlb));
}

int vm_mmu_translate_access(VM *vm, uint32_t vaddr, uint32_t access, uint32_t *pa_out) {
    uint32_t core_id;
    uint32_t root;
    uint32_t vpn;
    uint32_t tlb_index;
    uint32_t required_perms;
    VM_TlbEntry *entry;
    if (!vm || !pa_out) {
        return 0;
    }
    core_id = mmu_core_id(vm);
    if ((vm->mmu.ctrl[core_id] & MMU_CTRL_ENABLE) == 0u) {
        *pa_out = vaddr;
        return 1;
    }

    root = (uint32_t)(vm->mmu.root[core_id] & 0xFFFFF000ull);
    vpn = vaddr >> 12;
    tlb_index = vpn & (VM_MMU_TLB_ENTRIES - 1u);
    required_perms = mmu_access_required_perms(access);
    entry = &vm->cpus[core_id].tlb[tlb_index];
    if (entry->valid != 0u &&
        entry->vpn == vpn &&
        entry->root == root &&
        (entry->perms & required_perms) == required_perms) {
        *pa_out = entry->ppn | (vaddr & 0xFFFu);
        return 1;
    }

    {
        uint32_t pa;
        uint32_t perms;
        if (!mmu_walk_translate(vm, core_id, vaddr, access, &pa, &perms)) {
            return 0;
        }
        entry->valid = 1u;
        entry->vpn = vpn;
        entry->ppn = pa & 0xFFFFF000u;
        entry->root = root;
        entry->perms = perms;
        *pa_out = pa;
        return 1;
    }
}

static uint32_t mmu_read32(VM *vm, uint32_t addr) {
    uint32_t core_id = mmu_core_id(vm);
    const uint32_t offset = addr - MMU_BASE;
    switch (offset) {
        case MMU_REG_CAP:
            return (2u) | (2u << 8) | (12u << 16) | ((MMU_MAX_CORES & 0xFFu) << 24);
        case MMU_REG_CTRL:
            return vm->mmu.ctrl[core_id];
        case MMU_REG_ROOT_LO:
            return lo32_u64(vm->mmu.root[core_id]);
        case MMU_REG_ROOT_HI:
            return hi32_u64(vm->mmu.root[core_id]);
        case MMU_REG_FAULT_STATUS:
            return vm->mmu.fault_status[core_id];
        case MMU_REG_FAULT_ADDR_LO:
            return lo32_u64(vm->mmu.fault_addr[core_id]);
        case MMU_REG_FAULT_ADDR_HI:
            return hi32_u64(vm->mmu.fault_addr[core_id]);
        case MMU_REG_FAULT_INFO:
            return vm->mmu.fault_info[core_id];
        default:
            fprintf(stderr, "Unknown MMU MMIO register offset: 0x%08x\n", offset);
            return 0u;
    }
}

static void mmu_write32(VM *vm, uint32_t addr, uint32_t value) {
    uint32_t core_id = mmu_core_id(vm);
    const uint32_t offset = addr - MMU_BASE;
    switch (offset) {
        case MMU_REG_CTRL:
            vm->mmu.ctrl[core_id] = value & MMU_CTRL_ENABLE;
            vm_mmu_flush_tlb(vm, core_id);
            return;
        case MMU_REG_ROOT_LO:
            vm->mmu.root[core_id] = set_lo32_u64(vm->mmu.root[core_id], value);
            vm_mmu_flush_tlb(vm, core_id);
            return;
        case MMU_REG_ROOT_HI:
            vm->mmu.root[core_id] = set_hi32_u64(vm->mmu.root[core_id], value);
            vm_mmu_flush_tlb(vm, core_id);
            return;
        case MMU_REG_FAULT_STATUS:
            if ((value & MMU_FAULT_VALID) != 0u) {
                vm->mmu.fault_status[core_id] = 0u;
                vm->mmu.fault_addr[core_id] = 0u;
                vm->mmu.fault_info[core_id] = 0u;
            }
            return;
        default:
            fprintf(stderr, "Unknown MMU MMIO register offset: 0x%08x\n", offset);
            atomic_set_vm_halt(vm, 1);;
            return;
    }
}

uint32_t vm_mmu_fault_status(VM *vm) {
    uint32_t core_id = mmu_core_id(vm);
    if (!vm) {
        return 0u;
    }
    return vm->mmu.fault_status[core_id];
}

uint32_t vm_mmu_fault_info(VM *vm) {
    uint32_t core_id = mmu_core_id(vm);
    if (!vm) {
        return 0u;
    }
    return vm->mmu.fault_info[core_id];
}

uint32_t vm_mmu_root_lo(VM *vm) {
    uint32_t core_id = mmu_core_id(vm);
    if (!vm) {
        return 0u;
    }
    return lo32_u64(vm->mmu.root[core_id]);
}

void register_mmu_mmio(VM *vm) {
    static MMIO_Device mmu_dev;
    mmu_dev.start = MMU_BASE;
    mmu_dev.end = MMU_BASE + MMU_MMIO_SIZE - 1u;
    mmu_dev.read32 = mmu_read32;
    mmu_dev.write32 = mmu_write32;

    if (vm->mmio_count < MAX_MMIO_DEVICES) {
        vm->mmio_devices[vm->mmio_count++] = &mmu_dev;
        VM_RUNTIME_LOG("Registered VM MMU to MMIO ID %d\n", vm->mmio_count);
    }
}
