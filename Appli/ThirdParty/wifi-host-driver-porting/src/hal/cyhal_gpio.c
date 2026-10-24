/*
 * MIT License
 *
 * Copyright (c) 2024 Evlers
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Change Logs:
 * Date         Author      Notes
 * 2023-12-21   Evlers      first implementation
 * 2024-05-18   Evlers      add __builtin_clz to support the gcc compiler
 * 2024-05-22   Evlers      fix oob interrupt loss issue
 * 2024-05-28   Evlers      add assert to gpio external interrupt
 * 2024-07-09   Evlers      modify the gpio interrupt callback parameter to provide an absolute event type
 * 2024-08-14   Evlers      add __CLZ to support the iar compiler
 */

#include "cyhal_gpio.h"
#include <rtdevice.h>
#include "rthw.h"
#include "rtconfig.h"

#if defined(SOC_SERIES_STM32H7RS)
#include "stm32h7rsxx.h"
#endif

#define CYHAL_INT_TIMEOUT           10
#define CYHAL_IRQ_REARM_POLL_MS     5
#define CYHAL_MAX_EXTI_NUMBER       (16U)

/* Return pin number by counts the number of leading zeros of a data value.
 * NOTE: __CLZ  returns  32 if no bits are set in the source register, and zero
 * if bit 31 is set. Parameter 'pin' should be in range GPIO_PIN0..GPIO_PIN15. */
#if defined(__ARMCC_VERSION) && (__ARMCC_VERSION < 6100100)
#define CYHAL_GET_PIN_NUMBER(pin)   (uint32_t)(31u - __clz(pin))
#elif defined(__GNUC__) || (__ARMCC_VERSION >= 6100100)
#define CYHAL_GET_PIN_NUMBER(pin)   (uint32_t)(31u - __builtin_clz(pin))
#elif defined(__ICCARM__)
#include <intrinsics.h>
#define CYHAL_GET_PIN_NUMBER(pin)   (uint32_t)(31u - __CLZ(pin))
#endif

typedef struct
{
    cyhal_gpio_irq_handler_t    callback;
    void*                       callback_args;
    bool                        enable;
    cyhal_gpio_t                pin;

    /* fix oob interrupt loss issue */
    cyhal_gpio_irq_event_t      event;
    rt_tick_t                   tick;
    rt_bool_t                   last_level;
    rt_uint32_t                 soft_int_count;
    rt_uint32_t                 irq_count;
    rt_bool_t                   irq_latched;
} cyhal_gpio_event_callback_info_t;

static cyhal_gpio_event_callback_info_t _exti_callbacks_info[CYHAL_MAX_EXTI_NUMBER] = { 0u };

/*
 * HOST_WAKE is an active-low, level-like signal on the ART-Pi2 WiFi module.
 * The WHD callback only wakes a thread; it must not be allowed to run once
 * per edge while the module is holding the line low.  Mask just the EXTI
 * line, rather than de-initialising the GPIO from an ISR.  The latter briefly
 * re-enables PRIMASK inside the RT-Thread pin driver and can make a storm
 * re-enter before the callback has returned.
 */
#if !(defined(SOC_SERIES_STM32H7RS) && defined(CYHAL_OOB_USE_POLLING))
static void cyhal_gpio_mask_irq_line(cyhal_gpio_t pin)
{
#if defined(SOC_SERIES_STM32H7RS)
    uint32_t pin_number = CYHAL_GET_PIN_NUMBER(pin);
    uint32_t line_mask;
    IRQn_Type irq;

    if (pin_number >= CYHAL_MAX_EXTI_NUMBER)
    {
        return;
    }
    line_mask = 1UL << pin_number;
    irq = (IRQn_Type)(EXTI0_IRQn + (int32_t)pin_number);
    EXTI->PR1 = line_mask;
    EXTI->IMR1 &= ~line_mask;
    NVIC_ClearPendingIRQ(irq);
    NVIC_DisableIRQ(irq);
#else
    (void)rt_pin_irq_enable(pin, RT_FALSE);
#endif
}
#endif

static void cyhal_gpio_rearm_irq_line(cyhal_gpio_t pin)
{
#if defined(SOC_SERIES_STM32H7RS)
    uint32_t pin_number = CYHAL_GET_PIN_NUMBER(pin);
    uint32_t line_mask;
    IRQn_Type irq;

    if (pin_number >= CYHAL_MAX_EXTI_NUMBER)
    {
        return;
    }
    line_mask = 1UL << pin_number;
    irq = (IRQn_Type)(EXTI0_IRQn + (int32_t)pin_number);
    EXTI->PR1 = line_mask;
    NVIC_ClearPendingIRQ(irq);
    EXTI->IMR1 |= line_mask;
    NVIC_EnableIRQ(irq);
#else
    (void)rt_pin_irq_enable(pin, RT_TRUE);
#endif
}

