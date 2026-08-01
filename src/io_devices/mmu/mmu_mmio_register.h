#ifndef VM_MMU_MMIO_REGISTER_H
#define VM_MMU_MMIO_REGISTER_H

#include "../../vm.h"

void register_mmu_mmio(VM *vm);
int vm_mmu_translate_access_cpu(VM *vm,
                                VCPU *cpu,
                                uint32_t vaddr,
                                uint32_t access,
                                uint32_t *pa_out);
int vm_mmu_translate_access(VM *vm, uint32_t vaddr, uint32_t access, uint32_t *pa_out);
void vm_mmu_flush_tlb(VM *vm, uint32_t core_id);
void vm_mmu_flush_all_tlbs(VM *vm);
uint32_t vm_mmu_fault_status(VM *vm);
uint32_t vm_mmu_fault_info(VM *vm);
uint32_t vm_mmu_root_lo(VM *vm);

#endif
