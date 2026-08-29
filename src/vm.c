#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#if defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#endif
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_thread.h>
#include <pthread.h>

#include "engines/engine.h"
#include "vm.h"
#include "vm_runtime.h"
#include "stack.h"
#include "io.h"
#include "panic.h"
#include "runtime_log.h"
#include "loadbin.h"
#include "interrupt.h"
#include "memory.h"
#include "io_devices/disk/disk.h"
#include "io_devices/audio/audio.h"
#include "io_devices/ether/ether.h"
#include "io_devices/ether/ether_backend.h"
#include "io_devices/frame/frame.h"
#include "io_devices/gpu/gpu.h"
#include "io_devices/sysinfo/sysinfo_mmio_register.h"
#include "io_devices/intc/intc_mmio_register.h"
#include "io_devices/iommu/iommu_mmio_register.h"
#include "io_devices/mmu/mmu_mmio_register.h"
#include "io_devices/pcie/pcie.h"
#include "io_devices/time/time_mmio_register.h"
#include "io_devices/vga_display/vga_mmio_register.h"
#include "float.h"
#include "flags.h"
#include "debug.h"
#include "selftest.h"
#include "vnc.h"

const size_t MEM_SIZE = 1048576 * 64; // 64MB
enum { EXECUTION_TIMES_FLUSH_INTERVAL = 1024 };
enum { DEVICE_POLL_CHECK_INTERVAL = 2048 };
enum { DEVICE_POLL_PERIOD_NS = 1000000 };
enum { CPU_PACE_TARGET_NS = 250000 };
enum { CPU_PACE_MIN_QUANTUM = 64 };
enum { CPU_PACE_MAX_QUANTUM = 16384 };
enum { CPU_PACE_REBASE_NS = 2000000 };

typedef struct {
    VM *vm;
    int core_id;
} CpuThreadArg;

void vm_mmio_mark_range(VM *vm, uint32_t start, uint32_t end) {
    uint32_t first_page;
    uint32_t last_page;
    if (!vm || end < start) {
        return;
    }
    first_page = start >> VM_MMIO_PAGE_SHIFT;
    last_page = end >> VM_MMIO_PAGE_SHIFT;
    for (uint32_t page = first_page; page <= last_page; page++) {
        atomic_fetch_or_explicit(&vm->mmio_page_map[page >> 3u],
                                 (unsigned char)(1u << (page & 7u)),
                                 memory_order_release);
    }
    atomic_fetch_add_explicit(&vm->mmio_epoch, 1u, memory_order_release);
}

static void vm_build_mmio_page_map(VM *vm) {
    if (!vm) {
        return;
    }
    for (size_t i = 0; i < VM_MMIO_PAGE_MAP_BYTES; i++) {
        atomic_init(&vm->mmio_page_map[i], 0u);
    }
    atomic_init(&vm->mmio_epoch, 1u);
    for (int i = 0; i < vm->mmio_count; i++) {
        const MMIO_Device *dev = vm->mmio_devices[i];
        if (!dev) {
            continue;
        }
        vm_mmio_mark_range(vm, dev->start, dev->end);
    }
    vm->mmio_page_map_ready = 1u;
}

_Thread_local VCPU *vm_tls_vcpu = NULL;

static inline void vm_flush_execution_times(VCPU *cpu, uint64_t *local_cycles) {
    if (*local_cycles == 0) {
        return;
    }
    atomic_fetch_add_explicit(&cpu->execution_times, *local_cycles, memory_order_relaxed);
    *local_cycles = 0;
}

static void vm_set_current_thread_priority(SDL_ThreadPriority priority) {
#if defined(VM_STATIC_BUILD) && defined(__linux__)
    int nice_level = 0;
    const long thread_id = syscall(SYS_gettid);

    switch (priority) {
        case SDL_THREAD_PRIORITY_LOW:
            nice_level = 10;
            break;
        case SDL_THREAD_PRIORITY_HIGH:
            nice_level = -10;
            break;
        case SDL_THREAD_PRIORITY_TIME_CRITICAL:
            nice_level = -20;
            break;
        case SDL_THREAD_PRIORITY_NORMAL:
        default:
            nice_level = 0;
            break;
    }

    /* A fully static glibc executable must not fall back through SDL's
     * RTKit/D-Bus path: that path dlopens host DSOs and can introduce a
     * second, incompatible glibc/loader into the process.  Linux applies
     * setpriority() to the calling thread when addressed by its TID.  Keep
     * this best-effort, as raising priority normally requires CAP_SYS_NICE. */
    if (thread_id > 0) {
        (void)setpriority(PRIO_PROCESS, (id_t)thread_id, nice_level);
    }
#else
    (void)SDL_SetCurrentThreadPriority(priority);
#endif
}

static uint64_t vm_cpu_cycles_to_ns(uint64_t cycles, uint64_t frequency_hz) {
    const uint64_t seconds = cycles / frequency_hz;
    const uint64_t remainder = cycles % frequency_hz;
    return seconds * 1000000000ull +
           (remainder * 1000000000ull) / frequency_hz;
}

static uint32_t vm_cpu_pace_quantum(uint64_t frequency_hz) {
    uint64_t quantum = frequency_hz /
                       (1000000000ull / CPU_PACE_TARGET_NS);
    if (quantum < CPU_PACE_MIN_QUANTUM) {
        quantum = CPU_PACE_MIN_QUANTUM;
    }
    if (quantum > CPU_PACE_MAX_QUANTUM) {
        quantum = CPU_PACE_MAX_QUANTUM;
    }
    return (uint32_t)quantum;
}

