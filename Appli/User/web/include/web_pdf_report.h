#ifndef WEB_PDF_REPORT_H
#define WEB_PDF_REPORT_H

#include <rtthread.h>
#include "web_snapshot.h"

#define WEB_PDF_REPORT_CHUNK_SIZE 24576U
#define WEB_PDF_REPORT_STREAM_BUFFER_SIZE 16384U

typedef enum
{
    WEB_PDF_REPORT_IDLE = 0,
    WEB_PDF_REPORT_QUEUED,
    WEB_PDF_REPORT_RENDERING,
    WEB_PDF_REPORT_READY,
    WEB_PDF_REPORT_FAILED,
} web_pdf_report_state_t;

typedef struct
{
    rt_uint32_t job_id;
    web_pdf_report_state_t state;
    rt_uint32_t pdf_bytes;
    rt_uint32_t pdf_crc32;
    rt_uint32_t heap_peak_bytes;
    rt_uint32_t heap_live_bytes;
    rt_uint32_t heap_used_bytes;
    rt_uint32_t heap_capacity_bytes;
    rt_uint32_t stack_used_bytes;
    rt_uint32_t stack_capacity_bytes;
    rt_uint32_t topology_generation;
    rt_uint32_t render_count;
    rt_int32_t error_code;
    rt_bool_t pre_generated;
    char evidence_token[WEB_SNAPSHOT_TOKEN_SIZE];
} web_pdf_report_info_t;

int web_pdf_report_init(void);
int web_pdf_report_start(const char *evidence_token, rt_uint32_t *job_id);
int web_pdf_report_pre_generate(const char *evidence_token,
                                rt_uint32_t topology_generation);
void web_pdf_report_cancel_pending(void);
int web_pdf_report_get_info(rt_uint32_t job_id,
                            web_pdf_report_info_t *info);
int web_pdf_report_get_info_by_token(const char *evidence_token,
                                     web_pdf_report_info_t *info);
int web_pdf_report_copy_chunk(rt_uint32_t job_id, rt_uint32_t offset,
                              rt_uint8_t *buffer, rt_size_t buffer_size,
                              rt_uint32_t *total_length,
                              rt_uint32_t *pdf_crc32);
int web_pdf_report_stream_acquire(rt_uint32_t job_id,
                                 web_pdf_report_info_t *info);
int web_pdf_report_stream_span(rt_uint32_t job_id, rt_uint32_t offset,
                               const rt_uint8_t **data,
                               rt_size_t *length);
void web_pdf_report_stream_release(rt_uint32_t job_id);
rt_bool_t web_pdf_report_is_final_chunk(rt_uint32_t job_id,
                                        rt_uint32_t offset,
                                        rt_size_t chunk_length);

#endif /* WEB_PDF_REPORT_H */
