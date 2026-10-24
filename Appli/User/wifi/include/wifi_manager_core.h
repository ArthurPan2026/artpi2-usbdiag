/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WORK_WIFI_MANAGER_CORE_COMPAT_H
#define WORK_WIFI_MANAGER_CORE_COMPAT_H

#include <string.h>
#include "wifi_profile_manager.h"

typedef wpm_security_t wifi_security_t;
typedef wpm_reason_t wifi_reason_t;
typedef wpm_storage_status_t wifi_storage_status_t;
typedef enum {
    WIFI_PHASE_INITIALIZING = 0,
    WIFI_PHASE_IDLE = 1,
    WIFI_PHASE_SCANNING = 2,
    WIFI_PHASE_ASSOCIATING = 3,
    WIFI_PHASE_DHCP = 4,
    WIFI_PHASE_ONLINE = 5,
    WIFI_PHASE_RETRY_WAIT = 6,
    WIFI_PHASE_CANCELLING = 7,
    WIFI_PHASE_DISCONNECTED = 8,
    WIFI_PHASE_FAILED = 9
} wifi_phase_t;
/* ART-Pi2 startup/radio state is application data, outside the package core. */
typedef enum {
    WIFI_RADIO_INIT_WAITING_DISPLAY = 0,
    WIFI_RADIO_INIT_QUEUED = 1,
    WIFI_RADIO_INIT_INITIALIZING = 2,
    WIFI_RADIO_INIT_READY = 3,
    WIFI_RADIO_INIT_FAILED = 4,
    WIFI_RADIO_INIT_SLOW = 5
} wifi_radio_init_state_t;
typedef wpm_profile_t wifi_profile_t;
typedef wpm_config_t wifi_config_t;
typedef wpm_profile_summary_t wifi_profile_summary_t;
/* The product view adds asynchronous receipts and radio bootstrap state to
 * the manager-owned snapshot. The package snapshot contains only core state. */
typedef struct {
    wifi_phase_t phase;
    wifi_reason_t reason;
    wifi_storage_status_t storage;
    uint32_t operation_id, active_profile_id, config_revision, retry_in_ms;
    uint32_t connection_operation_id, config_operation_id;
    uint32_t saved_operation_id, config_saved_operation_id;
    uint32_t request_result_id;
    int request_result;
    bool auto_connect, auto_suspended, temporary, initialized;
    uint8_t profile_count, saved_profile_count, attempt, candidate_count;
    uint8_t ssid_length;
    bool pending_changes, cleanup_pending;
    char ssid[WPM_SSID_BYTES_MAX + 1], ip[16];
    wifi_profile_summary_t profiles[WPM_PROFILE_MAX];
    bool radio_ready;
    uint8_t radio_init_state;
} wifi_public_snapshot_t;
typedef wpm_core_job_kind_t wifi_core_job_kind_t;
typedef wpm_store_outcome_t wifi_store_outcome_t;
typedef wpm_core_job_t wifi_core_job_t;
typedef wpm_core_t wifi_core_t;
typedef wpm_core_adapter_state_t wifi_core_adapter_state_t;

