/**
 * LVGL 9.6 project configuration for the STM32H7RS board.
 *
 * This file intentionally contains only project policy.  All defaults and
 * option validation come from the official LVGL 9.6 generated
 * lv_conf_internal.h under include/lvgl/config.
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/* The CMake profile controls diagnostics, not the LVGL API version. */
#if defined(LVGL_PROFILE_RELEASE)
#define LVGL_PROFILE_IS_RELEASE 1
#else
#define LVGL_PROFILE_IS_RELEASE 0
#endif

/*==================== Color ====================*/
#define LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_RGB565
#define LV_DISABLE_API_MAPPING 1

/*==================== OS and standard library ====================*/
#define LV_USE_OS              LV_OS_RTTHREAD
#define LV_USE_STDLIB_MALLOC  LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_STRING  LV_STDLIB_RTTHREAD
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_RTTHREAD

/* LVGL objects stay in the dedicated PSRAM arena, separate from RT-Thread's
 * kernel heap and the H7RS DMA pool. */
#define LV_MEM_SIZE          (512u * 1024u)
#define LV_MEM_POOL_INCLUDE  "lvgl_mem_pool.h"
#define LV_MEM_POOL_ALLOC(size) ((void *)g_lvgl_mem_pool)

/*==================== Tick and draw buffers ====================*/
#define LV_TICK_CUSTOM             1
#define LV_DRAW_BUF_STRIDE_ALIGN   4
#define LV_DRAW_BUF_ALIGN          32
#define LV_LABEL_LONG_TXT_HINT     1

/*==================== Display ====================*/
#define LV_DEF_REFR_PERIOD         5
#define LV_USE_ST_LTDC             1
#define LV_ST_LTDC_USE_DMA2D_FLUSH 0

/*==================== NemaGFX / NeoChrom ====================*/
#define LV_USE_NEMA_GFX            1
#define LV_USE_NEMA_LIB            LV_NEMA_LIB_M7
#define LV_USE_NEMA_HAL            LV_NEMA_HAL_STM32
#define LV_NEMA_STM32_HAL_INCLUDE  <stm32h7rsxx_hal.h>
#define LV_NEMA_STM32_HAL_ATTRIBUTE_POOL_MEM \
    __attribute__((section(".nema_pool"), aligned(32)))
#define LV_NEMA_USE_CACHE          1
#define LV_NEMA_CACHE_HAL_INCLUDE  "stm32h7rsxx_hal.h"
#define LV_USE_NEMA_VG             0

/* Project H7RS Nema settings.  These are consumed by the project-preserved
 * synchronous command-list adapter and are not LVGL compatibility aliases. */
#define LV_NEMA_GFX_MEM_POOL_SIZE       (32u * 1024u)
#define LV_NEMA_GFX_COMMAND_LIST_SIZE   (8u * 1024u)
#define LV_NEMA_GFX_RING_SIZE           (4u * 1024u)
#define LV_NEMA_GFX_USE_RAW_FONT        1
#define LV_NEMA_GFX_ACCELERATE_LABEL    1
#define LV_NEMA_GFX_USE_THREAD          0

/*==================== Draw units ====================*/
#define LV_USE_DRAW_SW                 1
#define LV_DRAW_SW_SUPPORT_RGB565     1
#define LV_DRAW_SW_SUPPORT_RGB888     1
#define LV_DRAW_SW_SUPPORT_XRGB8888   0
#define LV_DRAW_SW_SUPPORT_ARGB8888   1
#define LV_DRAW_SW_SUPPORT_L8         0
#define LV_DRAW_SW_SUPPORT_AL88       0
#define LV_DRAW_SW_SUPPORT_A8         0
#define LV_DRAW_SW_SUPPORT_I1         0

/* LVGL DMA2D and the project DMA2D service share one H7RS broker. */
#ifndef LVGL_DRAW_DMA2D_ENABLED
#define LVGL_DRAW_DMA2D_ENABLED       1
#endif
#define LV_USE_DRAW_DMA2D             LVGL_DRAW_DMA2D_ENABLED
#define LV_DRAW_DMA2D_HAL_INCLUDE     "stm32h7rsxx_hal.h"
#define LV_USE_DRAW_DMA2D_INTERRUPT   LVGL_DRAW_DMA2D_ENABLED

