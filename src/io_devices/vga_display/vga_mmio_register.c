//
// Created by Max Wang on 2026/1/18.
//

#include "vga_mmio_register.h"
#include "../../runtime_log.h"


static inline size_t fb_pixel_index(VM *vm, uint32_t addr) {
    const size_t fb_base = FB_BASE(vm->memory_size);
    if (addr >= fb_base && addr < fb_base + FB_SIZE) {
        return (addr - fb_base) / 4;
    }
    if (addr >= FB_LEGACY_BASE && addr < FB_LEGACY_BASE + FB_SIZE) {
        return (addr - FB_LEGACY_BASE) / 4;
    }
    return 0;
}

static uint32_t fb_read32(VM *vm, uint32_t addr) {
    size_t pixel_index = fb_pixel_index(vm, addr);
    size_t row = vm_fb_row_from_pixel_index(pixel_index);
    vm_fb_row_lock(vm, row);
    uint32_t value = vm->fb[pixel_index];
    vm_fb_row_unlock(vm, row);
    return value;
}

static void fb_write32(VM *vm, uint32_t addr, uint32_t value) {
    size_t pixel_index = fb_pixel_index(vm, addr);
    size_t row = vm_fb_row_from_pixel_index(pixel_index);
    vm_fb_row_lock(vm, row);
    vm->fb[pixel_index] = value;
    vm_fb_mark_row_dirty(vm, row);
    vm_fb_row_unlock(vm, row);
}

void register_fb_mmio(VM *vm) {
    static MMIO_Device fb_dev;
    static MMIO_Device fb_legacy_dev;
    fb_dev.start = FB_BASE(vm->memory_size);
    fb_dev.end = FB_BASE(vm->memory_size) + FB_SIZE - 1;
    fb_dev.read32 = fb_read32;
    fb_dev.write32 = fb_write32;
    vm->mmio_devices[vm->mmio_count++] = &fb_dev;
    VM_RUNTIME_LOG("Registered VM Screen to MMIO ID %d\n", vm->mmio_count);

    fb_legacy_dev.start = FB_LEGACY_BASE;
    fb_legacy_dev.end = FB_LEGACY_BASE + FB_SIZE - 1;
    fb_legacy_dev.read32 = fb_read32;
    fb_legacy_dev.write32 = fb_write32;
    vm->mmio_devices[vm->mmio_count++] = &fb_legacy_dev;
    VM_RUNTIME_LOG("Registered VM Screen legacy alias to MMIO ID %d\n", vm->mmio_count);
}
