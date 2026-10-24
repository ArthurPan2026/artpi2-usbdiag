#include "web_pdf_report.h"
#include "watchdog_supervisor.h"

#include <limits.h>
#include <string.h>

#include "build_identity.h"
#include "hpdf.h"
#include "usb_descriptor_store.h"
#include "usb_evidence.h"
#include "web_pdf_assets.h"

#define WEB_PDF_CACHE_BYTES       (1024U * 1024U)
#define WEB_PDF_MEMHEAP_NAME      "pdfheap"
#define WEB_PDF_THREAD_NAME       "pdfgen"
#define WEB_PDF_THREAD_STACK_SIZE (20U * 1024U)
#define WEB_PDF_THREAD_PRIORITY   18U
#define WEB_PDF_THREAD_TIMESLICE  5U
#define WEB_PDF_PAGE_WIDTH        842.0f
#define WEB_PDF_PAGE_HEIGHT       595.0f
#define WEB_PDF_PAGE_MARGIN       36.0f

#define WEB_PDF_STATIC_SECTION \
    __attribute__((section(".psram_heap"), aligned(32)))
#define WEB_PDF_CACHE_SECTION \
    __attribute__((section(".pdf_report_cache"), aligned(32)))
#define WEB_PDF_THREAD_SECTION \
    __attribute__((section(".web_thread_ram"), aligned(32)))

extern rt_uint8_t __libharu_heap_start;
extern rt_uint8_t __libharu_heap_end;

typedef struct
{
    void *allocation_base;
    rt_size_t payload_size;
} web_pdf_allocation_header_t;

typedef struct
{
    rt_uint32_t current_bytes;
    rt_uint32_t peak_bytes;
    rt_uint32_t total_bytes;
    rt_uint32_t allocation_failures;
} web_pdf_allocator_stats_t;

static struct rt_memheap s_pdf_memheap;
static struct rt_mutex s_pdf_lock;
static struct rt_semaphore s_pdf_wakeup;
static struct rt_thread s_pdf_thread;
static rt_uint8_t s_pdf_thread_stack[WEB_PDF_THREAD_STACK_SIZE]
    WEB_PDF_THREAD_SECTION;
static rt_bool_t s_pdf_initialized;
static volatile rt_bool_t s_pdf_heap_ready;
static rt_bool_t s_pdf_stream_active;
static rt_uint32_t s_pdf_stream_job_id;
static rt_uint32_t s_pdf_heap_capacity;
static volatile rt_uint32_t s_pdf_error;
static volatile rt_uint32_t s_pdf_error_detail;
static web_pdf_allocator_stats_t s_allocator_stats;
static web_pdf_report_info_t s_report_info;
static web_pdf_report_info_t s_pending_report_info;
static rt_bool_t s_pending_report_valid;
static rt_uint32_t s_next_job_id;
static rt_uint32_t s_render_count;
static char s_requested_token[WEB_SNAPSHOT_TOKEN_SIZE];
static web_snapshot_pdf_view_t s_pdf_view WEB_PDF_STATIC_SECTION;
static usb_descriptor_interface_info_t
    s_pdf_interfaces[USB_DESCRIPTOR_MAX_INTERFACES] WEB_PDF_STATIC_SECTION;
static usb_descriptor_endpoint_info_t
    s_pdf_endpoints[USB_DESCRIPTOR_MAX_ENDPOINTS] WEB_PDF_STATIC_SECTION;
static rt_uint8_t s_pdf_output[WEB_PDF_CACHE_BYTES] WEB_PDF_CACHE_SECTION;

static void web_pdf_error_handler(HPDF_STATUS error_no,
                                  HPDF_STATUS detail_no,
                                  void *user_data)
{
    RT_UNUSED(user_data);
    s_pdf_error = (rt_uint32_t)error_no;
    s_pdf_error_detail = (rt_uint32_t)detail_no;
}

static void *web_pdf_alloc(HPDF_UINT requested_size)
{
    void *allocation_base;
    web_pdf_allocation_header_t *header;
    rt_uintptr_t aligned_address;
    rt_size_t payload_size = (rt_size_t)requested_size;
    rt_size_t allocation_size;

    if (!s_pdf_heap_ready ||
        payload_size > (rt_size_t)-1 - sizeof(*header) - 7U)
    {
        s_allocator_stats.allocation_failures++;
        return RT_NULL;
    }
    allocation_size = sizeof(*header) + 7U +
                      (payload_size ? payload_size : 1U);
    allocation_base = rt_memheap_alloc(&s_pdf_memheap, allocation_size);
    if (!allocation_base)
    {
        s_allocator_stats.allocation_failures++;
        return RT_NULL;
    }
    aligned_address = ((rt_uintptr_t)allocation_base +
                       sizeof(*header) + 7U) & ~(rt_uintptr_t)7U;
    header = (web_pdf_allocation_header_t *)(aligned_address - sizeof(*header));
    header->allocation_base = allocation_base;
    header->payload_size = payload_size;
    s_allocator_stats.current_bytes += (rt_uint32_t)payload_size;
    s_allocator_stats.total_bytes += (rt_uint32_t)payload_size;
    if (s_allocator_stats.current_bytes > s_allocator_stats.peak_bytes)
    {
        s_allocator_stats.peak_bytes = s_allocator_stats.current_bytes;
    }
    return (void *)(header + 1);
}

static void web_pdf_free(void *address)
{
    web_pdf_allocation_header_t *header;

    if (!address)
    {
        return;
    }
    header = ((web_pdf_allocation_header_t *)address) - 1;
    if (s_allocator_stats.current_bytes >= header->payload_size)
    {
        s_allocator_stats.current_bytes -=
            (rt_uint32_t)header->payload_size;
    }
    else
    {
        s_allocator_stats.current_bytes = 0U;
    }
    rt_memheap_free(header->allocation_base);
}

static rt_bool_t web_pdf_glyph_present(rt_uint32_t codepoint)
{
    rt_size_t index;

    if (codepoint > 0xFFFFU)
    {
        return RT_FALSE;
    }
    index = (rt_size_t)(codepoint >> 3);
    if (index >= work_web_pdf_font_coverage_length)
    {
        return RT_FALSE;
    }
    return (work_web_pdf_font_coverage[index] &
            (rt_uint8_t)(1U << (codepoint & 7U))) != 0U ?
           RT_TRUE : RT_FALSE;
}

static rt_uint8_t web_pdf_utf8_decode(const rt_uint8_t *input,
                                      rt_uint32_t *codepoint)
{
    rt_uint8_t length;
    rt_uint32_t value;
    rt_uint8_t index;

    if (!input || !codepoint || input[0] == 0U)
    {
        return 0U;
    }
    if (input[0] < 0x80U)
    {
        *codepoint = input[0];
        return 1U;
    }
    if (input[0] >= 0xC2U && input[0] <= 0xDFU)
    {
        length = 2U;
        value = input[0] & 0x1FU;
    }
    else if (input[0] >= 0xE0U && input[0] <= 0xEFU)
    {
        length = 3U;
        value = input[0] & 0x0FU;
    }
    else if (input[0] >= 0xF0U && input[0] <= 0xF4U)
    {
        length = 4U;
        value = input[0] & 0x07U;
    }
    else
    {
        *codepoint = 0xFFFDU;
        return 1U;
    }
    for (index = 1U; index < length; index++)
    {
        if (input[index] == 0U || (input[index] & 0xC0U) != 0x80U)
        {
            *codepoint = 0xFFFDU;
            return 1U;
        }
        value = (value << 6) | (input[index] & 0x3FU);
    }
    if ((length == 3U &&
         ((input[0] == 0xE0U && input[1] < 0xA0U) ||
          (input[0] == 0xEDU && input[1] >= 0xA0U))) ||
        (length == 4U &&
         ((input[0] == 0xF0U && input[1] < 0x90U) ||
          (input[0] == 0xF4U && input[1] >= 0x90U))))
    {
        *codepoint = 0xFFFDU;
        return 1U;
    }
    *codepoint = value;
    return length;
}

static void web_pdf_safe_text(const char *source, char *target,
                              rt_size_t target_size)
{
    const rt_uint8_t *cursor = (const rt_uint8_t *)(source ? source : "");
    rt_size_t used = 0U;

    if (!target || target_size == 0U)
    {
        return;
    }
    target[0] = '\0';
    while (*cursor != 0U && used + 1U < target_size)
    {
        rt_uint32_t codepoint = 0U;
        rt_uint8_t length = web_pdf_utf8_decode(cursor, &codepoint);

        if (length == 0U)
        {
            break;
        }
        if (codepoint < 0x20U || codepoint == 0x7FU)
        {
            if (used + 1U >= target_size)
            {
                break;
            }
            target[used++] = ' ';
            cursor += length;
            continue;
        }
        if (!web_pdf_glyph_present(codepoint))
        {
            char marker[20];
            int marker_length = rt_snprintf(
                marker, sizeof(marker), "[U+%04lX]",
                (unsigned long)codepoint);
            if (marker_length > 0 &&
                (rt_size_t)marker_length < target_size - used)
            {
                rt_memcpy(target + used, marker, (rt_size_t)marker_length);
                used += (rt_size_t)marker_length;
            }
            cursor += length;
            continue;
        }
        if (used + length >= target_size)
        {
            break;
        }
        rt_memcpy(target + used, cursor, length);
        used += length;
        cursor += length;
    }
    target[used] = '\0';
}

