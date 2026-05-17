#ifndef LAMP_LIBC_NETDB_H
#define LAMP_LIBC_NETDB_H

#include <sys/socket.h>

struct hostent { char *h_name; char **h_aliases; int h_addrtype; int h_length; char **h_addr_list; };
struct servent { char *s_name; char **s_aliases; int s_port; char *s_proto; };

#define NETDB_INTERNAL (-1)
#define NETDB_SUCCESS 0
#define HOST_NOT_FOUND 1
#define TRY_AGAIN 2
#define NO_RECOVERY 3
#define NO_DATA 4
#define NO_ADDRESS NO_DATA

struct addrinfo {
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    socklen_t ai_addrlen;
    struct sockaddr *ai_addr;
    char *ai_canonname;
    struct addrinfo *ai_next;
};

#define AI_CANONNAME 0x02
#define AI_NUMERICHOST 0x04
#define NI_NUMERICHOST 0x01
#define NI_NUMERICSERV 0x02
#define NI_NAMEREQD 0x04
#define EAI_FAIL (-5)

extern int h_errno;
const char *hstrerror(int err);
struct hostent *gethostbyname(const char *name);
struct servent *getservbyname(const char *name, const char *proto);
struct servent *getservbyport(int port, const char *proto);
int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res);
void freeaddrinfo(struct addrinfo *res);
int getnameinfo(const struct sockaddr *addr, socklen_t addrlen, char *host, socklen_t hostlen, char *serv, socklen_t servlen, int flags);

#endif
