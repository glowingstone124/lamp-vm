#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "code_memory.h"

#if defined(__APPLE__) && !defined(MAP_JIT)
#define MAP_JIT 0x0800
#endif

static size_t vm_jit_round_up(size_t value, size_t alignment) {
    const size_t remainder = value % alignment;
    if (remainder == 0u) {
        return value;
    }
    if (value > SIZE_MAX - (alignment - remainder)) {
        return 0u;
    }
    return value + alignment - remainder;
}

int vm_jit_code_arena_init(VmJitCodeArena *arena,
                           size_t slot_count,
                           size_t slot_size) {
    long page_size_long;
    size_t mapping_size;
    void *mapping;
    int mmap_flags = MAP_PRIVATE | MAP_ANONYMOUS;
    int mmap_prot = PROT_READ | PROT_WRITE;

    if (!arena || slot_count == 0u || slot_size == 0u ||
        slot_count > SIZE_MAX / slot_size) {
        return 0;
    }
    memset(arena, 0, sizeof(*arena));
    page_size_long = sysconf(_SC_PAGESIZE);
    if (page_size_long <= 0) {
        return 0;
    }
    mapping_size = vm_jit_round_up(slot_count * slot_size,
                                   (size_t)page_size_long);
    if (mapping_size == 0u) {
        return 0;
    }
#if defined(__APPLE__)
    mmap_flags |= MAP_JIT;
    mmap_prot |= PROT_EXEC;
#endif
    mapping = mmap(NULL, mapping_size, mmap_prot, mmap_flags, -1, 0);
    if (mapping == MAP_FAILED) {
        return 0;
    }
#if !defined(__APPLE__)
    if (mprotect(mapping, mapping_size, PROT_READ | PROT_EXEC) != 0) {
        munmap(mapping, mapping_size);
        return 0;
    }
#endif
    arena->mapping = mapping;
    arena->mapping_size = mapping_size;
    arena->slot_size = slot_size;
    arena->slot_count = slot_count;
    arena->page_size = (size_t)page_size_long;
    return 1;
}

void vm_jit_code_arena_destroy(VmJitCodeArena *arena) {
    if (!arena) {
        return;
    }
    if (arena->mapping && arena->mapping_size != 0u) {
        munmap(arena->mapping, arena->mapping_size);
    }
    memset(arena, 0, sizeof(*arena));
}

int vm_jit_code_assign_slot(const VmJitCodeArena *arena,
                            size_t slot_index,
                            VmJitCode *code) {
    if (!arena || !arena->mapping || !code ||
        slot_index >= arena->slot_count || arena->slot_size == 0u) {
        return 0;
    }
    memset(code, 0, sizeof(*code));
    code->mapping = (uint8_t *)arena->mapping + slot_index * arena->slot_size;
    code->mapping_size = arena->slot_size;
    code->arena_backed = 1u;
    return 1;
}

static int vm_jit_code_make_slot_writable(const VmJitCode *code,
                                          size_t page_size) {
#if defined(__APPLE__)
    (void)code;
    (void)page_size;
    return 1;
#else
    const uintptr_t start = (uintptr_t)code->mapping & ~(uintptr_t)(page_size - 1u);
    const uintptr_t end = vm_jit_round_up(
        (uintptr_t)code->mapping + code->mapping_size, page_size);
    if (end == 0u || end < start) {
        return 0;
    }
    return mprotect((void *)start, (size_t)(end - start),
                    PROT_READ | PROT_WRITE) == 0;
#endif
}

static int vm_jit_code_make_slot_executable(const VmJitCode *code,
                                            size_t page_size) {
#if defined(__APPLE__)
    (void)code;
    (void)page_size;
    return 1;
#else
    const uintptr_t start = (uintptr_t)code->mapping & ~(uintptr_t)(page_size - 1u);
    const uintptr_t end = vm_jit_round_up(
        (uintptr_t)code->mapping + code->mapping_size, page_size);
    if (end == 0u || end < start) {
        return 0;
    }
    return mprotect((void *)start, (size_t)(end - start),
                    PROT_READ | PROT_EXEC) == 0;
#endif
}

int vm_jit_code_publish(const uint32_t *words,
                        size_t word_count,
                        VmJitCode *out) {
    long page_size_long;
    size_t byte_count;
    size_t mapping_size;
    void *mapping;
    int arena_backed;
    int mmap_flags = MAP_PRIVATE | MAP_ANONYMOUS;
    int mmap_prot = PROT_READ | PROT_WRITE;

    if (!words || word_count == 0u || !out ||
        word_count > SIZE_MAX / sizeof(*words)) {
        return 0;
    }
    page_size_long = sysconf(_SC_PAGESIZE);
    if (page_size_long <= 0) {
        return 0;
    }
    byte_count = word_count * sizeof(*words);
    arena_backed = out->arena_backed != 0u;
    if (arena_backed) {
        if (!out->mapping || byte_count > out->mapping_size ||
            !vm_jit_code_make_slot_writable(out, (size_t)page_size_long)) {
            return 0;
        }
#if defined(__APPLE__) && defined(__aarch64__)
        pthread_jit_write_protect_np(0);
#endif
        memcpy(out->mapping, words, byte_count);
        __builtin___clear_cache((char *)out->mapping,
                                (char *)out->mapping + byte_count);
#if defined(__APPLE__) && defined(__aarch64__)
        pthread_jit_write_protect_np(1);
#endif
        if (!vm_jit_code_make_slot_executable(out,
                                              (size_t)page_size_long)) {
            return 0;
        }
        mapping = out->mapping;
        memcpy(&out->entry, &mapping, sizeof(out->entry));
        out->code_size = byte_count;
        return 1;
    }

    memset(out, 0, sizeof(*out));
    mapping_size = vm_jit_round_up(byte_count, (size_t)page_size_long);
    if (mapping_size == 0u) {
        return 0;
    }

#if defined(__APPLE__)
    mmap_flags |= MAP_JIT;
    mmap_prot |= PROT_EXEC;
#endif
    mapping = mmap(NULL, mapping_size, mmap_prot, mmap_flags, -1, 0);
    if (mapping == MAP_FAILED) {
        return 0;
    }

#if defined(__APPLE__) && defined(__aarch64__)
    pthread_jit_write_protect_np(0);
#endif
    memcpy(mapping, words, byte_count);
    __builtin___clear_cache((char *)mapping, (char *)mapping + byte_count);
#if defined(__APPLE__) && defined(__aarch64__)
    pthread_jit_write_protect_np(1);
#else
    if (mprotect(mapping, mapping_size, PROT_READ | PROT_EXEC) != 0) {
        munmap(mapping, mapping_size);
        return 0;
    }
#endif

    if (sizeof(out->entry) != sizeof(mapping)) {
        munmap(mapping, mapping_size);
        return 0;
    }
    memcpy(&out->entry, &mapping, sizeof(out->entry));
    out->mapping = mapping;
    out->mapping_size = mapping_size;
    out->code_size = byte_count;
    return 1;
}

void vm_jit_code_destroy(VmJitCode *code) {
    void *mapping;
    size_t mapping_size;
    if (!code) {
        return;
    }
    if (code->arena_backed) {
        mapping = code->mapping;
        mapping_size = code->mapping_size;
        memset(code, 0, sizeof(*code));
        code->mapping = mapping;
        code->mapping_size = mapping_size;
        code->arena_backed = 1u;
        return;
    }
    if (code->mapping && code->mapping_size != 0u) {
        munmap(code->mapping, code->mapping_size);
    }
    memset(code, 0, sizeof(*code));
}
