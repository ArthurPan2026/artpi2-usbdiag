#ifndef WORK_MONGOOSE_CONFIG_H
#define WORK_MONGOOSE_CONFIG_H

/* Mongoose 7.23 configuration for the ART-Pi2 application.  The board uses
 * RT-Thread as the scheduler, but the transport and all IPv4/TCP/HTTP work
 * are supplied by Mongoose's built-in stack. */
#define MG_ARCH MG_ARCH_CUBE
#define MG_ENABLE_TCPIP 1
#define MG_ENABLE_TCPIP_DRIVER_INIT 0
#define MG_ENABLE_DRIVER_CYW_SDIO 0
#define MG_ENABLE_DRIVER_CYW 0
#define MG_ENABLE_SOCKET 0
#define MG_ENABLE_BSD_SOCKETS 0
#define MG_ENABLE_BSD_PROTOTYPES 0
#define MG_ENABLE_IPV6 0
#define MG_ENABLE_DIRLIST 0
#define MG_TLS MG_TLS_NONE
#define MG_OTA MG_OTA_NONE
#define MG_OTA_STATE_GET() 0U
#define MG_OTA_STATE_SET(value) ((void)(value))
#define MG_OTA_ROLLBACK_TIMER_START() do { } while (0)
#define MG_OTA_ROLLBACK_TIMER_FEED() do { } while (0)
#define MG_OTA_ROLLBACK() do { } while (0)
#define MG_ENABLE_LOG 1
#define MG_ENABLE_TCPIP_PRINT_DEBUG_STATS 1
#define MG_ENABLE_CUSTOM_CALLOC 1
#define MG_ENABLE_CUSTOM_MILLIS 1
#define MG_IO_SIZE 2048
#define MG_MAX_RECV_SIZE 8192
#define MG_MAX_HTTP_HEADERS 24
#define MG_TCPIP_RX_BUDGET 4
/* Best measured Debug checkpoint: 44 KiB; Release remains at the 24 KiB base. */
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
#define MG_TCPIP_WIN 44000
#else
#define MG_TCPIP_WIN 24000
#endif
#define MG_ENABLE_TCPIP_TCPRTX 1
/* Keep per-connection retransmission storage bounded by the advertised window.
 * Debug retains its measured window; Release uses its smaller 24 KiB window. */
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
#define MG_TCPIP_TXQ_WINDOW 48000U
#else
#define MG_TCPIP_TXQ_WINDOW MG_TCPIP_WIN
#endif
#define MG_TCPIP_TXQ_EXTRA_BYTES 2048U
/* The built-in TCP listener stores half-open SYN entries in c->data.  The
 * default 32-byte scratch area holds only a few entries; this project uses
 * bounded application admission as the second line of defence, so reserve a
 * larger listener backlog without allocating another heap object. */
#define MG_DATA_SIZE 128

#endif
