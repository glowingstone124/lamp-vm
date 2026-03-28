#include "../include/kernel/console.h"
#include "../include/kernel/console_fb.h"
#include "../include/kernel/fd_selftest.h"
#include "../include/kernel/fs.h"
#include "../include/kernel/init_task.h"
#include "../include/kernel/printk.h"
#include "../include/kernel/sched.h"
#include "../include/kernel/spinlock.h"
#include "../include/kernel/types.h"
#include "../include/kernel/user_exec.h"

enum {
    INIT_LINE_CAP = 128u
};

typedef struct init_state {
    uint8_t line[INIT_LINE_CAP];
    uint32_t len;
    uint32_t started;
} init_state_t;

static init_state_t g_init_state;
static spinlock_t g_init_owner_lock;
static spinlock_t g_init_cmd_lock;
static volatile uint32_t g_init_owner_tid;
static volatile uint32_t g_init_busy;
static volatile uint32_t g_init_fdtest_running;
static uint8_t g_init_callsp_buf[128];

__attribute__((noinline)) static void init_mem_copy_u8(uint32_t dst_addr,
                                                       uint32_t src_addr,
                                                       uint32_t len) {
    volatile uint8_t *dst = (volatile uint8_t *)(uintptr_t)dst_addr;
    volatile const uint8_t *src = (volatile const uint8_t *)(uintptr_t)src_addr;
    for (uint32_t i = 0u; i < len; i++) {
        dst[i] = src[i];
    }
}

__attribute__((noinline)) static uint32_t init_addr_in_range(uint32_t v,
                                                             uint32_t lo,
                                                             uint32_t hi) {
    return (v >= lo && v < hi) ? 1u : 0u;
}

__attribute__((noinline)) static uint32_t init_addr_reloc(uint32_t v,
                                                          uint32_t lo,
                                                          uint32_t hi,
                                                          int32_t delta) {
    if (!init_addr_in_range(v, lo, hi)) {
        return v;
    }
    return (uint32_t)((int32_t)v + delta);
}

__attribute__((noinline)) static void init_reloc_words(uint32_t base,
                                                       uint32_t bytes,
                                                       uint32_t r0_lo,
                                                       uint32_t r0_hi,
                                                       int32_t d0,
                                                       uint32_t r1_lo,
                                                       uint32_t r1_hi,
                                                       int32_t d1) {
    uint32_t words = bytes / 4u;
    for (uint32_t i = 0u; i < words; i++) {
        volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)(base + i * 4u);
        uint32_t v = *p;
        v = init_addr_reloc(v, r0_lo, r0_hi, d0);
        v = init_addr_reloc(v, r1_lo, r1_hi, d1);
        *p = v;
    }
}

__attribute__((noinline)) static uint32_t init_callabi_prepare(const uint32_t *src,
                                                               uint32_t *dst,
                                                               uint32_t r0_lo,
                                                               uint32_t r0_hi,
                                                               int32_t d0,
                                                               uint32_t r1_lo,
                                                               uint32_t r1_hi,
                                                               int32_t d1) {
    uint32_t shadow[41];
    uint32_t out[41];
    uint32_t canary = 0x13579BDFu;
    init_mem_copy_u8((uint32_t)(uintptr_t)shadow,
                     (uint32_t)(uintptr_t)src,
                     (uint32_t)sizeof(shadow));
    init_reloc_words((uint32_t)(uintptr_t)shadow,
                     (uint32_t)sizeof(shadow),
                     r0_lo,
                     r0_hi,
                     d0,
                     r1_lo,
                     r1_hi,
                     d1);
    init_mem_copy_u8((uint32_t)(uintptr_t)out,
                     (uint32_t)(uintptr_t)shadow,
                     (uint32_t)sizeof(out));
    init_mem_copy_u8((uint32_t)(uintptr_t)dst,
                     (uint32_t)(uintptr_t)out,
                     (uint32_t)sizeof(out));
    return canary;
}

