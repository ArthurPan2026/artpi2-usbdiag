/*
 * ART-Pi2 complete USB descriptor inventory.
 *
 * This module is deliberately independent from the rolling evidence queue.
 * CherryUSB gives the observers a live, non-cacheable transfer buffer; the
 * observer copies the bytes here before the next control transfer can reuse
 * that buffer.  All long-lived storage is in PSRAM, not the RT-Thread heap.
 */
#include <rtthread.h>
#include <string.h>

#include "usbh_core.h"
#include "usb_desc_parser.h"
#include "usb_descriptor_store.h"

struct usb_descriptor_store_state
{
    usb_descriptor_manifest_t manifest;
    usb_descriptor_config_info_t configs[USB_DESCRIPTOR_MAX_CONFIGURATIONS];
    usb_descriptor_interface_info_t
        interfaces[USB_DESCRIPTOR_MAX_INTERFACES];
    usb_descriptor_endpoint_info_t
        endpoints[USB_DESCRIPTOR_MAX_ENDPOINTS];
    usb_descriptor_node_t nodes[USB_DESCRIPTOR_MAX_NODES];
    usb_descriptor_field_t fields[USB_DESCRIPTOR_MAX_FIELDS];
    usb_descriptor_string_info_t strings[USB_DESCRIPTOR_MAX_STRINGS];
    rt_uint8_t device_raw[USB_SIZEOF_DEVICE_DESC];
    rt_uint8_t config_blob[USB_DESCRIPTOR_CONFIG_BLOB_MAX];
    rt_uint8_t string_blob[USB_DESCRIPTOR_STRING_BLOB_MAX];
};

static struct usb_descriptor_store_state g_store USB_DESCRIPTOR_RAM_SECTION;
static struct rt_mutex g_store_mutex;
static rt_bool_t g_store_ready;
static rt_uint32_t g_next_generation;

#define USB_DESCRIPTOR_STORE_LOCK_TIMEOUT_MS 100U

static const char *usb_descriptor_class_name(rt_uint8_t class_code)
{
    switch (class_code)
    {
    case 0x00U: return "Interface Defined";
    case 0x01U: return "Audio";
    case 0x02U: return "CDC Communication";
    case 0x03U: return "HID";
    case 0x04U: return "Monitor";
    case 0x05U: return "Physical";
    case 0x06U: return "Image";
    case 0x07U: return "Printer";
    case 0x08U: return "Mass Storage";
    case 0x09U: return "Hub";
    case 0x0AU: return "CDC Data";
    case 0x0BU: return "Smart Card";
    case 0x0DU: return "Content Security";
    case 0x0EU: return "Video";
    case 0x0FU: return "Healthcare";
    case 0xDCU: return "Diagnostic";
    case 0xE0U: return "Wireless";
    case 0xEFU: return "Miscellaneous/Composite";
    case 0xFEU: return "Application Specific";
    case 0xFFU: return "Vendor Specific";
    default: return "Unknown Class";
    }
}

static rt_uint16_t read_le16(const rt_uint8_t *data)
{
    return (rt_uint16_t)data[0] | ((rt_uint16_t)data[1] << 8U);
}

