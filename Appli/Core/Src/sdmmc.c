/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    sdmmc.c
  * @brief   This file provides code for the configuration
  *          of the SDMMC instances.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "sdmmc.h"

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

SDIO_HandleTypeDef hsdio2;

/* SDMMC2 init function */

void MX_SDMMC2_SDIO_Init(void)
{

  /* USER CODE BEGIN SDMMC2_Init 0 */

  /* USER CODE END SDMMC2_Init 0 */

  /* USER CODE BEGIN SDMMC2_Init 1 */

  /* USER CODE END SDMMC2_Init 1 */
  hsdio2.Instance = SDMMC2;
  hsdio2.Init.ClockEdge = SDMMC_CLOCK_EDGE_RISING;
  hsdio2.Init.ClockPowerSave = SDMMC_CLOCK_POWER_SAVE_DISABLE;
  hsdio2.Init.BusWide = SDMMC_BUS_WIDE_4B;
  hsdio2.Init.HardwareFlowControl = SDMMC_HARDWARE_FLOW_CONTROL_DISABLE;
  hsdio2.Init.ClockDiv = 2;
  if (HAL_SDIO_Init(&hsdio2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SDMMC2_Init 2 */

  /* USER CODE END SDMMC2_Init 2 */

}

void HAL_SDIO_MspInit(SDIO_HandleTypeDef* sdioHandle)
{

  GPIO_InitTypeDef GPIO_InitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};
  if(sdioHandle->Instance==SDMMC2)
  {
  /* USER CODE BEGIN SDMMC2_MspInit 0 */

  /* USER CODE END SDMMC2_MspInit 0 */

  /** Initializes the peripherals clock
  */
    PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_SDMMC12;
    PeriphClkInit.Sdmmc12ClockSelection = RCC_SDMMC12CLKSOURCE_PLL2S;
    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
    {
      Error_Handler();
    }

    /* SDMMC2 clock enable */
    __HAL_RCC_SDMMC2_CLK_ENABLE();

    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    /**SDMMC2 GPIO Configuration
    PD7     ------> SDMMC2_CMD
    PG12     ------> SDMMC2_D3
    PG11     ------> SDMMC2_D2
    PC1     ------> SDMMC2_CK
    PB14     ------> SDMMC2_D0
    PB15     ------> SDMMC2_D1
    */
    GPIO_InitStruct.Pin = GPIO_PIN_7;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF11_SDMMC2;
    HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_12|GPIO_PIN_11;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF10_SDMMC2;
    HAL_GPIO_Init(GPIOG, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_1;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF9_SDMMC2;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_14|GPIO_PIN_15;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF9_SDMMC2;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    /* SDMMC2 interrupt Init */
    HAL_NVIC_SetPriority(SDMMC2_IRQn, 2, 0);
    HAL_NVIC_EnableIRQ(SDMMC2_IRQn);
  /* USER CODE BEGIN SDMMC2_MspInit 1 */

  /* USER CODE END SDMMC2_MspInit 1 */
  }
}

void HAL_SDIO_MspDeInit(SDIO_HandleTypeDef* sdioHandle)
{

  if(sdioHandle->Instance==SDMMC2)
  {
  /* USER CODE BEGIN SDMMC2_MspDeInit 0 */

  /* USER CODE END SDMMC2_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_SDMMC2_CLK_DISABLE();

    /**SDMMC2 GPIO Configuration
    PD7     ------> SDMMC2_CMD
    PG12     ------> SDMMC2_D3
    PG11     ------> SDMMC2_D2
    PC1     ------> SDMMC2_CK
    PB14     ------> SDMMC2_D0
    PB15     ------> SDMMC2_D1
    */
    HAL_GPIO_DeInit(GPIOD, GPIO_PIN_7);

    HAL_GPIO_DeInit(GPIOG, GPIO_PIN_12|GPIO_PIN_11);

    HAL_GPIO_DeInit(GPIOC, GPIO_PIN_1);

    HAL_GPIO_DeInit(GPIOB, GPIO_PIN_14|GPIO_PIN_15);

    /* SDMMC2 interrupt Deinit */
    HAL_NVIC_DisableIRQ(SDMMC2_IRQn);
  /* USER CODE BEGIN SDMMC2_MspDeInit 1 */

  /* USER CODE END SDMMC2_MspDeInit 1 */
  }
}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

