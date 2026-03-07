#ifndef LAMP_KERNEL_BLK_H
#define LAMP_KERNEL_BLK_H

#include "types.h"

enum {
    BLK_OK = 0,
    BLK_ERR_INVAL = -22,
    BLK_ERR_BUSY = -16,
    BLK_ERR_IO = -5
};

void blk_init(void);
void blk_irq_complete(void);
int blk_read_sectors(uint32_t lba, uint32_t count, uint32_t mem_addr);
int blk_write_sectors(uint32_t lba, uint32_t count, uint32_t mem_addr);

#endif
