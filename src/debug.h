//
// Created by Codex on 2026/01/28.
//
#ifndef VM_DEBUG_H
#define VM_DEBUG_H

#include <stdint.h>
#include <stddef.h>

struct VM;

#ifdef VM_DEBUG

#define VM_DEBUG_MAX_BREAKPOINTS 256

void vm_debug_init(struct VM *vm);
void vm_debug_destroy(struct VM *vm);

void vm_debug_pause_if_needed(struct VM *vm, uint32_t ip);
void vm_debug_count_instruction(struct VM *vm, uint8_t op);
void vm_debug_print_stats(const struct VM *vm);

#else

static inline void vm_debug_init(struct VM *vm) { (void)vm; }
static inline void vm_debug_destroy(struct VM *vm) { (void)vm; }
static inline void vm_debug_pause_if_needed(struct VM *vm, uint32_t ip) {
    (void)vm;
    (void)ip;
}
static inline void vm_debug_count_instruction(struct VM *vm, uint8_t op) {
    (void)vm;
    (void)op;
}
static inline void vm_debug_print_stats(const struct VM *vm) { (void)vm; }

#endif

#endif // VM_DEBUG_H
