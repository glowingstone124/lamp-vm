#ifndef LAMP_LIBC_ASSERT_H
#define LAMP_LIBC_ASSERT_H

#ifdef NDEBUG
#define assert(expr) ((void)0)
#else
void _exit(int status) __attribute__((noreturn));
#define assert(expr) ((expr) ? (void)0 : _exit(1))
#endif

#endif