static void vm_pace_cpu(VM *vm, uint64_t *epoch_ns,
                        uint64_t *paced_cycles) {
    uint64_t now_ns;
    uint64_t deadline_ns;
    if (!vm || !epoch_ns || !paced_cycles || vm->cpu_frequency_hz == 0u) {
        return;
    }
    now_ns = host_monotonic_time_ns();
    deadline_ns = *epoch_ns +
        vm_cpu_cycles_to_ns(*paced_cycles, vm->cpu_frequency_hz);
    if (deadline_ns > now_ns) {
        SDL_DelayPrecise(deadline_ns - now_ns);
    } else if (now_ns - deadline_ns > CPU_PACE_REBASE_NS) {
        /* A debugger stop or a host scheduling stall must not create a large
         * catch-up burst. Start a fresh execution budget at current time. */
        *epoch_ns = now_ns;
        *paced_cycles = 0u;
    }
}

static void *vm_thread(void *arg) {
    CpuThreadArg *thread_arg = (CpuThreadArg *)arg;
    VM *vm = thread_arg->vm;
    int core_id = thread_arg->core_id;
    free(thread_arg);
    vm_set_current_thread_priority(core_id == 0 ?
        SDL_THREAD_PRIORITY_HIGH : SDL_THREAD_PRIORITY_NORMAL);
    vm_tls_vcpu = &vm->cpus[core_id];
    uint64_t local_cycles = 0;
    uint64_t paced_cycles = 0u;
    uint64_t pace_epoch_ns = 0u;
    uint32_t device_poll_cycles = 0;
    uint64_t next_device_poll_ns = 0u;
    uint32_t pace_check_cycles = 0u;
    const uint32_t pace_quantum = vm_cpu_pace_quantum(vm->cpu_frequency_hz);
    int pacing_started = 0;

    while (1) {
        if (atomic_is_vm_stopped(vm)) {
            break;
        }
        if (core_id != 0 && !atomic_load_explicit(&vm->core_released[core_id], memory_order_acquire)) {
            sched_yield();
            continue;
        }
        if (atomic_load_explicit(&vm->debug_pause_requested,
                                 memory_order_acquire)) {
            vm_flush_execution_times(vm_tls_vcpu, &local_cycles);
            atomic_fetch_add_explicit(&vm->debug_paused_core_count, 1u,
                                      memory_order_acq_rel);
            while (atomic_load_explicit(&vm->debug_pause_requested,
                                        memory_order_acquire) &&
                   !atomic_is_vm_stopped(vm)) {
                if ((uint32_t)core_id ==
                        atomic_load_explicit(&vm->debug_step_core,
                                             memory_order_relaxed) &&
                    atomic_exchange_explicit(&vm->debug_step_requested, false,
                                             memory_order_acq_rel)) {
                    uint32_t stepped;
                    if (__builtin_expect(
                            vm_interrupt_pending_fast(vm, vm_tls_vcpu), 0)) {
                        vm_handle_interrupts(vm, vm_tls_vcpu);
                    }
                    stepped = vm_engine_execute_single(vm, vm_tls_vcpu);
                    local_cycles += stepped;
                    vm_flush_execution_times(vm_tls_vcpu, &local_cycles);
                    atomic_store_explicit(&vm->debug_step_completed, true,
                                          memory_order_release);
                    continue;
                }
                sched_yield();
            }
            atomic_fetch_sub_explicit(&vm->debug_paused_core_count, 1u,
                                      memory_order_acq_rel);
            continue;
        }
        if (!pacing_started) {
            pace_epoch_ns = host_monotonic_time_ns();
            paced_cycles = 0u;
            pace_check_cycles = 0u;
            pacing_started = 1;
        }
        if (core_id == 0) {
            vm_debug_pause_if_needed(vm, (uint32_t) vm_tls_vcpu->ip);
        }
        if (__builtin_expect(vm_interrupt_pending_fast(vm, vm_tls_vcpu), 0)) {
            vm_handle_interrupts(vm, vm_tls_vcpu);
        }
        const uint32_t executed_now =
            vm_engine_execute_quantum(vm, vm_tls_vcpu);
        local_cycles += executed_now;
        paced_cycles += executed_now;
        pace_check_cycles += executed_now;
        if (core_id == 0) {
            device_poll_cycles += executed_now;
        }
        if (local_cycles >= EXECUTION_TIMES_FLUSH_INTERVAL) {
            vm_flush_execution_times(vm_tls_vcpu, &local_cycles);
        }
        if (core_id == 0 && device_poll_cycles >= DEVICE_POLL_CHECK_INTERVAL) {
            const uint64_t now_ns = host_monotonic_time_ns();
            if (next_device_poll_ns == 0u || now_ns >= next_device_poll_ns) {
                ether_poll(vm);
                audio_poll(vm);
                next_device_poll_ns = now_ns + DEVICE_POLL_PERIOD_NS;
            }
            device_poll_cycles = 0;
        }
        if (pace_check_cycles >= pace_quantum) {
            vm_pace_cpu(vm, &pace_epoch_ns, &paced_cycles);
            pace_check_cycles = 0u;
        }
    }
    if (core_id == 0 && device_poll_cycles > 0) {
        ether_poll(vm);
        audio_poll(vm);
    }
    vm_flush_execution_times(vm_tls_vcpu, &local_cycles);
    return NULL;
}

