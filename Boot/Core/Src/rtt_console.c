#include <rtthread.h>
/* Moni: no usart.h; huart4 is declared in main.c (see stm32h7xx_it.c) */
#include "main.h"
extern UART_HandleTypeDef huart4;
#include "rtt_console.h"

#define RTT_CONSOLE_RX_BUFFER_SIZE 256U
#define RTT_CONSOLE_TX_CHUNK_SIZE  64U

static rt_uint8_t s_rx_buffer[RTT_CONSOLE_RX_BUFFER_SIZE];
static volatile rt_uint16_t s_rx_head;
static volatile rt_uint16_t s_rx_tail;
static rt_uint8_t s_rx_byte;
static volatile rt_uint32_t s_rx_overflow;
static rt_bool_t s_console_ready;

static struct rt_semaphore s_rx_sem;
static struct rt_mutex s_tx_lock;

static void rtt_console_flush(const rt_uint8_t *buffer, rt_size_t size)
{
    if (size != 0U)
    {
        (void)HAL_UART_Transmit(&huart4, (uint8_t *)buffer,
                                (uint16_t)size, HAL_MAX_DELAY);
    }
}

static void rtt_console_output_raw(const char *str)
{
    rt_uint8_t buffer[RTT_CONSOLE_TX_CHUNK_SIZE];
    rt_size_t used = 0;

    while (*str != '\0')
    {
        if (*str == '\n')
        {
            buffer[used++] = '\r';
            if (used == sizeof(buffer))
            {
                rtt_console_flush(buffer, used);
                used = 0;
            }
        }

        buffer[used++] = (rt_uint8_t)*str++;
        if (used == sizeof(buffer))
        {
            rtt_console_flush(buffer, used);
            used = 0;
        }
    }

    rtt_console_flush(buffer, used);
}

/* ------------------------------------------------------------------ */
/* console rt_device (RT_USING_DEVICE): FinSH reads input via this     */
/* device; rt_kprintf writes via it once rt_console_set_device runs.   */
/* ------------------------------------------------------------------ */

static rt_ssize_t rtt_console_dev_read(rt_device_t dev, rt_off_t pos,
                                       void *buffer, rt_size_t size)
{
    rt_uint8_t *buf = (rt_uint8_t *)buffer;
    rt_size_t i;

    (void)dev;
    (void)pos;

    for (i = 0; i < size; i++)
    {
        rt_base_t level;
        rt_uint8_t ch;

        /* block until a byte arrives (released by UART RX IRQ) */
        (void)rt_sem_take(&s_rx_sem, RT_WAITING_FOREVER);

        level = rt_hw_interrupt_disable();
        if (s_rx_tail != s_rx_head)
        {
            ch = s_rx_buffer[s_rx_tail];
            s_rx_tail = (rt_uint16_t)((s_rx_tail + 1U) %
                                      RTT_CONSOLE_RX_BUFFER_SIZE);
            rt_hw_interrupt_enable(level);
            buf[i] = ch;
        }
        else
        {
            rt_hw_interrupt_enable(level);
            i--; /* spurious semaphore count, retry */
        }
    }

    return size;
}

static void rtt_console_flush_crlf(const rt_uint8_t *buffer, rt_size_t size)
{
    rt_uint8_t out[RTT_CONSOLE_TX_CHUNK_SIZE];
    rt_size_t used = 0;
    rt_size_t i;

    for (i = 0; i < size; i++)
    {
        if (buffer[i] == '\n')
        {
            out[used++] = '\r';
            if (used == sizeof(out))
            {
                rtt_console_flush(out, used);
                used = 0;
            }
        }
        out[used++] = buffer[i];
        if (used == sizeof(out))
        {
            rtt_console_flush(out, used);
            used = 0;
        }
    }
    rtt_console_flush(out, used);
}

static rt_ssize_t rtt_console_dev_write(rt_device_t dev, rt_off_t pos,
                                        const void *buffer, rt_size_t size)
{
    (void)dev;
    (void)pos;
    /* terminals need CRLF; plain \n leaves the cursor off the left edge */
    rtt_console_flush_crlf((const rt_uint8_t *)buffer, size);
    return size;
}