static int web_pdf_text(HPDF_Page page, HPDF_Font font, float x, float top,
                        float size, const char *text,
                        float red, float green, float blue)
{
    char safe_text[512];
    web_pdf_safe_text(text, safe_text, sizeof(safe_text));
    if (HPDF_Page_BeginText(page) != HPDF_OK ||
        HPDF_Page_SetFontAndSize(page, font, size) != HPDF_OK ||
        HPDF_Page_SetRGBFill(page, red, green, blue) != HPDF_OK ||
        HPDF_Page_MoveTextPos(page, x, WEB_PDF_PAGE_HEIGHT - top - size) !=
            HPDF_OK ||
        HPDF_Page_ShowText(page, safe_text) != HPDF_OK ||
        HPDF_Page_EndText(page) != HPDF_OK)
    {
        return -RT_ERROR;
    }
    return RT_EOK;
}

static void web_pdf_rect(HPDF_Page page, float x, float top,
                         float width, float height,
                         float red, float green, float blue)
{
    HPDF_Page_SetRGBFill(page, red, green, blue);
    HPDF_Page_Rectangle(page, x,
                        WEB_PDF_PAGE_HEIGHT - top - height,
                        width, height);
    HPDF_Page_Fill(page);
}

static void web_pdf_line(HPDF_Page page, float x1, float top1,
                         float x2, float top2,
                         float red, float green, float blue, float width)
{
    HPDF_Page_SetRGBStroke(page, red, green, blue);
    HPDF_Page_SetLineWidth(page, width);
    HPDF_Page_MoveTo(page, x1, WEB_PDF_PAGE_HEIGHT - top1);
    HPDF_Page_LineTo(page, x2, WEB_PDF_PAGE_HEIGHT - top2);
    HPDF_Page_Stroke(page);
}

static HPDF_Page web_pdf_new_page(HPDF_Doc pdf, HPDF_Font font,
                                  rt_uint32_t page_number,
                                  rt_uint32_t snapshot_id)
{
    HPDF_Page page = HPDF_AddPage(pdf);
    char footer[80];

    if (!page)
    {
        return NULL;
    }
    if (HPDF_Page_SetSize(page, HPDF_PAGE_SIZE_A4,
                          HPDF_PAGE_LANDSCAPE) != HPDF_OK)
    {
        return NULL;
    }
    web_pdf_rect(page, 0.0f, 0.0f, WEB_PDF_PAGE_WIDTH, 64.0f,
                 0.055f, 0.145f, 0.220f);
    web_pdf_rect(page, 0.0f, 61.0f, WEB_PDF_PAGE_WIDTH, 3.0f,
                 0.090f, 0.790f, 0.830f);
    if (web_pdf_text(page, font, WEB_PDF_PAGE_MARGIN, 18.0f, 17.0f,
                     "ART-Pi2 USB 枚举核验报告",
                     0.96f, 0.98f, 1.00f) != RT_EOK ||
        web_pdf_text(page, font, 600.0f, 24.0f, 8.0f,
                     "板端生成 · Host 软件观测证据",
                     0.69f, 0.83f, 0.88f) != RT_EOK)
    {
        return NULL;
    }
    web_pdf_line(page, WEB_PDF_PAGE_MARGIN, 560.0f,
                 WEB_PDF_PAGE_WIDTH - WEB_PDF_PAGE_MARGIN, 560.0f,
                 0.80f, 0.85f, 0.89f, 0.6f);
    (void)rt_snprintf(footer, sizeof(footer),
                      "ART-Pi2 · USB Host 观测证据         快照 %lu · 第 %lu 页",
                      (unsigned long)snapshot_id,
                      (unsigned long)page_number);
    if (web_pdf_text(page, font, WEB_PDF_PAGE_MARGIN, 570.0f, 7.0f,
                     footer, 0.38f, 0.44f, 0.50f) != RT_EOK)
    {
        return NULL;
    }
    return page;
}

static const char *web_pdf_session_state(usb_evidence_session_state_t state)
{
    switch (state)
    {
    case USB_EVIDENCE_SESSION_WAITING: return "等待";
    case USB_EVIDENCE_SESSION_RECORDING: return "采集中";
    case USB_EVIDENCE_SESSION_ANALYZING: return "分析中";
    case USB_EVIDENCE_SESSION_FINAL: return "已完成";
    case USB_EVIDENCE_SESSION_DEGRADED: return "证据不完整";
    case USB_EVIDENCE_SESSION_FAILED: return "失败";
    default: return "未知";
    }
}

static const char *web_pdf_integrity(usb_evidence_integrity_t integrity)
{
    switch (integrity)
    {
    case USB_EVIDENCE_INTEGRITY_COMPLETE: return "完整";
    case USB_EVIDENCE_INTEGRITY_DEGRADED: return "降级";
    default: return "不可用";
    }
}

static const char *web_pdf_verdict(usb_rule_verdict_t verdict)
{
    switch (verdict)
    {
    case USB_RULE_PASS: return "通过";
    case USB_RULE_FAIL: return "失败";
    case USB_RULE_NOT_APPLICABLE: return "不适用";
    default: return "未观察";
    }
}

static const char *web_pdf_capture_status(
    usb_evidence_capture_status_t status)
{
    switch (status)
    {
    case USB_EVIDENCE_CAPTURE_COMPLETE: return "完整";
    case USB_EVIDENCE_CAPTURE_LOCAL_TRUNCATED: return "本地截断";
    case USB_EVIDENCE_CAPTURE_NO_DATA_STAGE: return "无数据阶段";
    case USB_EVIDENCE_CAPTURE_UNAVAILABLE: return "不可用";
    default: return "无返回数据";
    }
}

static const char *web_pdf_event_name(usb_evidence_event_type_t event_type)
{
    switch (event_type)
    {
    case USB_EVIDENCE_EVENT_PORT_RESET: return "PORT_RESET";
    case USB_EVIDENCE_EVENT_CONTROL_TRANSFER: return "CONTROL_TRANSFER";
    case USB_EVIDENCE_EVENT_ENUMERATION_COMPLETE:
        return "ENUMERATION_COMPLETE";
    case USB_EVIDENCE_EVENT_DISCONNECT: return "DISCONNECT";
    default: return "UNKNOWN";
    }
}

static const char *web_pdf_request_name(
    const web_snapshot_pdf_event_t *event)
{
    rt_uint8_t descriptor_type;

    if (!event || !event->has_setup)
    {
        if (event && event->event_type == USB_EVIDENCE_EVENT_PORT_RESET)
            return "端口复位";
        if (event &&
            event->event_type == USB_EVIDENCE_EVENT_ENUMERATION_COMPLETE)
            return "枚举结论";
        if (event && event->event_type == USB_EVIDENCE_EVENT_DISCONNECT)
            return "设备断开";
        return "无控制请求";
    }
    switch (event->setup_packet.b_request)
    {
    case 5U: return "SET_ADDRESS · 设置地址";
    case 9U: return "SET_CONFIGURATION · 选择配置";
    case 10U: return "GET_INTERFACE · 读取接口";
    case 6U:
        descriptor_type = (rt_uint8_t)(event->setup_packet.w_value >> 8);
        switch (descriptor_type)
        {
        case 1U: return "GET_DESCRIPTOR · 设备描述符";
        case 2U: return "GET_DESCRIPTOR · 配置描述符";
        case 3U: return "GET_DESCRIPTOR · 字符串描述符";
        case 0x22U: return "GET_DESCRIPTOR · HID Report";
        default: return "GET_DESCRIPTOR · 其他描述符";
        }
    default: return "控制传输请求";
    }
}

