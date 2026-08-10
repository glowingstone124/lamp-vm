#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "jit.h"

#include "../engine.h"
#include "code_memory.h"
#include "codegen.h"
#include "../../memory.h"
#include "../../panic.h"
#include "../../interrupt.h"

enum {
    VM_JIT_QUANTUM_MAX = 256u,
    VM_JIT_CACHE_WAYS = 4u,
    VM_JIT_CACHE_SETS = 256u,
    VM_JIT_CACHE_ENTRIES = VM_JIT_CACHE_WAYS * VM_JIT_CACHE_SETS,
    VM_JIT_CODE_SLOT_BYTES = 4096u,
};

typedef struct VmJitCacheEntry {
    VmJitBlock block;
    VmJitCode code;
    uint8_t valid;
} VmJitCacheEntry;

typedef struct VmJitCpuState {
    const VmJitBackend *backend;
    const VmJitMemoryOps *memory;
    VmJitCodeArena arena;
    uint8_t next_way[VM_JIT_CACHE_SETS];
    VmJitCacheEntry entries[VM_JIT_CACHE_ENTRIES];
} VmJitCpuState;

static int vm_jit_block_valid(const VM *vm,
                              const VCPU *cpu,
                              const VmJitCacheEntry *entry) {
    const VmJitBlock *block;
    size_t byte_count;
    if (!vm || !cpu || !entry || entry->valid == 0u || !entry->code.entry) {
        return 0;
    }
    block = &entry->block;
    if (block->count == 0u || block->start_ip != (vm_addr_t)cpu->ip ||
        block->mmu_epoch != atomic_load_explicit(&cpu->mmu_epoch,
                                                 memory_order_acquire) ||
        block->mmio_epoch !=
            (uint32_t)atomic_load_explicit(&vm->mmio_epoch,
                                           memory_order_acquire)) {
        return 0;
    }
    byte_count = (size_t)block->count * sizeof(uint64_t);
    if (block->host_pa >= vm->memory_size ||
        byte_count > vm->memory_size - block->host_pa) {
        return 0;
    }
    for (uint32_t i = 0u; i < block->count; i++) {
        if (load_le64(&vm->memory[block->host_pa + i * sizeof(uint64_t)]) !=
            block->raw[i]) {
            return 0;
        }
    }
    return 1;
}

static int vm_jit_build_block(VM *vm,
                              VCPU *cpu,
                              const VmJitBackend *backend,
                              VmJitBlock *block) {
    const vm_addr_t start_ip = (vm_addr_t)cpu->ip;
    const vm_addr_t start_page = start_ip & ~0xFFFu;
    const uint64_t mmu_epoch_before =
        atomic_load_explicit(&cpu->mmu_epoch, memory_order_acquire);
    const uint32_t mmio_epoch_before =
        (uint32_t)atomic_load_explicit(&vm->mmio_epoch,
                                       memory_order_acquire);
    uint32_t first_host_pa = UINT32_MAX;
    uint8_t count = 0u;

    if (!vm || !cpu || !backend || !block) {
        return 0;
    }
    memset(block, 0, sizeof(*block));
    for (uint32_t i = 0u; i < VM_JIT_BLOCK_MAX_OPS; i++) {
        const vm_addr_t ip = start_ip + i * (vm_addr_t)sizeof(uint64_t);
        uint32_t host_pa = UINT32_MAX;
        uint64_t inst;
        VM_DecodedOp decoded;

        if ((ip & ~0xFFFu) != start_page ||
            !vm_fetch64_exec_cpu_ex(vm, cpu, ip, &inst, &host_pa) ||
            host_pa == UINT32_MAX) {
            break;
        }
        if (i == 0u) {
            first_host_pa = host_pa;
        } else if (host_pa != first_host_pa + i * sizeof(uint64_t)) {
            break;
        }

        decoded.ip = ip;
        decoded.op = (uint8_t)((inst >> 56u) & 0xFFu);
        decoded.rd = (uint8_t)((inst >> 48u) & 0xFFu);
        decoded.rs1 = (uint8_t)((inst >> 40u) & 0xFFu);
        decoded.rs2 = (uint8_t)((inst >> 32u) & 0xFFu);
        decoded.imm = (int32_t)(inst & 0xFFFFFFFFu);
        if (!backend->supports_opcode(decoded.op)) {
            break;
        }

        block->raw[count] = inst;
        block->ops[count] = decoded;
        count++;
        if (backend->terminates_block(decoded.op)) {
            break;
        }
    }

    if (count == 0u || first_host_pa == UINT32_MAX ||
        mmu_epoch_before != atomic_load_explicit(&cpu->mmu_epoch,
                                                 memory_order_acquire) ||
        mmio_epoch_before !=
            (uint32_t)atomic_load_explicit(&vm->mmio_epoch,
                                           memory_order_acquire)) {
        return 0;
    }
    block->start_ip = start_ip;
    block->host_pa = first_host_pa;
    block->mmu_epoch = mmu_epoch_before;
    block->mmio_epoch = mmio_epoch_before;
    block->count = count;
    return 1;
}

