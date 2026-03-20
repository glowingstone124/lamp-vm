#include "../include/kernel/blk.h"
#include "../include/kernel/iommu.h"
#include "../include/kernel/platform.h"
#include "../include/kernel/sched.h"
#include "../include/kernel/spinlock.h"

static spinlock_t g_blk_lock;
static sched_waitq_t g_blk_waitq;
static volatile uint32_t g_blk_busy;
static volatile uint32_t g_blk_done;

enum {
    BLK_QUICK_POLL_ITERS = 131072u
};

static inline uint32_t io_in32(uint32_t addr) {
    uint32_t v;
    __asm__ volatile ("in %0, %1" : "=r"(v) : "r"(addr));
    return v;
}

static inline void io_out32(uint32_t addr, uint32_t value) {
    __asm__ volatile ("out %0, %1" :: "r"(value), "r"(addr));
}

static inline uint32_t blk_dma_range_ok(uint32_t mem_addr, uint32_t count) {
    uint32_t bytes;
    if (count == 0u) {
        return 0u;
    }
    if (count > (0xFFFFFFFFu / 512u)) {
        return 0u;
    }
    bytes = count * 512u;
    if (mem_addr >= KERNEL_MEM_SIZE) {
        return 0u;
    }
    return (bytes <= (KERNEL_MEM_SIZE - mem_addr)) ? 1u : 0u;
}

static int blk_submit_and_wait(uint32_t cmd, uint32_t lba, uint32_t count, uint32_t mem_addr) {
    uint32_t dma_addr;
    uint32_t bytes;
    uint32_t quick_poll_budget = 0u;
    if (!blk_dma_range_ok(mem_addr, count)) {
        return BLK_ERR_INVAL;
    }
    if (!(cmd == DISK_CMD_READ || cmd == DISK_CMD_WRITE)) {
        return BLK_ERR_INVAL;
    }
    bytes = count * 512u;
    if (!iommu_dma_iova(mem_addr, bytes, &dma_addr)) {
        return BLK_ERR_INVAL;
    }

    spinlock_lock(&g_blk_lock);
    if (g_blk_busy != 0u) {
        spinlock_unlock(&g_blk_lock);
        return BLK_ERR_BUSY;
    }

    g_blk_busy = 1u;
    g_blk_done = 0u;
    io_out32(IO_DISK_LBA, lba);
    io_out32(IO_DISK_MEM, dma_addr);
    io_out32(IO_DISK_COUNT, count);
    io_out32(IO_DISK_CMD, cmd);
    spinlock_unlock(&g_blk_lock);

    for (;;) {
        uint32_t done;
        uint32_t status;
        spinlock_lock(&g_blk_lock);
        done = g_blk_done;
        status = io_in32(IO_DISK_STATUS);
        if (status != DISK_STATUS_BUSY || done != 0u) {
            /*
             * Use status as source of truth: even if disk completion IRQ is lost,
             * the operation is complete once DISK_STATUS leaves BUSY.
             */
            g_blk_busy = 0u;
            g_blk_done = 0u;
            spinlock_unlock(&g_blk_lock);
            return (status == DISK_STATUS_FREE) ? BLK_OK : BLK_ERR_IO;
        }
        if (quick_poll_budget < BLK_QUICK_POLL_ITERS) {
            quick_poll_budget++;
            spinlock_unlock(&g_blk_lock);
            /*
             * For short I/O completions, switching away on every poll costs more
             * than the device latency itself, especially on single-core bring-up.
             * Keep the first window as a pure poll loop and only sleep after it.
             */
            continue;
        }
        quick_poll_budget = 0u;
        /*
         * Sleep with a short timeout so we periodically re-check DISK_STATUS
         * even when no IRQ wakeup arrives.
         */
        sched_waitq_sleep(&g_blk_waitq, 1u);
        spinlock_unlock(&g_blk_lock);
        sched_block_until_runnable();
    }
}

void blk_init(void) {
    iommu_init();
    spinlock_init(&g_blk_lock);
    sched_waitq_init(&g_blk_waitq);
    g_blk_busy = 0u;
    g_blk_done = 0u;
}

void blk_irq_complete(void) {
    uint32_t wake = 0u;
    spinlock_lock(&g_blk_lock);
    if (g_blk_busy != 0u) {
        g_blk_busy = 0u;
        g_blk_done = 1u;
        wake = 1u;
    }
    spinlock_unlock(&g_blk_lock);
    if (wake) {
        sched_waitq_wake_all(&g_blk_waitq);
    }
}

int blk_read_sectors(uint32_t lba, uint32_t count, uint32_t mem_addr) {
    return blk_submit_and_wait(DISK_CMD_READ, lba, count, mem_addr);
}

int blk_write_sectors(uint32_t lba, uint32_t count, uint32_t mem_addr) {
    return blk_submit_and_wait(DISK_CMD_WRITE, lba, count, mem_addr);
}
