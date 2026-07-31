#include "sysinfo_mmio_register.h"
#include "../../runtime_log.h"
#include "../../runtime_stats.h"

#include <stdio.h>
#include <string.h>

static void sysinfo_init_vendor(VM *vm) {
    static const char vendor[] = "LampVM";
    memset(vm->sysinfo_vendor_words, 0, sizeof(vm->sysinfo_vendor_words));
    for (size_t i = 0; i < sizeof(vendor) - 1; i++) {
        size_t word = i / 4u;
        size_t shift = (i % 4u) * 8u;
        if (word < SYSINFO_VENDOR_WORDS) {
            vm->sysinfo_vendor_words[word] |= ((uint32_t)(uint8_t)vendor[i]) << shift;
        }
    }
}

static uint32_t sysinfo_feature_bits(const VM *vm) {
    uint32_t bits = SYSINFO_FEATURE_TIME_MMIO |
                    SYSINFO_FEATURE_FB_MMIO |
                    SYSINFO_FEATURE_DISK_IO |
                    SYSINFO_FEATURE_TIMER_IRQ |
                    SYSINFO_FEATURE_INTC_MMIO |
                    SYSINFO_FEATURE_IOMMU_MMIO |
                    SYSINFO_FEATURE_MMU_PAGING |
                    SYSINFO_FEATURE_RUNTIME_STATS;
    if (vm->smp_cores > 1) {
        bits |= SYSINFO_FEATURE_SMP;
    }
    if (vm->ether) {
        bits |= SYSINFO_FEATURE_ETHER;
    }
    if (vm->pcie) {
        bits |= SYSINFO_FEATURE_PCIE;
    }
    return bits;
}

