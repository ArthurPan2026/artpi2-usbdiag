/*
 * Shared Wi-Fi state. Producers update it from the Mongoose service thread;
 * LVGL consumes a copied snapshot from its own thread.
 */
#ifndef WIFI_STATE_H
#define WIFI_STATE_H
#include <rtthread.h>
#include "wifi_manager_core.h"

#define WIFI_STATE_DISCONNECTED  0
#define WIFI_STATE_CONNECTING    1
#define WIFI_STATE_CONNECTED     2
#define WIFI_STATE_FAILED        3
#define WIFI_STATE_SCANNING      4

#define WIFI_SCAN_MAX_APS            32U
#define WIFI_SCAN_SSID_MAX_LENGTH    64U

#define WIFI_AP_FLAG_SSID_TRUNCATED  0x01U

enum wifi_scan_status
{
    WIFI_SCAN_STATUS_IDLE = 0,
    WIFI_SCAN_STATUS_SCANNING,
    WIFI_SCAN_STATUS_COMPLETE,
    WIFI_SCAN_STATUS_EMPTY,
    WIFI_SCAN_STATUS_TRUNCATED,
    WIFI_SCAN_STATUS_FAILED,
    WIFI_SCAN_STATUS_CANCELLED
};

typedef struct wifi_ap_record
{
    char ssid[WIFI_SCAN_SSID_MAX_LENGTH];
    rt_uint8_t ssid_length;
    rt_int16_t rssi;
    rt_uint8_t flags;
    rt_uint8_t ecn;
    rt_uint32_t security;
    wifi_security_t security_mode;
    rt_int16_t channel;
    rt_uint8_t bssid[6];
} wifi_ap_record_t;

typedef struct {
    wifi_public_snapshot_t network;
    int scan_status;
    rt_uint32_t scan_generation, scan_dropped;
    rt_uint8_t scan_count;
    wifi_ap_record_t access_points[WIFI_SCAN_MAX_APS];
} wifi_snapshot_t;

struct wifi_state {
    volatile int  conn_status;       /* one of WIFI_STATE_* */
    char          ip[32];
    char          ssid[64];
    char          status_msg[64];
    volatile int  scan_count;
    volatile int  scan_status;       /* one of enum wifi_scan_status */
    volatile rt_uint32_t scan_dropped_count;
    volatile rt_uint32_t scan_updated;
    volatile rt_uint32_t uart_overflow_count;
    volatile rt_uint32_t uart_error_count;
    wifi_ap_record_t scan_records[WIFI_SCAN_MAX_APS];
};

/* Global instance — defined in mongoose_service.c. */
extern struct wifi_state g_wifi;

static inline void wifi_copy_text(char *destination, rt_size_t destination_size,
                                  const char *source)
{
    if (destination_size == 0) return;
    rt_strncpy(destination, source ? source : "", destination_size - 1);
    destination[destination_size - 1] = '\0';
}

static inline void wifi_set_status(int st, const char *msg)
{
    rt_enter_critical();
    if (msg) wifi_copy_text(g_wifi.status_msg, sizeof(g_wifi.status_msg), msg);
    g_wifi.conn_status = st;
    rt_exit_critical();
}

static inline void wifi_set_ssid(const char *ssid)
{
    rt_enter_critical();
    wifi_copy_text(g_wifi.ssid, sizeof(g_wifi.ssid), ssid);
    rt_exit_critical();
}

static inline void wifi_set_ip(const char *ip)
{
    rt_enter_critical();
    wifi_copy_text(g_wifi.ip, sizeof(g_wifi.ip), ip);
    rt_exit_critical();
}

static inline void wifi_set_scan_result(const wifi_ap_record_t *records,
                                        rt_size_t count, int status,
                                        rt_uint32_t dropped_count)
{
    if (count > WIFI_SCAN_MAX_APS)
    {
        count = WIFI_SCAN_MAX_APS;
    }
    if (!records)
    {
        count = 0U;
    }

    rt_enter_critical();
    rt_memset(g_wifi.scan_records, 0, sizeof(g_wifi.scan_records));
    if (count > 0U)
    {
        rt_memcpy(g_wifi.scan_records, records,
                  count * sizeof(g_wifi.scan_records[0]));
    }
    g_wifi.scan_count = (int)count;
    g_wifi.scan_status = status;
    g_wifi.scan_dropped_count = dropped_count;
    g_wifi.scan_updated++;
    rt_exit_critical();
}

static inline void wifi_set_uart_counters(rt_uint32_t overflow_count,
                                          rt_uint32_t error_count)
{
    rt_enter_critical();
    g_wifi.uart_overflow_count = overflow_count;
    g_wifi.uart_error_count = error_count;
    rt_exit_critical();
}

#endif
