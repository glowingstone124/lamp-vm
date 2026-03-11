#ifndef LAMP_KERNEL_IOMMU_H
#define LAMP_KERNEL_IOMMU_H

#include "types.h"

void iommu_init(void);
uint32_t iommu_active(void);
uint32_t iommu_dma_iova(uint32_t pa_addr, uint32_t len, uint32_t *iova_out);

#endif
