#include "rtthread_uart4_device.h"

#include <rthw.h>

#include "stm32h7rsxx_hal.h"
#include "usart.h"

#define RTTHREAD_UART4_NAME  "uart4"
#define UART4_RX_BUFFER_SIZE 256U
#define UART4_TX_WAIT_LIMIT  1000000U

static struct rt_device s_uart4_device;
static uint8_t s_uart4_rx_byte;
static uint8_t s_uart4_rx_buffer[UART4_RX_BUFFER_SIZE];
static volatile uint16_t s_uart4_rx_head;
static volatile uint16_t s_uart4_rx_tail;
static volatile uint32_t s_uart4_error_count;
static rt_bool_t s_uart4_registered;

static rt_err_t uart4_device_init(rt_device_t device)
{
    (void)device;

    if (HAL_UART_Receive_IT(&huart4, &s_uart4_rx_byte, 1U) != HAL_OK)
    {
        return -RT_ERROR;
    }
    return RT_EOK;
}

static rt_err_t uart4_device_open(rt_device_t device, rt_uint16_t oflag)
{
    (void)device;
    (void)oflag;
    return RT_EOK;
}

static rt_err_t uart4_device_close(rt_device_t device)
{
    (void)device;
    return RT_EOK;
}

static rt_ssize_t uart4_device_read(rt_device_t device, rt_off_t position,
                                    void *buffer, rt_size_t size)
{
    uint8_t *destination = (uint8_t *)buffer;
    rt_size_t count = 0U;
    rt_base_t level;

    (void)device;
    (void)position;

    if (destination == RT_NULL || size == 0U)
    {
        return 0;
    }

    level = rt_hw_interrupt_disable();
    while (count < size && s_uart4_rx_tail != s_uart4_rx_head)
    {
        destination[count++] = s_uart4_rx_buffer[s_uart4_rx_tail];
        s_uart4_rx_tail = (uint16_t)((s_uart4_rx_tail + 1U) %
                                     UART4_RX_BUFFER_SIZE);
    }
    rt_hw_interrupt_enable(level);

    return (rt_ssize_t)count;
}

static rt_ssize_t uart4_device_write(rt_device_t device, rt_off_t position,
                                     const void *buffer, rt_size_t size)
{
    const uint8_t *source = (const uint8_t *)buffer;
    rt_size_t count = 0U;

    (void)device;
    (void)position;

    if (source == RT_NULL || size == 0U)
    {
        return 0;
    }

    while (count < size)
    {
        uint32_t guard = UART4_TX_WAIT_LIMIT;
        while ((UART4->ISR & USART_ISR_TXE_TXFNF) == 0U && guard-- != 0U)
        {
        }
        if (guard == 0U)
        {
            break;
        }
        UART4->TDR = source[count++];
    }

    return (rt_ssize_t)count;
}

static rt_err_t uart4_device_control(rt_device_t device, int command,
                                     void *argument)
{
    (void)device;
    (void)command;
    (void)argument;
    return -RT_ENOSYS;
}

static void uart4_rx_push(uint8_t byte)
{
    uint16_t head = s_uart4_rx_head;
    uint16_t next = (uint16_t)((head + 1U) % UART4_RX_BUFFER_SIZE);

    /* Drop the newest byte when the shell cannot keep up. */
    if (next != s_uart4_rx_tail)
    {
        s_uart4_rx_buffer[head] = byte;
        s_uart4_rx_head = next;
    }
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart == RT_NULL || huart->Instance != UART4)
    {
        return;
    }

    uart4_rx_push(s_uart4_rx_byte);

    /* Keep a one-byte receive armed for the next character. */
    (void)HAL_UART_Receive_IT(&huart4, &s_uart4_rx_byte, 1U);

    if (s_uart4_device.rx_indicate != RT_NULL)
    {
        (void)s_uart4_device.rx_indicate(&s_uart4_device, 1U);
    }
}

/* HAL stops an interrupt reception after an overrun/error.  Re-arm it here
 * after flushing the stale byte so a noisy or temporarily disconnected
 * console can never leave RXNEIE+ORE asserted forever.  This callback is
 * deliberately register/HAL-only and does not print from the ISR. */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart == RT_NULL || huart->Instance != UART4)
    {
        return;
    }

    s_uart4_error_count++;
    __HAL_UART_CLEAR_FLAG(huart, UART_CLEAR_OREF | UART_CLEAR_NEF |
                          UART_CLEAR_PEF | UART_CLEAR_FEF);
    __HAL_UART_SEND_REQ(huart, UART_RXDATA_FLUSH_REQUEST);
    huart->ErrorCode = HAL_UART_ERROR_NONE;
    (void)HAL_UART_Receive_IT(huart, &s_uart4_rx_byte, 1U);
}

rt_err_t rt_hw_uart4_device_init(void)
{
    rt_err_t result;

    if (s_uart4_registered)
    {
        return RT_EOK;
    }

    s_uart4_device.init = uart4_device_init;
    s_uart4_device.open = uart4_device_open;
    s_uart4_device.close = uart4_device_close;
    s_uart4_device.read = uart4_device_read;
    s_uart4_device.write = uart4_device_write;
    s_uart4_device.control = uart4_device_control;

    result = rt_device_register(&s_uart4_device, RTTHREAD_UART4_NAME,
                                RT_DEVICE_FLAG_RDWR |
                                RT_DEVICE_FLAG_INT_RX |
                                RT_DEVICE_FLAG_STREAM);
    if (result == RT_EOK)
    {
        s_uart4_registered = RT_TRUE;
    }
    return result;
}
