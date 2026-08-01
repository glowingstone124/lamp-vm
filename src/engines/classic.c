#include <stdint.h>

#include "engine_internal.h"
#include "../fetch.h"

uint32_t vm_engine_execute_classic(VM *vm, VCPU *cpu) {
    VM_DecodedOp decoded;
    uint64_t instruction = 0u;

    if (!cpu) {
        panic("No active CPU context\n", vm);
        return 1u;
    }

    decoded.ip = (vm_addr_t)cpu->ip;
    cpu->last_ip = decoded.ip;
    if (!vm_fetch64_exec_cpu(vm, cpu, decoded.ip, &instruction)) {
        return 1u;
    }
    cpu->ip = (size_t)(decoded.ip + (vm_addr_t)sizeof(uint64_t));
    vm_decode_inst_cached(cpu,
                          decoded.ip,
                          instruction,
                          &decoded.op,
                          &decoded.rd,
                          &decoded.rs1,
                          &decoded.rs2,
                          &decoded.imm);
    vm_engine_execute_decoded(vm, cpu, &decoded);
    return 1u;
}
