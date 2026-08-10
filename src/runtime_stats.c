#include "runtime_stats.h"

#include <stdio.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

enum { RUNTIME_STATS_SAMPLE_NS = 250000000u };

static uint64_t runtime_stats_resident_bytes(void) {
#if defined(__APPLE__)
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  (task_info_t)&info, &count) != KERN_SUCCESS) {
        return 0u;
    }
    return (uint64_t)info.resident_size;
#elif defined(__linux__)
    unsigned long total_pages = 0u;
    unsigned long resident_pages = 0u;
    long page_size;
    FILE *stream = fopen("/proc/self/statm", "r");
    if (!stream) {
        return 0u;
    }
    if (fscanf(stream, "%lu %lu", &total_pages, &resident_pages) != 2) {
        fclose(stream);
        return 0u;
    }
    fclose(stream);
    (void)total_pages;
    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        return 0u;
    }
    return (uint64_t)resident_pages * (uint64_t)page_size;
#else
    return 0u;
#endif
}

static uint64_t runtime_stats_virtual_cycles(uint64_t uptime_ns,
                                             uint64_t frequency_hz) {
    const uint64_t seconds = uptime_ns / 1000000000ull;
    const uint64_t remainder = uptime_ns % 1000000000ull;
    return seconds * frequency_hz +
           (remainder * frequency_hz) / 1000000000ull;
}

void vm_runtime_stats_sample(VM *vm, VmRuntimeStats *out) {
    uint64_t now_ns;
    uint64_t instructions = 0u;
    uint32_t active_cores = 0u;
    if (!vm || !out) {
        return;
    }

    now_ns = host_monotonic_time_ns();
    for (int core = 0; core < vm->smp_cores; core++) {
        instructions += atomic_load_explicit(&vm->cpus[core].execution_times,
                                             memory_order_relaxed);
        if (atomic_load_explicit(&vm->core_released[core],
                                 memory_order_relaxed)) {
            active_cores++;
        }
    }

    pthread_mutex_lock(&vm->runtime_stats_lock);
    if (vm->runtime_stats_last_sample_ns == 0u) {
        vm->runtime_stats_last_sample_ns = now_ns;
        vm->runtime_stats_last_instructions = instructions;
        vm->runtime_stats_cached.execution_rate_hz = 0u;
        vm->runtime_stats_cached.host_resident_bytes =
            runtime_stats_resident_bytes();
    } else if (now_ns - vm->runtime_stats_last_sample_ns >=
               RUNTIME_STATS_SAMPLE_NS) {
        const uint64_t elapsed_ns =
            now_ns - vm->runtime_stats_last_sample_ns;
        const uint64_t executed =
            instructions - vm->runtime_stats_last_instructions;
        vm->runtime_stats_cached.execution_rate_hz =
            (executed * 1000000000ull) / elapsed_ns;
        vm->runtime_stats_cached.host_resident_bytes =
            runtime_stats_resident_bytes();
        vm->runtime_stats_last_sample_ns = now_ns;
        vm->runtime_stats_last_instructions = instructions;
    }

    vm->runtime_stats_cached.cpu_frequency_hz = vm->cpu_frequency_hz;
    vm->runtime_stats_cached.uptime_ns =
        now_ns >= vm->start_monotonic_ns ?
        now_ns - vm->start_monotonic_ns : 0u;
    vm->runtime_stats_cached.virtual_cycles = runtime_stats_virtual_cycles(
        vm->runtime_stats_cached.uptime_ns, vm->cpu_frequency_hz);
    vm->runtime_stats_cached.executed_instructions = instructions;
    vm->runtime_stats_cached.guest_ram_bytes = vm->memory_size;
    vm->runtime_stats_cached.core_count = (uint32_t)vm->smp_cores;
    vm->runtime_stats_cached.active_core_count = active_cores;
    *out = vm->runtime_stats_cached;
    pthread_mutex_unlock(&vm->runtime_stats_lock);
}
