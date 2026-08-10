#include "codegen.h"

const VmJitBackend *vm_jit_codegen_host_backend(void) {
#if defined(__aarch64__)
    return vm_jit_arm64_backend();
#elif defined(__x86_64__)
    return vm_jit_x86_64_backend();
#else
    return NULL;
#endif
}
