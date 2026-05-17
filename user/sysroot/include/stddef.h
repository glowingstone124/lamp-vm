#ifndef LAMP_LIBC_STDDEF_H
#define LAMP_LIBC_STDDEF_H

#ifndef _LAMP_SIZE_T_DEFINED
#define _LAMP_SIZE_T_DEFINED
typedef __SIZE_TYPE__ size_t;
#endif
#ifndef _LAMP_PTRDIFF_T_DEFINED
#define _LAMP_PTRDIFF_T_DEFINED
typedef __PTRDIFF_TYPE__ ptrdiff_t;
#endif
typedef __WCHAR_TYPE__ wchar_t;

#ifndef NULL
#define NULL ((void *)0)
#endif

#define offsetof(type, member) __builtin_offsetof(type, member)

#endif
