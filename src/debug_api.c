#include "../include/lampvm/debug_api.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "interrupt.h"
#include "io.h"
#include "memory.h"
#include "engines/engine.h"
#include "io_devices/disk/disk.h"
#include "io_devices/ether/ether.h"
#include "io_devices/ether/ether_backend.h"
#include "loadbin.h"
#include "runtime_stats.h"
#include "vm.h"
#include "vm_runtime.h"

struct lamp_debug_vm {
    VM *vm;
    pthread_t runner;
    atomic_bool runner_started;
    atomic_bool runner_finished;
    atomic_bool runner_joined;
    int runner_result;
    char last_error[256];
};

static _Thread_local char lamp_debug_create_error[256];

static void lamp_debug_set_error(lamp_debug_vm_t *handle,
                                 const char *message) {
    char *destination = handle ? handle->last_error : lamp_debug_create_error;
    if (!message) {
        destination[0] = '\0';
        return;
    }
    (void)snprintf(destination, 256u, "%s", message);
}

static uint32_t lamp_debug_active_cores(const VM *vm) {
    uint32_t active = 0u;
    for (int core = 0; vm && core < vm->smp_cores; core++) {
        if (atomic_load_explicit(&vm->core_released[core],
                                 memory_order_acquire)) {
            active++;
        }
    }
    return active;
}

static int lamp_debug_wait_flag(const atomic_bool *flag,
                                int expected,
                                uint64_t timeout_ms) {
    const uint64_t started_ns = host_monotonic_time_ns();
    for (;;) {
        if ((atomic_load_explicit(flag, memory_order_acquire) ? 1 : 0) ==
            expected) {
            return 1;
        }
        if (timeout_ms != UINT64_MAX) {
            const uint64_t elapsed_ms =
                (host_monotonic_time_ns() - started_ns) / 1000000ull;
            if (elapsed_ms >= timeout_ms) {
                return 0;
            }
        }
        usleep(1000);
    }
}

static void *lamp_debug_runner(void *opaque) {
    lamp_debug_vm_t *handle = opaque;
    handle->runner_result = vm_run_headless(handle->vm, UINT64_MAX);
    atomic_store_explicit(&handle->runner_finished, true,
                          memory_order_release);
    return NULL;
}

lamp_debug_status_t lamp_debug_create_from_file(
    const lamp_debug_config_v1 *config,
    const char *program_path,
    const char *disk_path,
    lamp_debug_vm_t **out_vm) {
    uint64_t *program = NULL;
    uint8_t *data = NULL;
    size_t program_size = 0u;
    size_t data_size = 0u;
    ProgramLayout layout;
    ether_backend_t backend;
    lamp_debug_vm_t *handle;
    uint64_t memory_bytes;

    if (!config || !program_path || !out_vm ||
        config->struct_size < sizeof(*config) ||
        config->abi_version != LAMP_DEBUG_ABI_VERSION ||
        config->core_count == 0u || config->core_count > 64u ||
        config->execution_engine > LAMP_DEBUG_ENGINE_JIT) {
        lamp_debug_set_error(NULL, "invalid debugger configuration");
        return LAMP_DEBUG_INVALID_ARGUMENT;
    }
    *out_vm = NULL;
    if (!load_program_single(program_path, &program, &program_size,
                             &data, &data_size, &layout)) {
        lamp_debug_set_error(NULL, "failed to load guest image");
        return LAMP_DEBUG_IO_ERROR;
    }

    handle = calloc(1u, sizeof(*handle));
    if (!handle) {
        free(program);
        free(data);
        lamp_debug_set_error(NULL, "failed to allocate debugger handle");
        return LAMP_DEBUG_OUT_OF_MEMORY;
    }
    atomic_init(&handle->runner_started, false);
    atomic_init(&handle->runner_finished, false);
    atomic_init(&handle->runner_joined, false);
    memory_bytes = config->memory_bytes != 0u ?
        config->memory_bytes : (64ull * 1024ull * 1024ull);
    if (memory_bytes > SIZE_MAX) {
        free(handle);
        free(program);
        free(data);
        lamp_debug_set_error(NULL, "guest memory is too large for this host");
        return LAMP_DEBUG_INVALID_ARGUMENT;
    }

    handle->vm = vm_create((size_t)memory_bytes, program, program_size,
                           data, data_size, &layout,
                           (int)config->core_count);
    free(program);
    free(data);
    if (!handle->vm || atomic_is_vm_panicked(handle->vm)) {
        if (handle->vm) {
            vm_destroy(handle->vm);
        }
        free(handle);
        lamp_debug_set_error(NULL, "VM creation failed");
        return LAMP_DEBUG_INTERNAL_ERROR;
    }

    handle->vm->cpu_frequency_hz = config->cpu_frequency_hz != 0u ?
        config->cpu_frequency_hz :
        (uint64_t)VM_DEFAULT_CPU_MHZ * 1000000ull;
    vm_engine_set(handle->vm, (VmExecutionEngine)config->execution_engine);
    /* Embedded frontends consume serial bytes through the debugger API.
     * Without capture, the legacy IO path bypasses the TX FIFO and writes
     * directly to the host process stdout. */
    handle->vm->serial_capture_enabled = 1;
    init_ivt(handle->vm);
    if (ether_backend_null_create(&backend) == 0) {
        (void)ether_init(handle->vm, &backend);
    }
    if (disk_path && disk_path[0] != '\0') {
        disk_init(handle->vm, disk_path);
        if (atomic_is_vm_panicked(handle->vm)) {
            ether_shutdown(handle->vm);
            vm_destroy(handle->vm);
            free(handle);
            lamp_debug_set_error(NULL, "failed to open disk image");
            return LAMP_DEBUG_IO_ERROR;
        }
    }

    lamp_debug_set_error(handle, NULL);
    *out_vm = handle;
    return LAMP_DEBUG_OK;
}