static void vm_jit_cache_entry_clear(VmJitCacheEntry *entry) {
    if (!entry) {
        return;
    }
    entry->valid = 0u;
    vm_jit_code_destroy(&entry->code);
    memset(&entry->block, 0, sizeof(entry->block));
}

static VmJitCpuState *vm_jit_cpu_state(VCPU *cpu) {
    const VmJitBackend *backend;
    VmJitCpuState *state;
    if (!cpu) {
        return NULL;
    }
    if (cpu->jit_state) {
        return (VmJitCpuState *)cpu->jit_state;
    }
    backend = vm_jit_codegen_host_backend();
    if (!backend || !backend->available || !backend->available() ||
        !backend->memory_ops) {
        return NULL;
    }
    state = calloc(1u, sizeof(*state));
    if (!state) {
        return NULL;
    }
    state->backend = backend;
    state->memory = backend->memory_ops();
    if (!state->memory) {
        free(state);
        return NULL;
    }
    if (!vm_jit_code_arena_init(&state->arena,
                                VM_JIT_CACHE_ENTRIES,
                                VM_JIT_CODE_SLOT_BYTES)) {
        free(state);
        return NULL;
    }
    for (size_t i = 0u; i < VM_JIT_CACHE_ENTRIES; i++) {
        if (!vm_jit_code_assign_slot(&state->arena, i,
                                     &state->entries[i].code)) {
            vm_jit_code_arena_destroy(&state->arena);
            free(state);
            return NULL;
        }
    }
    cpu->jit_state = state;
    return state;
}

static VmJitCacheEntry *vm_jit_select_entry(VmJitCpuState *state,
                                             vm_addr_t ip) {
    const size_t set = ((size_t)ip >> 3u) & (VM_JIT_CACHE_SETS - 1u);
    const size_t base = set * VM_JIT_CACHE_WAYS;

    for (size_t way = 0u; way < VM_JIT_CACHE_WAYS; way++) {
        VmJitCacheEntry *entry = &state->entries[base + way];
        if (entry->valid != 0u && entry->block.start_ip == ip) {
            return entry;
        }
    }
    for (size_t way = 0u; way < VM_JIT_CACHE_WAYS; way++) {
        VmJitCacheEntry *entry = &state->entries[base + way];
        if (entry->valid == 0u) {
            return entry;
        }
    }

    const size_t way = state->next_way[set]++ & (VM_JIT_CACHE_WAYS - 1u);
    return &state->entries[base + way];
}

static uint32_t vm_jit_execute_one_block(VM *vm,
                                         VCPU *cpu,
                                         VmJitCpuState *state,
                                         int *can_continue) {
    VmJitCacheEntry *entry;
    uint32_t executed;
    if (can_continue) {
        *can_continue = 0;
    }
    entry = vm_jit_select_entry(state, (vm_addr_t)cpu->ip);
    if (!vm_jit_block_valid(vm, cpu, entry)) {
        vm_jit_cache_entry_clear(entry);
        if (!vm_jit_build_block(vm, cpu, state->backend, &entry->block) ||
            !state->backend->compile(&entry->block,
                                     state->memory,
                                     &entry->code)) {
            vm_jit_cache_entry_clear(entry);
            return vm_engine_execute_cached(vm, cpu);
        }
        entry->valid = 1u;
    }

    executed = entry->code.entry(vm, cpu);
    if (executed == 0u || executed > VM_JIT_ENTRY_MAX_INSTRUCTIONS) {
        vm_jit_cache_entry_clear(entry);
        return vm_engine_execute_cached(vm, cpu);
    }
    if (can_continue) {
        const uint8_t last_op = entry->block.ops[entry->block.count - 1u].op;
        *can_continue =
            entry->block.count == VM_JIT_BLOCK_MAX_OPS ||
            state->backend->terminates_block(last_op);
    }
    return executed;
}

uint32_t vm_engine_execute_jit(VM *vm, VCPU *cpu) {
    VmJitCpuState *state;
    uint32_t total = 0u;

    if (!cpu) {
        panic("No active CPU context\n", vm);
        return 1u;
    }
#if defined(VM_DEBUG)
    /* Preserve exact per-op tracing/counting in instrumentation builds. */
    return vm_engine_execute_cached(vm, cpu);
#endif
    state = vm_jit_cpu_state(cpu);
    if (!state) {
        return vm_engine_execute_cached(vm, cpu);
    }
    while (total < VM_JIT_QUANTUM_MAX && !atomic_is_vm_stopped(vm)) {
        int can_continue = 0;
        total += vm_jit_execute_one_block(vm, cpu, state, &can_continue);
        if (!can_continue || vm_interrupt_pending_fast(vm, cpu)) {
            break;
        }
    }
    return total != 0u ? total : vm_engine_execute_cached(vm, cpu);
}

void vm_jit_destroy_cpu(VCPU *cpu) {
    VmJitCpuState *state;
    if (!cpu || !cpu->jit_state) {
        return;
    }
    state = (VmJitCpuState *)cpu->jit_state;
    for (size_t i = 0u;
         i < sizeof(state->entries) / sizeof(state->entries[0]);
         i++) {
        vm_jit_cache_entry_clear(&state->entries[i]);
    }
    vm_jit_code_arena_destroy(&state->arena);
    free(state);
    cpu->jit_state = NULL;
}
