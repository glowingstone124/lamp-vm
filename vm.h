
//
// Created by Max Wang on 2025/12/28.
//
#pragma once
#ifndef VM_VM_H
#define VM_VM_H
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>

static inline uint64_t INST(uint8_t op, uint8_t rd, uint8_t rs1, uint8_t rs2, uint32_t imm) {
    return ((uint64_t)op << 56 | (uint64_t)rd << 48 | (uint64_t)rs1 << 40 | (uint64_t)rs2 << 32) |
        imm;
}
typedef struct VM VM;
typedef struct VCPU VCPU;
#ifdef VM_DEBUG
typedef struct VM_Debug VM_Debug;
#endif
#define MAX_MMIO_DEVICES 16

#define FB_WIDTH 640
#define FB_HEIGHT 480
#define FB_BPP 4
#define FB_SIZE (FB_WIDTH * FB_HEIGHT * FB_BPP)

#define IO_SIZE 256

#define REG_COUNT 32
#define DUMP_MEM_SEEK_LEN 16

#define IVT_SIZE 256
#define IRQ_BITMAP_WORDS (IVT_SIZE / 64)
#define IRQ_BITMAP_WORDS32 (IVT_SIZE / 32)
#define IVT_ENTRY_SIZE 8
#define CALL_STACK_SIZE 256
#define DATA_STACK_SIZE 256
#define ISR_STACK_SIZE 256

#define TIME_REALTIME_OFFSET   0
#define TIME_MONOTONIC_OFFSET  8
#define TIME_BOOTTIME_OFFSET   16

#define IVT_BASE 0x0000
#define CALL_STACK_BASE (IVT_BASE + IVT_SIZE * IVT_ENTRY_SIZE)
#define DATA_STACK_BASE (CALL_STACK_BASE + CALL_STACK_SIZE * 8)
#define ISR_STACK_BASE (DATA_STACK_BASE + DATA_STACK_SIZE * 8)
#define TIME_BASE (ISR_STACK_BASE + ISR_STACK_SIZE * 8)
#define PROGRAM_BASE (TIME_BASE + 28)

#define FB_BASE(addr_space_size) (addr_space_size)
#define FB_LEGACY_BASE 0x00620000u
#define SYSINFO_BASE (FB_LEGACY_BASE + FB_SIZE)
#define SYSINFO_MAGIC 0x31494D56u /* "VMI1" */
#define SYSINFO_LAYOUT_VERSION 2u
#define SYSINFO_VENDOR_WORDS 4u
#define SYSINFO_VENDOR_BYTES (SYSINFO_VENDOR_WORDS * 4u)
#define SYSINFO_ARCH_LAMP32 1u
#define SYSINFO_ENDIAN_LITTLE 1u
#define SYSINFO_PHYS_ADDR_BITS_32 32u
#define SYSINFO_DEFAULT_PAGE_SIZE 4096u
#define SYSINFO_TIMER_FREQ_1GHZ 1000000000u
#define SYSINFO_FEATURE_TIME_MMIO (1u << 0)
#define SYSINFO_FEATURE_FB_MMIO (1u << 1)
#define SYSINFO_FEATURE_DISK_IO (1u << 2)
#define SYSINFO_FEATURE_SMP (1u << 3)
#define SYSINFO_FEATURE_TIMER_IRQ (1u << 4)
#define SYSINFO_FEATURE_INTC_MMIO (1u << 5)
#define SYSINFO_FEATURE_IOMMU_MMIO (1u << 6)
#define SYSINFO_FEATURE_MMU_PAGING (1u << 7)
#define SYSINFO_REG_MAGIC 0x00u
#define SYSINFO_REG_VENDOR0 0x04u
#define SYSINFO_REG_MEM_BYTES_LO 0x14u
#define SYSINFO_REG_MEM_BYTES_HI 0x18u
#define SYSINFO_REG_DISK_BYTES_LO 0x1Cu
#define SYSINFO_REG_DISK_BYTES_HI 0x20u
#define SYSINFO_REG_SMP_CORES 0x24u
#define SYSINFO_REG_LAYOUT_VERSION 0x28u
#define SYSINFO_REG_ARCH_ID 0x2Cu
#define SYSINFO_REG_ENDIAN 0x30u
#define SYSINFO_REG_PHYS_ADDR_BITS 0x34u
#define SYSINFO_REG_PAGE_SIZE 0x38u
#define SYSINFO_REG_TIMER_FREQ_HZ 0x3Cu
#define SYSINFO_REG_FEATURES 0x40u
#define SYSINFO_REG_FB_WIDTH 0x44u
#define SYSINFO_REG_FB_HEIGHT 0x48u
#define SYSINFO_REG_FB_BPP 0x4Cu
#define SYSINFO_REG_FB_STRIDE_BYTES 0x50u
#define SYSINFO_REG_BOOT_REALTIME_NS_LO 0x54u
#define SYSINFO_REG_BOOT_REALTIME_NS_HI 0x58u
#define SYSINFO_SIZE 0x5Cu

