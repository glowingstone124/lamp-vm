#ifndef LAMP_KERNEL_PRINTK_H
#define LAMP_KERNEL_PRINTK_H

#include "types.h"

enum {
    KLOG_LEVEL_ERROR = 1u,
    KLOG_LEVEL_WARN = 2u,
    KLOG_LEVEL_INFO = 3u,
    KLOG_LEVEL_DEBUG = 4u
};

void kputc(uint32_t c);
void kputs(const char *s);
void kprintf(const char *s);
void kprint_u32(uint32_t v);
void kprint_hex32(uint32_t v);

uint32_t klog_should_emit(uint32_t level);
void klog_set_level(uint32_t level);
uint32_t klog_get_level(void);
void kio_lock(void);
void kio_unlock(void);
void klog_begin(uint32_t level, const char *tag);
void klog_end(void);
void klog_putc(uint32_t c);
void klog_puts(const char *s);
void klog_hex32(uint32_t v);
void klog_prefix(uint32_t level, const char *tag);
void klog_line(uint32_t level, const char *tag, const char *msg);

#define KLOGE(tag, msg) klog_line(KLOG_LEVEL_ERROR, (tag), (msg))
#define KLOGW(tag, msg) klog_line(KLOG_LEVEL_WARN, (tag), (msg))
#define KLOGI(tag, msg) klog_line(KLOG_LEVEL_INFO, (tag), (msg))
#define KLOGD(tag, msg) klog_line(KLOG_LEVEL_DEBUG, (tag), (msg))

#endif