static int web_pdf_draw_summary(HPDF_Doc pdf, HPDF_Font font,
                                const web_snapshot_pdf_view_t *view,
                                rt_uint32_t page_number)
{
    HPDF_Page page = web_pdf_new_page(pdf, font, page_number,
                                      view->snapshot_id);
    char line[256];
    char device[128];
    rt_uint32_t pass = 0U;
    rt_uint32_t fail = 0U;
    rt_uint32_t not_observed = 0U;
    rt_uint32_t index;
    const float right_x = 432.0f;

    if (!page)
    {
        return -RT_ERROR;
    }
    for (index = 0U; index < USB_EVIDENCE_RULE_COUNT; index++)
    {
        if (view->rules[index].verdict == USB_RULE_PASS)
            pass++;
        else if (view->rules[index].verdict == USB_RULE_FAIL)
            fail++;
        else if (view->rules[index].verdict == USB_RULE_NOT_OBSERVED)
            not_observed++;
    }
    (void)rt_snprintf(device, sizeof(device), "%s",
                      view->device_present ? view->product : "未观察到设备");
    (void)rt_snprintf(line, sizeof(line), "%s · 0x%04X:0x%04X",
                      device, view->vendor_id, view->product_id);
    if (web_pdf_text(page, font, WEB_PDF_PAGE_MARGIN, 78.0f, 11.0f,
                     "一次 HID 枚举，留下可复核的证据链",
                     0.07f, 0.21f, 0.30f) != RT_EOK ||
        web_pdf_text(page, font, WEB_PDF_PAGE_MARGIN, 97.0f, 8.0f,
                     "报告由开发板使用冻结的 USB Host 证据快照生成；时间为软件观测。",
                     0.32f, 0.40f, 0.47f) != RT_EOK)
    {
        return -RT_ERROR;
    }

#define WEB_PDF_CARD(x, y, title, value) \
    do { \
        web_pdf_rect(page, (x), (y), 374.0f, 38.0f, \
                     0.94f, 0.965f, 0.976f); \
        web_pdf_rect(page, (x), (y), 3.0f, 38.0f, \
                     0.09f, 0.79f, 0.83f); \
        if (web_pdf_text(page, font, (x) + 12.0f, (y) + 5.0f, 7.5f, \
                         (title), 0.29f, 0.39f, 0.47f) != RT_EOK || \
            web_pdf_text(page, font, (x) + 12.0f, (y) + 18.0f, 9.0f, \
                         (value), 0.08f, 0.17f, 0.23f) != RT_EOK) \
            return -RT_ERROR; \
    } while (0)

    WEB_PDF_CARD(36.0f, 122.0f, "设备 / VID:PID", line);
    (void)rt_snprintf(line, sizeof(line), "%s / %s",
                      web_pdf_session_state(view->session_state),
                      web_pdf_integrity(view->capture_integrity));
    WEB_PDF_CARD(right_x, 122.0f, "会话状态 / 证据完整性", line);
    (void)rt_snprintf(line, sizeof(line), "%u 条事件 · %u 项规则",
                      view->event_count, USB_EVIDENCE_RULE_COUNT);
    WEB_PDF_CARD(36.0f, 168.0f, "Host 事件 / 规则", line);
    (void)rt_snprintf(line, sizeof(line), "%lu 通过 · %lu 失败 · %lu 未观察",
                      (unsigned long)pass, (unsigned long)fail,
                      (unsigned long)not_observed);
    WEB_PDF_CARD(right_x, 168.0f, "规则结论", line);
    (void)rt_snprintf(line, sizeof(line), "%s · JSON CRC32 0x%08lX",
                      view->token, (unsigned long)view->json_crc32);
    WEB_PDF_CARD(36.0f, 214.0f, "快照代号 / JSON 摘要", line);
    (void)rt_snprintf(line, sizeof(line), "0x%08lX · 序列号已遮盖",
                      (unsigned long)view->content_crc32);
    WEB_PDF_CARD(right_x, 214.0f, "内容 CRC32 / 隐私", line);
#undef WEB_PDF_CARD

    if (web_pdf_text(page, font, WEB_PDF_PAGE_MARGIN, 272.0f, 11.0f,
                     "八阶段枚举时间线",
                     0.07f, 0.21f, 0.30f) != RT_EOK)
    {
        return -RT_ERROR;
    }
    web_pdf_rect(page, WEB_PDF_PAGE_MARGIN, 292.0f,
                 WEB_PDF_PAGE_WIDTH - WEB_PDF_PAGE_MARGIN * 2.0f,
                 22.0f, 0.08f, 0.19f, 0.27f);
    (void)web_pdf_text(page, font, 46.0f, 298.0f, 7.5f,
                       "阶段", 0.93f, 0.97f, 0.99f);
    (void)web_pdf_text(page, font, 340.0f, 298.0f, 7.5f,
                       "状态", 0.93f, 0.97f, 0.99f);
    (void)web_pdf_text(page, font, 465.0f, 298.0f, 7.5f,
                       "来源事件", 0.93f, 0.97f, 0.99f);
    (void)web_pdf_text(page, font, 600.0f, 298.0f, 7.5f,
                       "Host 软件观测时间", 0.93f, 0.97f, 0.99f);
    for (index = 0U; index < USB_EVIDENCE_TIMELINE_COUNT; index++)
    {
        const usb_evidence_timeline_item_t *stage = &view->timeline[index];
        char event[24];
        char time_text[48];
        float top = 314.0f + (float)index * 25.0f;
        const char *status = stage->status == USB_EVIDENCE_TIMELINE_OBSERVED ?
                             "已观察" :
                             (stage->status == USB_EVIDENCE_TIMELINE_FAILED ?
                              "失败" : "未观察");

        if (index & 1U)
        {
            web_pdf_rect(page, WEB_PDF_PAGE_MARGIN, top,
                         WEB_PDF_PAGE_WIDTH - WEB_PDF_PAGE_MARGIN * 2.0f,
                         25.0f, 0.96f, 0.975f, 0.985f);
        }
        (void)rt_snprintf(event, sizeof(event), "#%lu",
                          (unsigned long)stage->event_sequence);
        if (stage->timestamp_us == 0U)
        {
            rt_strncpy(time_text, "未记录", sizeof(time_text) - 1U);
            time_text[sizeof(time_text) - 1U] = '\0';
        }
        else
        {
            (void)rt_snprintf(time_text, sizeof(time_text), "%lu μs（Host）",
                              (unsigned long)stage->timestamp_us);
        }
        if (web_pdf_text(page, font, 46.0f, top + 7.0f, 8.0f,
                         stage->label, 0.12f, 0.20f, 0.26f) != RT_EOK ||
            web_pdf_text(page, font, 340.0f, top + 7.0f, 8.0f,
                         status, stage->status == USB_EVIDENCE_TIMELINE_FAILED ?
                         0.75f : 0.05f, 0.20f, 0.26f) != RT_EOK ||
            web_pdf_text(page, font, 465.0f, top + 7.0f, 8.0f,
                         event, 0.12f, 0.20f, 0.26f) != RT_EOK ||
            web_pdf_text(page, font, 600.0f, top + 7.0f, 8.0f,
                         time_text, 0.12f, 0.20f, 0.26f) != RT_EOK)
        {
            return -RT_ERROR;
        }
        web_pdf_line(page, WEB_PDF_PAGE_MARGIN, top + 25.0f,
                     WEB_PDF_PAGE_WIDTH - WEB_PDF_PAGE_MARGIN,
                     top + 25.0f, 0.84f, 0.88f, 0.91f, 0.35f);
    }
    (void)web_pdf_text(page, font, WEB_PDF_PAGE_MARGIN, 529.0f, 7.2f,
                       "序列号已遮盖；报告不包含完整原始负载。原始字节请使用完整证据 JSON 导出。",
                       0.34f, 0.42f, 0.48f);
    return RT_EOK;
}

