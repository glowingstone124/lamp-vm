#ifndef LAMP_LIBC_GRP_H
#define LAMP_LIBC_GRP_H

#include <sys/types.h>

struct group {
    char *gr_name;
    char *gr_passwd;
    gid_t gr_gid;
    char **gr_mem;
};

struct group *getgrgid(gid_t gid);
struct group *getgrnam(const char *name);
int initgroups(const char *user, gid_t group);
void endgrent(void);

#endif
