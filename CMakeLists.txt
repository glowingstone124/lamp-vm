cmake_minimum_required(VERSION 3.20)
project(lampvm C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# ------------------------------------------------
# Do not support MSVC
# ------------------------------------------------
if(MSVC)
    message(FATAL_ERROR "MSVC is not supported. Please use Clang or GCC.")
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug
        RelWithDebInfo
        Release
        MinSizeRel
)

option(VM_DEBUG "Enable VM debug features" OFF)
option(VM_ASAN "Enable AddressSanitizer" OFF)
option(VM_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(VM_LTO "Enable LTO/IPO in Release builds" ON)
option(VM_PGO_GENERATE "Build with PGO profile generation" OFF)
option(VM_PGO_USE "Build with PGO profile use" OFF)
option(VM_STATIC "Build a fully static Linux executable" OFF)

if(VM_PGO_GENERATE AND VM_PGO_USE)
    message(FATAL_ERROR "VM_PGO_GENERATE and VM_PGO_USE cannot both be ON")
endif()

if(VM_STATIC AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "VM_STATIC is supported only for Linux builds")
endif()

if(VM_STATIC AND (VM_ASAN OR VM_UBSAN))
    message(FATAL_ERROR
            "VM_STATIC cannot be combined with VM_ASAN or VM_UBSAN; "
            "use a dynamically linked sanitizer build")
endif()

set(SOURCES
        src/vm.c
        src/engines/engine.c
        src/engines/classic.c
        src/engines/cached.c
        src/engines/threaded.c
        src/engines/interpreter_core.c
        src/engines/jit/jit.c
        src/engines/jit/codegen.c
        src/engines/jit/code_memory.c
        src/engines/jit/runtime.c
        src/engines/jit/arm64/codegen_arm64.c
        src/engines/jit/arm64/memory_arm64.c
        src/engines/jit/x86_64/codegen_x86_64.c
        src/engines/jit/x86_64/memory_x86_64.c
        src/runtime_stats.c
        src/debug.c
        src/io_devices/frame/frame.c
        src/io.c
        src/loadbin.c
        src/panic.c
        src/io_devices/disk/disk.c
        src/interrupt.c
        src/io_devices/frame/terminalin.c
        src/memory.c
        src/mmio.c
        src/io_devices/vga_display/vga_mmio_register.c
        src/io_devices/gpu/gpu.c
        src/io_devices/dma/dma_ring.c
        src/io_devices/audio/audio.c
        src/io_devices/time/time_mmio_register.c
        src/io_devices/intc/intc_mmio_register.c
        src/io_devices/iommu/iommu_mmio_register.c
        src/io_devices/mmu/mmu_mmio_register.c
        src/io_devices/sysinfo/sysinfo_mmio_register.c
        src/io_devices/ether/ether.c
        src/io_devices/ether/ether_null.c
        src/io_devices/ether/ether_udp.c
        src/io_devices/ether/ether_nat.c
        src/io_devices/pcie/pcie.c
        src/io_devices/time/timer.c
        src/float.c
        src/selftest.c
        src/vnc.c
        src/vnc.h
)

add_executable(lampvm ${SOURCES})

if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)
endif()

find_package(SDL3 CONFIG QUIET)
if(VM_STATIC)
    if(SDL3_FOUND AND TARGET SDL3::SDL3-static)
        target_link_libraries(lampvm PRIVATE SDL3::SDL3-static)
    else()
        # Some Linux SDL3 packages install libSDL3.a and sdl3.pc without the
        # SDL3::SDL3-static CMake target. In that case use pkg-config's static
        # dependency closure, but link the SDL archive by absolute path so a
        # shared libSDL3 cannot be selected accidentally.
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(SDL3_PC REQUIRED sdl3)

        set(_vm_saved_library_suffixes ${CMAKE_FIND_LIBRARY_SUFFIXES})
        set(CMAKE_FIND_LIBRARY_SUFFIXES ${CMAKE_STATIC_LIBRARY_SUFFIX})
        unset(VM_SDL3_STATIC_ARCHIVE CACHE)
        find_library(VM_SDL3_STATIC_ARCHIVE
                NAMES SDL3
                HINTS
                    ${SDL3_PC_STATIC_LIBRARY_DIRS}
                    ${SDL3_PC_LIBRARY_DIRS}
        )
        set(CMAKE_FIND_LIBRARY_SUFFIXES ${_vm_saved_library_suffixes})
        unset(_vm_saved_library_suffixes)

        if(NOT VM_SDL3_STATIC_ARCHIVE)
            message(FATAL_ERROR
                    "VM_STATIC requires a static SDL3 archive (libSDL3.a). "
                    "Build/install SDL3 with SDL_STATIC=ON, or provide its "
                    "prefix through CMAKE_PREFIX_PATH or PKG_CONFIG_PATH.")
        endif()

        set(VM_SDL3_STATIC_PRIVATE_LIBS ${SDL3_PC_STATIC_LIBRARIES})
        list(REMOVE_ITEM VM_SDL3_STATIC_PRIVATE_LIBS SDL3)
        target_include_directories(lampvm PRIVATE ${SDL3_PC_INCLUDE_DIRS})
        target_link_directories(lampvm PRIVATE
                ${SDL3_PC_STATIC_LIBRARY_DIRS}
                ${SDL3_PC_LIBRARY_DIRS}
        )
        target_link_libraries(lampvm PRIVATE
                ${VM_SDL3_STATIC_ARCHIVE}
                ${VM_SDL3_STATIC_PRIVATE_LIBS}
        )
        target_compile_options(lampvm PRIVATE ${SDL3_PC_STATIC_CFLAGS_OTHER})
        target_link_options(lampvm PRIVATE ${SDL3_PC_STATIC_LDFLAGS_OTHER})
    endif()
