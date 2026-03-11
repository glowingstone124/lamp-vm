#ifndef LAMP_KERNEL_MMU_H
#define LAMP_KERNEL_MMU_H

#include "types.h"

enum {
    MMU_PROT_READ = 0x01u,
    MMU_PROT_WRITE = 0x02u,
    MMU_PROT_EXEC = 0x04u,
    MMU_PROT_USER = 0x08u
};

void mmu_init(void);
void mmu_init_ap(void);
uint32_t mmu_enabled(void);
int mmu_map_identity(uint32_t va, uint32_t len, uint32_t prot);

#endif
