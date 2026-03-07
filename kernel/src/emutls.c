#include "../include/kernel/types.h"
#include "../include/kernel/spinlock.h"

#define EMUTLS_MAX_OBJECTS 128u
#define EMUTLS_MAX_CPUS 32u
#define EMUTLS_ARENA_SIZE (256u * 1024u)

typedef struct {
    uint32_t size;
    uint32_t align;
    void *object;
    void *templ;
} emutls_control_t;

typedef struct {
    emutls_control_t *control;
    void *per_cpu[EMUTLS_MAX_CPUS];
} emutls_entry_t;

static spinlock_t g_emutls_lock;
static emutls_entry_t g_emutls_entries[EMUTLS_MAX_OBJECTS];
static uint8_t g_emutls_arena[EMUTLS_ARENA_SIZE];
static uint32_t g_emutls_arena_off;

static inline uint32_t emutls_cpu_id(void) {
    uint32_t id = 0u;
    __asm__ volatile("cpuid %0" : "=r"(id));
    return id;
}

static inline uint32_t emutls_align_up(uint32_t value, uint32_t align) {
    uint32_t mask;
    if (align == 0u) {
        return value;
    }
    if ((align & (align - 1u)) != 0u) {
        return value;
    }
    mask = align - 1u;
    return (value + mask) & ~mask;
}

static void emutls_copy_init(uint8_t *dst, const uint8_t *src, uint32_t n) {
    uint32_t i;
    if (src) {
        for (i = 0u; i < n; i++) {
            dst[i] = src[i];
        }
        return;
    }
    for (i = 0u; i < n; i++) {
        dst[i] = 0u;
    }
}

static emutls_entry_t *emutls_get_entry(emutls_control_t *control) {
    uint32_t i;
    emutls_entry_t *empty = 0;
    for (i = 0u; i < EMUTLS_MAX_OBJECTS; i++) {
        emutls_entry_t *e = &g_emutls_entries[i];
        if (e->control == control) {
            return e;
        }
        if (!e->control && !empty) {
            empty = e;
        }
    }
    if (!empty) {
        return 0;
    }
    empty->control = control;
    return empty;
}

void *__emutls_get_address(emutls_control_t *control) {
    uint32_t cpu;
    uint32_t size;
    uint32_t align;
    uint32_t off;
    emutls_entry_t *entry;
    uint8_t *obj;

    if (!control) {
        return 0;
    }

    cpu = emutls_cpu_id();
    if (cpu >= EMUTLS_MAX_CPUS) {
        cpu = 0u;
    }

    spinlock_lock(&g_emutls_lock);
    entry = emutls_get_entry(control);
    if (!entry) {
        spinlock_unlock(&g_emutls_lock);
        return 0;
    }

    if (entry->per_cpu[cpu]) {
        void *ret = entry->per_cpu[cpu];
        spinlock_unlock(&g_emutls_lock);
        return ret;
    }

    size = control->size ? control->size : 4u;
    align = control->align;
    if (align < 4u) {
        align = 4u;
    }
    off = emutls_align_up(g_emutls_arena_off, align);
    if (off > EMUTLS_ARENA_SIZE || size > (EMUTLS_ARENA_SIZE - off)) {
        spinlock_unlock(&g_emutls_lock);
        return 0;
    }

    obj = &g_emutls_arena[off];
    g_emutls_arena_off = off + size;
    emutls_copy_init(obj, (const uint8_t *)(uintptr_t)control->templ, size);
    entry->per_cpu[cpu] = obj;
    spinlock_unlock(&g_emutls_lock);
    return obj;
}
