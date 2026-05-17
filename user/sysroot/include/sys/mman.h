#ifndef LAMP_LIBC_SYS_MMAN_H
#define LAMP_LIBC_SYS_MMAN_H

#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED ((void *)-1)

void *mmap(void *addr, unsigned long len, int prot, int flags, int fd, long off);
int munmap(void *addr, unsigned long len);

#endif
#define MAP_FAILED ((void *)-1)
#define MAP_ANONYMOUS 0x20
