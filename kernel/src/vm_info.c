#include "../include/kernel/platform.h"
#include "../include/kernel/printk.h"
#include "../include/kernel/vm_info.h"

#define VM_INFO_TAG "vm_info"

static inline uint32_t vm_read32(uint32_t addr) {
    return *(volatile uint32_t *)(uintptr_t)addr;
}

int vm_info_load_boot(boot_info_t *out) {
    if (!out) {
        return 0;
    }

    out->magic = vm_read32(BOOTINFO_ADDR + 0x00u);
    out->version = vm_read32(BOOTINFO_ADDR + 0x04u);
    out->size = vm_read32(BOOTINFO_ADDR + 0x08u);
    if (out->magic != BOOTINFO_MAGIC || out->version != BOOTINFO_VERSION || out->size < BOOTINFO_SIZE) {
        return 0;
    }

    for (uint32_t i = 0; i < 4u; i++) {
        out->vendor_words[i] = vm_read32(BOOTINFO_ADDR + 0x0Cu + i * 4u);
    }
    out->mem_bytes_lo = vm_read32(BOOTINFO_ADDR + 0x1Cu);
    out->mem_bytes_hi = vm_read32(BOOTINFO_ADDR + 0x20u);
    out->disk_bytes_lo = vm_read32(BOOTINFO_ADDR + 0x24u);
    out->disk_bytes_hi = vm_read32(BOOTINFO_ADDR + 0x28u);
    out->smp_cores = vm_read32(BOOTINFO_ADDR + 0x2Cu);
    out->layout_version = vm_read32(BOOTINFO_ADDR + 0x30u);
    out->arch_id = vm_read32(BOOTINFO_ADDR + 0x34u);
    out->endian = vm_read32(BOOTINFO_ADDR + 0x38u);
    out->phys_addr_bits = vm_read32(BOOTINFO_ADDR + 0x3Cu);
    out->page_size = vm_read32(BOOTINFO_ADDR + 0x40u);
    out->timer_freq_hz = vm_read32(BOOTINFO_ADDR + 0x44u);
    out->features = vm_read32(BOOTINFO_ADDR + 0x48u);
    out->fb_width = vm_read32(BOOTINFO_ADDR + 0x4Cu);
    out->fb_height = vm_read32(BOOTINFO_ADDR + 0x50u);
    out->fb_bpp = vm_read32(BOOTINFO_ADDR + 0x54u);
    out->fb_stride_bytes = vm_read32(BOOTINFO_ADDR + 0x58u);
    out->boot_realtime_ns_lo = vm_read32(BOOTINFO_ADDR + 0x5Cu);
    out->boot_realtime_ns_hi = vm_read32(BOOTINFO_ADDR + 0x60u);
    return 1;
}

static void vm_info_print_vendor(const uint32_t vendor_words[4]) {
    int printed = 0;
    for (uint32_t wi = 0; wi < 4u; wi++) {
        uint32_t w = vendor_words[wi];
        for (uint32_t bi = 0; bi < 4u; bi++) {
            uint32_t c = (w >> (bi * 8u)) & 0xFFu;
            if (c == 0u) {
                return;
            }
            klog_putc(c);
            printed = 1;
        }
    }
    if (!printed) {
        klog_puts("unknown");
    }
}

