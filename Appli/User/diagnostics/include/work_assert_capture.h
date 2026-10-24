#ifndef WORK_ASSERT_CAPTURE_H
#define WORK_ASSERT_CAPTURE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void work_assert_capture_init(void);
__attribute__((noreturn)) void work_lvgl_assert_capture(const char *file,
                                                      const char *function,
                                                      uint32_t line);

#ifdef __cplusplus
}
#endif

/* LVGL 9.6 includes this through LV_ASSERT_CUSTOM_INCLUDE. It retains the
 * fail-stop behavior while recording the location before UART output. */
#ifndef LV_ASSERT_HANDLER
#define LV_ASSERT_HANDLER work_lvgl_assert_capture(__FILE__, __func__, __LINE__);
#endif

#endif
