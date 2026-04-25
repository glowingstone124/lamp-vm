#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define __USE_MISC
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <SDL2/SDL_timer.h>
#include <pthread.h>

#include "fetch.h"
#include "vm.h"
#include "stack.h"
#include "io.h"
#include "panic.h"
#include "loadbin.h"
#include "interrupt.h"
#include "memory.h"
#include "io_devices/disk/disk.h"
#include "io_devices/frame/frame.h"
#include "io_devices/sysinfo/sysinfo_mmio_register.h"
#include "io_devices/intc/intc_mmio_register.h"
#include "io_devices/iommu/iommu_mmio_register.h"
#include "io_devices/mmu/mmu_mmio_register.h"
#include "io_devices/time/time_mmio_register.h"
#include "io_devices/vga_display/display.h"
#include "io_devices/vga_display/vga_mmio_register.h"
#include "float.h"
#include "flags.h"
#include "debug.h"
#include "selftest.h"
#include "vnc.h"

const size_t MEM_SIZE = 1048576 * 64; // 64MB
enum { EXECUTION_TIMES_FLUSH_INTERVAL = 1024 };

typedef struct {
    VM *vm;
    int core_id;
} CpuThreadArg;

_Thread_local VCPU *vm_tls_vcpu = NULL;
volatile int g_vfork_trace_steps[32];
extern volatile int g_vfork_irq_probe_remaining[32];

static inline int vfork_irq_probe_active(const VCPU *cpu) {
    return cpu && cpu->core_id >= 0 && cpu->core_id < 32 && g_vfork_irq_probe_remaining[cpu->core_id] > 0;
}

static inline int vfork_irq_probe_addr(vm_addr_t addr) {
    return addr >= (vm_addr_t)0x00121f78u && addr < (vm_addr_t)0x001224d0u;
}

void update_zf_sf(VM *vm, int32_t result,VCPU *cpu) {
    if (!cpu)
        return;
    if (result == 0)
        cpu->flags |= FLAG_ZF;
    else
        cpu->flags &= ~FLAG_ZF;

    if (result < 0)
        cpu->flags |= FLAG_SF;
    else
        cpu->flags &= ~FLAG_SF;
}

void update_add_flags(VM *vm, int32_t a, int32_t b, int32_t result, VCPU *cpu) {
    if (!cpu)
        return;
    if ((uint32_t) a + (uint32_t) b < (uint32_t) a)
        cpu->flags |= FLAG_CF;
    else
        cpu->flags &= ~FLAG_CF;

    if (((a > 0 && b > 0 && result < 0) || (a < 0 && b < 0 && result > 0)))
        cpu->flags |= FLAG_OF;
    else
        cpu->flags &= ~FLAG_OF;

    update_zf_sf(vm, result,cpu);
}

void update_sub_flags(VM *vm, int32_t a, int32_t b, int32_t result,VCPU *cpu) {
    if (!cpu)
        return;
    if ((uint32_t) a < (uint32_t) b)
        cpu->flags |= FLAG_CF;
    else
        cpu->flags &= ~FLAG_CF;

    if (((a > 0 && b < 0 && result < 0) || (a < 0 && b > 0 && result > 0)))
        cpu->flags |= FLAG_OF;
    else
        cpu->flags &= ~FLAG_OF;

    update_zf_sf(vm, result, cpu);
}

static inline void clear_cf_of(VM *vm,VCPU *cpu) {
    if (!cpu)
        return;
    cpu->flags &= ~(FLAG_CF | FLAG_OF);
}

static inline void update_logic_flags(VM *vm, int32_t result,VCPU *cpu) {
    clear_cf_of(vm,cpu);
    update_zf_sf(vm, result,cpu);
}

static inline void set_cas_flags(VM *vm, int success,VCPU *cpu) {
    if (!cpu)
        return;
    cpu->flags &= ~(FLAG_ZF | FLAG_SF | FLAG_CF | FLAG_OF);
    if (success)
        cpu->flags |= FLAG_ZF;
}

static inline void ensure_atomic_aligned_or_panic(VM *vm, vm_addr_t addr, const char *op_name) {
    if ((addr & 0x3u) != 0) {
        panic(panic_format("%s unaligned address: 0x%08x", op_name, addr), vm);
    }
}

static inline void ensure_halfword_aligned_or_panic(VM *vm, vm_addr_t addr, const char *op_name) {
    if ((addr & 0x1u) != 0) {
        panic(panic_format("%s unaligned address: 0x%08x", op_name, addr), vm);
    }
}

static inline int vm_stack_region_valid(const VM *vm, vm_addr_t base, size_t bytes) {
    size_t start;
    if (!vm) {
        return 0;
    }
    start = (size_t)base;
    if (start > vm->memory_size) {
        return 0;
    }
    return bytes <= (vm->memory_size - start);
}

static inline void vm_irq_ack_input(VM *vm, uint32_t int_no) {
    if (!vm || !vm->interrupt_bitmap || int_no >= IVT_SIZE) {
        return;
    }
    {
        const size_t word_idx = ((size_t)int_no >> 6);
        const uint_fast64_t bit = (uint_fast64_t)(1ULL << (int_no & 63u));
        atomic_fetch_and(&vm->interrupt_bitmap[word_idx], ~bit);
    }
}

static inline vm_addr_t rel_target_from_last_ip(const VCPU *cpu, int32_t imm) {
    const int64_t base = (int64_t)(vm_addr_t)cpu->last_ip;
    const int64_t target = base + (int64_t)imm;
    return (vm_addr_t)target;
}

static inline uint32_t rotl32(uint32_t v, uint32_t sh) {
    sh &= 31u;
    if (sh == 0u) {
        return v;
    }
    return (v << sh) | (v >> (32u - sh));
}

static inline uint32_t rotr32(uint32_t v, uint32_t sh) {
    sh &= 31u;
    if (sh == 0u) {
        return v;
    }
    return (v >> sh) | (v << (32u - sh));
}

