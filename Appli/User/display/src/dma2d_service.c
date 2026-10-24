/*
 * Project-owned DMA2D service for STM32H7RS.
 */
#include "dma2d_service.h"
#include "usb_perf.h"
#include "dma2d_broker.h"
#include "lvgl_h7rs_cache.h"

#include "dma2d.h"
#include "stm32h7rsxx_hal.h"

#include <finsh.h>
#include <rtthread.h>

#define DMA2D_SERVICE_TIMEOUT_TICKS (2U * RT_TICK_PER_SECOND)
#define DMA2D_TEST_WIDTH            64U
#define DMA2D_TEST_HEIGHT           64U

extern DMA2D_HandleTypeDef hdma2d;

static struct rt_semaphore s_dma2d_done;
static volatile rt_bool_t s_dma2d_initialized;
static volatile rt_bool_t s_dma2d_active;
static volatile rt_err_t s_dma2d_status;
static volatile uint32_t s_dma2d_jobs;
static volatile uint32_t s_dma2d_error_count;
static volatile uint32_t s_dma2d_last_error;

/* Per-window profiling counters.  The service is synchronous and serialized,
 * therefore a transfer belongs to the LVGL frame in which its API call was
 * made. */
static volatile uint32_t s_dma2d_frame_open;
static volatile uint32_t s_dma2d_frame_jobs;
static volatile uint32_t s_dma2d_frame_cycles;
static volatile uint32_t s_dma2d_profile_frames;
static volatile uint32_t s_dma2d_profile_active_frames;
static volatile uint32_t s_dma2d_profile_jobs;
static volatile uint32_t s_dma2d_profile_errors;
static volatile uint32_t s_dma2d_profile_max_frame_cycles;
static volatile uint32_t s_dma2d_profile_last_job_cycles;
static volatile uint64_t s_dma2d_profile_total_cycles;
static volatile uint64_t s_dma2d_profile_total_bytes;

/* Small diagnostic surface in the non-cacheable PSRAM arena. */
__attribute__((section(".psram_heap"), aligned(32), used))
static uint16_t s_dma2d_test_surface[DMA2D_TEST_WIDTH * DMA2D_TEST_HEIGHT];

static void dma2d_service_complete(DMA2D_HandleTypeDef *handle)
{
    RT_UNUSED(handle);
    s_dma2d_status = RT_EOK;
    s_dma2d_active = RT_FALSE;
    rt_sem_release(&s_dma2d_done);
}

static void dma2d_service_error(DMA2D_HandleTypeDef *handle)
{
    s_dma2d_last_error = HAL_DMA2D_GetError(handle);
    dma2d_broker_set_error(DMA2D_BROKER_OWNER_SERVICE, s_dma2d_last_error);
    s_dma2d_error_count++;
    s_dma2d_profile_errors++;
    s_dma2d_status = -RT_ERROR;
    s_dma2d_active = RT_FALSE;
    rt_sem_release(&s_dma2d_done);
}

static uint64_t dma2d_service_estimate_bytes(uint32_t mode,
                                             uint32_t width,
                                             uint32_t height)
{
    uint64_t pixels = (uint64_t)width * (uint64_t)height;

    /* RGB565 is used for both the service source and destination. */
    return mode == DMA2D_R2M ? pixels * 2U : pixels * 4U;
}

static void dma2d_service_record_job(uint32_t mode, uint32_t width,
                                     uint32_t height, uint32_t start_cycles)
{
    uint32_t elapsed_cycles = DWT->CYCCNT - start_cycles;
    uint64_t bytes = dma2d_service_estimate_bytes(mode, width, height);

    s_dma2d_profile_jobs++;
    s_dma2d_profile_last_job_cycles = elapsed_cycles;
    s_dma2d_profile_total_cycles += elapsed_cycles;
    s_dma2d_profile_total_bytes += bytes;
    if(s_dma2d_frame_open)
    {
        s_dma2d_frame_jobs++;
        s_dma2d_frame_cycles += elapsed_cycles;
    }
}