#define WIFI_CONFIG_FORMAT_VERSION WPM_CONFIG_FORMAT_VERSION
#define WIFI_CONFIG_WIRE_MAX WPM_CONFIG_WIRE_MAX
#define WIFI_CORE_ALREADY_CONNECTED WPM_CORE_ALREADY_CONNECTED
#define WIFI_CORE_JOB_JOIN WPM_CORE_JOB_JOIN
#define WIFI_CORE_JOB_LEAVE WPM_CORE_JOB_LEAVE
#define WIFI_CORE_JOB_NONE WPM_CORE_JOB_NONE
#define WIFI_CORE_JOB_STORE WPM_CORE_JOB_STORE
#define WIFI_PASSWORD_BYTES_MAX 63U
typedef char wifi_password_limit_must_match_package[(WIFI_PASSWORD_BYTES_MAX == WPM_PASSWORD_BYTES_MAX) ? 1 : -1];
#define WIFI_EINVAL WPM_EINVAL
#define WIFI_EBUSY WPM_EBUSY
#define WIFI_ENOENT WPM_ENOENT
#define WIFI_EFULL WPM_EFULL
#define WIFI_EFOREIGN WPM_EFOREIGN
#define WIFI_ENOTINITIALIZED WPM_ENOTINITIALIZED
#define WIFI_ESTATE WPM_ESTATE
#define WIFI_ECAPACITY WPM_ECAPACITY
#define WIFI_PROFILE_MAX WPM_PROFILE_MAX
#define WIFI_REASON_AUTH WPM_REASON_AUTH
#define WIFI_REASON_AUTH_INCOMPLETE WPM_REASON_AUTH_INCOMPLETE
#define WIFI_REASON_BUSY WPM_REASON_BUSY
#define WIFI_REASON_CANCELLED WPM_REASON_CANCELLED
#define WIFI_REASON_CORRUPT WPM_REASON_CORRUPT
#define WIFI_REASON_DHCP WPM_REASON_DHCP
#define WIFI_REASON_FOREIGN WPM_REASON_FOREIGN
#define WIFI_REASON_FULL WPM_REASON_FULL
#define WIFI_REASON_INVALID WPM_REASON_INVALID
#define WIFI_REASON_IO WPM_REASON_IO
#define WIFI_REASON_NONE WPM_REASON_NONE
#define WIFI_REASON_NOT_FOUND WPM_REASON_NOT_FOUND
#define WIFI_REASON_STORAGE WPM_REASON_STORAGE
#define WIFI_REASON_UNSUPPORTED WPM_REASON_UNSUPPORTED
#define WIFI_SECURITY_OPEN WPM_SECURITY_OPEN
#define WIFI_SECURITY_UNSUPPORTED WPM_SECURITY_UNSUPPORTED
#define WIFI_SECURITY_WPA_MIXED WPM_SECURITY_WPA_MIXED
#define WIFI_SECURITY_WPA2 WPM_SECURITY_WPA2
#define WIFI_SSID_BYTES_MAX WPM_SSID_BYTES_MAX
#define WIFI_STORAGE_CORRUPT WPM_STORAGE_CORRUPT
#define WIFI_STORAGE_EMPTY WPM_STORAGE_EMPTY
#define WIFI_STORAGE_FAILED WPM_STORAGE_FAILED
#define WIFI_STORAGE_FOREIGN WPM_STORAGE_FOREIGN
#define WIFI_STORAGE_LOADING WPM_STORAGE_LOADING
#define WIFI_STORAGE_OK WPM_STORAGE_OK
#define WIFI_STORAGE_SAVING WPM_STORAGE_SAVING
#define WIFI_STORE_CLEAN WPM_STORE_CLEAN
#define WIFI_STORE_COMMITTED_DIRTY WPM_STORE_COMMITTED_DIRTY
#define WIFI_STORE_FAILED WPM_STORE_FAILED

#define wifi_config_decode wpm_config_decode
#define wifi_config_default wpm_config_default
#define wifi_config_encode wpm_config_encode
#define wifi_config_valid wpm_config_valid
#define wifi_core_cancel wpm_core_cancel
#define wifi_core_connect wpm_core_connect
#define wifi_core_connect_saved wpm_core_connect_saved
#define wifi_core_disconnect wpm_core_disconnect
#define wifi_core_forget wpm_core_forget
#define wifi_core_forget_all wpm_core_forget_all
#define wifi_core_init wpm_core_init
#define wifi_core_set_operation_allocator wpm_core_set_operation_allocator
#define wifi_core_adapter_snapshot wpm_core_adapter_snapshot
#define wifi_core_report_error wpm_core_report_error
#define wifi_core_mark_cleanup_pending wpm_core_mark_cleanup_pending
#define wifi_core_suspend_automatic wpm_core_suspend_automatic
#define wifi_core_take_dhcp_reset_request wpm_core_take_dhcp_reset_request
#define wifi_core_ip_ready wpm_core_ip_ready
#define wifi_core_link_lost wpm_core_link_lost
#define wifi_core_load wpm_core_load
#define wifi_core_next_job wpm_core_next_job
#define wifi_core_radio_result wpm_core_radio_result
#define wifi_core_retry_save wpm_core_retry_save
#define wifi_core_set_auto wpm_core_set_auto
#define wifi_core_set_profile_auto wpm_core_set_profile_auto
#define wifi_core_store_result wpm_core_store_result
#define wifi_core_tick wpm_core_tick
#define wifi_core_update_password wpm_core_update_password
#define wifi_core_use_temporary wpm_core_use_temporary
#define wifi_reason_name wpm_reason_name
#define wifi_secure_zero wpm_secure_zero
#define wifi_storage_name wpm_storage_name

