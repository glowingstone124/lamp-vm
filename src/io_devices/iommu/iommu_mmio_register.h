#ifndef VM_IOMMU_MMIO_REGISTER_H
#define VM_IOMMU_MMIO_REGISTER_H

#include "../../vm.h"

void register_iommu_mmio(VM *vm);
int vm_iommu_translate_dma(VM *vm, uint32_t dev_id, uint64_t iova, uint64_t len, uint64_t *pa_out);
int vm_iommu_translate_dma_ex(VM *vm, uint32_t dev_id, uint64_t iova, uint64_t len, uint32_t access, uint64_t *pa_out);

#endif
