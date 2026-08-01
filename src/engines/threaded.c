#include <stdint.h>
#include <stdatomic.h>

#include "engine_internal.h"
#include "../debug.h"
#include "../fetch.h"
#include "../interrupt.h"
#include "../memory.h"

/*
 * The threaded engine executes short, predecoded basic blocks.  It is kept
 * deliberately conservative: blocks never cross a guest page, stores and
 * control-flow instructions are exits, and unsupported instructions fall
 * back to the complete shared interpreter core.
 */
#define VM_THREADED_OPCODE_LIST(X) \
    X(ADD) X(SUB) X(MUL) X(MOV) X(MOVI) X(INC) X(ADDI) X(SUBI) \
    X(AND) X(OR) X(XOR) X(NOT) X(ANDI) X(ORI) X(XORI) \
    X(SHL) X(SHR) X(SAR) X(SHLI) X(SHRI) \
    X(ROL) X(ROR) X(ROLI) X(RORI) X(CMP) X(CMPI) \
    X(LOAD) X(LOAD16) X(LOAD32) X(LOADS8) X(LOADS16) \
    X(LOADX) X(LOADX16) X(LOADX32) \
    X(STORE) X(STORE16) X(STORE32) X(STOREX) X(STOREX16) X(STOREX32) \
    X(JMP) X(RJMP) X(JZ) X(JNZ) X(JG) X(JGE) X(JL) X(JLE) X(JC) X(JNC) \
    X(RJZ) X(RJNZ) X(RJG) X(RJGE) X(RJL) X(RJLE) X(RJC) X(RJNC) \
    X(FENCE) X(PAUSE) X(CPUID)

static int vm_threaded_opcode_supported(uint8_t op) {
    switch (op) {
#define VM_THREADED_SUPPORTED_CASE(name) case OP_##name:
        VM_THREADED_OPCODE_LIST(VM_THREADED_SUPPORTED_CASE)
#undef VM_THREADED_SUPPORTED_CASE
            return 1;
        default:
            return 0;
    }
}

static int vm_threaded_opcode_terminates(uint8_t op) {
    switch (op) {
        case OP_STORE:
        case OP_STORE16:
        case OP_STORE32:
        case OP_STOREX:
        case OP_STOREX16:
        case OP_STOREX32:
        case OP_JMP:
        case OP_RJMP:
        case OP_JZ:
        case OP_JNZ:
        case OP_JG:
        case OP_JGE:
        case OP_JL:
        case OP_JLE:
        case OP_JC:
        case OP_JNC:
        case OP_RJZ:
        case OP_RJNZ:
        case OP_RJG:
        case OP_RJGE:
        case OP_RJL:
        case OP_RJLE:
        case OP_RJC:
        case OP_RJNC:
            return 1;
        default:
            return 0;
    }
}