static int web_pdf_draw_rules(HPDF_Doc pdf, HPDF_Font font,
                              const web_snapshot_pdf_view_t *view,
                              rt_uint32_t page_number)
{
    HPDF_Page page = web_pdf_new_page(pdf, font, page_number,
                                      view->snapshot_id);
    rt_uint32_t index;

    if (!page ||
        web_pdf_text(page, font, WEB_PDF_PAGE_MARGIN, 82.0f, 11.0f,
                     "十二项规则判定与证据引用",
                     0.07f, 0.21f, 0.30f) != RT_EOK ||
        web_pdf_text(page, font, WEB_PDF_PAGE_MARGIN, 101.0f, 8.0f,
                     "每项判定保留本次捕获的期望、实测和来源字节范围。",
                     0.32f, 0.40f, 0.47f) != RT_EOK)
    {
        return -RT_ERROR;
    }
    for (index = 0U; index < USB_EVIDENCE_RULE_COUNT; index++)
    {
        const usb_evidence_rule_result_t *rule = &view->rules[index];
        char title[96];
        char detail[256];
        rt_uint32_t ref_index;
        rt_size_t detail_used = 0U;
        float top = 125.0f + (float)index * 32.0f;

        (void)rt_snprintf(title, sizeof(title), "%s  ·  %s",
                          rule->rule_id ? rule->rule_id : "RULE",
                          web_pdf_verdict(rule->verdict));
        (void)rt_snprintf(detail, sizeof(detail), "期望：%s  实测：%s",
                          rule->expected, rule->observed);
        detail_used = rt_strlen(detail);
        for (ref_index = 0U;
             ref_index < rule->evidence_ref_count &&
             ref_index < USB_EVIDENCE_REFERENCE_COUNT;
             ref_index++)
        {
            const usb_evidence_reference_t *reference =
                &rule->evidence_refs[ref_index];
            int added = rt_snprintf(detail + detail_used,
                                    sizeof(detail) - detail_used,
                                    "  ·  #%lu +%u:%u",
                                    (unsigned long)reference->event_sequence,
                                    reference->byte_offset,
                                    reference->byte_length);
            if (added <= 0 ||
                (rt_size_t)added >= sizeof(detail) - detail_used)
            {
                break;
            }
            detail_used += (rt_size_t)added;
        }
        if (index & 1U)
        {
            web_pdf_rect(page, WEB_PDF_PAGE_MARGIN, top,
                         WEB_PDF_PAGE_WIDTH - WEB_PDF_PAGE_MARGIN * 2.0f,
                         32.0f, 0.96f, 0.975f, 0.985f);
        }
        if (web_pdf_text(page, font, 46.0f, top + 4.0f, 8.2f,
                         title, 0.06f, 0.46f, 0.50f) != RT_EOK ||
            web_pdf_text(page, font, 130.0f, top + 17.0f, 7.2f,
                         rule->spec_clause, 0.32f, 0.40f, 0.47f) != RT_EOK ||
            web_pdf_text(page, font, 345.0f, top + 17.0f, 7.2f,
                         detail, 0.22f, 0.30f, 0.37f) != RT_EOK)
        {
            return -RT_ERROR;
        }
        web_pdf_line(page, WEB_PDF_PAGE_MARGIN, top + 32.0f,
                     WEB_PDF_PAGE_WIDTH - WEB_PDF_PAGE_MARGIN,
                     top + 32.0f, 0.84f, 0.88f, 0.91f, 0.35f);
    }
    return RT_EOK;
}

static int web_pdf_draw_event_page(HPDF_Doc pdf, HPDF_Font font,
                                   const web_snapshot_pdf_view_t *view,
                                   rt_uint32_t page_number,
                                   rt_uint32_t first_event,
                                   rt_uint32_t end_event)
{
    HPDF_Page page = web_pdf_new_page(pdf, font, page_number,
                                      view->snapshot_id);
    rt_uint32_t index;

    if (!page ||
        web_pdf_text(page, font, WEB_PDF_PAGE_MARGIN, 82.0f, 11.0f,
                     "Host 事件记录",
                     0.07f, 0.21f, 0.30f) != RT_EOK ||
        web_pdf_text(page, font, WEB_PDF_PAGE_MARGIN, 101.0f, 8.0f,
                     "保留请求、设备返回、捕获长度和 Host 软件观测耗时；未复制原始负载。",
                     0.32f, 0.40f, 0.47f) != RT_EOK)
    {
        return -RT_ERROR;
    }
    for (index = first_event; index < end_event; index++)
    {
        const web_snapshot_pdf_event_t *event = &view->events[index];
        char event_title[80];
        char request[256];
        char result[192];
        float top = 126.0f + (float)(index - first_event) * 64.0f;
        const char *capture = web_pdf_capture_status(event->capture_status);

        (void)rt_snprintf(event_title, sizeof(event_title), "#%lu  %s",
                          (unsigned long)event->sequence,
                          web_pdf_event_name(event->event_type));
        if (event->has_setup)
        {
            (void)rt_snprintf(
                request, sizeof(request),
                "请求：%s · bmRequestType 0x%02X · bRequest 0x%02X · "
                "wValue 0x%04X · wIndex 0x%04X · wLength %u",
                web_pdf_request_name(event),
                event->setup_packet.bm_request_type,
                event->setup_packet.b_request,
                event->setup_packet.w_value,
                event->setup_packet.w_index,
                event->setup_packet.w_length);
        }
        else
        {
            (void)rt_snprintf(request, sizeof(request), "请求：%s",
                              web_pdf_request_name(event));
        }
        (void)rt_snprintf(
            result, sizeof(result),
            "返回 %ld · 捕获 %s %u/%u B · Host 软件观测 %s",
            (long)event->return_status, capture,
            event->captured_length,
            event->transfer_length_known ? event->transfer_length :
                                           event->captured_length,
            event->timing_valid ?
                "" : "未成对采集");
        if (event->timing_valid)
        {
            rt_size_t used = rt_strlen(result);
            if (used < sizeof(result))
            {
                (void)rt_snprintf(result + used, sizeof(result) - used,
                                  "%lu μs",
                                  (unsigned long)event->duration_us);
            }
        }
        if ((index - first_event) & 1U)
        {
            web_pdf_rect(page, WEB_PDF_PAGE_MARGIN, top,
                         WEB_PDF_PAGE_WIDTH - WEB_PDF_PAGE_MARGIN * 2.0f,
                         64.0f, 0.96f, 0.975f, 0.985f);
        }
        if (web_pdf_text(page, font, 46.0f, top + 5.0f, 8.0f,
                         event_title, 0.06f, 0.46f, 0.50f) != RT_EOK ||
            web_pdf_text(page, font, 46.0f, top + 21.0f, 7.2f,
                         request, 0.12f, 0.20f, 0.26f) != RT_EOK ||
            web_pdf_text(page, font, 46.0f, top + 39.0f, 7.2f,
                         result, 0.32f, 0.40f, 0.47f) != RT_EOK)
        {
            return -RT_ERROR;
        }
        web_pdf_line(page, WEB_PDF_PAGE_MARGIN, top + 64.0f,
                     WEB_PDF_PAGE_WIDTH - WEB_PDF_PAGE_MARGIN,
                     top + 52.0f, 0.84f, 0.88f, 0.91f, 0.35f);
    }
    return RT_EOK;
}

static rt_bool_t web_pdf_topology_is_consistent(
    const usb_descriptor_manifest_t *before,
    const usb_descriptor_manifest_t *after,
    const web_snapshot_pdf_view_t *view,
    rt_size_t interface_count,
    rt_size_t endpoint_count,
    rt_uint32_t expected_generation)
{
    if (!before || !after || !view || !before->present ||
        before->generation != after->generation ||
        (expected_generation != 0U &&
         before->generation != expected_generation) ||
        before->interface_count != after->interface_count ||
        before->endpoint_count != after->endpoint_count ||
        interface_count != before->interface_count ||
        endpoint_count != before->endpoint_count ||
        !view->device_present || before->vendor_id != view->vendor_id ||
        before->product_id != view->product_id ||
        before->speed != view->speed || before->bcd_usb != view->bcd_usb ||
        before->bcd_device != view->bcd_device)
    {
        return RT_FALSE;
    }
    if (before->manufacturer[0] && view->manufacturer[0] &&
        rt_strcmp(before->manufacturer, view->manufacturer) != 0)
    {
        return RT_FALSE;
    }
    if (before->product[0] && view->product[0] &&
        rt_strcmp(before->product, view->product) != 0)
    {
        return RT_FALSE;
    }
    return RT_TRUE;
}