static struct rt_device s_console_dev;

rt_err_t rtt_console_init(void)
{
    if (s_console_ready)
    {
        return RT_EOK;
    }

    s_rx_head = 0;
    s_rx_tail = 0;
    s_rx_overflow = 0;

    RT_ASSERT(rt_sem_init(&s_rx_sem, "conrx", 0, RT_IPC_FLAG_PRIO) == RT_EOK);
    RT_ASSERT(rt_mutex_init(&s_tx_lock, "contx", RT_IPC_FLAG_PRIO) == RT_EOK);

    /* register as the RT-Thread console character device */
    s_console_dev.type    = RT_Device_Class_Char;
    s_console_dev.read    = rtt_console_dev_read;
    s_console_dev.write   = rtt_console_dev_write;
    s_console_dev.init    = RT_NULL;
    s_console_dev.open    = RT_NULL;
    s_console_dev.close   = RT_NULL;
    s_console_dev.control = RT_NULL;
    if (rt_device_register(&s_console_dev, "console", RT_DEVICE_FLAG_RDWR) != RT_EOK)
    {
        return -RT_ERROR;
    }
    rt_console_set_device("console");

    s_console_ready = RT_TRUE;
    if (HAL_UART_Receive_IT(&huart4, &s_rx_byte, 1) != HAL_OK)
    {
        s_console_ready = RT_FALSE;
        return -RT_ERROR;
    }

    return RT_EOK;
}

rt_uint32_t rtt_console_rx_overflow_get(void)
{
    rt_base_t level;
    rt_uint32_t overflow;

    level = rt_hw_interrupt_disable();
    overflow = s_rx_overflow;
    rt_hw_interrupt_enable(level);

    return overflow;
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    rt_uint16_t next;

    if ((huart != &huart4) || !s_console_ready)
    {
        return;
    }

    next = (rt_uint16_t)((s_rx_head + 1U) % RTT_CONSOLE_RX_BUFFER_SIZE);
    if (next == s_rx_tail)
    {
        s_rx_overflow++;
    }
    else
    {
        s_rx_buffer[s_rx_head] = s_rx_byte;
        s_rx_head = next;
        (void)rt_sem_release(&s_rx_sem);
    }

    (void)HAL_UART_Receive_IT(&huart4, &s_rx_byte, 1);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if ((huart == &huart4) && s_console_ready)
    {
        (void)HAL_UART_Receive_IT(&huart4, &s_rx_byte, 1);
    }
}

void rt_hw_console_output(const char *str)
{
    if (str == RT_NULL)
    {
        return;
    }

    /*
     * The console transport is blocking, so it is only valid in thread or
     * pre-scheduler context. Interrupt and fault paths must not enter HAL
     * UART transmission, even through a fallback path.
     */
    if (__get_IPSR() != 0U)
    {
        return;
    }

    if (s_console_ready &&
        (rt_thread_self() != RT_NULL) &&
        (rt_mutex_take(&s_tx_lock, RT_WAITING_FOREVER) == RT_EOK))
    {
        rtt_console_output_raw(str);
        (void)rt_mutex_release(&s_tx_lock);
        return;
    }

    rtt_console_output_raw(str);
}

signed char rt_hw_console_getchar(void)
{
    rt_base_t level;
    rt_uint8_t ch;

    if (!s_console_ready || (__get_IPSR() != 0U))
    {
        return -1;
    }

    while (rt_sem_take(&s_rx_sem, RT_WAITING_FOREVER) == RT_EOK)
    {
        level = rt_hw_interrupt_disable();
        if (s_rx_tail != s_rx_head)
        {
            ch = s_rx_buffer[s_rx_tail];
            s_rx_tail = (rt_uint16_t)((s_rx_tail + 1U) %
                                      RTT_CONSOLE_RX_BUFFER_SIZE);
            rt_hw_interrupt_enable(level);
            return (signed char)ch;
        }
        rt_hw_interrupt_enable(level);
    }

    return -1;
}
