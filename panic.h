//
// Created by Max Wang on 2025/12/29.
//

#ifndef VM_PANIC_H
#define VM_PANIC_H
const char* panic_format(const char* fmt, ...);
void panic(const char* input, VM* vm);
#endif //VM_PANIC_H