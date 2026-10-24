/* SPDX-License-Identifier: Apache-2.0 */
#include <rtthread.h>

#include "usbdiag_session.h"

#ifdef RT_USING_COMPONENTS_INIT

static const rt_uint8_t s_device_descriptor[18] = {
    18U, 1U, 0x10U, 0x02U, 0U, 0U, 0U, 64U,
    0x34U, 0x12U, 0x78U, 0x56U, 0U, 1U, 1U, 2U, 3U, 1U
};

static const rt_uint8_t s_configuration_descriptor[25] = {
    9U, 2U, 25U, 0U, 1U, 1U, 0U, 0x80U, 50U,
    9U, 4U, 0U, 0U, 1U, 3U, 1U, 1U, 0U,
    7U, 5U, 0x81U, 3U, 8U, 0U, 10U
};

static usbdiag_control_event_t make_event(uint32_t sequence,
                                          uint8_t request,
                                          uint16_t value,
                                          uint16_t requested_length,
                                          const rt_uint8_t *payload,
                                          uint16_t payload_length)
{
    usbdiag_control_event_t event;
    rt_memset(&event, 0, sizeof(event));
    event.session_id = 1U;
    event.attempt_id = 1U;
    event.sequence = sequence;
    event.speed = USBDIAG_SPEED_FULL;
    event.has_setup = true;
    event.setup.bm_request_type = request == 6U ? 0x80U : 0U;
    event.setup.request = request;
    event.setup.value = value;
    event.setup.length = requested_length;
    event.transfer_status = USBDIAG_TRANSFER_SUCCESS;
    event.capture_status = payload_length == 0U ?
        USBDIAG_CAPTURE_NO_DATA : USBDIAG_CAPTURE_COMPLETE;
    event.transfer_length_known = true;
    event.transfer_length = payload_length;
    event.captured_length = payload_length;
    event.payload = payload;
    event.payload_size = payload_length;
    return event;
}

static int usbdiag_package_smoke(void)
{
    usbdiag_session_t session;
    usbdiag_control_event_t event;
    rt_uint8_t index;

    if (usbdiag_session_begin(&session, 1U, 1U, USBDIAG_SPEED_FULL) !=
        USBDIAG_SESSION_NOT_FINAL)
        goto failed;

    event = make_event(1U, 6U, 0x0100U, 8U, s_device_descriptor, 8U);
    if (usbdiag_session_feed(&session, &event) == USBDIAG_SESSION_INVALID)
        goto failed;
    event = make_event(2U, 6U, 0x0100U, sizeof(s_device_descriptor),
                       s_device_descriptor, sizeof(s_device_descriptor));
    if (usbdiag_session_feed(&session, &event) == USBDIAG_SESSION_INVALID)
        goto failed;
    event = make_event(3U, 6U, 0x0200U, 9U,
                       s_configuration_descriptor, 9U);
    if (usbdiag_session_feed(&session, &event) == USBDIAG_SESSION_INVALID)
        goto failed;
    event = make_event(4U, 6U, 0x0200U,
                       sizeof(s_configuration_descriptor),
                       s_configuration_descriptor,
                       sizeof(s_configuration_descriptor));
    if (usbdiag_session_feed(&session, &event) == USBDIAG_SESSION_INVALID)
        goto failed;
    event = make_event(5U, 5U, 1U, 0U, RT_NULL, 0U);
    if (usbdiag_session_feed(&session, &event) == USBDIAG_SESSION_INVALID)
        goto failed;
    event = make_event(6U, 9U, 1U, 0U, RT_NULL, 0U);
    if (usbdiag_session_feed(&session, &event) == USBDIAG_SESSION_INVALID)
        goto failed;
    if (usbdiag_session_end(&session, true) != USBDIAG_SESSION_COMPLETE)
        goto failed;

    for (index = 0U; index < USBDIAG_RULE_COUNT; index++)
    {
        if (session.rules[index].verdict != USBDIAG_PASS &&
            session.rules[index].verdict != USBDIAG_NOT_APPLICABLE)
            goto failed;
    }
    rt_kprintf("USB_ENUM_DIAG_TEST: PASS version=%s rules=%u\n",
               usbdiag_version(), (unsigned int)USBDIAG_RULE_COUNT);
    return 0;

failed:
    rt_kprintf("USB_ENUM_DIAG_TEST: FAIL version=%s\n", usbdiag_version());
    return -RT_ERROR;
}

INIT_APP_EXPORT(usbdiag_package_smoke);

#ifdef RT_USING_FINSH
#include <finsh.h>
static int usbdiag_pkg_test(int argc, char **argv)
{
    RT_UNUSED(argc);
    RT_UNUSED(argv);
    return usbdiag_package_smoke();
}
MSH_CMD_EXPORT(usbdiag_pkg_test, run the usb_enum_diag package session smoke test);
#endif
#endif
