#ifndef LAMP_LIBC_STDIO_H
#define LAMP_LIBC_STDIO_H

#include <stddef.h>
#include <stdarg.h>
#include <sys/types.h>

typedef struct FILE FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

#define EOF (-1)
#define BUFSIZ 4096

int printf(const char *fmt, ...);
int fprintf(FILE *stream, const char *fmt, ...);
int dprintf(int fd, const char *fmt, ...);
int sprintf(char *str, const char *fmt, ...);
int sscanf(const char *str, const char *fmt, ...);
int snprintf(char *str, size_t size, const char *fmt, ...);
int vsnprintf(char *str, size_t size, const char *fmt, va_list ap);
int vasprintf(char **strp, const char *fmt, va_list ap);
int vfprintf(FILE *stream, const char *fmt, va_list ap);
int fputs(const char *s, FILE *stream);
int fputs_unlocked(const char *s, FILE *stream);
int puts(const char *s);
int fputc(int c, FILE *stream);
int putc(int c, FILE *stream);
int putc_unlocked(int c, FILE *stream);
int putchar(int c);
int putchar_unlocked(int c);
int fgetc(FILE *stream);
int getc(FILE *stream);
int getc_unlocked(FILE *stream);
int getchar(void);
int getchar_unlocked(void);
int fflush(FILE *stream);
FILE *fopen(const char *path, const char *mode);
FILE *fdopen(int fd, const char *mode);
FILE *freopen(const char *path, const char *mode, FILE *stream);
int fclose(FILE *stream);
int fileno(FILE *stream);
int fileno_unlocked(FILE *stream);
int fseeko(FILE *stream, off_t offset, int whence);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
char *fgets(char *s, int size, FILE *stream);
char *fgets_unlocked(char *s, int size, FILE *stream);
ssize_t getline(char **lineptr, size_t *n, FILE *stream);
int feof(FILE *stream);
int feof_unlocked(FILE *stream);
int ferror(FILE *stream);
int ferror_unlocked(FILE *stream);
void clearerr(FILE *stream);
void perror(const char *s);

#endif
