#include "usb_evidence.h"

/* Presentation-only evidence source.  USB Host is intentionally not part of
 * this ART-Pi2 baseline; the UI receives an explicit unavailable snapshot. */
static usb_evidence_snapshot_t s_snapshot;
static rt_bool_t s_initialized;

static void evidence_stub_ensure_initialized(void)
{
    if (s_initialized)
    {
        return;
    }

    rt_memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.schema_version = USB_EVIDENCE_SCHEMA_VERSION;
    s_snapshot.parser_version = USB_EVIDENCE_PARSER_VERSION;
    s_snapshot.ruleset_version = USB_EVIDENCE_RULESET_VERSION;
    s_snapshot.session_state = USB_EVIDENCE_SESSION_WAITING;
    s_snapshot.capture_integrity = USB_EVIDENCE_INTEGRITY_UNAVAILABLE;
    s_snapshot.is_final = RT_FALSE;
    for (rt_size_t index = 0U; index < USB_EVIDENCE_RULE_COUNT; index++)
    {
        s_snapshot.rules[index].verdict = USB_RULE_NOT_OBSERVED;
    }
    s_initialized = RT_TRUE;
}

int usb_evidence_start(void)
{
    evidence_stub_ensure_initialized();
    return RT_EOK;
}

void usb_evidence_record_port_reset(rt_uint8_t port, rt_uint8_t speed)
{
    RT_UNUSED(port);
    RT_UNUSED(speed);
}

void usb_evidence_record_control(rt_uint8_t speed,
                                 rt_uint8_t port,
                                 rt_uint32_t started_at_us,
                                 rt_uint32_t completed_at_us,
                                 rt_bool_t timing_valid,
                                 const usb_evidence_setup_t *setup,
                                 rt_int32_t return_status,
                                 const rt_uint8_t *response)
{
    RT_UNUSED(speed);
    RT_UNUSED(port);
    RT_UNUSED(started_at_us);
    RT_UNUSED(completed_at_us);
    RT_UNUSED(timing_valid);
    RT_UNUSED(setup);
    RT_UNUSED(return_status);
    RT_UNUSED(response);
}

rt_bool_t usb_evidence_clock_us(rt_uint32_t *timestamp_us)
{
    RT_UNUSED(timestamp_us);
    return RT_FALSE;
}

void usb_evidence_record_enumeration_complete(rt_int32_t return_status)
{
    RT_UNUSED(return_status);
}

void usb_evidence_record_disconnect(void)
{
}

void usb_evidence_set_attempt_id(rt_uint32_t attempt_id)
{
    RT_UNUSED(attempt_id);
}

rt_bool_t usb_evidence_get_snapshot(usb_evidence_snapshot_t *snapshot)
{
    evidence_stub_ensure_initialized();
    if (snapshot != RT_NULL)
    {
        *snapshot = s_snapshot;
        return RT_TRUE;
    }
    return RT_FALSE;
}

rt_bool_t usb_evidence_get_snapshot_meta(rt_uint32_t *session_id,
                                         rt_uint32_t *revision)
{
    evidence_stub_ensure_initialized();
    if (session_id != RT_NULL)
    {
        *session_id = s_snapshot.session_id;
    }
    if (revision != RT_NULL)
    {
        *revision = s_snapshot.revision;
    }
    return RT_TRUE;
}

const char *usb_evidence_session_state_name(usb_evidence_session_state_t state)
{
    switch (state)
    {
    case USB_EVIDENCE_SESSION_WAITING:
        return "WAITING";
    case USB_EVIDENCE_SESSION_RECORDING:
        return "RECORDING";
    case USB_EVIDENCE_SESSION_ANALYZING:
        return "ANALYZING";
    case USB_EVIDENCE_SESSION_FINAL:
        return "FINAL";
    case USB_EVIDENCE_SESSION_DEGRADED:
        return "DEGRADED";
    case USB_EVIDENCE_SESSION_FAILED:
        return "FAILED";
    default:
        return "UNKNOWN";
    }
}

const char *usb_evidence_integrity_name(usb_evidence_integrity_t integrity)
{
    switch (integrity)
    {
    case USB_EVIDENCE_INTEGRITY_COMPLETE:
        return "COMPLETE";
    case USB_EVIDENCE_INTEGRITY_DEGRADED:
        return "DEGRADED";
    case USB_EVIDENCE_INTEGRITY_UNAVAILABLE:
        return "UNAVAILABLE";
    default:
        return "UNKNOWN";
    }
}

const char *usb_evidence_capture_status_name(
    usb_evidence_capture_status_t status)
{
    switch (status)
    {
    case USB_EVIDENCE_CAPTURE_COMPLETE:
        return "COMPLETE";
    case USB_EVIDENCE_CAPTURE_LOCAL_TRUNCATED:
        return "LOCAL_TRUNCATED";
    case USB_EVIDENCE_CAPTURE_NO_DATA_STAGE:
        return "NO_DATA_STAGE";
    case USB_EVIDENCE_CAPTURE_UNAVAILABLE:
        return "UNAVAILABLE";
    case USB_EVIDENCE_CAPTURE_NONE:
    default:
        return "NONE";
    }
}