#ifndef LAMPVM_EMBEDDED
static void vm_run_serial(VM *vm) {
    const int cores = (vm->smp_cores > 0) ? vm->smp_cores : 1;
    pthread_t *thread_ids = malloc(sizeof(pthread_t) * (size_t)cores);
    int created_threads = 0;
    int stdin_flags = -1;
    int stdin_restore = 0;
    int raw_mode = 0;
    if (!thread_ids) {
        panic("Failed to allocate CPU thread list", vm);
        return;
    }

    stdin_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (stdin_flags >= 0) {
        if (fcntl(STDIN_FILENO, F_SETFL, stdin_flags | O_NONBLOCK) == 0) {
            stdin_restore = 1;
        }
    }
    if (isatty(STDIN_FILENO)) {
        enable_raw_mode();
        raw_mode = 1;
    }

    for (int i = 0; i < cores; i++) {
        CpuThreadArg *arg = malloc(sizeof(CpuThreadArg));
        if (!arg) {
            panic("Failed to allocate CPU thread argument", vm);
            atomic_set_vm_halt(vm, 1);
            break;
        }
        arg->vm = vm;
        arg->core_id = i;
        if (pthread_create(&thread_ids[i], NULL, vm_thread, arg) != 0) {
            free(arg);
            panic("Failed to create CPU thread", vm);
            atomic_set_vm_halt(vm, 1);
            break;
        }
        created_threads++;
    }

    while (!atomic_is_vm_stopped(vm)) {
        vm_handle_keyboard(vm);
        usleep(1000);
    }

    if (raw_mode) {
        disable_raw_mode();
    }
    if (stdin_restore) {
        (void)fcntl(STDIN_FILENO, F_SETFL, stdin_flags);
    }

    for (int i = 0; i < created_threads; i++) {
        pthread_join(thread_ids[i], NULL);
    }
    free(thread_ids);
}
#endif

int vm_run_headless(VM *vm, uint64_t timeout_ms) {
    const int cores = (vm->smp_cores > 0) ? vm->smp_cores : 1;
    pthread_t *thread_ids = malloc(sizeof(pthread_t) * (size_t)cores);
    if (!thread_ids) {
        panic("Failed to allocate CPU thread list", vm);
        return 0;
    }

    int created_threads = 0;
    for (int i = 0; i < cores; i++) {
        CpuThreadArg *arg = malloc(sizeof(CpuThreadArg));
        if (!arg) {
            atomic_set_vm_halt(vm, 1);;
            break;
        }
        arg->vm = vm;
        arg->core_id = i;
        if (pthread_create(&thread_ids[i], NULL, vm_thread, arg) != 0) {
            free(arg);
            atomic_set_vm_halt(vm, 1);;
            break;
        }
        created_threads++;
    }

    const uint64_t start_ns = host_monotonic_time_ns();
    while (!atomic_is_vm_stopped(vm)) {
        const uint64_t elapsed_ms = (host_monotonic_time_ns() - start_ns) / 1000000ull;
        if (elapsed_ms > timeout_ms) {
            atomic_set_vm_halt(vm, 1);;
            break;
        }
        usleep(1000);
    }

    for (int i = 0; i < created_threads; i++) {
        pthread_join(thread_ids[i], NULL);
    }
    free(thread_ids);
    return atomic_is_vm_panicked(vm) ? 0 : 1;
}

void vm_dump(const VM *vm, int mem_preview) {
    const VCPU *cpu = (vm && vm->cpus) ? &vm->cpus[0] : NULL;
    if (!vm || !cpu)
        return;
    printf("VM dump:\n");
    printf("Core: 0\n");
    printf("CPU state: csp=%d dsp=%d isp=%d in_interrupt=%d irq_masked=%d\n",
           cpu->csp,
           cpu->dsp,
           cpu->isp,
           cpu->in_interrupt,
           cpu->irq_masked);
    printf("Registers:\n");
    for (int i = 0; i < REG_COUNT; i++) {
        printf("r%d = %d\n", i, cpu->regs[i]);
    }

    printf("Call Stack (top -> bottom):\n");
    for (int i = cpu->csp; i < CALL_STACK_SIZE; i++) {
        const vm_addr_t addr = cpu->call_stack_base + (vm_addr_t)i * 8u;
        const uint64_t v = (uint64_t) vm->memory[addr + 0]
            | ((uint64_t) vm->memory[addr + 1] << 8)
            | ((uint64_t) vm->memory[addr + 2] << 16)
            | ((uint64_t) vm->memory[addr + 3] << 24)
            | ((uint64_t) vm->memory[addr + 4] << 32)
            | ((uint64_t) vm->memory[addr + 5] << 40)
            | ((uint64_t) vm->memory[addr + 6] << 48)
            | ((uint64_t) vm->memory[addr + 7] << 56);
        printf("[%d] = %llu\n", i, (unsigned long long) v);
    }
    if (cpu->csp == CALL_STACK_SIZE) {
        printf("<empty>\n");
    }
    printf("Data Stack (top -> bottom):\n");
    for (int i = cpu->dsp; i < DATA_STACK_SIZE; i++) {
        const vm_addr_t addr = cpu->data_stack_base + (vm_addr_t)i * 4u;
        const uint32_t v = (uint32_t) vm->memory[addr + 0]
            | ((uint32_t) vm->memory[addr + 1] << 8)
            | ((uint32_t) vm->memory[addr + 2] << 16)
            | ((uint32_t) vm->memory[addr + 3] << 24);
        printf("[%d] = %u\n", i, v);
    }
    if (cpu->dsp == DATA_STACK_SIZE) {
        printf("<empty>\n");
    }
    printf("Memory (first %d cells):\n", mem_preview);
    for (int i = 0; i < mem_preview && i < (ssize_t)MEM_SIZE; i++) {
        printf("[%d] = %d\n", i, vm->memory[i]);
    }
    printf("IP = %lu\n", cpu->ip);
    printf("ZF = %d\n", (cpu->flags & FLAG_ZF) != 0);
}

void vm_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    printf("======================\n\n");
    fprintf(stderr, "VM encountered a fatal error and could not recover:\n");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
    printf("\n======================\n");
    fflush(stderr);
}

VM *g_host_dump_vm = NULL;

