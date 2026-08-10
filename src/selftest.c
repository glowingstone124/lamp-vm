#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "selftest.h"
#include "vm.h"
#include "vm_runtime.h"
#include "flags.h"
#include "engines/engine.h"
#include "engines/jit/code_memory.h"
#include "loadbin.h"
#include "interrupt.h"
#include "memory.h"
#include "io.h"
#include "mmio.h"
#include "io_devices/disk/disk.h"
#include "io_devices/audio/audio.h"
#include "io_devices/ether/ether.h"
#include "io_devices/ether/ether_backend.h"
#include "io_devices/iommu/iommu_mmio_register.h"
#include "io_devices/mmu/mmu_mmio_register.h"
#include "io_devices/pcie/pcie.h"
#include "runtime_stats.h"

static VmExecutionEngine g_selftest_engine = VM_ENGINE_CLASSIC;

static VM *selftest_vm_create(size_t memory_size,
                              const uint64_t *program,
                              size_t program_size,
                              const uint8_t *data,
                              size_t data_size,
                              const ProgramLayout *layout,
                              int smp_cores) {
    VM *vm = vm_create(memory_size, program, program_size,
                       data, data_size, layout, smp_cores);
    if (vm) {
        vm->execution_engine = g_selftest_engine;
    }
    return vm;
}

#define vm_create selftest_vm_create

static int run_selftest_startap_cpuid(void) {
    const vm_addr_t flag_addr = 0x3000;
    const vm_addr_t ap_entry = PROGRAM_BASE + 11 * 8;
    uint64_t program[] = {
        /* BSP */
        INST(OP_MOVI, 1, 0, 0, 1),                    /* r1 = target core */
        INST(OP_MOVI, 2, 0, 0, ap_entry),             /* r2 = ap entry */
        INST(OP_STARTAP, 1, 2, 0, 0),                 /* start AP1 */
        INST(OP_MOVI, 4, 0, 0, flag_addr),            /* r4 = flag addr */
        INST(OP_LOAD32, 3, 4, 0, 0),                  /* r3 = *flag */
        INST(OP_CMPI, 3, 0, 0, 1),                    /* r3 == 1 ? */
        INST(OP_JNZ, 0, 0, 0, PROGRAM_BASE + 4 * 8),  /* loop */
        INST(OP_HALT, 0, 0, 0, 0),
        INST(OP_PAUSE, 0, 0, 0, 0),
        INST(OP_PAUSE, 0, 0, 0, 0),
        INST(OP_PAUSE, 0, 0, 0, 0),
        /* AP entry */
        INST(OP_CPUID, 5, 0, 0, 0),                   /* r5 = core_id */
        INST(OP_MOVI, 6, 0, 0, flag_addr),
        INST(OP_STORE32, 5, 6, 0, 0),                 /* *flag = core_id */
        INST(OP_PAUSE, 0, 0, 0, 0),
        INST(OP_JMP, 0, 0, 0, ap_entry + 3 * 8),
    };
    VM *vm = vm_create(MEM_SIZE, program, sizeof(program) / sizeof(program[0]), NULL, 0, NULL, 2);
    if (!vm)
        return 0;
    disk_init(vm, "./disk.img");
    init_ivt(vm);
    int ok = vm_run_headless(vm, 2000);
    uint32_t v = vm_read32(vm, flag_addr);
    ok = ok && (v == 1);
    vm_destroy(vm);
    return ok;
}

static int run_selftest_ipi(void) {
    const vm_addr_t ready_addr = 0x3010;
    const vm_addr_t ipi_addr = 0x3014;
    const vm_addr_t ap_entry = PROGRAM_BASE + 14 * 8;
    const vm_addr_t isr_entry = PROGRAM_BASE + 20 * 8;
    uint64_t program[] = {
        /* BSP */
        INST(OP_MOVI, 1, 0, 0, 1),
        INST(OP_MOVI, 2, 0, 0, ap_entry),
        INST(OP_STARTAP, 1, 2, 0, 0),
        INST(OP_MOVI, 10, 0, 0, ready_addr),
        INST(OP_LOAD32, 11, 10, 0, 0),
        INST(OP_CMPI, 11, 0, 0, 1),
        INST(OP_JNZ, 0, 0, 0, PROGRAM_BASE + 4 * 8),
        INST(OP_MOVI, 12, 0, 0, 5),                   /* vector=5 */
        INST(OP_IPI, 1, 12, 0, 0),                    /* send IPI to core1 */
        INST(OP_MOVI, 13, 0, 0, ipi_addr),
        INST(OP_LOAD32, 14, 13, 0, 0),
        INST(OP_CMPI, 14, 0, 0, 1),
        INST(OP_JNZ, 0, 0, 0, PROGRAM_BASE + 10 * 8),
        INST(OP_HALT, 0, 0, 0, 0),
        /* AP entry */
        INST(OP_MOVI, 6, 0, 0, ready_addr),
        INST(OP_MOVI, 7, 0, 0, 1),
        INST(OP_STORE32, 7, 6, 0, 0),
        INST(OP_PAUSE, 0, 0, 0, 0),
        INST(OP_JMP, 0, 0, 0, ap_entry + 3 * 8),
        INST(OP_PAUSE, 0, 0, 0, 0),
        /* ISR(vector=5) */
        INST(OP_MOVI, 8, 0, 0, ipi_addr),
        INST(OP_MOVI, 9, 0, 0, 1),
        INST(OP_STORE32, 9, 8, 0, 0),
        INST(OP_IRET, 0, 0, 0, 0),
    };

    VM *vm = vm_create(MEM_SIZE, program, sizeof(program) / sizeof(program[0]), NULL, 0, NULL, 2);
    if (!vm)
        return 0;
    disk_init(vm, "./disk.img");
    init_ivt(vm);
    register_isr(vm, 5, isr_entry);
    int ok = vm_run_headless(vm, 2500);
    uint32_t ipi = vm_read32(vm, ipi_addr);
    ok = ok && (ipi == 1);
    vm_destroy(vm);
    return ok;
}

static int run_selftest_mmu_percpu_root(void) {
    const vm_addr_t bsp_root_addr = 0x3030;
    const vm_addr_t ap_root_addr = 0x3034;
    const vm_addr_t ap_done_addr = 0x3038;
    const vm_addr_t ap_entry = PROGRAM_BASE + 18 * 8;
    uint64_t program[] = {
        INST(OP_MOVI, 1, 0, 0, 1),                             /* target core */
        INST(OP_MOVI, 2, 0, 0, ap_entry),
        INST(OP_STARTAP, 1, 2, 0, 0),
        INST(OP_MOVI, 10, 0, 0, MMU_BASE + MMU_REG_ROOT_LO),   /* MMU root lo */
        INST(OP_MOVI, 11, 0, 0, 0x00111000),
        INST(OP_STORE32, 11, 10, 0, 0),                        /* BSP writes root */
        INST(OP_MOVI, 12, 0, 0, ap_done_addr),
        INST(OP_LOAD32, 13, 12, 0, 0),
        INST(OP_CMPI, 13, 0, 0, 1),
        INST(OP_JNZ, 0, 0, 0, PROGRAM_BASE + 7 * 8),           /* wait AP done */
        INST(OP_LOAD32, 14, 10, 0, 0),                         /* read BSP root */
        INST(OP_MOVI, 15, 0, 0, bsp_root_addr),
        INST(OP_STORE32, 14, 15, 0, 0),
        INST(OP_HALT, 0, 0, 0, 0),
        INST(OP_PAUSE, 0, 0, 0, 0),
        INST(OP_PAUSE, 0, 0, 0, 0),
        INST(OP_PAUSE, 0, 0, 0, 0),
        INST(OP_PAUSE, 0, 0, 0, 0),
        /* AP entry */
        INST(OP_MOVI, 20, 0, 0, MMU_BASE + MMU_REG_ROOT_LO),
        INST(OP_MOVI, 21, 0, 0, 0x00222000),
        INST(OP_STORE32, 21, 20, 0, 0),                        /* AP writes root */
        INST(OP_LOAD32, 22, 20, 0, 0),                         /* AP reads back */
        INST(OP_MOVI, 23, 0, 0, ap_root_addr),
        INST(OP_STORE32, 22, 23, 0, 0),
        INST(OP_MOVI, 24, 0, 0, ap_done_addr),
        INST(OP_MOVI, 25, 0, 0, 1),
        INST(OP_STORE32, 25, 24, 0, 0),
        INST(OP_PAUSE, 0, 0, 0, 0),
        INST(OP_JMP, 0, 0, 0, ap_entry + 9 * 8),
    };

    VM *vm = vm_create(MEM_SIZE, program, sizeof(program) / sizeof(program[0]), NULL, 0, NULL, 2);
    if (!vm) {
        return 0;
    }
    disk_init(vm, "./disk.img");
    init_ivt(vm);
    int ok = vm_run_headless(vm, 2500);
    uint32_t bsp_root = vm_read32(vm, bsp_root_addr);
    uint32_t ap_root = vm_read32(vm, ap_root_addr);
    ok = ok && (bsp_root == 0x00111000u) && (ap_root == 0x00222000u);
    vm_destroy(vm);
    return ok;
}

static int run_selftest_mmu_global_tlb_flush(void) {
    enum {
        root_pa = 0x4000u,
        l2_pa = 0x5000u,
        first_pa = 0x6000u,
        second_pa = 0x7000u,
        va = 0x00123040u,
    };
    const uint32_t pde = (va >> 22) & 0x3FFu;
    const uint32_t pte = (va >> 12) & 0x3FFu;
    const uint32_t page_offset = va & 0xFFFu;
    const uint32_t perms = MMU_PTE_P | MMU_PTE_W | MMU_PTE_X;
    uint64_t program[] = {
        INST(OP_HALT, 0, 0, 0, 0),
    };
    VM *vm = vm_create(MEM_SIZE, program,
                       sizeof(program) / sizeof(program[0]),
                       NULL, 0, NULL, 2);
    uint32_t pa0 = 0u;
    uint32_t pa1 = 0u;
    int ok;

    if (!vm) {
        return 0;
    }
    store_le32(&vm->memory[root_pa + pde * 4u], l2_pa | perms);
    store_le32(&vm->memory[l2_pa + pte * 4u], first_pa | perms);
    for (int core = 0; core < 2; core++) {
        vm->mmu.root[core] = root_pa;
        vm->mmu.ctrl[core] = MMU_CTRL_ENABLE;
        vm_mmu_flush_tlb(vm, (uint32_t)core);
    }

    ok = vm_mmu_translate_access_cpu(vm, &vm->cpus[0], va,
                                     VM_MMU_ACC_READ, &pa0) &&
         vm_mmu_translate_access_cpu(vm, &vm->cpus[1], va,
                                     VM_MMU_ACC_READ, &pa1) &&
         pa0 == first_pa + page_offset &&
         pa1 == first_pa + page_offset;

    store_le32(&vm->memory[l2_pa + pte * 4u], second_pa | perms);
    ok = ok && vm_mmu_translate_access_cpu(vm, &vm->cpus[0], va,
                                           VM_MMU_ACC_READ, &pa0) &&
         vm_mmu_translate_access_cpu(vm, &vm->cpus[1], va,
                                     VM_MMU_ACC_READ, &pa1) &&
         pa0 == first_pa + page_offset &&
         pa1 == first_pa + page_offset;

    vm_mmu_flush_all_tlbs(vm);
    ok = ok && vm_mmu_translate_access_cpu(vm, &vm->cpus[0], va,
                                           VM_MMU_ACC_READ, &pa0) &&
         vm_mmu_translate_access_cpu(vm, &vm->cpus[1], va,
                                     VM_MMU_ACC_READ, &pa1) &&
         pa0 == second_pa + page_offset &&
         pa1 == second_pa + page_offset;
    vm_destroy(vm);
    return ok;
}

