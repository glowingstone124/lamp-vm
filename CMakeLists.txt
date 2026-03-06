# ------------------------------------------------
# Do not support MSVC
# ------------------------------------------------
if(MSVC)
    message(FATAL_ERROR "MSVC is not supported. Please use Clang or GCC.")
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

if(VM_PGO_GENERATE AND VM_PGO_USE)
    message(FATAL_ERROR "VM_PGO_GENERATE and VM_PGO_USE cannot both be ON")
endif()

target_compile_options(vm PRIVATE
        -fno-omit-frame-pointer
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wstrict-prototypes
        -Wmissing-prototypes
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(vm PRIVATE DEBUG_BUILD)
    target_compile_options(vm PRIVATE -g3 -Og)

elseif(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    target_compile_definitions(vm PRIVATE RELEASE_BUILD)
    target_compile_options(vm PRIVATE -g -O2)

elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_definitions(vm PRIVATE RELEASE_BUILD)
    target_compile_options(vm PRIVATE -O2)

elseif(CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
    target_compile_definitions(vm PRIVATE RELEASE_BUILD)
    target_compile_options(vm PRIVATE -Os)

else()
    target_compile_definitions(vm PRIVATE RELEASE_BUILD)
    target_compile_options(vm PRIVATE -g -O2)
endif()

if(VM_DEBUG)
    target_compile_definitions(vm PRIVATE VM_DEBUG VM_MEMCHECK VM_INSTR_STATS)
endif()

if(VM_ASAN)
    target_compile_options(vm PRIVATE -fsanitize=address)
    target_link_options(vm PRIVATE -fsanitize=address)
endif()

if(VM_UBSAN)
    target_compile_options(vm PRIVATE -fsanitize=undefined)
    target_link_options(vm PRIVATE -fsanitize=undefined)
endif()

if(VM_PGO_GENERATE)
    target_compile_options(vm PRIVATE -fprofile-generate)
    target_link_options(vm PRIVATE -fprofile-generate)
endif()

if(VM_PGO_USE)
    target_compile_options(vm PRIVATE -fprofile-use -fprofile-correction)
    target_link_options(vm PRIVATE -fprofile-use)
endif()

include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
if(CMAKE_BUILD_TYPE STREQUAL "Release" AND VM_LTO)
    if(ipo_supported)
        set_property(TARGET vm PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    else()
        message(WARNING "IPO/LTO not supported: ${ipo_error}")
    endif()
endif()