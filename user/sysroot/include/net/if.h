#ifndef LAMP_LIBC_NET_IF_H
#define LAMP_LIBC_NET_IF_H

#include <sys/socket.h>

#define IF_NAMESIZE 16

struct ifreq {
    char ifr_name[IF_NAMESIZE];
    union {
        struct sockaddr ifr_addr;
        struct sockaddr ifr_dstaddr;
        struct sockaddr ifr_broadaddr;
        struct sockaddr ifr_netmask;
        struct sockaddr ifr_hwaddr;
        short ifr_flags;
        int ifr_ifindex;
    } ifr_ifru;
};

#define ifr_addr        ifr_ifru.ifr_addr
#define ifr_dstaddr     ifr_ifru.ifr_dstaddr
#define ifr_broadaddr   ifr_ifru.ifr_broadaddr
#define ifr_netmask     ifr_ifru.ifr_netmask
#define ifr_flags       ifr_ifru.ifr_flags
#define ifr_ifindex     ifr_ifru.ifr_ifindex

unsigned int if_nametoindex(const char *name);
char *if_indextoname(unsigned int ifindex, char *name);

#endif
