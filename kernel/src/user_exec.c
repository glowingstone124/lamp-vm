#include "../include/kernel/fs.h"
#include "../include/kernel/mmu.h"
#include "../include/kernel/platform.h"
#include "../include/kernel/printk.h"
#include "../include/kernel/irq.h"
#include "../include/kernel/trap.h"
#include "../include/kernel/sched.h"
#include "../include/kernel/spinlock.h"
#include "../include/kernel/syscall.h"
#include "../include/kernel/user_exec.h"

enum {
    USER_EXEC_MAX_TASKS = 4u,
    USER_EXEC_PATH_CAP = 96u,
    USER_EXEC_MAX_ARGV = 8u,
    USER_EXEC_MAX_ENVP = 8u,
    USER_EXEC_ARG_STR_CAP = 64u,
    USER_EXEC_ENV_STR_CAP = 64u,
    USER_EXEC_ELF_MAX = 512u * 1024u,
    USER_EXIT_CODE_LOAD_FAIL = 127u
};

enum {
    ELF_MAGIC0 = 0x7Fu,
    ELF_MAGIC1 = 'E',
    ELF_MAGIC2 = 'L',
    ELF_MAGIC3 = 'F',
    ELFCLASS32 = 1u,
    ELFDATA2LSB = 1u,
    ET_EXEC = 2u,
    PT_LOAD = 1u,
    PT_PHDR = 6u,
    PF_X = 0x1u,
    PF_W = 0x2u,
    PF_R = 0x4u
};

enum {
    AT_NULL = 0u,
    AT_PHDR = 3u,
    AT_PHENT = 4u,
    AT_PHNUM = 5u,
    AT_PAGESZ = 6u,
    AT_ENTRY = 9u
};

typedef struct __attribute__((packed)) {
    uint8_t e_ident[16];
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
} elf32_ehdr_t;

typedef struct __attribute__((packed)) {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} elf32_phdr_t;

typedef struct {
    uint32_t used;
    char path[USER_EXEC_PATH_CAP];
    uint32_t argc;
    uint32_t envc;
    char argv[USER_EXEC_MAX_ARGV][USER_EXEC_ARG_STR_CAP];
    char envp[USER_EXEC_MAX_ENVP][USER_EXEC_ENV_STR_CAP];
} user_exec_ctx_t;

static spinlock_t g_user_exec_lock;
static volatile uint32_t g_user_exec_inited;
static user_exec_ctx_t g_user_exec_ctx[USER_EXEC_MAX_TASKS];
static uint8_t g_user_exec_elf_buf[USER_EXEC_ELF_MAX];
static volatile uint32_t g_user_exec_cache_valid;
static uint32_t g_user_exec_cache_size;
static char g_user_exec_cache_path[USER_EXEC_PATH_CAP];
static uint8_t g_user_exec_cache_buf[USER_EXEC_ELF_MAX];
volatile uint32_t g_user_exec_saved_sp[32];
volatile uint32_t g_user_exec_saved_csp[32];
static volatile uint32_t g_user_exec_enter_entry[32];
static volatile uint32_t g_user_exec_enter_sp[32];

extern int user_exec_enter_asm(uint32_t entry, uint32_t stack_ptr);

static inline uint32_t user_exec_cpu_ctx_read32(uint32_t io_addr) {
    uint32_t value;
    __asm__ volatile("in %0, %1" : "=r"(value) : "r"(io_addr));
    return value;
}

static inline uint32_t user_exec_cpu_index(void) {
    uint32_t cpu = 0u;
    __asm__ volatile("cpuid %0" : "=r"(cpu));
    if (cpu >= 32u) {
        cpu = 0u;
    }
    return cpu;
}

static inline void user_exec_cpu_ctx_write32(uint32_t io_addr, uint32_t value) {
    __asm__ volatile("out %0, %1" :: "r"(value), "r"(io_addr));
}

static inline void user_exec_isr_write_u32(uint32_t isr_base, uint32_t index, uint32_t value) {
    volatile uint32_t *slot = (volatile uint32_t *)(uintptr_t)(isr_base + index * 8u);
    slot[0] = value;
    slot[1] = 0u;
}

