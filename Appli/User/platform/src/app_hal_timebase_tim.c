#include "stm32h7rsxx_hal.h"

#define TIM_CNT_FREQ 1000000U
#define TIM_FREQ     1000U

TIM_HandleTypeDef htim7;

HAL_StatusTypeDef HAL_InitTick(uint32_t tick_priority)
{
    RCC_ClkInitTypeDef clock_config;
    uint32_t timer_clock;
    uint32_t apb1_prescaler;
    uint32_t prescaler;
    uint32_t flash_latency;
    HAL_StatusTypeDef status;

    __HAL_RCC_TIM7_CLK_ENABLE();
    HAL_RCC_GetClockConfig(&clock_config, &flash_latency);
    apb1_prescaler = clock_config.APB1CLKDivider;

    if (apb1_prescaler == RCC_APB1_DIV1)
    {
        timer_clock = HAL_RCC_GetPCLK1Freq();
    }
    else if (apb1_prescaler == RCC_APB1_DIV2)
    {
        timer_clock = 2UL * HAL_RCC_GetPCLK1Freq();
    }
    else if (__HAL_RCC_GET_TIMCLKPRESCALER() == RCC_TIMPRES_DISABLE)
    {
        timer_clock = 2UL * HAL_RCC_GetPCLK1Freq();
    }
    else
    {
        timer_clock = 4UL * HAL_RCC_GetPCLK1Freq();
    }

    prescaler = (timer_clock / TIM_CNT_FREQ) - 1U;
    htim7.Instance = TIM7;
    htim7.Init.Period = ((uint32_t)uwTickFreq * (TIM_CNT_FREQ / TIM_FREQ)) - 1U;
    htim7.Init.Prescaler = prescaler;
    htim7.Init.ClockDivision = 0U;
    htim7.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim7.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;

    status = HAL_TIM_Base_Init(&htim7);
    if (status != HAL_OK)
    {
        return status;
    }

    status = HAL_TIM_Base_Start_IT(&htim7);
    if (status != HAL_OK)
    {
        return status;
    }

    if (tick_priority >= (1UL << __NVIC_PRIO_BITS))
    {
        return HAL_ERROR;
    }
    HAL_NVIC_SetPriority(TIM7_IRQn, tick_priority, 0U);
    HAL_NVIC_EnableIRQ(TIM7_IRQn);
    uwTickPrio = tick_priority;
    return HAL_OK;
}

void HAL_SuspendTick(void)
{
    __HAL_TIM_DISABLE_IT(&htim7, TIM_IT_UPDATE);
}

void HAL_ResumeTick(void)
{
    __HAL_TIM_ENABLE_IT(&htim7, TIM_IT_UPDATE);
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *timer)
{
    if (timer != NULL && timer->Instance == TIM7)
    {
        HAL_IncTick();
    }
}

void TIM7_IRQHandler(void)
{
    HAL_TIM_IRQHandler(&htim7);
}