__attribute__((noinline)) static uint32_t init_callabi_big_prepare(const uint32_t *src,
                                                                   uint32_t *dst,
                                                                   uint32_t r0_lo,
                                                                   uint32_t r0_hi,
                                                                   int32_t d0,
                                                                   uint32_t r1_lo,
                                                                   uint32_t r1_hi,
                                                                   int32_t d1) {
    uint32_t pad0[24];
    uint32_t shadow[41];
    uint32_t pad1[20];
    uint32_t out[41];
    uint32_t pad2[28];
    uint32_t marker = 0x2468ACE0u;

    for (uint32_t i = 0u; i < 24u; i++) {
        pad0[i] = 0xA0000000u + i;
    }
    for (uint32_t i = 0u; i < 20u; i++) {
        pad1[i] = 0xB0000000u + i;
    }
    for (uint32_t i = 0u; i < 28u; i++) {
        pad2[i] = 0xC0000000u + i;
    }

    init_mem_copy_u8((uint32_t)(uintptr_t)shadow,
                     (uint32_t)(uintptr_t)src,
                     (uint32_t)sizeof(shadow));
    init_mem_copy_u8((uint32_t)(uintptr_t)out,
                     (uint32_t)(uintptr_t)shadow,
                     (uint32_t)sizeof(out));
    init_reloc_words((uint32_t)(uintptr_t)out,
                     (uint32_t)sizeof(out),
                     r0_lo,
                     r0_hi,
                     d0,
                     r1_lo,
                     r1_hi,
                     d1);
    init_mem_copy_u8((uint32_t)(uintptr_t)dst,
                     (uint32_t)(uintptr_t)out,
                     (uint32_t)sizeof(out));

    marker ^= pad0[3];
    marker ^= pad1[7];
    marker ^= pad2[11];
    return marker;
}

__attribute__((noinline)) static uint32_t init_callfi_prepare(const uint32_t *parent_in,
                                                              uint32_t *child_out,
                                                              uint32_t child_base,
                                                              uint32_t child_sp) {
    uint32_t pad0[24];
    uint32_t shadow[41];
    uint32_t pad1[20];

    for (uint32_t i = 0u; i < 24u; i++) {
        pad0[i] = 0xD0000000u + i;
    }
    for (uint32_t i = 0u; i < 20u; i++) {
        pad1[i] = 0xE0000000u + i;
    }

    init_mem_copy_u8((uint32_t)(uintptr_t)shadow,
                     (uint32_t)(uintptr_t)parent_in,
                     (uint32_t)sizeof(shadow));
    shadow[0] = child_base;
    shadow[39] = child_sp;
    init_mem_copy_u8((uint32_t)(uintptr_t)child_out,
                     (uint32_t)(uintptr_t)shadow,
                     (uint32_t)sizeof(shadow));

    return 0xCAFEBABEu ^ pad0[3] ^ pad1[7];
}

__attribute__((noinline)) static uint32_t init_callsp_prepare(const uint8_t *src,
                                                              uint32_t len) {
    uint32_t pad0[24];
    uint32_t pad1[20];

    for (uint32_t i = 0u; i < 24u; i++) {
        pad0[i] = 0x51000000u + i;
    }
    for (uint32_t i = 0u; i < 20u; i++) {
        pad1[i] = 0x52000000u + i;
    }

    init_mem_copy_u8((uint32_t)(uintptr_t)g_init_callsp_buf,
                     (uint32_t)(uintptr_t)src,
                     len);

    return 0x1234ABCDu ^ pad0[5] ^ pad1[9];
}

