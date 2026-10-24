/*
******************************************************************************
**
** @file        : LinkerScript.ld
**
** @author      : STM32CubeIDE
**
**  Abstract    : Linker script for STM32H7Rxx8 Device
**                      64Kbytes FLASH
**                      456Kbytes RAM
**
**                Set heap size, stack size and stack location according
**                to application requirements.
**
**                Set memory bank area and size if external memory is used
**
**  Target      : STMicroelectronics STM32
**
**  Distribution: The file is distributed as is, without any warranty
**                of any kind.
**
******************************************************************************
** @attention
**
** Copyright (c) 2023 STMicroelectronics.
** All rights reserved.
**
** This software is licensed under terms that can be found in the LICENSE file
** in the root directory of this software component.
** If no LICENSE file comes with this software, it is provided AS-IS.
**
******************************************************************************
*/

/* Entry Point */
ENTRY(Reset_Handler)


/* Memories definition */
MEMORY
{
  RAM           (xrw) : ORIGIN = __RAM,    LENGTH = __RAM_CACHED_SIZE
  RAM_NONCACHEABLEBUFFER (xrw) : ORIGIN = __USB_NOCACHE_BEGIN,  LENGTH = __RAM_NONCACHEABLEBUFFER_SIZE
  USB_EVIDENCE_RAM (rw) : ORIGIN = __EVIDENCE_RAM_BEGIN, LENGTH = __EVIDENCE_RAM_SIZE
  USB_HOST_STATE_RAM (rw) : ORIGIN = __USB_HOST_STATE_RAM_BEGIN, LENGTH = __USB_HOST_STATE_RAM_SIZE
  USB_DESCRIPTOR_RAM (rw) : ORIGIN = __USB_DESCRIPTOR_RAM_BEGIN, LENGTH = __USB_DESCRIPTOR_RAM_SIZE
  WIFI_WEB_RAM (rw) : ORIGIN = __WIFI_WEB_RAM_BEGIN, LENGTH = __WIFI_WEB_RAM_SIZE
  DISPLAY_RAM (rw) : ORIGIN = __DISPLAY_RAM_BEGIN, LENGTH = __DISPLAY_RAM_SIZE
  LVGL_RAM (rw) : ORIGIN = __LVGL_RAM_BEGIN, LENGTH = __LVGL_RAM_SIZE
  PSRAM_HEAP (rw) : ORIGIN = __PSRAM_HEAP_BEGIN, LENGTH = __PSRAM_HEAP_SIZE
  WIFI_SDIO_NC (rw) : ORIGIN = __WIFI_SDIO_NC_BEGIN, LENGTH = __WIFI_SDIO_NC_SIZE
  USB_STREAM_NC (rw) : ORIGIN = __USB_STREAM_NC_BEGIN, LENGTH = __USB_STREAM_NC_SIZE
  ITCM          (rw)  : ORIGIN = 0x00000000,  LENGTH = 0x00010000
  DTCM          (xrw) : ORIGIN = 0x20000000,  LENGTH = 0x00010000
  SRAMAHB       (rw)  : ORIGIN = 0x30000000,  LENGTH = 0x00008000
  BKPSRAM       (rw)  : ORIGIN = 0x38800000,  LENGTH = 0x00001000
  FLASH         (rx)  : ORIGIN = __FLASH_BEGIN, LENGTH = __FLASH_SIZE
}

/* Describe the ELF load permissions explicitly.  In particular, keep the
 * executable RAM functions apart from writable initialized data. */
PHDRS
{
  dtcm         PT_LOAD FLAGS(6);
  noncacheable PT_LOAD FLAGS(6);
  flash        PT_LOAD FLAGS(5);
  data         PT_LOAD FLAGS(6);
  ramfunc      PT_LOAD FLAGS(5);
  bss          PT_LOAD FLAGS(6);
  display      PT_LOAD FLAGS(6);
  evidence     PT_LOAD FLAGS(6);
  host_state   PT_LOAD FLAGS(6);
  descriptor   PT_LOAD FLAGS(6);
  wifi_web     PT_LOAD FLAGS(6);
  lvgl         PT_LOAD FLAGS(6);
  psram_heap   PT_LOAD FLAGS(6);
  wifi_sdio    PT_LOAD FLAGS(6);
}
/* Highest address of the user mode stack */
_estack = ORIGIN(DTCM) + LENGTH(DTCM); /* end of "DTCM" Ram type memory */

