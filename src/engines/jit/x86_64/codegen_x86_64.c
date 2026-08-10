#include "../codegen.h"

/*
 * The abstract target is intentionally wired now so x86-64 lowering can be
 * added without changing the engine/cache contract.  Until then this backend
 * reports unavailable and --engine jit safely delegates to cached.
 */
static int vm_jit_x86_64_available(void) {
    return 0;
}

static int vm_jit_x86_64_supports_opcode(uint8_t op) {
    (void)op;
    return 0;
}

static int vm_jit_x86_64_terminates_block(uint8_t op) {
    (void)op;
    return 1;
}

static int vm_jit_x86_64_compile(const VmJitBlock *block,
                                 const VmJitMemoryOps *memory,
                                 VmJitCode *out) {
    (void)block;
    (void)memory;
    (void)out;
    return 0;
}

const VmJitBackend *vm_jit_x86_64_backend(void) {
    static const VmJitBackend backend = {
        .name = "x86_64-scratch",
        .available = vm_jit_x86_64_available,
        .supports_opcode = vm_jit_x86_64_supports_opcode,
        .terminates_block = vm_jit_x86_64_terminates_block,
        .compile = vm_jit_x86_64_compile,
        .memory_ops = vm_jit_x86_64_memory_ops,
    };
    return &backend;
}
