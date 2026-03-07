#ifndef LAMP_KERNEL_SMP_H
#define LAMP_KERNEL_SMP_H

#include "types.h"

void smp_init_bsp(void);
void smp_init_ap(void);
void smp_start_aps(void);
uint32_t smp_online_cpus(void);
uint32_t smp_target_cpus(void);
uint32_t smp_cpu_id(void);

#endif
