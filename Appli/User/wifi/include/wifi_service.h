#ifndef WIFI_SERVICE_H
#define WIFI_SERVICE_H

#include <rtthread.h>
#include "wifi_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Requests are queued for the single Mongoose event-loop owner.  Connection
 * and scan completion are reported asynchronously through g_wifi. */
int wifi_request_scan(void);
int wifi_request_connect(const char *ssid, const char *password);
int wifi_request_connect_ap(const wifi_ap_record_t *access_point,
                            const char *password);
int wifi_request_connect_network(const wifi_ap_record_t *, const char *password,
    rt_bool_t remember, rt_uint32_t *operation);
int wifi_request_connect_saved(rt_uint32_t profile, rt_uint32_t *operation);
int wifi_request_update_password(rt_uint32_t profile, const char *password,
    rt_uint32_t *operation);
int wifi_request_disconnect(rt_uint32_t *operation);
int wifi_request_cancel(rt_uint32_t *operation);
int wifi_request_forget(rt_uint32_t profile, rt_uint32_t *operation);
int wifi_request_forget_all(rt_uint32_t *operation);
int wifi_request_set_autoconnect(rt_bool_t enabled, rt_uint32_t *operation);
int wifi_request_set_profile_autoconnect(rt_uint32_t profile, rt_bool_t enabled,
    rt_uint32_t *operation);
int wifi_request_retry_save(rt_uint32_t *operation);
int wifi_request_use_temporary(rt_uint32_t *operation);
int wifi_get_snapshot(wifi_snapshot_t *snapshot);

/* True once WHD firmware/CLM and Mongoose's built-in TCP/IP interface have
 * been initialized.  It intentionally does not require DHCP/link-up; the
 * purpose is to serialize external-memory-heavy graphics startup behind the
 * WHD initialization transaction. */
rt_bool_t mongoose_service_is_ready(void);

#ifdef __cplusplus
}
#endif

#endif
