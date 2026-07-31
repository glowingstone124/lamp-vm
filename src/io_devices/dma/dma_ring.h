#ifndef VM_DMA_RING_H
#define VM_DMA_RING_H

#include <stdint.h>

#include "../../vm.h"
#include "../../../include/lampvm/device_abi.h"

typedef struct vm_dma_ring {
    uint64_t submit_iova;
    uint64_t complete_iova;
    uint32_t submit_count;
    uint32_t submit_head;
    uint32_t submit_tail;
    uint32_t complete_count;
    uint32_t complete_head;
    uint32_t complete_tail;
    uint32_t completion_sequence;
} vm_dma_ring_t;

void vm_dma_ring_reset(vm_dma_ring_t *ring);
int vm_dma_ring_count_valid(uint32_t count);
int vm_dma_ring_ready(const vm_dma_ring_t *ring);
int vm_dma_ring_set_submit_tail(vm_dma_ring_t *ring, uint32_t tail);
int vm_dma_ring_set_complete_head(vm_dma_ring_t *ring, uint32_t head);
int vm_dma_ring_submission_available(const vm_dma_ring_t *ring);
int vm_dma_ring_completion_space(const vm_dma_ring_t *ring);
int vm_dma_ring_peek(VM *vm, uint32_t iommu_dev, const vm_dma_ring_t *ring,
                     lamp_dma_desc_t *desc_out);
void vm_dma_ring_consume(vm_dma_ring_t *ring);
int vm_dma_ring_complete(VM *vm, uint32_t iommu_dev, vm_dma_ring_t *ring,
                         uint32_t cookie, uint32_t status, uint32_t bytes);

#endif /* VM_DMA_RING_H */
