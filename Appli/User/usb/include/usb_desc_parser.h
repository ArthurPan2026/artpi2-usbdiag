/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-07-05     Antigravity  first version for USB descriptor parser
 */

#ifndef USB_DESC_PARSER_H
#define USB_DESC_PARSER_H

#include <rtthread.h>

#define MAX_DESC_FIELDS 16

/* USB interface/device class names are shared by the parser, UI and the
 * generic descriptor inventory. */
const char *usb_desc_class_name(rt_uint8_t class_code);

/*
 * Decoded descriptor node
 */
typedef struct usb_desc_node
{
    rt_uint8_t  bLength;
    rt_uint8_t  bDescriptorType;
    const char  *type_name;
    const char  *field_names[MAX_DESC_FIELDS];
    char        field_values[MAX_DESC_FIELDS][128];
    rt_uint8_t  field_count;
    /* --- Teaching-mode enrichment (additive; console path unaffected) --- */
    rt_uint8_t  field_offsets[MAX_DESC_FIELDS];   /* byte offset of field in descriptor */
    rt_uint8_t  field_lengths[MAX_DESC_FIELDS];   /* field width in bytes (1/2/...) */
    const char  *field_meanings[MAX_DESC_FIELDS]; /* plain-language meaning (const literal) */
    char        summary[48];                      /* one-line scannable tree-row summary */
} usb_desc_node_t;

/**
 * @brief    Parse a single raw descriptor packet.
 *
 * @param    raw      Pointer to the raw byte array
 * @param    len      Length of the raw packet
 * @param    node     Pointer to the target node structure to fill
 *
 * @return   rt_bool_t RT_TRUE on success, RT_FALSE on parse failure
 */
rt_bool_t usb_desc_parse_single(const rt_uint8_t *raw,
                                 rt_uint16_t len,
                                 usb_desc_node_t *node);

/**
 * @brief Parse a descriptor returned for a specific USB string index.
 *
 * String descriptor index 0 is a Language ID table, not UTF-16 text.  The
 * index-aware entry point keeps that protocol distinction available to the
 * evidence UI while preserving usb_desc_parse_single() for callers that only
 * have a raw descriptor without the GET_DESCRIPTOR context.
 */
rt_bool_t usb_desc_parse_string(const rt_uint8_t *raw,
                                rt_uint16_t len,
                                rt_uint8_t string_index,
                                usb_desc_node_t *node);

/**
 * @brief    Print the parsed descriptor node fields onto the console.
 */
void usb_desc_print_node(const usb_desc_node_t *node);

#endif /* USB_DESC_PARSER_H */
