
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
void vm_error(const char *fmt, ...);
static inline uint64_t INST(uint8_t op, uint8_t rd, uint8_t rs1, uint8_t rs2, uint32_t imm) {
    return ((uint64_t)op << 56 | (uint64_t)rd << 48 | (uint64_t)rs1 << 40 | (uint64_t)rs2 << 32) |
        imm;
}
typedef struct VM VM;
typedef struct VCPU VCPU;
#ifdef VM_DEBUG
typedef struct VM_Debug VM_Debug;
#endif
#define MAX_MMIO_DEVICES 32
#define VM_MMIO_PAGE_SHIFT 12u
#define VM_MMIO_PAGE_COUNT (1u << (32u - VM_MMIO_PAGE_SHIFT))
#define VM_MMIO_PAGE_MAP_BYTES (VM_MMIO_PAGE_COUNT / 8u)

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
#define CALL_STACK_SIZE 2048
#define DATA_STACK_SIZE 2048
#define ISR_STACK_SIZE 2048
#define VM_TASK_C_STACK_BYTES 4096u
#define VM_STACK_POOL_SLOTS 64u

#define TIME_REALTIME_OFFSET   0
#define TIME_MONOTONIC_OFFSET  8
#define TIME_BOOTTIME_OFFSET   16

#define IVT_BASE 0x0000
#define CALL_STACK_BASE 0x00008000u
#define DATA_STACK_BASE (CALL_STACK_BASE + CALL_STACK_SIZE * 8)
#define ISR_STACK_BASE (DATA_STACK_BASE + DATA_STACK_SIZE * 8)
#define TIME_BASE (ISR_STACK_BASE + ISR_STACK_SIZE * 8)
#define PROGRAM_BASE (TIME_BASE + 28)

#define FB_BASE(addr_space_size) (addr_space_size)
#define FB_LEGACY_BASE 0x00620000u
#define SYSINFO_BASE (FB_LEGACY_BASE + FB_SIZE)
#define SYSINFO_MAGIC 0x31494D56u /* "VMI1" */
#define SYSINFO_LAYOUT_VERSION 3u
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
#define SYSINFO_FEATURE_ETHER (1u << 8)
#define SYSINFO_FEATURE_PCIE (1u << 9)
#define SYSINFO_FEATURE_RUNTIME_STATS (1u << 10)
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
#define SYSINFO_REG_CPU_FREQ_HZ_LO 0x5Cu
#define SYSINFO_REG_CPU_FREQ_HZ_HI 0x60u
#define SYSINFO_REG_CPU_CYCLES_LO 0x64u
#define SYSINFO_REG_CPU_CYCLES_HI 0x68u
#define SYSINFO_REG_EXEC_COUNT_LO 0x6Cu
#define SYSINFO_REG_EXEC_COUNT_HI 0x70u
#define SYSINFO_REG_EXEC_RATE_HZ_LO 0x74u
#define SYSINFO_REG_EXEC_RATE_HZ_HI 0x78u
#define SYSINFO_REG_UPTIME_NS_LO 0x7Cu
#define SYSINFO_REG_UPTIME_NS_HI 0x80u
#define SYSINFO_REG_HOST_RSS_BYTES_LO 0x84u
#define SYSINFO_REG_HOST_RSS_BYTES_HI 0x88u
#define SYSINFO_REG_RUNTIME_VERSION 0x8Cu
#define SYSINFO_RUNTIME_VERSION 1u
#define SYSINFO_SIZE 0x90u

#define VM_DEFAULT_CPU_MHZ 100u
#define VM_MAX_CPU_MHZ 10000u

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
#define IOMMU_REG_ROOT_LO 0x038u
#define IOMMU_REG_ROOT_HI 0x03Cu
#define IOMMU_MMIO_SIZE 0x100u

#define IOMMU_CTRL_ENABLE 0x01u
#define IOMMU_DEV_CTRL_ENABLE 0x01u
#define IOMMU_DEV_CTRL_PAGED 0x02u

#define IOMMU_PTE_P 0x00000001u
#define IOMMU_PTE_R 0x00000002u
#define IOMMU_PTE_W 0x00000004u

#define IOMMU_DMA_READ 0x01u
#define IOMMU_DMA_WRITE 0x02u

#define IOMMU_FAULT_VALID 0x01u
#define IOMMU_FAULT_REASON_SHIFT 4u
#define IOMMU_FAULT_REASON_DEV_INVALID 1u
#define IOMMU_FAULT_REASON_UNMAPPED 2u
#define IOMMU_FAULT_REASON_BOUNDS 3u
#define IOMMU_FAULT_REASON_PA_RANGE 4u
#define IOMMU_FAULT_REASON_PTABLE_OOB 5u
#define IOMMU_FAULT_REASON_BAD_ROOT 6u
#define IOMMU_FAULT_REASON_NONCONTIG 7u
#define IOMMU_FAULT_REASON_PERM 8u