static rt_uint32_t descriptor_crc32(const rt_uint8_t *data, rt_size_t length)
{
    rt_uint32_t crc = 0xFFFFFFFFU;
    rt_size_t index;

    if (!data)
    {
        return 0U;
    }
    for (index = 0U; index < length; index++)
    {
        rt_uint8_t bit;

        crc ^= data[index];
        for (bit = 0U; bit < 8U; bit++)
        {
            crc = (crc >> 1U) ^
                  ((crc & 1U) != 0U ? 0xEDB88320U : 0U);
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

static void copy_text(char *destination, rt_size_t size, const char *source)
{
    if (!destination || size == 0U)
    {
        return;
    }
    if (!source)
    {
        destination[0] = '\0';
        return;
    }
    rt_strncpy(destination, source, size - 1U);
    destination[size - 1U] = '\0';
}

static void set_overflow_reason_locked(const char *reason)
{
    if (g_store.manifest.overflow_reason[0] == '\0')
    {
        copy_text(g_store.manifest.overflow_reason,
                  sizeof(g_store.manifest.overflow_reason), reason);
    }
}

static void append_utf8(char *text, rt_size_t size, rt_size_t *used,
                        rt_uint32_t codepoint)
{
    if (!text || !used || *used >= size - 1U)
    {
        return;
    }
    if (codepoint <= 0x7FU)
    {
        if (*used + 1U < size)
        {
            text[(*used)++] = (char)codepoint;
        }
    }
    else if (codepoint <= 0x7FFU)
    {
        if (*used + 2U < size)
        {
            text[(*used)++] = (char)(0xC0U | (codepoint >> 6U));
            text[(*used)++] = (char)(0x80U | (codepoint & 0x3FU));
        }
    }
    else if (codepoint <= 0xFFFFU)
    {
        if (*used + 3U < size)
        {
            text[(*used)++] = (char)(0xE0U | (codepoint >> 12U));
            text[(*used)++] = (char)(0x80U | ((codepoint >> 6U) & 0x3FU));
            text[(*used)++] = (char)(0x80U | (codepoint & 0x3FU));
        }
    }
    else if (*used + 4U < size)
    {
        text[(*used)++] = (char)(0xF0U | (codepoint >> 18U));
        text[(*used)++] = (char)(0x80U | ((codepoint >> 12U) & 0x3FU));
        text[(*used)++] = (char)(0x80U | ((codepoint >> 6U) & 0x3FU));
        text[(*used)++] = (char)(0x80U | (codepoint & 0x3FU));
    }
}

static void string_to_utf8(const rt_uint8_t *raw, rt_uint16_t length,
                           char *text, rt_size_t text_size)
{
    rt_size_t used = 0U;
    rt_uint16_t offset;

    if (!text || text_size == 0U)
    {
        return;
    }
    text[0] = '\0';
    if (!raw || length < 2U)
    {
        return;
    }
    for (offset = 2U; offset + 1U < length; offset += 2U)
    {
        rt_uint32_t codepoint = read_le16(raw + offset);

        /* Properly combine UTF-16 surrogate pairs. */
        if (codepoint >= 0xD800U && codepoint <= 0xDBFFU &&
            offset + 3U < length)
        {
            rt_uint32_t low = read_le16(raw + offset + 2U);

            if (low >= 0xDC00U && low <= 0xDFFFU)
            {
                codepoint = 0x10000U +
                            ((codepoint - 0xD800U) << 10U) +
                            (low - 0xDC00U);
                offset += 2U;
            }
            else
            {
                codepoint = 0xFFFD;
            }
        }
        else if (codepoint >= 0xD800U && codepoint <= 0xDFFFU)
        {
            codepoint = 0xFFFD;
        }
        append_utf8(text, text_size, &used, codepoint);
        if (used >= text_size - 1U)
        {
            break;
        }
    }
    text[used] = '\0';
}

static rt_bool_t store_lock(void)
{
    return g_store_ready &&
           rt_mutex_take(&g_store_mutex,
                         rt_tick_from_millisecond(
                             USB_DESCRIPTOR_STORE_LOCK_TIMEOUT_MS)) == RT_EOK;
}

static void store_unlock(void)
{
    if (g_store_ready)
    {
        rt_mutex_release(&g_store_mutex);
    }
}

static rt_int16_t find_config_by_index_locked(rt_uint8_t index)
{
    rt_uint8_t i;

    for (i = 0U; i < g_store.manifest.stored_configurations; i++)
    {
        if (g_store.configs[i].index == index)
        {
            return (rt_int16_t)i;
        }
    }
    return -1;
}

static rt_int16_t find_config_by_value_locked(rt_uint8_t value)
{
    rt_uint8_t i;

    for (i = 0U; i < g_store.manifest.stored_configurations; i++)
    {
        if (g_store.configs[i].configuration_value == value)
        {
            return (rt_int16_t)i;
        }
    }
    return -1;
}

static rt_uint16_t add_node_locked(rt_uint8_t config_index,
                                   rt_uint16_t offset, rt_uint16_t length,
                                   rt_uint16_t parent_id,
                                   rt_uint8_t interface_number,
                                   rt_uint8_t alternate_setting,
                                   const rt_uint8_t *raw)
{
    usb_descriptor_node_t *node;
    usb_desc_node_t parsed;
    rt_uint16_t node_id;
    rt_uint8_t field_index;

    if (g_store.manifest.node_count >= USB_DESCRIPTOR_MAX_NODES)
    {
        g_store.manifest.node_truncated = RT_TRUE;
        return 0xFFFFU;
    }
    node_id = g_store.manifest.node_count++;
    node = &g_store.nodes[node_id];
    rt_memset(node, 0, sizeof(*node));
    node->node_id = node_id;
    node->parent_id = parent_id;
    node->offset = offset;
    node->length = length;
    node->config_index = config_index;
    node->interface_number = interface_number;
    node->alternate_setting = alternate_setting;
    node->descriptor_type = raw && length >= 2U ? raw[1] : 0U;
    node->field_start = g_store.manifest.field_count;

    if (raw && length >= 2U && usb_desc_parse_single(raw, length, &parsed))
    {
        copy_text(node->type_name, sizeof(node->type_name), parsed.type_name);
        copy_text(node->summary, sizeof(node->summary), parsed.summary);
        for (field_index = 0U; field_index < parsed.field_count; field_index++)
        {
            usb_descriptor_field_t *field;

            if (g_store.manifest.field_count >= USB_DESCRIPTOR_MAX_FIELDS)
            {
                g_store.manifest.node_truncated = RT_TRUE;
                break;
            }
            field = &g_store.fields[g_store.manifest.field_count++];
            rt_memset(field, 0, sizeof(*field));
            field->node_id = node_id;
            field->byte_offset = parsed.field_offsets[field_index];
            field->byte_length = parsed.field_lengths[field_index];
            copy_text(field->name, sizeof(field->name),
                      parsed.field_names[field_index]);
            copy_text(field->value, sizeof(field->value),
                      parsed.field_values[field_index]);
            node->field_count++;
        }
    }
    else
    {
        copy_text(node->type_name, sizeof(node->type_name),
                  "Other/Class Descriptor");
        rt_snprintf(node->summary, sizeof(node->summary),
                    "Raw descriptor type 0x%02X, %u bytes",
                    node->descriptor_type, length);
        node->truncated = RT_TRUE;
    }
    return node_id;
}

static rt_int16_t add_interface_locked(rt_uint8_t config_index,
                                       rt_uint8_t interface_number,
                                       rt_uint8_t alternate_setting,
                                       rt_uint8_t endpoint_count,
                                       rt_uint8_t class_code,
                                       rt_uint8_t subclass,
                                       rt_uint8_t protocol,
                                       rt_uint8_t string_index,
                                       rt_uint16_t node_id)
{
    usb_descriptor_interface_info_t *info;
    rt_uint16_t index;

    if (g_store.manifest.interface_count >= USB_DESCRIPTOR_MAX_INTERFACES)
    {
        g_store.manifest.interface_truncated = RT_TRUE;
        return -1;
    }
    index = g_store.manifest.interface_count++;
    info = &g_store.interfaces[index];
    rt_memset(info, 0, sizeof(*info));
    info->config_index = config_index;
    info->interface_number = interface_number;
    info->alternate_setting = alternate_setting;
    info->endpoint_count = 0U;
    info->class_code = class_code;
    info->subclass = subclass;
    info->protocol = protocol;
    info->string_index = string_index;
    info->node_id = node_id;
    info->endpoint_start = g_store.manifest.endpoint_count;
    copy_text(info->class_name, sizeof(info->class_name),
              usb_descriptor_class_name(class_code));
    return (rt_int16_t)index;
}

static void add_endpoint_locked(rt_uint8_t config_index,
                                rt_uint8_t interface_number,
                                rt_uint8_t alternate_setting,
                                const rt_uint8_t *raw,
                                rt_uint16_t node_id,
                                rt_int16_t interface_index)
{
    usb_descriptor_endpoint_info_t *endpoint;

    if (g_store.manifest.endpoint_count >= USB_DESCRIPTOR_MAX_ENDPOINTS)
    {
        g_store.manifest.endpoint_truncated = RT_TRUE;
        return;
    }
    endpoint = &g_store.endpoints[g_store.manifest.endpoint_count++];
    rt_memset(endpoint, 0, sizeof(*endpoint));
    endpoint->config_index = config_index;
    endpoint->interface_number = interface_number;
    endpoint->alternate_setting = alternate_setting;
    endpoint->address = raw[2];
    endpoint->attributes = raw[3];
    endpoint->max_packet_size = read_le16(raw + 4U);
    endpoint->interval = raw[6];
    endpoint->node_id = node_id;
    if (interface_index >= 0)
    {
        usb_descriptor_interface_info_t *info =
            &g_store.interfaces[interface_index];

        if (info->endpoint_count != 0xFFU)
        {
            info->endpoint_count++;
        }
    }
}

static void update_interface_node_summary_locked(
    const usb_descriptor_interface_info_t *info)
{
    usb_descriptor_node_t *node;
    const char *status;

    if (!info || info->node_id >= g_store.manifest.node_count)
    {
        return;
    }
    node = &g_store.nodes[info->node_id];
    if (info->class_bound)
    {
        status = info->driver_name[0] != '\0' ? info->driver_name : "bound";
    }
    else if (info->class_bind_failed)
    {
        status = "bind failed";
    }
    else
    {
        status = "diagnostic-only";
    }
    rt_snprintf(node->summary, sizeof(node->summary),
                "Interface %u alt %u class 0x%02X [%s]",
                info->interface_number, info->alternate_setting,
                info->class_code, status);
}

static void parse_configuration_locked(rt_uint8_t config_index,
                                       usb_descriptor_config_info_t *config,
                                       const rt_uint8_t *data,
                                       rt_uint16_t length)
{
    rt_uint16_t offset;
    rt_uint16_t root_id;
    rt_uint16_t current_interface_node = 0xFFFFU;
    rt_int16_t current_interface = -1;
    rt_uint8_t current_interface_number = 0xFFU;
    rt_uint8_t current_alt_setting = 0xFFU;
    rt_uint8_t interface_seen[256] = {0};
    rt_uint8_t unique_interfaces = 0U;

    if (!config || !data || length < 2U)
    {
        return;
    }
    root_id = add_node_locked(config_index, 0U,
                              length >= 9U ? 9U : length,
                              0xFFFFU, 0xFFU, 0xFFU, data);
    offset = length >= 9U ? 9U : length;
    while (offset < length)
    {
        rt_uint8_t descriptor_length;
        rt_uint8_t descriptor_type;
        rt_uint16_t node_id;

        if (length - offset < 2U)
        {
            config->malformed = RT_TRUE;
            break;
        }
        descriptor_length = data[offset];
        descriptor_type = data[offset + 1U];
        if (descriptor_length < 2U || descriptor_length > length - offset)
        {
            config->malformed = RT_TRUE;
            break;
        }

        if (descriptor_type == USB_DESCRIPTOR_TYPE_INTERFACE &&
            descriptor_length >= USB_SIZEOF_INTERFACE_DESC)
        {
            const rt_uint8_t *intf = data + offset;

            current_interface_number = intf[2];
            current_alt_setting = intf[3];
            node_id = add_node_locked(config_index, offset, descriptor_length,
                                      root_id, current_interface_number,
                                      current_alt_setting, intf);
            current_interface_node = node_id;
            current_interface = add_interface_locked(
                config_index, intf[2], intf[3], intf[4], intf[5], intf[6],
                intf[7], intf[8], node_id);
            if (!interface_seen[intf[2]])
            {
                interface_seen[intf[2]] = 1U;
                unique_interfaces++;
            }
        }
        else if (descriptor_type == USB_DESCRIPTOR_TYPE_ENDPOINT &&
                 descriptor_length >= USB_SIZEOF_ENDPOINT_DESC)
        {
            node_id = add_node_locked(config_index, offset, descriptor_length,
                                      current_interface_node != 0xFFFFU ?
                                      current_interface_node : root_id,
                                      current_interface_number,
                                      current_alt_setting, data + offset);
            add_endpoint_locked(config_index, current_interface_number,
                                current_alt_setting, data + offset, node_id,
                                current_interface);
        }
        else
        {
            node_id = add_node_locked(config_index, offset, descriptor_length,
                                      current_interface_node != 0xFFFFU ?
                                      current_interface_node : root_id,
                                      current_interface_number,
                                      current_alt_setting, data + offset);
        }
        RT_UNUSED(node_id);
        offset = (rt_uint16_t)(offset + descriptor_length);
    }
    config->interface_count = unique_interfaces;
    config->parsed = offset == length && !config->malformed;
}

static void update_string_references_locked(rt_uint8_t index,
                                            const char *text)
{
    rt_uint16_t i;

    /* Descriptor index 0 is a Language ID table, not a human-readable
     * string.  Never use its display text as a manufacturer/product/
     * interface name. */
    if (index == 0U)
    {
        return;
    }

    if (index == g_store.manifest.device_string_indices[0])
    {
        copy_text(g_store.manifest.manufacturer,
                  sizeof(g_store.manifest.manufacturer), text);
    }
    if (index == g_store.manifest.device_string_indices[1])
    {
        copy_text(g_store.manifest.product,
                  sizeof(g_store.manifest.product), text);
    }
    if (index == g_store.manifest.device_string_indices[2])
    {
        copy_text(g_store.manifest.serial,
                  sizeof(g_store.manifest.serial), text);
    }
    for (i = 0U; i < g_store.manifest.stored_configurations; i++)
    {
        if (g_store.configs[i].string_index == index)
        {
            /* Configuration text is currently exposed through the string
             * inventory; the compact configuration record has no text field. */
        }
    }
    for (i = 0U; i < g_store.manifest.interface_count; i++)
    {
        if (g_store.interfaces[i].string_index == index)
        {
            copy_text(g_store.interfaces[i].interface_name,
                      sizeof(g_store.interfaces[i].interface_name), text);
        }
    }
}

int usb_descriptor_store_init(void)
{
    if (g_store_ready)
    {
        return RT_EOK;
    }
    if (rt_mutex_init(&g_store_mutex, "usbdesc", RT_IPC_FLAG_FIFO) != RT_EOK)
    {
        return -RT_ENOMEM;
    }
    rt_memset(&g_store, 0, sizeof(g_store));
    g_store.manifest.active_config_index = 0xFFU;
    g_store_ready = RT_TRUE;
    return RT_EOK;
}

void usb_descriptor_store_begin_device(const struct usbh_hubport *hport)
{
    rt_uint32_t generation;

    if (!store_lock())
    {
        return;
    }
    generation = ++g_next_generation;
    rt_memset(&g_store, 0, sizeof(g_store));
    g_store.manifest.generation = generation;
    g_store.manifest.present = RT_TRUE;
    g_store.manifest.active_config_index = 0xFFU;
    if (hport)
    {
        g_store.manifest.port = hport->port;
        g_store.manifest.hub_index = hport->parent ? hport->parent->index : 0U;
        g_store.manifest.speed = hport->speed;
        g_store.manifest.device_class = hport->device_desc.bDeviceClass;
        g_store.manifest.device_subclass = hport->device_desc.bDeviceSubClass;
        g_store.manifest.device_protocol = hport->device_desc.bDeviceProtocol;
        copy_text(g_store.manifest.device_class_name,
                  sizeof(g_store.manifest.device_class_name),
                  usb_descriptor_class_name(hport->device_desc.bDeviceClass));
    }
    store_unlock();
}

void usb_descriptor_store_record_device(const struct usbh_hubport *hport,
                                        const rt_uint8_t *data,
                                        rt_uint16_t length)
{
    if (!store_lock())
    {
        return;
    }
    if (!g_store.manifest.present && hport)
    {
        /* A malformed or unusually fast device can reach this observer before
         * the start marker has been consumed.  Start a valid inventory anyway. */
        g_store.manifest.present = RT_TRUE;
    }
    if (hport)
    {
        g_store.manifest.vendor_id = hport->device_desc.idVendor;
        g_store.manifest.product_id = hport->device_desc.idProduct;
        g_store.manifest.bcd_usb = hport->device_desc.bcdUSB;
        g_store.manifest.bcd_device = hport->device_desc.bcdDevice;
        g_store.manifest.device_class = hport->device_desc.bDeviceClass;
        g_store.manifest.device_subclass = hport->device_desc.bDeviceSubClass;
        g_store.manifest.device_protocol = hport->device_desc.bDeviceProtocol;
        g_store.manifest.declared_configurations =
            hport->device_desc.bNumConfigurations;
        g_store.manifest.device_string_indices[0] =
            hport->device_desc.iManufacturer;
        g_store.manifest.device_string_indices[1] = hport->device_desc.iProduct;
        g_store.manifest.device_string_indices[2] =
            hport->device_desc.iSerialNumber;
        copy_text(g_store.manifest.device_class_name,
                  sizeof(g_store.manifest.device_class_name),
                  usb_descriptor_class_name(hport->device_desc.bDeviceClass));
    }
    if (data && length > 0U)
    {
        rt_uint16_t copy_length = length > sizeof(g_store.device_raw) ?
                                  sizeof(g_store.device_raw) : length;

        rt_memcpy(g_store.device_raw, data, copy_length);
        (void)add_node_locked(0xFFU, 0U, copy_length, 0xFFFFU, 0xFFU,
                              0xFFU, g_store.device_raw);
    }
    store_unlock();
}

void usb_descriptor_store_record_configuration(
    const struct usbh_hubport *hport, rt_uint8_t config_index,
    const rt_uint8_t *data, rt_uint16_t length,
    rt_uint16_t declared_length, rt_int32_t result)
{
    rt_uint32_t remaining;
    rt_uint16_t copy_length = 0U;
    rt_int16_t config_slot;
    usb_descriptor_config_info_t *config;

    RT_UNUSED(hport);
    if (!store_lock())
    {
        return;
    }
    config_slot = find_config_by_index_locked(config_index);
    if (config_slot >= 0)
    {
        config = &g_store.configs[config_slot];
    }
    else
    {
        if (g_store.manifest.stored_configurations >=
            USB_DESCRIPTOR_MAX_CONFIGURATIONS)
        {
            g_store.manifest.config_truncated = RT_TRUE;
            set_overflow_reason_locked("configuration_count_limit");
            store_unlock();
            return;
        }
        config_slot = g_store.manifest.stored_configurations++;
        config = &g_store.configs[config_slot];
        rt_memset(config, 0, sizeof(*config));
        config->index = config_index;
    }
    config->declared_length = declared_length;
    config->captured_length = 0U;
    config->raw_offset = g_store.manifest.config_captured_bytes;
    config->complete = RT_FALSE;
    config->parsed = RT_FALSE;
    config->malformed = RT_FALSE;
    config->truncated = RT_FALSE;
    copy_text(config->status, sizeof(config->status), "complete");
    if (data && length > 0U && result >= 0)
    {
        remaining = USB_DESCRIPTOR_CONFIG_BLOB_MAX -
                    g_store.manifest.config_captured_bytes;
        copy_length = length > remaining ? (rt_uint16_t)remaining : length;
        if (copy_length > 0U)
        {
            rt_memcpy(g_store.config_blob +
                      g_store.manifest.config_captured_bytes,
                      data, copy_length);
            g_store.manifest.config_captured_bytes += copy_length;
            config->captured_length = copy_length;
        }
    }
    if (declared_length >= USB_SIZEOF_CONFIG_DESC && data && length >= 5U)
    {
        config->configuration_value = data[5];
        config->string_index = data[6];
    }
    if (declared_length > CONFIG_USBHOST_REQUEST_BUFFER_LEN ||
        length < declared_length || copy_length < length)
    {
        config->truncated = RT_TRUE;
        g_store.manifest.config_truncated = RT_TRUE;
        if (declared_length > CONFIG_USBHOST_REQUEST_BUFFER_LEN)
        {
            copy_text(config->status, sizeof(config->status),
                      "request_buffer_limit");
            set_overflow_reason_locked("request_buffer_limit");
        }
        else if (copy_length < length)
        {
            copy_text(config->status, sizeof(config->status),
                      "config_blob_limit");
            set_overflow_reason_locked("config_blob_limit");
        }
        else
        {
            copy_text(config->status, sizeof(config->status),
                      "transfer_incomplete");
            set_overflow_reason_locked("transfer_incomplete");
        }
    }
    else if (result < 0)
    {
        copy_text(config->status, sizeof(config->status),
                  "control_transfer_failed");
        set_overflow_reason_locked("control_transfer_failed");
    }
    config->complete = result >= 0 && !config->truncated &&
                      declared_length >= USB_SIZEOF_CONFIG_DESC &&
                      length >= declared_length;
    g_store.manifest.config_declared_bytes += declared_length;
    if (config->complete && config->captured_length >= USB_SIZEOF_CONFIG_DESC)
    {
        parse_configuration_locked(
            config_index, config,
            g_store.config_blob + config->raw_offset,
            config->captured_length);
        if (g_store.manifest.active_config_index == 0xFFU)
        {
            g_store.manifest.active_config_index = config_index;
        }
    }
    else if (config->captured_length >= USB_SIZEOF_CONFIG_DESC)
    {
        /* A partial configuration is still useful for diagnostics. */
        parse_configuration_locked(
            config_index, config,
            g_store.config_blob + config->raw_offset,
            config->captured_length);
    }
    if (g_store.manifest.stored_configurations > 0U)
    {
        g_store.manifest.config_complete = RT_TRUE;
        for (config_slot = 0; config_slot <
             (rt_int16_t)g_store.manifest.stored_configurations; config_slot++)
        {
            if (!g_store.configs[config_slot].complete)
            {
                g_store.manifest.config_complete = RT_FALSE;
                break;
            }
        }
    }
    store_unlock();
}

void usb_descriptor_store_record_control(const struct usbh_hubport *hport,
                                         const struct usb_setup_packet *setup,
                                         const rt_uint8_t *data,
                                         rt_int32_t result)
{
    rt_uint8_t string_index;
    rt_uint16_t descriptor_length;
    rt_uint16_t copy_length;
    rt_uint32_t remaining;
    rt_int16_t slot;
    usb_descriptor_string_info_t *info;

    RT_UNUSED(hport);
    if (!setup || setup->bRequest != USB_REQUEST_GET_DESCRIPTOR ||
        (setup->bmRequestType & USB_REQUEST_DIR_MASK) == 0U ||
        (setup->bmRequestType & USB_REQUEST_TYPE_MASK) != USB_REQUEST_STANDARD ||
        (setup->wValue >> 8U) != USB_DESCRIPTOR_TYPE_STRING ||
        result <= 0 || !data)
    {
        return;
    }
    descriptor_length = data[0];
    if (descriptor_length < 2U || descriptor_length > (rt_uint16_t)result)
    {
        descriptor_length = (rt_uint16_t)result;
    }
    if (descriptor_length < 2U)
    {
        return;
    }
    string_index = (rt_uint8_t)(setup->wValue & 0xFFU);
    if (!store_lock())
    {
        return;
    }
    slot = -1;
    for (rt_uint8_t i = 0U; i < g_store.manifest.string_count; i++)
    {
        if (g_store.strings[i].index == string_index)
        {
            slot = i;
            break;
        }
    }
    if (slot < 0)
    {
        if (g_store.manifest.string_count >= USB_DESCRIPTOR_MAX_STRINGS)
        {
            store_unlock();
            return;
        }
        slot = g_store.manifest.string_count++;
    }
    info = &g_store.strings[slot];
    rt_memset(info, 0, sizeof(*info));
    info->index = string_index;
    info->declared_length = data[0];
    info->raw_offset = (rt_uint16_t)g_store.manifest.string_captured_bytes;
    remaining = USB_DESCRIPTOR_STRING_BLOB_MAX -
                g_store.manifest.string_captured_bytes;
    copy_length = descriptor_length > remaining ?
                  (rt_uint16_t)remaining : descriptor_length;
    if (copy_length > 0U)
    {
        rt_memcpy(g_store.string_blob + g_store.manifest.string_captured_bytes,
                  data, copy_length);
        info->raw_length = copy_length;
        g_store.manifest.string_captured_bytes += copy_length;
    }
    string_to_utf8(data, descriptor_length, info->text, sizeof(info->text));
    if (string_index == 0U && descriptor_length >= 4U)
    {
        g_store.manifest.language_id = read_le16(data + 2U);
        g_store.manifest.language_count =
            (rt_uint8_t)((descriptor_length - 2U) / 2U);
        rt_snprintf(info->text, sizeof(info->text), "LANGID 0x%04X",
                    g_store.manifest.language_id);
    }
    update_string_references_locked(string_index, info->text);
    store_unlock();
}

void usb_descriptor_store_record_class_binding(
    const struct usbh_hubport *hport, rt_uint8_t interface_number,
    const char *driver_name, rt_int32_t result)
{
    rt_int16_t config_slot;
    rt_uint16_t i;

    if (!hport || !store_lock())
    {
        return;
    }
    config_slot = find_config_by_value_locked(
        hport->config.config_desc.bConfigurationValue);
    if (config_slot < 0)
    {
        store_unlock();
        return;
    }
    for (i = 0U; i < g_store.manifest.interface_count; i++)
    {
        usb_descriptor_interface_info_t *info = &g_store.interfaces[i];

        if (info->config_index != g_store.configs[config_slot].index ||
            info->interface_number != interface_number ||
            info->alternate_setting != 0U)
        {
            continue;
        }
        if (!driver_name)
        {
            /* No matching class driver is a normal diagnostic-only state,
             * not a failed USB enumeration. */
            info->class_bound = RT_FALSE;
            info->class_bind_failed = RT_FALSE;
        }
        else
        {
            info->class_bound = result >= 0;
            info->class_bind_failed = result < 0;
        }
        copy_text(info->driver_name, sizeof(info->driver_name),
                  driver_name ? driver_name : "");
        if (driver_name && result < 0)
        {
            g_store.manifest.class_bind_failures++;
        }
        update_interface_node_summary_locked(info);
        store_unlock();
        return;
    }
    store_unlock();
}

void usb_descriptor_store_finish(const struct usbh_hubport *hport,
                                 rt_int32_t result)
{
    rt_uint16_t i;
    rt_int16_t active_slot = -1;

    if (!store_lock())
    {
        return;
    }
    if (hport)
    {
        active_slot = find_config_by_value_locked(
            hport->config.config_desc.bConfigurationValue);
        if (active_slot >= 0)
        {
            g_store.manifest.active_config_index =
                g_store.configs[active_slot].index;
        }
        for (i = 0U; i < hport->config.config_desc.bNumInterfaces &&
             i < CONFIG_USBHOST_MAX_INTERFACES; i++)
        {
            const struct usbh_interface *interface = &hport->config.intf[i];
            const struct usb_interface_descriptor *descriptor =
                &interface->altsetting[0].intf_desc;
            rt_uint16_t j;

            for (j = 0U; j < g_store.manifest.interface_count; j++)
            {
                usb_descriptor_interface_info_t *info = &g_store.interfaces[j];

                if (active_slot >= 0 &&
                    info->config_index == g_store.configs[active_slot].index &&
                    info->interface_number == descriptor->bInterfaceNumber &&
                    info->alternate_setting == descriptor->bAlternateSetting)
                {
                    if (interface->class_driver &&
                        interface->class_driver->driver_name &&
                        info->driver_name[0] == '\0')
                    {
                        copy_text(info->driver_name, sizeof(info->driver_name),
                                  interface->class_driver->driver_name);
                        info->class_bound = interface->priv != RT_NULL;
                    }
                    update_interface_node_summary_locked(info);
                    break;
                }
            }
        }
    }
    g_store.manifest.enumeration_complete = result >= 0;
    g_store.manifest.config_complete =
        g_store.manifest.declared_configurations != 0U &&
        g_store.manifest.stored_configurations >=
            g_store.manifest.declared_configurations;
    for (i = 0U; i < g_store.manifest.stored_configurations; i++)
    {
        if (!g_store.configs[i].complete)
        {
            g_store.manifest.config_complete = RT_FALSE;
            break;
        }
    }
    g_store.manifest.config_crc32 = descriptor_crc32(
        g_store.config_blob, g_store.manifest.config_captured_bytes);
    g_store.manifest.unknown_interface_count = 0U;
    for (i = 0U; i < g_store.manifest.interface_count; i++)
    {
        if (!g_store.interfaces[i].class_bound &&
            !g_store.interfaces[i].class_bind_failed)
        {
            g_store.manifest.unknown_interface_count++;
        }
    }
    rt_kprintf("[USB] inventory VID:PID=%04x:%04x speed=%u configs=%u/%u "
               "config=%lu/%lu complete=%d crc=0x%08lx interfaces=%u "
               "endpoints=%u unknown=%u bind_fail=%u%s\r\n",
               g_store.manifest.vendor_id, g_store.manifest.product_id,
               g_store.manifest.speed, g_store.manifest.stored_configurations,
               g_store.manifest.declared_configurations,
               (unsigned long)g_store.manifest.config_captured_bytes,
               (unsigned long)g_store.manifest.config_declared_bytes,
               g_store.manifest.config_complete,
               (unsigned long)g_store.manifest.config_crc32,
               g_store.manifest.interface_count, g_store.manifest.endpoint_count,
               g_store.manifest.unknown_interface_count,
               g_store.manifest.class_bind_failures,
               g_store.manifest.config_truncated ? " truncated" : "");
    store_unlock();
}

void usb_descriptor_store_disconnect(void)
{
    rt_uint32_t generation;

    if (!store_lock())
    {
        return;
    }
    generation = g_store.manifest.generation;
    rt_memset(&g_store, 0, sizeof(g_store));
    g_store.manifest.generation = generation;
    g_store.manifest.active_config_index = 0xFFU;
    store_unlock();
}

rt_bool_t usb_descriptor_store_get_manifest(usb_descriptor_manifest_t *manifest)
{
    if (!manifest || !store_lock())
    {
        return RT_FALSE;
    }
    *manifest = g_store.manifest;
    store_unlock();
    return RT_TRUE;
}

rt_size_t usb_descriptor_store_copy_config(rt_uint8_t config_index,
                                           rt_uint32_t offset,
                                           rt_uint8_t *buffer,
                                           rt_size_t buffer_size)
{
    rt_int16_t slot;
    usb_descriptor_config_info_t *config;
    rt_size_t count;

    if (!buffer || buffer_size == 0U || !store_lock())
    {
        return 0U;
    }
    slot = find_config_by_index_locked(config_index);
    if (slot < 0)
    {
        store_unlock();
        return 0U;
    }
    config = &g_store.configs[slot];
    if (offset >= config->captured_length)
    {
        store_unlock();
        return 0U;
    }
    count = config->captured_length - offset;
    if (count > buffer_size)
    {
        count = buffer_size;
    }
    rt_memcpy(buffer, g_store.config_blob + config->raw_offset + offset, count);
    store_unlock();
    return count;
}

rt_size_t usb_descriptor_store_copy_device_raw(rt_uint32_t offset,
                                               rt_uint8_t *buffer,
                                               rt_size_t buffer_size)
{
    rt_size_t count;

    if (!buffer || buffer_size == 0U || !store_lock())
    {
        return 0U;
    }
    if (offset >= USB_SIZEOF_DEVICE_DESC)
    {
        store_unlock();
        return 0U;
    }
    count = USB_SIZEOF_DEVICE_DESC - offset;
    if (count > buffer_size)
    {
        count = buffer_size;
    }
    rt_memcpy(buffer, g_store.device_raw + offset, count);
    store_unlock();
    return count;
}

rt_size_t usb_descriptor_store_copy_string_raw(rt_uint8_t string_index,
                                               rt_uint32_t offset,
                                               rt_uint8_t *buffer,
                                               rt_size_t buffer_size)
{
    rt_uint8_t i;
    rt_size_t count;

    if (!buffer || buffer_size == 0U || !store_lock())
    {
        return 0U;
    }
    for (i = 0U; i < g_store.manifest.string_count; i++)
    {
        const usb_descriptor_string_info_t *info = &g_store.strings[i];

        if (info->index != string_index)
        {
            continue;
        }
        if (offset >= info->raw_length)
        {
            store_unlock();
            return 0U;
        }
        count = info->raw_length - offset;
        if (count > buffer_size)
        {
            count = buffer_size;
        }
        rt_memcpy(buffer, g_store.string_blob + info->raw_offset + offset,
                  count);
        store_unlock();
        return count;
    }
    store_unlock();
    return 0U;
}

rt_bool_t usb_descriptor_store_get_config_info(
    rt_uint8_t config_index, usb_descriptor_config_info_t *info)
{
    rt_int16_t slot;

    if (!info || !store_lock())
    {
        return RT_FALSE;
    }
    slot = find_config_by_index_locked(config_index);
    if (slot < 0)
    {
        store_unlock();
        return RT_FALSE;
    }
    *info = g_store.configs[slot];
    store_unlock();
    return RT_TRUE;
}

rt_size_t usb_descriptor_store_copy_configs(
    usb_descriptor_config_info_t *configs, rt_size_t capacity)
{
    rt_size_t count;

    if (!configs || capacity == 0U || !store_lock())
    {
        return 0U;
    }
    count = g_store.manifest.stored_configurations;
    if (count > capacity)
    {
        count = capacity;
    }
    rt_memcpy(configs, g_store.configs, count * sizeof(configs[0]));
    store_unlock();
    return count;
}

rt_size_t usb_descriptor_store_copy_interfaces(
    usb_descriptor_interface_info_t *interfaces, rt_size_t capacity)
{
    rt_size_t count;

    if (!interfaces || capacity == 0U || !store_lock())
    {
        return 0U;
    }
    count = g_store.manifest.interface_count;
    if (count > capacity)
    {
        count = capacity;
    }
    rt_memcpy(interfaces, g_store.interfaces,
              count * sizeof(interfaces[0]));
    store_unlock();
    return count;
}

rt_bool_t usb_descriptor_store_get_interface(
    rt_uint16_t index, usb_descriptor_interface_info_t *interface_info)
{
    if (!interface_info || !g_store_ready ||
        rt_mutex_take(&g_store_mutex,
                      rt_tick_from_millisecond(
                          USB_DESCRIPTOR_STORE_LOCK_TIMEOUT_MS)) != RT_EOK)
    {
        return RT_FALSE;
    }
    if (index >= g_store.manifest.interface_count)
    {
        rt_mutex_release(&g_store_mutex);
        return RT_FALSE;
    }
    *interface_info = g_store.interfaces[index];
    rt_mutex_release(&g_store_mutex);
    return RT_TRUE;
}

rt_size_t usb_descriptor_store_copy_endpoints(
    usb_descriptor_endpoint_info_t *endpoints, rt_size_t capacity)
{
    rt_size_t count;

    if (!endpoints || capacity == 0U || !store_lock())
    {
        return 0U;
    }
    count = g_store.manifest.endpoint_count;
    if (count > capacity)
    {
        count = capacity;
    }
    rt_memcpy(endpoints, g_store.endpoints,
              count * sizeof(endpoints[0]));
    store_unlock();
    return count;
}

rt_bool_t usb_descriptor_store_get_endpoint(
    rt_uint16_t index, usb_descriptor_endpoint_info_t *endpoint_info)
{
    if (!endpoint_info || !g_store_ready ||
        rt_mutex_take(&g_store_mutex,
                      rt_tick_from_millisecond(
                          USB_DESCRIPTOR_STORE_LOCK_TIMEOUT_MS)) != RT_EOK)
    {
        return RT_FALSE;
    }
    if (index >= g_store.manifest.endpoint_count)
    {
        rt_mutex_release(&g_store_mutex);
        return RT_FALSE;
    }
    *endpoint_info = g_store.endpoints[index];
    rt_mutex_release(&g_store_mutex);
    return RT_TRUE;
}

rt_size_t usb_descriptor_store_copy_nodes(usb_descriptor_node_t *nodes,
                                          rt_size_t capacity)
{
    rt_size_t count;

    if (!nodes || capacity == 0U || !store_lock())
    {
        return 0U;
    }
    count = g_store.manifest.node_count;
    if (count > capacity)
    {
        count = capacity;
    }
    rt_memcpy(nodes, g_store.nodes, count * sizeof(nodes[0]));
    store_unlock();
    return count;
}

rt_size_t usb_descriptor_store_copy_fields(usb_descriptor_field_t *fields,
                                           rt_size_t capacity)
{
    rt_size_t count;

    if (!fields || capacity == 0U || !store_lock())
    {
        return 0U;
    }
    count = g_store.manifest.field_count;
    if (count > capacity)
    {
        count = capacity;
    }
    rt_memcpy(fields, g_store.fields, count * sizeof(fields[0]));
    store_unlock();
    return count;
}

rt_size_t usb_descriptor_store_copy_strings(
    usb_descriptor_string_info_t *strings, rt_size_t capacity)
{
    rt_size_t count;

    if (!strings || capacity == 0U || !store_lock())
    {
        return 0U;
    }
    count = g_store.manifest.string_count;
    if (count > capacity)
    {
        count = capacity;
    }
    rt_memcpy(strings, g_store.strings, count * sizeof(strings[0]));
    store_unlock();
    return count;
}

rt_bool_t usb_descriptor_store_copy_node_raw(rt_uint16_t node_id,
                                             rt_uint8_t *buffer,
                                             rt_size_t buffer_size,
                                             rt_size_t *copied)
{
    usb_descriptor_node_t *node;
    rt_int16_t slot;
    rt_size_t count;

    if (copied)
    {
        *copied = 0U;
    }
    if (!buffer || buffer_size == 0U || !g_store_ready ||
        rt_mutex_take(&g_store_mutex,
                      rt_tick_from_millisecond(
                          USB_DESCRIPTOR_STORE_LOCK_TIMEOUT_MS)) != RT_EOK)
    {
        return RT_FALSE;
    }
    if (node_id >= g_store.manifest.node_count)
    {
        rt_mutex_release(&g_store_mutex);
        return RT_FALSE;
    }
    node = &g_store.nodes[node_id];
    if (node->config_index == 0xFFU)
    {
        if (node->offset >= USB_SIZEOF_DEVICE_DESC)
        {
            store_unlock();
            return RT_FALSE;
        }
        count = USB_SIZEOF_DEVICE_DESC - node->offset;
        if (count > node->length)
        {
            count = node->length;
        }
        if (count > buffer_size)
        {
            count = buffer_size;
        }
        if (count > 0U)
        {
            rt_memcpy(buffer, g_store.device_raw + node->offset, count);
        }
        if (copied)
        {
            *copied = count;
        }
        store_unlock();
        return count != 0U;
    }
    slot = find_config_by_index_locked(node->config_index);
    if (slot < 0 || node->offset >= g_store.configs[slot].captured_length)
    {
        store_unlock();
        return RT_FALSE;
    }
    count = g_store.configs[slot].captured_length - node->offset;
    if (count > node->length)
    {
        count = node->length;
    }
    if (count > buffer_size)
    {
        count = buffer_size;
    }
    rt_memcpy(buffer, g_store.config_blob + g_store.configs[slot].raw_offset +
              node->offset, count);
    if (copied)
    {
        *copied = count;
    }
    store_unlock();
    return RT_TRUE;
}

rt_bool_t usb_descriptor_store_get_string_info(
    rt_uint8_t string_index, usb_descriptor_string_info_t *info)
{
    rt_uint8_t i;

    if (!info || !store_lock())
    {
        return RT_FALSE;
    }
    for (i = 0U; i < g_store.manifest.string_count; i++)
    {
        if (g_store.strings[i].index == string_index)
        {
            *info = g_store.strings[i];
            store_unlock();
            return RT_TRUE;
        }
    }
    store_unlock();
    return RT_FALSE;
}