#define INTC_BASE 0x0074D000u
#define INTC_REG_PENDING 0x000u
#define INTC_REG_ENABLE 0x040u
#define INTC_REG_PRIORITY 0x100u
#define INTC_REG_EOI 0x500u
#define INTC_MMIO_SIZE 0x504u

#define IOMMU_BASE 0x0074E000u
#define IOMMU_REG_CAP 0x000u
#define IOMMU_REG_CTRL 0x004u
#define IOMMU_REG_DEVSEL 0x008u
#define IOMMU_REG_DEV_CTRL 0x00Cu
#define IOMMU_REG_IOVA_BASE_LO 0x010u
#define IOMMU_REG_IOVA_BASE_HI 0x014u
#define IOMMU_REG_IOVA_SIZE 0x018u
#define IOMMU_REG_PA_BASE_LO 0x01Cu
#define IOMMU_REG_PA_BASE_HI 0x020u
#define IOMMU_REG_FAULT_STATUS 0x024u
#define IOMMU_REG_FAULT_DEV 0x028u
#define IOMMU_REG_FAULT_IOVA_LO 0x02Cu
#define IOMMU_REG_FAULT_IOVA_HI 0x030u
#define IOMMU_REG_FAULT_LEN 0x034u
#define IOMMU_MMIO_SIZE 0x100u

#define IOMMU_CTRL_ENABLE 0x01u
#define IOMMU_DEV_CTRL_ENABLE 0x01u

#define IOMMU_FAULT_VALID 0x01u
#define IOMMU_FAULT_REASON_SHIFT 4u
#define IOMMU_FAULT_REASON_DEV_INVALID 1u
#define IOMMU_FAULT_REASON_UNMAPPED 2u
#define IOMMU_FAULT_REASON_BOUNDS 3u
#define IOMMU_FAULT_REASON_PA_RANGE 4u

#define IOMMU_MAX_DEVICES 4u
#define IOMMU_DEV_DISK 0u

#define MMU_MAX_CORES 32u

#define MMU_BASE 0x0074F000u
#define MMU_REG_CAP 0x000u
#define MMU_REG_CTRL 0x004u
#define MMU_REG_ROOT_LO 0x008u
#define MMU_REG_ROOT_HI 0x00Cu
#define MMU_REG_FAULT_STATUS 0x010u
#define MMU_REG_FAULT_ADDR_LO 0x014u
#define MMU_REG_FAULT_ADDR_HI 0x018u
#define MMU_REG_FAULT_INFO 0x01Cu
#define MMU_MMIO_SIZE 0x100u

#define MMU_CTRL_ENABLE 0x01u

#define MMU_PTE_P 0x00000001u
#define MMU_PTE_W 0x00000002u
#define MMU_PTE_U 0x00000004u
#define MMU_PTE_X 0x00000008u

#define MMU_FAULT_VALID 0x01u
#define MMU_FAULT_REASON_SHIFT 4u
#define MMU_FAULT_REASON_NOT_PRESENT 1u
#define MMU_FAULT_REASON_PERM 2u
#define MMU_FAULT_REASON_PTABLE_OOB 3u
#define MMU_FAULT_REASON_BAD_ROOT 4u

#define VM_MMU_ACC_READ 0x01u
#define VM_MMU_ACC_WRITE 0x02u
#define VM_MMU_ACC_EXEC 0x04u
#define VM_MMU_ACC_USER 0x08u
typedef uint32_t vm_addr_t;

typedef struct {
    FILE *fp;

    uint32_t lba;
    uint32_t mem_addr;
    uint32_t count;

    pthread_t worker_thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond_var;

    uint8_t status;
    int pending_cmd;
    int current_cmd;
    bool thread_running;
    bool op_complete;
} Disk;