lamp_debug_status_t lamp_debug_start(lamp_debug_vm_t *handle) {
    if (!handle || !handle->vm) {
        return LAMP_DEBUG_INVALID_ARGUMENT;
    }
    if (atomic_exchange_explicit(&handle->runner_started, true,
                                 memory_order_acq_rel)) {
        lamp_debug_set_error(handle, "VM has already been started");
        return LAMP_DEBUG_INVALID_STATE;
    }
    if (pthread_create(&handle->runner, NULL, lamp_debug_runner, handle) != 0) {
        atomic_store_explicit(&handle->runner_started, false,
                              memory_order_release);
        lamp_debug_set_error(handle, "failed to create VM runner thread");
        return LAMP_DEBUG_INTERNAL_ERROR;
    }
    return LAMP_DEBUG_OK;
}

lamp_debug_status_t lamp_debug_pause(lamp_debug_vm_t *handle,
                                     uint64_t timeout_ms) {
    uint32_t active;
    uint64_t started_ns;
    if (!handle || !handle->vm) {
        return LAMP_DEBUG_INVALID_ARGUMENT;
    }
    if (!atomic_load_explicit(&handle->runner_started, memory_order_acquire) ||
        atomic_load_explicit(&handle->runner_finished, memory_order_acquire)) {
        lamp_debug_set_error(handle, "only a running VM can be paused");
        return LAMP_DEBUG_INVALID_STATE;
    }
    atomic_store_explicit(&handle->vm->debug_pause_requested, true,
                          memory_order_release);
    active = lamp_debug_active_cores(handle->vm);
    started_ns = host_monotonic_time_ns();
    while (atomic_load_explicit(&handle->vm->debug_paused_core_count,
                                memory_order_acquire) < active) {
        if (atomic_load_explicit(&handle->runner_finished,
                                 memory_order_acquire)) {
            return LAMP_DEBUG_INVALID_STATE;
        }
        if (timeout_ms != UINT64_MAX &&
            (host_monotonic_time_ns() - started_ns) / 1000000ull >= timeout_ms) {
            atomic_store_explicit(&handle->vm->debug_pause_requested, false,
                                  memory_order_release);
            lamp_debug_set_error(handle, "timed out waiting for vCPUs to pause");
            return LAMP_DEBUG_TIMEOUT;
        }
        usleep(1000);
    }
    return LAMP_DEBUG_OK;
}