static int run_selftest_fetch_invalidation(VmExecutionEngine engine) {
    enum {
        root_pa = 0x4000u,
        l2_pa = 0x5000u,
        first_pa = 0x6000u,
        second_pa = 0x7000u,
        va = 0x00123000u,
    };
    const uint32_t pde = (va >> 22) & 0x3FFu;
    const uint32_t pte = (va >> 12) & 0x3FFu;
    const uint32_t perms = MMU_PTE_P | MMU_PTE_W | MMU_PTE_X;
    uint64_t program[] = {
        INST(OP_HALT, 0, 0, 0, 0),
    };
    VM *vm = vm_create(MEM_SIZE, program,
                       sizeof(program) / sizeof(program[0]),
                       NULL, 0, NULL, 1);
    uint64_t first_inst = INST(OP_MOVI, 1, 0, 0, 1);
    uint64_t second_inst = INST(OP_MOVI, 1, 0, 0, 2);
    int ok;

    if (!vm) {
        return 0;
    }
    vm->execution_engine = engine;
    store_le64(&vm->memory[first_pa], first_inst);
    store_le64(&vm->memory[second_pa], second_inst);
    store_le32(&vm->memory[root_pa + pde * 4u], l2_pa | perms);
    store_le32(&vm->memory[l2_pa + pte * 4u], first_pa | perms);
    vm->mmu.root[0] = root_pa;
    vm->mmu.ctrl[0] = MMU_CTRL_ENABLE;
    vm_mmu_flush_tlb(vm, 0u);
    vm->cpus[0].ip = va;
    (void)vm_engine_execute_quantum(vm, &vm->cpus[0]);
    ok = vm->cpus[0].regs[1] == 1u;

    /* Changing the instruction bytes at the same physical address must be
     * observed even without a mapping change. */
    store_le64(&vm->memory[first_pa], second_inst);
    vm->cpus[0].ip = va;
    (void)vm_engine_execute_quantum(vm, &vm->cpus[0]);
    ok = ok && vm->cpus[0].regs[1] == 2u;

    /* Restoring the old bytes through a different mapping must not reuse the
     * cached host address after a global MMU epoch change. */
    store_le64(&vm->memory[first_pa], first_inst);
    store_le32(&vm->memory[l2_pa + pte * 4u], second_pa | perms);
    vm_mmu_flush_all_tlbs(vm);
    vm->cpus[0].ip = va;
    (void)vm_engine_execute_quantum(vm, &vm->cpus[0]);
    ok = ok && vm->cpus[0].regs[1] == 2u;
    vm_destroy(vm);
    return ok;
}

static int run_selftest_iommu_paged_translation(void) {
    enum {
        root_pa = 0x4000u,
        l2_pa = 0x5000u,
        iova = 0x20000000u,
        pa0 = 0x7000u,
        pa1 = 0x9000u
    };
    uint64_t program[] = {
        INST(OP_HALT, 0, 0, 0, 0),
    };
    VM *vm = vm_create(MEM_SIZE, program, sizeof(program) / sizeof(program[0]), NULL, 0, NULL, 1);
    if (!vm) {
        return 0;
    }

    uint32_t pde = (iova >> 22) & 0x3FFu;
    uint32_t pte = (iova >> 12) & 0x3FFu;
    store_le32(&vm->memory[root_pa + pde * 4u], l2_pa | IOMMU_PTE_P);
    store_le32(&vm->memory[l2_pa + pte * 4u], pa0 | IOMMU_PTE_P | IOMMU_PTE_R | IOMMU_PTE_W);
    store_le32(&vm->memory[l2_pa + (pte + 1u) * 4u], pa1 | IOMMU_PTE_P | IOMMU_PTE_R | IOMMU_PTE_W);

    vm->iommu.ctrl = IOMMU_CTRL_ENABLE;
    vm->iommu.devices[IOMMU_DEV_DISK].ctrl = IOMMU_DEV_CTRL_ENABLE | IOMMU_DEV_CTRL_PAGED;
    vm->iommu.devices[IOMMU_DEV_DISK].root = root_pa;

    uint64_t out = 0u;
    int ok = vm_iommu_translate_dma(vm, IOMMU_DEV_DISK, iova + 0x20u, 64u, &out);
    ok = ok && (out == pa0 + 0x20u);

    out = 0u;
    ok = ok && !vm_iommu_translate_dma(vm, IOMMU_DEV_DISK, iova + 0xFF0u, 32u, &out);
    ok = ok && ((vm->iommu.fault_status >> IOMMU_FAULT_REASON_SHIFT) == IOMMU_FAULT_REASON_NONCONTIG);

    store_le32(&vm->memory[l2_pa + pte * 4u], pa0 | IOMMU_PTE_P | IOMMU_PTE_R);
    out = 0u;
    ok = ok && !vm_iommu_translate_dma_ex(vm, IOMMU_DEV_DISK, iova + 0x20u, 64u, IOMMU_DMA_WRITE, &out);
    ok = ok && ((vm->iommu.fault_status >> IOMMU_FAULT_REASON_SHIFT) == IOMMU_FAULT_REASON_PERM);

    vm_destroy(vm);
    return ok;
}

typedef struct {
    uint32_t scratch;
    uint32_t relocated_base;
    int relocated_called;
} PcieDemoDeviceState;

static uint32_t pcie_demo_bar_read32(VM *vm, PciFunction *f, uint32_t bar_index, uint32_t offset) {
    (void)vm;
    (void)bar_index;
    PcieDemoDeviceState *st = (PcieDemoDeviceState *)f->cookie;
    return (offset == 0u) ? st->scratch : 0u;
}

static void pcie_demo_bar_write32(VM *vm, PciFunction *f, uint32_t bar_index, uint32_t offset, uint32_t value) {
    (void)vm;
    (void)bar_index;
    PcieDemoDeviceState *st = (PcieDemoDeviceState *)f->cookie;
    if (offset == 0u) {
        st->scratch = value;
    }
}

static void pcie_demo_bar_relocated(VM *vm, PciFunction *f, uint32_t bar_index, uint32_t new_base) {
    (void)vm;
    (void)bar_index;
    PcieDemoDeviceState *st = (PcieDemoDeviceState *)f->cookie;
    st->relocated_base = new_base;
    st->relocated_called = 1;
}

static int run_selftest_pcie_enumeration(void) {
    uint64_t program[] = {
        INST(OP_HALT, 0, 0, 0, 0),
    };
    VM *vm = vm_create(MEM_SIZE, program, sizeof(program) / sizeof(program[0]), NULL, 0, NULL, 1);
    if (!vm) {
        return 0;
    }
    init_ivt(vm);

    int ok = (vm->pcie != NULL);

    /* Host bridge at 00:00.0 must already be enumerable. */
    uint32_t hb_id = vm_mmio_read32(vm, PCIE_ECAM_BASE + PCI_CFG_VENDOR_ID);
    ok = ok && ((hb_id & 0xFFFFu) == LAMP_PCI_VENDOR_ID) && ((hb_id >> 16) == 0x0001u);

    /* A high unpopulated slot must read back as all-1s. */
    const uint32_t empty_addr = PCIE_ECAM_BASE + (31u * PCI_ECAM_FUNC_COUNT) * PCI_ECAM_FUNC_SIZE;
    ok = ok && (vm_mmio_read32(vm, empty_addr + PCI_CFG_VENDOR_ID) == 0xFFFFFFFFu);

    /* Register a demo endpoint at 01:00.0 with one 4KB BAR, MSI, and a PCIe capability. */
    PcieDemoDeviceState demo_state;
    memset(&demo_state, 0, sizeof(demo_state));
    demo_state.scratch = 0xCAFEBABEu;

    PciFunction *fn = pci_register_function(vm, 1u, 0u, LAMP_PCI_VENDOR_ID, 0xBEEFu,
                                             PCI_CLASS_NETWORK, PCI_SUBCLASS_ETHERNET, 0x00u);
    ok = ok && (fn != NULL);
    if (!fn) {
        vm_destroy(vm);
        return ok;
    }

    pci_configure_bar(vm, fn, 0u, 0x1000u, 0u, 0u,
                       pcie_demo_bar_read32, pcie_demo_bar_write32, pcie_demo_bar_relocated, &demo_state);
    uint8_t msi_off = pci_add_msi_capability(fn);
    uint8_t exp_off = pci_add_express_capability(fn, 0x0u);
    ok = ok && (msi_off != 0u) && (exp_off != 0u);
    pci_set_irq_pin(fn, 1u, 0x2Bu); /* INTA#, legacy fallback vector */

    const uint32_t fn_addr = PCIE_ECAM_BASE + (1u * PCI_ECAM_FUNC_COUNT) * PCI_ECAM_FUNC_SIZE;

    uint32_t fn_id = vm_mmio_read32(vm, fn_addr + PCI_CFG_VENDOR_ID);
    ok = ok && ((fn_id & 0xFFFFu) == LAMP_PCI_VENDOR_ID) && ((fn_id >> 16) == 0xBEEFu);

    /* Status register must advertise a capability list once one has been added. */
    uint32_t status_cmd = vm_mmio_read32(vm, fn_addr + PCI_CFG_COMMAND);
    ok = ok && (((status_cmd >> 16) & PCI_STATUS_CAP_LIST) != 0u);

    /* BAR size probe: write all-1s, read back the size mask (type bits = 0 for a 32-bit non-prefetchable BAR). */
    vm_mmio_write32(vm, fn_addr + PCI_CFG_BAR0, 0xFFFFFFFFu);
    uint32_t bar_probe = vm_mmio_read32(vm, fn_addr + PCI_CFG_BAR0);
    ok = ok && (bar_probe == 0xFFFFF000u);

    /* Assign a real base; the BAR window must not be decoded until Command.MEM_ENABLE is set.
     * Chosen just above the ECAM window (0x00900000..0x009FFFFF) with margin below the
     * kernel vfork snapshot area (0x01000000), see the PCIE_ECAM_BASE placement note in pcie.h. */
    const uint32_t bar_base = 0x00A10000u;
    vm_mmio_write32(vm, fn_addr + PCI_CFG_BAR0, bar_base);
    uint32_t bar_val = vm_mmio_read32(vm, fn_addr + PCI_CFG_BAR0);
    ok = ok && (bar_val == bar_base);
    ok = ok && demo_state.relocated_called && (demo_state.relocated_base == bar_base);
    ok = ok && (find_mmio(vm, bar_base) == NULL);

    vm_mmio_write32(vm, fn_addr + PCI_CFG_COMMAND, PCI_COMMAND_MEM_ENABLE | PCI_COMMAND_BUS_MASTER);
    ok = ok && (find_mmio(vm, bar_base) != NULL);

    uint32_t scratch_read = vm_mmio_read32(vm, bar_base + 0u);
    ok = ok && (scratch_read == 0xCAFEBABEu);
    vm_mmio_write32(vm, bar_base + 0u, 0x12345678u);
    ok = ok && (demo_state.scratch == 0x12345678u);

    /* MSI: enable, program address (core 0) + data (vector 40), then have the "device" notify. */
    const uint32_t vector = 40u;
    uint32_t msi_ctrl = vm_mmio_read32(vm, fn_addr + msi_off);
    msi_ctrl |= (uint32_t)0x1u << 16; /* Message Control bit0 = MSI Enable */
    vm_mmio_write32(vm, fn_addr + msi_off, msi_ctrl);
    vm_mmio_write32(vm, fn_addr + msi_off + 0x4u, 0u); /* Message Address Low: core 0 */
    vm_mmio_write32(vm, fn_addr + msi_off + 0x8u, 0u); /* Message Address High */
    vm_mmio_write32(vm, fn_addr + msi_off + 0xCu, vector);

    pci_notify_irq(vm, fn);
    uint32_t reg_index = vector / 32u;
    uint32_t bit = vector % 32u;
    uint32_t pending = vm_interrupt_read_pending32(vm, 0, reg_index);
    ok = ok && (((pending >> bit) & 0x1u) != 0u);

    /* Disabling MSI must fall back to the legacy INTx vector. */
    msi_ctrl &= ~((uint32_t)0x1u << 16);
    vm_mmio_write32(vm, fn_addr + msi_off, msi_ctrl);
    pci_notify_irq(vm, fn);
    uint32_t legacy_pending = vm_interrupt_read_pending32(vm, 0, 0x2Bu / 32u);
    ok = ok && (((legacy_pending >> (0x2Bu % 32u)) & 0x1u) != 0u);

    /* PCI Express capability: Data Link Layer Active must read back set (no physical link to train). */
    uint32_t link_status_ctrl = vm_mmio_read32(vm, fn_addr + exp_off + 0x10u);
    ok = ok && (((link_status_ctrl >> 16) & (1u << 13)) != 0u);

    vm_destroy(vm);
    return ok;
}

