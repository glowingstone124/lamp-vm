#include "../codegen.h"

#include "../../../memory.h"

static uint32_t vm_jit_arm64_core_id(const VM *vm, const VCPU *cpu) {
    if (!vm || !cpu || cpu->core_id < 0 ||
        cpu->core_id >= (int)MMU_MAX_CORES ||
        (vm->smp_cores > 0 && cpu->core_id >= vm->smp_cores)) {
        return 0u;
    }
    return (uint32_t)cpu->core_id;
}

static int vm_jit_arm64_translate_fast(VM *vm,
                                       VCPU *cpu,
                                       vm_addr_t addr,
                                       uint32_t len,
                                       uint32_t access,
                                       uint32_t *pa_out) {
    const uint32_t core_id = vm_jit_arm64_core_id(vm, cpu);
    VCPU *canonical_cpu;
    uint32_t vpn;
    VM_TlbEntry *entry;
    uint64_t epoch;
    uint32_t required;
    uint32_t root;

    if (!vm || !cpu || !pa_out || len == 0u) {
        return 0;
    }
    if ((vm->mmu.ctrl[core_id] & MMU_CTRL_ENABLE) == 0u) {
        *pa_out = addr;
        return 1;
    }
    if (((addr & 0xFFFu) + len) > 0x1000u || !vm->cpus) {
        return 0;
    }
    canonical_cpu = &vm->cpus[core_id];
    vpn = addr >> 12u;
    entry = &canonical_cpu->tlb[vpn & (VM_MMU_TLB_ENTRIES - 1u)];
    epoch = atomic_load_explicit(&canonical_cpu->mmu_epoch,
                                 memory_order_acquire);
    root = (uint32_t)(vm->mmu.root[core_id] & 0xFFFFF000ull);
    required = MMU_PTE_P;
    if ((access & VM_MMU_ACC_WRITE) != 0u) {
        required |= MMU_PTE_W;
    }
    if (entry->valid == 0u || entry->epoch != epoch || entry->vpn != vpn ||
        entry->root != root || (entry->perms & required) != required) {
        return 0;
    }
    *pa_out = entry->ppn | (addr & 0xFFFu);
    return 1;
}

static int vm_jit_arm64_page_is_plain_ram(VM *vm,
                                          uint32_t pa,
                                          uint32_t len) {
    size_t fb_index;
    uint32_t first_page;
    uint32_t last_page;
    if (!in_ram(vm, pa, len) || fb_byte_index(vm, pa, &fb_index) ||
        vm->mmio_page_map_ready == 0u) {
        return 0;
    }
    first_page = pa >> VM_MMIO_PAGE_SHIFT;
    last_page = (pa + len - 1u) >> VM_MMIO_PAGE_SHIFT;
    for (uint32_t page = first_page; page <= last_page; page++) {
        const uint8_t mask = (uint8_t)(1u << (page & 7u));
        const unsigned char page_bits =
            atomic_load_explicit(&vm->mmio_page_map[page >> 3u],
                                 memory_order_relaxed);
        if ((page_bits & mask) != 0u) {
            return 0;
        }
    }
    return 1;
}

static uint8_t vm_jit_arm64_read8(VM *vm,
                                  VCPU *cpu,
                                  vm_addr_t addr) {
    uint32_t pa;
    if (vm_jit_arm64_translate_fast(vm, cpu, addr, 1u,
                                    VM_MMU_ACC_READ, &pa) &&
        vm_jit_arm64_page_is_plain_ram(vm, pa, 1u)) {
        return vm->memory[pa];
    }
    return vm_read8_cpu(vm, cpu, addr);
}

static uint32_t vm_jit_arm64_read32(VM *vm,
                                    VCPU *cpu,
                                    vm_addr_t addr) {
    uint32_t pa;
    if (vm_jit_arm64_translate_fast(vm, cpu, addr, 4u,
                                    VM_MMU_ACC_READ, &pa) &&
        vm_jit_arm64_page_is_plain_ram(vm, pa, 4u)) {
        return load_le32(&vm->memory[pa]);
    }
    return vm_read32_cpu(vm, cpu, addr);
}

static void vm_jit_arm64_write8(VM *vm,
                                VCPU *cpu,
                                vm_addr_t addr,
                                uint8_t value) {
    uint32_t pa;
    if (vm_jit_arm64_translate_fast(vm, cpu, addr, 1u,
                                    VM_MMU_ACC_WRITE, &pa) &&
        vm_jit_arm64_page_is_plain_ram(vm, pa, 1u)) {
        vm->memory[pa] = value;
        return;
    }
    vm_write8_cpu(vm, cpu, addr, value);
}

static void vm_jit_arm64_write32(VM *vm,
                                 VCPU *cpu,
                                 vm_addr_t addr,
                                 uint32_t value) {
    uint32_t pa;
    if (vm_jit_arm64_translate_fast(vm, cpu, addr, 4u,
                                    VM_MMU_ACC_WRITE, &pa) &&
        vm_jit_arm64_page_is_plain_ram(vm, pa, 4u)) {
        store_le32(&vm->memory[pa], value);
        return;
    }
    vm_write32_cpu(vm, cpu, addr, value);
}

const VmJitMemoryOps *vm_jit_arm64_memory_ops(void) {
    static const VmJitMemoryOps ops = {
        .read8 = vm_jit_arm64_read8,
        .read32 = vm_jit_arm64_read32,
        .write8 = vm_jit_arm64_write8,
        .write32 = vm_jit_arm64_write32,
    };
    return &ops;
}