__attribute__((noreturn)) static void user_exec_enter_iret_current(uint32_t entry, uint32_t stack_ptr) {
    uint32_t isr_base = user_exec_cpu_ctx_read32(IO_CPU_CTX_ISR_BASE);
    uint32_t isp = user_exec_cpu_ctx_read32(IO_CPU_CTX_ISP);

    if (isp > (VM_ISR_STACK_ENTRIES - 34u)) {
        sched_exit_code(USER_EXIT_CODE_LOAD_FAIL);
    }

    for (uint32_t reg = 0u; reg < 32u; reg++) {
        user_exec_isr_write_u32(isr_base, isp + (31u - reg), 0u);
    }
    user_exec_isr_write_u32(isr_base, isp + (31u - 30u), stack_ptr);
    user_exec_isr_write_u32(isr_base, isp + (31u - 31u), stack_ptr);
    user_exec_isr_write_u32(isr_base, isp + 32u, 0u);
    user_exec_isr_write_u32(isr_base, isp + 33u, entry);

    user_exec_cpu_ctx_write32(IO_CPU_CTX_DSP, VM_DATA_STACK_ENTRIES);
    user_exec_cpu_ctx_write32(IO_CPU_CTX_IRQ_MASK, 0u);
    __asm__ volatile(
        "out %0, %1\n"
        "iret\n"
        :
        : "r"(VM_CALL_STACK_ENTRIES), "r"(IO_CPU_CTX_CSP)
        : "memory");
    __builtin_unreachable();
}

__attribute__((noreturn)) static void user_exec_enter_via_irq(uint32_t entry, uint32_t stack_ptr) {
    uint32_t cpu = user_exec_cpu_index();
    uint32_t irq = IRQ_USER_EXEC_ENTER;
    g_user_exec_enter_entry[cpu] = entry;
    g_user_exec_enter_sp[cpu] = stack_ptr;
    __asm__ volatile("int %0" :: "r"(irq) : "memory");
    __builtin_unreachable();
}

static void user_exec_irq_enter(uint32_t irq_no) {
    uint32_t cpu;
    uint32_t entry;
    uint32_t stack_ptr;
    if (irq_no != IRQ_USER_EXEC_ENTER) {
        sched_exit_code(USER_EXIT_CODE_LOAD_FAIL);
    }
    cpu = user_exec_cpu_index();
    entry = g_user_exec_enter_entry[cpu];
    stack_ptr = g_user_exec_enter_sp[cpu];
    g_user_exec_enter_entry[cpu] = 0u;
    g_user_exec_enter_sp[cpu] = 0u;
    user_exec_enter_iret_current(entry, stack_ptr);
}

__asm__(
    ".text\n"
    ".globl user_exec_enter_asm\n"
    "user_exec_enter_asm:\n"
    "  cpuid r3\n"
    "  add r3, r3, r3\n"
    "  add r3, r3, r3\n"
    "  movi r2, g_user_exec_saved_sp\n"
    "  add r2, r2, r3\n"
    "  store32 r30, r2, 0\n"
    "  movi r2, g_user_exec_saved_csp\n"
    "  add r2, r2, r3\n"
    "  store32 r31, r2, 0\n"
    "  mov r30, r1\n"
    "  mov r31, r1\n"
    "  callr r0\n"
    "  cpuid r3\n"
    "  add r3, r3, r3\n"
    "  add r3, r3, r3\n"
    "  movi r2, g_user_exec_saved_sp\n"
    "  add r2, r2, r3\n"
    "  load32 r30, r2, 0\n"
    "  movi r2, g_user_exec_saved_csp\n"
    "  add r2, r2, r3\n"
    "  load32 r31, r2, 0\n"
    "  ret\n"
);

static void user_exec_lazy_init(void) {
    if (g_user_exec_inited != 0u) {
        return;
    }
    spinlock_init(&g_user_exec_lock);
    trap_register(IRQ_USER_EXEC_ENTER, user_exec_irq_enter);
    irq_set_priority(IRQ_USER_EXEC_ENTER, 0xE8u);
    irq_enable(IRQ_USER_EXEC_ENTER);
    g_user_exec_cache_valid = 0u;
    g_user_exec_cache_size = 0u;
    g_user_exec_cache_path[0] = '\0';
    g_user_exec_inited = 1u;
}

