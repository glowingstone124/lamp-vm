#ifndef LAMPVM_DEBUG_API_H
#define LAMPVM_DEBUG_API_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define LAMP_DEBUG_EXPORT __declspec(dllexport)
#elif defined(__GNUC__)
#define LAMP_DEBUG_EXPORT __attribute__((visibility("default")))
#else
#define LAMP_DEBUG_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define LAMP_DEBUG_ABI_VERSION 1u
#define LAMP_DEBUG_REGISTER_COUNT 32u
#define LAMP_DEBUG_FRAMEBUFFER_WIDTH 640u
#define LAMP_DEBUG_FRAMEBUFFER_HEIGHT 480u
#define LAMP_DEBUG_MOUSE_LEFT 0x01u
#define LAMP_DEBUG_MOUSE_RIGHT 0x02u
#define LAMP_DEBUG_MOUSE_MIDDLE 0x04u

typedef struct lamp_debug_vm lamp_debug_vm_t;

typedef enum lamp_debug_status {
    LAMP_DEBUG_OK = 0,
    LAMP_DEBUG_INVALID_ARGUMENT = 1,
    LAMP_DEBUG_INVALID_STATE = 2,
    LAMP_DEBUG_OUT_OF_MEMORY = 3,
    LAMP_DEBUG_IO_ERROR = 4,
    LAMP_DEBUG_TIMEOUT = 5,
    LAMP_DEBUG_INTERNAL_ERROR = 6
} lamp_debug_status_t;

typedef enum lamp_debug_engine {
    LAMP_DEBUG_ENGINE_CLASSIC = 0,
    LAMP_DEBUG_ENGINE_CACHED = 1,
    LAMP_DEBUG_ENGINE_THREADED = 2,
    LAMP_DEBUG_ENGINE_JIT = 3
} lamp_debug_engine_t;

typedef enum lamp_debug_vm_state {
    LAMP_DEBUG_VM_CREATED = 0,
    LAMP_DEBUG_VM_RUNNING = 1,
    LAMP_DEBUG_VM_PAUSED = 2,
    LAMP_DEBUG_VM_STOPPED = 3,
    LAMP_DEBUG_VM_PANICKED = 4
} lamp_debug_vm_state_t;

typedef struct lamp_debug_config_v1 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t memory_bytes;
    uint64_t cpu_frequency_hz;
    uint32_t core_count;
    uint32_t execution_engine;
} lamp_debug_config_v1;

typedef struct lamp_debug_cpu_snapshot_v1 {
    uint32_t struct_size;
    uint32_t core_id;
    uint32_t registers[LAMP_DEBUG_REGISTER_COUNT];
    uint64_t ip;
    uint64_t last_ip;
    uint32_t flags;
    int32_t call_stack_pointer;
    int32_t data_stack_pointer;
    int32_t interrupt_stack_pointer;
    uint32_t active_interrupt;
    uint8_t in_interrupt;
    uint8_t irq_masked;
    uint8_t is_bootstrap_processor;
    uint8_t reserved;
} lamp_debug_cpu_snapshot_v1;

typedef struct lamp_debug_stats_v1 {
    uint32_t struct_size;
    uint32_t state;
    uint64_t cpu_frequency_hz;
    uint64_t virtual_cycles;
    uint64_t executed_instructions;
    uint64_t execution_rate_hz;
    uint64_t uptime_ns;
    uint64_t host_resident_bytes;
    uint64_t guest_ram_bytes;
    uint32_t core_count;
    uint32_t active_core_count;
} lamp_debug_stats_v1;

LAMP_DEBUG_EXPORT lamp_debug_status_t lamp_debug_create_from_file(
    const lamp_debug_config_v1 *config,
    const char *program_path,
    const char *disk_path,
    lamp_debug_vm_t **out_vm);

LAMP_DEBUG_EXPORT lamp_debug_status_t lamp_debug_start(lamp_debug_vm_t *handle);
LAMP_DEBUG_EXPORT lamp_debug_status_t lamp_debug_pause(lamp_debug_vm_t *handle,
                                                       uint64_t timeout_ms);
LAMP_DEBUG_EXPORT lamp_debug_status_t lamp_debug_step(lamp_debug_vm_t *handle,
                                                      uint32_t core_id,
                                                      uint64_t timeout_ms);
LAMP_DEBUG_EXPORT lamp_debug_status_t lamp_debug_resume(lamp_debug_vm_t *handle);
LAMP_DEBUG_EXPORT lamp_debug_status_t lamp_debug_request_stop(lamp_debug_vm_t *handle);
LAMP_DEBUG_EXPORT lamp_debug_status_t lamp_debug_join(lamp_debug_vm_t *handle,
                                                      uint64_t timeout_ms);
LAMP_DEBUG_EXPORT void lamp_debug_destroy(lamp_debug_vm_t *handle);

LAMP_DEBUG_EXPORT lamp_debug_vm_state_t lamp_debug_state(
    const lamp_debug_vm_t *handle);
LAMP_DEBUG_EXPORT lamp_debug_status_t lamp_debug_get_stats(
    lamp_debug_vm_t *handle,
    lamp_debug_stats_v1 *out_stats);
LAMP_DEBUG_EXPORT lamp_debug_status_t lamp_debug_get_cpu(
    lamp_debug_vm_t *handle,
    uint32_t core_id,
    lamp_debug_cpu_snapshot_v1 *out_cpu);
LAMP_DEBUG_EXPORT lamp_debug_status_t lamp_debug_read_memory(
    lamp_debug_vm_t *handle,
    uint32_t address,
    uint8_t *destination,
    size_t size);
LAMP_DEBUG_EXPORT lamp_debug_status_t lamp_debug_write_memory(
    lamp_debug_vm_t *handle,
    uint32_t address,
    const uint8_t *source,
    size_t size);
LAMP_DEBUG_EXPORT lamp_debug_status_t lamp_debug_read_framebuffer(
    lamp_debug_vm_t *handle,
    uint32_t *destination,
    size_t pixel_capacity);
LAMP_DEBUG_EXPORT lamp_debug_status_t lamp_debug_send_key(
    lamp_debug_vm_t *handle,
    uint8_t set1_scancode,
    uint8_t extended,
    uint8_t pressed);
LAMP_DEBUG_EXPORT lamp_debug_status_t lamp_debug_send_mouse(
    lamp_debug_vm_t *handle,
    int32_t delta_x,
    int32_t delta_y,
    uint8_t buttons);
LAMP_DEBUG_EXPORT size_t lamp_debug_serial_read(lamp_debug_vm_t *handle,
                                               uint8_t *destination,
                                               size_t capacity);
LAMP_DEBUG_EXPORT size_t lamp_debug_serial_write(lamp_debug_vm_t *handle,
                                                const uint8_t *source,
                                                size_t size);
LAMP_DEBUG_EXPORT const char *lamp_debug_last_error(
    const lamp_debug_vm_t *handle);

#ifdef __cplusplus
}
#endif

#endif /* LAMPVM_DEBUG_API_H */