static int web_pdf_draw_topology_page(
    HPDF_Doc pdf, HPDF_Font font, const web_snapshot_pdf_view_t *view,
    const usb_descriptor_manifest_t *manifest,
    rt_size_t interface_count, rt_size_t endpoint_count,
    rt_uint32_t first_interface, rt_uint32_t end_interface,
    rt_uint32_t page_number)
{
    HPDF_Page page = web_pdf_new_page(pdf, font, page_number,
                                      view->snapshot_id);
    rt_uint32_t index;
    char heading[160];

    if (!page)
    {
        return -RT_ERROR;
    }
    (void)rt_snprintf(heading, sizeof(heading),
                      "导出时 USB 实时拓扑 · generation %lu",
                      (unsigned long)manifest->generation);
    if (web_pdf_text(page, font, WEB_PDF_PAGE_MARGIN, 82.0f, 11.0f,
                     heading, 0.07f, 0.21f, 0.30f) != RT_EOK)
    {
        return -RT_ERROR;
    }
    (void)rt_snprintf(heading, sizeof(heading),
                      "%lu 个接口 · %lu 个端点 · 拓扑 generation 与证据快照代号分开核对",
                      (unsigned long)interface_count,
                      (unsigned long)endpoint_count);
    if (web_pdf_text(page, font, WEB_PDF_PAGE_MARGIN, 101.0f, 8.0f,
                     heading, 0.32f, 0.40f, 0.47f) != RT_EOK)
    {
        return -RT_ERROR;
    }
    for (index = first_interface; index < end_interface; index++)
    {
        const usb_descriptor_interface_info_t *interface =
            &s_pdf_interfaces[index];
        char title[144];
        char details[256];
        rt_uint32_t endpoint_index;
        rt_size_t used = 0U;
        float top = 126.0f + (float)(index - first_interface) * 26.0f;

        (void)rt_snprintf(title, sizeof(title),
                          "接口 %u.%u · class 0x%02X / 0x%02X / 0x%02X",
                          interface->interface_number,
                          interface->alternate_setting,
                          interface->class_code, interface->subclass,
                          interface->protocol);
        (void)rt_snprintf(details, sizeof(details),
                          "Host 驱动：%s%s",
                          interface->driver_name[0] ?
                              interface->driver_name : "未报告",
                          interface->class_bound ? "（已绑定）" :
                          (interface->class_bind_failed ? "（绑定失败）" :
                                                          "（未报告）"));
        used = rt_strlen(details);
        for (endpoint_index = 0U; endpoint_index < endpoint_count;
             endpoint_index++)
        {
            const usb_descriptor_endpoint_info_t *endpoint =
                &s_pdf_endpoints[endpoint_index];
            int added;
            if (endpoint->config_index != interface->config_index ||
                endpoint->interface_number != interface->interface_number ||
                endpoint->alternate_setting != interface->alternate_setting)
            {
                continue;
            }
            added = rt_snprintf(details + used, sizeof(details) - used,
                                " · EP 0x%02X MPS %u interval %u",
                                endpoint->address,
                                endpoint->max_packet_size,
                                endpoint->interval);
            if (added <= 0 || (rt_size_t)added >= sizeof(details) - used)
            {
                break;
            }
            used += (rt_size_t)added;
        }
        if (web_pdf_text(page, font, 46.0f, top + 3.0f, 8.0f,
                         title, 0.06f, 0.46f, 0.50f) != RT_EOK ||
            web_pdf_text(page, font, 46.0f, top + 14.0f, 7.0f,
                         details, 0.28f, 0.37f, 0.44f) != RT_EOK)
        {
            return -RT_ERROR;
        }
        web_pdf_line(page, WEB_PDF_PAGE_MARGIN, top + 26.0f,
                     WEB_PDF_PAGE_WIDTH - WEB_PDF_PAGE_MARGIN,
                     top + 26.0f, 0.84f, 0.88f, 0.91f, 0.35f);
    }
    (void)web_pdf_text(page, font, WEB_PDF_PAGE_MARGIN, 535.0f, 7.2f,
                       "该附录是 Host 读取的描述符声明与驱动绑定状态，不证明 HID 输入报告收发或硬件根因。",
                       0.34f, 0.42f, 0.48f);
    return RT_EOK;
}

static int web_pdf_draw_topology_unavailable(
    HPDF_Doc pdf, HPDF_Font font, const web_snapshot_pdf_view_t *view,
    rt_uint32_t page_number)
{
    HPDF_Page page = web_pdf_new_page(pdf, font, page_number,
                                      view->snapshot_id);
    if (!page ||
        web_pdf_text(page, font, WEB_PDF_PAGE_MARGIN, 82.0f, 11.0f,
                     "未附加实时 USB 拓扑",
                     0.68f, 0.38f, 0.08f) != RT_EOK ||
        web_pdf_text(page, font, WEB_PDF_PAGE_MARGIN, 106.0f, 8.0f,
                     "设备代次变化、清单不完整或设备身份与冻结证据不一致，"
                     "因此没有把拓扑数据混入本报告。",
                     0.32f, 0.40f, 0.47f) != RT_EOK)
    {
        return -RT_ERROR;
    }
    return RT_EOK;
}

static int web_pdf_draw_build(HPDF_Doc pdf, HPDF_Font font,
                              const web_snapshot_pdf_view_t *view,
                              rt_uint32_t page_number)
{
    HPDF_Page page = web_pdf_new_page(pdf, font, page_number,
                                      view->snapshot_id);
    char fingerprint[80];
    char profile[128];
    if (!page ||
        web_pdf_text(page, font, WEB_PDF_PAGE_MARGIN, 82.0f, 11.0f,
                     "构建身份与报告边界",
                     0.07f, 0.21f, 0.30f) != RT_EOK)
    {
        return -RT_ERROR;
    }
    (void)rt_snprintf(fingerprint, sizeof(fingerprint), "%s",
                      WORK_SOURCE_FINGERPRINT);
    (void)rt_snprintf(profile, sizeof(profile), "%s / %s",
                      WORK_BUILD_PROFILE, WORK_BUILD_TYPE);
    (void)web_pdf_text(page, font, 46.0f, 122.0f, 8.0f,
                       "源码 / 配置指纹", 0.29f, 0.39f, 0.47f);
    (void)web_pdf_text(page, font, 210.0f, 122.0f, 8.0f,
                       fingerprint, 0.08f, 0.17f, 0.23f);
    (void)web_pdf_text(page, font, 46.0f, 146.0f, 8.0f,
                       "预设 / 构建类型", 0.29f, 0.39f, 0.47f);
    (void)web_pdf_text(page, font, 210.0f, 146.0f, 8.0f,
                       profile, 0.08f, 0.17f, 0.23f);
    (void)web_pdf_text(page, font, 46.0f, 170.0f, 8.0f,
                       "编译器", 0.29f, 0.39f, 0.47f);
    (void)web_pdf_text(page, font, 210.0f, 170.0f, 8.0f,
                       WORK_BUILD_COMPILER, 0.08f, 0.17f, 0.23f);
    (void)web_pdf_text(page, font, 46.0f, 194.0f, 8.0f,
                       "RT-Thread / LVGL", 0.29f, 0.39f, 0.47f);
    (void)web_pdf_text(page, font, 210.0f, 194.0f, 8.0f,
                       WORK_BUILD_RTTHREAD_VERSION " / "
                       WORK_BUILD_LVGL_VERSION,
                       0.08f, 0.17f, 0.23f);
    (void)web_pdf_text(page, font, WEB_PDF_PAGE_MARGIN, 250.0f, 8.0f,
                       "时间线仅代表 USB Host 软件观测，不是 USB 总线分析仪测量。"
                       "规则结论只描述本次捕获证据，不构成全面合规认证或硬件根因诊断。",
                       0.34f, 0.42f, 0.48f);
    (void)web_pdf_text(page, font, WEB_PDF_PAGE_MARGIN, 280.0f, 8.0f,
                       "设备序列号已遮盖；本 PDF 不包含原始传输负载。"
                       "原始字节请通过完整证据 JSON 按快照代号与 CRC 复核。",
                       0.34f, 0.42f, 0.48f);
    return RT_EOK;
}

static int web_pdf_build(HPDF_Doc pdf, HPDF_Font font,
                         const web_snapshot_pdf_view_t *view,
                         rt_uint32_t expected_topology_generation)
{
    rt_uint32_t page_number = 1U;
    rt_uint32_t event_index;
    rt_uint32_t page_start;
    rt_bool_t topology_available = RT_FALSE;
    rt_size_t interface_count = 0U;
    rt_size_t endpoint_count = 0U;
    usb_descriptor_manifest_t before;
    usb_descriptor_manifest_t after;

    if (web_pdf_draw_summary(pdf, font, view, page_number++) != RT_EOK ||
        web_pdf_draw_rules(pdf, font, view, page_number++) != RT_EOK)
    {
        return -RT_ERROR;
    }
    for (event_index = 0U; event_index < view->event_count; )
    {
        rt_uint32_t event_end = event_index + 6U;
        if (event_end > view->event_count)
        {
            event_end = view->event_count;
        }
        if (web_pdf_draw_event_page(pdf, font, view, page_number++,
                                    event_index, event_end) != RT_EOK)
        {
            return -RT_ERROR;
        }
        event_index = event_end;
    }

    if (usb_descriptor_store_get_manifest(&before))
    {
        interface_count = usb_descriptor_store_copy_interfaces(
            s_pdf_interfaces, USB_DESCRIPTOR_MAX_INTERFACES);
        endpoint_count = usb_descriptor_store_copy_endpoints(
            s_pdf_endpoints, USB_DESCRIPTOR_MAX_ENDPOINTS);
        if (usb_descriptor_store_get_manifest(&after) &&
            web_pdf_topology_is_consistent(&before, &after, view,
                                           interface_count, endpoint_count,
                                           expected_topology_generation))
        {
            rt_uint32_t topology_start = 0U;
            const rt_uint32_t rows_per_page = 14U;
            topology_available = RT_TRUE;
            if (interface_count == 0U &&
                web_pdf_draw_topology_page(
                    pdf, font, view, &after, interface_count,
                    endpoint_count, 0U, 0U, page_number++) != RT_EOK)
            {
                return -RT_ERROR;
            }
            while (topology_start < interface_count)
            {
                rt_uint32_t topology_end = topology_start + rows_per_page;
                if (topology_end > interface_count)
                {
                    topology_end = interface_count;
                }
                if (web_pdf_draw_topology_page(
                        pdf, font, view, &after, interface_count,
                        endpoint_count, topology_start, topology_end,
                        page_number++) != RT_EOK)
                {
                    return -RT_ERROR;
                }
                topology_start = topology_end;
            }
        }
    }
    if (!topology_available &&
        web_pdf_draw_topology_unavailable(pdf, font, view,
                                          page_number++) != RT_EOK)
    {
        return -RT_ERROR;
    }
    page_start = page_number;
    if (web_pdf_draw_build(pdf, font, view, page_start) != RT_EOK)
    {
        return -RT_ERROR;
    }
    return RT_EOK;
}