typedef struct {
    uint32_t ctrl;
    uint32_t selected_dev;
    uint32_t fault_status;
    uint32_t fault_dev;
    uint64_t fault_iova;
    uint32_t fault_len;
    struct {
        uint32_t ctrl;
        uint32_t iova_size;
        uint64_t iova_base;
        uint64_t pa_base;
    } devices[IOMMU_MAX_DEVICES];
} IOMMU;

typedef struct {
    uint32_t ctrl[MMU_MAX_CORES];
    uint64_t root[MMU_MAX_CORES];
    uint32_t fault_status[MMU_MAX_CORES];
    uint64_t fault_addr[MMU_MAX_CORES];
    uint32_t fault_info[MMU_MAX_CORES];
} MMU;
typedef uint32_t (*mmio_read32_fn)(VM *vm, uint32_t addr);
typedef void (*mmio_write32_fn)(VM *vm, uint32_t addr, uint32_t val);
typedef struct {
    uint32_t start;
    uint32_t end;
    mmio_read32_fn read32;
    mmio_write32_fn write32;
} MMIO_Device;
struct VCPU {
    uint32_t regs[REG_COUNT];

    atomic_uint_fast64_t execution_times;
    size_t ip;
    size_t last_ip;
    unsigned int flags;
    int dsp;
    int csp;
    int isp;
    int in_interrupt;
    int irq_masked;
    int core_id;
    vm_addr_t call_stack_base;
    vm_addr_t data_stack_base;
    vm_addr_t isr_stack_base;
    int is_bsp;
};

extern _Thread_local VCPU *vm_tls_vcpu;

static inline VCPU *vm_current_cpu(VM *vm);

struct VM{
    int halted;
    int panic;
    uint8_t *memory;
    size_t memory_size;

    /*
     * framebuffer is mapped after main memory:
     * [fb_base, fb_base + FB_SIZE)
     */
    uint32_t *fb;
    uint32_t *fb_front;
    pthread_mutex_t fb_row_locks[FB_HEIGHT];

    int io[IO_SIZE];
    uint8_t serial_rx_fifo[256];
    uint16_t serial_rx_head;
    uint16_t serial_rx_tail;

    Disk disk;
    IOMMU iommu;
    MMU mmu;
    atomic_uint_fast64_t *interrupt_bitmap;
    atomic_uint_fast64_t *interrupt_enable_bitmap;
    atomic_uchar interrupt_priority[IVT_SIZE];

    uint64_t start_realtime_ns;
    uint64_t start_monotonic_ns;
    uint64_t latched_realtime;
    uint64_t latched_monotonic;
    uint64_t latched_boottime;
    uint64_t disk_size_bytes;
    uint32_t sysinfo_vendor_words[SYSINFO_VENDOR_WORDS];

    int suspend_count;

    MMIO_Device *mmio_devices[MAX_MMIO_DEVICES];
    int mmio_count;
    MMIO_Device *mmio_cache_dev;
    uint32_t mmio_cache_start;
    uint32_t mmio_cache_end;

    /*
     * SMP runtime configuration and state.
     */
    int smp_cores;
    VCPU *cpus;
    atomic_bool *core_released;
    pthread_mutex_t shared_lock;
    vm_addr_t stack_pool_base;
    size_t stack_pool_size;

    /*
     * Timer shared datas.
     */
    atomic_uint timer_period_us;
    atomic_uint_fast64_t timer_next_deadline_ns;
    atomic_bool timer_enabled;
    atomic_bool timer_thread_running;
    int timer_thread_started;
    pthread_t timer_worker_thread;

#ifdef VM_DEBUG
    VM_Debug *debug;
#endif
};

static inline VCPU *vm_current_cpu(VM *vm) {
    if (vm_tls_vcpu)
        return vm_tls_vcpu;
    if (!vm || !vm->cpus)
        return NULL;
    return &vm->cpus[0];
}

static inline void vm_shared_lock(VM *vm) {
    if (vm)
        pthread_mutex_lock(&vm->shared_lock);
}

static inline void vm_shared_unlock(VM *vm) {
    if (vm)
        pthread_mutex_unlock(&vm->shared_lock);
}

static inline size_t vm_fb_row_from_byte_index(size_t fb_byte_index) {
    return fb_byte_index / (size_t)(FB_WIDTH * FB_BPP);
}