static int vm_threaded_block_valid(const VM *vm,
                                   const VCPU *cpu,
                                   const VM_DecodedBlock *block) {
    size_t byte_count;
    if (!vm || !cpu || !block || block->valid == 0u || block->count == 0u ||
        block->start_ip != (vm_addr_t)cpu->ip ||
        block->mmu_epoch != atomic_load_explicit(&cpu->mmu_epoch,
                                                 memory_order_acquire) ||
        block->mmio_epoch != (uint32_t)atomic_load_explicit(&vm->mmio_epoch,
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

static int vm_threaded_build_block(VM *vm,
                                   VCPU *cpu,
                                   VM_DecodedBlock *block) {
    const vm_addr_t start_ip = (vm_addr_t)cpu->ip;
    const vm_addr_t start_page = start_ip & ~0xFFFu;
    const uint64_t mmu_epoch_before =
        atomic_load_explicit(&cpu->mmu_epoch, memory_order_acquire);
    const uint32_t mmio_epoch_before =
        (uint32_t)atomic_load_explicit(&vm->mmio_epoch, memory_order_acquire);
    uint32_t first_host_pa = UINT32_MAX;
    uint8_t count = 0u;

    block->valid = 0u;
    for (uint32_t i = 0u; i < VM_THREADED_BLOCK_MAX_OPS; i++) {
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
        decoded.op = (uint8_t)((inst >> 56) & 0xFFu);
        decoded.rd = (uint8_t)((inst >> 48) & 0xFFu);
        decoded.rs1 = (uint8_t)((inst >> 40) & 0xFFu);
        decoded.rs2 = (uint8_t)((inst >> 32) & 0xFFu);
        decoded.imm = (int32_t)(inst & 0xFFFFFFFFu);
        if (!vm_threaded_opcode_supported(decoded.op)) {
            break;
        }

        block->raw[count] = inst;
        block->ops[count] = decoded;
        count++;
        if (vm_threaded_opcode_terminates(decoded.op)) {
            break;
        }
    }

    if (count == 0u || first_host_pa == UINT32_MAX ||
        mmu_epoch_before != atomic_load_explicit(&cpu->mmu_epoch,
                                                 memory_order_acquire) ||
        mmio_epoch_before != (uint32_t)atomic_load_explicit(&vm->mmio_epoch,
                                                            memory_order_acquire)) {
        return 0;
    }
    block->start_ip = start_ip;
    block->host_pa = first_host_pa;
    block->mmu_epoch = mmu_epoch_before;
    block->mmio_epoch = mmio_epoch_before;
    block->count = count;
    block->reserved = 0u;
    block->valid = 1u;
    return 1;
}

#if defined(__clang__)
#define VM_THREADED_COMPUTED_GOTO 1
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-label-as-value"
#elif defined(__GNUC__)
#define VM_THREADED_COMPUTED_GOTO 1
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#else
#define VM_THREADED_COMPUTED_GOTO 0
#endif

static uint32_t vm_execute_threaded_block(VM *vm,
                                          VCPU *cpu,
                                          const VM_DecodedBlock *block) {
    const VM_DecodedOp *decoded;
    uint32_t index = 0u;
    uint32_t executed = 0u;
#if VM_THREADED_COMPUTED_GOTO
#define VM_THREADED_DISPATCH_ENTRY(name) [OP_##name] = &&vm_threaded_##name,
    static void *const dispatch[256] = {
        VM_THREADED_OPCODE_LIST(VM_THREADED_DISPATCH_ENTRY)
    };
#undef VM_THREADED_DISPATCH_ENTRY
#endif

#define VM_THREADED_PREPARE()                                                   \
    do {                                                                         \
        decoded = &block->ops[index];                                             \
        cpu->last_ip = decoded->ip;                                               \
        cpu->ip = (size_t)(decoded->ip + (vm_addr_t)sizeof(uint64_t));            \
    } while (0)
#define VM_THREADED_NEXT()                                                       \
    do {                                                                         \
        executed++;                                                              \
        index++;                                                                 \
        if (index >= block->count) {                                              \
            return executed;                                                     \
        }                                                                         \
        if ((executed & 7u) == 0u &&                                             \
            (atomic_is_vm_stopped(vm) || vm_interrupt_pending_fast(vm, cpu))) {  \
            return executed;                                                     \
        }                                                                         \
        VM_THREADED_PREPARE();                                                    \
        goto vm_threaded_dispatch;                                                \
    } while (0)
#define VM_THREADED_EXIT()                                                       \
    do {                                                                         \
        executed++;                                                              \
        return executed;                                                         \
    } while (0)

    VM_THREADED_PREPARE();
vm_threaded_dispatch:
    vm_debug_count_instruction(vm, decoded->op);
#if VM_THREADED_COMPUTED_GOTO
    if (dispatch[decoded->op] == NULL) {
        return executed;
    }
    goto *dispatch[decoded->op];
#else
    switch (decoded->op) {
#define VM_THREADED_DISPATCH_CASE(name) case OP_##name: goto vm_threaded_##name;
        VM_THREADED_OPCODE_LIST(VM_THREADED_DISPATCH_CASE)
#undef VM_THREADED_DISPATCH_CASE
        default: return executed;
    }
#endif

vm_threaded_ADD: {
        const int32_t a = cpu->regs[decoded->rs1];
        const int32_t b = cpu->regs[decoded->rs2];
        const int32_t result = a + b;
        cpu->regs[decoded->rd] = result;
        vm_engine_update_add_flags(vm, a, b, result, cpu);
        VM_THREADED_NEXT();
    }
vm_threaded_SUB: {
        const int32_t a = cpu->regs[decoded->rs1];
        const int32_t b = cpu->regs[decoded->rs2];
        const int32_t result = a - b;
        cpu->regs[decoded->rd] = result;
        vm_engine_update_sub_flags(vm, a, b, result, cpu);
        VM_THREADED_NEXT();
    }
vm_threaded_MUL:
    cpu->regs[decoded->rd] =
        cpu->regs[decoded->rs1] * cpu->regs[decoded->rs2];
    vm_engine_update_logic_flags(vm, cpu->regs[decoded->rd], cpu);
    VM_THREADED_NEXT();
vm_threaded_MOV:
    cpu->regs[decoded->rd] = cpu->regs[decoded->rs1];
    vm_engine_update_logic_flags(vm, cpu->regs[decoded->rd], cpu);
    VM_THREADED_NEXT();
vm_threaded_MOVI:
    cpu->regs[decoded->rd] = decoded->imm;
    vm_engine_update_logic_flags(vm, cpu->regs[decoded->rd], cpu);
    VM_THREADED_NEXT();
vm_threaded_INC: {
        const int32_t a = cpu->regs[decoded->rd];
        const int32_t result = a + 1;
        cpu->regs[decoded->rd] = result;
        vm_engine_update_add_flags(vm, a, 1, result, cpu);
        VM_THREADED_NEXT();
    }
vm_threaded_ADDI: {
        const int32_t a = cpu->regs[decoded->rs1];
        const int32_t result = a + decoded->imm;
        cpu->regs[decoded->rd] = result;
        vm_engine_update_add_flags(vm, a, decoded->imm, result, cpu);
        VM_THREADED_NEXT();
    }
vm_threaded_SUBI: {
        const int32_t a = cpu->regs[decoded->rs1];
        const int32_t result = a - decoded->imm;
        cpu->regs[decoded->rd] = result;
        vm_engine_update_sub_flags(vm, a, decoded->imm, result, cpu);
        VM_THREADED_NEXT();
    }
vm_threaded_AND:
    cpu->regs[decoded->rd] =
        cpu->regs[decoded->rs1] & cpu->regs[decoded->rs2];
    vm_engine_update_logic_flags(vm, cpu->regs[decoded->rd], cpu);
    VM_THREADED_NEXT();
vm_threaded_OR:
    cpu->regs[decoded->rd] =
        cpu->regs[decoded->rs1] | cpu->regs[decoded->rs2];
    vm_engine_update_logic_flags(vm, cpu->regs[decoded->rd], cpu);
    VM_THREADED_NEXT();
vm_threaded_XOR:
    cpu->regs[decoded->rd] =
        cpu->regs[decoded->rs1] ^ cpu->regs[decoded->rs2];
    vm_engine_update_logic_flags(vm, cpu->regs[decoded->rd], cpu);
    VM_THREADED_NEXT();
vm_threaded_NOT:
    cpu->regs[decoded->rd] = ~cpu->regs[decoded->rs1];
    vm_engine_update_logic_flags(vm, cpu->regs[decoded->rd], cpu);
    VM_THREADED_NEXT();
vm_threaded_ANDI:
    cpu->regs[decoded->rd] = cpu->regs[decoded->rs1] & decoded->imm;
    vm_engine_update_logic_flags(vm, cpu->regs[decoded->rd], cpu);
    VM_THREADED_NEXT();
vm_threaded_ORI:
    cpu->regs[decoded->rd] = cpu->regs[decoded->rs1] | decoded->imm;
    vm_engine_update_logic_flags(vm, cpu->regs[decoded->rd], cpu);
    VM_THREADED_NEXT();
vm_threaded_XORI:
    cpu->regs[decoded->rd] = cpu->regs[decoded->rs1] ^ decoded->imm;
    vm_engine_update_logic_flags(vm, cpu->regs[decoded->rd], cpu);
    VM_THREADED_NEXT();
vm_threaded_SHL:
    cpu->regs[decoded->rd] = (int32_t)((uint32_t)cpu->regs[decoded->rs1] <<
        ((uint32_t)cpu->regs[decoded->rs2] & 31u));
    vm_engine_update_logic_flags(vm, cpu->regs[decoded->rd], cpu);
    VM_THREADED_NEXT();
vm_threaded_SHR:
    cpu->regs[decoded->rd] = (int32_t)((uint32_t)cpu->regs[decoded->rs1] >>
        ((uint32_t)cpu->regs[decoded->rs2] & 31u));
    vm_engine_update_logic_flags(vm, cpu->regs[decoded->rd], cpu);
    VM_THREADED_NEXT();
vm_threaded_SAR:
    cpu->regs[decoded->rd] = cpu->regs[decoded->rs1] >>
        ((uint32_t)cpu->regs[decoded->rs2] & 31u);
    vm_engine_update_logic_flags(vm, cpu->regs[decoded->rd], cpu);
    VM_THREADED_NEXT();
vm_threaded_SHLI:
    cpu->regs[decoded->rd] = (int32_t)((uint32_t)cpu->regs[decoded->rs1] <<
        ((uint32_t)decoded->imm & 31u));
    vm_engine_update_logic_flags(vm, cpu->regs[decoded->rd], cpu);
    VM_THREADED_NEXT();
vm_threaded_SHRI:
    cpu->regs[decoded->rd] = (int32_t)((uint32_t)cpu->regs[decoded->rs1] >>
        ((uint32_t)decoded->imm & 31u));
    vm_engine_update_logic_flags(vm, cpu->regs[decoded->rd], cpu);
    VM_THREADED_NEXT();
vm_threaded_ROL:
    cpu->regs[decoded->rd] = (int32_t)vm_engine_rotl32(
        (uint32_t)cpu->regs[decoded->rs1],
        (uint32_t)cpu->regs[decoded->rs2] & 31u);
    vm_engine_update_logic_flags(vm, cpu->regs[decoded->rd], cpu);
    VM_THREADED_NEXT();
vm_threaded_ROR:
    cpu->regs[decoded->rd] = (int32_t)vm_engine_rotr32(
        (uint32_t)cpu->regs[decoded->rs1],
        (uint32_t)cpu->regs[decoded->rs2] & 31u);
    vm_engine_update_logic_flags(vm, cpu->regs[decoded->rd], cpu);
    VM_THREADED_NEXT();
vm_threaded_ROLI:
    cpu->regs[decoded->rd] = (int32_t)vm_engine_rotl32(
        (uint32_t)cpu->regs[decoded->rs1], (uint32_t)decoded->imm & 31u);
    vm_engine_update_logic_flags(vm, cpu->regs[decoded->rd], cpu);
    VM_THREADED_NEXT();
vm_threaded_RORI:
    cpu->regs[decoded->rd] = (int32_t)vm_engine_rotr32(
        (uint32_t)cpu->regs[decoded->rs1], (uint32_t)decoded->imm & 31u);
    vm_engine_update_logic_flags(vm, cpu->regs[decoded->rd], cpu);
    VM_THREADED_NEXT();
vm_threaded_CMP: {
        const int32_t a = cpu->regs[decoded->rd];
        const int32_t b = cpu->regs[decoded->rs1];
        vm_engine_update_sub_flags(vm, a, b, a - b, cpu);
        VM_THREADED_NEXT();
    }
vm_threaded_CMPI: {
        const int32_t a = cpu->regs[decoded->rd];
        vm_engine_update_sub_flags(vm, a, decoded->imm, a - decoded->imm, cpu);
        VM_THREADED_NEXT();
    }
vm_threaded_LOAD:
    cpu->regs[decoded->rd] = (uint32_t)vm_read8_cpu(
        vm, cpu, cpu->regs[decoded->rs1] + decoded->imm);
    vm_engine_update_zf_sf(vm, cpu->regs[decoded->rd], cpu);
    VM_THREADED_NEXT();
vm_threaded_LOAD16: {
        const vm_addr_t addr = cpu->regs[decoded->rs1] + decoded->imm;
        vm_engine_ensure_halfword_aligned_or_panic(vm, addr, "LOAD16");
        cpu->regs[decoded->rd] = (uint32_t)(
            (uint16_t)vm_read8_cpu(vm, cpu, addr) |
            ((uint16_t)vm_read8_cpu(vm, cpu, addr + 1u) << 8));
        vm_engine_update_logic_flags(vm, cpu->regs[decoded->rd], cpu);
        VM_THREADED_NEXT();
    }
vm_threaded_LOAD32:
    cpu->regs[decoded->rd] = vm_read32_cpu(
        vm, cpu, cpu->regs[decoded->rs1] + decoded->imm);
    vm_engine_update_logic_flags(vm, cpu->regs[decoded->rd], cpu);
    VM_THREADED_NEXT();
vm_threaded_LOADS8:
    cpu->regs[decoded->rd] = (int32_t)(int8_t)vm_read8_cpu(
        vm, cpu, cpu->regs[decoded->rs1] + decoded->imm);
    vm_engine_update_logic_flags(vm, cpu->regs[decoded->rd], cpu);
    VM_THREADED_NEXT();
vm_threaded_LOADS16: {
        const vm_addr_t addr = cpu->regs[decoded->rs1] + decoded->imm;
        uint16_t bits;
        vm_engine_ensure_halfword_aligned_or_panic(vm, addr, "LOADS16");
        bits = (uint16_t)((uint16_t)vm_read8_cpu(vm, cpu, addr) |
                          ((uint16_t)vm_read8_cpu(vm, cpu, addr + 1u) << 8));
        cpu->regs[decoded->rd] = (int32_t)(int16_t)bits;
        vm_engine_update_logic_flags(vm, cpu->regs[decoded->rd], cpu);
        VM_THREADED_NEXT();
    }
vm_threaded_LOADX:
    cpu->regs[decoded->rd] = (uint32_t)vm_read8_cpu(
        vm, cpu, cpu->regs[decoded->rs1] + cpu->regs[decoded->rs2] + decoded->imm);
    vm_engine_update_zf_sf(vm, cpu->regs[decoded->rd], cpu);
    VM_THREADED_NEXT();
vm_threaded_LOADX16: {
        const vm_addr_t addr = cpu->regs[decoded->rs1] +
                               cpu->regs[decoded->rs2] + decoded->imm;
        vm_engine_ensure_halfword_aligned_or_panic(vm, addr, "LOADX16");
        cpu->regs[decoded->rd] = (uint32_t)(
            (uint16_t)vm_read8_cpu(vm, cpu, addr) |
            ((uint16_t)vm_read8_cpu(vm, cpu, addr + 1u) << 8));
        vm_engine_update_logic_flags(vm, cpu->regs[decoded->rd], cpu);
        VM_THREADED_NEXT();
    }
vm_threaded_LOADX32:
    cpu->regs[decoded->rd] = vm_read32_cpu(
        vm, cpu, cpu->regs[decoded->rs1] + cpu->regs[decoded->rs2] + decoded->imm);
    vm_engine_update_logic_flags(vm, cpu->regs[decoded->rd], cpu);
    VM_THREADED_NEXT();
vm_threaded_STORE:
    vm_write8_cpu(vm, cpu, cpu->regs[decoded->rs1] + decoded->imm,
                  (uint8_t)cpu->regs[decoded->rd]);
    VM_THREADED_NEXT();
vm_threaded_STORE16: {
        const vm_addr_t addr = cpu->regs[decoded->rs1] + decoded->imm;
        const uint16_t value = (uint16_t)cpu->regs[decoded->rd];
        vm_engine_ensure_halfword_aligned_or_panic(vm, addr, "STORE16");
        vm_write8_cpu(vm, cpu, addr, (uint8_t)value);
        vm_write8_cpu(vm, cpu, addr + 1u, (uint8_t)(value >> 8));
        VM_THREADED_NEXT();
    }
vm_threaded_STORE32:
    vm_write32_cpu(vm, cpu, cpu->regs[decoded->rs1] + decoded->imm,
                   (uint32_t)cpu->regs[decoded->rd]);
    VM_THREADED_NEXT();
vm_threaded_STOREX:
    vm_write8_cpu(vm, cpu,
                  cpu->regs[decoded->rs1] + cpu->regs[decoded->rs2] + decoded->imm,
                  (uint8_t)cpu->regs[decoded->rd]);
    VM_THREADED_NEXT();
vm_threaded_STOREX16: {
        const vm_addr_t addr = cpu->regs[decoded->rs1] +
                               cpu->regs[decoded->rs2] + decoded->imm;
        const uint16_t value = (uint16_t)cpu->regs[decoded->rd];
        vm_engine_ensure_halfword_aligned_or_panic(vm, addr, "STOREX16");
        vm_write8_cpu(vm, cpu, addr, (uint8_t)value);
        vm_write8_cpu(vm, cpu, addr + 1u, (uint8_t)(value >> 8));
        VM_THREADED_NEXT();
    }
vm_threaded_STOREX32:
    vm_write32_cpu(vm, cpu,
                   cpu->regs[decoded->rs1] + cpu->regs[decoded->rs2] + decoded->imm,
                   (uint32_t)cpu->regs[decoded->rd]);
    VM_THREADED_NEXT();
vm_threaded_JMP:
    cpu->ip = (size_t)(vm_addr_t)decoded->imm;
    VM_THREADED_EXIT();
vm_threaded_RJMP:
    cpu->ip = (size_t)vm_engine_rel_target_from_last_ip(cpu, decoded->imm);
    VM_THREADED_EXIT();
vm_threaded_JZ:
    if (cpu->flags & FLAG_ZF) cpu->ip = (size_t)(vm_addr_t)decoded->imm;
    VM_THREADED_EXIT();
vm_threaded_JNZ:
    if (!(cpu->flags & FLAG_ZF)) cpu->ip = (size_t)(vm_addr_t)decoded->imm;
    VM_THREADED_EXIT();
vm_threaded_JG:
    if (!(cpu->flags & FLAG_ZF) &&
        ((cpu->flags & FLAG_SF) == (cpu->flags & FLAG_OF)))
        cpu->ip = (size_t)(vm_addr_t)decoded->imm;
    VM_THREADED_EXIT();
vm_threaded_JGE:
    if ((cpu->flags & FLAG_SF) == (cpu->flags & FLAG_OF))
        cpu->ip = (size_t)(vm_addr_t)decoded->imm;
    VM_THREADED_EXIT();
vm_threaded_JL:
    if ((cpu->flags & FLAG_SF) != (cpu->flags & FLAG_OF))
        cpu->ip = (size_t)(vm_addr_t)decoded->imm;
    VM_THREADED_EXIT();
vm_threaded_JLE:
    if ((cpu->flags & FLAG_ZF) ||
        (cpu->flags & FLAG_SF) != (cpu->flags & FLAG_OF))
        cpu->ip = (size_t)(vm_addr_t)decoded->imm;
    VM_THREADED_EXIT();
vm_threaded_JC:
    if (cpu->flags & FLAG_CF) cpu->ip = (size_t)(vm_addr_t)decoded->imm;
    VM_THREADED_EXIT();
vm_threaded_JNC:
    if (!(cpu->flags & FLAG_CF)) cpu->ip = (size_t)(vm_addr_t)decoded->imm;
    VM_THREADED_EXIT();
vm_threaded_RJZ:
    if (cpu->flags & FLAG_ZF)
        cpu->ip = (size_t)vm_engine_rel_target_from_last_ip(cpu, decoded->imm);
    VM_THREADED_EXIT();
vm_threaded_RJNZ:
    if (!(cpu->flags & FLAG_ZF))
        cpu->ip = (size_t)vm_engine_rel_target_from_last_ip(cpu, decoded->imm);
    VM_THREADED_EXIT();
vm_threaded_RJG:
    if (!(cpu->flags & FLAG_ZF) &&
        ((cpu->flags & FLAG_SF) == (cpu->flags & FLAG_OF)))
        cpu->ip = (size_t)vm_engine_rel_target_from_last_ip(cpu, decoded->imm);
    VM_THREADED_EXIT();
vm_threaded_RJGE:
    if ((cpu->flags & FLAG_SF) == (cpu->flags & FLAG_OF))
        cpu->ip = (size_t)vm_engine_rel_target_from_last_ip(cpu, decoded->imm);
    VM_THREADED_EXIT();
vm_threaded_RJL:
    if ((cpu->flags & FLAG_SF) != (cpu->flags & FLAG_OF))
        cpu->ip = (size_t)vm_engine_rel_target_from_last_ip(cpu, decoded->imm);
    VM_THREADED_EXIT();
vm_threaded_RJLE:
    if ((cpu->flags & FLAG_ZF) ||
        (cpu->flags & FLAG_SF) != (cpu->flags & FLAG_OF))
        cpu->ip = (size_t)vm_engine_rel_target_from_last_ip(cpu, decoded->imm);
    VM_THREADED_EXIT();
vm_threaded_RJC:
    if (cpu->flags & FLAG_CF)
        cpu->ip = (size_t)vm_engine_rel_target_from_last_ip(cpu, decoded->imm);
    VM_THREADED_EXIT();
vm_threaded_RJNC:
    if (!(cpu->flags & FLAG_CF))
        cpu->ip = (size_t)vm_engine_rel_target_from_last_ip(cpu, decoded->imm);
    VM_THREADED_EXIT();
vm_threaded_FENCE:
    atomic_thread_fence(memory_order_seq_cst);
    VM_THREADED_NEXT();
vm_threaded_PAUSE:
    vm_engine_host_cpu_relax();
    VM_THREADED_NEXT();
vm_threaded_CPUID:
    cpu->regs[decoded->rd] = (uint32_t)cpu->core_id;
    vm_engine_update_logic_flags(vm, cpu->regs[decoded->rd], cpu);
    VM_THREADED_NEXT();

#undef VM_THREADED_EXIT
#undef VM_THREADED_NEXT
#undef VM_THREADED_PREPARE
}

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif


uint32_t vm_engine_execute_threaded(VM *vm, VCPU *cpu) {
#ifndef VM_DEBUG
    VM_DecodedBlock *block = &cpu->threaded_blocks[
        ((vm_addr_t)cpu->ip >> 3u) &
        (VM_THREADED_BLOCK_CACHE_ENTRIES - 1u)];
    if (!vm_threaded_block_valid(vm, cpu, block) &&
        !vm_threaded_build_block(vm, cpu, block)) {
        return vm_engine_execute_cached(vm, cpu);
    }
    {
        const uint32_t executed =
            vm_execute_threaded_block(vm, cpu, block);
        if (executed != 0u) {
            return executed;
        }
    }
#endif
    return vm_engine_execute_cached(vm, cpu);
}

#undef VM_THREADED_COMPUTED_GOTO
#undef VM_THREADED_OPCODE_LIST
