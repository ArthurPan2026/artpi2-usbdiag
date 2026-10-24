/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-07-05     Antigravity  first version for USB descriptor parser
 */

#include <rtthread.h>
#include <string.h>
#include "usb_desc_parser.h"

/* USB-IF base class names.  Unknown values remain visible in the caller's
 * hexadecimal class-code field rather than being treated as an error. */
const char *usb_desc_class_name(rt_uint8_t class_code)
{
    switch (class_code)
    {
        case 0x00: return "Interface level definition";
        case 0x01: return "Audio Device";
        case 0x02: return "CDC Communication Control";
        case 0x03: return "HID (Human Interface Device)";
        case 0x04: return "Monitor";
        case 0x05: return "Physical Device";
        case 0x06: return "Image Device";
        case 0x07: return "Printer";
        case 0x08: return "Mass Storage (U-Disk)";
        case 0x09: return "USB HUB";
        case 0x0A: return "CDC Data Channel";
        case 0x0B: return "Smart Card Reader";
        case 0x0D: return "Content Security";
        case 0x0E: return "Video (Camera)";
        case 0x0F: return "Healthcare";
        case 0xDC: return "Diagnostic Device";
        case 0xE0: return "Wireless Controller";
        case 0xEF: return "Miscellaneous/Composite";
        case 0xFE: return "Application Specific";
        case 0xFF: return "Vendor Specific";
        default:   return "Unknown Class";
    }
}

static const char *_get_class_name(rt_uint8_t class_code)
{
    return usb_desc_class_name(class_code);
}

static rt_uint8_t _bcd_byte_to_decimal(rt_uint8_t value)
{
    return (rt_uint8_t)(((value >> 4) * 10U) + (value & 0x0FU));
}

