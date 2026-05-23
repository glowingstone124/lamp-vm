#include <stdint.h>

// VM memory/layout constants
#define MEM_SIZE (64u * 1024u * 1024u)
#define IVT_BASE 0x0000u
#define IVT_ENTRY_SIZE 8u
#define SYSINFO_MMIO_BASE 0x0074C000u
#define SYSINFO_REG_FEATURES 0x40u
#define SYSINFO_FEATURE_INTC_MMIO (1u << 5)
#define SYSINFO_MAGIC 0x31494D56u /* "VMI1" */
#define SYSINFO_COPY_START 0x04u
#define SYSINFO_COPY_END 0x58u
#define INTC_MMIO_BASE 0x0074D000u
#define INTC_REG_ENABLE 0x040u
#define INTC_REG_PRIORITY 0x100u
#define INTC_REG_EOI 0x500u
#define BOOTINFO_ADDR 0x002FF000u
#define BOOTINFO_MAGIC 0x3049424Cu /* "LBI0" */
#define BOOTINFO_VERSION 2u
#define BOOTINFO_SIZE 0x64u

// Disk IO ports (match io.h)
#define DISK_CMD   0x10
#define DISK_LBA   0x11
#define DISK_MEM   0x12
#define DISK_COUNT 0x13
#define DISK_STATUS 0x14

#define DISK_CMD_READ 1

#define INT_DISK_COMPLETE 0x02
#define INTC_WORD_COUNT 8u

// ELF settings
#define ELF_MAGIC0 0x7F
#define ELF_MAGIC1 'E'
#define ELF_MAGIC2 'L'
#define ELF_MAGIC3 'F'
#define ELFCLASS32 1
#define ELFDATA2LSB 1
#define ET_EXEC 2
#define PT_LOAD 1

// Kernel storage
#define KERNEL_LBA 1u
#define KERNEL_ELF_BUF 0x00300000u
#define KERNEL_ELF_MAX (512u * 1024u)

// Simple IO asm helpers
static inline void out_io(uint32_t addr, uint32_t val) {
    __asm__ volatile ("out %0, %1" :: "r"(val), "r"(addr));
}

static inline uint32_t in_io(uint32_t addr) {
    uint32_t v;
    __asm__ volatile ("in %0, %1" : "=r"(v) : "r"(addr));
    return v;
}

static inline void halt(void) {
    __asm__ volatile ("halt");
}

static inline void write_u32(uint32_t addr, uint32_t v) {
    *(volatile uint32_t *)(uintptr_t)addr = v;
}

static inline void write_u64(uint32_t addr, uint64_t v) {
    write_u32(addr, (uint32_t)(v & 0xFFFFFFFFu));
    write_u32(addr + 4, (uint32_t)(v >> 32));
}

static inline uint32_t read_u32(uint32_t addr) {
    return *(volatile uint32_t *)(uintptr_t)addr;
}

static inline void *memcpy8(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

static inline void *memset8(void *dst, uint8_t v, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (uint32_t i = 0; i < n; i++) d[i] = v;
    return dst;
}

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf32_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} Elf32_Phdr;

static volatile uint32_t disk_done = 0;
static volatile uint32_t g_intc_available = 0u;

static inline void intc_write32(uint32_t reg, uint32_t value) {
    write_u32(INTC_MMIO_BASE + reg, value);
}

static void bios_intc_init(void) {
    const uint32_t sysinfo_magic = read_u32(SYSINFO_MMIO_BASE + 0x00u);
    if (sysinfo_magic != SYSINFO_MAGIC) {
        g_intc_available = 0u;
        return;
    }

    const uint32_t features = read_u32(SYSINFO_MMIO_BASE + SYSINFO_REG_FEATURES);
    if ((features & SYSINFO_FEATURE_INTC_MMIO) == 0u) {
        g_intc_available = 0u;
        return;
    }

    g_intc_available = 1u;
    for (uint32_t i = 0; i < INTC_WORD_COUNT; i++) {
        intc_write32(INTC_REG_ENABLE + i * 4u, 0u);
    }
    intc_write32(INTC_REG_PRIORITY + INT_DISK_COMPLETE * 4u, 0xE0u);
    intc_write32(INTC_REG_ENABLE + (INT_DISK_COMPLETE / 32u) * 4u,
                 (1u << (INT_DISK_COMPLETE % 32u)));
}

