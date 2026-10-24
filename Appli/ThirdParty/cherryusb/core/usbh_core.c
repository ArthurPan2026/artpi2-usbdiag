/*
 * Copyright (c) 2022, sakumisu
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "usbh_core.h"

#undef USB_DBG_TAG
#define USB_DBG_TAG "usbh_core"
#include "usb_log.h"

struct usbh_class_info *usbh_class_info_table_begin = NULL;
struct usbh_class_info *usbh_class_info_table_end = NULL;

usb_slist_t g_bus_head = USB_SLIST_OBJECT_INIT(g_bus_head);

/* Project-local observation hooks.  Strong definitions in
 * usb_evidence_wrapper.c record bounded data and leave parsing to the
 * evidence thread. */
__WEAK bool usbh_control_transfer_started_observer(
    struct usbh_hubport *hport, uint32_t *started_at_us)
{
    (void)hport;
    (void)started_at_us;
    return false;
}

__WEAK bool usbh_control_transfer_completed_observer(
    struct usbh_hubport *hport, uint32_t *completed_at_us)
{
    (void)hport;
    (void)completed_at_us;
    return false;
}

__WEAK void usbh_control_transfer_observer(struct usbh_hubport *hport,
                                           const struct usb_setup_packet *setup,
                                           const uint8_t *buffer,
                                           int result,
                                           uint32_t started_at_us,
                                           uint32_t completed_at_us,
                                           bool timing_valid)
{
    (void)hport;
    (void)setup;
    (void)buffer;
    (void)result;
    (void)started_at_us;
    (void)completed_at_us;
    (void)timing_valid;
}

__WEAK void usbh_enumeration_started_observer(struct usbh_hubport *hport)
{
    (void)hport;
}

__WEAK void usbh_enumeration_finished_observer(struct usbh_hubport *hport,
                                               int result)
{
    (void)hport;
    (void)result;
}

__WEAK void usbh_hubport_release_observer(struct usbh_hubport *hport)
{
    (void)hport;
}

__WEAK void usbh_device_descriptor_observer(struct usbh_hubport *hport,
                                            const uint8_t *buffer,
                                            uint16_t length)
{
    (void)hport;
    (void)buffer;
    (void)length;
}

__WEAK void usbh_configuration_descriptor_observer(
    struct usbh_hubport *hport, uint8_t config_index,
    const uint8_t *buffer, uint16_t length, uint16_t declared_length,
    int result)
{
    (void)hport;
    (void)config_index;
    (void)buffer;
    (void)length;
    (void)declared_length;
    (void)result;
}

__WEAK void usbh_class_binding_observer(struct usbh_hubport *hport,
                                        uint8_t interface_number,
                                        const struct usbh_class_driver *driver,
                                        int result)
{
    (void)hport;
    (void)interface_number;
    (void)driver;
    (void)result;
}

struct setup_align_buffer {
    uint8_t buffer[USB_ALIGN_UP(8, CONFIG_USB_ALIGN_SIZE)];
};

USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t ep0_request_buffer[CONFIG_USBHOST_MAX_BUS][USB_ALIGN_UP(CONFIG_USBHOST_REQUEST_BUFFER_LEN, CONFIG_USB_ALIGN_SIZE)];
USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t selected_config_buffer[CONFIG_USBHOST_MAX_BUS][USB_ALIGN_UP(CONFIG_USBHOST_REQUEST_BUFFER_LEN, CONFIG_USB_ALIGN_SIZE)];
USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX struct setup_align_buffer g_setup_buffer[CONFIG_USBHOST_MAX_BUS][CONFIG_USBHOST_MAX_EXTHUBS + 1][CONFIG_USBHOST_MAX_EHPORTS];

/* USB topology/configuration state is CPU-only.  Keep it separate from the
 * immutable descriptor blob and from the DMA-only AXI SRAM window. */
struct usbh_bus g_usbhost_bus[CONFIG_USBHOST_MAX_BUS]
    USB_HOST_STATE_RAM_SECTION;

/* general descriptor field offsets */
#define DESC_bLength         0 /** Length offset */
#define DESC_bDescriptorType 1 /** Descriptor type offset */

#define USB_DEV_ADDR_MAX         0x7f
#define USB_DEV_ADDR_MARK_OFFSET 5
#define USB_DEV_ADDR_MARK_MASK   0x1f

static int usbh_allocate_devaddr(struct usbh_devaddr_map *devgen)
{
    uint8_t startaddr = devgen->next;
    uint8_t devaddr;
    int index;
    int bitno;

    for (;;) {
        devaddr = devgen->next;
        if (devgen->next >= 0x7f) {
            devgen->next = 2;
        } else {
            devgen->next++;
        }

        index = devaddr >> 5;
        bitno = devaddr & 0x1f;
        if ((devgen->alloctab[index] & (1 << bitno)) == 0) {
            devgen->alloctab[index] |= (1 << bitno);
            return (int)devaddr;
        }

        if (startaddr == devaddr) {
            return -USB_ERR_NOMEM;
        }
    }
}

static int __usbh_free_devaddr(struct usbh_devaddr_map *devgen, uint8_t devaddr)
{
    int index;
    int bitno;

    if ((devaddr > 0) && (devaddr < USB_DEV_ADDR_MAX)) {
        index = devaddr >> USB_DEV_ADDR_MARK_OFFSET;
        bitno = devaddr & USB_DEV_ADDR_MARK_MASK;

        /* Free the address  */
        if ((devgen->alloctab[index] |= (1 << bitno)) != 0) {
            devgen->alloctab[index] &= ~(1 << bitno);
        } else {
            return -1;
        }

        if (devaddr < devgen->next) {
            devgen->next = devaddr;
        }
    }

    return 0;
}

static int usbh_free_devaddr(struct usbh_hubport *hport)
{
    if (hport->dev_addr > 0) {
        __usbh_free_devaddr(&hport->bus->devgen, hport->dev_addr);
    }
    return 0;
}

static const struct usbh_class_driver *usbh_find_class_driver(uint8_t class, uint8_t subclass, uint8_t protocol,
                                                              uint16_t vid, uint16_t pid)
{
    struct usbh_class_info *index = NULL;

    for (index = usbh_class_info_table_begin; index < usbh_class_info_table_end; index++) {
        if ((index->match_flags & USB_CLASS_MATCH_INTF_CLASS) && !(index->bInterfaceClass == class)) {
            continue;
        }
        if ((index->match_flags & USB_CLASS_MATCH_INTF_SUBCLASS) && !(index->bInterfaceSubClass == subclass)) {
            continue;
        }
        if ((index->match_flags & USB_CLASS_MATCH_INTF_PROTOCOL) && !(index->bInterfaceProtocol == protocol)) {
            continue;
        }
        if (index->match_flags & USB_CLASS_MATCH_VID_PID && index->id_table) {
            /* scan id table */
            uint32_t i;
            for (i = 0; index->id_table[i][0]; i++) {
                if (index->id_table[i][0] == vid && index->id_table[i][1] == pid) {
                    break;
                }
            }
            /* do not match, continue next */
            if (!index->id_table[i][0]) {
                continue;
            }
        }
        return index->class_driver;
    }
    return NULL;
}