rt_bool_t usb_desc_parse_single(const rt_uint8_t *raw_in,
                                 rt_uint16_t len,
                                 usb_desc_node_t *node)
{
    if (raw_in == RT_NULL || node == RT_NULL || len < 2)
    {
        return RT_FALSE;
    }

    volatile const rt_uint8_t *raw = raw_in;
    memset(node, 0, sizeof(usb_desc_node_t));
    node->bLength = raw[0];
    node->bDescriptorType = raw[1];

    if (node->bLength < 2U || node->bLength > len)
    {
        return RT_FALSE;
    }

    switch (node->bDescriptorType)
    {
        case 0x01: /* Device Descriptor */
        {
            if (node->bLength < 18)
            {
                return RT_FALSE;
            }

            node->type_name = "设备描述符";
            node->field_count = 10;

            node->field_names[0] = "bcdUSB";
            rt_snprintf(node->field_values[0], 128, "USB %d.%02d (0x%04X)",
                        _bcd_byte_to_decimal(raw[3]),
                        _bcd_byte_to_decimal(raw[2]),
                        (raw[3] << 8) | raw[2]);
            node->field_offsets[0] = 2; node->field_lengths[0] = 2;
            node->field_meanings[0] = "USB 规范版本号，主机据此判断设备能力";

            node->field_names[1] = "bDeviceClass";
            rt_snprintf(node->field_values[1], 128, "0x%02X (%s)",
                        raw[4], _get_class_name(raw[4]));
            node->field_offsets[1] = 4; node->field_lengths[1] = 1;
            node->field_meanings[1] = "设备类别，0x00 表示接口级别定义";

            node->field_names[2] = "bDeviceSubClass";
            rt_snprintf(node->field_values[2], 128, "0x%02X", raw[5]);
            node->field_offsets[2] = 5; node->field_lengths[2] = 1;
            node->field_meanings[2] = "设备子类别，由 USB-IF 分配";

            node->field_names[3] = "bDeviceProtocol";
            rt_snprintf(node->field_values[3], 128, "0x%02X", raw[6]);
            node->field_offsets[3] = 6; node->field_lengths[3] = 1;
            node->field_meanings[3] = "设备协议，与设备类别配合使用";

            node->field_names[4] = "bMaxPacketSize0";
            rt_snprintf(node->field_values[4], 128, "%d 字节", raw[7]);
            node->field_offsets[4] = 7; node->field_lengths[4] = 1;
            node->field_meanings[4] = "端点 0 的最大数据包大小（8/16/32/64）";

            node->field_names[5] = "idVendor";
            rt_snprintf(node->field_values[5], 128, "0x%04X", (raw[9] << 8) | raw[8]);
            node->field_offsets[5] = 8; node->field_lengths[5] = 2;
            node->field_meanings[5] = "厂商编号，由 USB-IF 分配";

            node->field_names[6] = "idProduct";
            rt_snprintf(node->field_values[6], 128, "0x%04X", (raw[11] << 8) | raw[10]);
            node->field_offsets[6] = 10; node->field_lengths[6] = 2;
            node->field_meanings[6] = "产品编号，由厂商自定义";

            node->field_names[7] = "bcdDevice";
            rt_snprintf(node->field_values[7], 128, "v%d.%02d",
                        _bcd_byte_to_decimal(raw[13]),
                        _bcd_byte_to_decimal(raw[12]));
            node->field_offsets[7] = 12; node->field_lengths[7] = 2;
            node->field_meanings[7] = "设备版本号（BCD 编码）";

            node->field_names[8] = "iProduct/iMfg/iSerial";
            rt_snprintf(node->field_values[8], 128, "序列 %d/%d/%d", raw[15], raw[14], raw[16]);
            node->field_offsets[8] = 14; node->field_lengths[8] = 3;
            node->field_meanings[8] = "字符串索引：制造商/产品/序列号";

            node->field_names[9] = "bNumConfigurations";
            rt_snprintf(node->field_values[9], 128, "%d", raw[17]);
            node->field_offsets[9] = 17; node->field_lengths[9] = 1;
            node->field_meanings[9] = "设备支持的配置数量";

            rt_snprintf(node->summary, sizeof(node->summary),
                        "设备 VID:PID %04X:%04X  USB %d.%02d",
                        (raw[9]<<8)|raw[8], (raw[11]<<8)|raw[10],
                        _bcd_byte_to_decimal(raw[3]),
                        _bcd_byte_to_decimal(raw[2]));
            break;
        }

        case 0x02: /* Configuration Descriptor */
        {
            if (node->bLength < 9)
            {
                return RT_FALSE;
            }

            node->type_name = "配置描述符";
            node->field_count = 6;

            node->field_names[0] = "wTotalLength";
            rt_snprintf(node->field_values[0], 128, "%d 字节", (raw[3] << 8) | raw[2]);
            node->field_offsets[0] = 2; node->field_lengths[0] = 2;
            node->field_meanings[0] = "此配置的描述符总长度（含接口和端点）";

            node->field_names[1] = "bNumInterfaces";
            rt_snprintf(node->field_values[1], 128, "%d 个接口", raw[4]);
            node->field_offsets[1] = 4; node->field_lengths[1] = 1;
            node->field_meanings[1] = "此配置包含的接口数量";

            node->field_names[2] = "bConfigurationValue";
            rt_snprintf(node->field_values[2], 128, "配置 %d", raw[5]);
            node->field_offsets[2] = 5; node->field_lengths[2] = 1;
            node->field_meanings[2] = "SetConfiguration 时使用的参数值";

            node->field_names[3] = "iConfiguration";
            rt_snprintf(node->field_values[3], 128, "字符串 #%d", raw[6]);
            node->field_offsets[3] = 6; node->field_lengths[3] = 1;
            node->field_meanings[3] = "配置名称的字符串描述符索引";

            node->field_names[4] = "bmAttributes";
            rt_snprintf(node->field_values[4], 128, "0x%02X (%s%s)",
                        raw[7],
                        (raw[7] & 0x40) ? "自供电" : "总线供电",
                        (raw[7] & 0x20) ? ", 远程唤醒" : "");
            node->field_offsets[4] = 7; node->field_lengths[4] = 1;
            node->field_meanings[4] = "供电方式：bit6=自供电, bit5=支持远程唤醒";

            node->field_names[5] = "bMaxPower";
            rt_snprintf(node->field_values[5], 128, "%d mA", raw[8] * 2);
            node->field_offsets[5] = 8; node->field_lengths[5] = 1;
            node->field_meanings[5] = "最大功耗 = 值 × 2 mA";

            rt_snprintf(node->summary, sizeof(node->summary),
                        "配置 #%d  %d接口  %dmA%s",
                        raw[5], raw[4], raw[8]*2,
                        (raw[7]&0x40)?" 自供电":"");
            break;
        }

        case 0x04: /* Interface Descriptor */
        {
            if (node->bLength < 9)
            {
                return RT_FALSE;
            }

            node->type_name = "接口描述符";
            node->field_count = 6;

            node->field_names[0] = "bInterfaceNumber";
            rt_snprintf(node->field_values[0], 128, "接口 %d", raw[2]);
            node->field_offsets[0] = 2; node->field_lengths[0] = 1;
            node->field_meanings[0] = "接口序号，从 0 开始";

            node->field_names[1] = "bAlternateSetting";
            rt_snprintf(node->field_values[1], 128, "备选 %d", raw[3]);
            node->field_offsets[1] = 3; node->field_lengths[1] = 1;
            node->field_meanings[1] = "备选设置编号（如不同分辨率/带宽）";

            node->field_names[2] = "bNumEndpoints";
            rt_snprintf(node->field_values[2], 128, "%d 个端点", raw[4]);
            node->field_offsets[2] = 4; node->field_lengths[2] = 1;
            node->field_meanings[2] = "接口使用的端点数量（不含端点 0）";

            node->field_names[3] = "bInterfaceClass";
            rt_snprintf(node->field_values[3], 128, "0x%02X (%s)",
                        raw[5], _get_class_name(raw[5]));
            node->field_offsets[3] = 5; node->field_lengths[3] = 1;
            node->field_meanings[3] = "接口类别：如 HID(0x03)、MSC(0x08)、CDC(0x02)";

            node->field_names[4] = "bInterfaceSubClass";
            rt_snprintf(node->field_values[4], 128, "0x%02X", raw[6]);
            node->field_offsets[4] = 6; node->field_lengths[4] = 1;
            node->field_meanings[4] = "接口子类别（如 HID Boot Interface）";

            node->field_names[5] = "bInterfaceProtocol";
            rt_snprintf(node->field_values[5], 128, "0x%02X", raw[7]);
            node->field_offsets[5] = 7; node->field_lengths[5] = 1;
            node->field_meanings[5] = "接口协议（如键盘=0x01/鼠标=0x02）";

            rt_snprintf(node->summary, sizeof(node->summary),
                        "接口 #%d  %s  %d端点",
                        raw[2], _get_class_name(raw[5]), raw[4]);
            break;
        }

        case 0x05: /* Endpoint Descriptor */
        {
            if (node->bLength < 7)
            {
                return RT_FALSE;
            }

            node->type_name = "端点描述符";
            node->field_count = 4;

            node->field_names[0] = "bEndpointAddress";
            rt_snprintf(node->field_values[0], 128, "0x%02X (%s, EP%d)",
                        raw[2], (raw[2] & 0x80) ? "输入" : "输出", raw[2] & 0x0F);
            node->field_offsets[0] = 2; node->field_lengths[0] = 1;
            node->field_meanings[0] = "bit7=方向(1=输入), bit0-3=端点号";

            node->field_names[1] = "bmAttributes";
            {
                const char *ep_type;
                switch (raw[3] & 0x03)
                {
                    case 0x00: ep_type = "控制传输"; break;
                    case 0x01: ep_type = "同步传输"; break;
                    case 0x02: ep_type = "批量传输"; break;
                    case 0x03: ep_type = "中断传输"; break;
                    default:   ep_type = "?"; break;
                }
                rt_snprintf(node->field_values[1], 128, "0x%02X (%s)", raw[3], ep_type);
            }
            node->field_offsets[1] = 3; node->field_lengths[1] = 1;
            node->field_meanings[1] = "传输类型：控制/同步/批量/中断";

            node->field_names[2] = "wMaxPacketSize";
            rt_snprintf(node->field_values[2], 128, "%d 字节", (raw[5] << 8) | raw[4]);
            node->field_offsets[2] = 4; node->field_lengths[2] = 2;
            node->field_meanings[2] = "单次传输的最大数据包大小";

            node->field_names[3] = "bInterval";
            rt_snprintf(node->field_values[3], 128, "%d (帧/微帧)", raw[6]);
            node->field_offsets[3] = 6; node->field_lengths[3] = 1;
            node->field_meanings[3] = "数据传输间隔，单位取决于传输类型和速度";

            {
                const char *ep_type;
                switch (raw[3] & 0x03) {
                    case 0x00: ep_type = "控制"; break;
                    case 0x01: ep_type = "同步"; break;
                    case 0x02: ep_type = "批量"; break;
                    case 0x03: ep_type = "中断"; break;
                    default:   ep_type = "?"; break;
                }
                rt_snprintf(node->summary, sizeof(node->summary),
                            "端点 EP%d %s  0x%02X %s  %d字节",
                            raw[2]&0x0F, (raw[2]&0x80)?"输入":"输出",
                            raw[2], ep_type,
                            (raw[5]<<8)|raw[4]);
            }
            break;
        }

        case 0x21: /* HID Descriptor */
        {
            if (node->bLength < 9)
            {
                return RT_FALSE;
            }

            node->type_name = "HID 描述符";
            node->field_count = 4;

            node->field_names[0] = "bcdHID";
            rt_snprintf(node->field_values[0], 128, "v%d.%02d",
                        _bcd_byte_to_decimal(raw[3]),
                        _bcd_byte_to_decimal(raw[2]));
            node->field_offsets[0] = 2; node->field_lengths[0] = 2;
            node->field_meanings[0] = "HID 规范版本号";

            node->field_names[1] = "bCountryCode";
            rt_snprintf(node->field_values[1], 128, "0x%02X", raw[4]);
            node->field_offsets[1] = 4; node->field_lengths[1] = 1;
            node->field_meanings[1] = "键盘区域码，0x00=未本地化";

            node->field_names[2] = "bNumDescriptors";
            rt_snprintf(node->field_values[2], 128, "%d", raw[5]);
            node->field_offsets[2] = 5; node->field_lengths[2] = 1;
            node->field_meanings[2] = "HID 报告描述符的数量（通常为 1）";

            node->field_names[3] = "wDescriptorLength0";
            rt_snprintf(node->field_values[3], 128, "%d 字节", (raw[8] << 8) | raw[7]);
            node->field_offsets[3] = 7; node->field_lengths[3] = 2;
            node->field_meanings[3] = "HID 报告描述符的大小";

            rt_snprintf(node->summary, sizeof(node->summary),
                        "HID v%d.%02d  报告长度 %d字节",
                        _bcd_byte_to_decimal(raw[3]),
                        _bcd_byte_to_decimal(raw[2]),
                        (raw[8]<<8)|raw[7]);
            break;
        }

        case 0x03: /* String Descriptor */
        {
            /* Payload is UTF-16LE after the 2-byte header; preview it as
             * UTF-8 so the UI can show the actual device text. */
            rt_uint16_t char_count = (node->bLength - 2U) / 2U;
            rt_uint16_t s;
            rt_uint8_t out[128];
            rt_uint8_t o = 0U;

            /* "字符串描述符" is 19 UTF-8 bytes, one over the 16-byte
             * type_name field in the snapshot; use the short form. */
            node->type_name = "字符串";
            node->field_count = 2;

            node->field_names[0] = "内容";
            for (s = 2U; s + 1U < node->bLength && o < sizeof(out) - 1U;
                 s += 2U)
            {
                rt_uint16_t cp = (rt_uint16_t)(raw[s] |
                                               ((rt_uint16_t)raw[s + 1U] << 8));
                if (cp < 0x80U)
                {
                    out[o++] = (rt_uint8_t)cp;
                }
                else if (cp < 0x800U && o + 1U < sizeof(out) - 1U)
                {
                    out[o++] = (rt_uint8_t)(0xC0U | (cp >> 6));
                    out[o++] = (rt_uint8_t)(0x80U | (cp & 0x3FU));
                }
                else if (o + 2U < sizeof(out) - 1U)
                {
                    out[o++] = (rt_uint8_t)(0xE0U | (cp >> 12));
                    out[o++] = (rt_uint8_t)(0x80U | ((cp >> 6) & 0x3FU));
                    out[o++] = (rt_uint8_t)(0x80U | (cp & 0x3FU));
                }
            }
            out[o] = '\0';
            rt_snprintf(node->field_values[0], 128, "%s", out);
            node->field_offsets[0] = 2;
            node->field_lengths[0] = node->bLength - 2U;
            node->field_meanings[0] = "UTF-16LE 文本（UTF-8 预览）";

            node->field_names[1] = "字符数";
            rt_snprintf(node->field_values[1], 128, "%u", char_count);
            node->field_offsets[1] = 2;
            node->field_lengths[1] = node->bLength - 2U;
            node->field_meanings[1] = "字符串长度（UTF-16 码元数）";

            rt_snprintf(node->summary, sizeof(node->summary),
                        "字符串  %u 字符  %s", char_count, out);
            break;
        }

        default:
        {
            node->type_name = "其他/类描述符";
            node->field_count = 1;
            node->field_names[0] = "RawHex";
            rt_snprintf(node->field_values[0], 128, "类型 0x%02X, 长度 %d",
                        node->bDescriptorType, node->bLength);
            node->field_offsets[0] = 0;
            node->field_lengths[0] = node->bLength;
            node->field_meanings[0] = "原始描述符数据";
            rt_snprintf(node->summary, sizeof(node->summary),
                        "其他 0x%02X  %d字节",
                        node->bDescriptorType, node->bLength);
            break;
        }
    }

    return RT_TRUE;
}