lamp_debug_status_t lamp_debug_step(lamp_debug_vm_t *handle,
                                    uint32_t core_id,
                                    uint64_t timeout_ms) {
    const uint64_t started_ns = host_monotonic_time_ns();
    if (!handle || !handle->vm ||
        core_id >= (uint32_t)handle->vm->smp_cores) {
        return LAMP_DEBUG_INVALID_ARGUMENT;
    }
    if (lamp_debug_state(handle) != LAMP_DEBUG_VM_PAUSED ||
        !atomic_load_explicit(&handle->vm->core_released[core_id],
                              memory_order_acquire)) {
        lamp_debug_set_error(handle,
                             "single-step requires a paused, active vCPU");
        return LAMP_DEBUG_INVALID_STATE;
    }
    if (atomic_load_explicit(&handle->vm->debug_step_requested,
                             memory_order_acquire)) {
        lamp_debug_set_error(handle, "a single-step is already pending");
        return LAMP_DEBUG_INVALID_STATE;
    }
    atomic_store_explicit(&handle->vm->debug_step_completed, false,
                          memory_order_release);
    atomic_store_explicit(&handle->vm->debug_step_core, core_id,
                          memory_order_release);
    /* Publish the request after its target and completion state. */
    atomic_store_explicit(&handle->vm->debug_step_requested, true,
                          memory_order_release);

    while (!atomic_load_explicit(&handle->vm->debug_step_completed,
                                 memory_order_acquire)) {
        if (atomic_is_vm_stopped(handle->vm)) {
            return LAMP_DEBUG_INVALID_STATE;
        }
        if (timeout_ms != UINT64_MAX &&
            (host_monotonic_time_ns() - started_ns) / 1000000ull >=
                timeout_ms) {
            atomic_store_explicit(&handle->vm->debug_step_requested, false,
                                  memory_order_release);
            lamp_debug_set_error(handle, "timed out waiting for single-step");
            return LAMP_DEBUG_TIMEOUT;
        }
        usleep(1000);
    }
    return LAMP_DEBUG_OK;
}

lamp_debug_status_t lamp_debug_resume(lamp_debug_vm_t *handle) {
    if (!handle || !handle->vm) {
        return LAMP_DEBUG_INVALID_ARGUMENT;
    }
    atomic_store_explicit(&handle->vm->debug_pause_requested, false,
                          memory_order_release);
    atomic_store_explicit(&handle->vm->debug_step_requested, false,
                          memory_order_release);
    return LAMP_DEBUG_OK;
}

lamp_debug_status_t lamp_debug_request_stop(lamp_debug_vm_t *handle) {
    if (!handle || !handle->vm) {
        return LAMP_DEBUG_INVALID_ARGUMENT;
    }
    atomic_store_explicit(&handle->vm->debug_pause_requested, false,
                          memory_order_release);
    atomic_set_vm_halt(handle->vm, 1);
    return LAMP_DEBUG_OK;
}

lamp_debug_status_t lamp_debug_join(lamp_debug_vm_t *handle,
                                    uint64_t timeout_ms) {
    if (!handle || !handle->vm) {
        return LAMP_DEBUG_INVALID_ARGUMENT;
    }
    if (!atomic_load_explicit(&handle->runner_started, memory_order_acquire)) {
        lamp_debug_set_error(handle, "VM has not been started");
        return LAMP_DEBUG_INVALID_STATE;
    }
    if (!lamp_debug_wait_flag(&handle->runner_finished, 1, timeout_ms)) {
        return LAMP_DEBUG_TIMEOUT;
    }
    if (!atomic_exchange_explicit(&handle->runner_joined, true,
                                  memory_order_acq_rel)) {
        (void)pthread_join(handle->runner, NULL);
    }
    return handle->runner_result ? LAMP_DEBUG_OK : LAMP_DEBUG_INTERNAL_ERROR;
}

void lamp_debug_destroy(lamp_debug_vm_t *handle) {
    if (!handle) {
        return;
    }
    if (handle->vm) {
        if (atomic_load_explicit(&handle->runner_started,
                                 memory_order_acquire) &&
            !atomic_load_explicit(&handle->runner_joined,
                                  memory_order_acquire)) {
            (void)lamp_debug_request_stop(handle);
            (void)lamp_debug_join(handle, UINT64_MAX);
        }
        ether_shutdown(handle->vm);
        vm_destroy(handle->vm);
    }
    free(handle);
}

lamp_debug_vm_state_t lamp_debug_state(const lamp_debug_vm_t *handle) {
    if (!handle || !handle->vm) {
        return LAMP_DEBUG_VM_STOPPED;
    }
    if (atomic_is_vm_panicked(handle->vm)) {
        return LAMP_DEBUG_VM_PANICKED;
    }
    if (atomic_load_explicit(&handle->runner_finished, memory_order_acquire) ||
        atomic_is_vm_stopped(handle->vm)) {
        return LAMP_DEBUG_VM_STOPPED;
    }
    if (atomic_load_explicit(&handle->vm->debug_pause_requested,
                             memory_order_acquire)) {
        return LAMP_DEBUG_VM_PAUSED;
    }
    if (atomic_load_explicit(&handle->runner_started, memory_order_acquire)) {
        return LAMP_DEBUG_VM_RUNNING;
    }
    return LAMP_DEBUG_VM_CREATED;
}