static void vm_info_log_features(uint32_t features) {
    if (!klog_should_emit(KLOG_LEVEL_INFO)) {
        return;
    }
    int first = 1;
    klog_begin(KLOG_LEVEL_INFO, VM_INFO_TAG);
    klog_puts("features=");
    klog_hex32(features);
    klog_puts(" [");
    if (features & BOOTINFO_FEATURE_TIME_MMIO) {
        if (!first) klog_putc((uint32_t)' ');
        klog_puts("TIME");
        first = 0;
    }
    if (features & BOOTINFO_FEATURE_FB_MMIO) {
        if (!first) klog_putc((uint32_t)' ');
        klog_puts("FB");
        first = 0;
    }
    if (features & BOOTINFO_FEATURE_DISK_IO) {
        if (!first) klog_putc((uint32_t)' ');
        klog_puts("DISK");
        first = 0;
    }
    if (features & BOOTINFO_FEATURE_SMP) {
        if (!first) klog_putc((uint32_t)' ');
        klog_puts("SMP");
        first = 0;
    }
    if (features & BOOTINFO_FEATURE_TIMER_IRQ) {
        if (!first) klog_putc((uint32_t)' ');
        klog_puts("TIMER_IRQ");
        first = 0;
    }
    if (features & BOOTINFO_FEATURE_INTC_MMIO) {
        if (!first) klog_putc((uint32_t)' ');
        klog_puts("INTC_MMIO");
        first = 0;
    }
    if (features & BOOTINFO_FEATURE_IOMMU_MMIO) {
        if (!first) klog_putc((uint32_t)' ');
        klog_puts("IOMMU_MMIO");
        first = 0;
    }
    if (features & BOOTINFO_FEATURE_MMU_PAGING) {
        if (!first) klog_putc((uint32_t)' ');
        klog_puts("MMU_PAGING");
        first = 0;
    }
    if (first) {
        klog_puts("none");
    }
    klog_puts("]");
    klog_end();
}

void vm_info_log_boot(void) {
    boot_info_t info;
    if (!vm_info_load_boot(&info)) {
        KLOGW(VM_INFO_TAG, "bootinfo missing");
        return;
    }

    if (klog_should_emit(KLOG_LEVEL_INFO)) {
        klog_begin(KLOG_LEVEL_INFO, VM_INFO_TAG);
        klog_puts("vendor=");
        vm_info_print_vendor(info.vendor_words);
        klog_end();

        klog_begin(KLOG_LEVEL_INFO, VM_INFO_TAG);
        klog_puts("mem_lo=");
        klog_hex32(info.mem_bytes_lo);
        klog_puts(" mem_hi=");
        klog_hex32(info.mem_bytes_hi);
        klog_puts(" disk_lo=");
        klog_hex32(info.disk_bytes_lo);
        klog_puts(" disk_hi=");
        klog_hex32(info.disk_bytes_hi);
        klog_puts(" smp=");
        klog_hex32(info.smp_cores);
        klog_end();

        klog_begin(KLOG_LEVEL_INFO, VM_INFO_TAG);
        klog_puts("layout=");
        klog_hex32(info.layout_version);
        klog_puts(" arch=");
        klog_hex32(info.arch_id);
        klog_puts(" endian=");
        klog_hex32(info.endian);
        klog_puts(" paddr_bits=");
        klog_hex32(info.phys_addr_bits);
        klog_puts(" page=");
        klog_hex32(info.page_size);
        klog_puts(" timer_hz=");
        klog_hex32(info.timer_freq_hz);
        klog_end();

        klog_begin(KLOG_LEVEL_INFO, VM_INFO_TAG);
        klog_puts("fb_w=");
        klog_hex32(info.fb_width);
        klog_puts(" fb_h=");
        klog_hex32(info.fb_height);
        klog_puts(" fb_bpp=");
        klog_hex32(info.fb_bpp);
        klog_puts(" fb_stride=");
        klog_hex32(info.fb_stride_bytes);
        klog_puts(" boot_rt_lo=");
        klog_hex32(info.boot_realtime_ns_lo);
        klog_puts(" boot_rt_hi=");
        klog_hex32(info.boot_realtime_ns_hi);
        klog_end();

        vm_info_log_features(info.features);
    }

    if (info.layout_version != SYSINFO_LAYOUT_VERSION) {
        KLOGW(VM_INFO_TAG, "sysinfo layout mismatch");
    }
    if (info.arch_id != BOOTINFO_ARCH_LAMP32 || info.endian != BOOTINFO_ENDIAN_LITTLE) {
        KLOGW(VM_INFO_TAG, "arch/endian mismatch");
    }
    if (info.mem_bytes_hi != 0u || info.mem_bytes_lo != KERNEL_MEM_SIZE) {
        klog_begin(KLOG_LEVEL_WARN, VM_INFO_TAG);
        klog_puts("mem_size contract mismatch expect=");
        klog_hex32(KERNEL_MEM_SIZE);
        klog_puts(" got_lo=");
        klog_hex32(info.mem_bytes_lo);
        klog_puts(" got_hi=");
        klog_hex32(info.mem_bytes_hi);
        klog_end();
    }
}
