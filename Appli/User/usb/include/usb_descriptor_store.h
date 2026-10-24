/*
 * Complete USB descriptor inventory for the ART-Pi2 USB host.
 *
 * The rolling USB evidence queue intentionally keeps only a small control
 * transfer history.  This module is the separate, bounded store for the
 * complete Device/Configuration data and the structured interface index.
 */
#ifndef USB_DESCRIPTOR_STORE_H
#define USB_DESCRIPTOR_STORE_H

#include <rtthread.h>

#include "usb_config.h"
#include "usb_util.h"
#include "usb_def.h"

#ifdef __cplusplus
extern "C" {
#endif

#define USB_DESCRIPTOR_CONFIG_BLOB_MAX       65536U
#define USB_DESCRIPTOR_STRING_BLOB_MAX      16384U
#define USB_DESCRIPTOR_MAX_CONFIGURATIONS   8U
#define USB_DESCRIPTOR_MAX_INTERFACES       128U
#define USB_DESCRIPTOR_MAX_ENDPOINTS        256U
#define USB_DESCRIPTOR_MAX_NODES            256U
#define USB_DESCRIPTOR_MAX_FIELDS           512U
#define USB_DESCRIPTOR_MAX_STRINGS          64U
#define USB_DESCRIPTOR_CLASS_NAME_MAX       32U
#define USB_DESCRIPTOR_DRIVER_NAME_MAX      32U
#define USB_DESCRIPTOR_TYPE_NAME_MAX         32U
#define USB_DESCRIPTOR_SUMMARY_MAX           64U
#define USB_DESCRIPTOR_STRING_TEXT_MAX       128U

typedef struct
{
    rt_uint8_t index;
    rt_uint8_t configuration_value;
    rt_uint8_t interface_count;
    rt_uint8_t string_index;
    rt_uint32_t raw_offset;
    rt_uint16_t declared_length;
    rt_uint16_t captured_length;
    rt_bool_t complete;
    rt_bool_t parsed;
    rt_bool_t malformed;
    rt_bool_t truncated;
    char status[32];
} usb_descriptor_config_info_t;

typedef struct
{
    rt_uint8_t config_index;
    rt_uint8_t interface_number;
    rt_uint8_t alternate_setting;
    rt_uint8_t endpoint_count;
    rt_uint8_t class_code;
    rt_uint8_t subclass;
    rt_uint8_t protocol;
    rt_uint8_t string_index;
    rt_uint16_t node_id;
    rt_uint16_t endpoint_start;
    rt_bool_t class_bound;
    rt_bool_t class_bind_failed;
    char class_name[USB_DESCRIPTOR_CLASS_NAME_MAX];
    char driver_name[USB_DESCRIPTOR_DRIVER_NAME_MAX];
    char interface_name[USB_DESCRIPTOR_STRING_TEXT_MAX];
} usb_descriptor_interface_info_t;

typedef struct
{
    rt_uint8_t config_index;
    rt_uint8_t interface_number;
    rt_uint8_t alternate_setting;
    rt_uint8_t address;
    rt_uint8_t attributes;
    rt_uint8_t interval;
    rt_uint16_t max_packet_size;
    rt_uint16_t node_id;
} usb_descriptor_endpoint_info_t;

typedef struct
{
    rt_uint16_t node_id;
    rt_uint16_t parent_id;
    rt_uint16_t offset;
    rt_uint16_t length;
    rt_uint16_t field_start;
    rt_uint8_t field_count;
    rt_uint8_t config_index;
    rt_uint8_t descriptor_type;
    rt_uint8_t interface_number;
    rt_uint8_t alternate_setting;
    rt_bool_t truncated;
    char type_name[USB_DESCRIPTOR_TYPE_NAME_MAX];
    char summary[USB_DESCRIPTOR_SUMMARY_MAX];
} usb_descriptor_node_t;

typedef struct
{
    rt_uint16_t node_id;
    rt_uint8_t byte_offset;
    rt_uint8_t byte_length;
    char name[24];
    char value[96];
} usb_descriptor_field_t;

typedef struct
{
    rt_uint8_t index;
    rt_uint16_t raw_offset;
    rt_uint16_t raw_length;
    rt_uint16_t declared_length;
    char text[USB_DESCRIPTOR_STRING_TEXT_MAX];
} usb_descriptor_string_info_t;

typedef struct
{
    rt_uint32_t generation;
    rt_uint8_t port;
    rt_uint8_t hub_index;
    rt_uint8_t speed;
    rt_uint8_t device_class;
    rt_uint8_t device_subclass;
    rt_uint8_t device_protocol;
    rt_uint8_t language_count;
    rt_uint16_t language_id;
    rt_uint16_t vendor_id;
    rt_uint16_t product_id;
    rt_uint16_t bcd_usb;
    rt_uint16_t bcd_device;
    rt_uint8_t device_string_indices[3];
    rt_uint8_t declared_configurations;
    rt_uint8_t stored_configurations;
    rt_uint8_t active_config_index;
    rt_uint8_t reserved;
    rt_uint32_t config_declared_bytes;
    rt_uint32_t config_captured_bytes;
    rt_uint32_t string_captured_bytes;
    rt_uint32_t config_crc32;
    rt_uint16_t interface_count;
    rt_uint16_t endpoint_count;
    rt_uint16_t node_count;
    rt_uint16_t field_count;
    rt_uint16_t string_count;
    rt_uint16_t class_bind_failures;
    rt_uint16_t unknown_interface_count;
    rt_bool_t present;
    rt_bool_t enumeration_complete;
    rt_bool_t config_complete;
    rt_bool_t config_truncated;
    rt_bool_t node_truncated;
    rt_bool_t interface_truncated;
    rt_bool_t endpoint_truncated;
    char manufacturer[USB_DESCRIPTOR_STRING_TEXT_MAX];
    char product[USB_DESCRIPTOR_STRING_TEXT_MAX];
    char serial[USB_DESCRIPTOR_STRING_TEXT_MAX];
    char device_class_name[USB_DESCRIPTOR_CLASS_NAME_MAX];
    char overflow_reason[32];
} usb_descriptor_manifest_t;

struct usbh_hubport;
struct usb_setup_packet;

int usb_descriptor_store_init(void);
void usb_descriptor_store_begin_device(const struct usbh_hubport *hport);
void usb_descriptor_store_record_device(const struct usbh_hubport *hport,
                                        const rt_uint8_t *data,
                                        rt_uint16_t length);
void usb_descriptor_store_record_configuration(
    const struct usbh_hubport *hport, rt_uint8_t config_index,
    const rt_uint8_t *data, rt_uint16_t length,
    rt_uint16_t declared_length, rt_int32_t result);
void usb_descriptor_store_record_control(const struct usbh_hubport *hport,
                                         const struct usb_setup_packet *setup,
                                         const rt_uint8_t *data,
                                         rt_int32_t result);
void usb_descriptor_store_record_class_binding(
    const struct usbh_hubport *hport, rt_uint8_t interface_number,
    const char *driver_name, rt_int32_t result);
void usb_descriptor_store_finish(const struct usbh_hubport *hport,
                                 rt_int32_t result);
void usb_descriptor_store_disconnect(void);

rt_bool_t usb_descriptor_store_get_manifest(usb_descriptor_manifest_t *manifest);
rt_size_t usb_descriptor_store_copy_config(rt_uint8_t config_index,
                                           rt_uint32_t offset,
                                           rt_uint8_t *buffer,
                                           rt_size_t buffer_size);
rt_size_t usb_descriptor_store_copy_device_raw(rt_uint32_t offset,
                                               rt_uint8_t *buffer,
                                               rt_size_t buffer_size);
rt_size_t usb_descriptor_store_copy_string_raw(rt_uint8_t string_index,
                                               rt_uint32_t offset,
                                               rt_uint8_t *buffer,
                                               rt_size_t buffer_size);
rt_bool_t usb_descriptor_store_get_config_info(
    rt_uint8_t config_index, usb_descriptor_config_info_t *info);
rt_size_t usb_descriptor_store_copy_configs(
    usb_descriptor_config_info_t *configs, rt_size_t capacity);
rt_size_t usb_descriptor_store_copy_interfaces(
    usb_descriptor_interface_info_t *interfaces, rt_size_t capacity);
rt_bool_t usb_descriptor_store_get_interface(
    rt_uint16_t index, usb_descriptor_interface_info_t *interface_info);
rt_size_t usb_descriptor_store_copy_endpoints(
    usb_descriptor_endpoint_info_t *endpoints, rt_size_t capacity);
rt_bool_t usb_descriptor_store_get_endpoint(
    rt_uint16_t index, usb_descriptor_endpoint_info_t *endpoint_info);
rt_size_t usb_descriptor_store_copy_nodes(usb_descriptor_node_t *nodes,
                                          rt_size_t capacity);
rt_size_t usb_descriptor_store_copy_fields(usb_descriptor_field_t *fields,
                                           rt_size_t capacity);
rt_size_t usb_descriptor_store_copy_strings(
    usb_descriptor_string_info_t *strings, rt_size_t capacity);
rt_bool_t usb_descriptor_store_copy_node_raw(rt_uint16_t node_id,
                                             rt_uint8_t *buffer,
                                             rt_size_t buffer_size,
                                             rt_size_t *copied);
rt_bool_t usb_descriptor_store_get_string_info(
    rt_uint8_t string_index, usb_descriptor_string_info_t *info);

#ifdef __cplusplus
}
#endif

#endif /* USB_DESCRIPTOR_STORE_H */
