/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WPM_PROFILE_MANAGER_H
#define WPM_PROFILE_MANAGER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef WPM_PROFILE_MAX
#define WPM_PROFILE_MAX 8U
#endif
#if WPM_PROFILE_MAX < 1 || WPM_PROFILE_MAX > 8
#error "WPM_PROFILE_MAX must be in the range 1..8 for wire format v1"
#endif
#define WPM_SSID_BYTES_MAX 32U
#define WPM_PASSWORD_BYTES_MAX 63U
#define WPM_CONFIG_WIRE_MAX (24U + 108U * WPM_PROFILE_MAX)
#define WPM_CONFIG_FORMAT_VERSION 1U

/* Stable operation return values. */
#define WPM_EINVAL (-1)
#define WPM_EBUSY (-2)
#define WPM_ENOENT (-3)
#define WPM_EFULL (-4)
#define WPM_EFOREIGN (-5)
#define WPM_ENOTINITIALIZED (-6)
#define WPM_ESTATE (-7)
#define WPM_ECAPACITY (-8)

/* wpm_core_connect_saved result for an already healthy matching connection. */
#define WPM_CORE_ALREADY_CONNECTED 1

typedef enum { WPM_SECURITY_OPEN = 0, WPM_SECURITY_WPA2 = 1,
    WPM_SECURITY_WPA_MIXED = 2, WPM_SECURITY_UNSUPPORTED = 3 } wpm_security_t;
/* Scanning is an adapter concern and is not a state of this core. */
typedef enum {
    WPM_PHASE_INITIALIZING = 0,
    WPM_PHASE_IDLE = 1,
    WPM_PHASE_ASSOCIATING = 2,
    WPM_PHASE_DHCP = 3,
    WPM_PHASE_ONLINE = 4,
    WPM_PHASE_RETRY_WAIT = 5,
    WPM_PHASE_CANCELLING = 6,
    WPM_PHASE_DISCONNECTED = 7,
    WPM_PHASE_FAILED = 8
} wpm_phase_t;
typedef enum { WPM_REASON_NONE, WPM_REASON_NOT_FOUND, WPM_REASON_AUTH,
    WPM_REASON_UNSUPPORTED, WPM_REASON_DHCP, WPM_REASON_STORAGE,
    WPM_REASON_IO, WPM_REASON_CANCELLED, WPM_REASON_FULL,
    WPM_REASON_BUSY, WPM_REASON_CORRUPT, WPM_REASON_FOREIGN,
    WPM_REASON_INVALID, WPM_REASON_AUTH_INCOMPLETE } wpm_reason_t;
typedef enum { WPM_STORAGE_LOADING, WPM_STORAGE_EMPTY, WPM_STORAGE_OK,
    WPM_STORAGE_SAVING, WPM_STORAGE_FAILED, WPM_STORAGE_CORRUPT,
    WPM_STORAGE_FOREIGN } wpm_storage_status_t;

/* Secrets are private to the coordinator/store/worker, never public status. */
typedef struct {
    uint32_t id, last_success;
    wpm_security_t security;
    bool auto_connect;
    uint8_t ssid_length, password_length;
    uint8_t ssid[WPM_SSID_BYTES_MAX];
    uint8_t password[WPM_PASSWORD_BYTES_MAX + 1];
} wpm_profile_t;
typedef struct {
    uint32_t next_id, success_counter, last_success_id;
    bool auto_connect;
    uint8_t count;
    wpm_profile_t profiles[WPM_PROFILE_MAX];
} wpm_config_t;
typedef struct {
    uint32_t id, last_success;
    wpm_security_t security;
    bool auto_connect, persisted;
    uint8_t ssid_length;
    char ssid[WPM_SSID_BYTES_MAX + 1];
} wpm_profile_summary_t;
typedef struct {
    wpm_phase_t phase;
    wpm_reason_t reason;
    wpm_storage_status_t storage;
    uint32_t operation_id, active_profile_id, config_revision, retry_in_ms;
    bool auto_connect, auto_suspended, temporary, initialized;
    uint8_t profile_count, saved_profile_count, attempt, candidate_count, ssid_length;
    bool pending_changes, cleanup_pending;
    char ssid[WPM_SSID_BYTES_MAX + 1], ip[46];
    wpm_profile_summary_t profiles[WPM_PROFILE_MAX];
} wpm_public_snapshot_t;

typedef enum { WPM_CORE_JOB_NONE, WPM_CORE_JOB_JOIN, WPM_CORE_JOB_LEAVE,
    WPM_CORE_JOB_STORE } wpm_core_job_kind_t;
typedef uint32_t (*wpm_operation_id_allocator_t)(void *context);
typedef enum { WPM_STORE_FAILED=0, WPM_STORE_CLEAN=1,
    WPM_STORE_COMMITTED_DIRTY=2, WPM_STORE_FOREIGN=3 } wpm_store_outcome_t;