VM *vm_create(size_t memory_size,
              const uint64_t *program,
              size_t program_size,
              const uint8_t *data,
              size_t data_size,
              const ProgramLayout *layout,
              int smp_cores) {
    VM *vm = malloc(sizeof(VM));
    if (!vm)
        return NULL;
    g_host_dump_vm = vm;

    memset(vm, 0, sizeof(VM));
    vm->smp_cores = (smp_cores > 0) ? smp_cores : 1;
    vm->execution_engine = VM_ENGINE_CLASSIC;
    vm->cpu_frequency_hz = (uint64_t)VM_DEFAULT_CPU_MHZ * 1000000ull;
    vm->cpus = calloc((size_t)vm->smp_cores, sizeof(VCPU));
    atomic_init(&vm->stop_flags, 0u);
    atomic_init(&vm->debug_pause_requested, false);
    atomic_init(&vm->debug_paused_core_count, 0u);
    atomic_init(&vm->debug_step_requested, false);
    atomic_init(&vm->debug_step_completed, false);
    atomic_init(&vm->debug_step_core, 0u);
    atomic_init(&vm->ram_write_tracking_active, false);
    if (!vm->cpus) {
        free(vm);
        vm_error("Couldn't create CPU.");
        return NULL;
    }
    vm->core_released = calloc((size_t)vm->smp_cores, sizeof(atomic_bool));
    if (!vm->core_released) {
        free(vm->cpus);
        free(vm);
        vm_error("Couldn't allocate VM CPU list");
        return NULL;
    }
    vm->interrupt_bitmap = calloc((size_t)vm->smp_cores * (size_t)IRQ_BITMAP_WORDS,
                                  sizeof(atomic_uint_fast64_t));
    if (!vm->interrupt_bitmap) {
        free(vm->core_released);
        free(vm->cpus);
        free(vm);
        vm_error("Couldn't allocate VM CPU Interrupt bitmap");
        return NULL;
    }
    vm->interrupt_enable_bitmap = calloc((size_t)vm->smp_cores * (size_t)IRQ_BITMAP_WORDS,
                                         sizeof(atomic_uint_fast64_t));
    if (!vm->interrupt_enable_bitmap) {
        free(vm->interrupt_bitmap);
        free(vm->core_released);
        free(vm->cpus);
        free(vm);
        vm_error("Couldn't allocate VM CPU Interrupt enable bitmap");
        return NULL;
    }
    vm->interrupt_pending_summary = calloc((size_t)vm->smp_cores,
                                           sizeof(atomic_uint_fast32_t));
    if (!vm->interrupt_pending_summary) {
        free(vm->interrupt_enable_bitmap);
        free(vm->interrupt_bitmap);
        free(vm->core_released);
        free(vm->cpus);
        free(vm);
        vm_error("Couldn't allocate VM CPU Interrupt pending summary");
        return NULL;
    }
    for (int core = 0; core < vm->smp_cores; core++) {
        atomic_init(&vm->interrupt_pending_summary[core], 0u);
    }

    pthread_mutexattr_t shared_attr;
    pthread_mutexattr_init(&shared_attr);
    pthread_mutexattr_settype(&shared_attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&vm->shared_lock, &shared_attr);
    pthread_mutexattr_destroy(&shared_attr);
    pthread_mutex_init(&vm->runtime_stats_lock, NULL);

    vm->memory_size = memory_size;
    vm->memory = malloc(memory_size);
    if (!vm->memory) {
        pthread_mutex_destroy(&vm->runtime_stats_lock);
        pthread_mutex_destroy(&vm->shared_lock);
        free(vm->interrupt_pending_summary);
        free(vm->interrupt_enable_bitmap);
        free(vm->interrupt_bitmap);
        free(vm->core_released);
        free(vm->cpus);
        free(vm);
        vm_error("Couldn't allocate VM memory %zu", memory_size);
        return NULL;
    }
    memset(vm->memory, 0, memory_size);
    vm->ram_page_count = (memory_size >> VM_RAM_PAGE_SHIFT) +
        ((memory_size & VM_RAM_PAGE_MASK) != 0u ? 1u : 0u);
    vm->ram_page_generations = calloc(
        vm->ram_page_count, sizeof(*vm->ram_page_generations));
    if (!vm->ram_page_generations) {
        free(vm->memory);
        pthread_mutex_destroy(&vm->runtime_stats_lock);
        pthread_mutex_destroy(&vm->shared_lock);
        free(vm->interrupt_pending_summary);
        free(vm->interrupt_enable_bitmap);
        free(vm->interrupt_bitmap);
        free(vm->core_released);
        free(vm->cpus);
        free(vm);
        vm_error("Couldn't allocate RAM page generations");
        return NULL;
    }
    for (size_t page = 0u; page < vm->ram_page_count; page++) {
        atomic_init(&vm->ram_page_generations[page], 0u);
    }

    size_t fb_base = FB_BASE(memory_size);

    vm->fb = malloc(FB_SIZE);
    vm->fb_front = malloc(FB_SIZE);
    if (!vm->fb || !vm->fb_front) {
        free(vm->fb_front);
        free(vm->fb);
        free(vm->ram_page_generations);
        free(vm->memory);
        pthread_mutex_destroy(&vm->runtime_stats_lock);
        pthread_mutex_destroy(&vm->shared_lock);
        free(vm->interrupt_pending_summary);
        free(vm->interrupt_enable_bitmap);
        free(vm->interrupt_bitmap);
        free(vm->core_released);
        free(vm->cpus);
        free(vm);
        vm_error("Couldn't allocate VM frame buffer");
        return NULL;
    }
    memset(vm->fb, 0, FB_SIZE);
    memset(vm->fb_front, 0, FB_SIZE);
    for (size_t row = 0; row < FB_HEIGHT; row++) {
        atomic_init(&vm->fb_row_dirty[row], 1u);
        if (pthread_mutex_init(&vm->fb_row_locks[row], NULL) != 0) {
            for (size_t done = 0; done < row; done++) {
                pthread_mutex_destroy(&vm->fb_row_locks[done]);
            }
            free(vm->fb_front);
            free(vm->fb);
            free(vm->ram_page_generations);
            free(vm->memory);
            pthread_mutex_destroy(&vm->runtime_stats_lock);
            pthread_mutex_destroy(&vm->shared_lock);
            free(vm->interrupt_pending_summary);
            free(vm->interrupt_enable_bitmap);
            free(vm->interrupt_bitmap);
            free(vm->core_released);
            free(vm->cpus);
            free(vm);
            vm_error("Couldn't allocate VM frame buffer row lock");
            return NULL;
        }
    }
    VM_RUNTIME_LOG("vm->fb = %p\n", (void *)vm->fb);
    VM_RUNTIME_LOG("fb_base = 0x%zx\n", fb_base);
    VM_RUNTIME_LOG("fb address mod 4 = %zu\n", ((size_t)vm->fb) % 4);
    VM_RUNTIME_LOG("Initializing MMIO....\n");
    vm->mmio_count = 0;
    memset(vm->mmio_devices, 0, sizeof(vm->mmio_devices));
    vm->disk_size_bytes = DISK_SIZE;
    register_fb_mmio(vm);
    register_time_mmio(vm);
    register_intc_mmio(vm);
    register_iommu_mmio(vm);
    register_mmu_mmio(vm);
    register_sysinfo_mmio(vm);
    register_pcie_mmio(vm);
    vm_build_mmio_page_map(vm);
    if (gpu_device_init(vm) != 0) {
        vm_error("Couldn't initialize optional PCI display device");
    }
    if (audio_device_init(vm) != 0) {
        vm_error("Couldn't initialize optional PCI audio device");
    }
    size_t prog_bytes = program_size * sizeof(uint64_t);
    uint32_t text_base = PROGRAM_BASE;
    uint32_t data_base = PROGRAM_BASE + (uint32_t) prog_bytes;
    uint32_t bss_base = data_base + (uint32_t) data_size;
    uint32_t bss_size = 0;

    if (layout) {
        text_base = layout->text_base;
        data_base = layout->data_base;
        bss_base = layout->bss_base;
        bss_size = layout->bss_size;
        if (layout->text_size != 0 && layout->text_size != prog_bytes) {
            fprintf(stderr, "Warning: layout TEXT_SIZE (%u) != program size (%zu)\n",
                    layout->text_size, prog_bytes);
        }
    }

    if ((size_t) text_base + prog_bytes > memory_size) {
        panic("Program too large\n", vm);
        return vm;
    }

    const size_t call_stack_bytes = (size_t)CALL_STACK_SIZE * 8u;
    const size_t data_stack_bytes = (size_t)DATA_STACK_SIZE * 4u;
    const size_t isr_stack_bytes = (size_t)ISR_STACK_SIZE * 8u;
    const size_t per_core_stack_bytes = call_stack_bytes + data_stack_bytes + isr_stack_bytes;
    const size_t stack_slot_bytes = per_core_stack_bytes + (size_t)VM_TASK_C_STACK_BYTES;
    size_t stack_pool_slots = (size_t)vm->smp_cores;
    size_t image_end = (size_t)text_base + prog_bytes;
    if ((size_t)data_base + data_size > image_end)
        image_end = (size_t)data_base + data_size;
    if ((size_t)bss_base + bss_size > image_end)
        image_end = (size_t)bss_base + bss_size;

    if (stack_pool_slots < VM_STACK_POOL_SLOTS) {
        stack_pool_slots = VM_STACK_POOL_SLOTS;
    }
    vm->stack_pool_size = stack_slot_bytes * stack_pool_slots;
    if (vm->stack_pool_size >= memory_size) {
        panic("Stack pool too large for RAM", vm);
        return vm;
    }
    vm->stack_pool_base = (vm_addr_t)(memory_size - vm->stack_pool_size);
    if (image_end > vm->stack_pool_base) {
        panic("Program/data overlaps stack pool", vm);
        return vm;
    }

    if (vm->smp_cores == 1) {
        if ((size_t)PROGRAM_BASE > vm->stack_pool_base) {
            panic("Legacy core stack overlaps reserved stack pool", vm);
            return vm;
        }
    }

    memcpy(vm->memory + text_base, program, prog_bytes);
    if (data && data_size > 0) {
        if ((size_t) data_base + data_size > memory_size) {
            panic("Data segment out of range\n", vm);
            return vm;
        }
        memcpy(vm->memory + data_base, data, data_size);
    }
    if (bss_size > 0) {
        if ((size_t) bss_base + bss_size > memory_size) {
            panic("BSS segment out of range\n", vm);
            return vm;
        }
        memset(vm->memory + bss_base, 0, bss_size);
        {
            size_t sample = bss_size < 64 ? bss_size : 64;
            size_t bad = 0;
            for (size_t i = 0; i < sample; i++) {
                if (vm->memory[bss_base + i] != 0) {
                    bad++;
                }
            }
            VM_RUNTIME_LOG("BSS clear: base=0x%08x size=%u first=%u last=%u bad_in_first_%zu=%zu\n",
                           bss_base,
                           bss_size,
                           vm->memory[bss_base],
                           vm->memory[bss_base + bss_size - 1],
                           sample,
                           bad);
        }
    }

    for (int i = 0; i < vm->smp_cores; i++) {
        /* BSP always uses the traditional boot stack at CALL_STACK_BASE.
         * AP cores get per-core stacks from the pool once the kernel
         * explicitly starts them via STARTAP. */
        vm_addr_t core_stack_base = (i == 0)
            ? CALL_STACK_BASE
            : vm->stack_pool_base + (vm_addr_t)(stack_slot_bytes * (size_t)(i - 1));
        atomic_init(&vm->cpus[i].execution_times, 0);
        atomic_init(&vm->cpus[i].mmu_epoch, 1u);
        vm->cpus[i].core_id = i;
        vm->cpus[i].is_bsp = (i == 0) ? 1 : 0;
        /* Only BSP starts from the BIOS entry point.  AP cores are
         * halted until the kernel explicitly sets their IP and calls
         * STARTAP. */
        vm->cpus[i].ip = (i == 0) ? text_base : 0;
        vm->cpus[i].last_ip = vm->cpus[i].ip;
        vm->cpus[i].call_stack_base = core_stack_base;
        vm->cpus[i].data_stack_base = core_stack_base + (vm_addr_t)call_stack_bytes;
        vm->cpus[i].isr_stack_base = core_stack_base + (vm_addr_t)call_stack_bytes + (vm_addr_t)data_stack_bytes;
        vm->cpus[i].csp = CALL_STACK_SIZE;
        vm->cpus[i].dsp = DATA_STACK_SIZE;
        vm->cpus[i].isp = ISR_STACK_SIZE;
        vm->cpus[i].active_interrupt_no = IVT_SIZE;
        vm->cpus[i].irq_masked = 0;
        atomic_init(&vm->core_released[i], (i == 0));
        VM_RUNTIME_LOG("[cpu%u] start ip=0x%08x csp=%u dsp=%u isp=%u r30=0x%08x r31=0x%08x call_base=0x%08x\n",
                       (unsigned)i,
                       (unsigned)vm->cpus[i].ip,
                       (unsigned)vm->cpus[i].csp,
                       (unsigned)vm->cpus[i].dsp,
                       (unsigned)vm->cpus[i].isp,
                       (unsigned)vm->cpus[i].regs[30],
                       (unsigned)vm->cpus[i].regs[31],
                       (unsigned)vm->cpus[i].call_stack_base);
        /* For APs that weren't explicitly started, gate at the
         * instruction loop so they never fetch from IP=0. */
    }

    vm->start_realtime_ns = host_unix_time_ns();
    vm->start_monotonic_ns = host_monotonic_time_ns();
    vm->suspend_count = 0;
    vm->io[SCREEN_ATTRIBUTE] = SERIAL_STATUS_TX_READY;
    vm->ps2_config = 0x03u;
    vm->ps2_status = PS2_STATUS_SYSTEM;
    vm->ps2_kbd_enabled = 1u;
    vm->ps2_mouse_enabled = 1u;
    vm->ps2_kbd_scanning = 1u;
    vm->ps2_mouse_reporting = 1u;
    vm->ps2_input_ready = 0u;
    vm->ps2_mouse_sample_rate = 100u;
    vm->ps2_mouse_resolution = 2u;
    vm->io[PS2_STATUS] = vm->ps2_status;
    vm_debug_init(vm);
    return vm;
}