static uint32_t sysinfo_read32(VM *vm, uint32_t addr) {
    const uint32_t offset = addr - SYSINFO_BASE;
    uint64_t value64;
    if (offset == SYSINFO_REG_MAGIC) {
        return SYSINFO_MAGIC;
    }
    if (offset >= SYSINFO_REG_VENDOR0 &&
        offset < SYSINFO_REG_VENDOR0 + SYSINFO_VENDOR_BYTES &&
        (offset & 0x3u) == 0u) {
        const uint32_t index = (offset - SYSINFO_REG_VENDOR0) / 4u;
        return vm->sysinfo_vendor_words[index];
    }
    if (offset == SYSINFO_REG_MEM_BYTES_LO) {
        return (uint32_t)(vm->memory_size & 0xFFFFFFFFu);
    }
    if (offset == SYSINFO_REG_MEM_BYTES_HI) {
        return (uint32_t)(((uint64_t)vm->memory_size >> 32) & 0xFFFFFFFFu);
    }
    if (offset == SYSINFO_REG_DISK_BYTES_LO) {
        return (uint32_t)(vm->disk_size_bytes & 0xFFFFFFFFu);
    }
    if (offset == SYSINFO_REG_DISK_BYTES_HI) {
        return (uint32_t)((vm->disk_size_bytes >> 32) & 0xFFFFFFFFu);
    }
    if (offset == SYSINFO_REG_SMP_CORES) {
        return (uint32_t)vm->smp_cores;
    }
    if (offset == SYSINFO_REG_LAYOUT_VERSION) {
        return SYSINFO_LAYOUT_VERSION;
    }
    if (offset == SYSINFO_REG_ARCH_ID) {
        return SYSINFO_ARCH_LAMP32;
    }
    if (offset == SYSINFO_REG_ENDIAN) {
        return SYSINFO_ENDIAN_LITTLE;
    }
    if (offset == SYSINFO_REG_PHYS_ADDR_BITS) {
        return SYSINFO_PHYS_ADDR_BITS_32;
    }
    if (offset == SYSINFO_REG_PAGE_SIZE) {
        return SYSINFO_DEFAULT_PAGE_SIZE;
    }
    if (offset == SYSINFO_REG_TIMER_FREQ_HZ) {
        return SYSINFO_TIMER_FREQ_1GHZ;
    }
    if (offset == SYSINFO_REG_FEATURES) {
        return sysinfo_feature_bits(vm);
    }
    if (offset == SYSINFO_REG_FB_WIDTH) {
        return FB_WIDTH;
    }
    if (offset == SYSINFO_REG_FB_HEIGHT) {
        return FB_HEIGHT;
    }
    if (offset == SYSINFO_REG_FB_BPP) {
        return FB_BPP;
    }
    if (offset == SYSINFO_REG_FB_STRIDE_BYTES) {
        return FB_WIDTH * FB_BPP;
    }
    if (offset == SYSINFO_REG_BOOT_REALTIME_NS_LO) {
        return (uint32_t)(vm->start_realtime_ns & 0xFFFFFFFFu);
    }
    if (offset == SYSINFO_REG_BOOT_REALTIME_NS_HI) {
        return (uint32_t)((vm->start_realtime_ns >> 32) & 0xFFFFFFFFu);
    }
    if (offset == SYSINFO_REG_CPU_FREQ_HZ_LO ||
        offset == SYSINFO_REG_CPU_CYCLES_LO ||
        offset == SYSINFO_REG_EXEC_COUNT_LO ||
        offset == SYSINFO_REG_EXEC_RATE_HZ_LO ||
        offset == SYSINFO_REG_UPTIME_NS_LO ||
        offset == SYSINFO_REG_HOST_RSS_BYTES_LO) {
        vm_runtime_stats_sample(vm, &vm->sysinfo_stats_latch);
    }
    switch (offset) {
        case SYSINFO_REG_CPU_FREQ_HZ_LO:
        case SYSINFO_REG_CPU_FREQ_HZ_HI:
            value64 = vm->sysinfo_stats_latch.cpu_frequency_hz;
            break;
        case SYSINFO_REG_CPU_CYCLES_LO:
        case SYSINFO_REG_CPU_CYCLES_HI:
            value64 = vm->sysinfo_stats_latch.virtual_cycles;
            break;
        case SYSINFO_REG_EXEC_COUNT_LO:
        case SYSINFO_REG_EXEC_COUNT_HI:
            value64 = vm->sysinfo_stats_latch.executed_instructions;
            break;
        case SYSINFO_REG_EXEC_RATE_HZ_LO:
        case SYSINFO_REG_EXEC_RATE_HZ_HI:
            value64 = vm->sysinfo_stats_latch.execution_rate_hz;
            break;
        case SYSINFO_REG_UPTIME_NS_LO:
        case SYSINFO_REG_UPTIME_NS_HI:
            value64 = vm->sysinfo_stats_latch.uptime_ns;
            break;
        case SYSINFO_REG_HOST_RSS_BYTES_LO:
        case SYSINFO_REG_HOST_RSS_BYTES_HI:
            value64 = vm->sysinfo_stats_latch.host_resident_bytes;
            break;
        case SYSINFO_REG_RUNTIME_VERSION:
            return SYSINFO_RUNTIME_VERSION;
        default:
            value64 = 0u;
            break;
    }
    if (offset >= SYSINFO_REG_CPU_FREQ_HZ_LO &&
        offset <= SYSINFO_REG_HOST_RSS_BYTES_HI) {
        return (offset & 4u) == 0u ? (uint32_t)(value64 >> 32) :
                                     (uint32_t)value64;
    }

    fprintf(stderr, "Unknown SYSINFO MMIO register offset: 0x%08x\n", offset);
    return 0;
}

static void sysinfo_write32(VM *vm, uint32_t addr, uint32_t value) {
    (void)value;
    fprintf(stderr, "Attempted write to read-only SYSINFO MMIO at 0x%08x\n", addr);
    atomic_set_vm_halt(vm, 1);;
}

void register_sysinfo_mmio(VM *vm) {
    static MMIO_Device sysinfo_dev;
    sysinfo_init_vendor(vm);
    sysinfo_dev.start = SYSINFO_BASE;
    sysinfo_dev.end = SYSINFO_BASE + SYSINFO_SIZE - 1u;
    sysinfo_dev.read32 = sysinfo_read32;
    sysinfo_dev.write32 = sysinfo_write32;

    if (vm->mmio_count < MAX_MMIO_DEVICES) {
        vm->mmio_devices[vm->mmio_count++] = &sysinfo_dev;
        VM_RUNTIME_LOG("Registered VM SysInfo to MMIO ID %d\n", vm->mmio_count);
    }
}
