#include "../include/kernel/iommu.h"
#include "../include/kernel/platform.h"
#include "../include/kernel/printk.h"
#include "../include/kernel/vm_info.h"

#define IOMMU_TAG "iommu"
#define IOMMU_DISK_DEV_ID 0u
#define IOMMU_DMA_IOVA_BASE 0x01000000u

static volatile uint32_t g_iommu_active;

static inline void mmio_write32(uint32_t addr, uint32_t value) {
    *(volatile uint32_t *)(uintptr_t)addr = value;
}

static inline uint32_t mmio_read32(uint32_t addr) {
    return *(volatile uint32_t *)(uintptr_t)addr;
}

void iommu_init(void) {
    boot_info_t info;
    uint32_t cap;
    uint32_t devs;

    g_iommu_active = 0u;
    if (!vm_info_load_boot(&info)) {
        KLOGW(IOMMU_TAG, "bootinfo missing, disabled");
        return;
    }
    if ((info.features & BOOTINFO_FEATURE_IOMMU_MMIO) == 0u) {
        KLOGI(IOMMU_TAG, "feature absent, bypass");
        return;
    }

    cap = mmio_read32(IOMMU_MMIO_BASE + IOMMU_REG_CAP);
    devs = cap & 0xFFu;
    if (devs <= IOMMU_DISK_DEV_ID) {
        KLOGW(IOMMU_TAG, "invalid cap, bypass");
        return;
    }

    mmio_write32(IOMMU_MMIO_BASE + IOMMU_REG_CTRL, 0u);
    mmio_write32(IOMMU_MMIO_BASE + IOMMU_REG_DEVSEL, IOMMU_DISK_DEV_ID);
    mmio_write32(IOMMU_MMIO_BASE + IOMMU_REG_DEV_CTRL, 0u);
    mmio_write32(IOMMU_MMIO_BASE + IOMMU_REG_IOVA_BASE_LO, IOMMU_DMA_IOVA_BASE);
    mmio_write32(IOMMU_MMIO_BASE + IOMMU_REG_IOVA_BASE_HI, 0u);
    mmio_write32(IOMMU_MMIO_BASE + IOMMU_REG_IOVA_SIZE, KERNEL_MEM_SIZE);
    mmio_write32(IOMMU_MMIO_BASE + IOMMU_REG_PA_BASE_LO, 0u);
    mmio_write32(IOMMU_MMIO_BASE + IOMMU_REG_PA_BASE_HI, 0u);
    mmio_write32(IOMMU_MMIO_BASE + IOMMU_REG_FAULT_STATUS, 1u);
    mmio_write32(IOMMU_MMIO_BASE + IOMMU_REG_DEV_CTRL, 1u);
    mmio_write32(IOMMU_MMIO_BASE + IOMMU_REG_CTRL, 1u);

    g_iommu_active = 1u;
    KLOGI(IOMMU_TAG, "enabled disk iova window");
}

uint32_t iommu_active(void) {
    return g_iommu_active ? 1u : 0u;
}

uint32_t iommu_dma_iova(uint32_t pa_addr, uint32_t len, uint32_t *iova_out) {
    uint32_t iova;
    if (!iova_out || len == 0u) {
        return 0u;
    }
    if (pa_addr >= KERNEL_MEM_SIZE || len > (KERNEL_MEM_SIZE - pa_addr)) {
        return 0u;
    }
    if (!g_iommu_active) {
        *iova_out = pa_addr;
        return 1u;
    }
    if (pa_addr > (0xFFFFFFFFu - IOMMU_DMA_IOVA_BASE)) {
        return 0u;
    }
    iova = IOMMU_DMA_IOVA_BASE + pa_addr;
    *iova_out = iova;
    return 1u;
}