static void bios_publish_boot_info(void) {
    uint32_t magic = read_u32(SYSINFO_MMIO_BASE + 0x00u);
    if (magic != SYSINFO_MAGIC) {
        write_u32(BOOTINFO_ADDR + 0x00u, 0u);
        return;
    }

    write_u32(BOOTINFO_ADDR + 0x00u, BOOTINFO_MAGIC);
    write_u32(BOOTINFO_ADDR + 0x04u, BOOTINFO_VERSION);
    write_u32(BOOTINFO_ADDR + 0x08u, BOOTINFO_SIZE);

    uint32_t bootinfo_off = 0x0Cu;
    for (uint32_t sysinfo_off = SYSINFO_COPY_START;
         sysinfo_off <= SYSINFO_COPY_END;
         sysinfo_off += 4u, bootinfo_off += 4u) {
        write_u32(BOOTINFO_ADDR + bootinfo_off, read_u32(SYSINFO_MMIO_BASE + sysinfo_off));
    }
}

__attribute__((naked)) void isr_disk_complete(void) {
    __asm__ volatile (
        "movi r0, disk_done\n"
        "movi r1, 1\n"
        "store32 r1, r0, 0\n"
        "movi r0, g_intc_available\n"
        "load32 r1, r0, 0\n"
        "jz r1, bios_isr_disk_done_eoi_skip\n"
        "movi r0, 0x0074D500\n"
        "movi r1, 2\n"
        "store32 r1, r0, 0\n"
        "bios_isr_disk_done_eoi_skip:\n"
        "iret\n"
    );
}

static void register_isr(uint32_t int_no, void (*handler)(void)) {
    uint32_t addr = IVT_BASE + int_no * IVT_ENTRY_SIZE;
    write_u64(addr, (uint32_t)(uintptr_t)handler);
}

static void disk_read_sectors(uint32_t lba, uint32_t count, uint32_t mem_addr) {
    disk_done = 0;
    out_io(DISK_LBA, lba);
    out_io(DISK_MEM, mem_addr);
    out_io(DISK_COUNT, count);
    out_io(DISK_CMD, DISK_CMD_READ);

    // Wait for interrupt
    while (!disk_done) {
        // optional: spin
    }
}

static int elf_validate(const Elf32_Ehdr *eh) {
    return eh->e_ident[0] == ELF_MAGIC0 &&
           eh->e_ident[1] == ELF_MAGIC1 &&
           eh->e_ident[2] == ELF_MAGIC2 &&
           eh->e_ident[3] == ELF_MAGIC3 &&
           eh->e_ident[4] == ELFCLASS32 &&
           eh->e_ident[5] == ELFDATA2LSB &&
           eh->e_type == ET_EXEC;
}

static uint32_t elf_file_size(const Elf32_Ehdr *eh) {
    const Elf32_Phdr *ph = (const Elf32_Phdr *)((const uint8_t *)eh + eh->e_phoff);
    uint32_t max_end = 0;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        uint32_t end = ph[i].p_offset + ph[i].p_filesz;
        if (end > max_end) max_end = end;
    }
    // ensure we at least cover headers
    uint32_t hdr_end = eh->e_phoff + (uint32_t)eh->e_phentsize * eh->e_phnum;
    if (hdr_end > max_end) max_end = hdr_end;
    return max_end;
}

static void bios_debug_hex32(uint32_t v) {
    static const char hx[] = "0123456789ABCDEF";
    out_io(0x01, hx[(v>>28)&0xF]); out_io(0x01, hx[(v>>24)&0xF]);
    out_io(0x01, hx[(v>>20)&0xF]); out_io(0x01, hx[(v>>16)&0xF]);
    out_io(0x01, hx[(v>>12)&0xF]); out_io(0x01, hx[(v>> 8)&0xF]);
    out_io(0x01, hx[(v>> 4)&0xF]); out_io(0x01, hx[v&0xF]);
}