static rt_bool_t usb_desc_parse_language_ids(const rt_uint8_t *raw,
                                             rt_uint16_t len,
                                             usb_desc_node_t *node)
{
    rt_uint16_t descriptor_length;
    rt_uint16_t language_count;
    rt_uint16_t index;
    rt_size_t used = 0U;

    if (!raw || !node || len < 2U || raw[1] != 0x03U)
    {
        return RT_FALSE;
    }
    descriptor_length = raw[0];
    if (descriptor_length < 4U || descriptor_length > len ||
        ((descriptor_length - 2U) & 1U) != 0U)
    {
        return RT_FALSE;
    }

    language_count = (rt_uint16_t)((descriptor_length - 2U) / 2U);
    rt_memset(node, 0, sizeof(*node));
    node->bLength = (rt_uint8_t)descriptor_length;
    node->bDescriptorType = raw[1];
    node->type_name = "Language IDs";
    node->field_count = 2U;

    node->field_names[0] = "LANGID";
    node->field_offsets[0] = 2U;
    node->field_lengths[0] = (rt_uint8_t)(descriptor_length - 2U);
    node->field_meanings[0] = "USB Language ID table";
    node->field_values[0][0] = '\0';
    for (index = 0U; index < language_count; index++)
    {
        rt_uint16_t language_id = (rt_uint16_t)(raw[2U + index * 2U] |
                                                ((rt_uint16_t)raw[3U + index * 2U]
                                                 << 8U));
        int written;

        written = rt_snprintf(node->field_values[0] + used,
                              sizeof(node->field_values[0]) - used,
                              used == 0U ? "0x%04X" : ", 0x%04X",
                              language_id);
        if (written < 0 || (rt_size_t)written >=
            sizeof(node->field_values[0]) - used)
        {
            used = sizeof(node->field_values[0]) - 1U;
            node->field_values[0][used] = '\0';
            break;
        }
        used += (rt_size_t)written;
    }

    node->field_names[1] = "Count";
    node->field_offsets[1] = 2U;
    node->field_lengths[1] = (rt_uint8_t)(descriptor_length - 2U);
    node->field_meanings[1] = "Number of supported USB languages";
    rt_snprintf(node->field_values[1], sizeof(node->field_values[1]), "%u",
                language_count);

    rt_snprintf(node->summary, sizeof(node->summary),
                "LANGID %s (%u)", node->field_values[0], language_count);
    return RT_TRUE;
}

rt_bool_t usb_desc_parse_string(const rt_uint8_t *raw,
                                rt_uint16_t len,
                                rt_uint8_t string_index,
                                usb_desc_node_t *node)
{
    if (string_index == 0U)
    {
        return usb_desc_parse_language_ids(raw, len, node);
    }
    return usb_desc_parse_single(raw, len, node);
}

void usb_desc_print_node(const usb_desc_node_t *node)
{
    rt_int32_t i;

    if (node == RT_NULL)
    {
        return;
    }

    rt_kprintf("  |-- %s (Length: %d)\n", node->type_name, node->bLength);
    for (i = 0; i < node->field_count; i++)
    {
        rt_kprintf("  |   |-- %s: %s\n", node->field_names[i], node->field_values[i]);
    }
}