lamp_debug_status_t lamp_debug_get_stats(lamp_debug_vm_t *handle,
                                         lamp_debug_stats_v1 *out_stats) {
    VmRuntimeStats stats;
    if (!handle || !handle->vm || !out_stats ||
        out_stats->struct_size < sizeof(*out_stats)) {
        return LAMP_DEBUG_INVALID_ARGUMENT;
    }
    vm_runtime_stats_sample(handle->vm, &stats);
    out_stats->state = (uint32_t)lamp_debug_state(handle);
    out_stats->cpu_frequency_hz = stats.cpu_frequency_hz;
    out_stats->virtual_cycles = stats.virtual_cycles;
    out_stats->executed_instructions = stats.executed_instructions;
    out_stats->execution_rate_hz = stats.execution_rate_hz;
    out_stats->uptime_ns = stats.uptime_ns;
    out_stats->host_resident_bytes = stats.host_resident_bytes;
    out_stats->guest_ram_bytes = stats.guest_ram_bytes;
    out_stats->core_count = stats.core_count;
    out_stats->active_core_count = stats.active_core_count;
    return LAMP_DEBUG_OK;
}

static int lamp_debug_can_inspect(const lamp_debug_vm_t *handle) {
    const lamp_debug_vm_state_t state = lamp_debug_state(handle);
    return state == LAMP_DEBUG_VM_CREATED ||
           state == LAMP_DEBUG_VM_PAUSED ||
           state == LAMP_DEBUG_VM_STOPPED ||
           state == LAMP_DEBUG_VM_PANICKED;
}

lamp_debug_status_t lamp_debug_get_cpu(
    lamp_debug_vm_t *handle,
    uint32_t core_id,
    lamp_debug_cpu_snapshot_v1 *out_cpu) {
    const VCPU *cpu;
    if (!handle || !handle->vm || !out_cpu ||
        out_cpu->struct_size < sizeof(*out_cpu) ||
        core_id >= (uint32_t)handle->vm->smp_cores) {
        return LAMP_DEBUG_INVALID_ARGUMENT;
    }
    if (!lamp_debug_can_inspect(handle)) {
        lamp_debug_set_error(handle, "pause the VM before reading CPU state");
        return LAMP_DEBUG_INVALID_STATE;
    }
    cpu = &handle->vm->cpus[core_id];
    out_cpu->core_id = core_id;
    memcpy(out_cpu->registers, cpu->regs, sizeof(out_cpu->registers));
    out_cpu->ip = cpu->ip;
    out_cpu->last_ip = cpu->last_ip;
    out_cpu->flags = cpu->flags;
    out_cpu->call_stack_pointer = cpu->csp;
    out_cpu->data_stack_pointer = cpu->dsp;
    out_cpu->interrupt_stack_pointer = cpu->isp;
    out_cpu->active_interrupt = cpu->active_interrupt_no;
    out_cpu->in_interrupt = (uint8_t)(cpu->in_interrupt != 0);
    out_cpu->irq_masked = (uint8_t)(cpu->irq_masked != 0);
    out_cpu->is_bootstrap_processor = (uint8_t)(cpu->is_bsp != 0);
    out_cpu->reserved = 0u;
    return LAMP_DEBUG_OK;
}

lamp_debug_status_t lamp_debug_read_memory(lamp_debug_vm_t *handle,
                                           uint32_t address,
                                           uint8_t *destination,
                                           size_t size) {
    if (!handle || !handle->vm || (!destination && size != 0u) ||
        (uint64_t)address + (uint64_t)size > handle->vm->memory_size) {
        return LAMP_DEBUG_INVALID_ARGUMENT;
    }
    if (!lamp_debug_can_inspect(handle)) {
        lamp_debug_set_error(handle, "pause the VM before reading memory");
        return LAMP_DEBUG_INVALID_STATE;
    }
    memcpy(destination, handle->vm->memory + address, size);
    return LAMP_DEBUG_OK;
}

lamp_debug_status_t lamp_debug_write_memory(lamp_debug_vm_t *handle,
                                            uint32_t address,
                                            const uint8_t *source,
                                            size_t size) {
    if (!handle || !handle->vm || (!source && size != 0u) ||
        (uint64_t)address + (uint64_t)size > handle->vm->memory_size) {
        return LAMP_DEBUG_INVALID_ARGUMENT;
    }
    if (!lamp_debug_can_inspect(handle)) {
        lamp_debug_set_error(handle, "pause the VM before writing memory");
        return LAMP_DEBUG_INVALID_STATE;
    }
    memcpy(handle->vm->memory + address, source, size);
    vm_ram_mark_written(handle->vm, address, size);
    return LAMP_DEBUG_OK;
}

