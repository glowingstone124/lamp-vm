#include "../include/kernel/mmu.h"

#include "../include/kernel/platform.h"
#include "../include/kernel/printk.h"
#include "../include/kernel/vm_info.h"

#define MMU_TAG "mmu"
#define MMU_PTE_P 0x00000001u
#define MMU_PTE_W 0x00000002u
#define MMU_PTE_U 0x00000004u
#define MMU_PTE_X 0x00000008u
#define MMU_MAP_SPAN_BYTES 0x00400000u /* 4MB per L1 entry */
#define MMU_L1_ENTRIES 1024u
#define MMU_L2_ENTRIES 1024u
#define MMU_PAGE_SIZE 4096u
#define MMU_PTE_PAGE_SHIFT 12u
#define MMU_PDE_SHIFT 22u
#define MMU_VA_MAP_LIMIT (KERNEL_MEM_SIZE + FB_SIZE)
#define MMU_L2_TABLES ((MMU_VA_MAP_LIMIT + MMU_MAP_SPAN_BYTES - 1u) / MMU_MAP_SPAN_BYTES)

static volatile uint32_t g_mmu_enabled;
static volatile uint32_t g_root_pa_lo;

static uint32_t g_l1[MMU_L1_ENTRIES] __attribute__((aligned(4096)));
static uint32_t g_l2[MMU_L2_TABLES][MMU_L2_ENTRIES] __attribute__((aligned(4096)));

extern uint8_t __text_start[];
extern uint8_t __text_end[];
extern uint8_t __rodata_start[];
extern uint8_t __rodata_end[];

static inline void mmio_write32(uint32_t addr, uint32_t value) {
    *(volatile uint32_t *)(uintptr_t)addr = value;
}

static inline uint32_t mmio_read32(uint32_t addr) {
    return *(volatile uint32_t *)(uintptr_t)addr;
}

static inline uint32_t align_down_4k(uint32_t v) {
    return v & ~(MMU_PAGE_SIZE - 1u);
}

static inline uint32_t align_up_4k(uint32_t v) {
    return (v + MMU_PAGE_SIZE - 1u) & ~(MMU_PAGE_SIZE - 1u);
}

static inline uint32_t mmu_prot_to_pte_flags(uint32_t prot) {
    uint32_t flags = 0u;
    if (prot & MMU_PROT_WRITE) {
        flags |= MMU_PTE_W;
    }
    if (prot & MMU_PROT_EXEC) {
        flags |= MMU_PTE_X;
    }
    if (prot & MMU_PROT_USER) {
        flags |= MMU_PTE_U;
    }
    return flags;
}

static int mmu_map_identity_page(uint32_t va, uint32_t prot) {
    uint32_t pde;
    uint32_t pte;
    uint32_t pte_flags;
    uint32_t pde_flags;

    if (va >= MMU_VA_MAP_LIMIT) {
        return -1;
    }
    pde = va >> MMU_PDE_SHIFT;
    pte = (va >> MMU_PTE_PAGE_SHIFT) & 0x3FFu;
    if (pde >= MMU_L2_TABLES) {
        return -1;
    }

    pte_flags = mmu_prot_to_pte_flags(prot);
    g_l2[pde][pte] = (va & 0xFFFFF000u) | MMU_PTE_P | pte_flags;

    pde_flags = 0u;
    for (uint32_t i = 0u; i < MMU_L2_ENTRIES; i++) {
        if ((g_l2[pde][i] & MMU_PTE_P) == 0u) {
            continue;
        }
        pde_flags |= g_l2[pde][i] & (MMU_PTE_W | MMU_PTE_U | MMU_PTE_X);
    }
    g_l1[pde] = ((uint32_t)(uintptr_t)&g_l2[pde][0] & 0xFFFFF000u) | MMU_PTE_P | pde_flags;
    return 0;
}

static inline uint32_t mmu_kernel_page_prot(uint32_t pa) {
    const uint32_t text_lo = align_down_4k((uint32_t)(uintptr_t)__text_start);
    const uint32_t text_hi = align_up_4k((uint32_t)(uintptr_t)__text_end);
    const uint32_t ro_lo = align_down_4k((uint32_t)(uintptr_t)__rodata_start);
    const uint32_t ro_hi = align_up_4k((uint32_t)(uintptr_t)__rodata_end);

    if (pa >= text_lo && pa < text_hi) {
        return MMU_PROT_READ | MMU_PROT_EXEC;
    }
    if (pa >= ro_lo && pa < ro_hi) {
        return MMU_PROT_READ;
    }
    return MMU_PROT_READ | MMU_PROT_WRITE;
}