void vm_destroy(VM *vm) {
    if (!vm)
        return;

    if (vm->timer_thread_started) {
        atomic_store(&vm->timer_enabled, false);
        atomic_store(&vm->timer_thread_running, false);
        pthread_join(vm->timer_worker_thread, NULL);
        vm->timer_thread_started = 0;
    }

    vm_debug_destroy(vm);
    disk_close(vm);
    audio_device_shutdown(vm);
    gpu_device_shutdown(vm);
    pthread_mutex_destroy(&vm->runtime_stats_lock);
    pthread_mutex_destroy(&vm->shared_lock);
    for (size_t row = 0; row < FB_HEIGHT; row++) {
        pthread_mutex_destroy(&vm->fb_row_locks[row]);
    }
    if (vm->interrupt_bitmap)
        free(vm->interrupt_bitmap);
    if (vm->interrupt_enable_bitmap)
        free(vm->interrupt_enable_bitmap);
    if (vm->interrupt_pending_summary)
        free(vm->interrupt_pending_summary);
    if (vm->core_released)
        free(vm->core_released);
    vm_engine_destroy_vm(vm);
    if (vm->cpus)
        free(vm->cpus);
    if (vm->memory)
        free(vm->memory);
    if (vm->ram_page_generations)
        free(vm->ram_page_generations);
    if (vm->fb)
        free(vm->fb);
    if (vm->fb_front)
        free(vm->fb_front);
    if (vm->pcie)
        free(vm->pcie);
    free(vm);
}

