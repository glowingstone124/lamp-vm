#ifndef LAMP_LIBC_DIRENT_H
#define LAMP_LIBC_DIRENT_H

#include <lamp/abi.h>

typedef struct DIR DIR;

struct dirent {
    unsigned long d_ino;
    long d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[256];
};

#define DT_UNKNOWN LAMP_DT_UNKNOWN
#define DT_REG LAMP_DT_REG
#define DT_DIR LAMP_DT_DIR
#define DT_CHR LAMP_DT_CHR
#define DT_LNK LAMP_DT_LNK
#define DT_SOCK LAMP_DT_SOCK

DIR *opendir(const char *path);
struct dirent *readdir(DIR *dirp);
int closedir(DIR *dirp);

#endif