static int run_selftest_relctrl(void) {
    const vm_addr_t flag_addr = 0x3020;
    uint64_t program[] = {
        INST(OP_MOVI, 10, 0, 0, flag_addr), /* r10 = flag addr */
        INST(OP_MOVI, 11, 0, 0, 0),         /* r11 = 0 */
        INST(OP_STORE32, 11, 10, 0, 0),     /* *flag = 0 */
        INST(OP_MOVI, 1, 0, 0, 0),          /* r1 = 0 */
        INST(OP_RJMP, 0, 0, 0, 16),         /* skip next insn */
        INST(OP_MOVI, 1, 0, 0, 111),        /* should not execute */
        INST(OP_RCALL, 0, 0, 0, 104),       /* call fn at idx 19 */
        INST(OP_CMPI, 1, 0, 0, 7),          /* ZF = 1 */
        INST(OP_RJZ, 0, 0, 0, 16),          /* go to idx 10 */
        INST(OP_RJMP, 0, 0, 0, 56),         /* fail */
        INST(OP_CMPI, 1, 0, 0, 8),          /* ZF = 0 */
        INST(OP_RJNZ, 0, 0, 0, 16),         /* go to idx 13 */
        INST(OP_RJMP, 0, 0, 0, 32),         /* fail */
        INST(OP_MOVI, 11, 0, 0, 1),         /* pass: *flag = 1 */
        INST(OP_STORE32, 11, 10, 0, 0),
        INST(OP_HALT, 0, 0, 0, 0),
        INST(OP_MOVI, 11, 0, 0, 2),         /* fail: *flag = 2 */
        INST(OP_STORE32, 11, 10, 0, 0),
        INST(OP_HALT, 0, 0, 0, 0),
        INST(OP_MOVI, 1, 0, 0, 7),          /* function body */
        INST(OP_RET, 0, 0, 0, 0),
    };

    VM *vm = vm_create(MEM_SIZE, program, sizeof(program) / sizeof(program[0]), NULL, 0, NULL, 1);
    if (!vm)
        return 0;
    disk_init(vm, "./disk.img");
    init_ivt(vm);
    int ok = vm_run_headless(vm, 1000);
    uint32_t flag = vm_read32(vm, flag_addr);
    ok = ok && (flag == 1);
    vm_destroy(vm);
    return ok;
}

static int run_selftest_zero_branch_flags(void) {
    const vm_addr_t flag_addr = 0x3024;
    const vm_addr_t fail_addr = PROGRAM_BASE + 16 * 8;
    uint64_t program[] = {
        INST(OP_MOVI, 10, 0, 0, flag_addr),  /* r10 = flag addr */
        INST(OP_MOVI, 11, 0, 0, 0),          /* *flag = 0 */
        INST(OP_STORE32, 11, 10, 0, 0),
        INST(OP_MOVI, 2, 0, 0, 123),         /* non-zero rs (must be ignored) */
        INST(OP_MOVI, 1, 0, 0, 7),
        INST(OP_CMPI, 1, 0, 0, 7),           /* ZF = 1 */
        INST(OP_RJZ, 2, 0, 0, 16),           /* go to idx 8 */
        INST(OP_JMP, 0, 0, 0, fail_addr),
        INST(OP_MOVI, 2, 0, 0, 0),           /* zero rs (must be ignored) */
        INST(OP_MOVI, 1, 0, 0, 7),
        INST(OP_CMPI, 1, 0, 0, 8),           /* ZF = 0 */
        INST(OP_RJNZ, 2, 0, 0, 16),          /* go to idx 13 */
        INST(OP_JMP, 0, 0, 0, fail_addr),
        INST(OP_MOVI, 11, 0, 0, 1),          /* pass: *flag = 1 */
        INST(OP_STORE32, 11, 10, 0, 0),
        INST(OP_HALT, 0, 0, 0, 0),
        INST(OP_MOVI, 11, 0, 0, 2),          /* fail: *flag = 2 */
        INST(OP_STORE32, 11, 10, 0, 0),
        INST(OP_HALT, 0, 0, 0, 0),
    };

    VM *vm = vm_create(MEM_SIZE, program, sizeof(program) / sizeof(program[0]), NULL, 0, NULL, 1);
    if (!vm)
        return 0;
    disk_init(vm, "./disk.img");
    init_ivt(vm);
    int ok = vm_run_headless(vm, 1000);
    uint32_t flag = vm_read32(vm, flag_addr);
    ok = ok && (flag == 1);
    vm_destroy(vm);
    return ok;
}

static int run_selftest_callr_unused_fields(void) {
    const vm_addr_t flag_addr = 0x3028;
    const vm_addr_t fail_addr = PROGRAM_BASE + 11 * 8;
    const vm_addr_t fn_addr = PROGRAM_BASE + 14 * 8;
    uint64_t program[] = {
        INST(OP_MOVI, 10, 0, 0, flag_addr),     /* r10 = flag addr */
        INST(OP_MOVI, 11, 0, 0, 0),             /* *flag = 0 */
        INST(OP_STORE32, 11, 10, 0, 0),
        INST(OP_MOVI, 20, 0, 0, fn_addr),       /* r20 = function address */
        INST(OP_MOVI, 1, 0, 0, 0),
        INST(OP_CALLR, 20, 7, 8, 123),          /* rs1/rs2/imm must be ignored */
        INST(OP_CMPI, 1, 0, 0, 42),             /* function result */
        INST(OP_JNZ, 0, 0, 0, fail_addr),
        INST(OP_MOVI, 11, 0, 0, 1),             /* pass */
        INST(OP_STORE32, 11, 10, 0, 0),
        INST(OP_HALT, 0, 0, 0, 0),
        INST(OP_MOVI, 11, 0, 0, 2),             /* fail */
        INST(OP_STORE32, 11, 10, 0, 0),
        INST(OP_HALT, 0, 0, 0, 0),
        INST(OP_MOVI, 1, 0, 0, 42),             /* function body */
        INST(OP_RET, 0, 0, 0, 0),
    };

    VM *vm = vm_create(MEM_SIZE, program, sizeof(program) / sizeof(program[0]), NULL, 0, NULL, 1);
    if (!vm)
        return 0;
    disk_init(vm, "./disk.img");
    init_ivt(vm);
    int ok = vm_run_headless(vm, 1000);
    uint32_t flag = vm_read32(vm, flag_addr);
    ok = ok && (flag == 1);
    vm_destroy(vm);
    return ok;
}

static int run_selftest_atomic_conformance(void) {
    const vm_addr_t flag_addr = 0x3040;
    const vm_addr_t word_addr = 0x3044;
    const vm_addr_t fail_addr = PROGRAM_BASE + 46 * 8;
    uint64_t program[] = {
        INST(OP_MOVI, 10, 0, 0, flag_addr),     /* r10 = flag addr */
        INST(OP_MOVI, 11, 0, 0, 0),             /* *flag = 0 */
        INST(OP_STORE32, 11, 10, 0, 0),
        INST(OP_MOVI, 12, 0, 0, word_addr),     /* r12 = atomic word addr */
        INST(OP_MOVI, 1, 0, 0, 10),
        INST(OP_STLR, 1, 12, 77, 0),            /* rs2 ignored */
        INST(OP_LDAR, 2, 12, 88, 0),            /* rs2 ignored */
        INST(OP_CMPI, 2, 0, 0, 10),
        INST(OP_JNZ, 0, 0, 0, fail_addr),
        INST(OP_MOVI, 3, 0, 0, 7),              /* addend */
        INST(OP_MOVI, 4, 0, 0, 0),              /* old value result */
        INST(OP_XADD, 4, 12, 3, 0),             /* old=10, mem=17 */
        INST(OP_CMPI, 4, 0, 0, 10),
        INST(OP_JNZ, 0, 0, 0, fail_addr),
        INST(OP_LDAR, 5, 12, 0, 0),
        INST(OP_CMPI, 5, 0, 0, 17),
        INST(OP_JNZ, 0, 0, 0, fail_addr),
        INST(OP_MOVI, 6, 0, 0, 99),
        INST(OP_MOVI, 7, 0, 0, 0),
        INST(OP_XCHG, 7, 12, 6, 0),             /* old=17, mem=99 */
        INST(OP_CMPI, 7, 0, 0, 17),
        INST(OP_JNZ, 0, 0, 0, fail_addr),
        INST(OP_LDAR, 8, 12, 0, 0),
        INST(OP_CMPI, 8, 0, 0, 99),
        INST(OP_JNZ, 0, 0, 0, fail_addr),
        INST(OP_MOVI, 9, 0, 0, 99),             /* expected */
        INST(OP_MOVI, 13, 0, 0, 123),           /* desired */
        INST(OP_CAS, 9, 12, 13, 0),             /* success => ZF=1 */
        INST(OP_JNZ, 0, 0, 0, fail_addr),       /* fail if ZF=0 */
        INST(OP_CMPI, 9, 0, 0, 99),             /* old value */
        INST(OP_JNZ, 0, 0, 0, fail_addr),
        INST(OP_LDAR, 14, 12, 0, 0),
        INST(OP_CMPI, 14, 0, 0, 123),
        INST(OP_JNZ, 0, 0, 0, fail_addr),
        INST(OP_MOVI, 15, 0, 0, 77),            /* wrong expected */
        INST(OP_MOVI, 16, 0, 0, 55),
        INST(OP_CAS, 15, 12, 16, 0),            /* failure => ZF=0 */
        INST(OP_JZ, 0, 0, 0, fail_addr),        /* fail if ZF=1 */
        INST(OP_CMPI, 15, 0, 0, 123),           /* observed old value */
        INST(OP_JNZ, 0, 0, 0, fail_addr),
        INST(OP_LDAR, 17, 12, 0, 0),
        INST(OP_CMPI, 17, 0, 0, 123),
        INST(OP_JNZ, 0, 0, 0, fail_addr),
        INST(OP_MOVI, 11, 0, 0, 1),             /* pass */
        INST(OP_STORE32, 11, 10, 0, 0),
        INST(OP_HALT, 0, 0, 0, 0),
        INST(OP_MOVI, 11, 0, 0, 2),             /* fail */
        INST(OP_STORE32, 11, 10, 0, 0),
        INST(OP_HALT, 0, 0, 0, 0),
    };

    VM *vm = vm_create(MEM_SIZE, program, sizeof(program) / sizeof(program[0]), NULL, 0, NULL, 1);
    if (!vm)
        return 0;
    disk_init(vm, "./disk.img");
    init_ivt(vm);
    int ok = vm_run_headless(vm, 1000);
    uint32_t flag = vm_read32(vm, flag_addr);
    uint32_t word = vm_read32(vm, word_addr);
    ok = ok && (flag == 1) && (word == 123u);
    vm_destroy(vm);
    return ok;
}

static int run_selftest_div0_interrupt_conformance(void) {
    const vm_addr_t flag_addr = 0x3050;
    const vm_addr_t vector_addr = 0x3054;
    const vm_addr_t count_addr = 0x3058;
    const vm_addr_t fail_addr = PROGRAM_BASE + 19 * 8;
    const vm_addr_t isr_entry = PROGRAM_BASE + 22 * 8;
    uint64_t program[] = {
        INST(OP_MOVI, 10, 0, 0, flag_addr),              /* *flag = 0 */
        INST(OP_MOVI, 11, 0, 0, 0),
        INST(OP_STORE32, 11, 10, 0, 0),
        INST(OP_MOVI, 12, 0, 0, vector_addr),            /* *vector = 0 */
        INST(OP_STORE32, 11, 12, 0, 0),
        INST(OP_MOVI, 13, 0, 0, count_addr),             /* *count = 0 */
        INST(OP_STORE32, 11, 13, 0, 0),
        INST(OP_MOVI, 1, 0, 0, 42),
        INST(OP_MOVI, 2, 0, 0, 0),
        INST(OP_DIV, 3, 1, 2, 0),                        /* triggers INT_DIVIDE_BY_ZERO */
        INST(OP_LOAD32, 4, 12, 0, 0),                    /* vector captured by ISR */
        INST(OP_CMPI, 4, 0, 0, INT_DIVIDE_BY_ZERO),
        INST(OP_JNZ, 0, 0, 0, fail_addr),
        INST(OP_LOAD32, 5, 13, 0, 0),                    /* ISR count */
        INST(OP_CMPI, 5, 0, 0, 1),
        INST(OP_JNZ, 0, 0, 0, fail_addr),
        INST(OP_MOVI, 11, 0, 0, 1),                      /* pass */
        INST(OP_STORE32, 11, 10, 0, 0),
        INST(OP_HALT, 0, 0, 0, 0),
        INST(OP_MOVI, 11, 0, 0, 2),                      /* fail */
        INST(OP_STORE32, 11, 10, 0, 0),
        INST(OP_HALT, 0, 0, 0, 0),
        INST(OP_MOVI, 20, 0, 0, vector_addr),            /* ISR: store r31 */
        INST(OP_STORE32, 31, 20, 0, 0),
        INST(OP_MOVI, 21, 0, 0, count_addr),             /* ISR: count++ */
        INST(OP_LOAD32, 22, 21, 0, 0),
        INST(OP_INC, 22, 0, 0, 0),
        INST(OP_STORE32, 22, 21, 0, 0),
        INST(OP_MOVI, 23, 0, 0, INTC_BASE + INTC_REG_EOI), /* ISR: EOI(INT_DIVIDE_BY_ZERO) */
        INST(OP_MOVI, 24, 0, 0, INT_DIVIDE_BY_ZERO),
        INST(OP_STORE32, 24, 23, 0, 0),
        INST(OP_IRET, 0, 0, 0, 0),
    };

    VM *vm = vm_create(MEM_SIZE, program, sizeof(program) / sizeof(program[0]), NULL, 0, NULL, 1);
    if (!vm)
        return 0;
    disk_init(vm, "./disk.img");
    init_ivt(vm);
    register_isr(vm, INT_DIVIDE_BY_ZERO, isr_entry);
    int ok = vm_run_headless(vm, 1000);
    uint32_t flag = vm_read32(vm, flag_addr);
    uint32_t vector = vm_read32(vm, vector_addr);
    uint32_t count = vm_read32(vm, count_addr);
    ok = ok && (flag == 1u) && (vector == (uint32_t)INT_DIVIDE_BY_ZERO) && (count == 1u);
    vm_destroy(vm);
    return ok;
}