static void cyhal_gpio_dispatch_callback(cyhal_gpio_event_callback_info_t *info)
{
    if (info != NULL && info->enable && info->callback != NULL)
    {
        info->callback(info->callback_args, info->event);
        info->tick = rt_tick_get();
    }
}


/** Initialize the GPIO pin
 *
 * @param[in]  pin The GPIO pin to initialize
 * @param[in]  direction The pin direction (input/output)
 * @param[in]  drvMode The pin drive mode
 * @param[in]  initVal Initial value on the pin
 *
 * @return The status of the init request
 */
cy_rslt_t cyhal_gpio_init(cyhal_gpio_t pin, cyhal_gpio_direction_t direction, cyhal_gpio_drive_mode_t drvMode,
                          bool initVal)
{
    rt_uint8_t mode = PIN_MODE_INPUT;

    switch (drvMode)
    {
    case CYHAL_GPIO_DRIVE_NONE:
            mode = PIN_MODE_INPUT;
        break;

    case CYHAL_GPIO_DRIVE_PULLUP:
            mode = (direction == CYHAL_GPIO_DIR_INPUT) ? PIN_MODE_INPUT_PULLUP : PIN_MODE_OUTPUT;
        break;

    case CYHAL_GPIO_DRIVE_PULLDOWN:
            mode = (direction == CYHAL_GPIO_DIR_INPUT) ? PIN_MODE_INPUT_PULLDOWN : PIN_MODE_OUTPUT;
        break;

    case CYHAL_GPIO_DRIVE_OPENDRAINDRIVESLOW:
            mode = (direction == CYHAL_GPIO_DIR_INPUT) ? PIN_MODE_INPUT_PULLDOWN : PIN_MODE_OUTPUT_OD;
        break;

    case CYHAL_GPIO_DRIVE_OPENDRAINDRIVESHIGH:
            mode = (direction == CYHAL_GPIO_DIR_INPUT) ? PIN_MODE_INPUT_PULLUP : PIN_MODE_OUTPUT_OD;
        break;

    case CYHAL_GPIO_DRIVE_STRONG:
            mode = PIN_MODE_OUTPUT;
        break;

    case CYHAL_GPIO_DRIVE_PULLUPDOWN:
            if ((direction == CYHAL_GPIO_DIR_OUTPUT) || (direction == CYHAL_GPIO_DIR_BIDIRECTIONAL))
            {
                mode = PIN_MODE_OUTPUT;
            }
            else
            {
                mode = PIN_MODE_INPUT_PULLUP;
            }
        break;
    }

    rt_pin_mode(pin, mode);
    rt_pin_write(pin, initVal);

    return CY_RSLT_SUCCESS;
}

/** Uninitialize the gpio peripheral and the cyhal_gpio_t object
 *
 * @param[in] pin Pin number
 */
void cyhal_gpio_free(cyhal_gpio_t pin)
{
    uint32_t pin_number = CYHAL_GET_PIN_NUMBER(pin);

    if (pin_number < CYHAL_MAX_EXTI_NUMBER)
    {
        _exti_callbacks_info[pin_number].enable = false;
        _exti_callbacks_info[pin_number].irq_latched = RT_FALSE;
#if !(defined(SOC_SERIES_STM32H7RS) && defined(CYHAL_OOB_USE_POLLING))
        cyhal_gpio_mask_irq_line(pin);
#endif
    }
#if !(defined(SOC_SERIES_STM32H7RS) && defined(CYHAL_OOB_USE_POLLING))
    rt_pin_detach_irq(pin);
#endif
    rt_pin_mode(pin, PIN_MODE_INPUT);
}

/** Set the output value for the pin. This only works for output & in_out pins.
 *
 * @param[in] pin   The GPIO object
 * @param[in] value The value to be set (high = true, low = false)
 */
void cyhal_gpio_write(cyhal_gpio_t pin, bool value)
{
    rt_pin_write(pin, value);
}

/** Read the input value.  This only works for input & in_out pins.
 *
 * @param[in]  pin   The GPIO object
 * @return The value of the IO (true = high, false = low)
 */
bool cyhal_gpio_read(cyhal_gpio_t pin)
{
    return rt_pin_read(pin) ? true : false;
}


