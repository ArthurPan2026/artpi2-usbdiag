/**
 * Runtime coverage accounting for the unified 14/16/20/24 px UI glyph set.
 */
#include <rtthread.h>
#include <string.h>
#include "ui_fonts.h"

static ui_font_coverage_t g_coverage;
static uint32_t g_missing_codepoints[UI_FONT_MISSING_MAX];
static const lv_font_t * const s_ui_fonts[] = {
    UI_FONT_14, UI_FONT_16, UI_FONT_20, UI_FONT_24
};
/* Coverage is immutable after the four generated cmaps pass the build gate.
 * Avoid four font-descriptor lookups for every repeated ASCII character.
 * Non-ASCII results are verified across all sizes once and memoized. */
typedef struct { uint32_t codepoint; rt_bool_t available; } glyph_check_t;
static glyph_check_t s_glyph_checks[96];
static unsigned s_glyph_check_next;

static uint32_t ui_font_utf8_next(const char **text, rt_bool_t *valid)
{
    const uint8_t *bytes = (const uint8_t *)*text;
    uint32_t codepoint;
    uint8_t length;

    if (valid)
    {
        *valid = RT_TRUE;
    }
    if (bytes[0] < 0x80U) {
        *text += 1;
        return bytes[0];
    }

    if ((bytes[0] & 0xE0U) == 0xC0U) {
        codepoint = bytes[0] & 0x1FU;
        length = 2U;
    } else if ((bytes[0] & 0xF0U) == 0xE0U) {
        codepoint = bytes[0] & 0x0FU;
        length = 3U;
    } else if ((bytes[0] & 0xF8U) == 0xF0U) {
        codepoint = bytes[0] & 0x07U;
        length = 4U;
    } else {
        *text += 1;
        if (valid)
        {
            *valid = RT_FALSE;
        }
        return 0xFFFDU;
    }

    for (uint8_t index = 1U; index < length; index++) {
        if (bytes[index] == '\0') {
            *text += 1;
            if (valid)
            {
                *valid = RT_FALSE;
            }
            return 0xFFFDU;
        }
        if ((bytes[index] & 0xC0U) != 0x80U) {
            *text += 1;
            if (valid)
            {
                *valid = RT_FALSE;
            }
            return 0xFFFDU;
        }
        codepoint = (codepoint << 6) | (bytes[index] & 0x3FU);
    }

    if ((length == 2U && codepoint < 0x80U) ||
        (length == 3U && codepoint < 0x800U) ||
        (length == 4U && (codepoint < 0x10000U || codepoint > 0x10FFFFU)) ||
        (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) {
        *text += 1;
        if (valid)
        {
            *valid = RT_FALSE;
        }
        return 0xFFFDU;
    }

    *text += length;
    return codepoint;
}

static rt_bool_t ui_font_has_glyph(uint32_t codepoint)
{
    lv_font_glyph_dsc_t glyph_dsc;
    rt_bool_t available = RT_TRUE;
    if (codepoint >= 0x20U && codepoint <= 0x7EU) return RT_TRUE;
    for (unsigned i = 0; i < 96; i++)
        if (s_glyph_checks[i].codepoint == codepoint) return s_glyph_checks[i].available;

    for (size_t index = 0U; index < sizeof(s_ui_fonts) / sizeof(s_ui_fonts[0]); index++)
    {
        if (!lv_font_get_glyph_dsc(s_ui_fonts[index], &glyph_dsc, codepoint, 0U) ||
            glyph_dsc.is_placeholder)
        {
            available = RT_FALSE;
        }
    }
    s_glyph_checks[s_glyph_check_next].codepoint = codepoint;
    s_glyph_checks[s_glyph_check_next].available = available;
    s_glyph_check_next = (s_glyph_check_next + 1U) % 96U;
    return available;
}

static rt_bool_t ui_font_is_missing_known(uint32_t codepoint)
{
    for (uint16_t index = 0U; index < g_coverage.missing_unique_count; index++) {
        if (g_missing_codepoints[index] == codepoint) {
            return RT_TRUE;
        }
    }
    return RT_FALSE;
}

static void ui_font_record_missing(uint32_t codepoint)
{
    if (ui_font_is_missing_known(codepoint)) {
        return;
    }

    g_coverage.last_missing_codepoint = codepoint;
    if (g_coverage.missing_unique_count < UI_FONT_MISSING_MAX) {
        g_missing_codepoints[g_coverage.missing_unique_count] = codepoint;
        g_coverage.missing_unique_count++;
    } else if (g_coverage.missing_overflow_count < UINT16_MAX) {
        g_coverage.missing_overflow_count++;
    }
    g_coverage.revision++;
}

void ui_font_observe_dynamic_text(const char *text)
{
    const char *cursor = text;

    if (!cursor) {
        return;
    }

    g_coverage.observed_text_count++;
    while (*cursor) {
        rt_bool_t valid;
        uint32_t codepoint = ui_font_utf8_next(&cursor, &valid);

        if (!valid || codepoint < 0x20U || codepoint == 0x7FU ||
            !ui_font_has_glyph(codepoint)) {
            ui_font_record_missing(codepoint);
        }
    }
}

static rt_size_t ui_font_sanitize_dynamic_text_impl(
    const char *input, char *output, rt_size_t output_size,
    rt_bool_t record_missing)
{
    const char *cursor = input;
    rt_size_t written = 0U;

    if (output == RT_NULL || output_size == 0U)
    {
        return 0U;
    }
    output[0] = '\0';
    if (input == RT_NULL)
    {
        return 0U;
    }

    while (*cursor != '\0' && written + 1U < output_size)
    {
        const char *character = cursor;
        rt_bool_t valid;
        uint32_t codepoint = ui_font_utf8_next(&cursor, &valid);
        rt_bool_t safe = valid && codepoint >= 0x20U && codepoint != 0x7FU &&
                         ui_font_has_glyph(codepoint);

        if (safe)
        {
            rt_size_t byte_count = (rt_size_t)(cursor - character);

            /* Copy each Unicode scalar atomically so the caller's bound
             * never leaves a partial UTF-8 sequence at the end. */
            if (written + byte_count >= output_size)
            {
                break;
            }
            rt_memcpy(output + written, character, byte_count);
            written += byte_count;
        }
        else
        {
            if (record_missing)
            {
                ui_font_record_missing(codepoint);
            }
            output[written++] = '?';
        }
    }
    output[written] = '\0';
    if (record_missing)
    {
        g_coverage.observed_text_count++;
    }
    return written;
}

size_t ui_font_sanitize_dynamic_text(const char *input, char *output,
                                     size_t output_size)
{
    return ui_font_sanitize_dynamic_text_impl(input, output, output_size,
                                               RT_TRUE);
}

#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
static void ui_font_sanitizer_test_cmd(void)
{
    static const char input[] = {
        'A', (char)0xE5, (char)0xA3, (char)0xB0,
        (char)0xE6, (char)0x98, (char)0x8E,
        (char)0xF0, (char)0x9F, (char)0x98, (char)0x80,
        'B', (char)0xC0, 'C', '\0'
    };
    static const char expected[] = "A声明?B?C";
    char output[16];
    char truncated_output[3];
    rt_size_t output_length;
    rt_size_t truncated_length;

    output_length = ui_font_sanitize_dynamic_text_impl(
        input, output, sizeof(output), RT_FALSE);
    truncated_length = ui_font_sanitize_dynamic_text_impl(
        "声", truncated_output, sizeof(truncated_output), RT_FALSE);
    if (rt_strcmp(output, expected) == 0 && output_length == sizeof(expected) - 1U &&
        truncated_length == 0U && truncated_output[0] == '\0')
    {
        rt_kprintf("[UI_FONT_TEST] PASS sizes=14,16,20,24 supported_glyphs=2\r\n");
        rt_kprintf("[UI_FONT_TEST] unsupported=visible-fallback invalid_utf8=visible-fallback utf8_truncation=atomic\r\n");
    }
    else
    {
        rt_kprintf("[UI_FONT_TEST] FAIL output=%s\r\n", output);
    }
}
MSH_CMD_EXPORT(ui_font_sanitizer_test_cmd,
               debug-only LVGL runtime glyph fallback test);
#endif

void ui_font_get_coverage(ui_font_coverage_t *coverage)
{
    if (!coverage) {
        return;
    }
    *coverage = g_coverage;
}

void ui_font_reset_coverage(void)
{
    uint32_t revision = g_coverage.revision + 1U;

    rt_memset(&g_coverage, 0, sizeof(g_coverage));
    rt_memset(g_missing_codepoints, 0, sizeof(g_missing_codepoints));
    g_coverage.revision = revision;
}