#define IOMMU_MAX_DEVICES 4u
#define IOMMU_DEV_DISK 0u
#define IOMMU_DEV_ETHER 1u
#define IOMMU_DEV_AUDIO 2u

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

/*
 * PCIe ECAM (Enhanced Configuration Access Mechanism) window.
 * Layout follows the PCI Express base spec: bus/device/function each get a
 * fixed 4KB configuration-space slice, addressed as:
 *   addr = PCIE_ECAM_BASE + ((bus << 20) | (dev << 15) | (func << 12)) + offset
 * Only bus 0 is populated for now (PCI_ECAM_BUS_COUNT = 1), which is enough
 * for a single flat hierarchy of endpoints hanging off the host bridge.
 *
 * Placement note: this window must avoid several fixed regions that are
 * *not* reflected in the low MMIO cluster (0x0074C000..0x00750100):
 *   - 0x00000000..~0x00430000: BIOS/kernel image, stacks, ELF load buffers.
 *   - ~0x00750000..0x00800000: headroom the native C stack (rooted at
 *     0x00800000, growing down -- see bios/bios.c "_start": `movi r30,
 *     8388608`) actively uses across BIOS and early kernel execution.
 *   - 0x01000000..0x01200000: kernel vfork snapshot scratch
 *     (kernel/src/sched_task.c SCHED_VFORK_SNAPSHOT_BASE).
 *   - 0x02000000..0x03000000: guest userspace process region
 *     (kernel/include/kernel/platform.h USER_REGION_BASE/SIZE).
 *   - 0x03B80000..0x04000000: SMP per-task stack pool (top of RAM).
 * 0x00900000 sits in the gap above the native stack's high-water mark and
 * well below the vfork snapshot area, with >1MB of margin on both sides.
 */
#define PCIE_ECAM_BASE 0x00900000u
#define PCI_ECAM_BUS_COUNT 1u
#define PCI_ECAM_DEV_COUNT 32u
#define PCI_ECAM_FUNC_COUNT 8u
#define PCI_ECAM_FUNC_SIZE 4096u
#define PCIE_ECAM_SIZE (PCI_ECAM_BUS_COUNT * PCI_ECAM_DEV_COUNT * PCI_ECAM_FUNC_COUNT * PCI_ECAM_FUNC_SIZE)

#define VM_MMU_ACC_READ 0x01u
#define VM_MMU_ACC_WRITE 0x02u
#define VM_MMU_ACC_EXEC 0x04u
#define VM_MMU_ACC_USER 0x08u
#define VM_MMU_TLB_ENTRIES 64u
#define VM_DECODE_CACHE_ENTRIES 256u
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
    bool initialized;
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
        uint64_t root;
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

typedef struct {
    uint32_t vpn;
    uint32_t ppn;
    uint32_t root;
    uint32_t perms;
    uint8_t valid;
} VM_TlbEntry;

typedef struct {
    vm_addr_t ip;
    uint64_t inst;
    int32_t imm;
    uint8_t op;
    uint8_t rd;
    uint8_t rs1;
    uint8_t rs2;
    uint8_t valid;
} VM_DecodeCacheEntry;

typedef struct VmRuntimeStats {
    uint64_t cpu_frequency_hz;
    uint64_t virtual_cycles;
    uint64_t executed_instructions;
    uint64_t execution_rate_hz;
    uint64_t uptime_ns;
    uint64_t host_resident_bytes;
    uint64_t guest_ram_bytes;
    uint32_t core_count;
    uint32_t active_core_count;
} VmRuntimeStats;

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
    uint32_t active_interrupt_no;
    int irq_masked;
    int core_id;
    vm_addr_t call_stack_base;
    vm_addr_t data_stack_base;
    vm_addr_t isr_stack_base;
    int is_bsp;
    VM_TlbEntry tlb[VM_MMU_TLB_ENTRIES];
    VM_DecodeCacheEntry decode_cache[VM_DECODE_CACHE_ENTRIES];
};

extern _Thread_local VCPU *vm_tls_vcpu;

static inline VCPU *vm_current_cpu(VM *vm);

struct VM{
    _Atomic unsigned int stop_flags;
    uint8_t *memory;
    size_t memory_size;

    /*
     * framebuffer is mapped after main memory:
     * [fb_base, fb_base + FB_SIZE)
     */
    uint32_t *fb;
    uint32_t *fb_front;
    pthread_mutex_t fb_row_locks[FB_HEIGHT];
    atomic_uchar fb_row_dirty[FB_HEIGHT];

