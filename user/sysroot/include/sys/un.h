#ifndef LAMP_LIBC_SYS_UN_H
#define LAMP_LIBC_SYS_UN_H

#include <sys/socket.h>

struct sockaddr_un {
    unsigned short sun_family;
    char sun_path[108];
};

#endif