lamp_debug_status_t lamp_debug_read_framebuffer(
    lamp_debug_vm_t *handle,
    uint32_t *destination,
    size_t pixel_capacity) {
    const size_t required = (size_t)FB_WIDTH * (size_t)FB_HEIGHT;
    if (!handle || !handle->vm || !destination || pixel_capacity < required) {
        return LAMP_DEBUG_INVALID_ARGUMENT;
    }
    for (size_t row = 0u; row < FB_HEIGHT; row++) {
        vm_fb_row_lock(handle->vm, row);
        memcpy(destination + row * FB_WIDTH,
               handle->vm->fb + row * FB_WIDTH,
               (size_t)FB_WIDTH * sizeof(uint32_t));
        vm_fb_row_unlock(handle->vm, row);
    }
    return LAMP_DEBUG_OK;
}

lamp_debug_status_t lamp_debug_send_key(lamp_debug_vm_t *handle,
                                         uint8_t set1_scancode,
                                         uint8_t extended,
                                         uint8_t pressed) {
    int queued = 1;
    if (!handle || !handle->vm || set1_scancode == 0u ||
        set1_scancode > 0x7fu) {
        return LAMP_DEBUG_INVALID_ARGUMENT;
    }
    if (extended != 0u) {
        queued = vm_ps2_kbd_enqueue(handle->vm, 0xe0u);
    }
    queued = vm_ps2_kbd_enqueue(
        handle->vm,
        pressed != 0u ? set1_scancode :
                         (uint8_t)(set1_scancode | 0x80u)) && queued;
    if (!queued) {
        lamp_debug_set_error(handle, "guest keyboard input queue is full");
        return LAMP_DEBUG_IO_ERROR;
    }
    return LAMP_DEBUG_OK;
}

lamp_debug_status_t lamp_debug_send_mouse(lamp_debug_vm_t *handle,
                                           int32_t delta_x,
                                           int32_t delta_y,
                                           uint8_t buttons) {
    int64_t remaining_x = delta_x;
    int64_t remaining_y = delta_y;
    int queued = 1;
    if (!handle || !handle->vm || (buttons & ~0x07u) != 0u) {
        return LAMP_DEBUG_INVALID_ARGUMENT;
    }
    do {
        int packet_x = (int)remaining_x;
        int packet_y = (int)remaining_y;
        int ps2_y;
        uint8_t flags = 0x08u;
        if (remaining_x > 127) packet_x = 127;
        if (remaining_x < -127) packet_x = -127;
        if (remaining_y > 127) packet_y = 127;
        if (remaining_y < -127) packet_y = -127;
        remaining_x -= packet_x;
        remaining_y -= packet_y;
        ps2_y = -packet_y;
        if ((buttons & LAMP_DEBUG_MOUSE_LEFT) != 0u) flags |= 0x01u;
        if ((buttons & LAMP_DEBUG_MOUSE_RIGHT) != 0u) flags |= 0x02u;
        if ((buttons & LAMP_DEBUG_MOUSE_MIDDLE) != 0u) flags |= 0x04u;
        if (packet_x < 0) flags |= 0x10u;
        if (ps2_y < 0) flags |= 0x20u;
        queued = vm_ps2_mouse_enqueue_packet(
            handle->vm,
            flags,
            (uint8_t)(int8_t)packet_x,
            (uint8_t)(int8_t)ps2_y) && queued;
    } while (remaining_x != 0 || remaining_y != 0);
    if (!queued) {
        lamp_debug_set_error(handle, "guest mouse input queue is full");
        return LAMP_DEBUG_IO_ERROR;
    }
    return LAMP_DEBUG_OK;
}

size_t lamp_debug_serial_read(lamp_debug_vm_t *handle,
                              uint8_t *destination,
                              size_t capacity) {
    size_t count = 0u;
    if (!handle || !handle->vm || (!destination && capacity != 0u)) {
        return 0u;
    }
    while (count < capacity &&
           vm_serial_tx_dequeue(handle->vm, &destination[count])) {
        count++;
    }
    return count;
}

size_t lamp_debug_serial_write(lamp_debug_vm_t *handle,
                               const uint8_t *source,
                               size_t size) {
    size_t count = 0u;
    if (!handle || !handle->vm || (!source && size != 0u)) {
        return 0u;
    }
    while (count < size && vm_serial_rx_enqueue(handle->vm, source[count])) {
        count++;
    }
    return count;
}

const char *lamp_debug_last_error(const lamp_debug_vm_t *handle) {
    return handle ? handle->last_error : lamp_debug_create_error;
}
