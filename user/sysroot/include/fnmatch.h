#ifndef LAMP_LIBC_FNMATCH_H
#define LAMP_LIBC_FNMATCH_H

#define FNM_NOMATCH 1
int fnmatch(const char *pattern, const char *string, int flags);

#endif
#define FNM_PATHNAME 0x01
