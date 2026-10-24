#ifndef BOOT_VECTOR_VALIDATION_H
#define BOOT_VECTOR_VALIDATION_H

#include <stdint.h>

/* Valid internal RAM stack ranges on the ART-Pi2 STM32H7R7 configuration.
 * DTCM is 64 KiB; the contiguous AXI SRAM banks provide 456 KiB from
 * 0x24000000 through 0x24071FFF. */
#define BOOT_APPLICATION_DTCM_BEGIN       0x20000000U
#define BOOT_APPLICATION_DTCM_END         0x20010000U
#define BOOT_APPLICATION_AXI_SRAM_BEGIN   0x24000000U
#define BOOT_APPLICATION_AXI_SRAM_END     0x24072000U

static inline int boot_application_msp_is_valid(uint32_t msp)
{
    return ((msp > BOOT_APPLICATION_DTCM_BEGIN &&
             msp <= BOOT_APPLICATION_DTCM_END) ||
            (msp > BOOT_APPLICATION_AXI_SRAM_BEGIN &&
             msp <= BOOT_APPLICATION_AXI_SRAM_END)) ? 1 : 0;
}

#endif /* BOOT_VECTOR_VALIDATION_H */