static inline size_t vm_fb_row_from_pixel_index(size_t fb_pixel_index) {
    return fb_pixel_index / (size_t)FB_WIDTH;
}

static inline void vm_fb_row_lock(VM *vm, size_t row) {
    if (vm && row < FB_HEIGHT) {
        pthread_mutex_lock(&vm->fb_row_locks[row]);
    }
}

static inline void vm_fb_row_unlock(VM *vm, size_t row) {
    if (vm && row < FB_HEIGHT) {
        pthread_mutex_unlock(&vm->fb_row_locks[row]);
    }
}

enum {
    OP_ADD = 0x01,
    OP_SUB = 0x02,
    OP_MUL = 0x03,
    OP_DIV = 0x04,
    OP_HALT = 0x05,
    OP_JMP = 0x06,
    OP_JZ = 0x07,
    OP_PUSH = 0x08,
    OP_POP = 0x09,
    OP_CALL = 0x0A,
    OP_RET = 0x0B,
    OP_LOAD = 0x0C,
    OP_LOAD32 = 0x0D,
    OP_LOADX32 = 0x0E,
    OP_STORE = 0x0F,
    OP_STORE32 = 0x10,
    OP_STOREX32 = 0x11,
    OP_CMP = 0x12,
    OP_CMPI = 0x13,
    OP_MOV = 0x14,
    OP_MOVI = 0x15,
    OP_MEMSET = 0x16,
    OP_MEMCPY = 0x17,
    OP_IN = 0x18,
    OP_OUT = 0x19,
    OP_INT = 0x1A,
    OP_IRET = 0x1B,
    OP_MOD = 0x1C,
    OP_AND = 0x1D,
    OP_OR = 0x1E,
    OP_XOR = 0x1F,
    OP_NOT = 0x20,
    OP_SHL = 0x21,
    OP_SHR = 0x22,
    OP_SAR = 0x23,
    OP_JNZ = 0x24,
    OP_JG = 0x25,
    OP_JGE = 0x26,
    OP_JL = 0x27,
    OP_JLE = 0x28,
    OP_JC = 0x29,
    OP_JNC = 0x2A,
    OP_FADD = 0x2B,
    OP_FSUB = 0x2C,
    OP_FMUL = 0x2D,
    OP_FDIV = 0x2E,
    OP_FNEG = 0x2F,
    OP_FABS = 0x30,
    OP_FSQRT = 0x31,
    OP_FCMP = 0x32,
    OP_ITOF = 0x33,
    OP_FTOI = 0x34,
    OP_FLOAD32 = 0x35,
    OP_FSTORE32 = 0x36,
    OP_INC = 0x37,
    OP_ADDI = 0x38,
    OP_SUBI = 0x39,
    OP_ANDI = 0x3A,
    OP_ORI = 0x3B,
    OP_XORI = 0x3C,
    OP_SHLI = 0x3D,
    OP_SHRI = 0x3E,
    OP_CAS = 0x3F,
    OP_XADD = 0x40,
    OP_XCHG = 0x41,
    OP_LDAR = 0x42,
    OP_STLR = 0x43,
    OP_FENCE = 0x44,
    OP_PAUSE = 0x45,
    OP_STARTAP = 0x46,
    OP_IPI = 0x47,
    OP_CPUID = 0x48,
    OP_CALLR = 0x49,
    OP_RJMP = 0x4A,
    OP_RCALL = 0x4B,
    OP_RJZ = 0x4C,
    OP_RJNZ = 0x4D,
    OP_ROL = 0x4E,
    OP_ROR = 0x4F,
    OP_ROLI = 0x50,
    OP_RORI = 0x51,
    OP_LOAD16 = 0x52,
    OP_STORE16 = 0x53,
    OP_LOADS8 = 0x54,
    OP_LOADS16 = 0x55,
    OP_RJG = 0x56,
    OP_RJGE = 0x57,
    OP_RJL = 0x58,
    OP_RJLE = 0x59,
    OP_RJC = 0x5A,
    OP_RJNC = 0x5B,
    OP_INTI = 0x5C,
    OP_LOADX = 0x5D,
    OP_LOADX16 = 0x5E,
    OP_STOREX = 0x5F,
    OP_STOREX16 = 0x60,
};

void vm_dump(const VM *vm, int mem_preview);

static inline uint64_t host_unix_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}
static inline uint64_t host_monotonic_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

#endif
