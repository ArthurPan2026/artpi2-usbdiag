/*
 * lv_port_disp.c - LVGL direct framebuffer port for the ART-Pi2 RGBLCD.
 *
 * The LTDC scans one framebuffer while LVGL/NemaGFX renders the other.  The
 * ST LTDC driver switches the completed buffer during vertical blanking, so
 * there is no CPU flush copy and no third framebuffer.  Both buffers live in
 * the non-cacheable PSRAM display arena shared by LTDC and GPU2D.
 */
#include "lv_port_disp.h"

#include <lvgl/lvgl.h>
#include <lvgl/drivers/display/lv_st_ltdc.h>
#include "lvgl_mem_pool.h"
#include "lvgl_h7rs_cache.h"
#include "ltdc.h"

#include <finsh.h>
#include <rtthread.h>
#include <stdint.h>
#include <string.h>

#define LVGL_DISP_W       800U
#define LVGL_DISP_H       480U
#define LVGL_FB_PIXELS    (LVGL_DISP_W * LVGL_DISP_H)
#define LVGL_FB_BYTES     (LVGL_FB_PIXELS * sizeof(uint16_t))

/* Keep the framebuffer objects out of the normal BSS.  The linker places
 * this section at the beginning of DISPLAY_RAM (0x90000000). */
__attribute__((section(".framebuffer"), aligned(64), used))
static uint16_t lvgl_fb_a[LVGL_FB_PIXELS];

__attribute__((section(".framebuffer"), aligned(64), used))
static uint16_t lvgl_fb_b[LVGL_FB_PIXELS];

#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
/* A frozen, single-frame copy supports HTTP inspection without halting the
 * MCU. When networking is not yet provisioned, native SWD can also read this
 * immutable, cache-cleaned copy. It is absent in Release builds. */
static rt_uint8_t s_framebuffer_capture[LVGL_FB_BYTES]
    __attribute__((section(".wifi_web_ram"), aligned(32), used));
static struct rt_mutex s_framebuffer_capture_lock;
static rt_bool_t s_framebuffer_capture_lock_ready;
static rt_bool_t s_framebuffer_capture_valid;
static lvgl_framebuffer_capture_info_t s_framebuffer_capture_info;

static rt_uint32_t framebuffer_capture_crc32(const rt_uint8_t *data,
                                             rt_size_t length)
{
    rt_uint32_t crc = 0xFFFFFFFFU;
    rt_size_t index;

    for (index = 0U; index < length; index++)
    {
        rt_uint32_t bit;
        crc ^= data[index];
        for (bit = 0U; bit < 8U; bit++)
        {
            crc = (crc >> 1) ^ ((crc & 1U) ? 0xEDB88320U : 0U);
        }
    }
    return crc ^ 0xFFFFFFFFU;
}
#endif

int lvgl_disp_init(void)
{
    lv_display_t *disp;

    /* The sections are NOLOAD.  Initialize both surfaces before LTDC can
     * display the first frame; the first LVGL frame then paints the UI. */
    memset(lvgl_fb_a, 0, LVGL_FB_BYTES);
    memset(lvgl_fb_b, 0, LVGL_FB_BYTES);

    disp = lv_st_ltdc_create_direct(lvgl_fb_a, lvgl_fb_b, 0U);
    if(disp == NULL) {
        return -1;
    }

#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    if (rt_mutex_init(&s_framebuffer_capture_lock, "fb_cap",
                      RT_IPC_FLAG_PRIO) != RT_EOK)
    {
        return -1;
    }
    s_framebuffer_capture_lock_ready = RT_TRUE;
#endif

    return 0;
}