static void elf_load_and_jump(void) {
    // Read initial 4KB for headers
    disk_read_sectors(KERNEL_LBA, 8, KERNEL_ELF_BUF);

    Elf32_Ehdr *eh = (Elf32_Ehdr *)(uintptr_t)KERNEL_ELF_BUF;
    // Early debug: show raw e_entry from first 4KB read
    out_io(0x01, 'H'); out_io(0x01, '=');
    bios_debug_hex32(eh->e_entry);
    out_io(0x01, '\n');
    if (!elf_validate(eh)) {
        out_io(0x01, 'B'); out_io(0x01, 'A'); out_io(0x01, 'D');
        out_io(0x01, 'E'); out_io(0x01, 'L'); out_io(0x01, 'F'); out_io(0x01, '\n');
        halt();
    }

    uint32_t file_size = elf_file_size(eh);
    if (file_size == 0 || file_size > KERNEL_ELF_MAX) {
        halt();
    }

    uint32_t total_sectors = (file_size + 511u) / 512u;
    disk_read_sectors(KERNEL_LBA, total_sectors, KERNEL_ELF_BUF);

    // Save entry point BEFORE loading segments — BSS zeroing may
    // overwrite the ELF buffer (KERNEL_ELF_BUF) if the kernel BSS
    // extends past 0x00300000.
    uint32_t saved_entry = eh->e_entry;

    // Load segments
    const Elf32_Phdr *ph = (const Elf32_Phdr *)((const uint8_t *)eh + eh->e_phoff);
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        uint32_t dst_addr = ph[i].p_paddr ? ph[i].p_paddr : ph[i].p_vaddr;
        const uint8_t *src = (const uint8_t *)eh + ph[i].p_offset;
        uint8_t *dst = (uint8_t *)(uintptr_t)dst_addr;
        if (ph[i].p_filesz) {
            memcpy8(dst, src, ph[i].p_filesz);
        }
        if (ph[i].p_memsz > ph[i].p_filesz) {
            memset8(dst + ph[i].p_filesz, 0, ph[i].p_memsz - ph[i].p_filesz);
        }
    }

    // Publish boot info AFTER loading segments — BSS clearing may
    // have overwritten a previous write to BOOTINFO_ADDR.
    bios_publish_boot_info();

    // Jump to kernel entry (use saved value, eh buffer may be zeroed)
    void (*entry)(void) = (void (*)(void))(uintptr_t)saved_entry;
    // Debug: write entry point to serial port 0x01 before jumping
    {
        static const char hex[] = "0123456789ABCDEF";
        uint32_t ep = (uint32_t)(uintptr_t)entry;
        out_io(0x01, 'E'); out_io(0x01, '=');
        out_io(0x01, hex[(ep >> 28) & 0xF]);
        out_io(0x01, hex[(ep >> 24) & 0xF]);
        out_io(0x01, hex[(ep >> 20) & 0xF]);
        out_io(0x01, hex[(ep >> 16) & 0xF]);
        out_io(0x01, hex[(ep >> 12) & 0xF]);
        out_io(0x01, hex[(ep >>  8) & 0xF]);
        out_io(0x01, hex[(ep >>  4) & 0xF]);
        out_io(0x01, hex[ep & 0xF]);
        out_io(0x01, '\n');
    }
    entry();
}

void bios_main(void) {
    bios_intc_init();
    register_isr(INT_DISK_COMPLETE, isr_disk_complete);
    /* boot info is published inside elf_load_and_jump after BSS clear */
    elf_load_and_jump();
    halt();
}

__asm__(
    ".text\n"
    ".globl _start\n"
    "_start:\n"
    "  movi r30, 4194304\n"
    "  call bios_main\n"
    "  halt\n"
);