else()
    if(SDL3_FOUND)
        if(TARGET SDL3::SDL3)
            target_link_libraries(lampvm PRIVATE SDL3::SDL3)
        else()
            if(DEFINED SDL3_INCLUDE_DIRS)
                target_include_directories(lampvm PRIVATE ${SDL3_INCLUDE_DIRS})
            endif()
            if(DEFINED SDL3_LIBRARIES)
                target_link_libraries(lampvm PRIVATE ${SDL3_LIBRARIES})
            endif()
        endif()
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(SDL3 REQUIRED sdl3)
        target_include_directories(lampvm PRIVATE ${SDL3_INCLUDE_DIRS})
        target_link_directories(lampvm PRIVATE ${SDL3_LIBRARY_DIRS})
        target_link_libraries(lampvm PRIVATE ${SDL3_LIBRARIES})
        target_compile_options(lampvm PRIVATE ${SDL3_CFLAGS_OTHER})
    endif()
endif()

find_package(Threads REQUIRED)
target_link_libraries(lampvm PRIVATE Threads::Threads)

if(UNIX AND NOT APPLE)
    target_link_libraries(lampvm PRIVATE m)
    # Keep POSIX time APIs and the BSD-compatible IP/TCP declarations used by
    # the user-mode NAT backend visible under glibc.
    target_compile_definitions(lampvm PRIVATE
            _POSIX_C_SOURCE=200809L
            _DEFAULT_SOURCE
    )
endif()

if(VM_STATIC)
    target_compile_definitions(lampvm PRIVATE VM_STATIC_BUILD=1)
    target_link_options(lampvm PRIVATE -static)
    set_target_properties(lampvm PROPERTIES
            LINK_SEARCH_START_STATIC TRUE
            LINK_SEARCH_END_STATIC TRUE
    )
    message(STATUS "VM_STATIC enabled: producing a fully static Linux executable")
endif()

target_compile_options(lampvm PRIVATE
        -fno-omit-frame-pointer
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wstrict-prototypes
        -Wmissing-prototypes
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(lampvm PRIVATE DEBUG_BUILD)
    target_compile_options(lampvm PRIVATE -g3 -Og)
elseif(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    target_compile_definitions(lampvm PRIVATE RELEASE_BUILD)
    target_compile_options(lampvm PRIVATE -g -O2)
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_definitions(lampvm PRIVATE RELEASE_BUILD)
    target_compile_options(lampvm PRIVATE -O3 -fomit-frame-pointer)
elseif(CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
    target_compile_definitions(lampvm PRIVATE RELEASE_BUILD)
    target_compile_options(lampvm PRIVATE -Os)
else()
    target_compile_definitions(lampvm PRIVATE RELEASE_BUILD)
    target_compile_options(lampvm PRIVATE -g -O2)
endif()

if(VM_DEBUG)
    target_compile_definitions(lampvm PRIVATE VM_DEBUG VM_MEMCHECK VM_INSTR_STATS)
endif()

if(VM_ASAN)
    target_compile_options(lampvm PRIVATE -fsanitize=address)
    target_link_options(lampvm PRIVATE -fsanitize=address)
endif()

if(VM_UBSAN)
    target_compile_options(lampvm PRIVATE -fsanitize=undefined)
    target_link_options(lampvm PRIVATE -fsanitize=undefined)
endif()

if(VM_PGO_GENERATE)
    target_compile_options(lampvm PRIVATE -fprofile-generate)
    target_link_options(lampvm PRIVATE -fprofile-generate)
endif()

if(VM_PGO_USE)
    target_compile_options(lampvm PRIVATE -fprofile-use -fprofile-correction)
    target_link_options(lampvm PRIVATE -fprofile-use)
endif()

include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
if(CMAKE_BUILD_TYPE STREQUAL "Release" AND VM_LTO)
    if(ipo_supported)
        set_property(TARGET lampvm PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    else()
        message(WARNING "IPO/LTO not supported: ${ipo_error}")
    endif()
endif()
