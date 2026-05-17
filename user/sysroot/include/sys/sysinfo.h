#ifndef LAMP_LIBC_SYS_SYSINFO_H
#define LAMP_LIBC_SYS_SYSINFO_H

struct sysinfo { long uptime; unsigned long loads[3]; unsigned long totalram; unsigned long freeram; };
int sysinfo(struct sysinfo *info);

#endif