#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
rt_err_t lvgl_framebuffer_capture_publish(
    lvgl_framebuffer_capture_info_t *info)
{
    rt_uint32_t source_address;
    const rt_uint8_t *source;

    if (!s_framebuffer_capture_lock_ready || info == RT_NULL)
    {
        return -RT_EINVAL;
    }
    if (rt_mutex_take(&s_framebuffer_capture_lock, RT_WAITING_FOREVER) != RT_EOK)
    {
        return -RT_ERROR;
    }

    /* Read the address the panel is actually scanning, not LVGL's assumed
     * render target. The owner thread is blocked during the copy, so no new
     * LVGL/GPU write can modify either surface until the frozen copy finishes. */
    __DMB();
    source_address = LTDC_Layer1->CFBAR;
    if (source_address == (rt_uint32_t)(uintptr_t)lvgl_fb_a)
    {
        source = (const rt_uint8_t *)lvgl_fb_a;
    }
    else if (source_address == (rt_uint32_t)(uintptr_t)lvgl_fb_b)
    {
        source = (const rt_uint8_t *)lvgl_fb_b;
    }
    else
    {
        rt_mutex_release(&s_framebuffer_capture_lock);
        return -RT_EINVAL;
    }

    rt_memcpy(s_framebuffer_capture, source, LVGL_FB_BYTES);
    lvgl_h7rs_cache_clean_range(s_framebuffer_capture,LVGL_FB_BYTES);
    __DMB();
    s_framebuffer_capture_info.generation++;
    if (s_framebuffer_capture_info.generation == 0U)
    {
        s_framebuffer_capture_info.generation = 1U;
    }
    s_framebuffer_capture_info.source_address = source_address;
    s_framebuffer_capture_info.byte_count = LVGL_FB_BYTES;
    s_framebuffer_capture_info.crc32 =
        framebuffer_capture_crc32(s_framebuffer_capture, LVGL_FB_BYTES);
    s_framebuffer_capture_valid = RT_TRUE;
    *info = s_framebuffer_capture_info;

    rt_mutex_release(&s_framebuffer_capture_lock);
    return RT_EOK;
}

rt_err_t lvgl_framebuffer_capture_get_info(
    lvgl_framebuffer_capture_info_t *info)
{
    if (!s_framebuffer_capture_lock_ready || info == RT_NULL)
    {
        return -RT_EINVAL;
    }
    if (rt_mutex_take(&s_framebuffer_capture_lock, RT_WAITING_FOREVER) != RT_EOK)
    {
        return -RT_ERROR;
    }
    if (!s_framebuffer_capture_valid)
    {
        rt_mutex_release(&s_framebuffer_capture_lock);
        return -RT_EBUSY;
    }
    *info = s_framebuffer_capture_info;
    rt_mutex_release(&s_framebuffer_capture_lock);
    return RT_EOK;
}

int lvgl_framebuffer_capture_copy_page(
    rt_uint32_t generation, rt_uint32_t offset, rt_uint8_t *destination,
    rt_size_t capacity, rt_uint32_t *total_bytes)
{
    rt_size_t copy_length;

    if (!s_framebuffer_capture_lock_ready || destination == RT_NULL ||
        total_bytes == RT_NULL || capacity == 0U)
    {
        return -RT_EINVAL;
    }
    if (rt_mutex_take(&s_framebuffer_capture_lock, RT_WAITING_FOREVER) != RT_EOK)
    {
        return -RT_ERROR;
    }
    if (!s_framebuffer_capture_valid)
    {
        rt_mutex_release(&s_framebuffer_capture_lock);
        return -RT_EBUSY;
    }
    if (generation != s_framebuffer_capture_info.generation)
    {
        rt_mutex_release(&s_framebuffer_capture_lock);
        return -1001;
    }
    *total_bytes = s_framebuffer_capture_info.byte_count;
    if (offset >= *total_bytes)
    {
        rt_mutex_release(&s_framebuffer_capture_lock);
        return -1002;
    }
    copy_length = *total_bytes - offset;
    if (copy_length > capacity)
    {
        copy_length = capacity;
    }
    rt_memcpy(destination, s_framebuffer_capture + offset, copy_length);
    rt_mutex_release(&s_framebuffer_capture_lock);
    return (int)copy_length;
}
#endif

/* FinSH: report LVGL built-in memory pool usage. */
static void lvmem_cmd(void)
{
    lv_mem_monitor_t mon;

    lv_mem_monitor(&mon);
    rt_kprintf("lvgl pool: total_bytes=%lu free_bytes=%lu max_used_bytes=%lu "
               "used_blocks=%lu free_blocks=%lu frag=%lu%%\n",
               (unsigned long)mon.total_size,
               (unsigned long)mon.free_size,
               (unsigned long)mon.max_used,
               (unsigned long)mon.used_cnt,
               (unsigned long)mon.free_cnt,
               (unsigned long)mon.frag_pct);
}
MSH_CMD_EXPORT(lvmem_cmd, show LVGL memory pool usage);
