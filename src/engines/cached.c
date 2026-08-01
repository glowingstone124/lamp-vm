#include <stdint.h>

#include "engine_internal.h"
#include "../fetch.h"

uint32_t vm_engine_execute_cached(VM *vm, VCPU *cpu) {
    VM_DecodedOp decoded;

    if (!cpu) {
        panic("No active CPU context\n", vm);
        return 1u;
    }
    if (!vm_fetch64_decode_translated(vm,
                                      cpu,
                                      &decoded.op,
                                      &decoded.rd,
                                      &decoded.rs1,
                                      &decoded.rs2,
                                      &decoded.imm)) {
        return 1u;
    }
    decoded.ip = (vm_addr_t)cpu->last_ip;
    vm_engine_execute_decoded(vm, cpu, &decoded);
    return 1u;
}
