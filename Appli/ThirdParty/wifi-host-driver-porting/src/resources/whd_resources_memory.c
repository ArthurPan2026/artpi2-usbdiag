/*
 * Project-local resource provider for Infineon WHD WiFi5.
 *
 * The firmware and CLM objects are the official CYW43438 resources copied
 * into this project.  The NVRAM image is the board-specific AW-CU427-P image
 * and is padded here so the last SDIO block is always safe to read.
 */
#include <stdint.h>
#include <string.h>

#include "whd_resource_api.h"

#include "clm_resources.h"
#include "resources.h"
#include "wifi_nvram_image.h"

#define WHD_MEMORY_RESOURCE_BLOCK_SIZE 1024U

static uint8_t s_nvram_image[((sizeof(wifi_nvram_image) + 3U) & ~3U)];
static uint8_t s_nvram_ready;

static void whd_memory_prepare_nvram(void)
{
    uint32_t index;

    if (s_nvram_ready != 0U)
    {
        return;
    }

    memset(s_nvram_image, 0, sizeof(s_nvram_image));
    memcpy(s_nvram_image, wifi_nvram_image, sizeof(wifi_nvram_image));
    /* WHD expects the text image to be NUL terminated rather than newline
     * terminated.  The generated image normally already uses NULs, but keep
     * the conversion for compatibility with other board NVRAM generators. */
    for (index = 0U; index < sizeof(s_nvram_image); index++)
    {
        if (s_nvram_image[index] == '\n')
        {
            s_nvram_image[index] = '\0';
        }
    }
    s_nvram_ready = 1U;
}

static uint32_t whd_memory_resource_data(whd_resource_type_t type,
                                         const uint8_t **data)
{
    if (data == NULL)
    {
        return 0U;
    }

    switch (type)
    {
    case WHD_RESOURCE_WLAN_FIRMWARE:
        *data = wifi_firmware_image_data;
        return (uint32_t)sizeof(wifi_firmware_image_data);

    case WHD_RESOURCE_WLAN_NVRAM:
        whd_memory_prepare_nvram();
        *data = s_nvram_image;
        return (uint32_t)sizeof(s_nvram_image);

    case WHD_RESOURCE_WLAN_CLM:
        *data = wifi_firmware_clm_blob_data;
        return (uint32_t)sizeof(wifi_firmware_clm_blob_data);

    default:
        *data = NULL;
        return 0U;
    }
}

static whd_result_t whd_memory_resource_size(whd_driver_t whd_drv,
                                              whd_resource_type_t type,
                                              uint32_t *size_out)
{
    const uint8_t *data;

    (void)whd_drv;
    if (size_out == NULL)
    {
        return WHD_BADARG;
    }
    *size_out = whd_memory_resource_data(type, &data);
    return *size_out != 0U ? WHD_SUCCESS : WHD_BADARG;
}

static whd_result_t whd_memory_resource_block_size(whd_driver_t whd_drv,
                                                   whd_resource_type_t type,
                                                   uint32_t *size_out)
{
    (void)whd_drv;
    (void)type;
    if (size_out == NULL)
    {
        return WHD_BADARG;
    }
    *size_out = WHD_MEMORY_RESOURCE_BLOCK_SIZE;
    return WHD_SUCCESS;
}

static whd_result_t whd_memory_resource_block_count(whd_driver_t whd_drv,
                                                    whd_resource_type_t type,
                                                    uint32_t *count_out)
{
    uint32_t size;
    whd_result_t result;

    if (count_out == NULL)
    {
        return WHD_BADARG;
    }
    result = whd_memory_resource_size(whd_drv, type, &size);
    if (result != WHD_SUCCESS)
    {
        return result;
    }
    *count_out = (size + WHD_MEMORY_RESOURCE_BLOCK_SIZE - 1U) /
                 WHD_MEMORY_RESOURCE_BLOCK_SIZE;
    return WHD_SUCCESS;
}

static whd_result_t whd_memory_resource_block(whd_driver_t whd_drv,
                                              whd_resource_type_t type,
                                              uint32_t blockno,
                                              const uint8_t **data,
                                              uint32_t *size_out)
{
    const uint8_t *resource;
    uint32_t resource_size;
    uint32_t offset;
    uint32_t count;

    (void)whd_drv;
    if (data == NULL || size_out == NULL)
    {
        return WHD_BADARG;
    }
    resource_size = whd_memory_resource_data(type, &resource);
    count = (resource_size + WHD_MEMORY_RESOURCE_BLOCK_SIZE - 1U) /
            WHD_MEMORY_RESOURCE_BLOCK_SIZE;
    if (resource == NULL || blockno >= count)
    {
        return WHD_BADARG;
    }
    offset = blockno * WHD_MEMORY_RESOURCE_BLOCK_SIZE;
    *data = resource + offset;
    *size_out = resource_size - offset;
    if (*size_out > WHD_MEMORY_RESOURCE_BLOCK_SIZE)
    {
        *size_out = WHD_MEMORY_RESOURCE_BLOCK_SIZE;
    }
    return WHD_SUCCESS;
}

static whd_result_t whd_memory_resource_read(whd_driver_t whd_drv,
                                             whd_resource_type_t type,
                                             uint32_t offset, uint32_t size,
                                             uint32_t *size_out, void *buffer)
{
    const uint8_t *resource;
    uint32_t resource_size;
    uint32_t length;

    (void)whd_drv;
    if (size_out == NULL || buffer == NULL)
    {
        return WHD_BADARG;
    }
    resource_size = whd_memory_resource_data(type, &resource);
    if (resource == NULL || offset > resource_size)
    {
        *size_out = 0U;
        return WHD_BADARG;
    }
    length = resource_size - offset;
    if (length > size)
    {
        length = size;
    }
    memcpy(buffer, resource + offset, length);
    *size_out = length;
    return WHD_SUCCESS;
}

whd_resource_source_t resource_ops =
{
    .whd_resource_size = whd_memory_resource_size,
    .whd_get_resource_block_size = whd_memory_resource_block_size,
    .whd_get_resource_no_of_blocks = whd_memory_resource_block_count,
    .whd_get_resource_block = whd_memory_resource_block,
    .whd_resource_read = whd_memory_resource_read
};