static inline void wifi_core_snapshot(const wifi_core_t *core,
                                      uint32_t now_ms,
                                      wifi_public_snapshot_t *snapshot)
{
    wpm_public_snapshot_t core_snapshot;
    size_t i;

    if (snapshot == NULL)
    {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    if (core == NULL)
    {
        return;
    }
    wpm_core_snapshot(core, now_ms, &core_snapshot);
    switch (core_snapshot.phase) {
    case WPM_PHASE_INITIALIZING: snapshot->phase = WIFI_PHASE_INITIALIZING; break;
    case WPM_PHASE_IDLE: snapshot->phase = WIFI_PHASE_IDLE; break;
    case WPM_PHASE_ASSOCIATING: snapshot->phase = WIFI_PHASE_ASSOCIATING; break;
    case WPM_PHASE_DHCP: snapshot->phase = WIFI_PHASE_DHCP; break;
    case WPM_PHASE_ONLINE: snapshot->phase = WIFI_PHASE_ONLINE; break;
    case WPM_PHASE_RETRY_WAIT: snapshot->phase = WIFI_PHASE_RETRY_WAIT; break;
    case WPM_PHASE_CANCELLING: snapshot->phase = WIFI_PHASE_CANCELLING; break;
    case WPM_PHASE_DISCONNECTED: snapshot->phase = WIFI_PHASE_DISCONNECTED; break;
    case WPM_PHASE_FAILED: snapshot->phase = WIFI_PHASE_FAILED; break;
    default: snapshot->phase = WIFI_PHASE_FAILED; break;
    }
    snapshot->reason = core_snapshot.reason;
    snapshot->storage = core_snapshot.storage;
    snapshot->operation_id = core_snapshot.operation_id;
    snapshot->active_profile_id = core_snapshot.active_profile_id;
    snapshot->config_revision = core_snapshot.config_revision;
    snapshot->retry_in_ms = core_snapshot.retry_in_ms;
    snapshot->auto_connect = core_snapshot.auto_connect;
    snapshot->auto_suspended = core_snapshot.auto_suspended;
    snapshot->temporary = core_snapshot.temporary;
    snapshot->initialized = core_snapshot.initialized;
    snapshot->profile_count = core_snapshot.profile_count;
    snapshot->saved_profile_count = core_snapshot.saved_profile_count;
    snapshot->attempt = core_snapshot.attempt;
    snapshot->candidate_count = core_snapshot.candidate_count;
    snapshot->ssid_length = core_snapshot.ssid_length;
    snapshot->pending_changes = core_snapshot.pending_changes;
    snapshot->cleanup_pending = core_snapshot.cleanup_pending;
    memcpy(snapshot->ssid, core_snapshot.ssid, sizeof(snapshot->ssid));
    memcpy(snapshot->ip, core_snapshot.ip, sizeof(snapshot->ip));
    for (i = 0U; i < WIFI_PROFILE_MAX; i++)
    {
        snapshot->profiles[i] = core_snapshot.profiles[i];
    }
}

static inline const char *wifi_phase_name(wifi_phase_t phase)
{
    static const char *const names[] = {
        "initializing", "idle", "scanning", "associating", "dhcp",
        "online", "retry_wait", "cancelling", "disconnected", "failed"
    };
    return (unsigned)phase < (sizeof(names) / sizeof(names[0])) ?
        names[(unsigned)phase] : "unknown";
}

#endif