static void gpio_interrupt(void *args)
{
    cyhal_gpio_event_callback_info_t *info = (cyhal_gpio_event_callback_info_t *)args;
    uint32_t pin_number = CYHAL_GET_PIN_NUMBER(info->pin);

    /* Check the parameters */
    RT_ASSERT(pin_number < CYHAL_MAX_EXTI_NUMBER);

    if ((pin_number < CYHAL_MAX_EXTI_NUMBER) && (info->enable))
    {
        info->irq_count++;
        if (!info->irq_latched)
        {
            /* Stop a level-like HOST_WAKE signal from re-entering EXTI. */
            info->irq_latched = RT_TRUE;
#if !(defined(SOC_SERIES_STM32H7RS) && defined(CYHAL_OOB_USE_POLLING))
            cyhal_gpio_mask_irq_line(info->pin);
#endif
        }
        cyhal_gpio_dispatch_callback(info);
    }
}

/** Register/clear an interrupt handler for the pin toggle pin IRQ event
 *
 * @param[in] pin           The pin number
 * @param[in] intrPriority  The NVIC interrupt channel priority
 * @param[in] handler       The function to call when the specified event happens. Pass NULL to unregister the handler.
 * @param[in] handler_arg   Generic argument that will be provided to the handler when called, can be NULL
 */
void cyhal_gpio_register_irq(cyhal_gpio_t pin, uint8_t intrPriority, cyhal_gpio_irq_handler_t handler,
                             void *handler_arg)
{
    /* Get pin number */
    uint32_t pin_number = CYHAL_GET_PIN_NUMBER(pin);

    /* Check the parameters */
    RT_ASSERT(pin_number < CYHAL_MAX_EXTI_NUMBER);

    if (pin_number < CYHAL_MAX_EXTI_NUMBER && handler != NULL)
    {
        rt_base_t level = rt_hw_interrupt_disable();
        _exti_callbacks_info[pin_number].callback      = handler;
        _exti_callbacks_info[pin_number].callback_args = handler_arg;
        _exti_callbacks_info[pin_number].pin           = pin;
        rt_hw_interrupt_enable(level);
    }
    else if (pin_number >= CYHAL_MAX_EXTI_NUMBER)
    {
        /* Wrong param */
        RT_ASSERT(false);
    }
    else
    {
        /* Clean callback information */
        uint32_t level = rt_hw_interrupt_disable();
        _exti_callbacks_info[pin_number].callback      = NULL;
        _exti_callbacks_info[pin_number].callback_args = NULL;
        _exti_callbacks_info[pin_number].enable        = false;
        _exti_callbacks_info[pin_number].pin           = 0xFFFFFFFF;
        rt_hw_interrupt_enable(level);
    }
}

/* Notes:
 * Normally, we need to pull the pin up and then give the falling edge to trigger the interrupt, 
 * but the WiFi module sometimes will continue to be low, without raising the pin first, 
 * or sometimes only raised for less than 1us time, resulting in failure to trigger external interruptions.
 * In order to solve the problem that WiFi does not give a falling edge, 
 * we need to regularly check whether the edge times out.
 */
static void timer_callback (void *parameter)
{
    rt_tick_t tick = rt_tick_get();

    for (rt_uint8_t i = 0; i < CYHAL_MAX_EXTI_NUMBER; i ++)
    {
        cyhal_gpio_event_callback_info_t *info = &_exti_callbacks_info[i];

        if (info->enable)
        {
            bool level = rt_pin_read(info->pin) ? true : false;
            bool continuous_valid = false;

#if defined(SOC_SERIES_STM32H7RS) && defined(CYHAL_OOB_USE_POLLING)
            /* The CYW43438 HOST_WAKE signal is active-low and may stay low
             * while the WHD thread drains several SDIO responses.  Do not
             * attach PC5 to EXTI on H7RS: a level that remains asserted can
             * otherwise retrigger the interrupt continuously.  Polling keeps
             * the first edge latency below 1 ms and coalesces a held-low
             * interval into one notification every CYHAL_INT_TIMEOUT ms. */
            bool active = false;
            bool edge = false;

            switch (info->event)
            {
            case CYHAL_GPIO_IRQ_RISE:
                active = level;
                edge = (!info->last_level && level);
                break;
            case CYHAL_GPIO_IRQ_FALL:
                active = !level;
                edge = (info->last_level && !level);
                break;
            case CYHAL_GPIO_IRQ_BOTH:
                edge = (info->last_level != level);
                active = level;
                break;
            default:
                break;
            }

            if (edge)
            {
                gpio_interrupt(info);
            }
            else if (active &&
                     (tick - info->tick) >=
                         rt_tick_from_millisecond(CYHAL_INT_TIMEOUT))
            {
                info->soft_int_count++;
                cyhal_gpio_dispatch_callback(info);
            }
            else if (!active)
            {
                info->irq_latched = RT_FALSE;
                info->tick = tick;
            }
            info->last_level = level;
            continue;
#endif

            if (info->irq_latched)
            {
                if (level)
                {
                    /* The module released HOST_WAKE.  Re-arm only after
                     * observing the inactive level, so the next falling
                     * edge is a fresh notification. */
                    info->irq_latched = RT_FALSE;
                    info->last_level = RT_TRUE;
                    info->tick = tick;
                    cyhal_gpio_rearm_irq_line(info->pin);
                }
                else if ((tick - info->tick) >=
                         rt_tick_from_millisecond(CYHAL_INT_TIMEOUT))
                {
                    /* Preserve the missed-edge workaround, but keep it in
                     * thread/timer context while the EXTI line is masked. */
                    info->soft_int_count++;
                    cyhal_gpio_dispatch_callback(info);
                }
                continue;
            }

            /* Check whether pin are valid continuously */
            switch(info->event)
            {
            case CYHAL_GPIO_IRQ_RISE:
                    if (info->last_level && level)
                    {
                        continuous_valid = true;
                    }
                break;

            case CYHAL_GPIO_IRQ_FALL:
                    if (!info->last_level && !level)
                    {
                        continuous_valid = true;
                    }
                break;

            case CYHAL_GPIO_IRQ_BOTH:
                    /* This double edge mode should not have this problem */
                break;

            default:
                break;
            }

            /* Save the last level to see if it times out */
            info->last_level = level;

            if (continuous_valid)
            {
                rt_tick_t elapsed_time = tick - info->tick;

                /* If the pin does not trigger an interrupt continuous active, the interrupt is missed */
                if (elapsed_time >= rt_tick_from_millisecond(CYHAL_INT_TIMEOUT))
                {
                    /* Here the software triggers an interrupt */
                    info->soft_int_count ++;
                    gpio_interrupt(info);
                }
            }
        }
    }
}