int dma2d_service_init(void)
{
    rt_err_t result;

    if(s_dma2d_initialized) {
        return RT_EOK;
    }

    result = dma2d_broker_init();
    if(result != RT_EOK) {
        return result;
    }

    result = rt_sem_init(&s_dma2d_done, "d2ddone", 0, RT_IPC_FLAG_PRIO);
    if(result != RT_EOK) {
        return result;
    }

    /* HAL_DMA2D_Start_IT dispatches these function pointers from the ISR.
     * They remain installed when the transfer mode is reconfigured. */
    hdma2d.XferCpltCallback = dma2d_service_complete;
    hdma2d.XferErrorCallback = dma2d_service_error;
    s_dma2d_status = RT_EOK;
    s_dma2d_initialized = RT_TRUE;

    return RT_EOK;
}

static int dma2d_service_validate(uint32_t src_stride_pixels,
                                  uint32_t dst_stride_pixels,
                                  uint32_t width, uint32_t height)
{
    if(width == 0U || height == 0U || width > 0x3FFFU || height > 0xFFFFU) {
        return -RT_EINVAL;
    }
    if(src_stride_pixels < width || dst_stride_pixels < width) {
        return -RT_EINVAL;
    }
    if((src_stride_pixels - width) > 0x3FFFU ||
       (dst_stride_pixels - width) > 0x3FFFU) {
        return -RT_EINVAL;
    }

    return RT_EOK;
}

static int dma2d_service_prepare(uint32_t mode, uint32_t src_stride_pixels,
                                 uint32_t dst_stride_pixels)
{
    hdma2d.Init.Mode = mode;
    hdma2d.Init.ColorMode = DMA2D_OUTPUT_RGB565;
    hdma2d.Init.OutputOffset = dst_stride_pixels;

    if(HAL_DMA2D_Init(&hdma2d) != HAL_OK) {
        return -RT_ERROR;
    }

    if(mode == DMA2D_M2M) {
        hdma2d.LayerCfg[1].InputOffset = src_stride_pixels;
        hdma2d.LayerCfg[1].InputColorMode = DMA2D_INPUT_RGB565;
        hdma2d.LayerCfg[1].AlphaMode = DMA2D_NO_MODIF_ALPHA;
        hdma2d.LayerCfg[1].InputAlpha = 0U;
        hdma2d.LayerCfg[1].AlphaInverted = DMA2D_REGULAR_ALPHA;
        hdma2d.LayerCfg[1].RedBlueSwap = DMA2D_RB_REGULAR;
        hdma2d.LayerCfg[1].ChromaSubSampling = DMA2D_NO_CSS;

        if(HAL_DMA2D_ConfigLayer(&hdma2d, DMA2D_FOREGROUND_LAYER) != HAL_OK) {
            return -RT_ERROR;
        }
    }

    return RT_EOK;
}