static int parse_device_descriptor(struct usbh_hubport *hport, struct usb_device_descriptor *desc, uint16_t length)
{
    if (desc->bLength != USB_SIZEOF_DEVICE_DESC) {
        USB_LOG_ERR("invalid device bLength 0x%02x\r\n", desc->bLength);
        return -USB_ERR_INVAL;
    } else if (desc->bDescriptorType != USB_DESCRIPTOR_TYPE_DEVICE) {
        USB_LOG_ERR("unexpected device descriptor 0x%02x\r\n", desc->bDescriptorType);
        return -USB_ERR_INVAL;
    } else {
        if (length <= 8) {
            return 0;
        }
#if 0
        USB_LOG_DBG("Device Descriptor:\r\n");
        USB_LOG_DBG("bLength: 0x%02x           \r\n", desc->bLength);
        USB_LOG_DBG("bDescriptorType: 0x%02x   \r\n", desc->bDescriptorType);
        USB_LOG_DBG("bcdUSB: 0x%04x            \r\n", desc->bcdUSB);
        USB_LOG_DBG("bDeviceClass: 0x%02x      \r\n", desc->bDeviceClass);
        USB_LOG_DBG("bDeviceSubClass: 0x%02x   \r\n", desc->bDeviceSubClass);
        USB_LOG_DBG("bDeviceProtocol: 0x%02x   \r\n", desc->bDeviceProtocol);
        USB_LOG_DBG("bMaxPacketSize0: 0x%02x   \r\n", desc->bMaxPacketSize0);
        USB_LOG_DBG("idVendor: 0x%04x          \r\n", desc->idVendor);
        USB_LOG_DBG("idProduct: 0x%04x         \r\n", desc->idProduct);
        USB_LOG_DBG("bcdDevice: 0x%04x         \r\n", desc->bcdDevice);
        USB_LOG_DBG("iManufacturer: 0x%02x     \r\n", desc->iManufacturer);
        USB_LOG_DBG("iProduct: 0x%02x          \r\n", desc->iProduct);
        USB_LOG_DBG("iSerialNumber: 0x%02x     \r\n", desc->iSerialNumber);
        USB_LOG_DBG("bNumConfigurations: 0x%02x\r\n", desc->bNumConfigurations);
#endif
        hport->device_desc.bLength = desc->bLength;
        hport->device_desc.bDescriptorType = desc->bDescriptorType;
        hport->device_desc.bcdUSB = desc->bcdUSB;
        hport->device_desc.bDeviceClass = desc->bDeviceClass;
        hport->device_desc.bDeviceSubClass = desc->bDeviceSubClass;
        hport->device_desc.bDeviceProtocol = desc->bDeviceProtocol;
        hport->device_desc.bMaxPacketSize0 = desc->bMaxPacketSize0;
        hport->device_desc.idVendor = desc->idVendor;
        hport->device_desc.idProduct = desc->idProduct;
        hport->device_desc.bcdDevice = desc->bcdDevice;
        hport->device_desc.iManufacturer = desc->iManufacturer;
        hport->device_desc.iProduct = desc->iProduct;
        hport->device_desc.iSerialNumber = desc->iSerialNumber;
        hport->device_desc.bNumConfigurations = desc->bNumConfigurations;
    }
    return 0;
}

static int parse_config_descriptor(struct usbh_hubport *hport, struct usb_configuration_descriptor *desc, uint16_t length)
{
    struct usb_interface_descriptor *intf_desc;
    struct usb_endpoint_descriptor *ep_desc;
    uint8_t cur_alt_setting = 0xff;
    uint8_t cur_iface = 0xff;
    uint8_t cur_ep = 0xff;
    uint8_t cur_ep_num = 0xff;
    uint32_t desc_len = 0;
    uint8_t *p;

    if (desc->bLength != USB_SIZEOF_CONFIG_DESC) {
        USB_LOG_ERR("invalid config bLength 0x%02x\r\n", desc->bLength);
        return -USB_ERR_INVAL;
    } else if (desc->bDescriptorType != USB_DESCRIPTOR_TYPE_CONFIGURATION) {
        USB_LOG_ERR("unexpected config descriptor 0x%02x\r\n", desc->bDescriptorType);
        return -USB_ERR_INVAL;
    } else {
        if (length <= USB_SIZEOF_CONFIG_DESC) {
            return 0;
        }
#if 0
        USB_LOG_DBG("Config Descriptor:\r\n");
        USB_LOG_DBG("bLength: 0x%02x             \r\n", desc->bLength);
        USB_LOG_DBG("bDescriptorType: 0x%02x     \r\n", desc->bDescriptorType);
        USB_LOG_DBG("wTotalLength: 0x%04x        \r\n", desc->wTotalLength);
        USB_LOG_DBG("bNumInterfaces: 0x%02x      \r\n", desc->bNumInterfaces);
        USB_LOG_DBG("bConfigurationValue: 0x%02x \r\n", desc->bConfigurationValue);
        USB_LOG_DBG("iConfiguration: 0x%02x      \r\n", desc->iConfiguration);
        USB_LOG_DBG("bmAttributes: 0x%02x        \r\n", desc->bmAttributes);
        USB_LOG_DBG("bMaxPower: 0x%02x           \r\n", desc->bMaxPower);
#endif
        hport->config.config_desc.bLength = desc->bLength;
        hport->config.config_desc.bDescriptorType = desc->bDescriptorType;
        hport->config.config_desc.wTotalLength = desc->wTotalLength;
        hport->config.config_desc.bNumInterfaces = desc->bNumInterfaces;
        hport->config.config_desc.bConfigurationValue = desc->bConfigurationValue;
        hport->config.config_desc.iConfiguration = desc->iConfiguration;
        hport->config.config_desc.iConfiguration = desc->iConfiguration;
        hport->config.config_desc.bmAttributes = desc->bmAttributes;
        hport->config.config_desc.bMaxPower = desc->bMaxPower;

        p = (uint8_t *)desc;
        p += USB_SIZEOF_CONFIG_DESC;
        desc_len = USB_SIZEOF_CONFIG_DESC;

        memset(hport->config.intf, 0, sizeof(struct usbh_interface) * CONFIG_USBHOST_MAX_INTERFACES);

        while (desc_len < length) {
            uint8_t descriptor_length = p[DESC_bLength];

            if (descriptor_length < 2U ||
                descriptor_length > (length - desc_len)) {
                return -USB_ERR_INVAL;
            }

            switch (p[DESC_bDescriptorType]) {
                case USB_DESCRIPTOR_TYPE_INTERFACE:
                    if (descriptor_length < USB_SIZEOF_INTERFACE_DESC) {
                        return -USB_ERR_INVAL;
                    }
                    intf_desc = (struct usb_interface_descriptor *)p;
                    cur_iface = intf_desc->bInterfaceNumber;
                    cur_alt_setting = intf_desc->bAlternateSetting;
                    cur_ep_num = intf_desc->bNumEndpoints;
                    cur_ep = 0;

                    if (cur_iface >= CONFIG_USBHOST_MAX_INTERFACES) {
                        USB_LOG_WRN("Interface num %d exceeds class-driver storage; keep raw descriptor\r\n", cur_iface);
                        cur_iface = 0xff;
                        cur_alt_setting = 0xff;
                        break;
                    }

                    if (cur_ep_num > CONFIG_USBHOST_MAX_ENDPOINTS) {
                        USB_LOG_WRN("Interface %d declares %d endpoints; class-driver storage keeps first %d\r\n",
                                    cur_iface, cur_ep_num,
                                    CONFIG_USBHOST_MAX_ENDPOINTS);
                    }

                    if (cur_alt_setting >= CONFIG_USBHOST_MAX_INTF_ALTSETTINGS) {
                        USB_LOG_WRN("Interface %d alternate setting %d exceeds class-driver storage; keep raw descriptor\r\n",
                                    cur_iface, cur_alt_setting);
                        cur_alt_setting = 0xff;
                        break;
                    }

#if 0
                    USB_LOG_DBG("Interface Descriptor:\r\n");
                    USB_LOG_DBG("bLength: 0x%02x            \r\n", intf_desc->bLength);
                    USB_LOG_DBG("bDescriptorType: 0x%02x    \r\n", intf_desc->bDescriptorType);
                    USB_LOG_DBG("bInterfaceNumber: 0x%02x   \r\n", intf_desc->bInterfaceNumber);
                    USB_LOG_DBG("bAlternateSetting: 0x%02x  \r\n", intf_desc->bAlternateSetting);
                    USB_LOG_DBG("bNumEndpoints: 0x%02x      \r\n", intf_desc->bNumEndpoints);
                    USB_LOG_DBG("bInterfaceClass: 0x%02x    \r\n", intf_desc->bInterfaceClass);
                    USB_LOG_DBG("bInterfaceSubClass: 0x%02x \r\n", intf_desc->bInterfaceSubClass);
                    USB_LOG_DBG("bInterfaceProtocol: 0x%02x \r\n", intf_desc->bInterfaceProtocol);
                    USB_LOG_DBG("iInterface: 0x%02x         \r\n", intf_desc->iInterface);
#endif
                    memcpy(&hport->config.intf[cur_iface].altsetting[cur_alt_setting].intf_desc, intf_desc, 9);
                    /* Class drivers index the static endpoint array using
                     * bNumEndpoints.  Keep the raw descriptor authoritative
                     * in the inventory, but cap the operational view so a
                     * large vendor/composite interface cannot walk past it. */
                    if (hport->config.intf[cur_iface].altsetting[cur_alt_setting].intf_desc.bNumEndpoints > CONFIG_USBHOST_MAX_ENDPOINTS) {
                        hport->config.intf[cur_iface].altsetting[cur_alt_setting].intf_desc.bNumEndpoints = CONFIG_USBHOST_MAX_ENDPOINTS;
                    }
                    hport->config.intf[cur_iface].altsetting_num = cur_alt_setting + 1;
                    break;
                case USB_DESCRIPTOR_TYPE_ENDPOINT:
                    if (descriptor_length < USB_SIZEOF_ENDPOINT_DESC) {
                        return -USB_ERR_INVAL;
                    }
                    ep_desc = (struct usb_endpoint_descriptor *)p;
                    if (cur_iface < CONFIG_USBHOST_MAX_INTERFACES &&
                        cur_alt_setting < CONFIG_USBHOST_MAX_INTF_ALTSETTINGS &&
                        cur_ep < CONFIG_USBHOST_MAX_ENDPOINTS) {
                        memcpy(&hport->config.intf[cur_iface].altsetting[cur_alt_setting].ep[cur_ep].ep_desc, ep_desc, 7);
                        cur_ep++;
                    }
                    break;

                default:
                    break;
            }
            /* skip to next descriptor */
            desc_len += descriptor_length;
            p += descriptor_length;
        }
    }
    return 0;
}

