#include <rtdevice.h>
#include <rtthread.h>

#include "i2c.h"

#define I2C2_BUS_NAME "i2c2"
#define I2C2_TIMEOUT_MS 20U

static rt_tick_t s_last_mem_error_log;
static rt_uint32_t s_mem_error_count;

int i2c2_hal_probe(uint8_t address7)
{
    HAL_StatusTypeDef status = HAL_I2C_IsDeviceReady(
        &hi2c2, (uint16_t)address7 << 1, 2U, 100U);

    rt_kprintf("[I2C2] probe 0x%02x status=%d error=0x%08x ISR=0x%08x\r\n",
               address7, (int)status, (unsigned)HAL_I2C_GetError(&hi2c2),
               (unsigned)hi2c2.Instance->ISR);
    return status == HAL_OK ? 0 : -1;
}

static rt_ssize_t i2c2_master_xfer(struct rt_i2c_bus_device *bus,
                                   struct rt_i2c_msg msgs[],
                                   rt_uint32_t count)
{
    (void)bus;

    if (msgs == RT_NULL || count == 0U)
    {
        return -RT_EINVAL;
    }

    /* GT911 uses a 16-bit register write followed by a repeated-start read.
     * HAL_I2C_Mem_Read emits that transaction atomically. */
    if (count == 2U &&
        (msgs[0].flags & RT_I2C_RD) == 0U &&
        (msgs[1].flags & RT_I2C_RD) != 0U &&
        (msgs[0].flags & (RT_I2C_ADDR_10BIT | RT_I2C_NO_START |
                          RT_I2C_NO_STOP | RT_I2C_IGNORE_NACK |
                          RT_I2C_NO_READ_ACK)) == 0U &&
        (msgs[1].flags & (RT_I2C_ADDR_10BIT | RT_I2C_NO_START |
                          RT_I2C_NO_STOP | RT_I2C_IGNORE_NACK |
                          RT_I2C_NO_READ_ACK)) == 0U &&
        msgs[0].addr == msgs[1].addr && msgs[0].len == 2U &&
        msgs[0].buf != RT_NULL && msgs[1].buf != RT_NULL)
    {
        uint16_t register_address = ((uint16_t)msgs[0].buf[0] << 8) |
                                     msgs[0].buf[1];
        HAL_StatusTypeDef status = HAL_I2C_Mem_Read(
            &hi2c2, (uint16_t)(msgs[0].addr << 1), register_address,
            I2C_MEMADD_SIZE_16BIT, msgs[1].buf, msgs[1].len,
            I2C2_TIMEOUT_MS);
        if (status != HAL_OK)
        {
            rt_tick_t now = rt_tick_get();

            /* A disconnected/noisy touch panel can leave the status read
             * failing every few milliseconds. Keep the first error and one
             * periodic sample, while the GT911 statistics retain the exact
             * error count without flooding the console. */
            if (s_mem_error_count == 0U ||
                (now - s_last_mem_error_log) >= RT_TICK_PER_SECOND)
            {
                rt_kprintf("[I2C2] mem-read addr=0x%02x reg=0x%04x len=%u "
                           "status=%d error=0x%08x ISR=0x%08x count=%lu\r\n",
                           msgs[0].addr, register_address,
                           (unsigned)msgs[1].len, (int)status,
                           (unsigned)HAL_I2C_GetError(&hi2c2),
                           (unsigned)hi2c2.Instance->ISR,
                           (unsigned long)s_mem_error_count + 1U);
                s_last_mem_error_log = now;
            }
            s_mem_error_count++;
        }
        return status == HAL_OK ? 2 : -RT_ERROR;
    }

    for (rt_uint32_t index = 0U; index < count; index++)
    {
        HAL_StatusTypeDef status;
        rt_uint16_t flags = msgs[index].flags;

        if ((flags & (RT_I2C_ADDR_10BIT | RT_I2C_NO_START |
                      RT_I2C_NO_STOP | RT_I2C_IGNORE_NACK |
                      RT_I2C_NO_READ_ACK)) != 0U)
        {
            return -RT_ENOSYS;
        }
        if (msgs[index].buf == RT_NULL || msgs[index].len == 0U)
        {
            return -RT_EINVAL;
        }

        if ((flags & RT_I2C_RD) != 0U)
        {
            status = HAL_I2C_Master_Receive(
                &hi2c2, (uint16_t)(msgs[index].addr << 1), msgs[index].buf,
                msgs[index].len, I2C2_TIMEOUT_MS);
        }
        else
        {
            status = HAL_I2C_Master_Transmit(
                &hi2c2, (uint16_t)(msgs[index].addr << 1), msgs[index].buf,
                msgs[index].len, I2C2_TIMEOUT_MS);
        }
        if (status != HAL_OK)
        {
            return -RT_ERROR;
        }
    }
    return (rt_ssize_t)count;
}

static const struct rt_i2c_bus_device_ops i2c2_ops = {
    .master_xfer = i2c2_master_xfer,
    .slave_xfer = RT_NULL,
    .i2c_bus_control = RT_NULL,
};

static struct rt_i2c_bus_device i2c2_bus;

static int i2c2_bus_init(void)
{
    i2c2_bus.ops = &i2c2_ops;
    i2c2_bus.config.mode = 0U;
    i2c2_bus.config.max_hz = 100000U;
    i2c2_bus.config.usage_freq = 100000U;
    return rt_i2c_bus_device_register(&i2c2_bus, I2C2_BUS_NAME);
}
INIT_DEVICE_EXPORT(i2c2_bus_init);