static inline uint32_t user_exec_vm_read32(uint32_t addr) {
    return *(volatile uint32_t *)(uintptr_t)addr;
}

static inline uint64_t user_exec_monotonic_ns(void) {
    uint32_t hi_a;
    uint32_t lo;
    uint32_t hi_b;
    do {
        hi_a = user_exec_vm_read32(TIMER_MMIO_BASE + 0x10u);
        lo = user_exec_vm_read32(TIMER_MMIO_BASE + 0x0Cu);
        hi_b = user_exec_vm_read32(TIMER_MMIO_BASE + 0x10u);
    } while (hi_a != hi_b);
    return ((uint64_t)hi_a << 32) | (uint64_t)lo;
}

static uint32_t str_copy_trunc(char *dst, uint32_t cap, const char *src) {
    uint32_t i = 0u;
    if (!dst || cap == 0u) {
        return 0u;
    }
    if (!src) {
        dst[0] = '\0';
        return 0u;
    }
    while (i + 1u < cap && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
    return i;
}

static int str_copy_exact(char *dst, uint32_t cap, const char *src) {
    uint32_t n = str_copy_trunc(dst, cap, src);
    if (!src) {
        return 0;
    }
    return (src[n] == '\0') ? 0 : -1;
}

static uint32_t str_len(const char *s) {
    uint32_t n = 0u;
    if (!s) {
        return 0u;
    }
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static uint32_t str_eq(const char *a, const char *b) {
    uint32_t i = 0u;
    if (!a || !b) {
        return 0u;
    }
    while (a[i] != '\0' && b[i] != '\0') {
        if (a[i] != b[i]) {
            return 0u;
        }
        i++;
    }
    return (a[i] == '\0' && b[i] == '\0') ? 1u : 0u;
}

static void mem_copy_u8(uint8_t *dst, const uint8_t *src, uint32_t n) {
    if (!dst || !src || n == 0u) {
        return;
    }
    for (uint32_t i = 0u; i < n; i++) {
        dst[i] = src[i];
    }
}

static void mem_zero_u8(uint8_t *dst, uint32_t n) {
    if (!dst || n == 0u) {
        return;
    }
    for (uint32_t i = 0u; i < n; i++) {
        dst[i] = 0u;
    }
}

static void user_exec_ctx_reset(user_exec_ctx_t *ctx) {
    if (!ctx) {
        return;
    }
    mem_zero_u8((uint8_t *)ctx, (uint32_t)sizeof(*ctx));
}

static int user_exec_read_file(const char *path, uint32_t *out_size) {
    int fd;
    int n;
    uint64_t t_open_begin;
    uint64_t t_open_end;
    uint64_t t_read_end;
    uint32_t fs_backend = 0u;
    uint32_t file_id = 0u;
    uint32_t file_size = 0u;
    uint32_t file_off = 0u;
    uint32_t is_dir = 0u;
    uint32_t done = 0u;

    user_exec_lazy_init();

    if (!path || !out_size) {
        return FS_ERR_INVAL;
    }

    spinlock_lock(&g_user_exec_lock);
    if (g_user_exec_cache_valid != 0u &&
        g_user_exec_cache_size != 0u &&
        g_user_exec_cache_size <= USER_EXEC_ELF_MAX &&
        str_eq(path, &g_user_exec_cache_path[0])) {
        mem_copy_u8(&g_user_exec_elf_buf[0], &g_user_exec_cache_buf[0], g_user_exec_cache_size);
        *out_size = g_user_exec_cache_size;
        spinlock_unlock(&g_user_exec_lock);
        return 0;
    }
    spinlock_unlock(&g_user_exec_lock);

    t_open_begin = user_exec_monotonic_ns();
    fd = fs_open(path, SYS_O_RDONLY);
    if (fd < 0) {
        return fd;
    }
    if (sched_fd_regular_get(fd, &fs_backend, &file_id, &file_size, &file_off, &is_dir) != SCHED_FD_OK || is_dir) {
        (void)sched_fd_close(fd);
        return FS_ERR_INVAL;
    }
    klog_begin(KLOG_LEVEL_INFO, "user_exec");
    klog_puts("open done fd=");
    klog_hex32((uint32_t)fd);
    klog_puts(" size=");
    klog_hex32(file_size);
    klog_puts(" path=");
    klog_puts(path);
    klog_end();
    t_open_end = user_exec_monotonic_ns();
    klog_begin(KLOG_LEVEL_INFO, "user_exec");
    klog_puts("open elapsed_ns=");
    klog_hex32((uint32_t)(t_open_end - t_open_begin));
    klog_puts(" path=");
    klog_puts(path);
    klog_end();
    if (file_size == 0u || file_size > USER_EXEC_ELF_MAX) {
        (void)sched_fd_close(fd);
        return FS_ERR_INVAL;
    }

    while (done < file_size) {
        n = fs_read(fd, &g_user_exec_elf_buf[done], file_size - done);
        if (n <= 0) {
            (void)sched_fd_close(fd);
            return FS_ERR_IO;
        }
        done += (uint32_t)n;
    }
    (void)sched_fd_close(fd);
    klog_begin(KLOG_LEVEL_INFO, "user_exec");
    klog_puts("read done bytes=");
    klog_hex32(done);
    klog_puts(" path=");
    klog_puts(path);
    klog_end();
    t_read_end = user_exec_monotonic_ns();
    klog_begin(KLOG_LEVEL_INFO, "user_exec");
    klog_puts("read elapsed_ns=");
    klog_hex32((uint32_t)(t_read_end - t_open_end));
    klog_puts(" path=");
    klog_puts(path);
    klog_end();
    spinlock_lock(&g_user_exec_lock);
    str_copy_trunc(&g_user_exec_cache_path[0], USER_EXEC_PATH_CAP, path);
    g_user_exec_cache_size = done;
    mem_copy_u8(&g_user_exec_cache_buf[0], &g_user_exec_elf_buf[0], done);
    g_user_exec_cache_valid = 1u;
    spinlock_unlock(&g_user_exec_lock);
    *out_size = file_size;
    return 0;
}

static int user_exec_stack_push_u32(uint32_t *sp, uint32_t floor, uint32_t value) {
    if (!sp || *sp < floor + 4u) {
        return -1;
    }
    *sp -= 4u;
    *(volatile uint32_t *)(uintptr_t)(*sp) = value;
    return 0;
}

static int user_exec_build_initial_stack(const user_image_t *img,
                                         const char *const argv[],
                                         const char *const envp[],
                                         uint32_t *out_sp) {
    uint32_t sp = USER_STACK_TOP;
    const uint32_t floor = USER_STACK_TOP - USER_STACK_RESERVE;
    uint32_t argc = 0u;
    uint32_t envc = 0u;
    uint32_t argv_addr[USER_EXEC_MAX_ARGV];
    uint32_t envp_addr[USER_EXEC_MAX_ENVP];
    uint32_t auxv[][2] = {
        {AT_PAGESZ, 4096u},
        {AT_ENTRY, img ? img->entry : 0u},
        {AT_PHDR, img ? img->phdr : 0u},
        {AT_PHENT, img ? img->phent : 0u},
        {AT_PHNUM, img ? img->phnum : 0u},
        {AT_NULL, 0u}
    };
    uint32_t auxc = (uint32_t)(sizeof(auxv) / sizeof(auxv[0]));

    if (!out_sp) {
        return -1;
    }

    while (argv && argv[argc] && argc < USER_EXEC_MAX_ARGV) {
        uint32_t n = str_len(argv[argc]) + 1u;
        if (sp < floor + n) {
            return -1;
        }
        sp -= n;
        for (uint32_t i = 0u; i < n; i++) {
            *(volatile uint8_t *)(uintptr_t)(sp + i) = (uint8_t)argv[argc][i];
        }
        argv_addr[argc] = sp;
        argc++;
    }

    while (envp && envp[envc] && envc < USER_EXEC_MAX_ENVP) {
        uint32_t n = str_len(envp[envc]) + 1u;
        if (sp < floor + n) {
            return -1;
        }
        sp -= n;
        for (uint32_t i = 0u; i < n; i++) {
            *(volatile uint8_t *)(uintptr_t)(sp + i) = (uint8_t)envp[envc][i];
        }
        envp_addr[envc] = sp;
        envc++;
    }

    sp &= ~0x3u;

    for (uint32_t i = auxc; i > 0u; i--) {
        if (user_exec_stack_push_u32(&sp, floor, auxv[i - 1u][1]) != 0 ||
            user_exec_stack_push_u32(&sp, floor, auxv[i - 1u][0]) != 0) {
            return -1;
        }
    }

    if (user_exec_stack_push_u32(&sp, floor, 0u) != 0) {
        return -1;
    }
    for (uint32_t i = envc; i > 0u; i--) {
        if (user_exec_stack_push_u32(&sp, floor, envp_addr[i - 1u]) != 0) {
            return -1;
        }
    }

    if (user_exec_stack_push_u32(&sp, floor, 0u) != 0) {
        return -1;
    }
    for (uint32_t i = argc; i > 0u; i--) {
        if (user_exec_stack_push_u32(&sp, floor, argv_addr[i - 1u]) != 0) {
            return -1;
        }
    }
    if (user_exec_stack_push_u32(&sp, floor, argc) != 0) {
        return -1;
    }

    *out_sp = sp;
    return 0;
}

int user_exec_load_elf_from_ext4(const char *path,
                                 const char *const argv[],
                                 const char *const envp[],
                                 user_image_t *out_img) {
    uint32_t file_size = 0u;
    int rc = FS_ERR_INVAL;
    const elf32_ehdr_t *eh = 0;
    uint32_t user_end = USER_REGION_BASE + USER_REGION_SIZE;
    uint32_t phdr_addr = 0u;
    user_image_t img;

    if (!path || !out_img) {
        return FS_ERR_INVAL;
    }

    rc = user_exec_read_file(path, &file_size);
    if (rc != 0) {
        return rc;
    }
    if (file_size < sizeof(elf32_ehdr_t)) {
        return FS_ERR_INVAL;
    }

    eh = (const elf32_ehdr_t *)(const void *)&g_user_exec_elf_buf[0];
    if (eh->e_ident[0] != ELF_MAGIC0 || eh->e_ident[1] != ELF_MAGIC1 || eh->e_ident[2] != ELF_MAGIC2 ||
        eh->e_ident[3] != ELF_MAGIC3 || eh->e_ident[4] != ELFCLASS32 || eh->e_ident[5] != ELFDATA2LSB ||
        eh->e_type != ET_EXEC) {
        return FS_ERR_INVAL;
    }
    if (eh->e_phentsize != sizeof(elf32_phdr_t) || eh->e_phnum == 0u) {
        return FS_ERR_INVAL;
    }
    if (eh->e_phoff > file_size || (uint32_t)eh->e_phnum > (file_size - eh->e_phoff) / sizeof(elf32_phdr_t)) {
        return FS_ERR_INVAL;
    }
    if (eh->e_entry < USER_REGION_BASE || eh->e_entry >= user_end) {
        return FS_ERR_INVAL;
    }

    for (uint32_t i = 0u; i < eh->e_phnum; i++) {
        const elf32_phdr_t *ph =
            (const elf32_phdr_t *)(const void *)(&g_user_exec_elf_buf[eh->e_phoff + i * sizeof(elf32_phdr_t)]);
        uint32_t seg_end;
        uint32_t prot = MMU_PROT_USER;
        uint32_t load_prot;
        uint8_t *dst;
        const uint8_t *src;

        if (ph->p_type == PT_PHDR) {
            phdr_addr = ph->p_vaddr;
        }
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0u) {
            continue;
        }
        if (ph->p_filesz > ph->p_memsz) {
            return FS_ERR_INVAL;
        }
        if (ph->p_offset > file_size || ph->p_filesz > (file_size - ph->p_offset)) {
            return FS_ERR_INVAL;
        }
        seg_end = ph->p_vaddr + ph->p_memsz;
        if (seg_end < ph->p_vaddr || ph->p_vaddr < USER_REGION_BASE || seg_end > user_end) {
            return FS_ERR_INVAL;
        }

        if ((ph->p_flags & PF_R) != 0u || (ph->p_flags & PF_W) == 0u) {
            prot |= MMU_PROT_READ;
        }
        if ((ph->p_flags & PF_W) != 0u) {
            prot |= MMU_PROT_WRITE;
        }
        if ((ph->p_flags & PF_X) != 0u) {
            prot |= MMU_PROT_EXEC;
        }

        /*
         * Loader writes segment bytes from kernel context, so map writable first.
         * After copy/zero, tighten back to final ELF-derived protection.
         */
        load_prot = prot | MMU_PROT_WRITE;
        if (mmu_map_identity(ph->p_vaddr, ph->p_memsz, load_prot) != 0) {
            return FS_ERR_INVAL;
        }

        dst = (uint8_t *)(uintptr_t)ph->p_vaddr;
        src = (const uint8_t *)(const void *)(&g_user_exec_elf_buf[ph->p_offset]);
        for (uint32_t j = 0u; j < ph->p_filesz; j++) {
            dst[j] = src[j];
        }
        for (uint32_t j = ph->p_filesz; j < ph->p_memsz; j++) {
            dst[j] = 0u;
        }

        if (mmu_map_identity(ph->p_vaddr, ph->p_memsz, prot) != 0) {
            return FS_ERR_INVAL;
        }
    }

    if (mmu_map_identity(USER_STACK_TOP - USER_STACK_RESERVE,
                         USER_STACK_RESERVE,
                         MMU_PROT_READ | MMU_PROT_WRITE | MMU_PROT_USER) != 0) {
        return FS_ERR_INVAL;
    }
    for (uint32_t i = 0u; i < USER_STACK_RESERVE; i++) {
        *(volatile uint8_t *)(uintptr_t)(USER_STACK_TOP - USER_STACK_RESERVE + i) = 0u;
    }

    img.entry = eh->e_entry;
    img.phdr = phdr_addr;
    img.phent = eh->e_phentsize;
    img.phnum = eh->e_phnum;
    if (user_exec_build_initial_stack(&img, argv, envp, &img.stack_ptr) != 0) {
        return FS_ERR_INVAL;
    }
    *out_img = img;
    return 0;
}

static void user_exec_release_ctx(user_exec_ctx_t *ctx) {
    if (!ctx) {
        return;
    }
    spinlock_lock(&g_user_exec_lock);
    user_exec_ctx_reset(ctx);
    spinlock_unlock(&g_user_exec_lock);
}

static int user_exec_enter(uint32_t entry, uint32_t stack_ptr) {
    return user_exec_enter_asm(entry, stack_ptr);
}

static void user_exec_task_entry(sched_task_t *task, void *arg) {
    user_exec_ctx_t *ctx = (user_exec_ctx_t *)arg;
    user_image_t img;
    const char *argv[USER_EXEC_MAX_ARGV + 1u];
    const char *envp[USER_EXEC_MAX_ENVP + 1u];
    uint32_t i;
    int rc;
    uint64_t t_load_begin;
    uint64_t t_load_end;

    if (!task || !ctx) {
        return;
    }
    sched_task_set_kind(SCHED_TASK_KIND_USER);
    sched_task_set_exec_state(SCHED_EXEC_STATE_USER);

    klog_begin(KLOG_LEVEL_INFO, "user_exec");
    klog_puts("load start path=");
    klog_puts(&ctx->path[0]);
    klog_end();
    t_load_begin = user_exec_monotonic_ns();

    for (i = 0u; i < ctx->argc && i < USER_EXEC_MAX_ARGV; i++) {
        argv[i] = &ctx->argv[i][0];
    }
    argv[i] = 0;
    for (i = 0u; i < ctx->envc && i < USER_EXEC_MAX_ENVP; i++) {
        envp[i] = &ctx->envp[i][0];
    }
    envp[i] = 0;

    rc = user_exec_load_elf_from_ext4(&ctx->path[0], argv, envp, &img);
    if (rc != 0) {
        klog_begin(KLOG_LEVEL_ERROR, "user_exec");
        klog_puts("load failed rc=");
        klog_hex32((uint32_t)rc);
        klog_puts(" path=");
        klog_puts(&ctx->path[0]);
        klog_end();
        user_exec_release_ctx(ctx);
        if (task->state != SCHED_TASK_ZOMBIE) {
            sched_exit_code(USER_EXIT_CODE_LOAD_FAIL);
        }
        return;
    }
    t_load_end = user_exec_monotonic_ns();

    klog_begin(KLOG_LEVEL_INFO, "user_exec");
    klog_puts("load done entry=");
    klog_hex32(img.entry);
    klog_puts(" sp=");
    klog_hex32(img.stack_ptr);
    klog_puts(" path=");
    klog_puts(&ctx->path[0]);
    klog_end();
    klog_begin(KLOG_LEVEL_INFO, "user_exec");
    klog_puts("load elapsed_ns=");
    klog_hex32((uint32_t)(t_load_end - t_load_begin));
    klog_puts(" path=");
    klog_puts(&ctx->path[0]);
    klog_end();

    user_exec_release_ctx(ctx);
    user_exec_enter_via_irq(img.entry, img.stack_ptr);
}

int user_exec_spawn_path(const char *path, const char *const argv[], const char *const envp[]) {
    user_exec_ctx_t *ctx = 0;
    int tid;
    uint32_t i;

    user_exec_lazy_init();

    if (!path || path[0] == '\0') {
        return -1;
    }

    spinlock_lock(&g_user_exec_lock);
    for (i = 0u; i < USER_EXEC_MAX_TASKS; i++) {
        if (g_user_exec_ctx[i].used == 0u) {
            ctx = &g_user_exec_ctx[i];
            user_exec_ctx_reset(ctx);
            ctx->used = 1u;
            break;
        }
    }
    spinlock_unlock(&g_user_exec_lock);
    if (!ctx) {
        return -1;
    }

    if (str_copy_exact(&ctx->path[0], USER_EXEC_PATH_CAP, path) != 0) {
        user_exec_release_ctx(ctx);
        return -1;
    }

    ctx->argc = 0u;
    if (argv && argv[0]) {
        while (ctx->argc < USER_EXEC_MAX_ARGV && argv[ctx->argc]) {
            if (str_copy_exact(&ctx->argv[ctx->argc][0], USER_EXEC_ARG_STR_CAP, argv[ctx->argc]) != 0) {
                user_exec_release_ctx(ctx);
                return -1;
            }
            ctx->argc++;
        }
        if (argv[ctx->argc]) {
            user_exec_release_ctx(ctx);
            return -1;
        }
    } else {
        if (str_copy_exact(&ctx->argv[0][0], USER_EXEC_ARG_STR_CAP, path) != 0) {
            user_exec_release_ctx(ctx);
            return -1;
        }
        ctx->argc = 1u;
    }

    ctx->envc = 0u;
    if (envp) {
        while (ctx->envc < USER_EXEC_MAX_ENVP && envp[ctx->envc]) {
            if (str_copy_exact(&ctx->envp[ctx->envc][0], USER_EXEC_ENV_STR_CAP, envp[ctx->envc]) != 0) {
                user_exec_release_ctx(ctx);
                return -1;
            }
            ctx->envc++;
        }
        if (envp[ctx->envc]) {
            user_exec_release_ctx(ctx);
            return -1;
        }
    }

    tid = sched_spawn("user", user_exec_task_entry, ctx);
    if (tid < 0) {
        user_exec_release_ctx(ctx);
        return -1;
    }
    return tid;
}

int user_exec_execve_current(const char *path, const char *const argv[], const char *const envp[]) {
    user_image_t img;

    if (!path || path[0] == '\0') {
        return FS_ERR_INVAL;
    }

    int rc = user_exec_load_elf_from_ext4(path, argv, envp, &img);
    if (rc != 0) {
        return rc;
    }

    if (sched_fd_close_cloexec() < 0) {
        return FS_ERR_INVAL;
    }

    sched_task_set_kind(SCHED_TASK_KIND_USER);
    sched_task_set_exec_state(SCHED_EXEC_STATE_USER);
    sched_vfork_release_parent();
    user_exec_enter_iret_current(img.entry, img.stack_ptr);
}