static void usbh_print_hubport_info(struct usbh_hubport *hport)
{
    USB_LOG_RAW("Device Descriptor:\r\n");
    USB_LOG_RAW("bLength: 0x%02x           \r\n", hport->device_desc.bLength);
    USB_LOG_RAW("bDescriptorType: 0x%02x   \r\n", hport->device_desc.bDescriptorType);
    USB_LOG_RAW("bcdUSB: 0x%04x            \r\n", hport->device_desc.bcdUSB);
    USB_LOG_RAW("bDeviceClass: 0x%02x      \r\n", hport->device_desc.bDeviceClass);
    USB_LOG_RAW("bDeviceSubClass: 0x%02x   \r\n", hport->device_desc.bDeviceSubClass);
    USB_LOG_RAW("bDeviceProtocol: 0x%02x   \r\n", hport->device_desc.bDeviceProtocol);
    USB_LOG_RAW("bMaxPacketSize0: 0x%02x   \r\n", hport->device_desc.bMaxPacketSize0);
    USB_LOG_RAW("idVendor: 0x%04x          \r\n", hport->device_desc.idVendor);
    USB_LOG_RAW("idProduct: 0x%04x         \r\n", hport->device_desc.idProduct);
    USB_LOG_RAW("bcdDevice: 0x%04x         \r\n", hport->device_desc.bcdDevice);
    USB_LOG_RAW("iManufacturer: 0x%02x     \r\n", hport->device_desc.iManufacturer);
    USB_LOG_RAW("iProduct: 0x%02x          \r\n", hport->device_desc.iProduct);
    USB_LOG_RAW("iSerialNumber: 0x%02x     \r\n", hport->device_desc.iSerialNumber);
    USB_LOG_RAW("bNumConfigurations: 0x%02x\r\n", hport->device_desc.bNumConfigurations);

    USB_LOG_RAW("Config Descriptor:\r\n");
    USB_LOG_RAW("bLength: 0x%02x             \r\n", hport->config.config_desc.bLength);
    USB_LOG_RAW("bDescriptorType: 0x%02x     \r\n", hport->config.config_desc.bDescriptorType);
    USB_LOG_RAW("wTotalLength: 0x%04x        \r\n", hport->config.config_desc.wTotalLength);
    USB_LOG_RAW("bNumInterfaces: 0x%02x      \r\n", hport->config.config_desc.bNumInterfaces);
    USB_LOG_RAW("bConfigurationValue: 0x%02x \r\n", hport->config.config_desc.bConfigurationValue);
    USB_LOG_RAW("iConfiguration: 0x%02x      \r\n", hport->config.config_desc.iConfiguration);
    USB_LOG_RAW("bmAttributes: 0x%02x        \r\n", hport->config.config_desc.bmAttributes);
    USB_LOG_RAW("bMaxPower: 0x%02x           \r\n", hport->config.config_desc.bMaxPower);

    for (uint8_t i = 0; i < hport->config.config_desc.bNumInterfaces &&
         i < CONFIG_USBHOST_MAX_INTERFACES; i++) {
        for (uint8_t j = 0; j < hport->config.intf[i].altsetting_num; j++) {
            USB_LOG_RAW("\tInterface Descriptor:\r\n");
            USB_LOG_RAW("\tbLength: 0x%02x            \r\n", hport->config.intf[i].altsetting[j].intf_desc.bLength);
            USB_LOG_RAW("\tbDescriptorType: 0x%02x    \r\n", hport->config.intf[i].altsetting[j].intf_desc.bDescriptorType);
            USB_LOG_RAW("\tbInterfaceNumber: 0x%02x   \r\n", hport->config.intf[i].altsetting[j].intf_desc.bInterfaceNumber);
            USB_LOG_RAW("\tbAlternateSetting: 0x%02x  \r\n", hport->config.intf[i].altsetting[j].intf_desc.bAlternateSetting);
            USB_LOG_RAW("\tbNumEndpoints: 0x%02x      \r\n", hport->config.intf[i].altsetting[j].intf_desc.bNumEndpoints);
            USB_LOG_RAW("\tbInterfaceClass: 0x%02x    \r\n", hport->config.intf[i].altsetting[j].intf_desc.bInterfaceClass);
            USB_LOG_RAW("\tbInterfaceSubClass: 0x%02x \r\n", hport->config.intf[i].altsetting[j].intf_desc.bInterfaceSubClass);
            USB_LOG_RAW("\tbInterfaceProtocol: 0x%02x \r\n", hport->config.intf[i].altsetting[j].intf_desc.bInterfaceProtocol);
            USB_LOG_RAW("\tiInterface: 0x%02x         \r\n", hport->config.intf[i].altsetting[j].intf_desc.iInterface);

            for (uint8_t k = 0; k < hport->config.intf[i].altsetting[j].intf_desc.bNumEndpoints &&
                 k < CONFIG_USBHOST_MAX_ENDPOINTS; k++) {
                USB_LOG_RAW("\t\tEndpoint Descriptor:\r\n");
                USB_LOG_RAW("\t\tbLength: 0x%02x          \r\n", hport->config.intf[i].altsetting[j].ep[k].ep_desc.bLength);
                USB_LOG_RAW("\t\tbDescriptorType: 0x%02x  \r\n", hport->config.intf[i].altsetting[j].ep[k].ep_desc.bDescriptorType);
                USB_LOG_RAW("\t\tbEndpointAddress: 0x%02x \r\n", hport->config.intf[i].altsetting[j].ep[k].ep_desc.bEndpointAddress);
                USB_LOG_RAW("\t\tbmAttributes: 0x%02x     \r\n", hport->config.intf[i].altsetting[j].ep[k].ep_desc.bmAttributes);
                USB_LOG_RAW("\t\twMaxPacketSize: 0x%04x   \r\n", hport->config.intf[i].altsetting[j].ep[k].ep_desc.wMaxPacketSize);
                USB_LOG_RAW("\t\tbInterval: 0x%02x        \r\n", hport->config.intf[i].altsetting[j].ep[k].ep_desc.bInterval);
            }
        }
    }
}

