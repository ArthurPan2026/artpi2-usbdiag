/*
 * CherryUSB configuration for the ART-Pi2 STM32H7RS USB Host.
 *
 * CherryUSB's DWC2 host driver enables its own DMA path.  Static transfer
 * buffers are therefore aligned and placed in the linker-provided
 * non-cacheable AXI SRAM region.
 */
#ifndef CHERRYUSB_CONFIG_H
#define CHERRYUSB_CONFIG_H

#ifdef __RTTHREAD__
#include <rtthread.h>
#define CONFIG_USB_PRINTF(...) rt_kprintf(__VA_ARGS__)
#else
#include <stdio.h>
#define CONFIG_USB_PRINTF(...) printf(__VA_ARGS__)
#endif

#ifndef CONFIG_USB_DBG_LEVEL
#define CONFIG_USB_DBG_LEVEL USB_DBG_INFO
#endif

#define CONFIG_USB_PRINTF_COLOR_ENABLE
#define CONFIG_USB_DCACHE_ENABLE
#define CONFIG_USB_ALIGN_SIZE 32

#define USB_NOCACHE_RAM_SECTION \
    __attribute__((section(".noncacheable")))

#define USB_EVIDENCE_RAM_SECTION \
    __attribute__((section(".usb_evidence_ram"), aligned(32)))

#define USB_DESCRIPTOR_RAM_SECTION \
    __attribute__((section(".usb_descriptor_ram"), aligned(32)))

#define USB_HOST_STATE_RAM_SECTION \
    __attribute__((section(".usb_host_state_ram"), aligned(32)))

#define PSRAM_HEAP_RAM_SECTION \
    __attribute__((section(".psram_heap"), aligned(32)))

/* ================= USB Host Stack Configuration ================= */
#define CONFIG_USBHOST_MAX_BUS              1
#define CONFIG_USBHOST_MAX_RHPORTS          1
#define CONFIG_USBHOST_MAX_EXTHUBS          1
#define CONFIG_USBHOST_MAX_EHPORTS          4
#define CONFIG_USBHOST_MAX_INTERFACES       16
#define CONFIG_USBHOST_MAX_INTF_ALTSETTINGS 8
#define CONFIG_USBHOST_MAX_ENDPOINTS        16

#define CONFIG_USBHOST_MAX_CDC_ACM_CLASS    4
#define CONFIG_USBHOST_MAX_HID_CLASS        4
#define CONFIG_USBHOST_MAX_MSC_CLASS        2
#define CONFIG_USBHOST_MAX_AUDIO_CLASS      1
#define CONFIG_USBHOST_MAX_VIDEO_CLASS      1
#define CONFIG_USBHOST_MAX_VENDOR_SERIAL_CLASS 4

#define CONFIG_USBHOST_DEV_NAMELEN           16
#define CONFIG_USBHOST_PSC_PRIO              6
#define CONFIG_USBHOST_PSC_STACKSIZE         8192
#define CONFIG_USBHOST_GET_STRING_DESC
#define CONFIG_USBHOST_REQUEST_BUFFER_LEN    8192
#define CONFIG_USBHOST_CONTROL_TRANSFER_TIMEOUT 500
#define CONFIG_USBHOST_MSC_TIMEOUT           5000
/* USB 2.0 reset recovery is complete well before this conservative 100-ms
 * guard on ART-Pi2; keeping it configurable preserves compatibility with a
 * hub/device that needs a longer recovery interval. */
#define CONFIG_USBHOST_RESET_RECOVERY_DELAY_MS 100U

/* H7RS USB_OTG_HS uses the UTMI/internal high-speed PHY. */
#define CONFIG_USB_HS

#endif /* CHERRYUSB_CONFIG_H */
