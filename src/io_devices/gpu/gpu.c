#include "gpu.h"

#include <stdlib.h>
#include <string.h>

#include "../pcie/pcie.h"
#include "../../interrupt.h"
#include "../../../include/lampvm/device_abi.h"

typedef struct gpu_state {
    uint8_t *vram;
    uint8_t *firmware_backup;
    PciFunction *pci_function;
    uint32_t status;
    uint32_t scanout_offset;
    uint32_t pending_offset;
    uint32_t damage_x;
    uint32_t damage_y;
    uint32_t damage_w;
    uint32_t damage_h;
    uint32_t submit_seq;
    uint32_t complete_seq;
    uint32_t irq_status;
    uint32_t irq_enable;
    uint32_t cursor_pending_x;
    uint32_t cursor_pending_y;
    uint32_t cursor_pending_ctrl;
    uint32_t cursor_x;
    uint32_t cursor_y;
    uint32_t cursor_ctrl;
    uint8_t backup_valid;
} gpu_state_t;

static const char *const g_gpu_cursor_shape[LAMP_GPU_CURSOR_HEIGHT] = {
    "X...........",
    "XX..........",
    "XOX.........",
    "XOOX........",
    "XOOOX.......",
    "XOOOOX......",
    "XOOOOOX.....",
    "XOOOOOOX....",
    "XOOOOOOOX...",
    "XOOOOOOOOX..",
    "XOOOOXXXXX..",
    "XOOXOX......",
    "XOX.XOX.....",
    "XX..XOX.....",
    "X....XOX....",
    ".....XOX....",
    ".....XOX....",
    ".....XXX...."
};