#if !defined(WORK_EVIDENCE_DIAGNOSTICS) || WORK_EVIDENCE_DIAGNOSTICS
static void usbh_print_setup(struct usb_setup_packet *setup)
{
    (void)setup;

    USB_LOG_DBG("Setup: "
                "bmRequestType 0x%02x, bRequest 0x%02x, wValue 0x%04x, wIndex 0x%04x, wLength 0x%04x\r\n",
                setup->bmRequestType,
                setup->bRequest,
                setup->wValue,
                setup->wIndex,
                setup->wLength);
}
#endif

static int usbh_get_default_mps(int speed)
{
    switch (speed) {
        case USB_SPEED_LOW: /* For low speed, we use 8 bytes */
            return 8;
        case USB_SPEED_FULL: /* For full or high speed, we use 64 bytes */
        case USB_SPEED_HIGH:
            return 64;
        case USB_SPEED_SUPER: /* For super speed , we must use 512 bytes */
        case USB_SPEED_SUPER_PLUS:
            return 512;
        default:
            return 64;
    }
}

static uint16_t usbh_control_data_length(int result, uint16_t requested)
{
    uint32_t length;

    if (result <= 0 || requested == 0) {
        return 0;
    }

    /* The ART-Pi2 DWC2 port includes the eight setup bytes in its positive
     * control-transfer result.  Keep the public CherryUSB return convention
     * unchanged (HID/MSC use it), but normalize observer lengths here. */
    length = result >= USB_SIZEOF_SETUP_PACKET ?
             (uint32_t)result - USB_SIZEOF_SETUP_PACKET : (uint32_t)result;
    if (length > requested) {
        length = requested;
    }
    return (uint16_t)length;
}

static void usbh_add_string_index(uint8_t *indices, uint16_t *count,
                                  uint8_t index)
{
    uint16_t i;

    if (!indices || !count || index == 0) {
        return;
    }
    for (i = 0; i < *count; i++) {
        if (indices[i] == index) {
            return;
        }
    }
    if (*count < 256U) {
        indices[(*count)++] = index;
    }
}

static void usbh_collect_string_indices(const uint8_t *data, uint16_t length,
                                        uint8_t *indices, uint16_t *count)
{
    uint16_t offset = 0;

    if (!data || !indices || !count) {
        return;
    }
    while (offset + 2U <= length) {
        uint8_t descriptor_length = data[offset];
        uint8_t descriptor_type = data[offset + 1U];

        if (descriptor_length < 2U || descriptor_length > length - offset) {
            break;
        }
        if (offset == 0U && descriptor_type == USB_DESCRIPTOR_TYPE_CONFIGURATION &&
            descriptor_length >= USB_SIZEOF_CONFIG_DESC) {
            usbh_add_string_index(indices, count, data[offset + 6U]);
        } else if (descriptor_type == USB_DESCRIPTOR_TYPE_INTERFACE &&
                   descriptor_length >= USB_SIZEOF_INTERFACE_DESC) {
            usbh_add_string_index(indices, count, data[offset + 8U]);
        }
        offset = (uint16_t)(offset + descriptor_length);
    }
}

static int usbh_get_raw_string_descriptor(struct usbh_hubport *hport,
                                          uint8_t index, uint16_t language_id)
{
    struct usb_setup_packet *setup = hport->setup;

    setup->bmRequestType = USB_REQUEST_DIR_IN | USB_REQUEST_STANDARD |
                           USB_REQUEST_RECIPIENT_DEVICE;
    setup->bRequest = USB_REQUEST_GET_DESCRIPTOR;
    setup->wValue = (uint16_t)((USB_DESCRIPTOR_TYPE_STRING << 8U) | index);
    setup->wIndex = index == 0U ? 0U : language_id;
    setup->wLength = 255U;
    return usbh_control_transfer(hport, setup,
                                 ep0_request_buffer[hport->bus->busid]);
}

