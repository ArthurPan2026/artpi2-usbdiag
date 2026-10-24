#ifndef WORK_TEXT_PREVIEW_H
#define WORK_TEXT_PREVIEW_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Human-readable metadata only: copy complete UTF-8 scalars, never half a
 * glyph. This does not alter captured USB payloads or their CRCs. */
static inline size_t work_text_preview_copy(char *out, size_t capacity,
                                             const char *source, int ellipsis)
{
    size_t used = 0;
    int truncated = 0;
    if (!out || !capacity) return 0;
    if (!source) source = "";
    while (*source) {
        const uint8_t *p = (const uint8_t *)source;
        size_t length;
        uint32_t codepoint;
        if (p[0] < 0x80) { length = 1; codepoint = p[0]; }
        else if (p[0] >= 0xc2 && p[0] <= 0xdf) { length = 2; codepoint = p[0] & 31; }
        else if (p[0] >= 0xe0 && p[0] <= 0xef) { length = 3; codepoint = p[0] & 15; }
        else if (p[0] >= 0xf0 && p[0] <= 0xf4) { length = 4; codepoint = p[0] & 7; }
        else { truncated = 1; break; }
        size_t i;
        for (i = 1; i < length; i++) {
            if (!p[i] || (p[i] & 0xc0) != 0x80) break;
            codepoint = (codepoint << 6) | (p[i] & 63);
        }
        if (i != length || (length == 3 && codepoint < 0x800) ||
            (length == 4 && (codepoint < 0x10000 || codepoint > 0x10ffff)) ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff)) { truncated = 1; break; }
        if (used + length >= capacity) { truncated = 1; break; }
        memcpy(out + used, source, length);
        used += length; source += length;
    }
    if (truncated && ellipsis && capacity >= 4) {
        while (used > capacity - 4) {
            used--;
            while (used && ((uint8_t)out[used] & 0xc0) == 0x80) used--;
        }
        memcpy(out + used, "...", 3); used += 3;
    }
    out[used] = '\0';
    return used;
}
#endif
