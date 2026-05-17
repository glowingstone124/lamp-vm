#ifndef LAMP_LIBC_SYS_SYSMACROS_H
#define LAMP_LIBC_SYS_SYSMACROS_H

#define major(dev) (((dev) >> 8) & 0xff)
#define minor(dev) ((dev) & 0xff)
#define makedev(ma, mi) ((((ma) & 0xff) << 8) | ((mi) & 0xff))

#endif
