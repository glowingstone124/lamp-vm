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
            kputc(c);
            printed = 1;
        }
    }
    if (!printed) {
        kputs("unknown");
    }
}

static void vm_info_log_features(uint32_t features) {
    if (!klog_should_emit(KLOG_LEVEL_INFO)) {
        return;
    }
    int first = 1;
    klog_prefix(KLOG_LEVEL_INFO, VM_INFO_TAG);
    kputs("features=");
    kprint_hex32(features);
    kputs(" [");
    if (features & BOOTINFO_FEATURE_TIME_MMIO) {
        if (!first) kputc((uint32_t)' ');
        kputs("TIME");
        first = 0;
    }
    if (features & BOOTINFO_FEATURE_FB_MMIO) {
        if (!first) kputc((uint32_t)' ');
        kputs("FB");
        first = 0;
    }
    if (features & BOOTINFO_FEATURE_DISK_IO) {
        if (!first) kputc((uint32_t)' ');
        kputs("DISK");
        first = 0;
    }
    if (features & BOOTINFO_FEATURE_SMP) {
        if (!first) kputc((uint32_t)' ');
        kputs("SMP");
        first = 0;
    }
    if (features & BOOTINFO_FEATURE_TIMER_IRQ) {
        if (!first) kputc((uint32_t)' ');
        kputs("TIMER_IRQ");
        first = 0;
    }
    if (features & BOOTINFO_FEATURE_INTC_MMIO) {
        if (!first) kputc((uint32_t)' ');
        kputs("INTC_MMIO");
        first = 0;
    }
    if (first) {
        kputs("none");
    }
    kputs("]\n");
}

void vm_info_log_boot(void) {
    boot_info_t info;
    if (!vm_info_load_boot(&info)) {
        KLOGW(VM_INFO_TAG, "bootinfo missing");
        return;
    }

    if (klog_should_emit(KLOG_LEVEL_INFO)) {
        klog_prefix(KLOG_LEVEL_INFO, VM_INFO_TAG);
        kputs("vendor=");
        vm_info_print_vendor(info.vendor_words);
        kputs("\n");

        klog_prefix(KLOG_LEVEL_INFO, VM_INFO_TAG);
        kputs("mem_lo=");
        kprint_hex32(info.mem_bytes_lo);
        kputs(" mem_hi=");
        kprint_hex32(info.mem_bytes_hi);
        kputs(" disk_lo=");
        kprint_hex32(info.disk_bytes_lo);
        kputs(" disk_hi=");
        kprint_hex32(info.disk_bytes_hi);
        kputs(" smp=");
        kprint_hex32(info.smp_cores);
        kputs("\n");

        klog_prefix(KLOG_LEVEL_INFO, VM_INFO_TAG);
        kputs("layout=");
        kprint_hex32(info.layout_version);
        kputs(" arch=");
        kprint_hex32(info.arch_id);
        kputs(" endian=");
        kprint_hex32(info.endian);
        kputs(" paddr_bits=");
        kprint_hex32(info.phys_addr_bits);
        kputs(" page=");
        kprint_hex32(info.page_size);
        kputs(" timer_hz=");
        kprint_hex32(info.timer_freq_hz);
        kputs("\n");

        klog_prefix(KLOG_LEVEL_INFO, VM_INFO_TAG);
        kputs("fb_w=");
        kprint_hex32(info.fb_width);
        kputs(" fb_h=");
        kprint_hex32(info.fb_height);
        kputs(" fb_bpp=");
        kprint_hex32(info.fb_bpp);
        kputs(" fb_stride=");
        kprint_hex32(info.fb_stride_bytes);
        kputs(" boot_rt_lo=");
        kprint_hex32(info.boot_realtime_ns_lo);
        kputs(" boot_rt_hi=");
        kprint_hex32(info.boot_realtime_ns_hi);
        kputs("\n");

        vm_info_log_features(info.features);
    }

    if (info.layout_version != SYSINFO_LAYOUT_VERSION) {
        KLOGW(VM_INFO_TAG, "sysinfo layout mismatch");
    }
    if (info.arch_id != BOOTINFO_ARCH_LAMP32 || info.endian != BOOTINFO_ENDIAN_LITTLE) {
        KLOGW(VM_INFO_TAG, "arch/endian mismatch");
    }
    if (info.mem_bytes_hi != 0u || info.mem_bytes_lo != KERNEL_MEM_SIZE) {
        klog_prefix(KLOG_LEVEL_WARN, VM_INFO_TAG);
        kputs("mem_size contract mismatch expect=");
        kprint_hex32(KERNEL_MEM_SIZE);
        kputs(" got_lo=");
        kprint_hex32(info.mem_bytes_lo);
        kputs(" got_hi=");
        kprint_hex32(info.mem_bytes_hi);
        kputs("\n");
    }
}