static void mmu_build_kernel_identity_map(void) {
    const uint32_t pde_count = MMU_L2_TABLES;

    for (uint32_t i = 0u; i < MMU_L1_ENTRIES; i++) {
        g_l1[i] = 0u;
    }
    for (uint32_t pde = 0u; pde < pde_count; pde++) {
        uintptr_t l2_pa = (uintptr_t)&g_l2[pde][0];
        uint32_t pde_flags = 0u;
        for (uint32_t pte = 0u; pte < MMU_L2_ENTRIES; pte++) {
            const uint32_t pa = (pde << MMU_PDE_SHIFT) | (pte << MMU_PTE_PAGE_SHIFT);
            uint32_t pte_flags;
            if (pa >= MMU_VA_MAP_LIMIT) {
                g_l2[pde][pte] = 0u;
                continue;
            }
            pte_flags = mmu_prot_to_pte_flags(mmu_kernel_page_prot(pa));
            pde_flags |= pte_flags;
            g_l2[pde][pte] = (pa & 0xFFFFF000u) | MMU_PTE_P | pte_flags;
        }
        g_l1[pde] = ((uint32_t)l2_pa & 0xFFFFF000u) | MMU_PTE_P | pde_flags;
    }
}

static uint32_t mmu_enable_local(uint32_t root_pa_lo) {
    mmio_write32(MMU_MMIO_BASE + MMU_REG_CTRL, 0u);
    mmio_write32(MMU_MMIO_BASE + MMU_REG_ROOT_LO, root_pa_lo);
    mmio_write32(MMU_MMIO_BASE + MMU_REG_ROOT_HI, 0u);
    mmio_write32(MMU_MMIO_BASE + MMU_REG_FAULT_STATUS, 1u);
    mmio_write32(MMU_MMIO_BASE + MMU_REG_CTRL, 1u);
    return (mmio_read32(MMU_MMIO_BASE + MMU_REG_CTRL) & 0x1u) ? 1u : 0u;
}

int mmu_map_identity(uint32_t va, uint32_t len, uint32_t prot) {
    uint32_t start;
    uint32_t end;
    uint32_t page;

    if (len == 0u) {
        return 0;
    }
    if (va >= MMU_VA_MAP_LIMIT || len > (MMU_VA_MAP_LIMIT - va)) {
        return -1;
    }

    start = align_down_4k(va);
    end = align_up_4k(va + len);
    if (end < start || end > MMU_VA_MAP_LIMIT) {
        return -1;
    }

    for (page = start; page < end; page += MMU_PAGE_SIZE) {
        if (mmu_map_identity_page(page, prot) != 0) {
            return -1;
        }
    }
    return 0;
}

void mmu_init(void) {
    boot_info_t info;
    g_mmu_enabled = 0u;
    g_root_pa_lo = 0u;

    if (!vm_info_load_boot(&info)) {
        KLOGW(MMU_TAG, "bootinfo missing, paging bypass");
        return;
    }
    if ((info.features & BOOTINFO_FEATURE_MMU_PAGING) == 0u) {
        KLOGI(MMU_TAG, "feature absent, bypass");
        return;
    }
    if (info.page_size != MMU_PAGE_SIZE) {
        KLOGW(MMU_TAG, "unexpected page size, bypass");
        return;
    }

    mmu_build_kernel_identity_map();
    g_root_pa_lo = (uint32_t)((uintptr_t)&g_l1[0]);
    g_mmu_enabled = mmu_enable_local(g_root_pa_lo);
    if (!g_mmu_enabled) {
        KLOGW(MMU_TAG, "enable failed, bypass");
        return;
    }
    KLOGI(MMU_TAG, "paging enabled text=rx rodata=r data=rw");
}

void mmu_init_ap(void) {
    if (!g_mmu_enabled || g_root_pa_lo == 0u) {
        return;
    }
    if (!mmu_enable_local(g_root_pa_lo)) {
        KLOGW(MMU_TAG, "ap paging enable failed");
    }
}

uint32_t mmu_enabled(void) {
    return g_mmu_enabled ? 1u : 0u;
}