static int run_selftest_load16_signext_conformance(void) {
    const vm_addr_t flag_addr = 0x3060;
    const vm_addr_t data_addr = 0x3068;
    const vm_addr_t fail_addr = PROGRAM_BASE + 25 * 8;
    uint64_t program[] = {
        INST(OP_MOVI, 10, 0, 0, flag_addr),
        INST(OP_MOVI, 11, 0, 0, 0),
        INST(OP_STORE32, 11, 10, 0, 0),
        INST(OP_MOVI, 12, 0, 0, data_addr),
        INST(OP_MOVI, 1, 0, 0, 0xABCD),
        INST(OP_STORE16, 1, 12, 0, 0),
        INST(OP_LOAD16, 2, 12, 0, 0),
        INST(OP_CMPI, 2, 0, 0, 0xABCD),
        INST(OP_JNZ, 0, 0, 0, fail_addr),
        INST(OP_LOAD, 3, 12, 0, 1),                 /* high byte should be 0xAB */
        INST(OP_CMPI, 3, 0, 0, 0xAB),
        INST(OP_JNZ, 0, 0, 0, fail_addr),
        INST(OP_MOVI, 4, 0, 0, 0x80),
        INST(OP_STORE, 4, 12, 0, 4),
        INST(OP_LOADS8, 5, 12, 0, 4),
        INST(OP_CMPI, 5, 0, 0, -128),
        INST(OP_JNZ, 0, 0, 0, fail_addr),
        INST(OP_MOVI, 6, 0, 0, 0x8001),
        INST(OP_STORE16, 6, 12, 0, 6),
        INST(OP_LOADS16, 7, 12, 0, 6),
        INST(OP_CMPI, 7, 0, 0, -32767),
        INST(OP_JNZ, 0, 0, 0, fail_addr),
        INST(OP_MOVI, 11, 0, 0, 1),
        INST(OP_STORE32, 11, 10, 0, 0),
        INST(OP_HALT, 0, 0, 0, 0),
        INST(OP_MOVI, 11, 0, 0, 2),
        INST(OP_STORE32, 11, 10, 0, 0),
        INST(OP_HALT, 0, 0, 0, 0),
    };

    VM *vm = vm_create(MEM_SIZE, program, sizeof(program) / sizeof(program[0]), NULL, 0, NULL, 1);
    if (!vm)
        return 0;
    disk_init(vm, "./disk.img");
    init_ivt(vm);
    int ok = vm_run_headless(vm, 1000);
    uint32_t flag = vm_read32(vm, flag_addr);
    uint32_t raw = vm_read32(vm, data_addr);
    ok = ok && (flag == 1u) && ((raw & 0xFFFFu) == 0xABCDu);
    vm_destroy(vm);
    return ok;
}

static int run_selftest_indexed_rw_width_conformance(void) {
    const vm_addr_t flag_addr = 0x3090;
    const vm_addr_t data_addr = 0x3098;
    const vm_addr_t fail_addr = PROGRAM_BASE + 29 * 8;
    uint64_t program[] = {
        INST(OP_MOVI, 10, 0, 0, flag_addr),
        INST(OP_MOVI, 11, 0, 0, 0),
        INST(OP_STORE32, 11, 10, 0, 0),
        INST(OP_MOVI, 12, 0, 0, data_addr),
        INST(OP_MOVI, 13, 0, 0, 4),
        INST(OP_MOVI, 1, 0, 0, 0xA5),
        INST(OP_STOREX, 1, 12, 13, 1),               /* data[5] = 0xA5 */
        INST(OP_LOADX, 2, 12, 13, 1),                /* r2 = data[5] */
        INST(OP_CMPI, 2, 0, 0, 0xA5),
        INST(OP_JNZ, 0, 0, 0, fail_addr),
        INST(OP_MOVI, 3, 0, 0, 0xBEEF),
        INST(OP_STOREX16, 3, 12, 13, 2),             /* *(u16*)(data+6) = 0xBEEF */
        INST(OP_LOADX16, 4, 12, 13, 2),
        INST(OP_CMPI, 4, 0, 0, 0xBEEF),
        INST(OP_JNZ, 0, 0, 0, fail_addr),
        INST(OP_LOAD, 5, 12, 0, 6),                  /* low byte of 0xBEEF */
        INST(OP_CMPI, 5, 0, 0, 0xEF),
        INST(OP_JNZ, 0, 0, 0, fail_addr),
        INST(OP_LOAD, 6, 12, 0, 7),                  /* high byte of 0xBEEF */
        INST(OP_CMPI, 6, 0, 0, 0xBE),
        INST(OP_JNZ, 0, 0, 0, fail_addr),
        INST(OP_MOVI, 7, 0, 0, 0x12345678),
        INST(OP_STOREX32, 7, 12, 13, 8),             /* *(u32*)(data+12) */
        INST(OP_LOADX32, 8, 12, 13, 8),
        INST(OP_CMPI, 8, 0, 0, 0x12345678),
        INST(OP_JNZ, 0, 0, 0, fail_addr),
        INST(OP_MOVI, 11, 0, 0, 1),
        INST(OP_STORE32, 11, 10, 0, 0),
        INST(OP_HALT, 0, 0, 0, 0),
        INST(OP_MOVI, 11, 0, 0, 2),
        INST(OP_STORE32, 11, 10, 0, 0),
        INST(OP_HALT, 0, 0, 0, 0),
    };

    VM *vm = vm_create(MEM_SIZE, program, sizeof(program) / sizeof(program[0]), NULL, 0, NULL, 1);
    if (!vm)
        return 0;
    disk_init(vm, "./disk.img");
    init_ivt(vm);
    int ok = vm_run_headless(vm, 1000);
    uint32_t flag = vm_read32(vm, flag_addr);
    uint8_t byte5 = vm_read8(vm, data_addr + 5);
    uint32_t word = vm_read32(vm, data_addr + 12);
    ok = ok && (flag == 1u) && (byte5 == 0xA5u) && (word == 0x12345678u);
    vm_destroy(vm);
    return ok;
}

static int run_selftest_relcond_extended_conformance(void) {
    const vm_addr_t flag_addr = 0x3070;
    const vm_addr_t fail_addr = PROGRAM_BASE + 26 * 8;
    uint64_t program[] = {
        INST(OP_MOVI, 10, 0, 0, flag_addr),
        INST(OP_MOVI, 11, 0, 0, 0),
        INST(OP_STORE32, 11, 10, 0, 0),

        INST(OP_MOVI, 1, 0, 0, 5),
        INST(OP_CMPI, 1, 0, 0, 3),                   /* > */
        INST(OP_RJG, 0, 0, 0, 16),
        INST(OP_JMP, 0, 0, 0, fail_addr),

        INST(OP_CMPI, 1, 0, 0, 5),                   /* >= */
        INST(OP_RJGE, 0, 0, 0, 16),
        INST(OP_JMP, 0, 0, 0, fail_addr),

        INST(OP_CMPI, 1, 0, 0, 6),                   /* < */
        INST(OP_RJL, 0, 0, 0, 16),
        INST(OP_JMP, 0, 0, 0, fail_addr),

        INST(OP_CMPI, 1, 0, 0, 5),                   /* <= */
        INST(OP_RJLE, 0, 0, 0, 16),
        INST(OP_JMP, 0, 0, 0, fail_addr),

        INST(OP_CMPI, 1, 0, 0, 6),                   /* CF=1 */
        INST(OP_RJC, 0, 0, 0, 16),
        INST(OP_JMP, 0, 0, 0, fail_addr),

        INST(OP_MOVI, 1, 0, 0, 6),
        INST(OP_CMPI, 1, 0, 0, 5),                   /* CF=0 */
        INST(OP_RJNC, 0, 0, 0, 16),
        INST(OP_JMP, 0, 0, 0, fail_addr),

        INST(OP_MOVI, 11, 0, 0, 1),
        INST(OP_STORE32, 11, 10, 0, 0),
        INST(OP_HALT, 0, 0, 0, 0),
        INST(OP_MOVI, 11, 0, 0, 2),
        INST(OP_STORE32, 11, 10, 0, 0),
        INST(OP_HALT, 0, 0, 0, 0),
    };

    VM *vm = vm_create(MEM_SIZE, program, sizeof(program) / sizeof(program[0]), NULL, 0, NULL, 1);
    if (!vm)
        return 0;
    disk_init(vm, "./disk.img");
    init_ivt(vm);
    int ok = vm_run_headless(vm, 1000);
    uint32_t flag = vm_read32(vm, flag_addr);
    ok = ok && (flag == 1u);
    vm_destroy(vm);
    return ok;
}

static int run_selftest_inti_imm_conformance(void) {
    const vm_addr_t flag_addr = 0x3080;
    const vm_addr_t vector_addr = 0x3084;
    const vm_addr_t count_addr = 0x3088;
    const vm_addr_t fail_addr = PROGRAM_BASE + 17 * 8;
    const vm_addr_t isr_entry = PROGRAM_BASE + 20 * 8;
    uint64_t program[] = {
        INST(OP_MOVI, 10, 0, 0, flag_addr),
        INST(OP_MOVI, 11, 0, 0, 0),
        INST(OP_STORE32, 11, 10, 0, 0),
        INST(OP_MOVI, 12, 0, 0, vector_addr),
        INST(OP_STORE32, 11, 12, 0, 0),
        INST(OP_MOVI, 13, 0, 0, count_addr),
        INST(OP_STORE32, 11, 13, 0, 0),
        INST(OP_INTI, 7, 8, 9, 9),                   /* rd/rs1/rs2 ignored */
        INST(OP_LOAD32, 1, 12, 0, 0),
        INST(OP_CMPI, 1, 0, 0, 9),
        INST(OP_JNZ, 0, 0, 0, fail_addr),
        INST(OP_LOAD32, 2, 13, 0, 0),
        INST(OP_CMPI, 2, 0, 0, 1),
        INST(OP_JNZ, 0, 0, 0, fail_addr),
        INST(OP_MOVI, 11, 0, 0, 1),
        INST(OP_STORE32, 11, 10, 0, 0),
        INST(OP_HALT, 0, 0, 0, 0),
        INST(OP_MOVI, 11, 0, 0, 2),
        INST(OP_STORE32, 11, 10, 0, 0),
        INST(OP_HALT, 0, 0, 0, 0),
        INST(OP_MOVI, 20, 0, 0, vector_addr),        /* ISR: record vector */
        INST(OP_STORE32, 31, 20, 0, 0),
        INST(OP_MOVI, 21, 0, 0, count_addr),
        INST(OP_LOAD32, 22, 21, 0, 0),
        INST(OP_INC, 22, 0, 0, 0),
        INST(OP_STORE32, 22, 21, 0, 0),
        INST(OP_IRET, 0, 0, 0, 0),
    };

    VM *vm = vm_create(MEM_SIZE, program, sizeof(program) / sizeof(program[0]), NULL, 0, NULL, 1);
    if (!vm)
        return 0;
    disk_init(vm, "./disk.img");
    init_ivt(vm);
    register_isr(vm, 9, isr_entry);
    int ok = vm_run_headless(vm, 1000);
    uint32_t flag = vm_read32(vm, flag_addr);
    uint32_t vector = vm_read32(vm, vector_addr);
    uint32_t count = vm_read32(vm, count_addr);
    ok = ok && (flag == 1u) && (vector == 9u) && (count == 1u);
    vm_destroy(vm);
    return ok;
}