    int io[IO_SIZE];
    uint8_t serial_rx_fifo[256];
    uint16_t serial_rx_head;
    uint16_t serial_rx_tail;
    uint8_t serial_tx_fifo[8192];
    uint16_t serial_tx_head;
    uint16_t serial_tx_tail;
    uint64_t serial_tx_dropped;
    int serial_window_enabled;
    uint8_t ps2_kbd_fifo[256];
    uint16_t ps2_kbd_head;
    uint16_t ps2_kbd_tail;
    uint8_t ps2_mouse_fifo[256];
    uint16_t ps2_mouse_head;
    uint16_t ps2_mouse_tail;
    uint8_t ps2_out_fifo[512];
    uint8_t ps2_out_aux[512];
    uint8_t ps2_out_irq[512];
    uint16_t ps2_out_head;
    uint16_t ps2_out_tail;
    uint8_t ps2_config;
    uint8_t ps2_status;
    uint8_t ps2_pending_controller_write;
    uint8_t ps2_next_to_mouse;
    uint8_t ps2_kbd_enabled;
    uint8_t ps2_mouse_enabled;
    uint8_t ps2_kbd_scanning;
    uint8_t ps2_mouse_reporting;
    uint8_t ps2_kbd_expect;
    uint8_t ps2_mouse_expect;
    uint8_t ps2_mouse_sample_rate;
    uint8_t ps2_mouse_resolution;
    uint8_t ps2_mouse_scaling_2_1;

    Disk disk;
    IOMMU iommu;
    MMU mmu;
    atomic_uint_fast64_t *interrupt_bitmap;
    atomic_uint_fast64_t *interrupt_enable_bitmap;
    atomic_uint_fast32_t *interrupt_pending_summary;
    atomic_uchar interrupt_priority[IVT_SIZE];

    uint64_t start_realtime_ns;
    uint64_t start_monotonic_ns;
    uint64_t latched_realtime;
    uint64_t latched_monotonic;
    uint64_t latched_boottime;
    uint64_t disk_size_bytes;
    uint32_t sysinfo_vendor_words[SYSINFO_VENDOR_WORDS];
    uint64_t cpu_frequency_hz;
    pthread_mutex_t runtime_stats_lock;
    uint64_t runtime_stats_last_sample_ns;
    uint64_t runtime_stats_last_instructions;
    VmRuntimeStats runtime_stats_cached;
    VmRuntimeStats sysinfo_stats_latch;

    int suspend_count;

    MMIO_Device *mmio_devices[MAX_MMIO_DEVICES];
    int mmio_count;
    uint8_t mmio_page_map[VM_MMIO_PAGE_MAP_BYTES];
    uint8_t mmio_page_map_ready;
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

    pthread_t vnc_server_thread;

    /* Ethernet NIC state (opaque, managed by io_devices/ether/ether.c) */
    void *ether;

    /* PCI dumb-display state (opaque, managed by io_devices/gpu/gpu.c) */
    void *gpu;

    /* PCI PCM-audio state (opaque, managed by io_devices/audio/audio.c) */
    void *audio;

    /* PCIe root complex state (opaque, managed by io_devices/pcie/pcie.c) */
    void *pcie;

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

static inline void vm_fb_mark_row_dirty(VM *vm, size_t row) {
    if (vm && row < FB_HEIGHT) {
        atomic_store_explicit(&vm->fb_row_dirty[row], 1u, memory_order_release);
    }
}

static inline int vm_fb_take_row_dirty(VM *vm, size_t row) {
    if (!vm || row >= FB_HEIGHT) {
        return 0;
    }
    return atomic_exchange_explicit(&vm->fb_row_dirty[row], 0u, memory_order_acq_rel) != 0u;
}

static inline void vm_fb_mark_all_dirty(VM *vm) {
    if (!vm) {
        return;
    }
    for (size_t row = 0; row < FB_HEIGHT; row++) {
        vm_fb_mark_row_dirty(vm, row);
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

static inline int atomic_is_vm_halted(const VM *vm) {
    return (atomic_load_explicit(&vm->stop_flags, memory_order_acquire) & 1u) != 0u;
}

static inline int atomic_is_vm_panicked(const VM *vm) {
    return (atomic_load_explicit(&vm->stop_flags, memory_order_acquire) & 2u) != 0u;
}

static inline int atomic_is_vm_stopped(const VM *vm) {
    return atomic_load_explicit(&vm->stop_flags, memory_order_acquire) != 0u;
}

static inline void atomic_set_vm_halt(VM *vm, int value) {
    if (value) {
        atomic_fetch_or_explicit(&vm->stop_flags, 1u, memory_order_release);
    } else {
        atomic_fetch_and_explicit(&vm->stop_flags, ~1u, memory_order_release);
    }
}

static inline void atomic_set_vm_panic(VM *vm, int value) {
    if (value) {
        atomic_fetch_or_explicit(&vm->stop_flags, 2u, memory_order_release);
    } else {
        atomic_fetch_and_explicit(&vm->stop_flags, ~2u, memory_order_release);
    }
}

#endif