static int web_pdf_write_stream(HPDF_Doc pdf, rt_uint32_t *output_length)
{
    HPDF_BYTE chunk[WEB_PDF_REPORT_STREAM_BUFFER_SIZE];
    HPDF_UINT32 pdf_size = HPDF_GetStreamSize(pdf);
    HPDF_UINT32 remaining = pdf_size;
    rt_uint32_t copied = 0U;

    if (pdf_size == 0U || pdf_size > sizeof(s_pdf_output) ||
        HPDF_ResetStream(pdf) != HPDF_OK)
    {
        return -RT_ENOSPC;
    }
    while (remaining != 0U)
    {
        HPDF_UINT32 requested = remaining < sizeof(chunk) ?
                                remaining : sizeof(chunk);
        HPDF_UINT32 received = requested;
        HPDF_STATUS status = HPDF_ReadFromStream(pdf, chunk, &received);
        if (status != HPDF_OK || received == 0U ||
            received > requested || received > sizeof(s_pdf_output) - copied)
        {
            return -RT_ERROR;
        }
        rt_memcpy(s_pdf_output + copied, chunk, received);
        copied += received;
        remaining -= received;
    }
    *output_length = copied;
    return RT_EOK;
}

static int web_pdf_render_job(const char *token,
                             rt_uint32_t expected_topology_generation,
                             rt_uint32_t *pdf_bytes,
                             rt_uint32_t *pdf_crc32)
{
    HPDF_Doc pdf = NULL;
    HPDF_Font font;
    const char *font_name;
    int result;

    rt_memset(&s_allocator_stats, 0, sizeof(s_allocator_stats));
    s_pdf_error = HPDF_OK;
    s_pdf_error_detail = 0U;
    result = web_snapshot_get_pdf_view(token, &s_pdf_view);
    if (result != RT_EOK)
    {
        return result;
    }

    pdf = HPDF_NewEx(web_pdf_error_handler, web_pdf_alloc, web_pdf_free,
                     0U, RT_NULL);
    if (!pdf)
    {
        return -RT_ENOMEM;
    }
    if (HPDF_UseUTFEncodings(pdf) != HPDF_OK)
    {
        result = -RT_ERROR;
        goto cleanup;
    }
    font_name = HPDF_LoadTTFontFromMemory(
        pdf, work_web_pdf_font, (HPDF_UINT)work_web_pdf_font_length,
        HPDF_TRUE);
    if (!font_name || s_pdf_error != HPDF_OK)
    {
        result = -RT_ERROR;
        goto cleanup;
    }
    font = HPDF_GetFont(pdf, font_name, "UTF-8");
    if (!font || s_pdf_error != HPDF_OK)
    {
        result = -RT_ERROR;
        goto cleanup;
    }
    if (HPDF_SetInfoAttr(pdf, HPDF_INFO_TITLE,
                         "ART-Pi2 USB Enumeration Verification") != HPDF_OK ||
        HPDF_SetInfoAttr(pdf, HPDF_INFO_AUTHOR,
                         "ART-Pi2 USB Diagnostics") != HPDF_OK ||
        web_pdf_build(pdf, font, &s_pdf_view,
                      expected_topology_generation) != RT_EOK ||
        s_pdf_error != HPDF_OK)
    {
        result = -RT_ERROR;
        goto cleanup;
    }
    if (HPDF_SaveToStream(pdf) != HPDF_OK || s_pdf_error != HPDF_OK)
    {
        result = -RT_ERROR;
        goto cleanup;
    }
    result = web_pdf_write_stream(pdf, pdf_bytes);
    if (result != RT_EOK)
    {
        goto cleanup;
    }
    *pdf_crc32 = usb_evidence_bytes_crc32(s_pdf_output, *pdf_bytes);

cleanup:
    if (pdf)
    {
        HPDF_Free(pdf);
    }
    if (result == RT_EOK &&
        (s_allocator_stats.current_bytes != 0U ||
         s_allocator_stats.allocation_failures != 0U))
    {
        result = -RT_ERROR;
    }
    return result;
}

static rt_uint32_t web_pdf_thread_stack_used(void)
{
    const rt_uint8_t *stack = (const rt_uint8_t *)s_pdf_thread.stack_addr;
    rt_size_t used_prefix = 0U;

    if (!stack || s_pdf_thread.stack_size == 0U)
    {
        return 0U;
    }
    while (used_prefix < s_pdf_thread.stack_size &&
           stack[used_prefix] == '#')
    {
        used_prefix++;
    }
    return (rt_uint32_t)(s_pdf_thread.stack_size - used_prefix);
}

static rt_uint32_t web_pdf_next_job_id_locked(void)
{
    s_next_job_id++;
    if (s_next_job_id == 0U)
    {
        s_next_job_id++;
    }
    return s_next_job_id;
}

static void web_pdf_set_job_info(web_pdf_report_info_t *info,
                                const char *token,
                                rt_uint32_t topology_generation,
                                rt_bool_t pre_generated,
                                rt_uint32_t job_id)
{
    rt_memset(info, 0, sizeof(*info));
    info->job_id = job_id;
    info->state = WEB_PDF_REPORT_QUEUED;
    info->heap_capacity_bytes = s_pdf_heap_capacity;
    info->stack_capacity_bytes = sizeof(s_pdf_thread_stack);
    info->topology_generation = topology_generation;
    info->pre_generated = pre_generated;
    rt_strncpy(info->evidence_token, token,
               sizeof(info->evidence_token) - 1U);
    info->evidence_token[sizeof(info->evidence_token) - 1U] = '\0';
}

static void web_pdf_worker_entry(void *parameter)
{
    RT_UNUSED(parameter);
    rt_uint32_t watchdog_progress = 0U;
    while (1)
    {
        rt_uint32_t job_id;
        rt_uint32_t topology_generation;
        char token[WEB_SNAPSHOT_TOKEN_SIZE];
        rt_uint32_t pdf_bytes = 0U;
        rt_uint32_t pdf_crc32 = 0U;
        rt_size_t memheap_total = 0U;
        rt_size_t memheap_used = 0U;
        rt_size_t memheap_max_used = 0U;
        rt_bool_t start_pending = RT_FALSE;
        int result;

        if (rt_sem_take(&s_pdf_wakeup, RT_WAITING_FOREVER) != RT_EOK)
        {
            continue;
        }
        if (rt_mutex_take(&s_pdf_lock, RT_WAITING_FOREVER) != RT_EOK)
        {
            continue;
        }
        if (s_report_info.state != WEB_PDF_REPORT_QUEUED)
        {
            rt_mutex_release(&s_pdf_lock);
            continue;
        }
        s_report_info.state = WEB_PDF_REPORT_RENDERING;
        job_id = s_report_info.job_id;
        s_render_count++;
        if (s_render_count == 0U) s_render_count++;
        s_report_info.render_count = s_render_count;
        topology_generation = s_report_info.topology_generation;
        rt_strncpy(token, s_requested_token, sizeof(token) - 1U);
        token[sizeof(token) - 1U] = '\0';
        rt_mutex_release(&s_pdf_lock);

        watchdog_supervisor_component_progress(
            WATCHDOG_COMPONENT_PDF_WORKER, WATCHDOG_COMPONENT_STAGE_ACTIVE,
            job_id);

        result = web_pdf_render_job(token, topology_generation,
                                    &pdf_bytes, &pdf_crc32);
        rt_memheap_info(&s_pdf_memheap, &memheap_total,
                        &memheap_used, &memheap_max_used);

        if (rt_mutex_take(&s_pdf_lock, RT_WAITING_FOREVER) != RT_EOK)
        {
            continue;
        }
        if (s_report_info.job_id == job_id &&
            s_report_info.state == WEB_PDF_REPORT_RENDERING)
        {
            s_report_info.heap_peak_bytes = s_allocator_stats.peak_bytes;
            s_report_info.heap_live_bytes = s_allocator_stats.current_bytes;
            s_report_info.heap_used_bytes = (rt_uint32_t)memheap_used;
            s_report_info.heap_capacity_bytes = s_pdf_heap_capacity;
            s_report_info.stack_used_bytes =
                web_pdf_thread_stack_used();
            s_report_info.stack_capacity_bytes =
                s_pdf_thread.stack_size;
            if (result == RT_EOK)
            {
                s_report_info.pdf_bytes = pdf_bytes;
                s_report_info.pdf_crc32 = pdf_crc32;
                s_report_info.error_code = 0;
                s_report_info.state = WEB_PDF_REPORT_READY;
            }
            else
            {
                s_report_info.pdf_bytes = 0U;
                s_report_info.pdf_crc32 = 0U;
                s_report_info.error_code = result;
                s_report_info.state = WEB_PDF_REPORT_FAILED;
            }
        }
        if (s_pending_report_valid && !s_pdf_stream_active)
        {
            s_report_info = s_pending_report_info;
            rt_strncpy(s_requested_token, s_report_info.evidence_token,
                       sizeof(s_requested_token) - 1U);
            s_requested_token[sizeof(s_requested_token) - 1U] = '\0';
            rt_memset(&s_pending_report_info, 0,
                      sizeof(s_pending_report_info));
            s_pending_report_valid = RT_FALSE;
            start_pending = RT_TRUE;
        }
        rt_mutex_release(&s_pdf_lock);
        watchdog_supervisor_component_progress(
            WATCHDOG_COMPONENT_PDF_WORKER, WATCHDOG_COMPONENT_STAGE_IDLE,
            ++watchdog_progress);
        if (start_pending)
        {
            (void)rt_sem_release(&s_pdf_wakeup);
        }
    }
}