static int run_selftest_ps2_controller(void) {
    uint64_t program[] = {
        INST(OP_HALT, 0, 0, 0, 0),
    };
    VM *vm = vm_create(MEM_SIZE, program, sizeof(program) / sizeof(program[0]), NULL, 0, NULL, 1);
    if (!vm)
        return 0;
    init_ivt(vm);

    int ok = 1;
    ok = ok && ((vm_ps2_read_status(vm) & PS2_STATUS_OUT_FULL) == 0u);

    accept_io(vm, PS2_COMMAND, 0x20u);
    ok = ok && ((vm_ps2_read_status(vm) & PS2_STATUS_OUT_FULL) != 0u);
    ok = ok && (vm_ps2_read_data(vm) == 0x03u);

    accept_io(vm, PS2_DATA, 0xF2u);
    ok = ok && (vm_ps2_read_data(vm) == 0xFAu);
    ok = ok && (vm_ps2_read_data(vm) == 0xABu);
    ok = ok && (vm_ps2_read_data(vm) == 0x83u);

    ok = ok && vm_ps2_kbd_enqueue(vm, 0x1Eu);
    ok = ok && ((vm_ps2_read_status(vm) & PS2_STATUS_AUX_DATA) == 0u);
    ok = ok && (vm_ps2_read_data(vm) == 0x1Eu);

    accept_io(vm, PS2_COMMAND, 0xD4u);
    accept_io(vm, PS2_DATA, 0xF4u);
    ok = ok && ((vm_ps2_read_status(vm) & PS2_STATUS_AUX_DATA) != 0u);
    ok = ok && (vm_ps2_read_data(vm) == 0xFAu);

    ok = ok && vm_ps2_mouse_enqueue(vm, 0x08u);
    ok = ok && ((vm_ps2_read_status(vm) & PS2_STATUS_AUX_DATA) != 0u);
    ok = ok && (vm_ps2_read_data(vm) == 0x08u);

    /* The active 8042 path must keep working after the compatibility-only
     * legacy mouse FIFO fills up. The kernel does not consume that FIFO. */
    for (uint32_t i = 0u; i < 300u; i++) {
        const uint8_t value = (uint8_t)(i ^ 0x5Au);
        ok = ok && vm_ps2_mouse_enqueue(vm, value);
        ok = ok && ((vm_ps2_read_status(vm) & PS2_STATUS_AUX_DATA) != 0u);
        ok = ok && (vm_ps2_read_data(vm) == value);
    }

    /* Mouse reports are indivisible. With only two controller slots free,
     * rejecting a report must leave the producer index untouched; accepting
     * it later must expose exactly three ordered bytes. The legacy FIFO is
     * already full here, so it cannot hide an 8042 backpressure failure. */
    for (uint32_t i = 0u; i < 509u; i++) {
        ok = ok && vm_ps2_mouse_enqueue(vm, (uint8_t)i);
    }
    {
        const uint16_t head_before = vm->ps2_out_head;
        ok = ok && !vm_ps2_mouse_enqueue_packet(vm, 0x09u, 0x01u, 0x02u);
        ok = ok && vm->ps2_out_head == head_before;
    }
    for (uint32_t i = 0u; i < 509u; i++) {
        ok = ok && vm_ps2_read_data(vm) == (uint8_t)i;
    }
    ok = ok && vm_ps2_mouse_enqueue_packet(vm, 0x09u, 0x01u, 0x02u);
    ok = ok && vm_ps2_read_data(vm) == 0x09u;
    ok = ok && vm_ps2_read_data(vm) == 0x01u;
    ok = ok && vm_ps2_read_data(vm) == 0x02u;

    /* Reproduce the end-of-handler race: a new byte raises the mouse IRQ,
     * then a late EOI clears that latch while the 8042 buffer is non-empty.
     * The level source must immediately reassert it. */
    ok = ok && vm_ps2_mouse_enqueue(vm, 0x08u);
    ok = ok && (vm_ps2_read_data(vm) == 0x08u);
    ok = ok && vm_ps2_mouse_enqueue(vm, 0x09u);
    ok = ok && ((vm_interrupt_read_pending32(vm, BSP_CORE, 0u) &
                 (1u << INT_MOUSE)) != 0u);
    vm_write32(vm, INTC_BASE + INTC_REG_EOI, INT_MOUSE);
    ok = ok && ((vm_interrupt_read_pending32(vm, BSP_CORE, 0u) &
                 (1u << INT_MOUSE)) != 0u);
    ok = ok && (vm_ps2_read_data(vm) == 0x09u);

    vm_destroy(vm);
    return ok;
}

static int run_selftest_fb_accel(void) {
    uint64_t program[] = {
        INST(OP_HALT, 0, 0, 0, 0),
    };
    VM *vm = vm_create(MEM_SIZE, program, sizeof(program) / sizeof(program[0]), NULL, 0, NULL, 1);
    if (!vm)
        return 0;

    int ok = 1;
    for (size_t row = 0; row < FB_HEIGHT; row++) {
        (void)vm_fb_take_row_dirty(vm, row);
    }
    {
        const size_t target_row = 17u;
        const size_t target_col = 3u;
        const vm_addr_t target_addr = (vm_addr_t)FB_BASE(vm->memory_size) +
                                      (vm_addr_t)((target_row * FB_WIDTH + target_col) * FB_BPP);
        vm_write32(vm, target_addr, 0x0055AA11u);
        ok = ok && (vm_read32(vm, target_addr) == 0x0055AA11u);
        for (size_t row = 0; row < FB_HEIGHT; row++) {
            const int dirty = vm_fb_take_row_dirty(vm, row);
            ok = ok && (dirty == ((row == target_row) ? 1 : 0));
        }
    }

    accept_io(vm, FB_ACCEL_ARG0, 0x00112233u);
    accept_io(vm, FB_ACCEL_CMD, FB_ACCEL_CMD_CLEAR);
    ok = ok && (vm->fb[0] == 0x00112233u);
    ok = ok && (vm->fb[(size_t)FB_WIDTH * (size_t)FB_HEIGHT - 1u] == 0x00112233u);
    for (size_t row = 0; row < FB_HEIGHT; row++) {
        ok = ok && vm_fb_take_row_dirty(vm, row);
    }

    for (size_t y = 0; y < (size_t)FB_HEIGHT; y++) {
        vm->fb[y * (size_t)FB_WIDTH] = (uint32_t)y;
    }
    accept_io(vm, FB_ACCEL_ARG0, 0x00ABCDEFu);
    accept_io(vm, FB_ACCEL_CMD, FB_ACCEL_CMD_SCROLL_UP_8PX);
    ok = ok && (vm->fb[0] == 8u);
    ok = ok && (vm->fb[((size_t)FB_HEIGHT - 9u) * (size_t)FB_WIDTH] == (uint32_t)(FB_HEIGHT - 1u));
    ok = ok && (vm->fb[((size_t)FB_HEIGHT - 8u) * (size_t)FB_WIDTH] == 0x00ABCDEFu);
    ok = ok && (vm->fb[(size_t)FB_WIDTH * (size_t)FB_HEIGHT - 1u] == 0x00ABCDEFu);

    vm_destroy(vm);
    return ok;
}

static int run_selftest_nat_rx_queue(void) {
    static const uint8_t device_mac[6] = {0x02u, 0u, 0u, 0u, 0u, 1u};
    static const uint8_t guest_mac[6] = {0x02u, 0u, 0u, 0u, 0u, 2u};
    ether_backend_t backend;
    uint8_t request[42] = {0};
    uint8_t reply[64];
    int ok = 1;

    memset(&backend, 0, sizeof(backend));
    if (ether_backend_nat_create(&backend) != 0) return 0;
    if (!backend.init || backend.init(backend.state, device_mac) != 0) {
        if (backend.close) backend.close(backend.state);
        return 0;
    }

    memset(request, 0xFF, 6);
    memcpy(request + 6, guest_mac, sizeof(guest_mac));
    request[12] = 0x08u;
    request[13] = 0x06u;
    request[14] = 0x00u; request[15] = 0x01u;
    request[16] = 0x08u; request[17] = 0x00u;
    request[18] = 6u; request[19] = 4u;
    request[20] = 0x00u; request[21] = 0x01u;
    memcpy(request + 22, guest_mac, sizeof(guest_mac));
    request[28] = 10u; request[29] = 0u; request[30] = 2u; request[31] = 15u;
    request[38] = 10u; request[39] = 0u; request[40] = 2u; request[41] = 2u;

    for (int i = 0; i < 16; i++) {
        ok = ok && backend.send && backend.send(backend.state, request, sizeof(request)) == 0;
    }
    ok = ok && backend.send && backend.send(backend.state, request, sizeof(request)) != 0;
    for (int i = 0; i < 16; i++) {
        int n = backend.recv ? backend.recv(backend.state, reply, sizeof(reply)) : -1;
        ok = ok && n == 42;
        ok = ok && reply[12] == 0x08u && reply[13] == 0x06u;
        ok = ok && reply[20] == 0x00u && reply[21] == 0x02u;
        ok = ok && memcmp(reply, guest_mac, sizeof(guest_mac)) == 0;
    }
    ok = ok && backend.recv && backend.recv(backend.state, reply, sizeof(reply)) == 0;
    if (backend.close) backend.close(backend.state);
    return ok;
}

static int run_selftest_pci_ethernet(void) {
    uint64_t program[] = {INST(OP_HALT, 0, 0, 0, 0)};
    ether_backend_t backend;
    VM *vm = vm_create(MEM_SIZE, program, 1u, NULL, 0u, NULL, 1);
    if (!vm) return 0;

    memset(&backend, 0, sizeof(backend));
    if (ether_backend_null_create(&backend) != 0 || ether_init(vm, &backend) != 0) {
        vm_destroy(vm);
        return 0;
    }

    int ok = 1;
    const uint32_t fn_addr = PCIE_ECAM_BASE + PCI_ECAM_FUNC_COUNT * PCI_ECAM_FUNC_SIZE;
    uint32_t id = vm_mmio_read32(vm, fn_addr + PCI_CFG_VENDOR_ID);
    uint32_t class_rev = vm_mmio_read32(vm, fn_addr + PCI_CFG_REVISION_ID);
    ok = ok && (id & 0xFFFFu) == LAMP_PCI_VENDOR_ID;
    ok = ok && (id >> 16) == ETHER_PCI_DEVICE_ID;
    ok = ok && ((class_rev >> 24) & 0xFFu) == PCI_CLASS_NETWORK;
    ok = ok && ((class_rev >> 16) & 0xFFu) == PCI_SUBCLASS_ETHERNET;

    vm_mmio_write32(vm, fn_addr + PCI_CFG_BAR0, 0xFFFFFFFFu);
    ok = ok && vm_mmio_read32(vm, fn_addr + PCI_CFG_BAR0) == 0xFFFFF000u;
    const uint32_t bar0 = 0x00A00000u;
    vm_mmio_write32(vm, fn_addr + PCI_CFG_BAR0, bar0);
    vm_mmio_write32(vm, fn_addr + PCI_CFG_COMMAND,
                    PCI_COMMAND_MEM_ENABLE | PCI_COMMAND_BUS_MASTER);
    ok = ok && find_mmio(vm, bar0) != NULL;
    ok = ok && vm_mmio_read32(vm, bar0 + ETHER_OFF_STATUS) == ETHER_STATUS_LINK;
    ok = ok && vm_mmio_read32(vm, bar0 + ETHER_OFF_MAC_LO) == 0x00000002u;
    ok = ok && vm_mmio_read32(vm, bar0 + ETHER_OFF_MAC_HI) == 0x00000100u;

    ether_shutdown(vm);
    vm_destroy(vm);
    return ok;
}