static int dma2d_service_wait_start(uint32_t mode, uint32_t src_or_color,
                                    uint32_t dst, uint32_t src_stride_pixels,
                                    uint32_t dst_stride_pixels, uint32_t width,
                                    uint32_t height)
{
    rt_err_t result;
    int status;
    uint32_t start_cycles;
    uint32_t wait_start_cycles;

    if(!s_dma2d_initialized) {
        return -RT_ERROR;
    }
    status = dma2d_service_validate(src_stride_pixels, dst_stride_pixels,
                                    width, height);
    if(status != RT_EOK) {
        return status;
    }

    result = dma2d_broker_acquire(DMA2D_BROKER_OWNER_SERVICE,
                                  DMA2D_SERVICE_TIMEOUT_TICKS);
    if(result != RT_EOK) {
        return result;
    }

    /* Remove a completion left by an aborted or already finished job. */
    while(rt_sem_take(&s_dma2d_done, 0) == RT_EOK) {
    }

    status = dma2d_service_prepare(mode,
                                   src_stride_pixels - width,
                                   dst_stride_pixels - width);
    if(status != RT_EOK) {
        s_dma2d_error_count++;
        s_dma2d_profile_errors++;
        dma2d_broker_release(DMA2D_BROKER_OWNER_SERVICE);
        return status;
    }

    s_dma2d_status = -RT_ERROR;
    s_dma2d_active = RT_TRUE;
    s_dma2d_jobs++;
    start_cycles = DWT->CYCCNT;

    if(mode == DMA2D_M2M) {
        lvgl_h7rs_cache_clean_range((const void *)(uintptr_t)src_or_color,
                                    (size_t)src_stride_pixels * height * sizeof(uint16_t));
    }
    lvgl_h7rs_cache_invalidate_range((const void *)(uintptr_t)dst,
                                     (size_t)dst_stride_pixels * height * sizeof(uint16_t));

    if(HAL_DMA2D_Start_IT(&hdma2d, src_or_color, dst, width, height) != HAL_OK) {
        s_dma2d_active = RT_FALSE;
        s_dma2d_status = -RT_ERROR;
        s_dma2d_error_count++;
        s_dma2d_profile_errors++;
        dma2d_service_record_job(mode, width, height, start_cycles);
        dma2d_broker_release(DMA2D_BROKER_OWNER_SERVICE);
        return -RT_ERROR;
    }

    wait_start_cycles = DWT->CYCCNT;
    result = rt_sem_take(&s_dma2d_done, DMA2D_SERVICE_TIMEOUT_TICKS);
    usb_perf_record(USB_PERF_DMA2D_JOB_WAIT, wait_start_cycles,
                    DWT->CYCCNT);
    if(result != RT_EOK) {
        s_dma2d_active = RT_FALSE;
        (void)HAL_DMA2D_Abort(&hdma2d);
        s_dma2d_status = -RT_ETIMEOUT;
        s_dma2d_error_count++;
        s_dma2d_profile_errors++;
        dma2d_service_record_job(mode, width, height, start_cycles);
        dma2d_broker_release(DMA2D_BROKER_OWNER_SERVICE);
        return -RT_ETIMEOUT;
    }

    status = s_dma2d_status;
    lvgl_h7rs_cache_invalidate_range((const void *)(uintptr_t)dst,
                                     (size_t)dst_stride_pixels * height * sizeof(uint16_t));
    dma2d_service_record_job(mode, width, height, start_cycles);
    dma2d_broker_release(DMA2D_BROKER_OWNER_SERVICE);
    return status;
}

int dma2d_service_fill_rgb565(uint16_t *dst, uint32_t dst_stride_pixels,
                              uint16_t color, uint32_t width, uint32_t height)
{
    uint32_t red;
    uint32_t green;
    uint32_t blue;
    uint32_t argb8888;

    if(dst == RT_NULL) {
        return -RT_EINVAL;
    }

    /* HAL_DMA2D_Start_IT expects the R2M color as 0x00RRGGBB and converts it
     * to the configured output format.  The public API intentionally accepts
     * the framebuffer's native RGB565 value. */
    red = ((uint32_t)(color >> 11) & 0x1FU) * 255U / 31U;
    green = ((uint32_t)(color >> 5) & 0x3FU) * 255U / 63U;
    blue = ((uint32_t)color & 0x1FU) * 255U / 31U;
    argb8888 = (red << 16) | (green << 8) | blue;

    return dma2d_service_wait_start(DMA2D_R2M, argb8888,
                                    (uint32_t)dst, width, dst_stride_pixels,
                                    width, height);
}

int dma2d_service_blit_rgb565(const uint16_t *src, uint32_t src_stride_pixels,
                              uint16_t *dst, uint32_t dst_stride_pixels,
                              uint32_t width, uint32_t height)
{
    if(src == RT_NULL || dst == RT_NULL) {
        return -RT_EINVAL;
    }

    return dma2d_service_wait_start(DMA2D_M2M, (uint32_t)src,
                                    (uint32_t)dst, src_stride_pixels,
                                    dst_stride_pixels, width, height);
}

