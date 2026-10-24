/*
 * usb_evidence_wrapper.c - CherryUSB host-observation hooks
 *
 * Strong definitions override the __WEAK hooks in usbh_core.c (PATCH-Moni:
 * USB evidence observers). Called from the hub thread context during
 * enumeration / control transfers / port release. They only copy bounded
 * evidence and never parse descriptors, render the UI, or wait for network
 * activity — the analysis thread (usb_diag) does the heavy work.
 */
#include <rtthread.h>
#include "usbh_core.h"
#include "usb_evidence.h"
#include "usb_descriptor_store.h"
#include "usb_perf.h"

bool usbh_control_transfer_started_observer(struct usbh_hubport *hport,
                                            uint32_t *started_at_us)
{
    rt_uint32_t timestamp_us;

    if (hport == NULL || started_at_us == NULL ||
        usb_evidence_clock_us(&timestamp_us) != RT_TRUE)
    {
        return false;
    }
    *started_at_us = timestamp_us;
    return true;
}

bool usbh_control_transfer_completed_observer(struct usbh_hubport *hport,
                                              uint32_t *completed_at_us)
{
    rt_uint32_t timestamp_us;

    if (hport == NULL || completed_at_us == NULL ||
        usb_evidence_clock_us(&timestamp_us) != RT_TRUE)
    {
        return false;
    }
    *completed_at_us = timestamp_us;
    return true;
}

static void usb_evidence_copy_setup(usb_evidence_setup_t *destination,
                                    const struct usb_setup_packet *source)
{
    /* USB setup packets are byte-packed: the first 16-bit field starts at
     * offset 2.  Read every byte explicitly so -O2 cannot combine the
     * accesses into an unaligned 32-bit load on Cortex-M7. */
    /* Keep the loads volatile.  A packed USB setup packet has 16-bit
     * members at offsets 2/4/6; with -O2 a non-volatile byte sequence may be
     * legally coalesced into an unaligned 32-bit load by GCC. */
    volatile const uint8_t *raw = (volatile const uint8_t *)source;

    destination->bm_request_type = raw[0];
    destination->b_request = raw[1];
    destination->w_value = (rt_uint16_t)raw[2] |
                           ((rt_uint16_t)raw[3] << 8);
    destination->w_index = (rt_uint16_t)raw[4] |
                           ((rt_uint16_t)raw[5] << 8);
    destination->w_length = (rt_uint16_t)raw[6] |
                            ((rt_uint16_t)raw[7] << 8);
}

void usbh_control_transfer_observer(struct usbh_hubport *hport,
                                    const struct usb_setup_packet *setup,
                                    const uint8_t *buffer,
                                    int result,
                                    uint32_t started_at_us,
                                    uint32_t completed_at_us,
                                    bool timing_valid)
{
    usb_evidence_setup_t evidence_setup;
    rt_uint32_t callback_start = usb_perf_now();

    if (!hport || !setup)
    {
        usb_perf_record(USB_PERF_HOST_CALLBACK, callback_start,
                        usb_perf_now());
        return;
    }

    /* Newer CherryUSB (DWC2) counts the 8-byte setup stage in the control
     * transfer return value; usb_evidence expects the pure data length
     * (old CherryUSB semantics). Absorb the platform difference here. */
    if (result > 0)
    {
        result = (result >= 8) ? (result - 8) : 0;
    }

    /* Keep failures visible without serializing every successful transfer
     * on the 115200-baud console in the time-sensitive host callback. The
     * bounded evidence queue below still records every observed transfer. */
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    if (result < 0)
    {
        rt_kprintf("[ev] CTRL %02X %02X %04X %04X %04X ret=%d cap<=%u\r\n",
                   setup->bmRequestType, setup->bRequest, setup->wValue,
                   setup->wIndex, setup->wLength, result,
                   (result > 0) ? (unsigned)((result < setup->wLength) ?
                                 result : setup->wLength) : 0U);
    }
#endif
    usb_evidence_copy_setup(&evidence_setup, setup);
    usb_descriptor_store_record_control(hport, setup, buffer, result);
    usb_evidence_record_control(hport->speed, hport->port,
                                started_at_us, completed_at_us,
                                timing_valid ? RT_TRUE : RT_FALSE,
                                &evidence_setup, result, buffer);
    usb_perf_record(USB_PERF_HOST_CALLBACK, callback_start, usb_perf_now());
}

void usbh_enumeration_started_observer(struct usbh_hubport *hport)
{
    if (hport)
    {
        usb_descriptor_store_begin_device(hport);
        usb_evidence_record_port_reset(hport->port, hport->speed);
    }
}

void usbh_device_descriptor_observer(struct usbh_hubport *hport,
                                     const uint8_t *buffer,
                                     uint16_t length)
{
    usb_descriptor_store_record_device(hport, buffer, length);
}

void usbh_configuration_descriptor_observer(struct usbh_hubport *hport,
                                            uint8_t config_index,
                                            const uint8_t *buffer,
                                            uint16_t length,
                                            uint16_t declared_length,
                                            int result)
{
    usb_descriptor_store_record_configuration(hport, config_index, buffer,
                                               length, declared_length, result);
}

void usbh_class_binding_observer(struct usbh_hubport *hport,
                                 uint8_t interface_number,
                                 const struct usbh_class_driver *driver,
                                 int result)
{
    usb_descriptor_store_record_class_binding(
        hport, interface_number, driver ? driver->driver_name : RT_NULL,
        result);
}

void usbh_enumeration_finished_observer(struct usbh_hubport *hport,
                                        int result)
{
    usb_descriptor_store_finish(hport, result);
    usb_evidence_record_enumeration_complete(result);
}

void usbh_hubport_release_observer(struct usbh_hubport *hport)
{
    if (hport)
    {
        usb_descriptor_store_disconnect();
        usb_evidence_record_disconnect();
    }
}