#ifndef LAMPVM_EMBEDDED

typedef enum {
    VM_CLI_RUN,
    VM_CLI_SELFTEST,
    VM_CLI_BENCHMARK,
    VM_CLI_HELP
} VmCliCommand;

typedef struct {
    VmCliCommand command;
    const char *program_path;
    const char *net_mode;
    int smp_cores;
    uint32_t cpu_mhz;
    VmExecutionEngine execution_engine;
} VmCliOptions;

static void vm_cli_options_init(VmCliOptions *options) {
    options->command = VM_CLI_RUN;
    options->program_path = "boot.bin";
    options->net_mode = "nat";
    options->smp_cores = 1;
    options->cpu_mhz = VM_DEFAULT_CPU_MHZ;
    options->execution_engine = VM_ENGINE_CLASSIC;
}

static void print_usage(const char *prog) {
    printf("Lamp VM\n");
    printf("\n");
    printf("Usage:\n");
    printf("  %s run [program.bin] [options]\n", prog);
    printf("  %s test\n", prog);
    printf("  %s benchmark\n", prog);
    printf("  %s help\n", prog);
    printf("\n");
    printf("Common examples:\n");
    printf("  %s run bios/boot.bin\n", prog);
    printf("  %s run bios/boot.bin --cores 2\n", prog);
    printf("  %s test\n", prog);
    printf("  %s benchmark\n", prog);
    printf("\n");
    printf("Run options:\n");
    printf("  program.bin              guest program image (default: boot.bin)\n");
    printf("  --bin <file>             legacy spelling for program.bin\n");
    printf("  --cores, --smp <n>       CPU worker thread count in [1, 64] (default: 1)\n");
    printf("  --cpu-mhz <n>            per-vCPU execution cap in [1, 10000] MHz (default: %u)\n",
           VM_DEFAULT_CPU_MHZ);
    printf("  --engine <name>          execution engine: classic, cached, threaded, or jit (default: classic)\n");
    printf("  --net <mode>             ethernet backend: null, nat, or udp:<bind-port>:<peer-port>\n");
    printf("\n");
    printf("Compatibility alias: --selftest\n");
}