uint32_t dma2d_service_error_count(void)
{
    return s_dma2d_error_count;
}

uint32_t dma2d_service_last_error(void)
{
    return s_dma2d_last_error;
}

void dma2d_service_frame_begin(void)
{
    s_dma2d_frame_jobs = 0U;
    s_dma2d_frame_cycles = 0U;
    s_dma2d_frame_open = 1U;
}

void dma2d_service_frame_end(void)
{
    if(!s_dma2d_frame_open)
    {
        return;
    }

    s_dma2d_profile_frames++;
    if(s_dma2d_frame_jobs != 0U)
    {
        s_dma2d_profile_active_frames++;
    }
    if(s_dma2d_frame_cycles > s_dma2d_profile_max_frame_cycles)
    {
        s_dma2d_profile_max_frame_cycles = s_dma2d_frame_cycles;
    }
    s_dma2d_frame_open = 0U;
}

void dma2d_service_stats_reset(void)
{
    s_dma2d_profile_frames = 0U;
    s_dma2d_profile_active_frames = 0U;
    s_dma2d_profile_jobs = 0U;
    s_dma2d_profile_errors = 0U;
    s_dma2d_profile_max_frame_cycles = 0U;
    s_dma2d_profile_last_job_cycles = 0U;
    s_dma2d_profile_total_cycles = 0U;
    s_dma2d_profile_total_bytes = 0U;
    s_dma2d_frame_open = 0U;
    s_dma2d_frame_jobs = 0U;
    s_dma2d_frame_cycles = 0U;
}

void dma2d_service_stats_get(dma2d_service_stats_t *stats)
{
    if(stats == RT_NULL)
    {
        return;
    }

    stats->frames = s_dma2d_profile_frames;
    stats->active_frames = s_dma2d_profile_active_frames;
    stats->jobs = s_dma2d_profile_jobs;
    stats->errors = s_dma2d_profile_errors;
    stats->max_frame_cycles = s_dma2d_profile_max_frame_cycles;
    stats->last_job_cycles = s_dma2d_profile_last_job_cycles;
    stats->total_cycles = s_dma2d_profile_total_cycles;
    stats->total_bytes = s_dma2d_profile_total_bytes;
}

static void dma2d_status_cmd(void)
{
    rt_kprintf("[DMA2D] initialized=%d active=%d jobs=%lu errors=%lu last=0x%08lx state=%lu\n",
               (int)s_dma2d_initialized,
               (int)s_dma2d_active,
               (unsigned long)s_dma2d_jobs,
               (unsigned long)s_dma2d_error_count,
               (unsigned long)s_dma2d_last_error,
               (unsigned long)HAL_DMA2D_GetState(&hdma2d));
}
MSH_CMD_EXPORT(dma2d_status_cmd, show DMA2D service status);

static void dma2d_test_cmd(void)
{
    uint32_t i;
    int result;

    result = dma2d_service_fill_rgb565(s_dma2d_test_surface,
                                       DMA2D_TEST_WIDTH,
                                       0x07E0U,
                                       DMA2D_TEST_WIDTH,
                                       DMA2D_TEST_HEIGHT);
    if(result != RT_EOK) {
        rt_kprintf("[DMA2D] test failed: %d error=0x%08lx\n",
                   result, (unsigned long)s_dma2d_last_error);
        return;
    }

    for(i = 0; i < (DMA2D_TEST_WIDTH * DMA2D_TEST_HEIGHT); i++) {
        if(s_dma2d_test_surface[i] != 0x07E0U) {
            rt_kprintf("[DMA2D] test verify failed at %lu: 0x%04x\n",
                       (unsigned long)i, s_dma2d_test_surface[i]);
            return;
        }
    }

    rt_kprintf("[DMA2D] test passed: %ux%u RGB565 fill\n",
               DMA2D_TEST_WIDTH, DMA2D_TEST_HEIGHT);
}
MSH_CMD_EXPORT(dma2d_test_cmd, run DMA2D RGB565 fill test);
