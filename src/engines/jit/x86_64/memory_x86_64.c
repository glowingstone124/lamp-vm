#include "../codegen.h"

#include "../../../memory.h"

static uint8_t vm_jit_x86_64_read8(VM *vm,
                                    VCPU *cpu,
                                    vm_addr_t addr) {
    return vm_read8_cpu(vm, cpu, addr);
}

static uint32_t vm_jit_x86_64_read32(VM *vm,
                                     VCPU *cpu,
                                     vm_addr_t addr) {
    return vm_read32_cpu(vm, cpu, addr);
}

static void vm_jit_x86_64_write8(VM *vm,
                                 VCPU *cpu,
                                 vm_addr_t addr,
                                 uint8_t value) {
    vm_write8_cpu(vm, cpu, addr, value);
}

static void vm_jit_x86_64_write32(VM *vm,
                                  VCPU *cpu,
                                  vm_addr_t addr,
                                  uint32_t value) {
    vm_write32_cpu(vm, cpu, addr, value);
}

const VmJitMemoryOps *vm_jit_x86_64_memory_ops(void) {
    static const VmJitMemoryOps ops = {
        .read8 = vm_jit_x86_64_read8,
        .read32 = vm_jit_x86_64_read32,
        .write8 = vm_jit_x86_64_write8,
        .write32 = vm_jit_x86_64_write32,
    };
    return &ops;
}