typedef struct {
    wpm_core_job_kind_t kind;
    uint32_t operation_id, config_revision;
    bool scrub_old;
    wpm_profile_t target;
    wpm_config_t config;
} wpm_core_job_t;
/* Caller-owned storage. Treat members as private; use the public APIs below. */
typedef struct {
    wpm_config_t config, durable_config, inflight_config;
    wpm_profile_t target;
    wpm_phase_t phase;
    wpm_reason_t reason;
    wpm_storage_status_t storage;
    uint32_t operation_id, config_revision, durable_revision, store_revision;
    uint32_t retry_due, dhcp_due;
    uint32_t blocked_ids[WPM_PROFILE_MAX], candidates[WPM_PROFILE_MAX];
    uint8_t candidate_count, candidate_next, attempt, dhcp_retry;
    bool initialized, auto_suspended, from_auto, remember, temporary;
    bool join_pending, leave_pending, store_pending, scrub_pending;
    bool radio_busy, store_busy, join_ready, dhcp_reset_requested, cleanup_pending;
    bool target_valid;
    wpm_phase_t leave_destination;
    wpm_core_job_kind_t radio_job_kind;
    wpm_operation_id_allocator_t allocate_operation;
    void *operation_context;
    char ip[46];
} wpm_core_t;

/* Stable read-only state needed by a radio/storage adapter. */
typedef struct {
    wpm_phase_t phase;
    wpm_reason_t reason;
    wpm_storage_status_t storage;
    uint32_t operation_id, config_revision, durable_revision, store_revision;
    bool radio_pending, join_pending, leave_pending, radio_busy, link_ready;
    bool store_pending, store_busy, cleanup_pending, auto_suspended;
    bool dhcp_reset_pending, active_target_is_durable;
} wpm_core_adapter_state_t;

/*
 * Calls operating on one core instance must be serialized by a single owner.
 * The core is neither thread-safe nor ISR-safe. Asynchronous adapter results
 * should be sent back to that owner before calling the result APIs.
 */
void wpm_config_default(wpm_config_t *config);
bool wpm_config_valid(const wpm_config_t *config);
int wpm_config_encode(const wpm_config_t *, uint8_t *, size_t, size_t *);
/* On failure, clears the output and returns WPM_EINVAL. */
int wpm_config_decode(wpm_config_t *, const uint8_t *, size_t);
void wpm_secure_zero(void *, size_t);
void wpm_core_init(wpm_core_t *);
void wpm_core_set_operation_allocator(wpm_core_t *,
    wpm_operation_id_allocator_t allocator, void *context);
void wpm_core_load(wpm_core_t *, const wpm_config_t *, wpm_storage_status_t);
/*
 * All now_ms parameters and retry_in_ms snapshot values use monotonic
 * milliseconds from one time source. Scheduled intervals must be shorter
 * than 2^31 ms for wrap-safe deadline comparisons.
 */
/* Returns 0, WPM_EINVAL, WPM_ENOTINITIALIZED, or WPM_EFULL. */
int wpm_core_connect(wpm_core_t *, const uint8_t *ssid, size_t ssid_length,
    wpm_security_t, const char *password, bool remember, uint32_t now_ms);
/* Returns WPM_CORE_ALREADY_CONNECTED without state changes for a matching, durable ONLINE profile. */
/* Otherwise returns 0 or WPM_ENOENT. */
int wpm_core_connect_saved(wpm_core_t *, uint32_t id, uint32_t now_ms);
/* Returns 0, WPM_ENOENT, WPM_EINVAL, or WPM_EFULL. */
int wpm_core_update_password(wpm_core_t *, uint32_t id, const char *,
                             uint32_t now_ms);
void wpm_core_disconnect(wpm_core_t *);
void wpm_core_cancel(wpm_core_t *);
/* Returns 0 when a saved profile is removed or a pending remembered join is cancelled; else WPM_ENOENT. */
int wpm_core_forget(wpm_core_t *, uint32_t id);
void wpm_core_forget_all(wpm_core_t *);
void wpm_core_set_auto(wpm_core_t *, bool);
/* Returns 0 or WPM_ENOENT. */
int wpm_core_set_profile_auto(wpm_core_t *, uint32_t id, bool);
/* Returns 0 or WPM_EFOREIGN. */
int wpm_core_retry_save(wpm_core_t *);
/* Returns 0, WPM_ESTATE when not ONLINE, or WPM_EBUSY while work is active. */
int wpm_core_use_temporary(wpm_core_t *);
void wpm_core_tick(wpm_core_t *, uint32_t now_ms);
/* Requires non-NULL core and output. Jobs copy credentials; erase after adapter use. */
bool wpm_core_next_job(wpm_core_t *, wpm_core_job_t *);
/* Results are ignored unless kind and operation match the active radio job. */
void wpm_core_radio_result(wpm_core_t *, wpm_core_job_kind_t, uint32_t operation,
    wpm_reason_t result, uint32_t now_ms);
/* Accepts IPv4 or IPv6 presentation strings up to 45 bytes. */
void wpm_core_ip_ready(wpm_core_t *, uint32_t operation, const char *ip,
                       uint32_t now_ms);
void wpm_core_link_lost(wpm_core_t *, uint32_t operation, uint32_t now_ms);
/* Unknown outcomes are treated as failures, never as durable success. */
void wpm_core_store_result(wpm_core_t *, uint32_t revision, wpm_store_outcome_t);
void wpm_core_adapter_snapshot(const wpm_core_t *, wpm_core_adapter_state_t *);
void wpm_core_report_error(wpm_core_t *, wpm_reason_t reason);
void wpm_core_mark_cleanup_pending(wpm_core_t *);
void wpm_core_suspend_automatic(wpm_core_t *);
bool wpm_core_take_dhcp_reset_request(wpm_core_t *);
void wpm_core_snapshot(const wpm_core_t *, uint32_t now_ms,
                       wpm_public_snapshot_t *);
const char *wpm_phase_name(wpm_phase_t);
const char *wpm_reason_name(wpm_reason_t);
const char *wpm_storage_name(wpm_storage_status_t);

#ifdef __cplusplus
}
#endif
#endif
