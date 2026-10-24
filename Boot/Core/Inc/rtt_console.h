#ifndef __RTT_CONSOLE_H__
#define __RTT_CONSOLE_H__
#include <rtthread.h>
rt_err_t rtt_console_init(void);
rt_uint32_t rtt_console_rx_overflow_get(void);
#endif
