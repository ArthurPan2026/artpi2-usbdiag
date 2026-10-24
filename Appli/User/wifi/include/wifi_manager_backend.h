#ifndef WORK_WIFI_MANAGER_BACKEND_H
#define WORK_WIFI_MANAGER_BACKEND_H
#include "wifi_manager_core.h"
#include <rtthread.h>

/* Fixed-size, credential-free diagnostics for the WHD radio backend.  The
 * raw fields are numeric WHD return/event codes; this interface never carries
 * an SSID, password, or other profile data. */
typedef enum {
    WIFI_BACKEND_DIAG_STAGE_NONE = 0,
    WIFI_BACKEND_DIAG_STAGE_REQUEST,
    WIFI_BACKEND_DIAG_STAGE_DRIVER,
    WIFI_BACKEND_DIAG_STAGE_CALLBACK
} wifi_backend_diag_stage_t;

typedef enum {
    WIFI_BACKEND_DIAG_OUTCOME_NONE = 0,
    WIFI_BACKEND_DIAG_OUTCOME_IN_PROGRESS,
    WIFI_BACKEND_DIAG_OUTCOME_SUCCESS,
    WIFI_BACKEND_DIAG_OUTCOME_BUSY,
    WIFI_BACKEND_DIAG_OUTCOME_ABORTED,
    WIFI_BACKEND_DIAG_OUTCOME_CANCELLED,
    WIFI_BACKEND_DIAG_OUTCOME_ERROR
} wifi_backend_diag_outcome_t;

/* Saturating event counters. A failed scan-stop command can increment errors
 * while the underlying scan later reports its own terminal outcome. */
typedef struct {
    uint32_t attempts;
    uint32_t successes;
    uint32_t busy;
    uint32_t aborted;
    uint32_t cancelled;
    uint32_t errors;
} wifi_backend_diag_counters_t;

typedef struct {
    uint32_t sequence;
    uint32_t raw_whd_result; /* Keeps an operation's latest non-success WHD code. */
    uint32_t raw_event_code;
    int32_t backend_result;
    uint8_t stage;
    uint8_t outcome;
    uint16_t reserved;
} wifi_backend_diag_operation_t;

/* A scan request rejected while another scan is active increments attempts
 * and busy without replacing last_scan, which belongs to the active scan. */
typedef struct {
    wifi_backend_diag_counters_t scan_counters;
    wifi_backend_diag_counters_t join_counters;
    wifi_backend_diag_counters_t leave_counters;
    wifi_backend_diag_operation_t last_scan;
    wifi_backend_diag_operation_t last_join;
    wifi_backend_diag_operation_t last_leave;
} wifi_backend_diagnostics_t;

/* Radio transactions run only on the wireless worker. Mongoose owns IP/reset. */
/* WHD bootstrap runs on wifi_worker; TCP/IP finalize runs on the Mongoose owner. */
int wifi_backend_initialize(void);
int wifi_backend_finalize(void);
wifi_reason_t wifi_backend_join(const wifi_profile_t *, bool (*cancel)(void *), void *);
wifi_reason_t wifi_backend_leave(void);
int wifi_backend_scan(void);
int wifi_backend_stop_scan(void);
int wifi_backend_get_diagnostics(wifi_backend_diagnostics_t *out);
bool wifi_backend_scan_active(void);
bool wifi_backend_link_ready(void);
void wifi_backend_publish_link(bool ready);
void wifi_backend_network_reset(uint32_t generation);
void wifi_backend_dhcp_reset(void);
int wifi_manager_service_init(void);
void wifi_manager_poll(void);
void wifi_manager_notify_ip_ready(uint32_t generation, const char *ip);
uint32_t wifi_manager_radio_generation(void);
int wifi_status_json(char *, size_t);
#endif