static int run_selftest_pci_gpu(void) {
    uint64_t program[] = {INST(OP_HALT, 0, 0, 0, 0)};
    VM *vm = vm_create(MEM_SIZE, program, 1u, NULL, 0u, NULL, 1);
    if (!vm) return 0;

    int ok = 1;
    const uint32_t fn_addr = PCIE_ECAM_BASE +
                             (2u * PCI_ECAM_FUNC_COUNT) * PCI_ECAM_FUNC_SIZE;
    const uint32_t ctrl = 0x00A10000u;
    const uint32_t vram = 0x00C00000u;
    const uint32_t row = 10u;
    const uint32_t col = 20u;
    const uint32_t pixel_offset = (row * FB_WIDTH + col) * FB_BPP;
    const uint32_t color = 0x0011AACC;
    const uint32_t flipped_color = 0x00CC8844u;
    const uint32_t second_scanout = FB_SIZE;
    const uint32_t vector = 41u;
    uint32_t id = vm_mmio_read32(vm, fn_addr + PCI_CFG_VENDOR_ID);
    uint32_t class_rev = vm_mmio_read32(vm, fn_addr + PCI_CFG_REVISION_ID);

    ok = ok && (id & 0xFFFFu) == LAMP_PCI_VENDOR_ID;
    ok = ok && (id >> 16) == LAMP_PCI_GPU_DEVICE_ID;
    ok = ok && ((class_rev >> 24) & 0xFFu) == PCI_CLASS_DISPLAY;
    vm_mmio_write32(vm, fn_addr + PCI_CFG_BAR0, 0xFFFFFFFFu);
    ok = ok && vm_mmio_read32(vm, fn_addr + PCI_CFG_BAR0) == 0xFFFFF000u;
    vm_mmio_write32(vm, fn_addr + PCI_CFG_BAR0 + 4u, 0xFFFFFFFFu);
    ok = ok && vm_mmio_read32(vm, fn_addr + PCI_CFG_BAR0 + 4u) == 0xFFC00008u;
    vm_mmio_write32(vm, fn_addr + PCI_CFG_BAR0, ctrl);
    vm_mmio_write32(vm, fn_addr + PCI_CFG_BAR0 + 4u, vram | PCI_BAR_PREFETCHABLE);
    vm_mmio_write32(vm, fn_addr + PCI_CFG_COMMAND,
                    PCI_COMMAND_MEM_ENABLE | PCI_COMMAND_BUS_MASTER);

    ok = ok && vm_mmio_read32(vm, ctrl + LAMP_GPU_REG_MAGIC) == LAMP_GPU_MAGIC;
    ok = ok && vm_mmio_read32(vm, ctrl + LAMP_GPU_REG_VRAM_SIZE) == LAMP_GPU_VRAM_SIZE;
    ok = ok && (vm_mmio_read32(vm, ctrl + LAMP_GPU_REG_CAPS) &
                LAMP_GPU_CAP_CURSOR) != 0u;
    for (size_t dirty_row = 0; dirty_row < FB_HEIGHT; dirty_row++) {
        (void)vm_fb_take_row_dirty(vm, dirty_row);
    }
    vm_mmio_write32(vm, vram + pixel_offset, color);
    vm_mmio_write32(vm, ctrl + LAMP_GPU_REG_DAMAGE_X, col);
    vm_mmio_write32(vm, ctrl + LAMP_GPU_REG_DAMAGE_Y, row);
    vm_mmio_write32(vm, ctrl + LAMP_GPU_REG_DAMAGE_W, 1u);
    vm_mmio_write32(vm, ctrl + LAMP_GPU_REG_DAMAGE_H, 1u);
    vm_mmio_write32(vm, ctrl + LAMP_GPU_REG_COMMAND,
                    LAMP_GPU_CMD_ENABLE | LAMP_GPU_CMD_FLUSH);
    ok = ok && vm->fb[(size_t)row * FB_WIDTH + col] == color;
    for (size_t dirty_row = 0; dirty_row < FB_HEIGHT; dirty_row++) {
        const int dirty = vm_fb_take_row_dirty(vm, dirty_row);
        ok = ok && (dirty == ((dirty_row == row) ? 1 : 0));
    }

    uint32_t cap = vm_mmio_read32(vm, fn_addr + PCI_CFG_CAP_PTR) & 0xFCu;
    while (cap >= PCI_CFG_CAP_START && cap < 0x100u) {
        uint32_t header = vm_mmio_read32(vm, fn_addr + cap);
        if ((header & 0xFFu) == PCI_CAP_ID_MSI) {
            vm_mmio_write32(vm, fn_addr + cap + 4u, 0u);
            vm_mmio_write32(vm, fn_addr + cap + 8u, 0u);
            vm_mmio_write32(vm, fn_addr + cap + 12u, vector);
            vm_mmio_write32(vm, fn_addr + cap, header | (1u << 16));
            break;
        }
        cap = (header >> 8) & 0xFCu;
    }
    ok = ok && cap >= PCI_CFG_CAP_START && cap < 0x100u;
    vm_mmio_write32(vm, ctrl + LAMP_GPU_REG_IRQ_ENABLE, LAMP_GPU_IRQ_FLIP_COMPLETE);
    vm_mmio_write32(vm, vram + second_scanout + pixel_offset, flipped_color);
    vm_mmio_write32(vm, ctrl + LAMP_GPU_REG_PENDING_OFFSET, second_scanout);
    vm_mmio_write32(vm, ctrl + LAMP_GPU_REG_COMMAND, LAMP_GPU_CMD_PAGE_FLIP);
    ok = ok && vm_mmio_read32(vm, ctrl + LAMP_GPU_REG_COMPLETE_SEQ) == 2u;
    ok = ok && vm_mmio_read32(vm, ctrl + LAMP_GPU_REG_SCANOUT_OFFSET) == second_scanout;
    ok = ok && vm->fb[(size_t)row * FB_WIDTH + col] == flipped_color;
    ok = ok && (vm_mmio_read32(vm, ctrl + LAMP_GPU_REG_IRQ_STATUS) &
                LAMP_GPU_IRQ_FLIP_COMPLETE) != 0u;
    ok = ok && ((vm_interrupt_read_pending32(vm, 0, vector / 32u) >> (vector % 32u)) & 1u) != 0u;
    vm_mmio_write32(vm, ctrl + LAMP_GPU_REG_IRQ_ACK, LAMP_GPU_IRQ_FLIP_COMPLETE);

    vm_mmio_write32(vm, ctrl + LAMP_GPU_REG_CURSOR_X, col);
    vm_mmio_write32(vm, ctrl + LAMP_GPU_REG_CURSOR_Y, row);
    vm_mmio_write32(vm, ctrl + LAMP_GPU_REG_CURSOR_CTRL,
                    LAMP_GPU_CURSOR_VISIBLE);
    vm_mmio_write32(vm, ctrl + LAMP_GPU_REG_COMMAND,
                    LAMP_GPU_CMD_CURSOR_UPDATE);
    ok = ok && vm->fb[(size_t)row * FB_WIDTH + col] == 0x00030A10u;

    vm_mmio_write32(vm, ctrl + LAMP_GPU_REG_CURSOR_X, col + 8u);
    vm_mmio_write32(vm, ctrl + LAMP_GPU_REG_CURSOR_Y, row + 8u);
    vm_mmio_write32(vm, ctrl + LAMP_GPU_REG_CURSOR_CTRL,
                    LAMP_GPU_CURSOR_VISIBLE |
                    (1u << LAMP_GPU_CURSOR_BUTTONS_SHIFT));
    vm_mmio_write32(vm, ctrl + LAMP_GPU_REG_COMMAND,
                    LAMP_GPU_CMD_CURSOR_UPDATE);
    ok = ok && vm->fb[(size_t)row * FB_WIDTH + col] == flipped_color;
    ok = ok && vm->fb[(size_t)(row + 10u) * FB_WIDTH + col + 9u] == 0x0038BDF8u;

    /* A scanout flip replaces the scene but must preserve the independent
     * cursor plane at its current position and button color. */
    vm_mmio_write32(vm, ctrl + LAMP_GPU_REG_PENDING_OFFSET, 0u);
    vm_mmio_write32(vm, ctrl + LAMP_GPU_REG_COMMAND, LAMP_GPU_CMD_PAGE_FLIP);
    ok = ok && vm->fb[(size_t)row * FB_WIDTH + col] == color;
    ok = ok && vm->fb[(size_t)(row + 8u) * FB_WIDTH + col + 8u] == 0x00030A10u;
    ok = ok && vm->fb[(size_t)(row + 10u) * FB_WIDTH + col + 9u] == 0x0038BDF8u;

    vm_mmio_write32(vm, ctrl + LAMP_GPU_REG_CURSOR_CTRL, 0u);
    vm_mmio_write32(vm, ctrl + LAMP_GPU_REG_COMMAND,
                    LAMP_GPU_CMD_CURSOR_UPDATE);
    ok = ok && vm->fb[(size_t)(row + 8u) * FB_WIDTH + col + 8u] == 0u;

    vm_mmio_write32(vm, ctrl + LAMP_GPU_REG_COMMAND, LAMP_GPU_CMD_DISABLE);
    ok = ok && vm->fb[(size_t)row * FB_WIDTH + col] == 0u;
    vm_destroy(vm);
    return ok;
}

static int run_selftest_pci_audio_dma(void) {
    enum {
        submit_pa = 0x00030000u,
        complete_pa = 0x00031000u,
        pcm_pa = 0x00032000u,
        iommu_root_pa = 0x00034000u,
        iommu_l2_pa = 0x00035000u,
        dma_iova_base = 0x10000000u,
        ring_count = 8u,
        pcm_bytes = 16u
    };
    uint64_t program[] = {INST(OP_HALT, 0, 0, 0, 0)};
    VM *vm = vm_create(MEM_SIZE, program, 1u, NULL, 0u, NULL, 1);
    if (!vm) return 0;

    int ok = 1;
    const uint32_t fn_addr = PCIE_ECAM_BASE +
                             (3u * PCI_ECAM_FUNC_COUNT) * PCI_ECAM_FUNC_SIZE;
    const uint32_t ctrl = 0x00E00000u;
    const uint32_t vector = 42u;
    uint32_t id = vm_mmio_read32(vm, fn_addr + PCI_CFG_VENDOR_ID);
    uint32_t class_rev = vm_mmio_read32(vm, fn_addr + PCI_CFG_REVISION_ID);

    ok = ok && (id & 0xFFFFu) == LAMP_PCI_VENDOR_ID;
    ok = ok && (id >> 16) == LAMP_PCI_AUDIO_DEVICE_ID;
    ok = ok && ((class_rev >> 24) & 0xFFu) == PCI_CLASS_MULTIMEDIA;
    ok = ok && ((class_rev >> 16) & 0xFFu) == PCI_SUBCLASS_AUDIO;
    vm_mmio_write32(vm, fn_addr + PCI_CFG_BAR0, 0xFFFFFFFFu);
    ok = ok && vm_mmio_read32(vm, fn_addr + PCI_CFG_BAR0) == 0xFFFFF000u;
    vm_mmio_write32(vm, fn_addr + PCI_CFG_BAR0, ctrl);
    vm_mmio_write32(vm, fn_addr + PCI_CFG_COMMAND,
                    PCI_COMMAND_MEM_ENABLE | PCI_COMMAND_BUS_MASTER);

    ok = ok && vm_mmio_read32(vm, ctrl + LAMP_AUDIO_REG_MAGIC) == LAMP_AUDIO_MAGIC;
    ok = ok && vm_mmio_read32(vm, ctrl + LAMP_AUDIO_REG_RATE) == LAMP_AUDIO_RATE;
    ok = ok && vm_mmio_read32(vm, ctrl + LAMP_AUDIO_REG_CHANNELS) == LAMP_AUDIO_CHANNELS;
    ok = ok && vm_mmio_read32(vm, ctrl + LAMP_AUDIO_REG_SAMPLE_BITS) ==
               LAMP_AUDIO_SAMPLE_BITS;

    vm->iommu.ctrl = IOMMU_CTRL_ENABLE;
    vm->iommu.devices[IOMMU_DEV_AUDIO].ctrl =
        IOMMU_DEV_CTRL_ENABLE | IOMMU_DEV_CTRL_PAGED;
    vm->iommu.devices[IOMMU_DEV_AUDIO].root = iommu_root_pa;
    const uint32_t dma_pde = dma_iova_base >> 22;
    store_le32(&vm->memory[iommu_root_pa + dma_pde * 4u],
               iommu_l2_pa | IOMMU_PTE_P);
    store_le32(&vm->memory[iommu_l2_pa +
                           (((dma_iova_base + submit_pa) >> 12) & 0x3FFu) * 4u],
               submit_pa | IOMMU_PTE_P | IOMMU_PTE_R | IOMMU_PTE_W);
    store_le32(&vm->memory[iommu_l2_pa +
                           (((dma_iova_base + complete_pa) >> 12) & 0x3FFu) * 4u],
               complete_pa | IOMMU_PTE_P | IOMMU_PTE_R | IOMMU_PTE_W);
    store_le32(&vm->memory[iommu_l2_pa +
                           (((dma_iova_base + pcm_pa) >> 12) & 0x3FFu) * 4u],
               pcm_pa | IOMMU_PTE_P | IOMMU_PTE_R | IOMMU_PTE_W);

    for (uint32_t i = 0u; i < pcm_bytes; i++) {
        vm->memory[pcm_pa + i] = (uint8_t)(i * 7u);
    }
    store_le32(&vm->memory[submit_pa + 0u], dma_iova_base + pcm_pa);
    store_le32(&vm->memory[submit_pa + 4u], 0u);
    store_le32(&vm->memory[submit_pa + 8u], pcm_bytes);
    store_le32(&vm->memory[submit_pa + 12u],
               LAMP_DMA_DESC_F_IRQ | LAMP_DMA_DESC_F_END);
    store_le32(&vm->memory[submit_pa + 16u], 0xA11D0001u);
    store_le32(&vm->memory[submit_pa + 20u], 0u);
    store_le32(&vm->memory[submit_pa + 24u], 0u);
    store_le32(&vm->memory[submit_pa + 28u], 0u);

    uint32_t cap = vm_mmio_read32(vm, fn_addr + PCI_CFG_CAP_PTR) & 0xFCu;
    while (cap >= PCI_CFG_CAP_START && cap < 0x100u) {
        uint32_t header = vm_mmio_read32(vm, fn_addr + cap);
        if ((header & 0xFFu) == PCI_CAP_ID_MSI) {
            vm_mmio_write32(vm, fn_addr + cap + 4u, 0u);
            vm_mmio_write32(vm, fn_addr + cap + 8u, 0u);
            vm_mmio_write32(vm, fn_addr + cap + 12u, vector);
            vm_mmio_write32(vm, fn_addr + cap, header | (1u << 16));
            break;
        }
        cap = (header >> 8) & 0xFCu;
    }
    ok = ok && cap >= PCI_CFG_CAP_START && cap < 0x100u;

    vm_mmio_write32(vm, ctrl + LAMP_AUDIO_REG_SUBMIT_BASE_LO,
                    dma_iova_base + submit_pa);
    vm_mmio_write32(vm, ctrl + LAMP_AUDIO_REG_SUBMIT_BASE_HI, 0u);
    vm_mmio_write32(vm, ctrl + LAMP_AUDIO_REG_SUBMIT_COUNT, ring_count);
    vm_mmio_write32(vm, ctrl + LAMP_AUDIO_REG_COMPLETE_BASE_LO,
                    dma_iova_base + complete_pa);
    vm_mmio_write32(vm, ctrl + LAMP_AUDIO_REG_COMPLETE_BASE_HI, 0u);
    vm_mmio_write32(vm, ctrl + LAMP_AUDIO_REG_COMPLETE_COUNT, ring_count);
    vm_mmio_write32(vm, ctrl + LAMP_AUDIO_REG_IRQ_ENABLE,
                    LAMP_AUDIO_IRQ_COMPLETION | LAMP_AUDIO_IRQ_ERROR);
    vm_mmio_write32(vm, ctrl + LAMP_AUDIO_REG_COMMAND, LAMP_AUDIO_CMD_ENABLE);
    vm_mmio_write32(vm, ctrl + LAMP_AUDIO_REG_SUBMIT_TAIL, 1u);
    audio_poll(vm);

    ok = ok && vm_mmio_read32(vm, ctrl + LAMP_AUDIO_REG_SUBMIT_HEAD) == 1u;
    ok = ok && vm_mmio_read32(vm, ctrl + LAMP_AUDIO_REG_COMPLETE_TAIL) == 1u;
    ok = ok && vm_mmio_read32(vm, ctrl + LAMP_AUDIO_REG_COMPLETED_DESCS) == 1u;
    ok = ok && load_le32(&vm->memory[complete_pa + 0u]) == 0xA11D0001u;
    ok = ok && load_le32(&vm->memory[complete_pa + 4u]) == LAMP_DMA_COMPLETION_OK;
    ok = ok && load_le32(&vm->memory[complete_pa + 8u]) == pcm_bytes;
    ok = ok && load_le32(&vm->memory[complete_pa + 12u]) == 1u;
    ok = ok && (vm_mmio_read32(vm, ctrl + LAMP_AUDIO_REG_IRQ_STATUS) &
                LAMP_AUDIO_IRQ_COMPLETION) != 0u;
    ok = ok && ((vm_interrupt_read_pending32(vm, 0, vector / 32u) >>
                 (vector % 32u)) & 1u) != 0u;
    vm_mmio_write32(vm, ctrl + LAMP_AUDIO_REG_COMPLETE_HEAD, 1u);
    vm_mmio_write32(vm, ctrl + LAMP_AUDIO_REG_IRQ_ACK,
                    LAMP_AUDIO_IRQ_COMPLETION | LAMP_AUDIO_IRQ_ERROR);

    vm_destroy(vm);
    return ok;
}

