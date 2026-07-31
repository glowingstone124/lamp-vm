#include "dma_ring.h"

#include <string.h>

#include "../iommu/iommu_mmio_register.h"

static uint32_t dma_from_le32(uint32_t value) {
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return __builtin_bswap32(value);
#else
    return value;
#endif
}

static uint32_t dma_to_le32(uint32_t value) {
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return __builtin_bswap32(value);
#else
    return value;
#endif
}

static int dma_guest_copy_from(VM *vm, uint32_t iommu_dev, uint64_t iova,
                               void *dst, uint32_t len) {
    uint64_t pa = 0u;
    if (!vm || !dst || len == 0u ||
        !vm_iommu_translate_dma_ex(vm, iommu_dev, iova, len, IOMMU_DMA_READ, &pa)) {
        return 0;
    }
    if (pa >= (uint64_t)vm->memory_size || len > (uint64_t)vm->memory_size - pa) {
        return 0;
    }
    memcpy(dst, &vm->memory[(size_t)pa], len);
    return 1;
}

static int dma_guest_copy_to(VM *vm, uint32_t iommu_dev, uint64_t iova,
                             const void *src, uint32_t len) {
    uint64_t pa = 0u;
    if (!vm || !src || len == 0u ||
        !vm_iommu_translate_dma_ex(vm, iommu_dev, iova, len, IOMMU_DMA_WRITE, &pa)) {
        return 0;
    }
    if (pa >= (uint64_t)vm->memory_size || len > (uint64_t)vm->memory_size - pa) {
        return 0;
    }
    memcpy(&vm->memory[(size_t)pa], src, len);
    return 1;
}

void vm_dma_ring_reset(vm_dma_ring_t *ring) {
    if (!ring) {
        return;
    }
    memset(ring, 0, sizeof(*ring));
}

int vm_dma_ring_count_valid(uint32_t count) {
    return count >= LAMP_DMA_RING_MIN_COUNT && count <= LAMP_DMA_RING_MAX_COUNT &&
           (count & (count - 1u)) == 0u;
}

int vm_dma_ring_ready(const vm_dma_ring_t *ring) {
    return ring && vm_dma_ring_count_valid(ring->submit_count) &&
           vm_dma_ring_count_valid(ring->complete_count);
}

int vm_dma_ring_set_submit_tail(vm_dma_ring_t *ring, uint32_t tail) {
    if (!ring || !vm_dma_ring_count_valid(ring->submit_count) || tail >= ring->submit_count) {
        return 0;
    }
    ring->submit_tail = tail;
    return 1;
}

int vm_dma_ring_set_complete_head(vm_dma_ring_t *ring, uint32_t head) {
    if (!ring || !vm_dma_ring_count_valid(ring->complete_count) || head >= ring->complete_count) {
        return 0;
    }
    ring->complete_head = head;
    return 1;
}

int vm_dma_ring_submission_available(const vm_dma_ring_t *ring) {
    return ring && ring->submit_head != ring->submit_tail;
}

int vm_dma_ring_completion_space(const vm_dma_ring_t *ring) {
    uint32_t next;
    if (!ring || !vm_dma_ring_count_valid(ring->complete_count)) {
        return 0;
    }
    next = (ring->complete_tail + 1u) & (ring->complete_count - 1u);
    return next != ring->complete_head;
}

int vm_dma_ring_peek(VM *vm, uint32_t iommu_dev, const vm_dma_ring_t *ring,
                     lamp_dma_desc_t *desc_out) {
    lamp_dma_desc_t raw;
    uint64_t desc_iova;
    if (!desc_out || !vm_dma_ring_ready(ring) || !vm_dma_ring_submission_available(ring)) {
        return 0;
    }
    desc_iova = ring->submit_iova + (uint64_t)ring->submit_head * sizeof(raw);
    if (!dma_guest_copy_from(vm, iommu_dev, desc_iova, &raw, (uint32_t)sizeof(raw))) {
        return 0;
    }
    desc_out->addr_lo = dma_from_le32(raw.addr_lo);
    desc_out->addr_hi = dma_from_le32(raw.addr_hi);
    desc_out->length = dma_from_le32(raw.length);
    desc_out->flags = dma_from_le32(raw.flags);
    desc_out->cookie = dma_from_le32(raw.cookie);
    desc_out->reserved0 = dma_from_le32(raw.reserved0);
    desc_out->reserved1 = dma_from_le32(raw.reserved1);
    desc_out->reserved2 = dma_from_le32(raw.reserved2);
    return 1;
}

void vm_dma_ring_consume(vm_dma_ring_t *ring) {
    if (!ring || !vm_dma_ring_count_valid(ring->submit_count)) {
        return;
    }
    ring->submit_head = (ring->submit_head + 1u) & (ring->submit_count - 1u);
}

int vm_dma_ring_complete(VM *vm, uint32_t iommu_dev, vm_dma_ring_t *ring,
                         uint32_t cookie, uint32_t status, uint32_t bytes) {
    lamp_dma_completion_t completion;
    uint64_t completion_iova;
    uint32_t next;
    if (!vm_dma_ring_ready(ring) || !vm_dma_ring_completion_space(ring)) {
        return 0;
    }
    next = (ring->complete_tail + 1u) & (ring->complete_count - 1u);
    ring->completion_sequence++;
    completion.cookie = dma_to_le32(cookie);
    completion.status = dma_to_le32(status);
    completion.bytes = dma_to_le32(bytes);
    completion.sequence = dma_to_le32(ring->completion_sequence);
    completion_iova = ring->complete_iova + (uint64_t)ring->complete_tail * sizeof(completion);
    if (!dma_guest_copy_to(vm, iommu_dev, completion_iova, &completion,
                           (uint32_t)sizeof(completion))) {
        return 0;
    }
    ring->complete_tail = next;
    return 1;
}
