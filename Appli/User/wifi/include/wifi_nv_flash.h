#ifndef WIFI_NV_FLASH_H
#define WIFI_NV_FLASH_H

#include <stddef.h>
#include <stdint.h>

#include "wifi_nv_layout.h"
#include "wpm_nv_journal.h"

#ifdef __cplusplus
extern "C" {
#endif

const wpm_nv_journal_io_t *wifi_nv_flash_io(void);
const wpm_nv_journal_geometry_t *wifi_nv_flash_geometry(void);

/* Implemented beside the BusFault handler; reads only the reserved NV range. */
int work_wifi_nv_guarded_flash_read(uint32_t address, uint8_t *data,
                                    size_t length);

#ifdef __cplusplus
}
#endif

#endif
