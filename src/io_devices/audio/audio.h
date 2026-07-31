#ifndef VM_AUDIO_H
#define VM_AUDIO_H

#include "../../vm.h"

int audio_device_init(VM *vm);
void audio_device_shutdown(VM *vm);
int audio_host_start(VM *vm);
void audio_host_stop(VM *vm);
void audio_poll(VM *vm);

#endif /* VM_AUDIO_H */