_Min_Heap_Size = 0x200;      /* required amount of heap  */
_Min_Stack_Size = 0x400; /* required amount of stack */

__FLASH_BEGIN  = 0x70000000;
/* ART-Pi2 uses a 64-MByte Winbond Octo-SPI NOR on XSPI2. */
/* Reserve 64 KiB for the watchdog trace A/B sidecar immediately before the
 * existing 260-KiB crash journal. Keep the journal start unchanged. */
__FLASH_SIZE   = 0x03FAF000;

__RAM          = 0x24000000;
__RAM_SIZE     = 0x72000;
/* SRAM4 is 72 KiB when ECC is disabled. The first 64 KiB stays in the
 * existing non-cacheable MPU region; a separate 8-KiB MPU region covers the
 * physical SRAM4 tail used by the watchdog trace ring. */
__RAM_CACHED_SIZE = 0x60000;
__USB_NOCACHE_BEGIN = 0x24060000;
__RAM_NONCACHEABLEBUFFER_SIZE = 0x12000;

/* The complete 32-MB PSRAM window is divided into ownership domains.  The
 * first 4 MB is reserved for the two 800x480 RGB565 framebuffers and future
 * display scratch buffers.  All other current objects are CPU-only and begin
 * at 0x90400000. */
__DISPLAY_RAM_BEGIN = 0x90000000;
__DISPLAY_RAM_SIZE = 0x00400000;
__EVIDENCE_RAM_BEGIN = 0x90400000;
__EVIDENCE_RAM_SIZE = 0x00400000;
__USB_HOST_STATE_RAM_BEGIN = 0x90800000;
__USB_HOST_STATE_RAM_SIZE = 0x00100000;
__USB_DESCRIPTOR_RAM_BEGIN = 0x90900000;
__USB_DESCRIPTOR_RAM_SIZE = 0x00100000;
__WIFI_WEB_RAM_BEGIN = 0x90A00000;
__WIFI_WEB_RAM_SIZE = 0x00200000;
__LVGL_RAM_BEGIN = 0x90C00000;
__LVGL_RAM_SIZE = 0x00200000;
__PSRAM_HEAP_BEGIN = 0x90E00000;
__PSRAM_HEAP_SIZE = 0x01000000;
/* SDMMC2 is an AHB master and can access the XSPI memory window.  Keep the
 * WHD TX staging buffers and SDMMC IDMA linked-list descriptors in a
 * dedicated MPU non-cacheable PSRAM slice; the remaining PSRAM is still
 * reserved for USB streaming. */
__WIFI_SDIO_NC_BEGIN = 0x91E00000;
__WIFI_SDIO_NC_SIZE = 0x00040000;
__USB_STREAM_NC_BEGIN = 0x91E40000;
__USB_STREAM_NC_SIZE = 0x001C0000;

