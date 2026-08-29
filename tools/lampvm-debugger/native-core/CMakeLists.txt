cmake_minimum_required(VERSION 3.20)
project(lampvm_debug_core C)

if(NOT DEFINED LAMPVM_ROOT)
    message(FATAL_ERROR "LAMPVM_ROOT must point to the Lamp VM source tree")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(LAMPVM_SOURCES
    ${LAMPVM_ROOT}/src/vm.c
    ${LAMPVM_ROOT}/src/debug_api.c
    ${LAMPVM_ROOT}/src/engines/engine.c
    ${LAMPVM_ROOT}/src/engines/classic.c
    ${LAMPVM_ROOT}/src/engines/cached.c
    ${LAMPVM_ROOT}/src/engines/threaded.c
    ${LAMPVM_ROOT}/src/engines/interpreter_core.c
    ${LAMPVM_ROOT}/src/engines/jit/jit.c
    ${LAMPVM_ROOT}/src/engines/jit/codegen.c
    ${LAMPVM_ROOT}/src/engines/jit/code_memory.c
    ${LAMPVM_ROOT}/src/engines/jit/runtime.c
    ${LAMPVM_ROOT}/src/engines/jit/arm64/codegen_arm64.c
    ${LAMPVM_ROOT}/src/engines/jit/arm64/arm64_emitter.c
    ${LAMPVM_ROOT}/src/engines/jit/arm64/arm64_branch.c
    ${LAMPVM_ROOT}/src/engines/jit/arm64/arm64_ops.c
    ${LAMPVM_ROOT}/src/engines/jit/arm64/memory_arm64.c
    ${LAMPVM_ROOT}/src/engines/jit/x86_64/codegen_x86_64.c
    ${LAMPVM_ROOT}/src/engines/jit/x86_64/memory_x86_64.c
    ${LAMPVM_ROOT}/src/runtime_stats.c
    ${LAMPVM_ROOT}/src/debug.c
    ${LAMPVM_ROOT}/src/io_devices/frame/frame.c
    ${LAMPVM_ROOT}/src/io.c
    ${LAMPVM_ROOT}/src/loadbin.c
    ${LAMPVM_ROOT}/src/panic.c
    ${LAMPVM_ROOT}/src/io_devices/disk/disk.c
    ${LAMPVM_ROOT}/src/interrupt.c
    ${LAMPVM_ROOT}/src/io_devices/frame/terminalin.c
    ${LAMPVM_ROOT}/src/memory.c
    ${LAMPVM_ROOT}/src/mmio.c
    ${LAMPVM_ROOT}/src/io_devices/vga_display/vga_mmio_register.c
    ${LAMPVM_ROOT}/src/io_devices/gpu/gpu.c
    ${LAMPVM_ROOT}/src/io_devices/dma/dma_ring.c
    ${LAMPVM_ROOT}/src/io_devices/audio/audio.c
    ${LAMPVM_ROOT}/src/io_devices/time/time_mmio_register.c
    ${LAMPVM_ROOT}/src/io_devices/intc/intc_mmio_register.c
    ${LAMPVM_ROOT}/src/io_devices/iommu/iommu_mmio_register.c
    ${LAMPVM_ROOT}/src/io_devices/mmu/mmu_mmio_register.c
    ${LAMPVM_ROOT}/src/io_devices/sysinfo/sysinfo_mmio_register.c
    ${LAMPVM_ROOT}/src/io_devices/ether/ether.c
    ${LAMPVM_ROOT}/src/io_devices/ether/ether_null.c
    ${LAMPVM_ROOT}/src/io_devices/ether/ether_udp.c
    ${LAMPVM_ROOT}/src/io_devices/ether/ether_nat.c
    ${LAMPVM_ROOT}/src/io_devices/pcie/pcie.c
    ${LAMPVM_ROOT}/src/io_devices/time/timer.c
    ${LAMPVM_ROOT}/src/float.c
    ${LAMPVM_ROOT}/src/selftest.c
    ${LAMPVM_ROOT}/src/vnc.c
)

add_library(lampvm_core SHARED ${LAMPVM_SOURCES})
target_include_directories(lampvm_core PUBLIC ${LAMPVM_ROOT}/include)
target_include_directories(lampvm_core PRIVATE ${LAMPVM_ROOT}/src)
target_compile_definitions(lampvm_core PRIVATE LAMPVM_DEBUG_CORE_BUILD=1)

if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)
endif()

find_package(SDL3 CONFIG QUIET)
if(SDL3_FOUND)
    if(TARGET SDL3::SDL3)
        target_link_libraries(lampvm_core PRIVATE SDL3::SDL3)
    else()
        if(DEFINED SDL3_INCLUDE_DIRS)
            target_include_directories(lampvm_core PRIVATE ${SDL3_INCLUDE_DIRS})
        endif()
        if(DEFINED SDL3_LIBRARIES)
            target_link_libraries(lampvm_core PRIVATE ${SDL3_LIBRARIES})
        endif()
    endif()
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL3 REQUIRED sdl3)
    target_include_directories(lampvm_core PRIVATE ${SDL3_INCLUDE_DIRS})
    target_link_directories(lampvm_core PRIVATE ${SDL3_LIBRARY_DIRS})
    target_link_libraries(lampvm_core PRIVATE ${SDL3_LIBRARIES})
    target_compile_options(lampvm_core PRIVATE ${SDL3_CFLAGS_OTHER})
endif()

find_package(Threads REQUIRED)
target_link_libraries(lampvm_core PRIVATE Threads::Threads)

if(UNIX AND NOT APPLE)
    target_link_libraries(lampvm_core PRIVATE m)
    target_compile_definitions(lampvm_core PRIVATE
        _POSIX_C_SOURCE=200809L
        _DEFAULT_SOURCE
    )
endif()

target_compile_options(lampvm_core PRIVATE
    -fno-omit-frame-pointer
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wstrict-prototypes
    -Wmissing-prototypes
)
