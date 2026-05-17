#ifndef LAMP_LIBC_REGEX_H
#define LAMP_LIBC_REGEX_H

typedef struct { int dummy; } regex_t;
typedef int regmatch_t;
#define REG_EXTENDED 1
#define REG_NOMATCH 1
int regcomp(regex_t *preg, const char *regex, int cflags);
int regexec(const regex_t *preg, const char *string, unsigned long nmatch, regmatch_t pmatch[], int eflags);
void regfree(regex_t *preg);

#endif