int web_pdf_report_init(void)
{
    rt_size_t heap_size;
    rt_err_t result;

    if (s_pdf_initialized)
    {
        return RT_EOK;
    }
    heap_size = (rt_size_t)((rt_uintptr_t)&__libharu_heap_end -
                            (rt_uintptr_t)&__libharu_heap_start);
    if (heap_size < 4U * 1024U * 1024U)
    {
        return -RT_ENOMEM;
    }
    result = rt_memheap_init(&s_pdf_memheap, WEB_PDF_MEMHEAP_NAME,
                             &__libharu_heap_start, heap_size);
    if (result != RT_EOK)
    {
        return result;
    }
    s_pdf_heap_capacity = (rt_uint32_t)heap_size;
    s_pdf_heap_ready = RT_TRUE;
    if (rt_mutex_init(&s_pdf_lock, "pdfjob", RT_IPC_FLAG_FIFO) != RT_EOK ||
        rt_sem_init(&s_pdf_wakeup, "pdfwk", 0U, RT_IPC_FLAG_FIFO) != RT_EOK)
    {
        s_pdf_heap_ready = RT_FALSE;
        return -RT_ERROR;
    }
    if (rt_thread_init(&s_pdf_thread, WEB_PDF_THREAD_NAME,
                       web_pdf_worker_entry, RT_NULL,
                       s_pdf_thread_stack, sizeof(s_pdf_thread_stack),
                       WEB_PDF_THREAD_PRIORITY,
                       WEB_PDF_THREAD_TIMESLICE) != RT_EOK)
    {
        s_pdf_heap_ready = RT_FALSE;
        return -RT_ENOMEM;
    }
    if (rt_thread_startup(&s_pdf_thread) != RT_EOK)
    {
        s_pdf_heap_ready = RT_FALSE;
        return -RT_ERROR;
    }
    watchdog_supervisor_component_expect(WATCHDOG_COMPONENT_PDF_WORKER, 0);
    rt_memset(&s_report_info, 0, sizeof(s_report_info));
    rt_memset(&s_pending_report_info, 0, sizeof(s_pending_report_info));
    s_report_info.state = WEB_PDF_REPORT_IDLE;
    s_pending_report_valid = RT_FALSE;
    s_pdf_stream_active = RT_FALSE;
    s_pdf_stream_job_id = 0U;
    s_next_job_id = 0U;
    s_render_count = 0U;
    s_requested_token[0] = '\0';
    s_pdf_initialized = RT_TRUE;
    return RT_EOK;
}

static int web_pdf_report_request(const char *evidence_token,
                                  rt_uint32_t topology_generation,
                                  rt_bool_t pre_generated,
                                  rt_uint32_t *job_id)
{
    rt_bool_t wake_worker = RT_FALSE;
    web_pdf_report_info_t *pending = &s_pending_report_info;

    if (!s_pdf_initialized || !evidence_token || !job_id)
    {
        return -RT_EINVAL;
    }
    if (rt_mutex_take(&s_pdf_lock, rt_tick_from_millisecond(10U)) != RT_EOK)
    {
        return -RT_EBUSY;
    }

    if (s_report_info.job_id != 0U &&
        s_report_info.state != WEB_PDF_REPORT_FAILED &&
        s_report_info.state != WEB_PDF_REPORT_IDLE &&
        rt_strcmp(s_report_info.evidence_token, evidence_token) == 0)
    {
        *job_id = s_report_info.job_id;
        rt_mutex_release(&s_pdf_lock);
        return RT_EOK;
    }
    if (s_pending_report_valid &&
        rt_strcmp(pending->evidence_token, evidence_token) == 0)
    {
        *job_id = pending->job_id;
        rt_mutex_release(&s_pdf_lock);
        return RT_EOK;
    }

    if (s_report_info.state == WEB_PDF_REPORT_RENDERING ||
        s_pdf_stream_active)
    {
        web_pdf_set_job_info(pending, evidence_token,
                             topology_generation, pre_generated,
                             web_pdf_next_job_id_locked());
        s_pending_report_valid = RT_TRUE;
        *job_id = pending->job_id;
    }
    else
    {
        web_pdf_set_job_info(&s_report_info, evidence_token,
                             topology_generation, pre_generated,
                             web_pdf_next_job_id_locked());
        rt_strncpy(s_requested_token, evidence_token,
                   sizeof(s_requested_token) - 1U);
        s_requested_token[sizeof(s_requested_token) - 1U] = '\0';
        s_pending_report_valid = RT_FALSE;
        rt_memset(&s_pending_report_info, 0,
                  sizeof(s_pending_report_info));
        *job_id = s_report_info.job_id;
        wake_worker = RT_TRUE;
    }

    rt_mutex_release(&s_pdf_lock);
    if (wake_worker && rt_sem_release(&s_pdf_wakeup) != RT_EOK)
    {
        return -RT_ERROR;
    }
    return RT_EOK;
}

static rt_uint32_t web_pdf_current_topology_generation(void)
{
    usb_descriptor_manifest_t manifest;

    if (!usb_descriptor_store_get_manifest(&manifest) || !manifest.present)
    {
        return 0U;
    }
    return manifest.generation;
}

int web_pdf_report_start(const char *evidence_token, rt_uint32_t *job_id)
{
    web_snapshot_manifest_t manifest;

    if (!evidence_token || !job_id)
    {
        return -RT_EINVAL;
    }
    if (web_snapshot_get_manifest(&manifest) != RT_EOK)
    {
        return -RT_EBUSY;
    }
    if (rt_strcmp(manifest.token, evidence_token) != 0)
    {
        return -WEB_SNAPSHOT_ERR_STALE;
    }
    return web_pdf_report_request(evidence_token,
                                  web_pdf_current_topology_generation(),
                                  RT_FALSE, job_id);
}

int web_pdf_report_pre_generate(const char *evidence_token,
                                rt_uint32_t topology_generation)
{
    web_snapshot_manifest_t manifest;
    rt_uint32_t job_id;

    if (!evidence_token || topology_generation == 0U)
    {
        return -RT_EINVAL;
    }
    if (web_snapshot_get_manifest(&manifest) != RT_EOK)
    {
        return -RT_EBUSY;
    }
    if (rt_strcmp(manifest.token, evidence_token) != 0 ||
        !manifest.is_final)
    {
        return -WEB_SNAPSHOT_ERR_STALE;
    }
    return web_pdf_report_request(evidence_token, topology_generation,
                                  RT_TRUE, &job_id);
}

void web_pdf_report_cancel_pending(void)
{
    if (!s_pdf_initialized ||
        rt_mutex_take(&s_pdf_lock, rt_tick_from_millisecond(10U)) != RT_EOK)
    {
        return;
    }
    s_pending_report_valid = RT_FALSE;
    rt_memset(&s_pending_report_info, 0, sizeof(s_pending_report_info));
    if (s_report_info.state == WEB_PDF_REPORT_QUEUED)
    {
        rt_memset(&s_report_info, 0, sizeof(s_report_info));
        s_report_info.state = WEB_PDF_REPORT_IDLE;
        s_requested_token[0] = '\0';
    }
    rt_mutex_release(&s_pdf_lock);
}