int usbh_enumerate(struct usbh_hubport *hport)
{
    struct usb_interface_descriptor *intf_desc;
    struct usb_setup_packet *setup;
    struct usb_device_descriptor *dev_desc;
    struct usb_endpoint_descriptor *ep;
    uint8_t *request_buffer;
    uint8_t *selected_buffer;
    uint8_t string_indices[256] = {0};
    uint16_t string_count = 0U;
    uint16_t ep_mps;
    uint16_t data_length;
    uint16_t language_id = 0x0409U;
    uint16_t selected_length = 0U;
    uint8_t config_value = 0U;
    uint8_t selected_config_index = 0xFFU;
    uint8_t config_index;
    uint8_t config_count;
    int dev_addr;
    int ret = 0;
    rt_bool_t selected_ready = RT_FALSE;

    hport->setup = (struct usb_setup_packet *)&g_setup_buffer[
        hport->bus->busid][hport->parent->index - 1][hport->port - 1];
    setup = hport->setup;
    ep = &hport->ep0;
    request_buffer = ep0_request_buffer[hport->bus->busid];
    selected_buffer = selected_config_buffer[hport->bus->busid];
    hport->raw_config_desc = NULL;
    memset(&hport->config, 0, sizeof(hport->config));

    usbh_enumeration_started_observer(hport);

    /* Configure EP0 with the default packet size and address zero. */
    ep->bEndpointAddress = 0x00;
    ep->bDescriptorType = USB_DESCRIPTOR_TYPE_ENDPOINT;
    ep->bmAttributes = USB_ENDPOINT_TYPE_CONTROL;
    ep->wMaxPacketSize = usbh_get_default_mps(hport->speed);
    ep->bInterval = 0;
    ep->bLength = 7;
    hport->dev_addr = 0;

    setup->bmRequestType = USB_REQUEST_DIR_IN | USB_REQUEST_STANDARD |
                           USB_REQUEST_RECIPIENT_DEVICE;
    setup->bRequest = USB_REQUEST_GET_DESCRIPTOR;
    setup->wValue = (uint16_t)(USB_DESCRIPTOR_TYPE_DEVICE << 8U);
    setup->wIndex = 0;
    setup->wLength = 8;
    ret = usbh_control_transfer(hport, setup, request_buffer);
    data_length = usbh_control_data_length(ret, 8U);
    if (ret < 0 || data_length < 8U) {
        USB_LOG_ERR("Failed to get device descriptor,errorcode:%d\r\n", ret);
        if (ret >= 0) ret = -USB_ERR_INVAL;
        goto errout;
    }
    ret = parse_device_descriptor(hport,
                                  (struct usb_device_descriptor *)request_buffer,
                                  data_length);
    if (ret < 0) {
        USB_LOG_ERR("Parse device descriptor prefix fail\r\n");
        goto errout;
    }

    dev_desc = (struct usb_device_descriptor *)request_buffer;
    ep_mps = dev_desc->bcdUSB >= USB_3_0 ?
             (uint16_t)(1U << dev_desc->bMaxPacketSize0) :
             dev_desc->bMaxPacketSize0;
    USB_LOG_DBG("Device rev=%04x cls=%02x sub=%02x proto=%02x size=%d\r\n",
                dev_desc->bcdUSB, dev_desc->bDeviceClass,
                dev_desc->bDeviceSubClass, dev_desc->bDeviceProtocol, ep_mps);
    ep->wMaxPacketSize = ep_mps;

    dev_addr = usbh_allocate_devaddr(&hport->bus->devgen);
    if (dev_addr < 0) {
        ret = dev_addr;
        USB_LOG_ERR("Failed to allocate devaddr,errorcode:%d\r\n", ret);
        goto errout;
    }

    setup->bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_STANDARD |
                           USB_REQUEST_RECIPIENT_DEVICE;
    setup->bRequest = USB_REQUEST_SET_ADDRESS;
    setup->wValue = (uint16_t)dev_addr;
    setup->wIndex = 0;
    setup->wLength = 0;
    ret = usbh_control_transfer(hport, setup, NULL);
    if (ret < 0) {
        USB_LOG_ERR("Failed to set devaddr,errorcode:%d\r\n", ret);
        goto errout;
    }
    usb_osal_msleep(10);
    hport->dev_addr = (uint8_t)dev_addr;

    /* Read and publish the complete Device Descriptor. */
    setup->bmRequestType = USB_REQUEST_DIR_IN | USB_REQUEST_STANDARD |
                           USB_REQUEST_RECIPIENT_DEVICE;
    setup->bRequest = USB_REQUEST_GET_DESCRIPTOR;
    setup->wValue = (uint16_t)(USB_DESCRIPTOR_TYPE_DEVICE << 8U);
    setup->wIndex = 0;
    setup->wLength = USB_SIZEOF_DEVICE_DESC;
    ret = usbh_control_transfer(hport, setup, request_buffer);
    data_length = usbh_control_data_length(ret, USB_SIZEOF_DEVICE_DESC);
    if (ret < 0 || data_length < USB_SIZEOF_DEVICE_DESC) {
        USB_LOG_ERR("Failed to get full device descriptor,errorcode:%d\r\n", ret);
        if (ret >= 0) ret = -USB_ERR_INVAL;
        goto errout;
    }
    ret = parse_device_descriptor(hport,
                                  (struct usb_device_descriptor *)request_buffer,
                                  data_length);
    if (ret < 0) {
        goto errout;
    }
    usbh_device_descriptor_observer(hport, request_buffer, data_length);
    dev_desc = (struct usb_device_descriptor *)request_buffer;
    usbh_add_string_index(string_indices, &string_count, dev_desc->iManufacturer);
    usbh_add_string_index(string_indices, &string_count, dev_desc->iProduct);
    usbh_add_string_index(string_indices, &string_count, dev_desc->iSerialNumber);
    config_count = dev_desc->bNumConfigurations;
    USB_LOG_INFO("New device found,idVendor:%04x,idProduct:%04x,bcdDevice:%04x\r\n",
                 dev_desc->idVendor, dev_desc->idProduct, dev_desc->bcdDevice);
    USB_LOG_INFO("The device has %d bNumConfigurations\r\n", config_count);

    /* Read every configuration.  A configuration that is too large for the
     * USB control buffer is still reported to the inventory as truncated; it
     * must not turn an otherwise identifiable device into an enumeration
     * failure.  The first complete, parseable configuration is selected for
     * the existing class drivers. */
    for (config_index = 0U; config_index < config_count; config_index++) {
        uint16_t total_length = 0U;
        int config_ret;

        setup->bmRequestType = USB_REQUEST_DIR_IN | USB_REQUEST_STANDARD |
                               USB_REQUEST_RECIPIENT_DEVICE;
        setup->bRequest = USB_REQUEST_GET_DESCRIPTOR;
        setup->wValue = (uint16_t)((USB_DESCRIPTOR_TYPE_CONFIGURATION << 8U) |
                                   config_index);
        setup->wIndex = 0;
        setup->wLength = USB_SIZEOF_CONFIG_DESC;
        config_ret = usbh_control_transfer(hport, setup, request_buffer);
        data_length = usbh_control_data_length(config_ret,
                                               USB_SIZEOF_CONFIG_DESC);
        if (data_length >= 4U && request_buffer[1] ==
            USB_DESCRIPTOR_TYPE_CONFIGURATION) {
            total_length = (uint16_t)request_buffer[2] |
                           ((uint16_t)request_buffer[3] << 8U);
        }
        if (config_ret < 0 || data_length < USB_SIZEOF_CONFIG_DESC ||
            total_length < USB_SIZEOF_CONFIG_DESC) {
            usbh_configuration_descriptor_observer(
                hport, config_index, request_buffer, data_length,
                total_length, config_ret < 0 ? config_ret : -USB_ERR_INVAL);
            USB_LOG_WRN("Configuration %u header unavailable,errorcode:%d\r\n",
                        config_index, config_ret);
            continue;
        }

        if (total_length > CONFIG_USBHOST_REQUEST_BUFFER_LEN) {
            usbh_collect_string_indices(request_buffer, data_length,
                                        string_indices, &string_count);
            usbh_configuration_descriptor_observer(
                hport, config_index, request_buffer, data_length,
                total_length, data_length);
            USB_LOG_WRN("Configuration %u is %u bytes; buffer limit is %u, diagnostic prefix kept\r\n",
                        config_index, total_length,
                        (unsigned int)CONFIG_USBHOST_REQUEST_BUFFER_LEN);
            continue;
        }

        setup->wLength = total_length;
        config_ret = usbh_control_transfer(hport, setup, request_buffer);
        data_length = usbh_control_data_length(config_ret, total_length);
        usbh_configuration_descriptor_observer(
            hport, config_index, request_buffer, data_length, total_length,
            config_ret < 0 ? config_ret : data_length);
        if (data_length >= USB_SIZEOF_CONFIG_DESC) {
            usbh_collect_string_indices(request_buffer, data_length,
                                        string_indices, &string_count);
        }
        if (config_ret < 0 || data_length < total_length) {
            USB_LOG_WRN("Configuration %u incomplete: %u/%u bytes\r\n",
                        config_index, data_length, total_length);
            continue;
        }

        if (!selected_ready) {
            memcpy(selected_buffer, request_buffer, total_length);
            if (parse_config_descriptor(
                    hport,
                    (struct usb_configuration_descriptor *)selected_buffer,
                    total_length) == 0) {
                selected_ready = RT_TRUE;
                selected_length = total_length;
                selected_config_index = config_index;
                config_value = ((struct usb_configuration_descriptor *)
                                selected_buffer)->bConfigurationValue;
            }
            else {
                USB_LOG_WRN("Configuration %u has malformed class storage; raw descriptor retained\r\n",
                            config_index);
            }
        }
    }

#ifdef CONFIG_USBHOST_GET_STRING_DESC
    /* Query Language ID first, then every string index referenced by the
     * Device and all captured configurations.  The control observer stores
     * the raw UTF-16 descriptor as well as a UTF-8 preview. */
    ret = usbh_get_raw_string_descriptor(hport, 0U, 0U);
    data_length = usbh_control_data_length(ret, 255U);
    if (ret >= 0 && data_length >= 4U &&
        request_buffer[1] == USB_DESCRIPTOR_TYPE_STRING) {
        language_id = (uint16_t)request_buffer[2] |
                      ((uint16_t)request_buffer[3] << 8U);
    }
    for (uint16_t string_index = 0U; string_index < string_count;
         string_index++) {
        (void)usbh_get_raw_string_descriptor(
            hport, string_indices[string_index], language_id);
    }
#endif

    if (!selected_ready) {
        /* Device identity and all available descriptor prefixes are still a
         * successful diagnostic enumeration even when no class-driver-sized
         * configuration could be selected. */
        USB_LOG_INFO("Enumeration complete for diagnostic-only device (no selectable configuration)\r\n");
        ret = 0;
        goto errout;
    }

    hport->raw_config_desc = selected_buffer;
    USB_LOG_INFO("Selected configuration %u, %u interfaces\r\n",
                 selected_config_index,
                 ((struct usb_configuration_descriptor *)selected_buffer)->bNumInterfaces);

    setup->bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_STANDARD |
                           USB_REQUEST_RECIPIENT_DEVICE;
    setup->bRequest = USB_REQUEST_SET_CONFIGURATION;
    setup->wValue = config_value;
    setup->wIndex = 0;
    setup->wLength = 0;
    ret = usbh_control_transfer(hport, setup, NULL);
    if (ret < 0) {
        USB_LOG_ERR("Failed to set configuration,errorcode:%d\r\n", ret);
        goto errout;
    }

#ifdef CONFIG_USBHOST_MSOS_ENABLE
    setup->bmRequestType = USB_REQUEST_DIR_IN | USB_REQUEST_VENDOR |
                           USB_REQUEST_RECIPIENT_DEVICE;
    setup->bRequest = CONFIG_USBHOST_MSOS_VENDOR_CODE;
    setup->wValue = 0;
    setup->wIndex = 0x0004;
    setup->wLength = 16;
    ret = usbh_control_transfer(hport, setup, request_buffer);
    if (ret < 0 && ret != -USB_ERR_STALL) {
        USB_LOG_ERR("Failed to get msosv1 compat id,errorcode:%d\r\n", ret);
        goto errout;
    }
#endif

    USB_LOG_INFO("Enumeration success, start loading class driver\r\n");
    for (uint8_t i = 0U; i < hport->config.config_desc.bNumInterfaces &&
         i < CONFIG_USBHOST_MAX_INTERFACES; i++) {
        int class_ret;
        struct usbh_class_driver *class_driver;

        if (hport->config.intf[i].altsetting_num == 0U) {
            continue;
        }
        intf_desc = &hport->config.intf[i].altsetting[0].intf_desc;
        if (intf_desc->bInterfaceClass == USB_DEVICE_CLASS_CDC &&
            i + 1U >= CONFIG_USBHOST_MAX_INTERFACES) {
            USB_LOG_WRN("CDC Interface %u has no room for its data interface; diagnostic-only\r\n",
                        intf_desc->bInterfaceNumber);
            usbh_class_binding_observer(hport, intf_desc->bInterfaceNumber,
                                        NULL, 0);
            continue;
        }
        class_driver = (struct usbh_class_driver *)usbh_find_class_driver(
            intf_desc->bInterfaceClass, intf_desc->bInterfaceSubClass,
            intf_desc->bInterfaceProtocol, hport->device_desc.idVendor,
            hport->device_desc.idProduct);
        if (class_driver == NULL) {
            USB_LOG_INFO("No class driver for Interface %u Class:0x%02x Subclass:0x%02x Protocol:0x%02x; diagnostic-only\r\n",
                         intf_desc->bInterfaceNumber,
                         intf_desc->bInterfaceClass,
                         intf_desc->bInterfaceSubClass,
                         intf_desc->bInterfaceProtocol);
            usbh_class_binding_observer(hport, intf_desc->bInterfaceNumber,
                                        NULL, 0);
            continue;
        }
        hport->config.intf[i].class_driver = class_driver;
        USB_LOG_INFO("Loading %s class driver\r\n", class_driver->driver_name);
        class_ret = CLASS_CONNECT(hport, i);
        usbh_class_binding_observer(hport, intf_desc->bInterfaceNumber,
                                    class_driver, class_ret);
        if (class_ret < 0) {
            USB_LOG_WRN("Class driver %s failed on Interface %u (%d); USB enumeration remains successful\r\n",
                        class_driver->driver_name,
                        intf_desc->bInterfaceNumber, class_ret);
        }
    }
    ret = 0;

errout:
    usbh_enumeration_finished_observer(hport, ret);
    /* raw_config_desc points at the static selected-config buffer.  It is
     * valid through every CLASS_CONNECT call and must never be returned to
     * the RT-Thread heap. */
    hport->raw_config_desc = NULL;
    RT_UNUSED(selected_length);
    return ret;
}

