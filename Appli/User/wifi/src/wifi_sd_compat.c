#include "stm32h7rsxx_hal.h"

#include <rtthread.h>

/* CubeMX generates the SDIO2 Init profile because SDMMC2 is configured as an
 * SDIO peripheral.  Infineon WHD owns the card-identification and CMD52/53
 * transaction sequence, so only the generated Init-field assignments are
 * retained here.  The generated HAL_SDIO_MspInit() remains the single source
 * of truth for the six SDIO pins and peripheral clock and is invoked by the
 * project-local WHD adapter after WL_REG_ON sequencing. */
HAL_StatusTypeDef work_hal_sdio_init_noop(SDIO_HandleTypeDef *hsdio)
{
    RT_UNUSED(hsdio);
    return HAL_OK;
}