/* Sections */
SECTIONS
{
  /* The startup code into "FLASH" Rom type memory */
  .isr_vector :
  {
    . = ALIGN(1024);
    __app_vector_table_start__ = .;
    KEEP(*(.isr_vector)) /* Startup code */
    . = ALIGN(4);
    __app_vector_table_end__ = .;
  } >FLASH :flash

  /* The Debug diagnostic build copies this table to ITCM as a fallback for
   * the case where VTOR is corrupted back to its reset value (zero). */
  .itcm_vector_backup (NOLOAD) : ALIGN(1024)
  {
    __itcm_vector_backup_start__ = .;
    . += 0x400;
    __itcm_vector_backup_end__ = .;
  } >ITCM :NONE

  ASSERT((ADDR(.isr_vector) & 0x3FF) == 0,
         "application vector table must be 1-KiB aligned")
  ASSERT(SIZEOF(.isr_vector) <= 0x3FC,
         "application vector table exceeds ITCM backup capacity")
  ASSERT(ADDR(.itcm_vector_backup) == ORIGIN(ITCM),
         "ITCM fallback table must start at address zero")
  ASSERT(SIZEOF(.itcm_vector_backup) == 0x400,
         "ITCM fallback table reservation must be 1 KiB")

  /* The program code and other data into "FLASH" Rom type memory */
  .text :
  {
    . = ALIGN(4);
    *(.text)           /* .text sections (code) */
    *(.text*)          /* .text* sections (code) */
    *(.glue_7)         /* glue arm to thumb code */
    *(.glue_7t)        /* glue thumb to arm code */
    *(.eh_frame)

    KEEP (*(.init))
    KEEP (*(.fini))

    . = ALIGN(4);
    _etext = .;        /* define a global symbols at end of code */
  } >FLASH :flash

  /* Constant data into "FLASH" Rom type memory */
  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)         /* .rodata sections (constants, strings, etc.) */
    *(.rodata*)        /* .rodata* sections (constants, strings, etc.) */
    . = ALIGN(4);
  } >FLASH :flash

  /* CherryUSB class drivers register through linker-collected descriptors. */
  .usbh_class_info (READONLY) :
  {
    . = ALIGN(4);
    __usbh_class_info_start__ = .;
    KEEP(*(.usbh_class_info))
    KEEP(*(.usbh_class_info.*))
    __usbh_class_info_end__ = .;
    . = ALIGN(4);
  } >FLASH :flash

  .ARM.extab (READONLY) :
  {
    . = ALIGN(4);
    *(.ARM.extab* .gnu.linkonce.armextab.*)
    . = ALIGN(4);
  } >FLASH :flash

  .ARM (READONLY) :
  {
    . = ALIGN(4);
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
    . = ALIGN(4);
  } >FLASH :flash

  .preinit_array (READONLY) :
  {
    . = ALIGN(4);
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP (*(.preinit_array*))
    PROVIDE_HIDDEN (__preinit_array_end = .);
    . = ALIGN(4);
  } >FLASH :flash

  .init_array (READONLY) :
  {
    . = ALIGN(4);
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP (*(SORT(.init_array.*)))
    KEEP (*(.init_array*))
    PROVIDE_HIDDEN (__init_array_end = .);
    . = ALIGN(4);
  } >FLASH :flash

  .fini_array (READONLY) :
  {
    . = ALIGN(4);
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP (*(SORT(.fini_array.*)))
    KEEP (*(.fini_array*))
    PROVIDE_HIDDEN (__fini_array_end = .);
    . = ALIGN(4);
  } >FLASH :flash

  /* RT-Thread component initializers. */
  .rti_fn : ALIGN(4)
  {
    __rt_init_begin = .;
    __rt_init_rti_start = .;
    KEEP(*(SORT_BY_NAME(.rti_fn.0)))
    __rt_init_rti_board_start = .;
    KEEP(*(SORT_BY_NAME(.rti_fn.1*)))
    __rt_init_rti_board_end = .;
    KEEP(*(SORT_BY_NAME(.rti_fn.2*)))
    KEEP(*(SORT_BY_NAME(.rti_fn.3*)))
    KEEP(*(SORT_BY_NAME(.rti_fn.4*)))
    KEEP(*(SORT_BY_NAME(.rti_fn.5*)))
    KEEP(*(SORT_BY_NAME(.rti_fn.6*)))
    __rt_init_rti_end = .;
    __rt_init_end = .;
  } >FLASH :flash

  /* The application has no FinSH command table, but keeping the section
   * makes this linker script compatible with optional diagnostics later. */
  FSymTab : ALIGN(4)
  {
    __fsymtab_start = .;
    KEEP(*(SORT_BY_NAME(FSymTab*)))
    __fsymtab_end = .;
  } >FLASH :flash

  /* Used by the startup to initialize data */
  _sidata = LOADADDR(.data);

  /* Initialized data sections into "RAM" Ram type memory */
  .data :
  {
    . = ALIGN(4);
    _sdata = .;        /* create a global symbol at data start */
    *(.data)           /* .data sections */
    *(.data*)          /* .data* sections */

    /* Keep the startup data image aligned before TLS and RAM functions. */
    . = ALIGN(8);

  } >RAM AT> FLASH :data

  /* Initialized TLS data section */
  .tdata : ALIGN(4)
  {
    *(.tdata .tdata.* .gnu.linkonce.td.*)
    . = ALIGN(4);
    PROVIDE(__tdata_end = .);
  } >RAM AT> FLASH :data

  /* Hot code used by Mongoose is copied to AXI SRAM before main(). Keep it
   * in its own RX load segment and within the startup copy span. */
  .ramfunc : ALIGN(4)
  {
    __ramfunc_start__ = .;
    *(.RamFunc)        /* .RamFunc sections */
    *(.RamFunc*)       /* .RamFunc* sections */
    . = ALIGN(4);
    __ramfunc_end__ = .;
  } >RAM AT> FLASH :ramfunc

  /* Startup copies .data, .tdata, alignment padding, and .ramfunc as one
   * contiguous image from _sidata through _edata. */
  _edata = .;
  PROVIDE(__data_end = .);

  PROVIDE( __tdata_start = ADDR(.tdata) );
  PROVIDE( __tdata_size = __tdata_end - __tdata_start );

  PROVIDE( __data_start = ADDR(.data) );
  PROVIDE( __data_size = __data_end - __data_start );

  PROVIDE( __tdata_source = LOADADDR(.tdata) );
  PROVIDE( __tdata_source_end = LOADADDR(.tdata) + SIZEOF(.tdata) );
  PROVIDE( __tdata_source_size = __tdata_source_end - __tdata_source );

  PROVIDE( __data_source = LOADADDR(.data) );
  PROVIDE( __data_source_end = LOADADDR(.ramfunc) + SIZEOF(.ramfunc) );
  PROVIDE( __data_source_size = __data_source_end - __data_source );


  /* Uninitialized data section into "RAM" Ram type memory */
  /* Uninitialized TLS data section */
  .tbss (NOLOAD) : ALIGN(4)
  {
    _sbss = .;         /* define a global symbol at bss start */
    __bss_start__ = _sbss;
    *(.tbss .tbss.*)
    . = ALIGN(4);
    PROVIDE( __tbss_end = . );
  } >RAM :bss

  PROVIDE( __tbss_start = ADDR(.tbss) );
  PROVIDE( __tbss_size = __tbss_end - __tbss_start );
  PROVIDE( __tbss_offset = ADDR(.tbss) - ADDR(.tdata) );

  PROVIDE( __tls_base = __tdata_start );
  PROVIDE( __tls_end = __tbss_end );
  PROVIDE( __tls_size = __tls_end - __tls_base );
  PROVIDE( __tls_align = MAX(ALIGNOF(.tdata), ALIGNOF(.tbss)) );
  PROVIDE( __tls_size_align = (__tls_size + __tls_align - 1) & ~(__tls_align - 1) );
  PROVIDE( __arm32_tls_tcb_offset = MAX(8, __tls_align) );
  PROVIDE( __arm64_tls_tcb_offset = MAX(16, __tls_align) );

  .bss (NOLOAD) : ALIGN(4)
  {
    /* This is used by the startup in order to initialize the .bss section */
    *(.bss)
    *(.bss*)
    *(COMMON)

    . = ALIGN(4);
    _ebss = .;         /* define a global symbol at bss end */
    __bss_end__ = _ebss;
      PROVIDE( __bss_end = .);
  } >RAM :bss
  PROVIDE( __non_tls_bss_start = ADDR(.bss) );

  PROVIDE( __bss_start = __tbss_start );
  PROVIDE( __bss_size = __bss_end - __bss_start );

  /* LTDC and NeoChrom share these two complete RGB565 surfaces.  They are
   * deliberately NOLOAD and non-cacheable; the application clears them once
   * before creating the direct-mode LVGL display. */
  .framebuffer (NOLOAD) : ALIGN(64)
  {
    __display_fb_start = .;
    KEEP(*(.framebuffer))
    KEEP(*(.framebuffer.*))
    . = ALIGN(64);
    __display_fb_end = .;
  } >DISPLAY_RAM :display

  /* Static UI surfaces are read by NeoChrom while the LTDC scans the
   * framebuffers.  Keep them in the same non-cacheable display domain so
   * scrolling can use a single RGB565 image without CPU copies. */
  .ui_cache (NOLOAD) : ALIGN(64)
  {
    __ui_cache_start = .;
    KEEP(*(.ui_cache))
    KEEP(*(.ui_cache.*))
    . = ALIGN(64);
    __ui_cache_end = .;
  } >DISPLAY_RAM :display

  /* Complete USB evidence, including the enlarged control-transfer records,
   * lives outside the framebuffer arena and never consumes the RT heap. */
  .usb_evidence_ram (NOLOAD) : ALIGN(32)
  {
    __usb_evidence_ram_start = .;
    *(.usb_evidence_ram)
    *(.usb_evidence_ram.*)
    . = ALIGN(32);
    __usb_evidence_ram_end = .;
  } >USB_EVIDENCE_RAM :evidence

  /* Mutable USB topology and Hub state is separate from raw descriptors. */
  .usb_host_state_ram (NOLOAD) : ALIGN(32)
  {
    __usb_host_state_ram_start = .;
    *(.usb_host_state_ram)
    *(.usb_host_state_ram.*)
    . = ALIGN(32);
    __usb_host_state_ram_end = .;
  } >USB_HOST_STATE_RAM :host_state

  /* Web evidence JSON and HTTP scratch storage. */
  .wifi_web_ram (NOLOAD) : ALIGN(32)
  {
    __wifi_web_ram_start = .;
    *(.wifi_web_ram)
    *(.wifi_web_ram.*)
    . = ALIGN(32);
    __wifi_web_ram_end = .;
  } >WIFI_WEB_RAM :wifi_web

  /* LVGL's CPU-owned allocator pool is isolated from RT-Thread's heap. */
  .lvgl_ram (NOLOAD) : ALIGN(32)
  {
    __lvgl_ram_start = .;
    *(.lvgl_ram)
    *(.lvgl_ram.*)
    . = ALIGN(32);
    __lvgl_ram_end = .;
  } >LVGL_RAM :lvgl

  /* Full USB Device/Configuration descriptor storage.  This section is
   * deliberately separate from the finite evidence event window and from
   * the RT-Thread heap. */
  .usb_descriptor_ram (NOLOAD) : ALIGN(32)
  {
    __usb_descriptor_ram_start = .;
    *(.usb_descriptor_ram)
    *(.usb_descriptor_ram.*)
    . = ALIGN(32);
    __usb_descriptor_ram_end = .;
  } >USB_DESCRIPTOR_RAM :descriptor

  /* Explicitly available large CPU-only PSRAM heap.  It is not connected to
   * RT-Thread's default heap; future UVC/UI buffers must opt in. */
  .psram_heap (NOLOAD) : ALIGN(32)
  {
    KEEP(*(.psram_heap))
    KEEP(*(.psram_heap.*))
    . = ALIGN(32);
  } >PSRAM_HEAP :psram_heap

  /* Board-side PDF generation owns a fixed output cache followed by a
   * bounded 8-MiB libHaru allocator. Both begin after all legacy static
   * .psram_heap objects, including the DMA2D diagnostic surface. */
  .pdf_report_cache (NOLOAD) : ALIGN(32)
  {
    __pdf_report_cache_start = .;
    KEEP(*(.pdf_report_cache))
    KEEP(*(.pdf_report_cache.*))
    . = ALIGN(32);
    __pdf_report_cache_end = .;
  } >PSRAM_HEAP :psram_heap

  .pdf_report_heap (NOLOAD) : ALIGN(32)
  {
    __libharu_heap_start = .;
    . += 0x00800000;
    . = ALIGN(32);
    __libharu_heap_end = .;
  } >PSRAM_HEAP :psram_heap

  /* Reserved non-cacheable PSRAM window for future USB streaming DMA. */
  .usb_stream_nc (NOLOAD) : ALIGN(32)
  {
    KEEP(*(.usb_stream_nc))
    KEEP(*(.usb_stream_nc.*))
    . = ALIGN(32);
  } >USB_STREAM_NC :NONE

  /* WHD packet ownership and SDMMC2 IDMA linked-list buffers. */
  .wifi_sdio_nc (NOLOAD) : ALIGN(32)
  {
    __wifi_sdio_nc_start = .;
    KEEP(*(.wifi_sdio_nc))
    KEEP(*(.wifi_sdio_nc.*))
    . = ALIGN(32);
    __wifi_sdio_nc_end = .;
  } >WIFI_SDIO_NC :wifi_sdio

  /* These symbols describe the whole linker-owned arenas, including the
   * currently empty output sections above. */
  PROVIDE(__psram_heap_start = ORIGIN(PSRAM_HEAP));
  PROVIDE(__psram_heap_end = ORIGIN(PSRAM_HEAP) + LENGTH(PSRAM_HEAP));
  PROVIDE(__usb_stream_nc_start = ORIGIN(USB_STREAM_NC));
  PROVIDE(__usb_stream_nc_end = ORIGIN(USB_STREAM_NC) + LENGTH(USB_STREAM_NC));
  PROVIDE(__wifi_sdio_nc_start = ORIGIN(WIFI_SDIO_NC));
  PROVIDE(__wifi_sdio_nc_end = ORIGIN(WIFI_SDIO_NC) + LENGTH(WIFI_SDIO_NC));

  /* RT-Thread thread control blocks and stacks stay in AXI SRAM.  The
   * external PSRAM is suitable for large application buffers, but keeping
   * scheduler context storage in on-chip SRAM avoids context-switch faults
   * while the XIP/SDIO traffic is busy. */
  .web_thread_ram (NOLOAD) : ALIGN(32)
  {
    __web_thread_ram_start = .;
    *(.web_thread_ram)
    *(.web_thread_ram.*)
    . = ALIGN(32);
    __web_thread_ram_end = .;
  } >DTCM :dtcm

  /* The RT-Thread heap begins after the normal internal BSS. LVGL's pool and
   * the display framebuffer are in dedicated PSRAM arenas. */
  PROVIDE(__rt_heap_start = ALIGN(__bss_end, 8));
  PROVIDE(__rt_heap_end = ORIGIN(RAM) + LENGTH(RAM));

  ASSERT(__usb_evidence_ram_end <= ORIGIN(USB_EVIDENCE_RAM) + LENGTH(USB_EVIDENCE_RAM),
         "USB evidence PSRAM region overflow")
  ASSERT(__usb_host_state_ram_end <= ORIGIN(USB_HOST_STATE_RAM) + LENGTH(USB_HOST_STATE_RAM),
         "USB Host state PSRAM region overflow")
  ASSERT(__usb_descriptor_ram_end <= ORIGIN(USB_DESCRIPTOR_RAM) + LENGTH(USB_DESCRIPTOR_RAM),
         "USB descriptor PSRAM region overflow")
  ASSERT(__wifi_web_ram_end <= ORIGIN(WIFI_WEB_RAM) + LENGTH(WIFI_WEB_RAM),
         "WiFi/Web PSRAM region overflow")
  ASSERT(__lvgl_ram_end <= ORIGIN(LVGL_RAM) + LENGTH(LVGL_RAM),
         "LVGL PSRAM region overflow")
  ASSERT(__libharu_heap_end <= ORIGIN(PSRAM_HEAP) + LENGTH(PSRAM_HEAP),
         "libHaru PSRAM heap exceeds its linker region")
  ASSERT(__nema_pool_end <= ORIGIN(RAM_NONCACHEABLEBUFFER) + LENGTH(RAM_NONCACHEABLEBUFFER),
         "NemaGFX pool internal RAM overflow")
  ASSERT(__display_fb_end <= ORIGIN(DISPLAY_RAM) + LENGTH(DISPLAY_RAM),
         "Display framebuffer PSRAM region overflow")
  ASSERT(__ui_cache_end <= ORIGIN(DISPLAY_RAM) + LENGTH(DISPLAY_RAM),
         "Static UI cache PSRAM region overflow")
  ASSERT(__wifi_sdio_nc_end <= ORIGIN(WIFI_SDIO_NC) + LENGTH(WIFI_SDIO_NC),
         "WiFi SDIO non-cacheable region overflow")

  /* NemaGFX's allocator and its circular command-list ring are accessed by
   * the GPU2D command processor.  Keep this compact pool in the internal
   * non-cacheable command window; direct framebuffer mode no longer needs the
   * old partial LVGL render buffer there. */
  .nema_pool (NOLOAD) : ALIGN(32)
  {
    __nema_pool_start = .;
    *(.nema_pool)
    *(.nema_pool.*)
    . = ALIGN(32);
    __nema_pool_end = .;
  } >RAM_NONCACHEABLEBUFFER :noncacheable

  RW_NONCACHEABLE (NOLOAD) : ALIGN(32)
  {
    __NONCACHEABLEBUFFER_BEGIN = .;
    . = ALIGN(32);
    KEEP(*(.noncacheable))
    KEEP(*(.noncacheable.*))
    KEEP(*(noncacheable_buffer))
    . = ALIGN(32);
    __NONCACHEABLEBUFFER_END = .;
  } > RAM_NONCACHEABLEBUFFER :noncacheable

  /* Fault records must be visible to SWD even when the fault occurred while
   * D-Cache was enabled.  Keep them in the same internal non-cacheable window
   * as the GPU command pool. */
  .fault_capture (NOLOAD) : ALIGN(32)
  {
    KEEP(*(.fault_capture))
    KEEP(*(.fault_capture.*))
  } > RAM_NONCACHEABLEBUFFER :noncacheable

  /* A 1-kHz, commit-last PC/LR ring survives IWDG system reset in SRAM4.
   * Its fixed address is shared with Boot; the map must keep all fault records
   * below it. SRAM4's final 8 KiB has a separate non-cacheable MPU region. */
  .watchdog_trace 0x2406C9A0 (NOLOAD) : ALIGN(32)
  {
    __watchdog_trace_start__ = .;
    KEEP(*(.watchdog_trace))
    KEEP(*(.watchdog_trace.*))
    . = ALIGN(32);
    __watchdog_trace_end__ = .;
  } > RAM_NONCACHEABLEBUFFER :noncacheable
  ASSERT(ADDR(.fault_capture) + SIZEOF(.fault_capture) <=
             ADDR(.watchdog_trace),
         "watchdog trace ring overlaps fault capture records")
  ASSERT(SIZEOF(.watchdog_trace) == 0x55A0,
         "watchdog trace ring must occupy 912 committed 24-byte samples and a 32-byte header")
  ASSERT(ADDR(.watchdog_trace) + SIZEOF(.watchdog_trace) <= 0x24072000,
         "watchdog trace ring exceeds physical SRAM4")

  /* BKPSRAM survives an IWDG system reset. Keep the retained context outside
   * startup's ordinary .bss clearing range and outside the cacheable RAM map. */
  .watchdog_bkpsram (NOLOAD) : ALIGN(32)
  {
    __watchdog_bkpsram_start__ = .;
    KEEP(*(.watchdog_bkpsram))
    . = ALIGN(32);
    __watchdog_bkpsram_end__ = .;
  } > BKPSRAM
  ASSERT(SIZEOF(.watchdog_bkpsram) == 0x1000,
         "watchdog BKPSRAM record must occupy exactly 4 KiB")

  /* User_heap_stack section, used to check that there is enough "RAM" Ram  type memory left */
  ._user_heap_stack (NOLOAD) :
  {
    . = ALIGN(8);
    PROVIDE ( end = . );
    PROVIDE ( _end = . );
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >DTCM :dtcm

  /* Remove information from the compiler libraries */
  /DISCARD/ :
  {
    libc.a:* ( * )
    libm.a:* ( * )
    libgcc.a:* ( * )
  }

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