__attribute__((noinline)) static uint32_t init_callprep_prepare(const uint32_t *parent_in,
                                                                uint32_t *child_out,
                                                                uint32_t parent_slot_base,
                                                                uint32_t child_slot_base,
                                                                uint32_t parent_sp,
                                                                uint32_t live_top) {
    uint32_t slot_words[96];
    uint32_t live_words[96];
    uint32_t child_live[41];
    uint32_t live_active_bytes = live_top - parent_sp;
    uint32_t child_c_top = child_slot_base + 0x1000u;
    uint32_t child_live_sp = child_c_top - live_active_bytes;
    int32_t delta_slot = (int32_t)(child_slot_base - parent_slot_base);
    int32_t delta_live = (int32_t)(child_live_sp - parent_sp);

    for (uint32_t i = 0u; i < 96u; i++) {
        slot_words[i] = 0x71000000u + i * 4u;
        live_words[i] = 0x72000000u + i * 4u;
    }
    for (uint32_t i = 0u; i < 41u; i++) {
        child_live[i] = parent_in[i];
    }

    init_mem_copy_u8((uint32_t)(uintptr_t)slot_words,
                     (uint32_t)(uintptr_t)parent_in,
                     96u * 4u);
    init_mem_copy_u8((uint32_t)(uintptr_t)live_words,
                     (uint32_t)(uintptr_t)parent_in,
                     96u * 4u);

    child_live[0] = child_slot_base;
    child_live[39] = child_live_sp;
    child_live[40] = 0xA5A5A5A5u;

    for (uint32_t i = 8u; i < 32u; i++) {
        uint32_t v = child_live[9u + i];
        v = init_addr_reloc(v, parent_slot_base, parent_slot_base + 0x1000u, delta_slot);
        v = init_addr_reloc(v, parent_sp, live_top, delta_live);
        child_live[9u + i] = v;
    }

    init_reloc_words((uint32_t)(uintptr_t)slot_words,
                     96u * 4u,
                     parent_slot_base,
                     parent_slot_base + 0x1000u,
                     delta_slot,
                     parent_sp,
                     live_top,
                     delta_live);
    init_reloc_words((uint32_t)(uintptr_t)live_words,
                     96u * 4u,
                     parent_slot_base,
                     parent_slot_base + 0x1000u,
                     delta_slot,
                     parent_sp,
                     live_top,
                     delta_live);

    init_mem_copy_u8((uint32_t)(uintptr_t)child_out,
                     (uint32_t)(uintptr_t)child_live,
                     (uint32_t)sizeof(child_live));

    return 0x55667788u;
}

static void init_puts(const char *s) {
    uint32_t len = 0u;
    if (!s) {
        return;
    }
    while (s[len] != '\0') {
        len++;
    }
    if (len != 0u) {
        (void)console_write((const uint8_t *)s, len);
    }
}