static uint64_t selftest_sysinfo_read64(VM *vm, uint32_t low_offset,
                                        uint32_t high_offset) {
    const uint32_t low = vm_mmio_read32(vm, SYSINFO_BASE + low_offset);
    const uint32_t high = vm_mmio_read32(vm, SYSINFO_BASE + high_offset);
    return ((uint64_t)high << 32) | low;
}

static int run_selftest_runtime_stats(void) {
    uint64_t program[] = {
        INST(OP_HALT, 0, 0, 0, 0),
    };
    VM *vm = vm_create(MEM_SIZE, program,
                       sizeof(program) / sizeof(program[0]),
                       NULL, 0, NULL, 1);
    VmRuntimeStats stats;
    int ok;
    if (!vm) {
        return 0;
    }

    vm->cpu_frequency_hz = 123000000ull;
    atomic_store_explicit(&vm->cpus[0].execution_times, 12345u,
                          memory_order_relaxed);
    vm_runtime_stats_sample(vm, &stats);

    ok = stats.cpu_frequency_hz == 123000000ull &&
         stats.executed_instructions == 12345u &&
         stats.guest_ram_bytes == MEM_SIZE &&
         stats.core_count == 1u &&
         stats.active_core_count == 1u;
    ok = ok && vm_mmio_read32(vm, SYSINFO_BASE + SYSINFO_REG_LAYOUT_VERSION) ==
                   SYSINFO_LAYOUT_VERSION;
    ok = ok && (vm_mmio_read32(vm, SYSINFO_BASE + SYSINFO_REG_FEATURES) &
                SYSINFO_FEATURE_RUNTIME_STATS) != 0u;
    ok = ok && vm_mmio_read32(vm, SYSINFO_BASE + SYSINFO_REG_RUNTIME_VERSION) ==
                   SYSINFO_RUNTIME_VERSION;
    ok = ok && selftest_sysinfo_read64(vm, SYSINFO_REG_CPU_FREQ_HZ_LO,
                                        SYSINFO_REG_CPU_FREQ_HZ_HI) ==
                   123000000ull;
    ok = ok && selftest_sysinfo_read64(vm, SYSINFO_REG_EXEC_COUNT_LO,
                                        SYSINFO_REG_EXEC_COUNT_HI) == 12345u;
    ok = ok && selftest_sysinfo_read64(vm, SYSINFO_REG_CPU_CYCLES_LO,
                                        SYSINFO_REG_CPU_CYCLES_HI) > 0u;

    vm_destroy(vm);
    return ok;
}

static int run_selftest_cpu_pacing(void) {
    enum { PACE_TEST_INSTRUCTIONS = 20000 };
    uint64_t *program = malloc(sizeof(*program) * PACE_TEST_INSTRUCTIONS);
    VM *vm;
    uint64_t start_ns;
    uint64_t elapsed_ns;
    uint64_t executed;
    int ok;
    if (!program) {
        return 0;
    }
    for (size_t i = 0; i + 1u < PACE_TEST_INSTRUCTIONS; i++) {
        program[i] = INST(OP_MOVI, 1, 0, 0, (uint32_t)i);
    }
    program[PACE_TEST_INSTRUCTIONS - 1u] = INST(OP_HALT, 0, 0, 0, 0);

    vm = vm_create(MEM_SIZE, program, PACE_TEST_INSTRUCTIONS,
                   NULL, 0, NULL, 1);
    free(program);
    if (!vm) {
        return 0;
    }
    vm->cpu_frequency_hz = 1000000ull;
    start_ns = host_monotonic_time_ns();
    ok = vm_run_headless(vm, 1000u);
    elapsed_ns = host_monotonic_time_ns() - start_ns;
    executed = atomic_load_explicit(&vm->cpus[0].execution_times,
                                    memory_order_relaxed);

    /* At 1 MHz, 20,000 one-cycle instructions need 20 ms. Keep a small
     * tolerance for the first host scheduling handoff. */
    ok = ok && executed == PACE_TEST_INSTRUCTIONS &&
         elapsed_ns >= 18000000ull;
    vm_destroy(vm);
    return ok;
}

static int run_selftest_integer_flag_edges(void) {
    typedef struct FlagCase {
        uint8_t op;
        uint32_t a;
        uint32_t b;
        uint32_t result;
        uint32_t result_flags;
    } FlagCase;
    static const FlagCase cases[] = {
        { OP_ADD, 0x7FFFFFFFu, 1u, 0x80000000u, FLAG_SF | FLAG_OF },
        { OP_ADD, 0xFFFFFFFFu, 1u, 0u, FLAG_CF | FLAG_ZF },
        { OP_ADD, 0x80000000u, 0x80000000u, 0u,
          FLAG_CF | FLAG_ZF | FLAG_OF },
        { OP_SUB, 0u, 0x80000000u, 0x80000000u,
          FLAG_CF | FLAG_SF | FLAG_OF },
        { OP_SUB, 0x80000000u, 1u, 0x7FFFFFFFu, FLAG_OF },
        { OP_SUB, 0u, 1u, 0xFFFFFFFFu, FLAG_CF | FLAG_SF },
    };
    const uint32_t preserved_flags = FLAG_PF | FLAG_AF | 0x80000000u;
    uint64_t program[] = {
        INST(OP_ADD, 3, 1, 2, 0),
        INST(OP_HALT, 0, 0, 0, 0),
    };
    VM *vm = vm_create(MEM_SIZE, program,
                       sizeof(program) / sizeof(program[0]),
                       NULL, 0, NULL, 1);
    int ok = 1;
    if (!vm) {
        return 0;
    }
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); i++) {
        VCPU *cpu = &vm->cpus[0];
        store_le64(&vm->memory[PROGRAM_BASE],
                   INST(cases[i].op, 3, 1, 2, 0));
        memset(cpu->regs, 0, sizeof(cpu->regs));
        cpu->regs[1] = cases[i].a;
        cpu->regs[2] = cases[i].b;
        cpu->flags = preserved_flags |
                     FLAG_CF | FLAG_ZF | FLAG_SF | FLAG_OF;
        cpu->ip = PROGRAM_BASE;
        cpu->last_ip = PROGRAM_BASE;
        atomic_store_explicit(&vm->stop_flags, 0u, memory_order_release);
        ok = ok && vm_engine_execute_quantum(vm, cpu) == 1u;
        ok = ok && cpu->regs[3] == cases[i].result;
        ok = ok && cpu->flags ==
            (preserved_flags | cases[i].result_flags);
    }
    vm_destroy(vm);
    return ok;
}

static int run_selftests_for_engine(VmExecutionEngine engine) {
    g_selftest_engine = engine;
    int ok1 = run_selftest_startap_cpuid();
    int ok2 = run_selftest_ipi();
    int ok3 = run_selftest_mmu_percpu_root();
    int ok4 = run_selftest_relctrl();
    int ok5 = run_selftest_zero_branch_flags();
    int ok6 = run_selftest_callr_unused_fields();
    int ok7 = run_selftest_atomic_conformance();
    int ok8 = run_selftest_div0_interrupt_conformance();
    int ok9 = run_selftest_load16_signext_conformance();
    int ok10 = run_selftest_indexed_rw_width_conformance();
    int ok11 = run_selftest_relcond_extended_conformance();
    int ok12 = run_selftest_inti_imm_conformance();
    int ok13 = run_selftest_ps2_controller();
    int ok14 = run_selftest_fb_accel();
    int ok15 = run_selftest_iommu_paged_translation();
    int ok16 = run_selftest_pcie_enumeration();
    int ok17 = run_selftest_nat_rx_queue();
    int ok18 = run_selftest_pci_ethernet();
    int ok19 = run_selftest_pci_gpu();
    int ok20 = run_selftest_pci_audio_dma();
    int ok21 = run_selftest_runtime_stats();
    int ok22 = run_selftest_cpu_pacing();
    int ok23 = run_selftest_mmu_global_tlb_flush();
    int ok24 = run_selftest_integer_flag_edges();

    printf("[selftest] engine=%s\n",
           engine == VM_ENGINE_CACHED ? "cached" :
           (engine == VM_ENGINE_THREADED ? "threaded" :
            (engine == VM_ENGINE_JIT ? "jit" : "classic")));
    printf("[selftest] startap_cpuid: %s\n", ok1 ? "PASS" : "FAIL");
    printf("[selftest] ipi: %s\n", ok2 ? "PASS" : "FAIL");
    printf("[selftest] mmu_percpu_root: %s\n", ok3 ? "PASS" : "FAIL");
    printf("[selftest] relctrl: %s\n", ok4 ? "PASS" : "FAIL");
    printf("[selftest] zero_branch_flags: %s\n", ok5 ? "PASS" : "FAIL");
    printf("[selftest] callr_unused_fields: %s\n", ok6 ? "PASS" : "FAIL");
    printf("[selftest] atomic_conformance: %s\n", ok7 ? "PASS" : "FAIL");
    printf("[selftest] div0_interrupt_conformance: %s\n", ok8 ? "PASS" : "FAIL");
    printf("[selftest] load16_signext_conformance: %s\n", ok9 ? "PASS" : "FAIL");
    printf("[selftest] indexed_rw_width_conformance: %s\n", ok10 ? "PASS" : "FAIL");
    printf("[selftest] relcond_extended_conformance: %s\n", ok11 ? "PASS" : "FAIL");
    printf("[selftest] inti_imm_conformance: %s\n", ok12 ? "PASS" : "FAIL");
    printf("[selftest] ps2_controller: %s\n", ok13 ? "PASS" : "FAIL");
    printf("[selftest] fb_accel: %s\n", ok14 ? "PASS" : "FAIL");
    printf("[selftest] iommu_paged_translation: %s\n", ok15 ? "PASS" : "FAIL");
    printf("[selftest] pcie_enumeration: %s\n", ok16 ? "PASS" : "FAIL");
    printf("[selftest] nat_rx_queue: %s\n", ok17 ? "PASS" : "FAIL");
    printf("[selftest] pci_ethernet: %s\n", ok18 ? "PASS" : "FAIL");
    printf("[selftest] pci_gpu: %s\n", ok19 ? "PASS" : "FAIL");
    printf("[selftest] pci_audio_dma: %s\n", ok20 ? "PASS" : "FAIL");
    printf("[selftest] runtime_stats: %s\n", ok21 ? "PASS" : "FAIL");
    printf("[selftest] cpu_pacing: %s\n", ok22 ? "PASS" : "FAIL");
    printf("[selftest] mmu_global_tlb_flush: %s\n", ok23 ? "PASS" : "FAIL");
    printf("[selftest] integer_flag_edges: %s\n", ok24 ? "PASS" : "FAIL");
    return (ok1 && ok2 && ok3 && ok4 && ok5 && ok6 && ok7 && ok8 && ok9 &&
            ok10 && ok11 && ok12 && ok13 && ok14 && ok15 && ok16 && ok17 &&
            ok18 && ok19 && ok20 && ok21 && ok22 && ok23 && ok24) ? 0 : 1;
}