static int parse_positive_int(const char *s, int *out) {
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < 1 || v > 64)
        return 0;
    *out = (int)v;
    return 1;
}

static int parse_cpu_mhz(const char *s, uint32_t *out) {
    char *end = NULL;
    unsigned long value;
    errno = 0;
    value = strtoul(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || value < 1u ||
        value > VM_MAX_CPU_MHZ) {
        return 0;
    }
    *out = (uint32_t)value;
    return 1;
}

static int parse_execution_engine(const char *s, VmExecutionEngine *out) {
    if (strcmp(s, "classic") == 0) {
        *out = VM_ENGINE_CLASSIC;
        return 1;
    }
    if (strcmp(s, "cached") == 0) {
        *out = VM_ENGINE_CACHED;
        return 1;
    }
    if (strcmp(s, "threaded") == 0) {
        *out = VM_ENGINE_THREADED;
        return 1;
    }
    if (strcmp(s, "jit") == 0) {
        *out = VM_ENGINE_JIT;
        return 1;
    }
    return 0;
}

static const char *execution_engine_name(VmExecutionEngine engine) {
    switch (engine) {
        case VM_ENGINE_CACHED: return "cached";
        case VM_ENGINE_THREADED: return "threaded";
        case VM_ENGINE_JIT: return "jit";
        case VM_ENGINE_CLASSIC:
        default: return "classic";
    }
}

static int parse_cli_options(int argc, char **argv, VmCliOptions *options) {
    int argi = 1;
    int program_path_set = 0;

    vm_cli_options_init(options);
    if (argc <= 1) {
        return 1;
    }

    if (strcmp(argv[argi], "run") == 0) {
        argi++;
    } else if (strcmp(argv[argi], "test") == 0 || strcmp(argv[argi], "selftest") == 0) {
        options->command = VM_CLI_SELFTEST;
        argi++;
    } else if (strcmp(argv[argi], "benchmark") == 0 ||
               strcmp(argv[argi], "bench") == 0) {
        options->command = VM_CLI_BENCHMARK;
        argi++;
    } else if (strcmp(argv[argi], "help") == 0 ||
               strcmp(argv[argi], "--help") == 0 ||
               strcmp(argv[argi], "-h") == 0) {
        options->command = VM_CLI_HELP;
        return 1;
    }

    for (int i = argi; i < argc; i++) {
        if (strcmp(argv[i], "--bin") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for --bin.\n");
                return 0;
            }
            options->program_path = argv[++i];
            program_path_set = 1;
        } else if (strcmp(argv[i], "--smp") == 0 || strcmp(argv[i], "--cores") == 0) {
            if (i + 1 >= argc || !parse_positive_int(argv[i + 1], &options->smp_cores)) {
                fprintf(stderr, "Invalid core count. Expected integer in [1, 64].\n");
                return 0;
            }
            i++;
        } else if (strcmp(argv[i], "--cpu-mhz") == 0) {
            if (i + 1 >= argc || !parse_cpu_mhz(argv[i + 1], &options->cpu_mhz)) {
                fprintf(stderr, "Invalid CPU clock. Expected MHz in [1, %u].\n",
                        VM_MAX_CPU_MHZ);
                return 0;
            }
            i++;
        } else if (strcmp(argv[i], "--engine") == 0) {
            if (i + 1 >= argc ||
                !parse_execution_engine(argv[i + 1], &options->execution_engine)) {
                fprintf(stderr, "Invalid execution engine. Expected classic, cached, threaded, or jit.\n");
                return 0;
            }
            i++;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            options->command = VM_CLI_HELP;
            return 1;
        } else if (strcmp(argv[i], "--selftest") == 0) {
            options->command = VM_CLI_SELFTEST;
        } else if (strcmp(argv[i], "--net") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for --net.\n");
                return 0;
            }
            options->net_mode = argv[++i];
        } else if (argv[i][0] != '-' && !program_path_set && options->command == VM_CLI_RUN) {
            options->program_path = argv[i];
            program_path_set = 1;
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            return 0;
        }
    }

    return 1;
}

static void print_launch_summary(const VmCliOptions *options) {
    VM_RUNTIME_LOG("Launching Lamp VM\n");
    VM_RUNTIME_LOG("  program: %s\n", options->program_path);
    VM_RUNTIME_LOG("  cores:   %d\n", options->smp_cores);
    VM_RUNTIME_LOG("  clock:   %u MHz nominal\n", options->cpu_mhz);
    VM_RUNTIME_LOG("  engine:  %s\n", execution_engine_name(options->execution_engine));
    VM_RUNTIME_LOG("  console: host serial stdin/stdout\n");
    VM_RUNTIME_LOG("  network: %s\n", options->net_mode);
}