void vm_instruction_case(VM *vm) {
    VCPU *cpu = vm_current_cpu(vm);
    if (!cpu)
        return;
    uint8_t op, rd, rs1, rs2;
    int32_t imm;
    FETCH64(vm, op, rd, rs1, rs2, imm);
    if (cpu->core_id >= 0 && cpu->core_id < 32 && g_vfork_trace_steps[cpu->core_id] > 0) {
        g_vfork_trace_steps[cpu->core_id]--;
        fprintf(stderr,
                "[steptrace] core=%d ip=0x%08zx op=%u rd=%u rs1=%u rs2=%u imm=0x%08x call_base=0x%08x csp=0x%08x dsp=0x%08x isp=0x%08x in_interrupt=%d irq_masked=%d\n",
                cpu->core_id,
                cpu->last_ip,
                (unsigned)op,
                (unsigned)rd,
                (unsigned)rs1,
                (unsigned)rs2,
                (uint32_t)imm,
                (uint32_t)cpu->call_stack_base,
                (uint32_t)cpu->csp,
                (uint32_t)cpu->dsp,
                (uint32_t)cpu->isp,
                cpu->in_interrupt ? 1 : 0,
                cpu->irq_masked ? 1 : 0);
    }
    vm_debug_count_instruction(vm, op);
    //printf("IP=%lu, executing opcode=%d\n", cpu->ip, op);
    //printf("0x%08x,0x%08x,0x%08x,0x%08x\n", rd,rs1,rs2,imm);
    switch (op) {
        case OP_ADD: {
            const int32_t a = cpu->regs[rs1];
            const int32_t b = cpu->regs[rs2];
            const int32_t res = a + b;
            cpu->regs[rd] = res;
            update_add_flags(vm, a, b, res, cpu);
            break;
        }

        case OP_SUB: {
            int32_t a = cpu->regs[rs1];
            int32_t b = cpu->regs[rs2];
            int32_t res = a - b;
            cpu->regs[rd] = res;
            update_sub_flags(vm, a, b, res, cpu);
            break;
        }
        case OP_MUL: {
            cpu->regs[rd] = cpu->regs[rs1] * cpu->regs[rs2];
            update_logic_flags(vm, cpu->regs[rd], cpu);
            break;
        }
        case OP_HALT: {
            atomic_set_vm_halt(vm, 1);;
            return;
        }
        case OP_JMP: {
            cpu->ip = (size_t)(vm_addr_t)imm;
            break;
        }
        case OP_RJMP: {
            cpu->ip = (size_t)rel_target_from_last_ip(cpu, imm);
            break;
        }
        case OP_PUSH: {
            data_push(vm, cpu->regs[rd]);
            break;
        }
        case OP_POP: {
            cpu->regs[rd] = data_pop(vm);
            update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_CALL: {
            if (vfork_irq_probe_active(cpu) && vfork_irq_probe_addr((vm_addr_t)imm)) {
                fprintf(stderr,
                        "[irqcall] core=%d kind=call last_ip=0x%08zx ret=0x%08zx target=0x%08x call_base=0x%08x csp=0x%08x dsp=0x%08x isp=0x%08x in_interrupt=%d\n",
                        cpu->core_id,
                        cpu->last_ip,
                        cpu->ip,
                        (uint32_t)(vm_addr_t)imm,
                        (uint32_t)cpu->call_stack_base,
                        (uint32_t)cpu->csp,
                        (uint32_t)cpu->dsp,
                        (uint32_t)cpu->isp,
                        cpu->in_interrupt ? 1 : 0);
            }
            if (0 && ((vm_addr_t)imm == (vm_addr_t)0x00125DC8u ||
                 (vm_addr_t)imm == (vm_addr_t)0x00127BC0u) && cpu->core_id == 1) {
                fprintf(stderr,
                        "[callprobe] core=%d kind=call last_ip=0x%08zx ret=0x%08zx target=0x%08x call_base=0x%08x csp=0x%08x r0=0x%08x r1=0x%08x in_interrupt=%d\n",
                        cpu->core_id,
                        cpu->last_ip,
                        cpu->ip,
                        (uint32_t)(vm_addr_t)imm,
                        (uint32_t)cpu->call_stack_base,
                        (uint32_t)cpu->csp,
                        (uint32_t)cpu->regs[0],
                        (uint32_t)cpu->regs[1],
                        cpu->in_interrupt ? 1 : 0);
            }
            call_push(vm, (uint64_t)(vm_addr_t)cpu->ip);
            cpu->ip = (size_t)(vm_addr_t)imm;
            break;
        }
        case OP_RCALL: {
            const vm_addr_t target = rel_target_from_last_ip(cpu, imm);
            if (vfork_irq_probe_active(cpu) && (vfork_irq_probe_addr(target) || vfork_irq_probe_addr((vm_addr_t)cpu->last_ip))) {
                fprintf(stderr,
                        "[irqcall] core=%d kind=rcall last_ip=0x%08zx ret=0x%08zx target=0x%08x call_base=0x%08x csp=0x%08x dsp=0x%08x isp=0x%08x in_interrupt=%d\n",
                        cpu->core_id,
                        cpu->last_ip,
                        cpu->ip,
                        (uint32_t)target,
                        (uint32_t)cpu->call_stack_base,
                        (uint32_t)cpu->csp,
                        (uint32_t)cpu->dsp,
                        (uint32_t)cpu->isp,
                        cpu->in_interrupt ? 1 : 0);
            }
            if (0 && (target == (vm_addr_t)0x00125DC8u ||
                 target == (vm_addr_t)0x00127BC0u) && cpu->core_id == 1) {
                fprintf(stderr,
                        "[callprobe] core=%d kind=rcall last_ip=0x%08zx ret=0x%08zx target=0x%08x call_base=0x%08x csp=0x%08x r0=0x%08x r1=0x%08x in_interrupt=%d\n",
                        cpu->core_id,
                        cpu->last_ip,
                        cpu->ip,
                        (uint32_t)target,
                        (uint32_t)cpu->call_stack_base,
                        (uint32_t)cpu->csp,
                        (uint32_t)cpu->regs[0],
                        (uint32_t)cpu->regs[1],
                        cpu->in_interrupt ? 1 : 0);
            }
            call_push(vm, (uint64_t)(vm_addr_t)cpu->ip);
            cpu->ip = (size_t)target;
            break;
        }
        case OP_CALLR: {
            if (0 && ((vm_addr_t)cpu->regs[rd] == (vm_addr_t)0x00125DC8u ||
                 (vm_addr_t)cpu->regs[rd] == (vm_addr_t)0x00127BC0u) && cpu->core_id == 1) {
                fprintf(stderr,
                        "[callprobe] core=%d kind=callr last_ip=0x%08zx ret=0x%08zx target=0x%08x call_base=0x%08x csp=0x%08x r0=0x%08x r1=0x%08x in_interrupt=%d\n",
                        cpu->core_id,
                        cpu->last_ip,
                        cpu->ip,
                        (uint32_t)(vm_addr_t)cpu->regs[rd],
                        (uint32_t)cpu->call_stack_base,
                        (uint32_t)cpu->csp,
                        (uint32_t)cpu->regs[0],
                        (uint32_t)cpu->regs[1],
                        cpu->in_interrupt ? 1 : 0);
            }
            call_push(vm, (uint64_t)(vm_addr_t)cpu->ip);
            cpu->ip = (size_t)(vm_addr_t)cpu->regs[rd];
            break;
        }
        case OP_RET: {
            const vm_addr_t before_call_base = cpu->call_stack_base;
            const int before_csp = cpu->csp;
            const uint64_t ret_addr = call_pop(vm);
            if (vfork_irq_probe_active(cpu) &&
                (vfork_irq_probe_addr((vm_addr_t)cpu->last_ip) || vfork_irq_probe_addr((vm_addr_t)ret_addr))) {
                fprintf(stderr,
                        "[irqret] core=%d last_ip=0x%08zx ret=0x%08x before_call_base=0x%08x before_csp=0x%08x after_call_base=0x%08x after_csp=0x%08x in_interrupt=%d\n",
                        cpu->core_id,
                        cpu->last_ip,
                        (uint32_t)ret_addr,
                        (uint32_t)before_call_base,
                        (uint32_t)before_csp,
                        (uint32_t)cpu->call_stack_base,
                        (uint32_t)cpu->csp,
                        cpu->in_interrupt ? 1 : 0);
            }
            if (0 && (vm_addr_t)ret_addr == (vm_addr_t)0x00130D00u) {
                fprintf(stderr,
                        "[retprobe] core=%d last_ip=0x%08zx ret=0x%08x before_call_base=0x%08x before_csp=0x%08x after_call_base=0x%08x after_csp=0x%08x\n",
                        cpu->core_id,
                        cpu->last_ip,
                        (uint32_t)ret_addr,
                        (uint32_t)before_call_base,
                        (uint32_t)before_csp,
                        (uint32_t)cpu->call_stack_base,
                        (uint32_t)cpu->csp);
            }
            cpu->ip = (size_t)(vm_addr_t)ret_addr;
            break;
        }
        case OP_LOAD: {
            const vm_addr_t addr = cpu->regs[rs1] + imm;
            cpu->regs[rd] = (uint32_t) vm_read8(vm, addr);
            update_zf_sf(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_LOAD16: {
            const vm_addr_t addr = cpu->regs[rs1] + imm;
            ensure_halfword_aligned_or_panic(vm, addr, "LOAD16");
            const uint16_t v = (uint16_t)((uint16_t)vm_read8(vm, addr) |
                                          ((uint16_t)vm_read8(vm, addr + 1) << 8));
            cpu->regs[rd] = (uint32_t)v;
            update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_LOAD32: {
            const vm_addr_t addr = cpu->regs[rs1] + imm;
            cpu->regs[rd] = vm_read32(vm, addr);
            update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_LOADS8: {
            const vm_addr_t addr = cpu->regs[rs1] + imm;
            cpu->regs[rd] = (int32_t)(int8_t)vm_read8(vm, addr);
            update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_LOADS16: {
            const vm_addr_t addr = cpu->regs[rs1] + imm;
            ensure_halfword_aligned_or_panic(vm, addr, "LOADS16");
            const uint16_t bits = (uint16_t)((uint16_t)vm_read8(vm, addr) |
                                             ((uint16_t)vm_read8(vm, addr + 1) << 8));
            cpu->regs[rd] = (int32_t)(int16_t)bits;
            update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_LOADX: {
            const vm_addr_t addr = cpu->regs[rs1] + cpu->regs[rs2] + imm;
            cpu->regs[rd] = (uint32_t)vm_read8(vm, addr);
            update_zf_sf(vm, cpu->regs[rd], cpu);
            break;
        }
        case OP_LOADX16: {
            const vm_addr_t addr = cpu->regs[rs1] + cpu->regs[rs2] + imm;
            ensure_halfword_aligned_or_panic(vm, addr, "LOADX16");
            const uint16_t v = (uint16_t)((uint16_t)vm_read8(vm, addr) |
                                          ((uint16_t)vm_read8(vm, addr + 1) << 8));
            cpu->regs[rd] = (uint32_t)v;
            update_logic_flags(vm, cpu->regs[rd], cpu);
            break;
        }
        case OP_LOADX32: {
            const vm_addr_t addr = cpu->regs[rs1] + cpu->regs[rs2] + imm;
            cpu->regs[rd] = vm_read32(vm, addr);
            update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_STORE: {
            const vm_addr_t addr = cpu->regs[rs1] + imm;
            vm_write8(vm, addr, (uint8_t) cpu->regs[rd]);
            break;
        }
        case OP_STORE16: {
            const vm_addr_t addr = cpu->regs[rs1] + imm;
            ensure_halfword_aligned_or_panic(vm, addr, "STORE16");
            const uint16_t v = (uint16_t)cpu->regs[rd];
            vm_write8(vm, addr, (uint8_t)(v & 0xFFu));
            vm_write8(vm, addr + 1, (uint8_t)(v >> 8));
            break;
        }
        case OP_STOREX: {
            const vm_addr_t addr = cpu->regs[rs1] + cpu->regs[rs2] + imm;
            vm_write8(vm, addr, (uint8_t)cpu->regs[rd]);
            break;
        }
        case OP_STOREX16: {
            const vm_addr_t addr = cpu->regs[rs1] + cpu->regs[rs2] + imm;
            ensure_halfword_aligned_or_panic(vm, addr, "STOREX16");
            const uint16_t v = (uint16_t)cpu->regs[rd];
            vm_write8(vm, addr, (uint8_t)(v & 0xFFu));
            vm_write8(vm, addr + 1, (uint8_t)(v >> 8));
            break;
        }
        case OP_STORE32: {
            const vm_addr_t addr = cpu->regs[rs1] + imm;
            vm_write32(vm, addr, (uint32_t) cpu->regs[rd]);
            break;
        }
        case OP_STOREX32: {
            const vm_addr_t addr = cpu->regs[rs1] + cpu->regs[rs2] + imm;
            vm_write32(vm, addr, (uint32_t) cpu->regs[rd]);
            break;
        }
        case OP_CMP: {
            const int32_t val1 = cpu->regs[rd];
            const int32_t val2 = cpu->regs[rs1];
            const int32_t res = val1 - val2;
            update_sub_flags(vm, val1, val2, res,cpu);
            break;
        }
        case OP_CMPI: {
            const int32_t val1 = cpu->regs[rd];
            const int32_t val2 = imm;
            const int32_t res = val1 - val2;
            update_sub_flags(vm, val1, val2, res,cpu);
            break;
        }
        case OP_MOV: {
            cpu->regs[rd] = cpu->regs[rs1];
            update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_MOVI: {
            cpu->regs[rd] = imm;
            update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_MEMSET: {
            const uint32_t base = (uint32_t) cpu->regs[rd];
            const uint8_t value = (uint8_t) cpu->regs[rs1];
            const uint32_t count = (uint32_t) imm;

            for (uint32_t i = 0; i < count; i++) {
                vm_write8(vm, base + i, value);
            }
            break;
        }
        case OP_MEMCPY: {
            const uint32_t dest = (uint32_t) cpu->regs[rd];
            const uint32_t src = (uint32_t) cpu->regs[rs1];
            const uint32_t count = (uint32_t) imm;

            for (uint32_t i = 0; i < count; i++) {
                uint8_t v = vm_read8(vm, src + i);
                vm_write8(vm, dest + i, v);
            }
            break;
        }
        case OP_IN: {
            const int addr = cpu->regs[rs1];
            if (addr >= 0 && addr < IO_SIZE) {
                if (addr == CPU_CTX_CSP) {
                    cpu->regs[rd] = cpu->csp;
                    break;
                }
                if (addr == CPU_CTX_DSP) {
                    cpu->regs[rd] = cpu->dsp;
                    break;
                }
                if (addr == CPU_CTX_IRQ_MASK) {
                    cpu->regs[rd] = cpu->irq_masked ? 1 : 0;
                    break;
                }
                if (addr == CPU_CTX_ISP) {
                    cpu->regs[rd] = cpu->isp;
                    break;
                }
                if (addr == CPU_CTX_CALL_BASE) {
                    cpu->regs[rd] = (int32_t)cpu->call_stack_base;
                    break;
                }
                if (addr == CPU_CTX_DATA_BASE) {
                    cpu->regs[rd] = (int32_t)cpu->data_stack_base;
                    break;
                }
                if (addr == CPU_CTX_ISR_BASE) {
                    cpu->regs[rd] = (int32_t)cpu->isr_stack_base;
                    break;
                }
                vm_shared_lock(vm);
                if (addr == KEYBOARD) {
                    int v = 0;
                    if (vm->serial_rx_tail != vm->serial_rx_head) {
                        v = (int)vm->serial_rx_fifo[vm->serial_rx_tail];
                        vm->serial_rx_tail = (uint16_t)((vm->serial_rx_tail + 1u) & 0xFFu);
                    }
                    cpu->regs[rd] = v;
                    if (vm->serial_rx_tail != vm->serial_rx_head) {
                        vm->io[KEYBOARD] = (int)vm->serial_rx_fifo[vm->serial_rx_tail];
                        vm->io[SCREEN_ATTRIBUTE] |= SERIAL_STATUS_RX_READY;
                    } else {
                        vm->io[KEYBOARD] = 0;
                        vm->io[SCREEN_ATTRIBUTE] &= ~SERIAL_STATUS_RX_READY;
                    }
                    /*
                     * RX read acts as IRQ acknowledge: clear serial/keyboard bits on core 0.
                     * This prevents stale pending bits from retriggering the same input forever.
                     */
                    vm_irq_ack_input(vm, INT_SERIAL);
                    vm_irq_ack_input(vm, INT_KEYBOARD);
                    if (vm->serial_rx_tail != vm->serial_rx_head &&
                        ((vm->io[SCREEN_ATTRIBUTE] >> 8) & SERIAL_CTRL_RX_INT_ENABLE)) {
                        trigger_interrupt(vm, INT_SERIAL);
                    }
                } else if (addr == PS2_KBD_DATA) {
                    int v = 0;
                    if (vm->ps2_kbd_tail != vm->ps2_kbd_head) {
                        v = (int)vm->ps2_kbd_fifo[vm->ps2_kbd_tail];
                        vm->ps2_kbd_tail = (uint16_t)((vm->ps2_kbd_tail + 1u) & 0xFFu);
                    }
                    cpu->regs[rd] = v;
                    if (vm->ps2_kbd_tail != vm->ps2_kbd_head) {
                        vm->io[PS2_KBD_DATA] = (int)vm->ps2_kbd_fifo[vm->ps2_kbd_tail];
                        vm->io[PS2_KBD_STATUS] |= PS2_STATUS_RX_READY;
                    } else {
                        vm->io[PS2_KBD_DATA] = 0;
                        vm->io[PS2_KBD_STATUS] &= ~PS2_STATUS_RX_READY;
                    }
                    vm_irq_ack_input(vm, INT_KEYBOARD);
                    if (vm->ps2_kbd_tail != vm->ps2_kbd_head) {
                        trigger_interrupt(vm, INT_KEYBOARD);
                    }
                } else if (addr == PS2_MOUSE_DATA) {
                    int v = 0;
                    if (vm->ps2_mouse_tail != vm->ps2_mouse_head) {
                        v = (int)vm->ps2_mouse_fifo[vm->ps2_mouse_tail];
                        vm->ps2_mouse_tail = (uint16_t)((vm->ps2_mouse_tail + 1u) & 0xFFu);
                    }
                    cpu->regs[rd] = v;
                    if (vm->ps2_mouse_tail != vm->ps2_mouse_head) {
                        vm->io[PS2_MOUSE_DATA] = (int)vm->ps2_mouse_fifo[vm->ps2_mouse_tail];
                        vm->io[PS2_MOUSE_STATUS] |= PS2_STATUS_RX_READY;
                    } else {
                        vm->io[PS2_MOUSE_DATA] = 0;
                        vm->io[PS2_MOUSE_STATUS] &= ~PS2_STATUS_RX_READY;
                    }
                    vm_irq_ack_input(vm, INT_MOUSE);
                    if (vm->ps2_mouse_tail != vm->ps2_mouse_head) {
                        trigger_interrupt(vm, INT_MOUSE);
                    }
                } else if (addr == SCREEN_ATTRIBUTE) {
                    cpu->regs[rd] = vm->io[SCREEN_ATTRIBUTE] & 0xFF;
                } else if (addr == PS2_KBD_STATUS) {
                    cpu->regs[rd] = vm->io[PS2_KBD_STATUS] & 0xFF;
                } else if (addr == PS2_MOUSE_STATUS) {
                    cpu->regs[rd] = vm->io[PS2_MOUSE_STATUS] & 0xFF;
                } else {
                    cpu->regs[rd] = vm->io[addr];
                }
                vm_shared_unlock(vm);
            } else {
                panic(panic_format("IN invalid IO address %d", addr), vm);
            }
            break;
        }
        case OP_OUT: {
            const int addr = cpu->regs[rs1];
            if (addr >= 0 && addr < IO_SIZE) {
                if (addr == CPU_CTX_CSP) {
                    const int v = cpu->regs[rd];
                    if (v >= 0 && v <= CALL_STACK_SIZE) {
                        cpu->csp = v;
                    }
                    break;
                }
                if (addr == CPU_CTX_DSP) {
                    const int v = cpu->regs[rd];
                    if (v >= 0 && v <= DATA_STACK_SIZE) {
                        cpu->dsp = v;
                    }
                    break;
                }
                if (addr == CPU_CTX_IRQ_MASK) {
                    cpu->irq_masked = (cpu->regs[rd] != 0);
                    break;
                }
                if (addr == CPU_CTX_ISP) {
                    const int v = cpu->regs[rd];
                    if (v >= 0 && v <= ISR_STACK_SIZE) {
                        cpu->isp = v;
                    }
                    break;
                }
                if (addr == CPU_CTX_CALL_BASE) {
                    const vm_addr_t v = (vm_addr_t)(uint32_t)cpu->regs[rd];
                    const vm_addr_t old = cpu->call_stack_base;
                    if (!vm_stack_region_valid(vm, v, (size_t)CALL_STACK_SIZE * 8u)) {
                        panic(panic_format("OUT invalid call stack base 0x%08x", (uint32_t)v), vm);
                        break;
                    }
                    if (0 && cpu->core_id == 1 &&
                        (v == (vm_addr_t)0x03DCBC00u || v == (vm_addr_t)0x03DD5000u ||
                         old == (vm_addr_t)0x03DCBC00u || old == (vm_addr_t)0x03DD5000u)) {
                        fprintf(stderr,
                                "[callbase-out] core=%d last_ip=0x%08zx old=0x%08x new=0x%08x csp=0x%08x irq_masked=%d in_interrupt=%d\n",
                                cpu->core_id,
                                cpu->last_ip,
                                (uint32_t)old,
                                (uint32_t)v,
                                (uint32_t)cpu->csp,
                                cpu->irq_masked,
                                cpu->in_interrupt);
                    }
                    cpu->call_stack_base = v;
                    break;
                }
                if (addr == CPU_CTX_DATA_BASE) {
                    const vm_addr_t v = (vm_addr_t)(uint32_t)cpu->regs[rd];
                    if (!vm_stack_region_valid(vm, v, (size_t)DATA_STACK_SIZE * 4u)) {
                        panic(panic_format("OUT invalid data stack base 0x%08x", (uint32_t)v), vm);
                        break;
                    }
                    cpu->data_stack_base = v;
                    break;
                }
                if (addr == CPU_CTX_ISR_BASE) {
                    const vm_addr_t v = (vm_addr_t)(uint32_t)cpu->regs[rd];
                    if (!vm_stack_region_valid(vm, v, (size_t)ISR_STACK_SIZE * 8u)) {
                        panic(panic_format("OUT invalid isr stack base 0x%08x", (uint32_t)v), vm);
                        break;
                    }
                    cpu->isr_stack_base = v;
                    break;
                }
                accept_io(vm, addr, cpu->regs[rd]);
            } else {
                panic(panic_format("OUT invalid IO address %d\n", addr), vm);
            }
            break;
        }
        case OP_INT: {
            const uint32_t int_no = cpu->regs[rd];
            vm_enter_interrupt(vm, int_no);
            break;
        }
        case OP_INTI: {
            const uint32_t int_no = (uint32_t)imm;
            vm_enter_interrupt(vm, int_no);
            break;
        }
        case OP_IRET: {
            vm_iret(vm);
            break;
        }
        case OP_AND: {
            cpu->regs[rd] = cpu->regs[rs1] & cpu->regs[rs2];
            update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_OR: {
            cpu->regs[rd] = cpu->regs[rs1] | cpu->regs[rs2];
            update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_XOR: {
            cpu->regs[rd] = cpu->regs[rs1] ^ cpu->regs[rs2];
            update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_NOT: {
            cpu->regs[rd] = ~cpu->regs[rs1];
            update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_SHL: {
            uint32_t sh = (uint32_t)cpu->regs[rs2] & 31u;
            cpu->regs[rd] = (int32_t)((uint32_t)cpu->regs[rs1] << sh);
            update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_SHR: {
            uint32_t sh = (uint32_t)cpu->regs[rs2] & 31u;
            cpu->regs[rd] = (int32_t)((uint32_t)cpu->regs[rs1] >> sh);
            update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_SAR: {
            uint32_t sh = (uint32_t)cpu->regs[rs2] & 31u;
            cpu->regs[rd] = cpu->regs[rs1] >> sh;
            update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_ROL: {
            uint32_t sh = (uint32_t)cpu->regs[rs2] & 31u;
            cpu->regs[rd] = (int32_t)rotl32((uint32_t)cpu->regs[rs1], sh);
            update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_ROR: {
            uint32_t sh = (uint32_t)cpu->regs[rs2] & 31u;
            cpu->regs[rd] = (int32_t)rotr32((uint32_t)cpu->regs[rs1], sh);
            update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_DIV: {
            if (cpu->regs[rs2] != 0) {
                cpu->regs[rd] = cpu->regs[rs1] / cpu->regs[rs2];
                update_logic_flags(vm, cpu->regs[rd],cpu);
            } else {
                trigger_interrupt(vm, INT_DIVIDE_BY_ZERO);
            }
            break;
        }
        case OP_MOD: {
            if (cpu->regs[rs2] != 0) {
                cpu->regs[rd] = cpu->regs[rs1] % cpu->regs[rs2];
                update_logic_flags(vm, cpu->regs[rd],cpu);
            } else {
                trigger_interrupt(vm, INT_DIVIDE_BY_ZERO);
            }
            break;
        }
        case OP_INC: {
            const int32_t a = cpu->regs[rd];
            const int32_t b = 1;
            const int32_t res = a + b;
            cpu->regs[rd] = res;
            update_add_flags(vm, a, b, res,cpu);
            break;
        }

        case OP_JZ: {
            if (cpu->flags & FLAG_ZF) {
                cpu->ip = (size_t)(vm_addr_t)imm;
            }
            break;
        }
        case OP_RJZ: {
            if (cpu->flags & FLAG_ZF) {
                cpu->ip = (size_t)rel_target_from_last_ip(cpu, imm);
            }
            break;
        }
        case OP_JNZ: {
            if (!(cpu->flags & FLAG_ZF)) {
                cpu->ip = (size_t)(vm_addr_t)imm;
            }
            break;
        }
        case OP_RJNZ: {
            if (!(cpu->flags & FLAG_ZF)) {
                cpu->ip = (size_t)rel_target_from_last_ip(cpu, imm);
            }
            break;
        }
        case OP_RJG: {
            if (!(cpu->flags & FLAG_ZF) && ((cpu->flags & FLAG_SF) == (cpu->flags & FLAG_OF))) {
                cpu->ip = (size_t)rel_target_from_last_ip(cpu, imm);
            }
            break;
        }
        case OP_RJGE: {
            if ((cpu->flags & FLAG_SF) == (cpu->flags & FLAG_OF)) {
                cpu->ip = (size_t)rel_target_from_last_ip(cpu, imm);
            }
            break;
        }
        case OP_RJL: {
            if ((cpu->flags & FLAG_SF) != (cpu->flags & FLAG_OF)) {
                cpu->ip = (size_t)rel_target_from_last_ip(cpu, imm);
            }
            break;
        }
        case OP_RJLE: {
            if ((cpu->flags & FLAG_ZF) || (cpu->flags & FLAG_SF) != (cpu->flags & FLAG_OF)) {
                cpu->ip = (size_t)rel_target_from_last_ip(cpu, imm);
            }
            break;
        }
        case OP_RJC: {
            if (cpu->flags & FLAG_CF) {
                cpu->ip = (size_t)rel_target_from_last_ip(cpu, imm);
            }
            break;
        }
        case OP_RJNC: {
            if (!(cpu->flags & FLAG_CF)) {
                cpu->ip = (size_t)rel_target_from_last_ip(cpu, imm);
            }
            break;
        }
        case OP_JG: {
            if (!(cpu->flags & FLAG_ZF) && ((cpu->flags & FLAG_SF) == (cpu->flags & FLAG_OF))) {
                cpu->ip = (size_t)(vm_addr_t)imm;
            }
            break;
        }
        case OP_JGE: {
            if ((cpu->flags & FLAG_SF) == (cpu->flags & FLAG_OF)) {
                cpu->ip = (size_t)(vm_addr_t)imm;
            }
            break;
        }
        case OP_JL: {
            if ((cpu->flags & FLAG_SF) != (cpu->flags & FLAG_OF)) {
                cpu->ip = (size_t)(vm_addr_t)imm;
            }
            break;
        }
        case OP_JLE: {
            if ((cpu->flags & FLAG_ZF) || (cpu->flags & FLAG_SF) != (cpu->flags & FLAG_OF)) {
                cpu->ip = (size_t)(vm_addr_t)imm;
            }
            break;
        }
        case OP_JC: {
            if (cpu->flags & FLAG_CF) {
                cpu->ip = (size_t)(vm_addr_t)imm;
            }
            break;
        }
        case OP_JNC: {
            if (!(cpu->flags & FLAG_CF)) {
                cpu->ip = (size_t)(vm_addr_t)imm;
            }
            break;
        }

        case OP_FADD: {
            float a = reg_as_f32(cpu->regs[rs1]);
            float b = reg_as_f32(cpu->regs[rs2]);
            float r = a + b;
            cpu->regs[rd] = f32_as_reg(r);
            update_logic_flags(vm, (r == 0.0f) ? 0 : (r < 0.0f ? -1 : 1),cpu);
            break;
        }
        case OP_FSUB: {
            float a = reg_as_f32(cpu->regs[rs1]);
            float b = reg_as_f32(cpu->regs[rs2]);
            float r = a - b;
            cpu->regs[rd] = f32_as_reg(r);
            update_logic_flags(vm, (r == 0.0f) ? 0 : (r < 0.0f ? -1 : 1),cpu);
            break;
        }
        case OP_FMUL: {
            float a = reg_as_f32(cpu->regs[rs1]);
            float b = reg_as_f32(cpu->regs[rs2]);
            float r = a * b;
            cpu->regs[rd] = f32_as_reg(r);
            update_logic_flags(vm, (r == 0.0f) ? 0 : (r < 0.0f ? -1 : 1),cpu);
            break;
        }
        case OP_FDIV: {
            float a = reg_as_f32(cpu->regs[rs1]);
            float b = reg_as_f32(cpu->regs[rs2]);
            float r = a / b;
            cpu->regs[rd] = f32_as_reg(r);
            update_logic_flags(vm, (r == 0.0f) ? 0 : (r < 0.0f ? -1 : 1),cpu);
            break;
        }
        case OP_FNEG: {
            float a = reg_as_f32(cpu->regs[rs1]);
            float r = -a;
            cpu->regs[rd] = f32_as_reg(r);
            update_logic_flags(vm, (r == 0.0f) ? 0 : (r < 0.0f ? -1 : 1),cpu);
            break;
        }
        case OP_FABS: {
            float a = reg_as_f32(cpu->regs[rs1]);
            float r = fabsf(a);
            cpu->regs[rd] = f32_as_reg(r);
            update_logic_flags(vm, (r == 0.0f) ? 0 : 1,cpu);
            break;
        }
        case OP_FSQRT: {
            float a = reg_as_f32(cpu->regs[rs1]);
            float r = sqrtf(a);
            cpu->regs[rd] = f32_as_reg(r);
            update_logic_flags(vm, (r == 0.0f) ? 0 : (r < 0.0f ? -1 : 1),cpu);
            break;
        }
        case OP_ITOF: {
            int32_t i = cpu->regs[rs1];
            float f = (float) i;
            cpu->regs[rd] = f32_as_reg(f);
            update_logic_flags(vm, (f == 0.0f) ? 0 : (f < 0.0f ? -1 : 1),cpu);
            break;
        }
        case OP_FTOI: {
            float f = reg_as_f32(cpu->regs[rs1]);
            if (f32_is_nan(f) || f > 2147483647.0f || f < -2147483648.0f) {
                cpu->regs[rd] = 0;
                cpu->flags |= FLAG_OF;
                cpu->flags &= ~(FLAG_ZF | FLAG_SF | FLAG_CF);
            } else {
                int32_t i = (int32_t) f;
                cpu->regs[rd] = i;
                update_logic_flags(vm, i,cpu);
            }
            break;
        }

        case OP_FLOAD32: {
            const vm_addr_t addr = cpu->regs[rs1] + imm;
            uint32_t bits = (uint32_t) vm_read32(vm, addr);
            cpu->regs[rd] = (int32_t) bits;
            float f = reg_as_f32(cpu->regs[rd]);
            update_logic_flags(vm, (f == 0.0f) ? 0 : (f < 0.0f ? -1 : 1),cpu);
            break;
        }
        case OP_FSTORE32: {
            const vm_addr_t addr = cpu->regs[rs1] + imm;
            vm_write32(vm, addr, (uint32_t) cpu->regs[rd]);
            break;
        }
        case OP_FCMP: {
            float a = reg_as_f32(cpu->regs[rd]);
            float b = reg_as_f32(cpu->regs[rs1]);
            update_fcmp_flags(vm, a, b);
            break;
        }
        case OP_ADDI: {
            const int32_t a = cpu->regs[rs1];
            const int32_t b = imm;
            const int32_t res = a + b;
            cpu->regs[rd] = res;
            update_add_flags(vm, a, b, res,cpu);
            break;
        }
        case OP_SUBI: {
            const int32_t a = cpu->regs[rs1];
            const int32_t b = imm;
            const int32_t res = a - b;
            cpu->regs[rd] = res;
            update_sub_flags(vm, a, b, res,cpu);
            break;
        }
        case OP_ANDI: {
            cpu->regs[rd] = cpu->regs[rs1] & imm;
            update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_ORI: {
            cpu->regs[rd] = cpu->regs[rs1] | imm;
            update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_XORI: {
            cpu->regs[rd] = cpu->regs[rs1] ^ imm;
            update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_SHLI: {
            uint32_t sh = (uint32_t)imm & 31u;
            cpu->regs[rd] = (int32_t)((uint32_t)cpu->regs[rs1] << sh);
            update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_SHRI: {
            uint32_t sh = (uint32_t)imm & 31u;
            cpu->regs[rd] = (int32_t)((uint32_t)cpu->regs[rs1] >> sh);
            update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_ROLI: {
            uint32_t sh = (uint32_t)imm & 31u;
            cpu->regs[rd] = (int32_t)rotl32((uint32_t)cpu->regs[rs1], sh);
            update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_RORI: {
            uint32_t sh = (uint32_t)imm & 31u;
            cpu->regs[rd] = (int32_t)rotr32((uint32_t)cpu->regs[rs1], sh);
            update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_CAS: {
            const vm_addr_t addr = cpu->regs[rs1] + imm;
            ensure_atomic_aligned_or_panic(vm, addr, "CAS");
            const uint32_t expected = (uint32_t)cpu->regs[rd];
            const uint32_t desired = (uint32_t)cpu->regs[rs2];
            int success = 0;
            const uint32_t old = vm_atomic_compare_exchange32_seqcst(vm, addr, expected, desired, &success);
            set_cas_flags(vm, success,cpu);
            cpu->regs[rd] = (int32_t)old;
            break;
        }
        case OP_XADD: {
            const vm_addr_t addr = cpu->regs[rs1] + imm;
            ensure_atomic_aligned_or_panic(vm, addr, "XADD");
            const uint32_t addend = (uint32_t)cpu->regs[rs2];
            const uint32_t old = vm_atomic_fetch_add32_seqcst(vm, addr, addend);
            const uint32_t newv = old + addend;
            cpu->regs[rd] = (int32_t)old;
            update_add_flags(vm, (int32_t)old, (int32_t)addend, (int32_t)newv,cpu);
            break;
        }
        case OP_XCHG: {
            const vm_addr_t addr = cpu->regs[rs1] + imm;
            ensure_atomic_aligned_or_panic(vm, addr, "XCHG");
            const uint32_t newv = (uint32_t)cpu->regs[rs2];
            const uint32_t old = vm_atomic_exchange32_seqcst(vm, addr, newv);
            cpu->regs[rd] = (int32_t)old;
            update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_LDAR: {
            const vm_addr_t addr = cpu->regs[rs1] + imm;
            ensure_atomic_aligned_or_panic(vm, addr, "LDAR");
            const uint32_t v = vm_atomic_load32_acquire(vm, addr);
            cpu->regs[rd] = (int32_t)v;
            update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_STLR: {
            const vm_addr_t addr = cpu->regs[rs1] + imm;
            ensure_atomic_aligned_or_panic(vm, addr, "STLR");
            const uint32_t v = (uint32_t)cpu->regs[rd];
            vm_atomic_store32_release(vm, addr, v);
            break;
        }
        case OP_FENCE: {
            (void)rd;
            (void)rs1;
            (void)rs2;
            (void)imm;
            atomic_thread_fence(memory_order_seq_cst);
            break;
        }
        case OP_PAUSE: {
            (void)rd;
            (void)rs1;
            (void)rs2;
            (void)imm;
            sched_yield();
            break;
        }
        case OP_STARTAP: {
            if (!cpu->is_bsp)
                break;
            const int target = (int)cpu->regs[rd];
            const vm_addr_t entry = (vm_addr_t)(cpu->regs[rs1] + imm);
            if (target <= 0 || target >= vm->smp_cores)
                break;
            vm->cpus[target].ip = entry;
            vm->cpus[target].last_ip = entry;
            atomic_store_explicit(&vm->core_released[target], true, memory_order_release);
            break;
        }
        case OP_IPI: {
            const int target = (int)cpu->regs[rd];
            const uint32_t int_no = (uint32_t)cpu->regs[rs1];
            trigger_interrupt_target(vm, target, int_no);
            break;
        }
        case OP_CPUID: {
            cpu->regs[rd] = (uint32_t)cpu->core_id;
            update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        default: {
            panic(panic_format("Unknown opcode %d\n", op), vm);
            return;
        }
    }
}

static inline void vm_flush_execution_times(VCPU *cpu, uint64_t *local_cycles) {
    if (*local_cycles == 0) {
        return;
    }
    atomic_fetch_add_explicit(&cpu->execution_times, *local_cycles, memory_order_relaxed);
    *local_cycles = 0;
}

void *vm_thread(void *arg) {
    CpuThreadArg *thread_arg = (CpuThreadArg *)arg;
    VM *vm = thread_arg->vm;
    int core_id = thread_arg->core_id;
    free(thread_arg);
    vm_tls_vcpu = &vm->cpus[core_id];
    uint64_t local_cycles = 0;

    while (1) {
        if (atomic_is_vm_halted(vm)|| atomic_is_vm_panicked(vm)) {
            break;
        }
        if (core_id != 0 && !atomic_load_explicit(&vm->core_released[core_id], memory_order_acquire)) {
            sched_yield();
            continue;
        }
        if (core_id == 0) {
            vm_debug_pause_if_needed(vm, (uint32_t) vm_tls_vcpu->ip);
        }
        vm_handle_interrupts(vm);
        vm_instruction_case(vm);
        local_cycles++;
        if (local_cycles >= EXECUTION_TIMES_FLUSH_INTERVAL) {
            vm_flush_execution_times(vm_tls_vcpu, &local_cycles);
        }
        if (core_id == 0) {
            disk_tick(vm);
        }
    }
    vm_flush_execution_times(vm_tls_vcpu, &local_cycles);
    return NULL;
}

void display_loop(VM *vm) {
    vga_display_init();
    const int frame_delay = 16; // ~60FPS
    while (!atomic_is_vm_halted(vm)) {
        uint32_t frame_start = SDL_GetTicks();
        display_poll_events(vm);
        display_update(vm);
        uint32_t frame_time = SDL_GetTicks() - frame_start;
        if (frame_time < frame_delay) {
            SDL_Delay(frame_delay - frame_time);
        }
    }
    display_shutdown();
}

void vm_run(VM *vm) {
    const int cores = (vm->smp_cores > 0) ? vm->smp_cores : 1;
    pthread_t *thread_ids = malloc(sizeof(pthread_t) * (size_t)cores);
    if (!thread_ids) {
        panic("Failed to allocate CPU thread list", vm);
        return;
    }

    int created_threads = 0;
    for (int i = 0; i < cores; i++) {
        CpuThreadArg *arg = malloc(sizeof(CpuThreadArg));
        if (!arg) {
            panic("Failed to allocate CPU thread argument", vm);
            atomic_set_vm_halt(vm, 1);;
            break;
        }
        arg->vm = vm;
        arg->core_id = i;
        if (pthread_create(&thread_ids[i], NULL, vm_thread, arg) != 0) {
            free(arg);
            panic("Failed to create CPU thread", vm);
            atomic_set_vm_halt(vm, 1);;
            break;
        }
        created_threads++;
    }

    display_loop(vm);

    for (int i = 0; i < created_threads; i++) {
        pthread_join(thread_ids[i], NULL);
    }
    free(thread_ids);
}

void vm_run_serial(VM *vm) {
    const int cores = (vm->smp_cores > 0) ? vm->smp_cores : 1;
    pthread_t *thread_ids = malloc(sizeof(pthread_t) * (size_t)cores);
    int created_threads = 0;
    int stdin_flags = -1;
    int stdin_restore = 0;
    int raw_mode = 0;
    if (!thread_ids) {
        panic("Failed to allocate CPU thread list", vm);
        return;
    }

    stdin_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (stdin_flags >= 0) {
        if (fcntl(STDIN_FILENO, F_SETFL, stdin_flags | O_NONBLOCK) == 0) {
            stdin_restore = 1;
        }
    }
    if (isatty(STDIN_FILENO)) {
        enable_raw_mode();
        raw_mode = 1;
    }

    for (int i = 0; i < cores; i++) {
        CpuThreadArg *arg = malloc(sizeof(CpuThreadArg));
        if (!arg) {
            panic("Failed to allocate CPU thread argument", vm);
            atomic_set_vm_halt(vm, 1);
            break;
        }
        arg->vm = vm;
        arg->core_id = i;
        if (pthread_create(&thread_ids[i], NULL, vm_thread, arg) != 0) {
            free(arg);
            panic("Failed to create CPU thread", vm);
            atomic_set_vm_halt(vm, 1);
            break;
        }
        created_threads++;
    }

    while (!atomic_is_vm_halted(vm) && !atomic_is_vm_panicked(vm)) {
        vm_handle_keyboard(vm);
        usleep(1000);
    }

    if (raw_mode) {
        disable_raw_mode();
    }
    if (stdin_restore) {
        (void)fcntl(STDIN_FILENO, F_SETFL, stdin_flags);
    }

    for (int i = 0; i < created_threads; i++) {
        pthread_join(thread_ids[i], NULL);
    }
    free(thread_ids);
}

int vm_run_headless(VM *vm, uint64_t timeout_ms) {
    const int cores = (vm->smp_cores > 0) ? vm->smp_cores : 1;
    pthread_t *thread_ids = malloc(sizeof(pthread_t) * (size_t)cores);
    if (!thread_ids) {
        panic("Failed to allocate CPU thread list", vm);
        return 0;
    }

    int created_threads = 0;
    for (int i = 0; i < cores; i++) {
        CpuThreadArg *arg = malloc(sizeof(CpuThreadArg));
        if (!arg) {
            atomic_set_vm_halt(vm, 1);;
            break;
        }
        arg->vm = vm;
        arg->core_id = i;
        if (pthread_create(&thread_ids[i], NULL, vm_thread, arg) != 0) {
            free(arg);
            atomic_set_vm_halt(vm, 1);;
            break;
        }
        created_threads++;
    }

    const uint64_t start_ns = host_monotonic_time_ns();
    while (!atomic_is_vm_halted(vm) && !atomic_is_vm_panicked(vm)) {
        const uint64_t elapsed_ms = (host_monotonic_time_ns() - start_ns) / 1000000ull;
        if (elapsed_ms > timeout_ms) {
            atomic_set_vm_halt(vm, 1);;
            break;
        }
        usleep(1000);
    }

    for (int i = 0; i < created_threads; i++) {
        pthread_join(thread_ids[i], NULL);
    }
    free(thread_ids);
    return atomic_is_vm_panicked(vm) ? 0 : 1;
}

void vm_dump(const VM *vm, int mem_preview) {
    const VCPU *cpu = (vm && vm->cpus) ? &vm->cpus[0] : NULL;
    if (!vm || !cpu)
        return;
    printf("VM dump:\n");
    printf("Core: 0\n");
    printf("CPU state: csp=%d dsp=%d isp=%d in_interrupt=%d irq_masked=%d\n",
           cpu->csp,
           cpu->dsp,
           cpu->isp,
           cpu->in_interrupt,
           cpu->irq_masked);
    printf("Registers:\n");
    for (int i = 0; i < REG_COUNT; i++) {
        printf("r%d = %d\n", i, cpu->regs[i]);
    }

    printf("Call Stack (top -> bottom):\n");
    for (int i = cpu->csp; i < CALL_STACK_SIZE; i++) {
        const vm_addr_t addr = cpu->call_stack_base + (vm_addr_t)i * 8u;
        const uint64_t v = (uint64_t) vm->memory[addr + 0]
            | ((uint64_t) vm->memory[addr + 1] << 8)
            | ((uint64_t) vm->memory[addr + 2] << 16)
            | ((uint64_t) vm->memory[addr + 3] << 24)
            | ((uint64_t) vm->memory[addr + 4] << 32)
            | ((uint64_t) vm->memory[addr + 5] << 40)
            | ((uint64_t) vm->memory[addr + 6] << 48)
            | ((uint64_t) vm->memory[addr + 7] << 56);
        printf("[%d] = %llu\n", i, (unsigned long long) v);
    }
    if (cpu->csp == CALL_STACK_SIZE) {
        printf("<empty>\n");
    }
    printf("Data Stack (top -> bottom):\n");
    for (int i = cpu->dsp; i < DATA_STACK_SIZE; i++) {
        const vm_addr_t addr = cpu->data_stack_base + (vm_addr_t)i * 4u;
        const uint32_t v = (uint32_t) vm->memory[addr + 0]
            | ((uint32_t) vm->memory[addr + 1] << 8)
            | ((uint32_t) vm->memory[addr + 2] << 16)
            | ((uint32_t) vm->memory[addr + 3] << 24);
        printf("[%d] = %u\n", i, v);
    }
    if (cpu->dsp == DATA_STACK_SIZE) {
        printf("<empty>\n");
    }
    printf("Memory (first %d cells):\n", mem_preview);
    for (int i = 0; i < mem_preview && i < (ssize_t)MEM_SIZE; i++) {
        printf("[%d] = %d\n", i, vm->memory[i]);
    }
    printf("IP = %lu\n", cpu->ip);
    printf("ZF = %d\n", (cpu->flags & FLAG_ZF) != 0);
}

void vm_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    printf("======================\n\n");
    fprintf(stderr, "VM encountered a fatal error and could not recover:\n");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
    printf("\n======================\n");
    fflush(stderr);
}

VM *vm_create(size_t memory_size,
              const uint64_t *program,
              size_t program_size,
              const uint8_t *data,
              size_t data_size,
              const ProgramLayout *layout,
              int smp_cores) {
    VM *vm = malloc(sizeof(VM));
    if (!vm)
        return NULL;

    memset(vm, 0, sizeof(VM));
    vm->smp_cores = (smp_cores > 0) ? smp_cores : 1;
    vm->cpus = calloc((size_t)vm->smp_cores, sizeof(VCPU));
    atomic_init(&vm->halted, 0);
    atomic_init(&vm->panic, 0);
    if (!vm->cpus) {
        free(vm);
        vm_error("Couldn't create CPU.");
        return NULL;
    }
    vm->core_released = calloc((size_t)vm->smp_cores, sizeof(atomic_bool));
    if (!vm->core_released) {
        free(vm->cpus);
        free(vm);
        vm_error("Couldn't allocate VM CPU list");
        return NULL;
    }
    vm->interrupt_bitmap = calloc((size_t)vm->smp_cores * (size_t)IRQ_BITMAP_WORDS,
                                  sizeof(atomic_uint_fast64_t));
    if (!vm->interrupt_bitmap) {
        free(vm->core_released);
        free(vm->cpus);
        free(vm);
        vm_error("Couldn't allocate VM CPU Interrupt bitmap");
        return NULL;
    }
    vm->interrupt_enable_bitmap = calloc((size_t)vm->smp_cores * (size_t)IRQ_BITMAP_WORDS,
                                         sizeof(atomic_uint_fast64_t));
    if (!vm->interrupt_enable_bitmap) {
        free(vm->interrupt_bitmap);
        free(vm->core_released);
        free(vm->cpus);
        free(vm);
        vm_error("Couldn't allocate VM CPU Interrupt enable bitmap");
        return NULL;
    }

    pthread_mutexattr_t shared_attr;
    pthread_mutexattr_init(&shared_attr);
    pthread_mutexattr_settype(&shared_attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&vm->shared_lock, &shared_attr);
    pthread_mutexattr_destroy(&shared_attr);

    vm->memory_size = memory_size;
    vm->memory = malloc(memory_size);
    if (!vm->memory) {
        free(vm->interrupt_enable_bitmap);
        free(vm->interrupt_bitmap);
        free(vm->core_released);
        free(vm->cpus);
        free(vm);
        vm_error("Couldn't allocate VM memory %zu", memory_size);
        return NULL;
    }
    memset(vm->memory, 0, memory_size);

    size_t fb_base = FB_BASE(memory_size);

    vm->fb = malloc(FB_SIZE);
    vm->fb_front = malloc(FB_SIZE);
    if (!vm->fb || !vm->fb_front) {
        free(vm->fb_front);
        free(vm->fb);
        free(vm->memory);
        free(vm->interrupt_enable_bitmap);
        free(vm->interrupt_bitmap);
        free(vm->core_released);
        free(vm->cpus);
        free(vm);
        vm_error("Couldn't allocate VM frame buffer");
        return NULL;
    }
    memset(vm->fb, 0, FB_SIZE);
    memset(vm->fb_front, 0, FB_SIZE);
    for (size_t row = 0; row < FB_HEIGHT; row++) {
        if (pthread_mutex_init(&vm->fb_row_locks[row], NULL) != 0) {
            for (size_t done = 0; done < row; done++) {
                pthread_mutex_destroy(&vm->fb_row_locks[done]);
            }
            free(vm->fb_front);
            free(vm->fb);
            free(vm->memory);
            free(vm->interrupt_enable_bitmap);
            free(vm->interrupt_bitmap);
            free(vm->core_released);
            free(vm->cpus);
            free(vm);
            vm_error("Couldn't allocate VM frame buffer row lock");
            return NULL;
        }
    }
    printf("vm->fb = %p\n", (void *) vm->fb);
    printf("fb_base = 0x%zx\n", fb_base);
    printf("fb address mod 4 = %zu\n", ((size_t) vm->fb) % 4);

    printf("Initializing MMIO.... \n");
    vm->mmio_count = 0;
    memset(vm->mmio_devices, 0, sizeof(vm->mmio_devices));
    vm->disk_size_bytes = DISK_SIZE;
    register_fb_mmio(vm);
    register_time_mmio(vm);
    register_intc_mmio(vm);
    register_iommu_mmio(vm);
    register_mmu_mmio(vm);
    register_sysinfo_mmio(vm);
    size_t prog_bytes = program_size * sizeof(uint64_t);
    uint32_t text_base = PROGRAM_BASE;
    uint32_t data_base = PROGRAM_BASE + (uint32_t) prog_bytes;
    uint32_t bss_base = data_base + (uint32_t) data_size;
    uint32_t bss_size = 0;

    if (layout) {
        text_base = layout->text_base;
        data_base = layout->data_base;
        bss_base = layout->bss_base;
        bss_size = layout->bss_size;
        if (layout->text_size != 0 && layout->text_size != prog_bytes) {
            printf("Warning: layout TEXT_SIZE (%u) != program size (%zu)\n",
                   layout->text_size, prog_bytes);
        }
    }

    if ((size_t) text_base + prog_bytes > memory_size) {
        panic("Program too large\n", vm);
        return vm;
    }

    const size_t call_stack_bytes = (size_t)CALL_STACK_SIZE * 8u;
    const size_t data_stack_bytes = (size_t)DATA_STACK_SIZE * 4u;
    const size_t isr_stack_bytes = (size_t)ISR_STACK_SIZE * 8u;
    const size_t per_core_stack_bytes = call_stack_bytes + data_stack_bytes + isr_stack_bytes;
    const size_t stack_slot_bytes = per_core_stack_bytes + (size_t)VM_TASK_C_STACK_BYTES;
    size_t stack_pool_slots = (size_t)vm->smp_cores;
    size_t image_end = (size_t)text_base + prog_bytes;
    if ((size_t)data_base + data_size > image_end)
        image_end = (size_t)data_base + data_size;
    if ((size_t)bss_base + bss_size > image_end)
        image_end = (size_t)bss_base + bss_size;

    if (stack_pool_slots < VM_STACK_POOL_SLOTS) {
        stack_pool_slots = VM_STACK_POOL_SLOTS;
    }
    vm->stack_pool_size = stack_slot_bytes * stack_pool_slots;
    if (vm->stack_pool_size >= memory_size) {
        panic("Stack pool too large for RAM", vm);
        return vm;
    }
    vm->stack_pool_base = (vm_addr_t)(memory_size - vm->stack_pool_size);
    if (image_end > vm->stack_pool_base) {
        panic("Program/data overlaps stack pool", vm);
        return vm;
    }

    if (vm->smp_cores == 1) {
        if ((size_t)PROGRAM_BASE > vm->stack_pool_base) {
            panic("Legacy core stack overlaps reserved stack pool", vm);
            return vm;
        }
    }

    memcpy(vm->memory + text_base, program, prog_bytes);
    if (data && data_size > 0) {
        if ((size_t) data_base + data_size > memory_size) {
            panic("Data segment out of range\n", vm);
            return vm;
        }
        memcpy(vm->memory + data_base, data, data_size);
    }
    if (bss_size > 0) {
        if ((size_t) bss_base + bss_size > memory_size) {
            panic("BSS segment out of range\n", vm);
            return vm;
        }
        memset(vm->memory + bss_base, 0, bss_size);
        {
            size_t sample = bss_size < 64 ? bss_size : 64;
            size_t bad = 0;
            for (size_t i = 0; i < sample; i++) {
                if (vm->memory[bss_base + i] != 0) {
                    bad++;
                }
            }
            printf("BSS clear: base=0x%08x size=%u first=%u last=%u bad_in_first_%zu=%zu\n",
                   bss_base,
                   bss_size,
                   vm->memory[bss_base],
                   vm->memory[bss_base + bss_size - 1],
                   sample,
                   bad);
        }
    }

    for (int i = 0; i < vm->smp_cores; i++) {
        vm_addr_t core_stack_base = (vm->smp_cores == 1)
            ? CALL_STACK_BASE
            : vm->stack_pool_base + (vm_addr_t)(stack_slot_bytes * (size_t)i);
        atomic_init(&vm->cpus[i].execution_times, 0);
        vm->cpus[i].core_id = i;
        vm->cpus[i].is_bsp = (i == 0) ? 1 : 0;
        vm->cpus[i].ip = text_base;
        vm->cpus[i].last_ip = text_base;
        vm->cpus[i].call_stack_base = core_stack_base;
        vm->cpus[i].data_stack_base = core_stack_base + (vm_addr_t)call_stack_bytes;
        vm->cpus[i].isr_stack_base = core_stack_base + (vm_addr_t)call_stack_bytes + (vm_addr_t)data_stack_bytes;
        vm->cpus[i].csp = CALL_STACK_SIZE;
        vm->cpus[i].dsp = DATA_STACK_SIZE;
        vm->cpus[i].isp = ISR_STACK_SIZE;
        vm->cpus[i].irq_masked = 0;
        atomic_init(&vm->core_released[i], (i == 0));
    }

    vm->start_realtime_ns = host_unix_time_ns();
    vm->start_monotonic_ns = host_monotonic_time_ns();
    vm->suspend_count = 0;
    vm->io[SCREEN_ATTRIBUTE] = SERIAL_STATUS_TX_READY;
    vm_debug_init(vm);
    return vm;
}

void vm_destroy(VM *vm) {
    if (!vm)
        return;

    if (vm->timer_thread_started) {
        atomic_store(&vm->timer_enabled, false);
        atomic_store(&vm->timer_thread_running, false);
        pthread_join(vm->timer_worker_thread, NULL);
        vm->timer_thread_started = 0;
    }

    vm_debug_destroy(vm);
    disk_close(vm);
    pthread_mutex_destroy(&vm->shared_lock);
    for (size_t row = 0; row < FB_HEIGHT; row++) {
        pthread_mutex_destroy(&vm->fb_row_locks[row]);
    }
    if (vm->interrupt_bitmap)
        free(vm->interrupt_bitmap);
    if (vm->interrupt_enable_bitmap)
        free(vm->interrupt_enable_bitmap);
    if (vm->core_released)
        free(vm->core_released);
    if (vm->cpus)
        free(vm->cpus);
    if (vm->memory)
        free(vm->memory);
    if (vm->fb)
        free(vm->fb);
    if (vm->fb_front)
        free(vm->fb_front);
    free(vm);
}

static void print_usage(const char *prog) {
    printf("Usage: %s [--bin <file>] [--smp <cores>] [--selftest] [--serial-stdin]\n", prog);
    printf("Defaults: --bin boot.bin --smp 1\n");
}

static int parse_positive_int(const char *s, int *out) {
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < 1 || v > 64)
        return 0;
    *out = (int)v;
    return 1;
}

int main(int argc, char **argv) {
    printf("Lamp VM version 1.0.0-rc1 \n");
#ifdef DEBUG_BUILD
    printf("This copy was built with debug flag. It may causing huge performance impact. \n");
#endif
    const char *filename = "boot.bin";
    int smp_cores = 1;
    int selftest = 0;
    int serial_stdin = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--bin") == 0) {
            if (i + 1 >= argc) {
                print_usage(argv[0]);
                return 1;
            }
            filename = argv[++i];
        } else if (strcmp(argv[i], "--smp") == 0) {
            if (i + 1 >= argc || !parse_positive_int(argv[i + 1], &smp_cores)) {
                printf("Invalid --smp value. Expected integer in [1, 64].\n");
                print_usage(argv[0]);
                return 1;
            }
            i++;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--selftest") == 0) {
            selftest = 1;
        } else if (strcmp(argv[i], "--serial-stdin") == 0) {
            serial_stdin = 1;
        } else {
            printf("Unknown argument: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }
    if (selftest) {
        return run_selftests();
    }

    size_t program_size = 0;
    size_t data_size = 0;
    uint64_t *program = NULL;
    uint8_t *data = NULL;
    ProgramLayout layout;

    if (!load_program_single(filename, &program, &program_size, &data, &data_size, &layout)) {
        printf("Failed to load program from %s\n", filename);
        return 1;
    }

    printf("Loaded program from %s, %zu instructions.\n", filename, program_size);
    printf("Loaded data: %zu bytes.\n", data_size);
    printf("Layout: TEXT_BASE=0x%08X TEXT_SIZE=%u DATA_BASE=0x%08X DATA_SIZE=%u BSS_BASE=0x%08X BSS_SIZE=%u\n",
           layout.text_base, layout.text_size,
           layout.data_base, layout.data_size,
           layout.bss_base, layout.bss_size);

    VM *vm = vm_create(MEM_SIZE, program, program_size, data, data_size, &layout, smp_cores);
    if (!vm) {
        printf("Failed to create VM.\n");
        free(program);
        free(data);
        return 1;
    }
    disk_init(vm, "./disk.img");
    init_ivt(vm);
    if (smp_cores > 1) {
        printf("SMP mode enabled: %d cores (per-core architectural state, shared memory).\n", smp_cores);
    }
    printf("Loaded VM. \n Call Stack size: %d\n Data Stack size: %d \n Memory Size: %lu\n Memory "
           "Head: %p\n",
           CALL_STACK_SIZE,
           DATA_STACK_SIZE,
           MEM_SIZE,
           (void *) vm->memory);
    init_screen();
    printf("VNC function is still in preview, use it with caution.\n");
    vnc_run(vm);
    if (serial_stdin) {
        printf("Serial stdin mode enabled (headless).\n");
        vm_run_serial(vm);
    } else {
        vm_run(vm);
    }
#ifdef DBEUG
    vm_dump(vm, 1024);
#endif

    uint64_t total_execution_times = 0;
    for (int i = 0; i < vm->smp_cores; i++) {
        total_execution_times += atomic_load_explicit(&vm->cpus[i].execution_times, memory_order_relaxed);
    }
    vnc_exit();
    printf("Execution complete in %lu cycles.\n",
           (unsigned long)total_execution_times);
    vm_debug_print_stats(vm);
    vm_destroy(vm);
    free(program);
    free(data);
    return 0;
}