void usbh_hubport_release(struct usbh_hubport *hport)
{
    if (hport->connected) {
        usbh_hubport_release_observer(hport);
        hport->connected = false;
        usbh_free_devaddr(hport);
        for (uint8_t i = 0; i < hport->config.config_desc.bNumInterfaces &&
             i < CONFIG_USBHOST_MAX_INTERFACES; i++) {
            if (hport->config.intf[i].class_driver && hport->config.intf[i].class_driver->disconnect) {
                CLASS_DISCONNECT(hport, i);
            }
        }
        hport->config.config_desc.bNumInterfaces = 0;
        usbh_kill_urb(&hport->ep0_urb);
        if (hport->mutex) {
            usb_osal_mutex_delete(hport->mutex);
        }
    }
}

static void usbh_bus_init(struct usbh_bus *bus, uint8_t busid, uintptr_t reg_base)
{
    memset(bus, 0, sizeof(struct usbh_bus));
    bus->busid = busid;
    bus->hcd.hcd_id = busid;
    bus->hcd.reg_base = reg_base;

    /* devaddr 1 is for roothub */
    bus->devgen.next = 2;

    usb_slist_add_tail(&g_bus_head, &bus->list);
}

int usbh_initialize(uint8_t busid, uintptr_t reg_base)
{
    struct usbh_bus *bus;

    if (busid >= CONFIG_USBHOST_MAX_BUS) {
        USB_LOG_ERR("bus overflow\r\n");
        while (1) {
        }
    }

    bus = &g_usbhost_bus[busid];

    usbh_bus_init(bus, busid, reg_base);

#ifdef __ARMCC_VERSION /* ARM C Compiler */
    extern const int usbh_class_info$$Base;
    extern const int usbh_class_info$$Limit;
    usbh_class_info_table_begin = (struct usbh_class_info *)&usbh_class_info$$Base;
    usbh_class_info_table_end = (struct usbh_class_info *)&usbh_class_info$$Limit;
#elif defined(__GNUC__)
    extern uint32_t __usbh_class_info_start__;
    extern uint32_t __usbh_class_info_end__;
    usbh_class_info_table_begin = (struct usbh_class_info *)&__usbh_class_info_start__;
    usbh_class_info_table_end = (struct usbh_class_info *)&__usbh_class_info_end__;
#elif defined(__ICCARM__) || defined(__ICCRX__) || defined(__ICCRISCV__)
    usbh_class_info_table_begin = (struct usbh_class_info *)__section_begin(".usbh_class_info");
    usbh_class_info_table_end = (struct usbh_class_info *)__section_end(".usbh_class_info");
#endif
    usbh_hub_initialize(bus);
    return 0;
}