/** Enable or Disable the GPIO IRQ
 *
 * @param[in] pin    The GPIO object
 * @param[in] event  The GPIO IRQ event
 * @param[in] enable True to turn on interrupts, False to turn off
 */
void cyhal_gpio_irq_enable(cyhal_gpio_t pin, cyhal_gpio_irq_event_t event, bool enable)
{
    /* Get pin number */
    uint32_t pin_number = CYHAL_GET_PIN_NUMBER(pin);

    /* Check the parameters */
    RT_ASSERT(pin_number < CYHAL_MAX_EXTI_NUMBER);

    if (enable)
    {
        rt_uint8_t mode = PIN_IRQ_MODE_RISING;
        switch(event)
        {
        case CYHAL_GPIO_IRQ_RISE:
                mode = PIN_IRQ_MODE_RISING;
            break;

        case CYHAL_GPIO_IRQ_FALL:
                mode = PIN_IRQ_MODE_FALLING;
            break;

        case CYHAL_GPIO_IRQ_BOTH:
                mode = PIN_IRQ_MODE_RISING_FALLING;
            break;

        default:
            break;
        }

        /* Publish the event and initial level before unmasking EXTI.  This
         * closes the small race in which HOST_WAKE falls between
         * rt_pin_attach_irq() and rt_pin_irq_enable(). */
        _exti_callbacks_info[pin_number].event = event;
        _exti_callbacks_info[pin_number].last_level = rt_pin_read(pin);
        _exti_callbacks_info[pin_number].irq_latched = RT_FALSE;
        _exti_callbacks_info[pin_number].tick = rt_tick_get();
        _exti_callbacks_info[pin_number].enable = true;
#if !(defined(SOC_SERIES_STM32H7RS) && defined(CYHAL_OOB_USE_POLLING))
        if (rt_pin_attach_irq(pin, mode, gpio_interrupt,
                              &_exti_callbacks_info[pin_number]) != RT_EOK ||
            rt_pin_irq_enable(pin, RT_TRUE) != RT_EOK)
        {
            _exti_callbacks_info[pin_number].enable = false;
            RT_ASSERT(false);
        }
#else
        RT_UNUSED(mode);
#endif

        static rt_timer_t timer = NULL;
        if (timer == NULL)
        {
            timer = rt_timer_create("whd_oob", timer_callback, NULL,
                                        rt_tick_from_millisecond(CYHAL_IRQ_REARM_POLL_MS),
                                        RT_TIMER_FLAG_PERIODIC | RT_TIMER_FLAG_SOFT_TIMER);
            rt_timer_start(timer);
        }
    }
    else
    {
        _exti_callbacks_info[pin_number].enable = false;
        _exti_callbacks_info[pin_number].irq_latched = RT_FALSE;
#if !(defined(SOC_SERIES_STM32H7RS) && defined(CYHAL_OOB_USE_POLLING))
        cyhal_gpio_mask_irq_line(pin);
        rt_pin_irq_enable(pin, RT_FALSE);
        rt_pin_detach_irq(pin);
#endif
    }
}