static void init_ethernet_backend_from_cli(VM *vm, const char *net_mode) {
    ether_backend_t backend;
    int backend_ok = 0;

    if (strcmp(net_mode, "nat") == 0) {
        backend_ok = (ether_backend_nat_create(&backend) == 0);
    } else if (strcmp(net_mode, "null") == 0) {
        backend_ok = (ether_backend_null_create(&backend) == 0);
    } else if (strncmp(net_mode, "udp:", 4) == 0) {
        int bind_p = 9000;
        int peer_p = 9001;
        if (sscanf(net_mode + 4, "%d:%d", &bind_p, &peer_p) == 2 &&
            bind_p > 0 && bind_p <= 65535 && peer_p > 0 && peer_p <= 65535) {
            backend_ok = (ether_backend_udp_create(&backend, (uint16_t)bind_p, (uint16_t)peer_p) == 0);
        } else {
            fprintf(stderr, "Invalid --net udp mode, expected udp:<bind-port>:<peer-port>\n");
        }
    } else {
        fprintf(stderr, "Unknown --net mode '%s'; falling back to null backend.\n", net_mode);
    }

    if (!backend_ok) {
        ether_backend_null_create(&backend);
    }
    ether_init(vm, &backend);
}

static void vm_host_signal_dump_state(int sig) {
    (void)sig;
    extern VM *g_host_dump_vm;
    VM *vm = g_host_dump_vm;
    if (!vm) {
        return;
    }
    fprintf(stderr, "\n[host-dump] vCPU state:\n");
    for (int i = 0; i < vm->smp_cores; i++) {
        VCPU *cpu = &vm->cpus[i];
        fprintf(stderr,
                "[host-dump] core=%d ip=0x%08zx last_ip=0x%08zx flags=0x%x "
                "csp=%u dsp=%u isp=%u r30=0x%08x r31=0x%08x r0=0x%08x r1=0x%08x r2=0x%08x\n",
                i, cpu->ip, cpu->last_ip, (unsigned)cpu->flags,
                (unsigned)cpu->csp, (unsigned)cpu->dsp, (unsigned)cpu->isp,
                (unsigned)cpu->regs[30], (unsigned)cpu->regs[31],
                (unsigned)cpu->regs[0], (unsigned)cpu->regs[1],
                (unsigned)cpu->regs[2]);
    }
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGUSR1, vm_host_signal_dump_state);
    VM_RUNTIME_LOG("Lamp VM version 1.0.0-rc1\n");
#ifdef DEBUG_BUILD
    VM_RUNTIME_LOG("This copy was built with debug flags and may be slower.\n");
#endif
    VmCliOptions options;
    if (!parse_cli_options(argc, argv, &options)) {
        print_usage(argv[0]);
        return 1;
    }
    if (options.command == VM_CLI_HELP) {
        print_usage(argv[0]);
        return 0;
    }
    if (options.command == VM_CLI_SELFTEST) {
        return run_selftests();
    }
    if (options.command == VM_CLI_BENCHMARK) {
        return run_benchmark();
    }
    print_launch_summary(&options);

    size_t program_size = 0;
    size_t data_size = 0;
    uint64_t *program = NULL;
    uint8_t *data = NULL;
    ProgramLayout layout;

    if (!load_program_single(options.program_path, &program, &program_size, &data, &data_size, &layout)) {
        fprintf(stderr, "Failed to load program from %s\n", options.program_path);
        return 1;
    }

    VM_RUNTIME_LOG("Loaded program from %s, %zu instructions.\n", options.program_path, program_size);
    VM_RUNTIME_LOG("Loaded data: %zu bytes.\n", data_size);
    VM_RUNTIME_LOG("Layout: TEXT_BASE=0x%08X TEXT_SIZE=%u DATA_BASE=0x%08X DATA_SIZE=%u BSS_BASE=0x%08X BSS_SIZE=%u\n",
                   layout.text_base, layout.text_size,
                   layout.data_base, layout.data_size,
                   layout.bss_base, layout.bss_size);

    VM *vm = vm_create(MEM_SIZE, program, program_size, data, data_size, &layout, options.smp_cores);
    if (!vm) {
        fprintf(stderr, "Failed to create VM.\n");
        free(program);
        free(data);
        return 1;
    }
    vm->cpu_frequency_hz = (uint64_t)options.cpu_mhz * 1000000ull;
    vm_engine_set(vm, options.execution_engine);
    disk_init(vm, "./disk.img");

    init_ethernet_backend_from_cli(vm, options.net_mode);

    init_ivt(vm);
    if (options.smp_cores > 1) {
        VM_RUNTIME_LOG("SMP mode enabled: %d cores.\n", options.smp_cores);
    }
    VM_RUNTIME_LOG("Loaded VM: call stack=%d data stack=%d memory=%lu head=%p\n",
                   CALL_STACK_SIZE, DATA_STACK_SIZE, MEM_SIZE, (void *)vm->memory);
    init_screen();
    VM_RUNTIME_LOG("VNC preview server enabled.\n");
    vnc_run(vm);
    (void)audio_host_start(vm);
    VM_RUNTIME_LOG("Host serial stdin/stdout enabled.\n");
    vm_run_serial(vm);
#ifdef DBEUG
    vm_dump(vm, 1024);
#endif

    uint64_t total_execution_times = 0;
    for (int i = 0; i < vm->smp_cores; i++) {
        total_execution_times += atomic_load_explicit(&vm->cpus[i].execution_times, memory_order_relaxed);
    }
    vnc_exit();
    audio_host_stop(vm);
    ether_shutdown(vm);
    VM_RUNTIME_LOG("Execution complete in %lu cycles.\n",
                   (unsigned long)total_execution_times);
    vm_debug_print_stats(vm);
    vm_destroy(vm);
    free(program);
    free(data);
    return 0;
}

#endif /* LAMPVM_EMBEDDED */