static int run_selftest_jit_code_arena(void) {
    VmJitCodeArena arena;
    VmJitCode code;
    const uint32_t first_words[] = { 0xD503201Fu, 0xD65F03C0u };
    const uint32_t second_words[] = { 0xD503203Fu, 0xD65F03C0u };
    void *slot;
    int ok;

    memset(&arena, 0, sizeof(arena));
    memset(&code, 0, sizeof(code));
    if (!vm_jit_code_arena_init(&arena, 2u, 4096u) ||
        !vm_jit_code_assign_slot(&arena, 1u, &code)) {
        vm_jit_code_arena_destroy(&arena);
        return 0;
    }
    slot = code.mapping;
    ok = vm_jit_code_publish(first_words,
                             sizeof(first_words) / sizeof(first_words[0]),
                             &code) &&
         code.mapping == slot && code.entry != NULL &&
         code.code_size == sizeof(first_words);
    vm_jit_code_destroy(&code);
    ok = ok && code.mapping == slot && code.entry == NULL &&
         vm_jit_code_publish(second_words,
                             sizeof(second_words) / sizeof(second_words[0]),
                             &code) &&
         code.mapping == slot && code.entry != NULL &&
         code.code_size == sizeof(second_words);
    vm_jit_code_destroy(&code);
    vm_jit_code_arena_destroy(&arena);
    return ok;
}

int run_selftests(void) {
    const int classic_ok =
        run_selftests_for_engine(VM_ENGINE_CLASSIC) == 0;
    const int cached_ok =
        run_selftests_for_engine(VM_ENGINE_CACHED) == 0;
    const int threaded_ok =
        run_selftests_for_engine(VM_ENGINE_THREADED) == 0;
    const int jit_ok =
        run_selftests_for_engine(VM_ENGINE_JIT) == 0;
    const int invalidation_ok =
        run_selftest_fetch_invalidation(VM_ENGINE_CACHED);
    const int jit_invalidation_ok =
        run_selftest_fetch_invalidation(VM_ENGINE_JIT);
    const int jit_code_arena_ok = run_selftest_jit_code_arena();
    printf("[selftest] cached_fetch_invalidation: %s\n",
           invalidation_ok ? "PASS" : "FAIL");
    printf("[selftest] jit_block_invalidation: %s\n",
           jit_invalidation_ok ? "PASS" : "FAIL");
    printf("[selftest] jit_code_arena: %s\n",
           jit_code_arena_ok ? "PASS" : "FAIL");
    g_selftest_engine = VM_ENGINE_CLASSIC;
    return (classic_ok && cached_ok && threaded_ok && jit_ok &&
            invalidation_ok && jit_invalidation_ok && jit_code_arena_ok) ? 0 : 1;
}

static const char *benchmark_engine_name(VmExecutionEngine engine) {
    return engine == VM_ENGINE_CACHED ? "cached" :
           (engine == VM_ENGINE_THREADED ? "threaded" :
            (engine == VM_ENGINE_JIT ? "jit" : "classic"));
}

static int run_interpreter_benchmark_mode(const char *label,
                                          int enable_mmu,
                                          VmExecutionEngine engine) {
    enum {
        BENCHMARK_ITERATIONS = 10000000u,
        BENCHMARK_MMU_ROOT = 0x4000u,
        BENCHMARK_MMU_L2 = 0x5000u,
    };
    const uint64_t expected_instructions =
        2ull * (uint64_t)BENCHMARK_ITERATIONS + 2ull;
    uint64_t program[] = {
        INST(OP_MOVI, 1, 0, 0, BENCHMARK_ITERATIONS),
        INST(OP_SUBI, 1, 1, 0, 1),
        INST(OP_RJNZ, 0, 0, 0, (uint32_t)-8),
        INST(OP_HALT, 0, 0, 0, 0),
    };
    VM *vm = vm_create(MEM_SIZE, program,
                       sizeof(program) / sizeof(program[0]),
                       NULL, 0, NULL, 1);
    uint64_t start_ns;
    uint64_t elapsed_ns;
    uint64_t executed;
    double mips;
    int ok;

    if (!vm) {
        fprintf(stderr, "[benchmark] failed to create VM (%s)\n", label);
        return 1;
    }

    /* Zero disables the virtual clock limiter so this measures the host-side
     * single-core interpreter ceiling rather than the configured vCPU clock. */
    vm->cpu_frequency_hz = 0u;
    vm->execution_engine = engine;
    init_ivt(vm);
    if (enable_mmu) {
        const uint32_t page = (uint32_t)PROGRAM_BASE & ~0xFFFu;
        const uint32_t pde_index = ((uint32_t)PROGRAM_BASE >> 22) & 0x3FFu;
        const uint32_t pte_index = ((uint32_t)PROGRAM_BASE >> 12) & 0x3FFu;
        const uint32_t perms = MMU_PTE_P | MMU_PTE_W | MMU_PTE_X;
        store_le32(&vm->memory[BENCHMARK_MMU_ROOT + pde_index * 4u],
                   BENCHMARK_MMU_L2 | perms);
        store_le32(&vm->memory[BENCHMARK_MMU_L2 + pte_index * 4u],
                   page | perms);
        vm->mmu.root[0] = BENCHMARK_MMU_ROOT;
        vm->mmu.ctrl[0] = MMU_CTRL_ENABLE;
        vm_mmu_flush_tlb(vm, 0u);
    }
    start_ns = host_monotonic_time_ns();
    ok = vm_run_headless(vm, 30000u);
    elapsed_ns = host_monotonic_time_ns() - start_ns;
    executed = atomic_load_explicit(&vm->cpus[0].execution_times,
                                    memory_order_relaxed);
    mips = elapsed_ns != 0u
        ? ((double)executed * 1000.0) / (double)elapsed_ns
        : 0.0;

    printf("[benchmark] %-7s single-core %-8s: %.1f MIPS "
           "(%llu instructions, %.1f ms)\n",
           benchmark_engine_name(engine),
           label,
           mips,
           (unsigned long long)executed,
           (double)elapsed_ns / 1000000.0);

    ok = ok && !atomic_is_vm_panicked(vm) &&
         executed == expected_instructions &&
         vm->cpus[0].regs[1] == 0u;
    vm_destroy(vm);
    if (!ok) {
        fprintf(stderr,
                "[benchmark] invalid %s result: expected %llu instructions\n",
                label,
                (unsigned long long)expected_instructions);
        return 1;
    }
    return 0;
}

static int run_memory_benchmark_mode(const char *label,
                                     int enable_mmu,
                                     VmExecutionEngine engine) {
    enum {
        BENCHMARK_ITERATIONS = 5000000u,
        BENCHMARK_MMU_ROOT = 0x4000u,
        BENCHMARK_MMU_L2 = 0x5000u,
        BENCHMARK_DATA = 0x6000u,
    };
    const uint64_t expected_instructions =
        5ull * (uint64_t)BENCHMARK_ITERATIONS + 3ull;
    uint64_t program[] = {
        INST(OP_MOVI, 1, 0, 0, BENCHMARK_ITERATIONS),
        INST(OP_MOVI, 2, 0, 0, BENCHMARK_DATA),
        INST(OP_LOAD32, 3, 2, 0, 0),
        INST(OP_ADDI, 3, 3, 0, 1),
        INST(OP_STORE32, 3, 2, 0, 0),
        INST(OP_SUBI, 1, 1, 0, 1),
        INST(OP_RJNZ, 0, 0, 0, (uint32_t)-32),
        INST(OP_HALT, 0, 0, 0, 0),
    };
    VM *vm = vm_create(MEM_SIZE, program,
                       sizeof(program) / sizeof(program[0]),
                       NULL, 0, NULL, 1);
    uint64_t start_ns;
    uint64_t elapsed_ns;
    uint64_t executed;
    double mips;
    int ok;

    if (!vm) {
        fprintf(stderr, "[benchmark] failed to create VM (%s)\n", label);
        return 1;
    }
    vm->cpu_frequency_hz = 0u;
    vm->execution_engine = engine;
    init_ivt(vm);
    if (enable_mmu) {
        const uint32_t code_page = (uint32_t)PROGRAM_BASE & ~0xFFFu;
        const uint32_t pde_index = ((uint32_t)PROGRAM_BASE >> 22) & 0x3FFu;
        const uint32_t code_pte = ((uint32_t)PROGRAM_BASE >> 12) & 0x3FFu;
        const uint32_t data_pte = BENCHMARK_DATA >> 12;
        const uint32_t perms = MMU_PTE_P | MMU_PTE_W | MMU_PTE_X;
        store_le32(&vm->memory[BENCHMARK_MMU_ROOT + pde_index * 4u],
                   BENCHMARK_MMU_L2 | perms);
        store_le32(&vm->memory[BENCHMARK_MMU_L2 + code_pte * 4u],
                   code_page | perms);
        store_le32(&vm->memory[BENCHMARK_MMU_L2 + data_pte * 4u],
                   BENCHMARK_DATA | perms);
        vm->mmu.root[0] = BENCHMARK_MMU_ROOT;
        vm->mmu.ctrl[0] = MMU_CTRL_ENABLE;
        vm_mmu_flush_tlb(vm, 0u);
    }

    start_ns = host_monotonic_time_ns();
    ok = vm_run_headless(vm, 30000u);
    elapsed_ns = host_monotonic_time_ns() - start_ns;
    executed = atomic_load_explicit(&vm->cpus[0].execution_times,
                                    memory_order_relaxed);
    mips = elapsed_ns != 0u
        ? ((double)executed * 1000.0) / (double)elapsed_ns
        : 0.0;
    printf("[benchmark] %-7s single-core %-8s: %.1f MIPS "
           "(%llu instructions, %.1f ms)\n",
           benchmark_engine_name(engine),
           label,
           mips,
           (unsigned long long)executed,
           (double)elapsed_ns / 1000000.0);

    ok = ok && !atomic_is_vm_panicked(vm) &&
         executed == expected_instructions &&
         load_le32(&vm->memory[BENCHMARK_DATA]) == BENCHMARK_ITERATIONS;
    vm_destroy(vm);
    return ok ? 0 : 1;
}

int run_benchmark(void) {
    int ok = 1;
    const VmExecutionEngine engines[] = {
        VM_ENGINE_CLASSIC,
        VM_ENGINE_CACHED,
        VM_ENGINE_THREADED,
        VM_ENGINE_JIT,
    };
    for (size_t i = 0u; i < sizeof(engines) / sizeof(engines[0]); i++) {
        const VmExecutionEngine engine = engines[i];
        ok = ok && run_interpreter_benchmark_mode("flat", 0, engine) == 0;
        ok = ok && run_interpreter_benchmark_mode("MMU", 1, engine) == 0;
        ok = ok && run_memory_benchmark_mode("memory", 0, engine) == 0;
        ok = ok && run_memory_benchmark_mode("memory+MMU", 1, engine) == 0;
    }
    return ok ? 0 : 1;
}
