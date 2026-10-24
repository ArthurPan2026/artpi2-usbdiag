#ifndef WORK_BOOT_EXTMEM_MANAGER_H
#define WORK_BOOT_EXTMEM_MANAGER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int boot_extmem_init(uint32_t reset_flags);
void boot_jump_to_application(void);

#ifdef __cplusplus
}
#endif

#endif
