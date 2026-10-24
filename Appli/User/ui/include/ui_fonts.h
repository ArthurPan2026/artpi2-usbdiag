#ifndef UI_FONTS_H
#define UI_FONTS_H

#include <stdint.h>
#include <stddef.h>
#include <lvgl/lvgl.h>

#define UI_FONT_MISSING_MAX 16U
#define UI_FONT_GPU_ALIGNMENT 64U

/* BEGIN GENERATED UI FONT METADATA */
/* source_sha256=450625c8d46ab3df97b7904ded955ec2746d17ec76740cb1e91d1ba63a0f89af manifest_sha256=f134fa1f642d8167d743bee04b3fe7fdd714cf8916e1b3feefb24b674597e6f4 converter=lv_font_conv@1.5.3 */
#define UI_FONT_14_BITMAP_BYTES 45295U
#define UI_FONT_14_GPU_CAPACITY 45312U
#define UI_FONT_16_BITMAP_BYTES 57834U
#define UI_FONT_16_GPU_CAPACITY 57856U
#define UI_FONT_20_BITMAP_BYTES 88517U
#define UI_FONT_20_GPU_CAPACITY 88576U
#define UI_FONT_24_BITMAP_BYTES 127082U
#define UI_FONT_24_GPU_CAPACITY 127104U
#define UI_FONT_GPU_STAGING_BYTES 318848U
/* END GENERATED UI FONT METADATA */

typedef struct {
    uint32_t observed_text_count;
    uint32_t last_missing_codepoint;
    uint32_t revision;
    uint16_t missing_unique_count;
    uint16_t missing_overflow_count;
} ui_font_coverage_t;

LV_FONT_DECLARE(lv_font_notosans_14);
LV_FONT_DECLARE(lv_font_notosans_16);
LV_FONT_DECLARE(lv_font_notosans_20);
LV_FONT_DECLARE(lv_font_notosans_24);

/* Keep NemaGFX font reads in non-cacheable PSRAM instead of XIP Flash. */
size_t lv_font_notosans_14_gpu_bitmap_size(void);
int lv_font_notosans_14_gpu_prepare(void *destination, size_t capacity);
size_t lv_font_notosans_16_gpu_bitmap_size(void);
int lv_font_notosans_16_gpu_prepare(void *destination, size_t capacity);
size_t lv_font_notosans_20_gpu_bitmap_size(void);
int lv_font_notosans_20_gpu_prepare(void *destination, size_t capacity);
size_t lv_font_notosans_24_gpu_bitmap_size(void);
int lv_font_notosans_24_gpu_prepare(void *destination, size_t capacity);
void ui_font_gpu_init(void);

#define UI_FONT_14  (&lv_font_notosans_14)
#define UI_FONT_16  (&lv_font_notosans_16)
#define UI_FONT_20  (&lv_font_notosans_20)
#define UI_FONT_24  (&lv_font_notosans_24)

/* These APIs are owned by the LVGL thread because they query font glyphs. */
void ui_font_observe_dynamic_text(const char *text);
/* Copy runtime-provided text, checking every printable scalar against all four
 * sizes. Unsupported/invalid codepoints become ASCII '?'; evidence bytes are
 * not modified. Input and output buffers must not overlap. */
size_t ui_font_sanitize_dynamic_text(const char *input, char *output,
                                     size_t output_size);
void ui_font_get_coverage(ui_font_coverage_t *coverage);
void ui_font_reset_coverage(void);

#endif
