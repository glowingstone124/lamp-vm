#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "engine_internal.h"
#include "../debug.h"
#include "../float.h"
#include "../interrupt.h"
#include "../io.h"
#include "../io_devices/disk/disk.h"
#include "../memory.h"
#include "../stack.h"

volatile int g_vfork_trace_steps[32];
extern volatile int g_vfork_irq_probe_remaining[32];

static inline int vfork_irq_probe_active(const VCPU *cpu) {
    return cpu && cpu->core_id >= 0 && cpu->core_id < 32 && g_vfork_irq_probe_remaining[cpu->core_id] > 0;
}
static inline int vfork_irq_probe_addr(vm_addr_t addr) {
    return addr >= (vm_addr_t)0x00121f78u && addr < (vm_addr_t)0x001224d0u;
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
    if (!vm || int_no >= IVT_SIZE) {
        return;
    }
    vm_interrupt_eoi(vm, BSP_CORE, int_no);
}

void vm_engine_execute_decoded(VM *vm,
                               VCPU *cpu,
                               const VM_DecodedOp *decoded) {
    if (!cpu || !decoded)
        return;
    const uint8_t op = decoded->op;
    const uint8_t rd = decoded->rd;
    const uint8_t rs1 = decoded->rs1;
    const uint8_t rs2 = decoded->rs2;
    const int32_t imm = decoded->imm;
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
            const int32_t res = vm_engine_add_wrap32(a, b);
            cpu->regs[rd] = res;
            vm_engine_update_add_flags(vm, a, b, res, cpu);
            break;
        }

        case OP_SUB: {
            int32_t a = cpu->regs[rs1];
            int32_t b = cpu->regs[rs2];
            int32_t res = vm_engine_sub_wrap32(a, b);
            cpu->regs[rd] = res;
            vm_engine_update_sub_flags(vm, a, b, res, cpu);
            break;
        }
        case OP_MUL: {
            cpu->regs[rd] = vm_engine_mul_wrap32(cpu->regs[rs1],
                                                cpu->regs[rs2]);
            vm_engine_update_logic_flags(vm, cpu->regs[rd], cpu);
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
            cpu->ip = (size_t)vm_engine_rel_target_from_last_ip(cpu, imm);
            break;
        }
        case OP_PUSH: {
            data_push_cpu(vm, cpu, cpu->regs[rd]);
            break;
        }
        case OP_POP: {
            cpu->regs[rd] = data_pop_cpu(vm, cpu);
            vm_engine_update_logic_flags(vm, cpu->regs[rd],cpu);
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
            call_push_cpu(vm, cpu, (uint64_t)(vm_addr_t)cpu->ip);
            cpu->ip = (size_t)(vm_addr_t)imm;
            break;
        }
        case OP_RCALL: {
            const vm_addr_t target = vm_engine_rel_target_from_last_ip(cpu, imm);
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
            call_push_cpu(vm, cpu, (uint64_t)(vm_addr_t)cpu->ip);
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
            call_push_cpu(vm, cpu, (uint64_t)(vm_addr_t)cpu->ip);
            cpu->ip = (size_t)(vm_addr_t)cpu->regs[rd];
            break;
        }
        case OP_RET: {
            const vm_addr_t before_call_base = cpu->call_stack_base;
            const int before_csp = cpu->csp;
            const uint64_t ret_addr = call_pop_cpu(vm, cpu);
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
            cpu->regs[rd] = (uint32_t) vm_read8_cpu(vm, cpu, addr);
            vm_engine_update_zf_sf(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_LOAD16: {
            const vm_addr_t addr = cpu->regs[rs1] + imm;
            vm_engine_ensure_halfword_aligned_or_panic(vm, addr, "LOAD16");
            const uint16_t v = (uint16_t)((uint16_t)vm_read8_cpu(vm, cpu, addr) |
                                          ((uint16_t)vm_read8_cpu(vm, cpu, addr + 1) << 8));
            cpu->regs[rd] = (uint32_t)v;
            vm_engine_update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_LOAD32: {
            const vm_addr_t addr = cpu->regs[rs1] + imm;
            cpu->regs[rd] = vm_read32_cpu(vm, cpu, addr);
            vm_engine_update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_LOADS8: {
            const vm_addr_t addr = cpu->regs[rs1] + imm;
            cpu->regs[rd] = (int32_t)(int8_t)vm_read8_cpu(vm, cpu, addr);
            vm_engine_update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_LOADS16: {
            const vm_addr_t addr = cpu->regs[rs1] + imm;
            vm_engine_ensure_halfword_aligned_or_panic(vm, addr, "LOADS16");
            const uint16_t bits = (uint16_t)((uint16_t)vm_read8_cpu(vm, cpu, addr) |
                                             ((uint16_t)vm_read8_cpu(vm, cpu, addr + 1) << 8));
            cpu->regs[rd] = (int32_t)(int16_t)bits;
            vm_engine_update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_LOADX: {
            const vm_addr_t addr = cpu->regs[rs1] + cpu->regs[rs2] + imm;
            cpu->regs[rd] = (uint32_t)vm_read8_cpu(vm, cpu, addr);
            vm_engine_update_zf_sf(vm, cpu->regs[rd], cpu);
            break;
        }
        case OP_LOADX16: {
            const vm_addr_t addr = cpu->regs[rs1] + cpu->regs[rs2] + imm;
            vm_engine_ensure_halfword_aligned_or_panic(vm, addr, "LOADX16");
            const uint16_t v = (uint16_t)((uint16_t)vm_read8_cpu(vm, cpu, addr) |
                                          ((uint16_t)vm_read8_cpu(vm, cpu, addr + 1) << 8));
            cpu->regs[rd] = (uint32_t)v;
            vm_engine_update_logic_flags(vm, cpu->regs[rd], cpu);
            break;
        }
        case OP_LOADX32: {
            const vm_addr_t addr = cpu->regs[rs1] + cpu->regs[rs2] + imm;
            cpu->regs[rd] = vm_read32_cpu(vm, cpu, addr);
            vm_engine_update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_STORE: {
            const vm_addr_t addr = cpu->regs[rs1] + imm;
            vm_write8_cpu(vm, cpu, addr, (uint8_t) cpu->regs[rd]);
            break;
        }
        case OP_STORE16: {
            const vm_addr_t addr = cpu->regs[rs1] + imm;
            vm_engine_ensure_halfword_aligned_or_panic(vm, addr, "STORE16");
            const uint16_t v = (uint16_t)cpu->regs[rd];
            vm_write8_cpu(vm, cpu, addr, (uint8_t)(v & 0xFFu));
            vm_write8_cpu(vm, cpu, addr + 1, (uint8_t)(v >> 8));
            break;
        }
        case OP_STOREX: {
            const vm_addr_t addr = cpu->regs[rs1] + cpu->regs[rs2] + imm;
            vm_write8_cpu(vm, cpu, addr, (uint8_t)cpu->regs[rd]);
            break;
        }
        case OP_STOREX16: {
            const vm_addr_t addr = cpu->regs[rs1] + cpu->regs[rs2] + imm;
            vm_engine_ensure_halfword_aligned_or_panic(vm, addr, "STOREX16");
            const uint16_t v = (uint16_t)cpu->regs[rd];
            vm_write8_cpu(vm, cpu, addr, (uint8_t)(v & 0xFFu));
            vm_write8_cpu(vm, cpu, addr + 1, (uint8_t)(v >> 8));
            break;
        }
        case OP_STORE32: {
            const vm_addr_t addr = cpu->regs[rs1] + imm;
            vm_write32_cpu(vm, cpu, addr, (uint32_t) cpu->regs[rd]);
            break;
        }
        case OP_STOREX32: {
            const vm_addr_t addr = cpu->regs[rs1] + cpu->regs[rs2] + imm;
            vm_write32_cpu(vm, cpu, addr, (uint32_t) cpu->regs[rd]);
            break;
        }
        case OP_CMP: {
            const int32_t val1 = cpu->regs[rd];
            const int32_t val2 = cpu->regs[rs1];
            const int32_t res = vm_engine_sub_wrap32(val1, val2);
            vm_engine_update_sub_flags(vm, val1, val2, res,cpu);
            break;
        }
        case OP_CMPI: {
            const int32_t val1 = cpu->regs[rd];
            const int32_t val2 = imm;
            const int32_t res = vm_engine_sub_wrap32(val1, val2);
            vm_engine_update_sub_flags(vm, val1, val2, res,cpu);
            break;
        }
        case OP_MOV: {
            cpu->regs[rd] = cpu->regs[rs1];
            vm_engine_update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_MOVI: {
            cpu->regs[rd] = imm;
            vm_engine_update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_MEMSET: {
            const uint32_t base = (uint32_t) cpu->regs[rd];
            const uint8_t value = (uint8_t) cpu->regs[rs1];
            const uint32_t count = (uint32_t) imm;

            for (uint32_t i = 0; i < count; i++) {
                vm_write8_cpu(vm, cpu, base + i, value);
            }
            break;
        }
        case OP_MEMCPY: {
            const uint32_t dest = (uint32_t) cpu->regs[rd];
            const uint32_t src = (uint32_t) cpu->regs[rs1];
            const uint32_t count = (uint32_t) imm;

            for (uint32_t i = 0; i < count; i++) {
                uint8_t v = vm_read8_cpu(vm, cpu, src + i);
                vm_write8_cpu(vm, cpu, dest + i, v);
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
                if (addr == CPU_CTX_IN_INTERRUPT) {
                    cpu->regs[rd] = cpu->in_interrupt ? 1 : 0;
                    break;
                }
                vm_shared_lock(vm);
                if (addr == PS2_DATA) {
                    cpu->regs[rd] = vm_ps2_read_data(vm);
                } else if (addr == PS2_STATUS) {
                    cpu->regs[rd] = vm_ps2_read_status(vm);
                } else if (addr == KEYBOARD) {
                    int v = 0;
                    if (vm->serial_rx_tail != vm->serial_rx_head) {
                        v = (int)vm->serial_rx_fifo[vm->serial_rx_tail];
                        vm->serial_rx_tail = (uint16_t)((vm->serial_rx_tail + 1u) & 0xFFu);
                    }
                    if (getenv("LAMP_CONSOLE_TRACE")) {
                        fprintf(stderr, "[serial read] v=0x%02x head=%u tail=%u\n",
                                (unsigned)(v & 0xFF), (unsigned)vm->serial_rx_head,
                                (unsigned)vm->serial_rx_tail);
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
                } else if (addr == DISK_STATUS) {
                    cpu->regs[rd] = disk_read_status(vm);
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
                        if (getenv("LAMP_CTX_TRACE") &&
                            (cpu->last_ip >= (size_t)0x00100000u || cpu->in_interrupt)) {
                            fprintf(stderr,
                                    "[ctx-csp-out] core=%d last_ip=0x%08zx v=0x%08x old=0x%08x call_base=0x%08x r0=0x%08x r7=0x%08x r31=0x%08x in_interrupt=%d\n",
                                    cpu->core_id,
                                    cpu->last_ip,
                                    (uint32_t)v,
                                    (uint32_t)cpu->csp,
                                    (uint32_t)cpu->call_stack_base,
                                    (uint32_t)cpu->regs[0],
                                    (uint32_t)cpu->regs[7],
                                    (uint32_t)cpu->regs[31],
                                    cpu->in_interrupt ? 1 : 0);
                        }
                        cpu->csp = v;
                    } else {
                        panic(panic_format("OUT invalid csp %d (limit %u)", v, (unsigned)CALL_STACK_SIZE), vm);
                    }
                    break;
                }
                if (addr == CPU_CTX_DSP) {
                    const int v = cpu->regs[rd];
                    if (v >= 0 && v <= DATA_STACK_SIZE) {
                        cpu->dsp = v;
                    } else {
                        panic(panic_format("OUT invalid dsp %d (limit %u)", v, (unsigned)DATA_STACK_SIZE), vm);
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
                    } else {
                        panic(panic_format("OUT invalid isp %d (limit %u)", v, (unsigned)ISR_STACK_SIZE), vm);
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
                if (addr == CPU_CTX_IN_INTERRUPT) {
                    cpu->in_interrupt = (cpu->regs[rd] != 0);
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
            vm_enter_interrupt_cpu(vm, cpu, int_no);
            break;
        }
        case OP_INTI: {
            const uint32_t int_no = (uint32_t)imm;
            vm_enter_interrupt_cpu(vm, cpu, int_no);
            break;
        }
        case OP_IRET: {
            vm_iret_cpu(vm, cpu);
            break;
        }
        case OP_AND: {
            cpu->regs[rd] = cpu->regs[rs1] & cpu->regs[rs2];
            vm_engine_update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_OR: {
            cpu->regs[rd] = cpu->regs[rs1] | cpu->regs[rs2];
            vm_engine_update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_XOR: {
            cpu->regs[rd] = cpu->regs[rs1] ^ cpu->regs[rs2];
            vm_engine_update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_NOT: {
            cpu->regs[rd] = ~cpu->regs[rs1];
            vm_engine_update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_SHL: {
            uint32_t sh = (uint32_t)cpu->regs[rs2] & 31u;
            cpu->regs[rd] = (int32_t)((uint32_t)cpu->regs[rs1] << sh);
            vm_engine_update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_SHR: {
            uint32_t sh = (uint32_t)cpu->regs[rs2] & 31u;
            cpu->regs[rd] = (int32_t)((uint32_t)cpu->regs[rs1] >> sh);
            vm_engine_update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_SAR: {
            uint32_t sh = (uint32_t)cpu->regs[rs2] & 31u;
            cpu->regs[rd] = cpu->regs[rs1] >> sh;
            vm_engine_update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_ROL: {
            uint32_t sh = (uint32_t)cpu->regs[rs2] & 31u;
            cpu->regs[rd] = (int32_t)vm_engine_rotl32((uint32_t)cpu->regs[rs1], sh);
            vm_engine_update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_ROR: {
            uint32_t sh = (uint32_t)cpu->regs[rs2] & 31u;
            cpu->regs[rd] = (int32_t)vm_engine_rotr32((uint32_t)cpu->regs[rs1], sh);
            vm_engine_update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_DIV: {
            if (cpu->regs[rs2] != 0) {
                cpu->regs[rd] = cpu->regs[rs1] / cpu->regs[rs2];
                vm_engine_update_logic_flags(vm, cpu->regs[rd],cpu);
            } else {
                trigger_interrupt(vm, INT_DIVIDE_BY_ZERO);
            }
            break;
        }
        case OP_MOD: {
            if (cpu->regs[rs2] != 0) {
                cpu->regs[rd] = cpu->regs[rs1] % cpu->regs[rs2];
                vm_engine_update_logic_flags(vm, cpu->regs[rd],cpu);
            } else {
                trigger_interrupt(vm, INT_DIVIDE_BY_ZERO);
            }
            break;
        }
        case OP_INC: {
            const int32_t a = cpu->regs[rd];
            const int32_t b = 1;
            const int32_t res = vm_engine_add_wrap32(a, b);
            cpu->regs[rd] = res;
            vm_engine_update_add_flags(vm, a, b, res,cpu);
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
                cpu->ip = (size_t)vm_engine_rel_target_from_last_ip(cpu, imm);
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
                cpu->ip = (size_t)vm_engine_rel_target_from_last_ip(cpu, imm);
            }
            break;
        }
        case OP_RJG: {
            if (!(cpu->flags & FLAG_ZF) && ((cpu->flags & FLAG_SF) == (cpu->flags & FLAG_OF))) {
                cpu->ip = (size_t)vm_engine_rel_target_from_last_ip(cpu, imm);
            }
            break;
        }
        case OP_RJGE: {
            if ((cpu->flags & FLAG_SF) == (cpu->flags & FLAG_OF)) {
                cpu->ip = (size_t)vm_engine_rel_target_from_last_ip(cpu, imm);
            }
            break;
        }
        case OP_RJL: {
            if ((cpu->flags & FLAG_SF) != (cpu->flags & FLAG_OF)) {
                cpu->ip = (size_t)vm_engine_rel_target_from_last_ip(cpu, imm);
            }
            break;
        }
        case OP_RJLE: {
            if ((cpu->flags & FLAG_ZF) || (cpu->flags & FLAG_SF) != (cpu->flags & FLAG_OF)) {
                cpu->ip = (size_t)vm_engine_rel_target_from_last_ip(cpu, imm);
            }
            break;
        }
        case OP_RJC: {
            if (cpu->flags & FLAG_CF) {
                cpu->ip = (size_t)vm_engine_rel_target_from_last_ip(cpu, imm);
            }
            break;
        }
        case OP_RJNC: {
            if (!(cpu->flags & FLAG_CF)) {
                cpu->ip = (size_t)vm_engine_rel_target_from_last_ip(cpu, imm);
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
            vm_engine_update_logic_flags(vm, (r == 0.0f) ? 0 : (r < 0.0f ? -1 : 1),cpu);
            break;
        }
        case OP_FSUB: {
            float a = reg_as_f32(cpu->regs[rs1]);
            float b = reg_as_f32(cpu->regs[rs2]);
            float r = a - b;
            cpu->regs[rd] = f32_as_reg(r);
            vm_engine_update_logic_flags(vm, (r == 0.0f) ? 0 : (r < 0.0f ? -1 : 1),cpu);
            break;
        }
        case OP_FMUL: {
            float a = reg_as_f32(cpu->regs[rs1]);
            float b = reg_as_f32(cpu->regs[rs2]);
            float r = a * b;
            cpu->regs[rd] = f32_as_reg(r);
            vm_engine_update_logic_flags(vm, (r == 0.0f) ? 0 : (r < 0.0f ? -1 : 1),cpu);
            break;
        }
        case OP_FDIV: {
            float a = reg_as_f32(cpu->regs[rs1]);
            float b = reg_as_f32(cpu->regs[rs2]);
            float r = a / b;
            cpu->regs[rd] = f32_as_reg(r);
            vm_engine_update_logic_flags(vm, (r == 0.0f) ? 0 : (r < 0.0f ? -1 : 1),cpu);
            break;
        }
        case OP_FNEG: {
            float a = reg_as_f32(cpu->regs[rs1]);
            float r = -a;
            cpu->regs[rd] = f32_as_reg(r);
            vm_engine_update_logic_flags(vm, (r == 0.0f) ? 0 : (r < 0.0f ? -1 : 1),cpu);
            break;
        }
        case OP_FABS: {
            float a = reg_as_f32(cpu->regs[rs1]);
            float r = fabsf(a);
            cpu->regs[rd] = f32_as_reg(r);
            vm_engine_update_logic_flags(vm, (r == 0.0f) ? 0 : 1,cpu);
            break;
        }
        case OP_FSQRT: {
            float a = reg_as_f32(cpu->regs[rs1]);
            float r = sqrtf(a);
            cpu->regs[rd] = f32_as_reg(r);
            vm_engine_update_logic_flags(vm, (r == 0.0f) ? 0 : (r < 0.0f ? -1 : 1),cpu);
            break;
        }
        case OP_ITOF: {
            int32_t i = cpu->regs[rs1];
            float f = (float) i;
            cpu->regs[rd] = f32_as_reg(f);
            vm_engine_update_logic_flags(vm, (f == 0.0f) ? 0 : (f < 0.0f ? -1 : 1),cpu);
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
                vm_engine_update_logic_flags(vm, i,cpu);
            }
            break;
        }

        case OP_FLOAD32: {
            const vm_addr_t addr = cpu->regs[rs1] + imm;
            uint32_t bits = (uint32_t) vm_read32_cpu(vm, cpu, addr);
            cpu->regs[rd] = (int32_t) bits;
            float f = reg_as_f32(cpu->regs[rd]);
            vm_engine_update_logic_flags(vm, (f == 0.0f) ? 0 : (f < 0.0f ? -1 : 1),cpu);
            break;
        }
        case OP_FSTORE32: {
            const vm_addr_t addr = cpu->regs[rs1] + imm;
            vm_write32_cpu(vm, cpu, addr, (uint32_t) cpu->regs[rd]);
            break;
        }
        case OP_FCMP: {
            float a = reg_as_f32(cpu->regs[rd]);
            float b = reg_as_f32(cpu->regs[rs1]);
            update_fcmp_flags_cpu(cpu, a, b);
            break;
        }
        case OP_ADDI: {
            const int32_t a = cpu->regs[rs1];
            const int32_t b = imm;
            const int32_t res = vm_engine_add_wrap32(a, b);
            cpu->regs[rd] = res;
            vm_engine_update_add_flags(vm, a, b, res,cpu);
            break;
        }
        case OP_SUBI: {
            const int32_t a = cpu->regs[rs1];
            const int32_t b = imm;
            const int32_t res = vm_engine_sub_wrap32(a, b);
            cpu->regs[rd] = res;
            vm_engine_update_sub_flags(vm, a, b, res,cpu);
            break;
        }
        case OP_ANDI: {
            cpu->regs[rd] = cpu->regs[rs1] & imm;
            vm_engine_update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_ORI: {
            cpu->regs[rd] = cpu->regs[rs1] | imm;
            vm_engine_update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_XORI: {
            cpu->regs[rd] = cpu->regs[rs1] ^ imm;
            vm_engine_update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_SHLI: {
            uint32_t sh = (uint32_t)imm & 31u;
            cpu->regs[rd] = (int32_t)((uint32_t)cpu->regs[rs1] << sh);
            vm_engine_update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_SHRI: {
            uint32_t sh = (uint32_t)imm & 31u;
            cpu->regs[rd] = (int32_t)((uint32_t)cpu->regs[rs1] >> sh);
            vm_engine_update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_ROLI: {
            uint32_t sh = (uint32_t)imm & 31u;
            cpu->regs[rd] = (int32_t)vm_engine_rotl32((uint32_t)cpu->regs[rs1], sh);
            vm_engine_update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_RORI: {
            uint32_t sh = (uint32_t)imm & 31u;
            cpu->regs[rd] = (int32_t)vm_engine_rotr32((uint32_t)cpu->regs[rs1], sh);
            vm_engine_update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_CAS: {
            const vm_addr_t addr = cpu->regs[rs1] + imm;
            vm_engine_ensure_atomic_aligned_or_panic(vm, addr, "CAS");
            const uint32_t expected = (uint32_t)cpu->regs[rd];
            const uint32_t desired = (uint32_t)cpu->regs[rs2];
            int success = 0;
            const uint32_t old = vm_atomic_compare_exchange32_seqcst_cpu(vm, cpu, addr, expected, desired, &success);
            vm_engine_set_cas_flags(vm, success,cpu);
            cpu->regs[rd] = (int32_t)old;
            break;
        }
        case OP_XADD: {
            const vm_addr_t addr = cpu->regs[rs1] + imm;
            vm_engine_ensure_atomic_aligned_or_panic(vm, addr, "XADD");
            const uint32_t addend = (uint32_t)cpu->regs[rs2];
            const uint32_t old = vm_atomic_fetch_add32_seqcst_cpu(vm, cpu, addr, addend);
            const uint32_t newv = old + addend;
            cpu->regs[rd] = (int32_t)old;
            vm_engine_update_add_flags(vm, (int32_t)old, (int32_t)addend, (int32_t)newv,cpu);
            break;
        }
        case OP_XCHG: {
            const vm_addr_t addr = cpu->regs[rs1] + imm;
            vm_engine_ensure_atomic_aligned_or_panic(vm, addr, "XCHG");
            const uint32_t newv = (uint32_t)cpu->regs[rs2];
            const uint32_t old = vm_atomic_exchange32_seqcst_cpu(vm, cpu, addr, newv);
            cpu->regs[rd] = (int32_t)old;
            vm_engine_update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_LDAR: {
            const vm_addr_t addr = cpu->regs[rs1] + imm;
            vm_engine_ensure_atomic_aligned_or_panic(vm, addr, "LDAR");
            const uint32_t v = vm_atomic_load32_acquire_cpu(vm, cpu, addr);
            cpu->regs[rd] = (int32_t)v;
            vm_engine_update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        case OP_STLR: {
            const vm_addr_t addr = cpu->regs[rs1] + imm;
            vm_engine_ensure_atomic_aligned_or_panic(vm, addr, "STLR");
            const uint32_t v = (uint32_t)cpu->regs[rd];
            vm_atomic_store32_release_cpu(vm, cpu, addr, v);
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
            vm_engine_host_cpu_relax();
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
            vm_engine_update_logic_flags(vm, cpu->regs[rd],cpu);
            break;
        }
        default: {
            panic(panic_format("Unknown opcode %d\n", op), vm);
            return;
        }
    }
}
