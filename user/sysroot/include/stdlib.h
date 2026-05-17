#ifndef LAMP_LIBC_STDLIB_H
#define LAMP_LIBC_STDLIB_H

#include <stddef.h>

#define alloca(size) __builtin_alloca(size)

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 32767

int rand(void);
void srand(unsigned int seed);

void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void exit(int status) __attribute__((noreturn));
int atoi(const char *s);
long strtol(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);
long long strtoll(const char *nptr, char **endptr, int base);
unsigned long long strtoull(const char *nptr, char **endptr, int base);
double strtod(const char *nptr, char **endptr);
char *getenv(const char *name);
int setenv(const char *name, const char *value, int overwrite);
int putenv(char *string);
int unsetenv(const char *name);
int clearenv(void);
int mkstemp(char *template);
char *realpath(const char *path, char *resolved_path);
void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *));

#endif