int usbh_deinitialize(uint8_t busid)
{
    struct usbh_bus *bus;

    if (busid >= CONFIG_USBHOST_MAX_BUS) {
        USB_LOG_ERR("bus overflow\r\n");
        while (1) {
        }
    }

    bus = &g_usbhost_bus[busid];

    usbh_hub_deinitialize(bus);

    usb_slist_remove(&g_bus_head, &bus->list);

    return 0;
}

int usbh_control_transfer(struct usbh_hubport *hport, struct usb_setup_packet *setup, uint8_t *buffer)
{
    struct usbh_urb *urb;
    int ret;
    uint32_t started_at_us = 0U;
    uint32_t completed_at_us = 0U;
    bool timing_valid;

    if (!hport || !setup) {
        return -USB_ERR_INVAL;
    }

    urb = &hport->ep0_urb;

    usb_osal_mutex_take(hport->mutex);

#if !defined(WORK_EVIDENCE_DIAGNOSTICS) || WORK_EVIDENCE_DIAGNOSTICS
    usbh_print_setup(setup);
#endif

    usbh_control_urb_fill(urb, hport, setup, buffer, setup->wLength, CONFIG_USBHOST_CONTROL_TRANSFER_TIMEOUT, NULL, NULL);
    timing_valid = usbh_control_transfer_started_observer(
        hport, &started_at_us);
    ret = usbh_submit_urb(urb);
    timing_valid = usbh_control_transfer_completed_observer(
        hport, &completed_at_us) && timing_valid;
    if (ret == 0) {
        ret = urb->actual_length;
    }

    usb_osal_mutex_give(hport->mutex);
    usbh_control_transfer_observer(hport, setup, buffer, ret,
                                   started_at_us, completed_at_us,
                                   timing_valid);
    return ret;
}

int usbh_get_string_desc(struct usbh_hubport *hport, uint8_t index, uint8_t *output, uint16_t output_len)
{
    struct usb_setup_packet *setup;
    int ret;
    uint8_t *src;
    uint8_t *dst;
    uint16_t len;
    uint16_t data_length;
    uint16_t char_count;
    uint16_t i = 2;
    uint16_t j = 0;

    if (!hport || !output || output_len == 0U) {
        return -USB_ERR_INVAL;
    }
    setup = hport->setup;
    if (!setup) {
        return -USB_ERR_INVAL;
    }

    /* Get Manufacturer string */
    setup->bmRequestType = USB_REQUEST_DIR_IN | USB_REQUEST_STANDARD | USB_REQUEST_RECIPIENT_DEVICE;
    setup->bRequest = USB_REQUEST_GET_DESCRIPTOR;
    setup->wValue = (uint16_t)((USB_DESCRIPTOR_TYPE_STRING << 8) | index);
    setup->wIndex = 0x0409;
    setup->wLength = 255;

    ret = usbh_control_transfer(hport, setup, ep0_request_buffer[hport->bus->busid]);
    if (ret < 0) {
        return ret;
    }

    src = ep0_request_buffer[hport->bus->busid];
    dst = output;
    data_length = usbh_control_data_length(ret, setup->wLength);
    if (data_length < 2U || src[1] != USB_DESCRIPTOR_TYPE_STRING) {
        output[0] = '\0';
        return -USB_ERR_INVAL;
    }
    len = src[0];
    if (len < 2U || len > data_length) {
        len = data_length;
    }
    char_count = (uint16_t)((len - 2U) / 2U);
    if (char_count >= output_len) {
        char_count = (uint16_t)(output_len - 1U);
    }
    while (i + 1U < len && j < char_count) {
        dst[j] = src[i];
        i += 2;
        j++;
    }
    dst[j] = '\0';
    return 0;
}

int usbh_set_interface(struct usbh_hubport *hport, uint8_t intf, uint8_t altsetting)
{
    struct usb_setup_packet *setup = hport->setup;

    setup->bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_STANDARD | USB_REQUEST_RECIPIENT_INTERFACE;
    setup->bRequest = USB_REQUEST_SET_INTERFACE;
    setup->wValue = altsetting;
    setup->wIndex = intf;
    setup->wLength = 0;

    return usbh_control_transfer(hport, setup, NULL);
}

static void *usbh_list_all_interface_name(struct usbh_hub *hub, const char *devname)
{
    struct usbh_hubport *hport;
    struct usbh_hub *hub_next;
    void *priv;

    for (uint8_t port = 0; port < hub->nports; port++) {
        hport = &hub->child[port];
        if (hport->connected) {
            for (uint8_t itf = 0; itf < hport->config.config_desc.bNumInterfaces &&
                 itf < CONFIG_USBHOST_MAX_INTERFACES; itf++) {
                if (hport->config.intf[itf].class_driver && hport->config.intf[itf].class_driver->driver_name) {
                    if ((strncmp(hport->config.intf[itf].devname, devname, CONFIG_USBHOST_DEV_NAMELEN) == 0) && hport->config.intf[itf].priv)
                        return hport->config.intf[itf].priv;

                    if (strcmp(hport->config.intf[itf].class_driver->driver_name, "hub") == 0) {
                        hub_next = hport->config.intf[itf].priv;

                        if (hub_next && hub_next->connected) {
                            priv = usbh_list_all_interface_name(hub_next, devname);
                            if (priv) {
                                return priv;
                            }
                        }
                    }
                }
            }
        }
    }
    return NULL;
}

