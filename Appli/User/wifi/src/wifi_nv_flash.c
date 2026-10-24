/* SPDX-License-Identifier: Apache-2.0 */
#include "wifi_nv_flash.h"

#include <string.h>

#include "stm32h7rsxx_hal.h"

#define WIFI_NV_FLASH_END_ADDRESS \
    (WIFI_NV_FLASH_BASE_ADDRESS + \
     (WIFI_NV_FLASH_SECTOR_SIZE * WIFI_NV_FLASH_SECTOR_COUNT))

static int wifi_nv_flash_address(uint32_t offset, size_t length,
                                 uint32_t *address)
{
    uint32_t region_size = WIFI_NV_FLASH_SECTOR_SIZE *
                           WIFI_NV_FLASH_SECTOR_COUNT;

    if (address == NULL || length == 0U ||
        (offset & (WPM_NV_JOURNAL_PROGRAM_SIZE - 1U)) != 0U ||
        (length & (WPM_NV_JOURNAL_PROGRAM_SIZE - 1U)) != 0U ||
        offset > region_size || length > (size_t)(region_size - offset))
    {
        return -1;
    }
    *address = WIFI_NV_FLASH_BASE_ADDRESS + offset;
    return 0;
}

static int wifi_nv_flash_read(void *context, uint32_t offset, uint8_t *data,
                              size_t length)
{
    uint32_t address;
    uint32_t first;
    uint32_t end;

    (void)context;
    if (data == NULL || wifi_nv_flash_address(offset, length, &address) != 0)
    {
        return -1;
    }

    first = address & ~31U;
    end = (uint32_t)(address + length + 31U) & ~31U;
    __DSB();
    SCB_InvalidateDCache_by_Addr((void *)(uintptr_t)first,
                                 (int32_t)(end - first));
    __DSB();
    __ISB();
    return work_wifi_nv_guarded_flash_read(address, data, length);
}

static int wifi_nv_flash_program16(
    void *context, uint32_t offset,
    const uint8_t data[WPM_NV_JOURNAL_PROGRAM_SIZE])
{
    uint32_t source[4];
    uint32_t address;
    HAL_StatusTypeDef status;

    (void)context;
    if (data == NULL ||
        wifi_nv_flash_address(offset, WPM_NV_JOURNAL_PROGRAM_SIZE,
                              &address) != 0)
    {
        return -1;
    }

    memcpy(source, data, sizeof(source));
    if (HAL_FLASH_Unlock() != HAL_OK)
    {
        return -1;
    }
    status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_QUADWORD, address,
                               (uint32_t)(uintptr_t)source);
    (void)HAL_FLASH_Lock();
    return status == HAL_OK ? 0 : -1;
}

static int wifi_nv_flash_erase_sector(void *context, uint32_t sector_index)
{
    FLASH_EraseInitTypeDef erase;
    uint32_t sector_error = 0xFFFFFFFFUL;
    uint32_t address;
    HAL_StatusTypeDef status;

    (void)context;
    if (sector_index >= WIFI_NV_FLASH_SECTOR_COUNT)
    {
        return -1;
    }
    address = WIFI_NV_FLASH_BASE_ADDRESS +
              (sector_index * WIFI_NV_FLASH_SECTOR_SIZE);
    if (address < WIFI_NV_FLASH_BASE_ADDRESS ||
        address >= WIFI_NV_FLASH_END_ADDRESS)
    {
        return -1;
    }

    erase.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase.Sector = WIFI_NV_FLASH_FIRST_SECTOR + sector_index;
    erase.NbSectors = 1U;

    if (HAL_FLASH_Unlock() != HAL_OK)
    {
        return -1;
    }
    status = HAL_FLASHEx_Erase(&erase, &sector_error);
    (void)HAL_FLASH_Lock();
    return status == HAL_OK && sector_error == 0xFFFFFFFFUL ? 0 : -1;
}

const wpm_nv_journal_io_t *wifi_nv_flash_io(void)
{
    static const wpm_nv_journal_io_t io =
    {
        NULL,
        wifi_nv_flash_read,
        wifi_nv_flash_program16,
        wifi_nv_flash_erase_sector
    };
    return &io;
}

const wpm_nv_journal_geometry_t *wifi_nv_flash_geometry(void)
{
    static const wpm_nv_journal_geometry_t geometry =
    {
        WIFI_NV_FLASH_SECTOR_SIZE,
        WPM_NV_JOURNAL_MAX_PAYLOAD,
        WIFI_NV_FLASH_SECTOR_COUNT,
        WPM_NV_JOURNAL_PROGRAM_SIZE,
        0xFFU
    };
    return &geometry;
}