static uint32_t gpu_load_le32(const uint8_t *p) {
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void gpu_store_le32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static uint32_t gpu_cursor_shape_pixel(uint32_t x, uint32_t y) {
    const char pixel = g_gpu_cursor_shape[y][x];
    if (pixel == 'X') return 1u;
    if (pixel == 'O') return 2u;
    return 0u;
}

static uint32_t gpu_scanout_pixel(gpu_state_t *gpu, uint32_t offset,
                                  uint32_t x, uint32_t y) {
    const size_t pixel = (size_t)y * FB_WIDTH + x;
    return gpu_load_le32(&gpu->vram[offset + pixel * FB_BPP]);
}

/* Caller holds every framebuffer row touched by the cursor. */
static void gpu_cursor_restore_unlocked(VM *vm, gpu_state_t *gpu,
                                        uint32_t scanout_offset,
                                        uint32_t cursor_x, uint32_t cursor_y,
                                        uint32_t cursor_ctrl) {
    if ((cursor_ctrl & LAMP_GPU_CURSOR_VISIBLE) == 0u) {
        return;
    }
    for (uint32_t y = 0u; y < LAMP_GPU_CURSOR_HEIGHT; y++) {
        const uint32_t py = cursor_y + y;
        if (py >= FB_HEIGHT) break;
        for (uint32_t x = 0u; x < LAMP_GPU_CURSOR_WIDTH; x++) {
            const uint32_t px = cursor_x + x;
            if (px >= FB_WIDTH) break;
            if (gpu_cursor_shape_pixel(x, y) == 0u) continue;
            vm->fb[(size_t)py * FB_WIDTH + px] =
                gpu_scanout_pixel(gpu, scanout_offset, px, py);
        }
    }
}

/* Caller holds every framebuffer row touched by the cursor. */
static void gpu_cursor_draw_unlocked(VM *vm,
                                     uint32_t cursor_x, uint32_t cursor_y,
                                     uint32_t cursor_ctrl) {
    const uint32_t fill =
        ((cursor_ctrl & LAMP_GPU_CURSOR_BUTTONS_MASK) >>
         LAMP_GPU_CURSOR_BUTTONS_SHIFT) & 1u
            ? 0x0038BDF8u : 0x00F8FAFCu;
    if ((cursor_ctrl & LAMP_GPU_CURSOR_VISIBLE) == 0u) {
        return;
    }
    for (uint32_t y = 0u; y < LAMP_GPU_CURSOR_HEIGHT; y++) {
        const uint32_t py = cursor_y + y;
        if (py >= FB_HEIGHT) break;
        for (uint32_t x = 0u; x < LAMP_GPU_CURSOR_WIDTH; x++) {
            const uint32_t px = cursor_x + x;
            const uint32_t shape = gpu_cursor_shape_pixel(x, y);
            if (px >= FB_WIDTH) break;
            if (shape == 0u) continue;
            vm->fb[(size_t)py * FB_WIDTH + px] =
                shape == 1u ? 0x00030A10u : fill;
        }
    }
}

static void gpu_cursor_redraw(VM *vm, gpu_state_t *gpu) {
    uint32_t y0;
    uint32_t y1;
    if ((gpu->cursor_ctrl & LAMP_GPU_CURSOR_VISIBLE) == 0u ||
        gpu->cursor_y >= FB_HEIGHT) {
        return;
    }
    y0 = gpu->cursor_y;
    y1 = y0 + LAMP_GPU_CURSOR_HEIGHT;
    if (y1 > FB_HEIGHT) y1 = FB_HEIGHT;
    for (uint32_t row = y0; row < y1; row++) vm_fb_row_lock(vm, row);
    gpu_cursor_draw_unlocked(vm, gpu->cursor_x, gpu->cursor_y,
                             gpu->cursor_ctrl);
    for (uint32_t row = y0; row < y1; row++) {
        vm_fb_mark_row_dirty(vm, row);
        vm_fb_row_unlock(vm, row);
    }
}

static void gpu_cursor_apply(VM *vm, gpu_state_t *gpu) {
    const uint32_t old_x = gpu->cursor_x;
    const uint32_t old_y = gpu->cursor_y;
    const uint32_t old_ctrl = gpu->cursor_ctrl;
    const uint32_t new_x = gpu->cursor_pending_x < FB_WIDTH ?
        gpu->cursor_pending_x : FB_WIDTH - 1u;
    const uint32_t new_y = gpu->cursor_pending_y < FB_HEIGHT ?
        gpu->cursor_pending_y : FB_HEIGHT - 1u;
    const uint32_t new_ctrl = gpu->cursor_pending_ctrl &
        (LAMP_GPU_CURSOR_VISIBLE | LAMP_GPU_CURSOR_BUTTONS_MASK);
    uint32_t y0 = FB_HEIGHT;
    uint32_t y1 = 0u;

    if ((old_ctrl & LAMP_GPU_CURSOR_VISIBLE) != 0u && old_y < FB_HEIGHT) {
        y0 = old_y;
        y1 = old_y + LAMP_GPU_CURSOR_HEIGHT;
        if (y1 > FB_HEIGHT) y1 = FB_HEIGHT;
    }
    if ((new_ctrl & LAMP_GPU_CURSOR_VISIBLE) != 0u && new_y < FB_HEIGHT) {
        uint32_t new_y1 = new_y + LAMP_GPU_CURSOR_HEIGHT;
        if (new_y < y0) y0 = new_y;
        if (new_y1 > FB_HEIGHT) new_y1 = FB_HEIGHT;
        if (new_y1 > y1) y1 = new_y1;
    }

    if ((gpu->status & LAMP_GPU_STATUS_ENABLED) != 0u && y0 < y1) {
        for (uint32_t row = y0; row < y1; row++) vm_fb_row_lock(vm, row);
        gpu_cursor_restore_unlocked(vm, gpu, gpu->scanout_offset,
                                    old_x, old_y, old_ctrl);
        gpu_cursor_draw_unlocked(vm, new_x, new_y, new_ctrl);
        for (uint32_t row = y0; row < y1; row++) {
            vm_fb_mark_row_dirty(vm, row);
            vm_fb_row_unlock(vm, row);
        }
    }
    gpu->cursor_x = new_x;
    gpu->cursor_y = new_y;
    gpu->cursor_ctrl = new_ctrl;
}

static int gpu_scanout_valid(uint32_t offset) {
    const uint32_t scanout_bytes = FB_WIDTH * FB_HEIGHT * FB_BPP;
    return (offset & 3u) == 0u && offset <= LAMP_GPU_VRAM_SIZE &&
           scanout_bytes <= LAMP_GPU_VRAM_SIZE - offset;
}

static void gpu_raise_irq(VM *vm, gpu_state_t *gpu, uint32_t bits) {
    gpu->irq_status |= bits;
    if ((gpu->irq_enable & bits) != 0u && gpu->pci_function) {
        pci_notify_irq(vm, gpu->pci_function);
    }
}

static void gpu_restore_firmware(VM *vm, gpu_state_t *gpu) {
    if (!gpu->backup_valid) {
        return;
    }
    for (size_t row = 0; row < FB_HEIGHT; row++) {
        vm_fb_row_lock(vm, row);
    }
    for (size_t row = 0; row < FB_HEIGHT; row++) {
        const size_t row_bytes = (size_t)FB_WIDTH * FB_BPP;
        memcpy((uint8_t *)vm->fb + row * row_bytes,
               gpu->firmware_backup + row * row_bytes, row_bytes);
        vm_fb_mark_row_dirty(vm, row);
    }
    for (size_t row = 0; row < FB_HEIGHT; row++) {
        vm_fb_row_unlock(vm, row);
    }
}

static void gpu_copy_scanout_row(VM *vm, gpu_state_t *gpu, uint32_t offset,
                                 uint32_t row, uint32_t x, uint32_t w) {
    const size_t src_pixel = (size_t)row * FB_WIDTH + x;
    const size_t dst_pixel = src_pixel;
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    for (uint32_t col = 0u; col < w; col++) {
        vm->fb[dst_pixel + col] = gpu_load_le32(
            &gpu->vram[offset + (src_pixel + col) * FB_BPP]);
    }
#else
    memcpy(&vm->fb[dst_pixel], &gpu->vram[offset + src_pixel * FB_BPP],
           (size_t)w * FB_BPP);
#endif
}

static int gpu_present(VM *vm, gpu_state_t *gpu, uint32_t offset, int full_frame) {
    uint32_t x = full_frame ? 0u : gpu->damage_x;
    uint32_t y = full_frame ? 0u : gpu->damage_y;
    uint32_t w = full_frame ? FB_WIDTH : gpu->damage_w;
    uint32_t h = full_frame ? FB_HEIGHT : gpu->damage_h;

    if (!gpu_scanout_valid(offset)) {
        gpu->status |= LAMP_GPU_STATUS_BAD_SCANOUT;
        gpu_raise_irq(vm, gpu, LAMP_GPU_IRQ_ERROR);
        return 0;
    }
    if (w == 0u || h == 0u) {
        x = 0u;
        y = 0u;
        w = FB_WIDTH;
        h = FB_HEIGHT;
    }
    if (x >= FB_WIDTH || y >= FB_HEIGHT || w > FB_WIDTH - x || h > FB_HEIGHT - y) {
        gpu->status |= LAMP_GPU_STATUS_BAD_COMMAND;
        gpu_raise_irq(vm, gpu, LAMP_GPU_IRQ_ERROR);
        return 0;
    }

    if (full_frame) {
        /* Lock ordering matches SDL/VNC row traversal. Holding the complete
         * set makes a page flip indivisible to all scanout consumers. */
        for (uint32_t row = 0u; row < FB_HEIGHT; row++) {
            vm_fb_row_lock(vm, row);
        }
        for (uint32_t row = 0u; row < FB_HEIGHT; row++) {
            gpu_copy_scanout_row(vm, gpu, offset, row, 0u, FB_WIDTH);
            vm_fb_mark_row_dirty(vm, row);
        }
        gpu_cursor_draw_unlocked(vm, gpu->cursor_x, gpu->cursor_y,
                                 gpu->cursor_ctrl);
        for (uint32_t row = 0u; row < FB_HEIGHT; row++) {
            vm_fb_row_unlock(vm, row);
        }
    } else {
        for (uint32_t row = y; row < y + h; row++) {
            vm_fb_row_lock(vm, row);
            gpu_copy_scanout_row(vm, gpu, offset, row, x, w);
            vm_fb_mark_row_dirty(vm, row);
            vm_fb_row_unlock(vm, row);
        }
        gpu_cursor_redraw(vm, gpu);
    }
    gpu->status &= ~(LAMP_GPU_STATUS_BAD_COMMAND | LAMP_GPU_STATUS_BAD_SCANOUT);
    return 1;
}

static void gpu_reset(VM *vm, gpu_state_t *gpu, int restore_firmware) {
    if (restore_firmware) {
        gpu_restore_firmware(vm, gpu);
    }
    gpu->status = LAMP_GPU_STATUS_READY;
    gpu->scanout_offset = 0u;
    gpu->pending_offset = 0u;
    gpu->damage_x = 0u;
    gpu->damage_y = 0u;
    gpu->damage_w = FB_WIDTH;
    gpu->damage_h = FB_HEIGHT;
    gpu->submit_seq = 0u;
    gpu->complete_seq = 0u;
    gpu->irq_status = 0u;
    gpu->irq_enable = 0u;
    gpu->cursor_pending_x = 0u;
    gpu->cursor_pending_y = 0u;
    gpu->cursor_pending_ctrl = 0u;
    gpu->cursor_x = 0u;
    gpu->cursor_y = 0u;
    gpu->cursor_ctrl = 0u;
}

static void gpu_command(VM *vm, gpu_state_t *gpu, uint32_t command) {
    if ((command & LAMP_GPU_CMD_RESET) != 0u) {
        gpu_reset(vm, gpu, 1);
        return;
    }
    if ((command & LAMP_GPU_CMD_DISABLE) != 0u) {
        gpu_restore_firmware(vm, gpu);
        gpu->status &= ~LAMP_GPU_STATUS_ENABLED;
    }
    if ((command & LAMP_GPU_CMD_ENABLE) != 0u) {
        if (!gpu->backup_valid) {
            for (size_t row = 0; row < FB_HEIGHT; row++) {
                const size_t row_bytes = (size_t)FB_WIDTH * FB_BPP;
                vm_fb_row_lock(vm, row);
                memcpy(gpu->firmware_backup + row * row_bytes,
                       (const uint8_t *)vm->fb + row * row_bytes, row_bytes);
                vm_fb_row_unlock(vm, row);
            }
            gpu->backup_valid = 1u;
        }
        gpu->status |= LAMP_GPU_STATUS_ENABLED;
    }
    if ((command & LAMP_GPU_CMD_PAGE_FLIP) != 0u) {
        gpu->submit_seq++;
        if (gpu_present(vm, gpu, gpu->pending_offset, 1)) {
            gpu->scanout_offset = gpu->pending_offset;
            gpu->complete_seq = gpu->submit_seq;
            gpu_raise_irq(vm, gpu, LAMP_GPU_IRQ_FLIP_COMPLETE);
        }
    } else if ((command & LAMP_GPU_CMD_FLUSH) != 0u) {
        gpu->submit_seq++;
        if (gpu_present(vm, gpu, gpu->scanout_offset, 0)) {
            gpu->complete_seq = gpu->submit_seq;
            gpu_raise_irq(vm, gpu, LAMP_GPU_IRQ_FLUSH_COMPLETE);
        }
    }
    if ((command & LAMP_GPU_CMD_CURSOR_UPDATE) != 0u) {
        gpu_cursor_apply(vm, gpu);
    }
}

static uint32_t gpu_reg_read(gpu_state_t *gpu, uint32_t offset) {
    switch (offset) {
        case LAMP_GPU_REG_MAGIC: return LAMP_GPU_MAGIC;
        case LAMP_GPU_REG_VERSION: return LAMP_GPU_VERSION;
        case LAMP_GPU_REG_CAPS:
            return LAMP_GPU_CAP_DAMAGE | LAMP_GPU_CAP_PAGE_FLIP |
                   LAMP_GPU_CAP_MSI | LAMP_GPU_CAP_SHADOW_SCANOUT |
                   LAMP_GPU_CAP_CURSOR;
        case LAMP_GPU_REG_STATUS: return gpu->status;
        case LAMP_GPU_REG_WIDTH: return FB_WIDTH;
        case LAMP_GPU_REG_HEIGHT: return FB_HEIGHT;
        case LAMP_GPU_REG_STRIDE: return FB_WIDTH * FB_BPP;
        case LAMP_GPU_REG_FORMAT: return LAMP_GPU_FORMAT_XRGB8888;
        case LAMP_GPU_REG_VRAM_SIZE: return LAMP_GPU_VRAM_SIZE;
        case LAMP_GPU_REG_SCANOUT_OFFSET: return gpu->scanout_offset;
        case LAMP_GPU_REG_PENDING_OFFSET: return gpu->pending_offset;
        case LAMP_GPU_REG_DAMAGE_X: return gpu->damage_x;
        case LAMP_GPU_REG_DAMAGE_Y: return gpu->damage_y;
        case LAMP_GPU_REG_DAMAGE_W: return gpu->damage_w;
        case LAMP_GPU_REG_DAMAGE_H: return gpu->damage_h;
        case LAMP_GPU_REG_SUBMIT_SEQ: return gpu->submit_seq;
        case LAMP_GPU_REG_COMPLETE_SEQ: return gpu->complete_seq;
        case LAMP_GPU_REG_IRQ_STATUS: return gpu->irq_status;
        case LAMP_GPU_REG_IRQ_ENABLE: return gpu->irq_enable;
        case LAMP_GPU_REG_CURSOR_X: return gpu->cursor_pending_x;
        case LAMP_GPU_REG_CURSOR_Y: return gpu->cursor_pending_y;
        case LAMP_GPU_REG_CURSOR_CTRL: return gpu->cursor_pending_ctrl;
        default: return 0u;
    }
}

static void gpu_reg_write(VM *vm, gpu_state_t *gpu, uint32_t offset, uint32_t value) {
    switch (offset) {
        case LAMP_GPU_REG_PENDING_OFFSET: gpu->pending_offset = value; break;
        case LAMP_GPU_REG_DAMAGE_X: gpu->damage_x = value; break;
        case LAMP_GPU_REG_DAMAGE_Y: gpu->damage_y = value; break;
        case LAMP_GPU_REG_DAMAGE_W: gpu->damage_w = value; break;
        case LAMP_GPU_REG_DAMAGE_H: gpu->damage_h = value; break;
        case LAMP_GPU_REG_CURSOR_X: gpu->cursor_pending_x = value; break;
        case LAMP_GPU_REG_CURSOR_Y: gpu->cursor_pending_y = value; break;
        case LAMP_GPU_REG_CURSOR_CTRL: gpu->cursor_pending_ctrl = value; break;
        case LAMP_GPU_REG_COMMAND: gpu_command(vm, gpu, value); break;
        case LAMP_GPU_REG_IRQ_ENABLE: gpu->irq_enable = value &
            (LAMP_GPU_IRQ_FLUSH_COMPLETE | LAMP_GPU_IRQ_FLIP_COMPLETE | LAMP_GPU_IRQ_ERROR); break;
        case LAMP_GPU_REG_IRQ_ACK:
            gpu->irq_status &= ~value;
            if (gpu->pci_function && gpu->irq_status == 0u) {
                gpu->pci_function->status &= (uint16_t)~PCI_STATUS_INTX;
            }
            break;
        default: break;
    }
}

static uint32_t gpu_bar_read32(VM *vm, PciFunction *function,
                               uint32_t bar_index, uint32_t offset) {
    gpu_state_t *gpu = (gpu_state_t *)function->cookie;
    (void)vm;
    if (!gpu) {
        return 0xFFFFFFFFu;
    }
    if (bar_index == 0u) {
        return gpu_reg_read(gpu, offset);
    }
    if (bar_index == 1u && offset <= LAMP_GPU_VRAM_SIZE - 4u) {
        return gpu_load_le32(&gpu->vram[offset]);
    }
    return 0u;
}

static void gpu_bar_write32(VM *vm, PciFunction *function,
                            uint32_t bar_index, uint32_t offset, uint32_t value) {
    gpu_state_t *gpu = (gpu_state_t *)function->cookie;
    if (!gpu) {
        return;
    }
    if (bar_index == 0u) {
        gpu_reg_write(vm, gpu, offset, value);
    } else if (bar_index == 1u && offset <= LAMP_GPU_VRAM_SIZE - 4u) {
        gpu_store_le32(&gpu->vram[offset], value);
    }
}

int gpu_device_init(VM *vm) {
    gpu_state_t *gpu;
    PciFunction *function;
    if (!vm || !vm->pcie || vm->gpu) {
        return -1;
    }
    gpu = calloc(1, sizeof(*gpu));
    if (!gpu) {
        return -1;
    }
    gpu->vram = calloc(1, LAMP_GPU_VRAM_SIZE);
    gpu->firmware_backup = malloc(FB_SIZE);
    if (!gpu->vram || !gpu->firmware_backup) {
        free(gpu->firmware_backup);
        free(gpu->vram);
        free(gpu);
        return -1;
    }
    function = pci_register_function(vm, 2u, 0u, LAMP_PCI_VENDOR_ID,
                                     LAMP_PCI_GPU_DEVICE_ID,
                                     PCI_CLASS_DISPLAY, PCI_SUBCLASS_VGA, 0u);
    if (!function) {
        free(gpu->firmware_backup);
        free(gpu->vram);
        free(gpu);
        return -1;
    }
    gpu->pci_function = function;
    gpu_reset(vm, gpu, 0);
    pci_configure_bar(vm, function, 0u, LAMP_GPU_BAR0_SIZE, 0u, 0u,
                      gpu_bar_read32, gpu_bar_write32, NULL, gpu);
    pci_configure_bar(vm, function, 1u, LAMP_GPU_VRAM_SIZE, 0u, 1u,
                      gpu_bar_read32, gpu_bar_write32, NULL, gpu);
    (void)pci_add_pm_capability(function);
    (void)pci_add_msi_capability(function);
    (void)pci_add_express_capability(function, 0u);
    pci_set_irq_pin(function, 1u, INT_GPU);
    vm->gpu = gpu;
    return 0;
}

void gpu_device_shutdown(VM *vm) {
    gpu_state_t *gpu;
    if (!vm) {
        return;
    }
    gpu = (gpu_state_t *)vm->gpu;
    if (!gpu) {
        return;
    }
    free(gpu->firmware_backup);
    free(gpu->vram);
    free(gpu);
    vm->gpu = NULL;
}