static void usbh_list_all_interface_driver(struct usbh_hub *hub)
{
    struct usbh_hubport *hport;
    struct usbh_hub *hub_next;
    const char *speed_table[] = { "error-speed", "low-speed", "full-speed", "high-speed", "wireless-speed", "super-speed", "superplus-speed" };

    for (uint8_t port = 0; port < hub->nports; port++) {
        hport = &hub->child[port];
        if (hport->connected) {
            for (uint8_t itf = 0; itf < hport->config.config_desc.bNumInterfaces &&
                 itf < CONFIG_USBHOST_MAX_INTERFACES; itf++) {
                if (hport->config.intf[itf].class_driver && hport->config.intf[itf].class_driver->driver_name) {
                    for (uint8_t j = 0; j < hub->index; j++) {
                        USB_LOG_RAW("\t");
                    }

                    USB_LOG_RAW("|__Port %u, dev addr:0x%02x, If %u, ClassDriver=%s, %s\r\n",
                                hport->port,
                                hport->dev_addr,
                                itf,
                                hport->config.intf[itf].class_driver->driver_name,
                                speed_table[hport->speed]);

                    if (strcmp(hport->config.intf[itf].class_driver->driver_name, "hub") == 0) {
                        hub_next = hport->config.intf[itf].priv;

                        if (hub_next && hub_next->connected) {
                            usbh_list_all_interface_driver(hub_next);
                        }
                    }
                }
            }
        }
    }
}

static void usbh_list_all_interface_desc(struct usbh_bus *bus, struct usbh_hub *hub)
{
    struct usbh_hubport *hport;
    struct usbh_hub *hub_next;

    for (uint8_t port = 0; port < hub->nports; port++) {
        hport = &hub->child[port];
        if (hport->connected) {
            USB_LOG_RAW("\r\nBus %u, Hub %u, Port %u, dev addr:0x%02x, VID:PID 0x%04x:0x%04x\r\n",
                        bus->busid,
                        hub->index,
                        hport->port,
                        hport->dev_addr,
                        hport->device_desc.idVendor,
                        hport->device_desc.idProduct);
            usbh_print_hubport_info(hport);

            for (uint8_t itf = 0; itf < hport->config.config_desc.bNumInterfaces &&
                 itf < CONFIG_USBHOST_MAX_INTERFACES; itf++) {
                if (hport->config.intf[itf].class_driver && hport->config.intf[itf].class_driver->driver_name) {
                    if (strcmp(hport->config.intf[itf].class_driver->driver_name, "hub") == 0) {
                        hub_next = hport->config.intf[itf].priv;

                        if (hub_next && hub_next->connected) {
                            usbh_list_all_interface_desc(bus, hub_next);
                        }
                    }
                }
            }
        }
    }
}

static struct usbh_hubport *usbh_list_all_hubport(struct usbh_hub *hub, uint8_t hub_index, uint8_t hub_port)
{
    struct usbh_hubport *hport;
    struct usbh_hub *hub_next;

    if (hub->index == hub_index) {
        hport = &hub->child[hub_port - 1];
        if (hport->connected) {
            return hport;
        } else {
            return NULL;
        }
    } else {
        for (uint8_t port = 0; port < hub->nports; port++) {
            hport = &hub->child[port];
            if (hport->connected) {
                for (uint8_t itf = 0; itf < hport->config.config_desc.bNumInterfaces &&
                     itf < CONFIG_USBHOST_MAX_INTERFACES; itf++) {
                    if (hport->config.intf[itf].class_driver && hport->config.intf[itf].class_driver->driver_name) {
                        if (strcmp(hport->config.intf[itf].class_driver->driver_name, "hub") == 0) {
                            hub_next = hport->config.intf[itf].priv;

                            if (hub_next && hub_next->connected) {
                                hport = usbh_list_all_hubport(hub_next, hub_index, hub_port);
                                if (hport) {
                                    return hport;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    return NULL;
}

void *usbh_find_class_instance(const char *devname)
{
    usb_slist_t *bus_list;
    struct usbh_hub *hub;
    struct usbh_bus *bus;
    void *priv;
    size_t flags;

    flags = usb_osal_enter_critical_section();
    usb_slist_for_each(bus_list, &g_bus_head)
    {
        bus = usb_slist_entry(bus_list, struct usbh_bus, list);
        hub = &bus->hcd.roothub;

        priv = usbh_list_all_interface_name(hub, devname);
        if (priv) {
            usb_osal_leave_critical_section(flags);
            return priv;
        }
    }
    usb_osal_leave_critical_section(flags);
    return NULL;
}

struct usbh_hubport *usbh_find_hubport(uint8_t busid, uint8_t hub_index, uint8_t hub_port)
{
    struct usbh_hub *hub;
    struct usbh_bus *bus;
    struct usbh_hubport *hport;
    size_t flags;

    flags = usb_osal_enter_critical_section();

    bus = &g_usbhost_bus[busid];
    hub = &bus->hcd.roothub;

    hport = usbh_list_all_hubport(hub, hub_index, hub_port);
    usb_osal_leave_critical_section(flags);
    return hport;
}

int lsusb(int argc, char **argv)
{
    usb_slist_t *bus_list;
    struct usbh_hub *hub;
    struct usbh_bus *bus;
    size_t flags;

    if (argc < 2) {
        USB_LOG_RAW("Usage: lsusb [options]...\r\n");
        USB_LOG_RAW("List USB devices\r\n");
        USB_LOG_RAW("  -v, --verbose\r\n");
        USB_LOG_RAW("      Increase verbosity (show descriptors)\r\n");
        // USB_LOG_RAW("  -s [[bus]:[devnum]]\r\n");
        // USB_LOG_RAW("      Show only devices with specified device and/or bus numbers (in decimal)\r\n");
        // USB_LOG_RAW("  -d vendor:[product]\r\n");
        // USB_LOG_RAW("      Show only devices with the specified vendor and product ID numbers (in hexadecimal)\r\n");
        USB_LOG_RAW("  -t, --tree\r\n");
        USB_LOG_RAW("      Dump the physical USB device hierachy as a tree\r\n");
        USB_LOG_RAW("  -V, --version\r\n");
        USB_LOG_RAW("      Show version of program\r\n");
        USB_LOG_RAW("  -h, --help\r\n");
        USB_LOG_RAW("      Show usage and help\r\n");
        return 0;
    }
    if (argc > 3) {
        return 0;
    }

    flags = usb_osal_enter_critical_section();

    if (strcmp(argv[1], "-V") == 0) {
        USB_LOG_RAW("CherryUSB Version %s\r\n", CHERRYUSB_VERSION_STR);
    }

    if (strcmp(argv[1], "-t") == 0) {
        usb_slist_for_each(bus_list, &g_bus_head)
        {
            bus = usb_slist_entry(bus_list, struct usbh_bus, list);
            hub = &bus->hcd.roothub;

            USB_LOG_RAW("/: Bus %u, Hub %u, ports=%u, is roothub\r\n",
                        bus->busid,
                        hub->index,
                        hub->nports);
            usbh_list_all_interface_driver(hub);
        }
    }

    if (strcmp(argv[1], "-v") == 0) {
        usb_slist_for_each(bus_list, &g_bus_head)
        {
            bus = usb_slist_entry(bus_list, struct usbh_bus, list);
            hub = &bus->hcd.roothub;

            usbh_list_all_interface_desc(bus, hub);
        }
    }

    usb_osal_leave_critical_section(flags);
    return 0;
}
