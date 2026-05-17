#ifndef LAMP_LIBC_SYS_IOCTL_H
#define LAMP_LIBC_SYS_IOCTL_H

#include <lamp/abi.h>

#define TCGETS LAMP_IOCTL_TCGETS
#define TCSETS LAMP_IOCTL_TCSETS
#define TCSETSW LAMP_IOCTL_TCSETSW
#define TCSETSF LAMP_IOCTL_TCSETSF
#define TIOCGWINSZ LAMP_IOCTL_TIOCGWINSZ

int ioctl(int fd, unsigned long request, ...);

#endif