static uint32_t init_streq(const char *a, const char *b) {
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

static uint32_t init_starts_with(const char *s, const char *prefix) {
    uint32_t i = 0u;
    if (!s || !prefix) {
        return 0u;
    }
    while (prefix[i] != '\0') {
        if (s[i] != prefix[i]) {
            return 0u;
        }
        i++;
    }
    return 1u;
}

static void init_prompt(void) {
    init_puts("init$ ");
}

static void init_show_tty_mode(void) {
    init_puts("tty lflag=");
    kprint_hex32(console_tty_get_lflag());
    init_puts(" [");
    if ((console_tty_get_lflag() & TTY_LFLAG_ECHO) != 0u) {
        init_puts("ECHO ");
    }
    if ((console_tty_get_lflag() & TTY_LFLAG_ICANON) != 0u) {
        init_puts("ICANON ");
    }
    if ((console_tty_get_lflag() & TTY_LFLAG_ISIG) != 0u) {
        init_puts("ISIG ");
    }
    init_puts("]\n");
}

static void init_set_flag(uint32_t flag, uint32_t on) {
    uint32_t v = console_tty_get_lflag();
    if (on) {
        v |= flag;
    } else {
        v &= ~flag;
    }
    (void)console_tty_set_lflag(v);
    init_show_tty_mode();
}

static void init_show_poll_state(void) {
    init_puts("stdin can_read=");
    kprint_hex32(console_can_read());
    init_puts(" lines=");
    kprint_hex32(console_rx_lines());
    init_puts(" dropped=");
    kprint_hex32(console_rx_dropped());
    init_puts("\n");
}

static void init_run_callabi(void) {
    uint32_t src[41];
    uint32_t dst[41];
    volatile uint32_t before = 0x11223344u;
    volatile uint32_t after = 0x55667788u;
    volatile uint32_t mid = 0x99AABBCCu;
    uint32_t rc_small;
    uint32_t rc_big;
    uint32_t fail = 0u;

    for (uint32_t i = 0u; i < 41u; i++) {
        src[i] = 0x60000000u + i * 4u;
        dst[i] = 0u;
    }
    src[0] = 0x1004u;
    src[1] = 0x1010u;
    src[2] = 0x2008u;
    src[3] = 0x2014u;
    src[4] = 0x3000u;

    rc_small = init_callabi_prepare(src,
                                    dst,
                                    0x1000u,
                                    0x1100u,
                                    0x20,
                                    0x2000u,
                                    0x2100u,
                                    -0x10);

    if (before != 0x11223344u || after != 0x55667788u || mid != 0x99AABBCCu) {
        fail = 1u;
    }
    if (rc_small != 0x13579BDFu) {
        fail = 1u;
    }
    if (dst[0] != 0x1024u || dst[1] != 0x1030u || dst[2] != 0x1FF8u ||
        dst[3] != 0x2004u || dst[4] != 0x3000u) {
        fail = 1u;
    }

    for (uint32_t i = 0u; i < 41u; i++) {
        dst[i] = 0u;
    }
    rc_big = init_callabi_big_prepare(src,
                                      dst,
                                      0x1000u,
                                      0x1100u,
                                      0x20,
                                      0x2000u,
                                      0x2100u,
                                      -0x10);
    if (before != 0x11223344u || after != 0x55667788u || mid != 0x99AABBCCu) {
        fail = 1u;
    }
    if (rc_big != (0x2468ACE0u ^ (0xA0000000u + 3u) ^ (0xB0000000u + 7u) ^
                   (0xC0000000u + 11u))) {
        fail = 1u;
    }
    if (dst[0] != 0x1024u || dst[1] != 0x1030u || dst[2] != 0x1FF8u ||
        dst[3] != 0x2004u || dst[4] != 0x3000u) {
        fail = 1u;
    }

    if (fail == 0u) {
        init_puts("callabi ok\n");
        return;
    }

    init_puts("callabi fail rc_small=");
    kprint_hex32(rc_small);
    init_puts(" rc_big=");
    kprint_hex32(rc_big);
    init_puts(" before=");
    kprint_hex32(before);
    init_puts(" mid=");
    kprint_hex32(mid);
    init_puts(" after=");
    kprint_hex32(after);
    init_puts(" dst0=");
    kprint_hex32(dst[0]);
    init_puts(" dst1=");
    kprint_hex32(dst[1]);
    init_puts(" dst2=");
    kprint_hex32(dst[2]);
    init_puts(" dst3=");
    kprint_hex32(dst[3]);
    init_puts(" dst4=");
    kprint_hex32(dst[4]);
    init_puts("\n");
}

static void init_run_callfi(void) {
    uint32_t pad0[24];
    uint32_t child[41];
    uint32_t pad1[20];
    uint32_t call_local = 0x03DCBC00u;
    uint32_t parent[41];
    const uint32_t *parent_ptr = parent;
    const uint32_t *child_ptr = child;
    uint32_t rc;
    uint32_t direct_parent_base;
    uint32_t direct_parent_sp;
    uint32_t direct_call_local;
    uint32_t direct_child_base;
    uint32_t ptr_parent_base;
    uint32_t ptr_parent_sp;
    uint32_t ptr_child_base;
    uint32_t fail = 0u;

    for (uint32_t i = 0u; i < 24u; i++) {
        pad0[i] = 0x11000000u + i;
    }
    for (uint32_t i = 0u; i < 20u; i++) {
        pad1[i] = 0x22000000u + i;
    }
    for (uint32_t i = 0u; i < 41u; i++) {
        parent[i] = 0x33000000u + i;
        child[i] = 0u;
    }

    parent[0] = 0x03DCBC00u;
    parent[39] = 0x01FE7160u;

    rc = init_callfi_prepare(parent, child, 0x03DD5000u, 0x03DDD560u);

    direct_parent_base = parent[0];
    direct_parent_sp = parent[39];
    direct_call_local = call_local;
    direct_child_base = child[0];
    ptr_parent_base = parent_ptr[0];
    ptr_parent_sp = parent_ptr[39];
    ptr_child_base = child_ptr[0];

    if (rc != (0xCAFEBABEu ^ (0xD0000000u + 3u) ^ (0xE0000000u + 7u))) {
        fail = 1u;
    }
    if (pad0[3] != 0x11000003u || pad1[7] != 0x22000007u) {
        fail = 1u;
    }
    if (ptr_parent_base != 0x03DCBC00u || ptr_parent_sp != 0x01FE7160u ||
        ptr_child_base != 0x03DD5000u) {
        fail = 1u;
    }
    if (direct_parent_base != ptr_parent_base || direct_parent_sp != ptr_parent_sp ||
        direct_call_local != 0x03DCBC00u || direct_child_base != ptr_child_base) {
        fail = 1u;
    }

    if (fail == 0u) {
        init_puts("callfi ok\n");
        return;
    }

    init_puts("callfi fail rc=");
    kprint_hex32(rc);
    init_puts(" direct_parent_base=");
    kprint_hex32(direct_parent_base);
    init_puts(" direct_parent_sp=");
    kprint_hex32(direct_parent_sp);
    init_puts(" direct_call_local=");
    kprint_hex32(direct_call_local);
    init_puts(" direct_child_base=");
    kprint_hex32(direct_child_base);
    init_puts(" ptr_parent_base=");
    kprint_hex32(ptr_parent_base);
    init_puts(" ptr_parent_sp=");
    kprint_hex32(ptr_parent_sp);
    init_puts(" ptr_child_base=");
    kprint_hex32(ptr_child_base);
    init_puts("\n");
}

static void init_run_callsp(void) {
    uint8_t src[32];
    uint32_t before_sp;
    uint32_t after_sp;
    uint32_t rc;
    uint32_t fail = 0u;

    for (uint32_t i = 0u; i < 32u; i++) {
        src[i] = (uint8_t)(0x80u + i);
        g_init_callsp_buf[i] = 0u;
    }

    __asm__ volatile("mov %0, r30" : "=r"(before_sp));
    rc = init_callsp_prepare(src, 32u);
    __asm__ volatile("mov %0, r30" : "=r"(after_sp));

    if (rc != (0x1234ABCDu ^ (0x51000000u + 5u) ^ (0x52000000u + 9u))) {
        fail = 1u;
    }
    if (before_sp != after_sp) {
        fail = 1u;
    }
    for (uint32_t i = 0u; i < 32u; i++) {
        if (g_init_callsp_buf[i] != (uint8_t)(0x80u + i)) {
            fail = 1u;
            break;
        }
    }

    if (fail == 0u) {
        init_puts("callsp ok\n");
        return;
    }

    init_puts("callsp fail rc=");
    kprint_hex32(rc);
    init_puts(" before_sp=");
    kprint_hex32(before_sp);
    init_puts(" after_sp=");
    kprint_hex32(after_sp);
    init_puts(" dst=");
    kprint_hex32((uint32_t)(uintptr_t)g_init_callsp_buf);
    init_puts("\n");
}

static void init_run_callprep(void) {
    uint32_t pad0[24];
    uint32_t child[41];
    uint32_t pad1[20];
    uint32_t call_local = 0x03DCBC00u;
    uint32_t parent[41];
    const uint32_t *parent_ptr = parent;
    const uint32_t *child_ptr = child;
    uint32_t before_sp;
    uint32_t after_sp;
    uint32_t rc;
    uint32_t direct_parent_base;
    uint32_t direct_parent_sp;
    uint32_t direct_call_local;
    uint32_t direct_child_base;
    uint32_t ptr_parent_base;
    uint32_t ptr_parent_sp;
    uint32_t ptr_child_base;
    uint32_t fail = 0u;

    for (uint32_t i = 0u; i < 24u; i++) {
        pad0[i] = 0x81000000u + i;
    }
    for (uint32_t i = 0u; i < 20u; i++) {
        pad1[i] = 0x82000000u + i;
    }
    for (uint32_t i = 0u; i < 41u; i++) {
        parent[i] = 0x83000000u + i;
        child[i] = 0u;
    }

    parent[0] = 0x03DCBC00u;
    parent[39] = 0x01FE7158u;

    __asm__ volatile("mov %0, r30" : "=r"(before_sp));
    rc = init_callprep_prepare(parent,
                               child,
                               0x03DCBC00u,
                               0x03DD5000u,
                               0x01FE7158u,
                               0x01FE8000u);
    __asm__ volatile("mov %0, r30" : "=r"(after_sp));

    direct_parent_base = parent[0];
    direct_parent_sp = parent[39];
    direct_call_local = call_local;
    direct_child_base = child[0];
    ptr_parent_base = parent_ptr[0];
    ptr_parent_sp = parent_ptr[39];
    ptr_child_base = child_ptr[0];

    if (rc != 0x55667788u) {
        fail = 1u;
    }
    if (pad0[3] != 0x81000003u || pad1[7] != 0x82000007u) {
        fail = 1u;
    }
    if (before_sp != after_sp) {
        fail = 1u;
    }
    if (ptr_parent_base != 0x03DCBC00u || ptr_parent_sp != 0x01FE7158u ||
        ptr_child_base != 0x03DD5000u) {
        fail = 1u;
    }
    if (direct_parent_base != ptr_parent_base || direct_parent_sp != ptr_parent_sp ||
        direct_call_local != 0x03DCBC00u || direct_child_base != ptr_child_base) {
        fail = 1u;
    }

    if (fail == 0u) {
        init_puts("callprep ok\n");
        return;
    }

    init_puts("callprep fail rc=");
    kprint_hex32(rc);
    init_puts(" before_sp=");
    kprint_hex32(before_sp);
    init_puts(" after_sp=");
    kprint_hex32(after_sp);
    init_puts(" direct_parent_base=");
    kprint_hex32(direct_parent_base);
    init_puts(" direct_parent_sp=");
    kprint_hex32(direct_parent_sp);
    init_puts(" direct_call_local=");
    kprint_hex32(direct_call_local);
    init_puts(" direct_child_base=");
    kprint_hex32(direct_child_base);
    init_puts(" ptr_parent_base=");
    kprint_hex32(ptr_parent_base);
    init_puts(" ptr_parent_sp=");
    kprint_hex32(ptr_parent_sp);
    init_puts(" ptr_child_base=");
    kprint_hex32(ptr_child_base);
    init_puts("\n");
}

static void init_set_log_level(const char *lvl) {
    if (init_streq(lvl, "err")) {
        klog_set_level(KLOG_LEVEL_ERROR);
    } else if (init_streq(lvl, "warn")) {
        klog_set_level(KLOG_LEVEL_WARN);
    } else if (init_streq(lvl, "info")) {
        klog_set_level(KLOG_LEVEL_INFO);
    } else if (init_streq(lvl, "debug")) {
        klog_set_level(KLOG_LEVEL_DEBUG);
    } else {
        init_puts("usage: log <err|warn|info|debug>\n");
        return;
    }
    init_puts("log level=");
    kprint_hex32(klog_get_level());
    init_puts("\n");
}

static uint32_t init_parse_u32(const char *s, uint32_t *out) {
    uint32_t v = 0u;
    uint32_t seen = 0u;
    if (!s || !out) {
        return 0u;
    }
    while (*s == ' ') {
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        uint32_t digit = (uint32_t)(*s - '0');
        if (v > 429496729u || (v == 429496729u && digit > 5u)) {
            return 0u;
        }
        v = v * 10u + digit;
        seen = 1u;
        s++;
    }
    while (*s == ' ') {
        s++;
    }
    if (!seen || *s != '\0') {
        return 0u;
    }
    *out = v;
    return 1u;
}

static void init_wait_child_and_report_tag(const char *tag, int32_t tid) {
    uint32_t status = 0u;
    uint32_t start_tick = sched_ticks();
    const uint32_t wait_timeout_ticks = 2000u; /* 10s @ 5ms tick */
    for (;;) {
        int rc = sched_waitpid(tid, SCHED_WAITPID_WNOHANG, &status);
        if (rc == 0) {
            if ((sched_ticks() - start_tick) >= wait_timeout_ticks) {
                if (tag) {
                    init_puts(tag);
                    init_puts(" ");
                }
                init_puts("waitpid timeout\n");
                return;
            }
            sched_sleep_ticks(1u);
            continue;
        }
        if (rc <= 0) {
            if (tag) {
                init_puts(tag);
                init_puts(" ");
            }
            init_puts("waitpid failed\n");
            return;
        }
        if (tag) {
            init_puts(tag);
            init_puts(" ");
        }
        init_puts("exit status=");
        kprint_hex32((status >> 8u) & 0xFFu);
        init_puts("\n");
        return;
    }
}

static void init_wait_child_and_report(int32_t tid) {
    init_wait_child_and_report_tag("uhello", tid);
}

static void init_report_spawn_failed(const char *tag, int32_t rc, const char *path) {
    if (tag) {
        init_puts(tag);
        init_puts(" ");
    }
    init_puts("spawn failed rc=");
    kprint_hex32((uint32_t)rc);
    init_puts("\n");
    if (rc == FS_ERR_NOENT && path) {
        init_puts("missing user binary: ");
        init_puts(path);
        init_puts("\n");
        init_puts("hint: run `bash user/install_m1_to_disk.sh`\n");
    }
}

static void init_run_uhello(uint32_t count) {
    static const char *const argv[] = {"/bin/hello", 0};
    if (count == 0u) {
        init_puts("uhello count must be >= 1\n");
        return;
    }
    for (uint32_t i = 0u; i < count; i++) {
        int32_t tid = user_exec_spawn_path("/bin/hello", argv, 0);
        if (tid < 0) {
            init_report_spawn_failed("uhello", tid, "/bin/hello");
            return;
        }
        init_puts("uhello tid=");
        kprint_hex32((uint32_t)tid);
        if (count > 1u) {
            init_puts(" run=");
            kprint_hex32(i + 1u);
            init_puts("/");
            kprint_hex32(count);
        }
        init_puts("\n");
        init_wait_child_and_report(tid);
    }
}

static void init_run_uvfork(uint32_t count) {
    static const char *const argv[] = {"/bin/vfork_exec", 0};
    if (count == 0u) {
        init_puts("uvfork count must be >= 1\n");
        return;
    }
    for (uint32_t i = 0u; i < count; i++) {
        int32_t tid = user_exec_spawn_path("/bin/vfork_exec", argv, 0);
        if (tid < 0) {
            init_report_spawn_failed("uvfork", tid, "/bin/vfork_exec");
            return;
        }
        init_puts("uvfork tid=");
        kprint_hex32((uint32_t)tid);
        if (count > 1u) {
            init_puts(" run=");
            kprint_hex32(i + 1u);
            init_puts("/");
            kprint_hex32(count);
        }
        init_puts("\n");
        init_wait_child_and_report_tag("uvfork", tid);
    }
}

static void init_handle_cmd(char *line) {
    while (*line == ' ') {
        line++;
    }
    if (*line == '\0') {
        return;
    }

    if (init_streq(line, "help")) {
        init_puts("commands:\n");
        init_puts("  help\n");
        init_puts("  tty\n");
        init_puts("  tty echo <on|off>\n");
        init_puts("  tty canon <on|off>\n");
        init_puts("  tty isig <on|off>\n");
        init_puts("  log <err|warn|info|debug>\n");
        init_puts("  poll\n");
        init_puts("  clear\n");
        init_puts("  callabi\n");
        init_puts("  callfi\n");
        init_puts("  callsp\n");
        init_puts("  callprep\n");
        init_puts("  fdtest\n");
        init_puts("  uhello [count]\n");
        init_puts("  uvfork [count]\n");
        init_puts("  halt\n");
        return;
    }
    if (init_streq(line, "halt")) {
        __asm__ __volatile__("halt");
        return;
    }
    if (init_streq(line, "tty")) {
        init_show_tty_mode();
        return;
    }
    if (init_streq(line, "poll")) {
        init_show_poll_state();
        return;
    }
    if (init_streq(line, "clear")) {
        console_fb_clear();
        return;
    }
    if (init_streq(line, "callabi")) {
        init_run_callabi();
        return;
    }
    if (init_streq(line, "callfi")) {
        init_run_callfi();
        return;
    }
    if (init_streq(line, "callsp")) {
        init_run_callsp();
        return;
    }
    if (init_streq(line, "callprep")) {
        init_run_callprep();
        return;
    }
    if (init_streq(line, "fdtest")) {
        uint32_t do_run = 0u;
        spinlock_lock(&g_init_cmd_lock);
        if (g_init_fdtest_running == 0u) {
            g_init_fdtest_running = 1u;
            do_run = 1u;
        }
        spinlock_unlock(&g_init_cmd_lock);
        if (do_run) {
            fd_selftest_run();
            spinlock_lock(&g_init_cmd_lock);
            g_init_fdtest_running = 0u;
            spinlock_unlock(&g_init_cmd_lock);
        }
        return;
    }
    if (init_streq(line, "uhello")) {
        init_run_uhello(1u);
        return;
    }
    if (init_streq(line, "uvfork")) {
        init_run_uvfork(1u);
        return;
    }
    if (init_starts_with(line, "uhello ")) {
        uint32_t count = 0u;
        if (!init_parse_u32(line + 7, &count)) {
            init_puts("usage: uhello [count]\n");
            return;
        }
        init_run_uhello(count);
        return;
    }
    if (init_starts_with(line, "uvfork ")) {
        uint32_t count = 0u;
        if (!init_parse_u32(line + 7, &count)) {
            init_puts("usage: uvfork [count]\n");
            return;
        }
        init_run_uvfork(count);
        return;
    }
    if (init_starts_with(line, "log ")) {
        init_set_log_level(line + 4);
        return;
    }
    if (init_streq(line, "tty echo on")) {
        init_set_flag(TTY_LFLAG_ECHO, 1u);
        return;
    }
    if (init_streq(line, "tty echo off")) {
        init_set_flag(TTY_LFLAG_ECHO, 0u);
        return;
    }
    if (init_streq(line, "tty canon on")) {
        init_set_flag(TTY_LFLAG_ICANON, 1u);
        return;
    }
    if (init_streq(line, "tty canon off")) {
        init_set_flag(TTY_LFLAG_ICANON, 0u);
        return;
    }
    if (init_streq(line, "tty isig on")) {
        init_set_flag(TTY_LFLAG_ISIG, 1u);
        return;
    }
    if (init_streq(line, "tty isig off")) {
        init_set_flag(TTY_LFLAG_ISIG, 0u);
        return;
    }

    init_puts("unknown command: ");
    init_puts(line);
    init_puts("\n");
}

static void init_task_entry(sched_task_t *task, void *arg) {
    init_state_t *st = (init_state_t *)arg;
    uint8_t cmd[INIT_LINE_CAP];
    int self_tid;
    uint8_t c = 0u;
    int n;
    (void)task;
    if (!st) {
        return;
    }

    self_tid = sched_current_tid();
    if (self_tid < 0) {
        return;
    }
    spinlock_lock(&g_init_owner_lock);
    if (g_init_owner_tid == 0u) {
        g_init_owner_tid = (uint32_t)self_tid;
    } else if (g_init_owner_tid != (uint32_t)self_tid) {
        spinlock_unlock(&g_init_owner_lock);
        sched_exit();
        return;
    }
    if (g_init_busy != 0u) {
        spinlock_unlock(&g_init_owner_lock);
        return;
    }
    g_init_busy = 1u;
    spinlock_unlock(&g_init_owner_lock);

    if (!st->started) {
        st->started = 1u;
        init_puts("init task online (type 'help')\n");
        init_prompt();
    }

    n = console_read(&c, 1u, 1u);
    if (n <= 0) {
        goto out_release;
    }

    if (c == (uint8_t)'\n') {
        uint32_t i;
        uint32_t cmd_len = st->len;
        if (cmd_len >= (INIT_LINE_CAP - 1u)) {
            cmd_len = INIT_LINE_CAP - 1u;
        }
        for (i = 0u; i < cmd_len; i++) {
            cmd[i] = st->line[i];
        }
        cmd[cmd_len] = '\0';
        st->len = 0u;
        init_handle_cmd((char *)cmd);
        init_prompt();
        goto out_release;
    }

    if (c == (uint8_t)'\t' || c == (uint8_t)'\v' || c == (uint8_t)'\f') {
        c = (uint8_t)' ';
    }

    if (c >= (uint8_t)' ' && c <= (uint8_t)'~') {
        if (st->len + 1u < INIT_LINE_CAP) {
            st->line[st->len++] = c;
        }
    }

out_release:
    spinlock_lock(&g_init_owner_lock);
    g_init_busy = 0u;
    spinlock_unlock(&g_init_owner_lock);
}

void init_task_spawn(void) {
    g_init_state.len = 0u;
    g_init_state.started = 0u;
    spinlock_init(&g_init_owner_lock);
    spinlock_init(&g_init_cmd_lock);
    g_init_owner_tid = 0u;
    g_init_busy = 0u;
    g_init_fdtest_running = 0u;
    if (sched_spawn("init", init_task_entry, &g_init_state) < 0) {
        KLOGW("init", "spawn failed");
    } else {
        KLOGI("init", "spawned");
    }
}