/*==================== Themes and widgets ====================*/
#define LV_USE_THEME_DEFAULT           1
#define LV_USE_THEME_SIMPLE            0
#define LV_USE_THEME_MONO              0
#define LV_USE_FLEX                    1
#define LV_USE_GRID                    0

#define LV_USE_ANIMIMG                 0
#define LV_USE_ARC                     0
#define LV_USE_ARCLABEL                0
#define LV_USE_BAR                     0
#define LV_USE_BARCODE                 0
#define LV_USE_BUTTON                  1
#define LV_USE_BUTTONMATRIX            1
#define LV_USE_CALENDAR                0
#define LV_USE_CANVAS                  1
#define LV_USE_CHART                   0
#define LV_USE_CHECKBOX                0
#define LV_USE_DROPDOWN                0
#define LV_USE_FFMPEG                  0
#define LV_USE_GIF                     0
#define LV_USE_GSTREAMER               0
#define LV_USE_IMAGE                   1
#define LV_USE_IMAGEBUTTON             0
#define LV_USE_IME_PINYIN              0
#define LV_USE_KEYBOARD                1
#define LV_USE_LABEL                   1
#define LV_USE_LED                     0
#define LV_USE_LINE                    0
#define LV_USE_LIST                    0
#define LV_USE_LOTTIE                  0
#define LV_USE_MENU                    0
#define LV_USE_MSGBOX                  0
#define LV_USE_QRCODE                  0
#define LV_USE_ROLLER                  1
#define LV_USE_SCALE                   0
#define LV_USE_SLIDER                  0
#define LV_USE_SPAN                    0
#define LV_USE_SPINBOX                 0
#define LV_USE_SPINNER                 0
#define LV_USE_SWITCH                  0
#define LV_USE_TABLE                   0
#define LV_USE_TABVIEW                 0
#define LV_USE_TEXTAREA                1
#define LV_USE_TILEVIEW                0
#define LV_USE_3DTEXTURE               0
#define LV_USE_OBJX_TEMPL              0
#define LV_USE_WIN                     0

/* 9.6's object style cache is a low-cost win for the descriptor-heavy UI. */
#define LV_OBJ_STYLE_CACHE             1

/* The current UI uses compiled image/font assets rather than LVGL file
 * decoders, so these caches would consume RAM without improving redraws. */
#define LV_CACHE_DEF_SIZE              0
#define LV_IMAGE_HEADER_CACHE_DEF_CNT  0

/*==================== Fonts ====================*/
#define LV_FONT_MONTSERRAT_14          1
#define LV_FONT_MONTSERRAT_20          1
#define LV_FONT_DEFAULT                LV_FONT_DEFAULT_MONTSERRAT_14

/*==================== Diagnostics ====================*/
#define LV_USE_LOG                     0
#define LV_USE_PERF_MONITOR            0
#define LV_USE_SYSMON                  0
#define LV_USE_ASSERT                  (!LVGL_PROFILE_IS_RELEASE)
#define LV_USE_ASSERT_NULL             (!LVGL_PROFILE_IS_RELEASE)
#define LV_USE_ASSERT_MALLOC           (!LVGL_PROFILE_IS_RELEASE)
#define LV_ASSERT_USE_CUSTOM_INCLUDE   (!LVGL_PROFILE_IS_RELEASE)
#define LV_ASSERT_CUSTOM_INCLUDE       "work_assert_capture.h"
#define LV_USE_CHECK_ARG               1
#define LV_CHECK_ARG_ASSERT_ON_FAIL    0
#define LV_CHECK_ARG_LOG_MODE          LV_CHECK_ARG_LOG_MODE_NONE

#if LVGL_PROFILE_IS_RELEASE
#define LV_USE_PROFILER                0
#define LV_USE_PROFILER_BUILTIN        0
#else
#define LV_USE_PROFILER                1
#define LV_USE_PROFILER_BUILTIN        1
#define LV_PROFILER_BUILTIN_BUF_SIZE   16384
#define LV_PROFILER_BUILTIN_DEFAULT_ENABLE 0
#endif

#endif /* LV_CONF_H */