int web_pdf_report_get_info(rt_uint32_t job_id,
                            web_pdf_report_info_t *info)
{
    if (!s_pdf_initialized || !info)
    {
        return -RT_EINVAL;
    }
    if (rt_mutex_take(&s_pdf_lock, rt_tick_from_millisecond(10U)) != RT_EOK)
    {
        return -RT_EBUSY;
    }
    if (job_id != 0U && s_report_info.job_id == job_id)
    {
        *info = s_report_info;
        rt_mutex_release(&s_pdf_lock);
        return RT_EOK;
    }
    if (s_pending_report_valid &&
        s_pending_report_info.job_id == job_id && job_id != 0U)
    {
        *info = s_pending_report_info;
        rt_mutex_release(&s_pdf_lock);
        return RT_EOK;
    }
    rt_mutex_release(&s_pdf_lock);
    return -WEB_SNAPSHOT_ERR_STALE;
}

int web_pdf_report_get_info_by_token(const char *evidence_token,
                                     web_pdf_report_info_t *info)
{
    if (!s_pdf_initialized || !evidence_token || !info)
    {
        return -RT_EINVAL;
    }
    if (rt_mutex_take(&s_pdf_lock, rt_tick_from_millisecond(10U)) != RT_EOK)
    {
        return -RT_EBUSY;
    }
    if (s_report_info.job_id != 0U &&
        rt_strcmp(s_report_info.evidence_token, evidence_token) == 0)
    {
        *info = s_report_info;
        rt_mutex_release(&s_pdf_lock);
        return RT_EOK;
    }
    if (s_pending_report_valid &&
        rt_strcmp(s_pending_report_info.evidence_token, evidence_token) == 0)
    {
        *info = s_pending_report_info;
        rt_mutex_release(&s_pdf_lock);
        return RT_EOK;
    }
    rt_mutex_release(&s_pdf_lock);
    return -RT_ENOENT;
}

int web_pdf_report_copy_chunk(rt_uint32_t job_id, rt_uint32_t offset,
                              rt_uint8_t *buffer, rt_size_t buffer_size,
                              rt_uint32_t *total_length,
                              rt_uint32_t *pdf_crc32)
{
    rt_size_t length;

    if (!s_pdf_initialized || !buffer || buffer_size == 0U)
    {
        return -RT_EINVAL;
    }
    if (rt_mutex_take(&s_pdf_lock, rt_tick_from_millisecond(10U)) != RT_EOK)
    {
        return -RT_EBUSY;
    }
    if (s_report_info.job_id != job_id || job_id == 0U)
    {
        rt_bool_t pending = s_pending_report_valid &&
                            s_pending_report_info.job_id == job_id;
        rt_mutex_release(&s_pdf_lock);
        return pending ? -RT_EBUSY : -WEB_SNAPSHOT_ERR_STALE;
    }
    if (s_report_info.state == WEB_PDF_REPORT_QUEUED ||
        s_report_info.state == WEB_PDF_REPORT_RENDERING)
    {
        rt_mutex_release(&s_pdf_lock);
        return -RT_EBUSY;
    }
    if (s_report_info.state != WEB_PDF_REPORT_READY)
    {
        rt_mutex_release(&s_pdf_lock);
        return -RT_ERROR;
    }
    if (total_length)
    {
        *total_length = s_report_info.pdf_bytes;
    }
    if (pdf_crc32)
    {
        *pdf_crc32 = s_report_info.pdf_crc32;
    }
    if (offset >= s_report_info.pdf_bytes ||
        offset % WEB_PDF_REPORT_CHUNK_SIZE != 0U)
    {
        rt_mutex_release(&s_pdf_lock);
        return -WEB_SNAPSHOT_ERR_RANGE;
    }
    length = (rt_size_t)(s_report_info.pdf_bytes - offset);
    if (length > WEB_PDF_REPORT_CHUNK_SIZE)
    {
        length = WEB_PDF_REPORT_CHUNK_SIZE;
    }
    if (length > buffer_size)
    {
        length = buffer_size;
    }
    rt_memcpy(buffer, s_pdf_output + offset, length);
    rt_mutex_release(&s_pdf_lock);
    return (int)length;
}

int web_pdf_report_stream_acquire(rt_uint32_t job_id,
                                  web_pdf_report_info_t *info)
{
    if (!s_pdf_initialized || job_id == 0U || info == RT_NULL)
    {
        return -RT_EINVAL;
    }
    if (rt_mutex_take(&s_pdf_lock, rt_tick_from_millisecond(10U)) != RT_EOK)
    {
        return -RT_EBUSY;
    }
    if (s_pdf_stream_active)
    {
        rt_mutex_release(&s_pdf_lock);
        return -RT_EBUSY;
    }
    if (s_report_info.job_id != job_id)
    {
        rt_bool_t pending = s_pending_report_valid &&
                            s_pending_report_info.job_id == job_id;
        rt_mutex_release(&s_pdf_lock);
        return pending ? -RT_EBUSY : -WEB_SNAPSHOT_ERR_STALE;
    }
    if (s_report_info.state == WEB_PDF_REPORT_QUEUED ||
        s_report_info.state == WEB_PDF_REPORT_RENDERING)
    {
        rt_mutex_release(&s_pdf_lock);
        return -RT_EBUSY;
    }
    if (s_report_info.state != WEB_PDF_REPORT_READY ||
        s_report_info.pdf_bytes < 5U ||
        s_report_info.pdf_bytes > WEB_PDF_CACHE_BYTES)
    {
        rt_mutex_release(&s_pdf_lock);
        return -RT_ERROR;
    }
    *info = s_report_info;
    s_pdf_stream_job_id = job_id;
    s_pdf_stream_active = RT_TRUE;
    rt_mutex_release(&s_pdf_lock);
    return RT_EOK;
}

int web_pdf_report_stream_span(rt_uint32_t job_id, rt_uint32_t offset,
                               const rt_uint8_t **data,
                               rt_size_t *length)
{
    rt_size_t span_length;

    if (!s_pdf_initialized || data == RT_NULL || length == RT_NULL)
    {
        return -RT_EINVAL;
    }
    *data = RT_NULL;
    *length = 0U;
    if (rt_mutex_take(&s_pdf_lock, rt_tick_from_millisecond(10U)) != RT_EOK)
    {
        return -RT_EBUSY;
    }
    if (!s_pdf_stream_active || s_pdf_stream_job_id != job_id ||
        s_report_info.job_id != job_id ||
        s_report_info.state != WEB_PDF_REPORT_READY)
    {
        rt_mutex_release(&s_pdf_lock);
        return -WEB_SNAPSHOT_ERR_STALE;
    }
    if (offset >= s_report_info.pdf_bytes)
    {
        rt_mutex_release(&s_pdf_lock);
        return -WEB_SNAPSHOT_ERR_RANGE;
    }
    span_length = (rt_size_t)(s_report_info.pdf_bytes - offset);
    if (span_length > WEB_PDF_REPORT_STREAM_BUFFER_SIZE)
    {
        span_length = WEB_PDF_REPORT_STREAM_BUFFER_SIZE;
    }
    *data = s_pdf_output + offset;
    *length = span_length;
    rt_mutex_release(&s_pdf_lock);
    return RT_EOK;
}

void web_pdf_report_stream_release(rt_uint32_t job_id)
{
    rt_bool_t wake_worker = RT_FALSE;

    if (!s_pdf_initialized || job_id == 0U)
    {
        return;
    }
    if (rt_mutex_take(&s_pdf_lock, RT_WAITING_FOREVER) != RT_EOK)
    {
        return;
    }
    if (s_pdf_stream_active && s_pdf_stream_job_id == job_id)
    {
        s_pdf_stream_active = RT_FALSE;
        s_pdf_stream_job_id = 0U;
        if (s_pending_report_valid &&
            s_report_info.state == WEB_PDF_REPORT_READY)
        {
            s_report_info = s_pending_report_info;
            rt_strncpy(s_requested_token, s_report_info.evidence_token,
                       sizeof(s_requested_token) - 1U);
            s_requested_token[sizeof(s_requested_token) - 1U] = '\0';
            rt_memset(&s_pending_report_info, 0,
                      sizeof(s_pending_report_info));
            s_pending_report_valid = RT_FALSE;
            wake_worker = RT_TRUE;
        }
    }
    rt_mutex_release(&s_pdf_lock);
    if (wake_worker)
    {
        (void)rt_sem_release(&s_pdf_wakeup);
    }
}

rt_bool_t web_pdf_report_is_final_chunk(rt_uint32_t job_id,
                                        rt_uint32_t offset,
                                        rt_size_t chunk_length)
{
    rt_bool_t final_chunk = RT_FALSE;

    if (s_pdf_initialized &&
        rt_mutex_take(&s_pdf_lock, rt_tick_from_millisecond(5U)) == RT_EOK)
    {
        if (s_report_info.job_id == job_id &&
            s_report_info.state == WEB_PDF_REPORT_READY &&
            (rt_uint64_t)offset + chunk_length >= s_report_info.pdf_bytes)
        {
            final_chunk = RT_TRUE;
        }
        rt_mutex_release(&s_pdf_lock);
    }
    return final_chunk;
}
