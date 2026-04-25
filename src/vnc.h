//
// Created by Max Wang on 2026/4/25.
//

#ifndef VM_VNC_H
#define VM_VNC_H
#include "vm.h"
#define VNC_PORT 5900
void vnc_run(VM* vm);

void vnc_exit(void);
#endif //VM_VNC_H
