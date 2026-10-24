/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    stm32h7rsxx_it.c
  * @brief   Interrupt Service Routines.
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
#include "main.h"
#include "stm32h7rsxx_it.h"
#include "watchdog_flash_log.h"
/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN TD */

/* USER CODE END TD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/* External variables --------------------------------------------------------*/
extern UART_HandleTypeDef huart4;
extern TIM_HandleTypeDef htim7;

/* USER CODE BEGIN EV */

/* USER CODE END EV */

/******************************************************************************/
/*           Cortex Processor Interruption and Exception Handlers          */
/******************************************************************************/
/**
  * @brief This function handles Non maskable interrupt.
  */
__attribute__((naked)) void NMI_Handler(void)
{
  __asm volatile(
      "tst lr, #4\n"
      "ite eq\n"
      "mrseq r0, msp\n"
      "mrsne r0, psp\n"
      "mov r1, lr\n"
      "mrs r2, ipsr\n"
      "b boot_watchdog_exception_capture\n");
}

/**
  * @brief This function handles Memory management fault.
  */
__attribute__((naked)) void MemManage_Handler(void)
{
  __asm volatile(
      "tst lr, #4\n"
      "ite eq\n"
      "mrseq r0, msp\n"
      "mrsne r0, psp\n"
      "mov r1, lr\n"
      "mrs r2, ipsr\n"
      "b boot_watchdog_exception_capture\n");
}

/**
  * @brief This function handles Pre-fetch fault, memory access fault.
  */
__attribute__((naked)) void BusFault_Handler(void)
{
  __asm volatile(
      "tst lr, #4\n"
      "ite eq\n"
      "mrseq r0, msp\n"
      "mrsne r0, psp\n"
      "mov r1, lr\n"
      "mrs r2, ipsr\n"
      "b boot_watchdog_exception_capture\n");
}

/**
  * @brief This function handles Undefined instruction or illegal state.
  */
__attribute__((naked)) void UsageFault_Handler(void)
{
  __asm volatile(
      "tst lr, #4\n"
      "ite eq\n"
      "mrseq r0, msp\n"
      "mrsne r0, psp\n"
      "mov r1, lr\n"
      "mrs r2, ipsr\n"
      "b boot_watchdog_exception_capture\n");
}

__attribute__((naked)) void HardFault_Handler(void)
{
  __asm volatile(
      "tst lr, #4\n"
      "ite eq\n"
      "mrseq r0, msp\n"
      "mrsne r0, psp\n"
      "mov r1, lr\n"
      "mrs r2, ipsr\n"
      "b boot_watchdog_exception_capture\n");
}

/**
  * @brief This function handles System service call via SWI instruction.
  */
void SVC_Handler(void)
{
  /* USER CODE BEGIN SVCall_IRQn 0 */

  /* USER CODE END SVCall_IRQn 0 */
  /* USER CODE BEGIN SVCall_IRQn 1 */

  /* USER CODE END SVCall_IRQn 1 */
}

/**
  * @brief This function handles Debug monitor.
  */
void DebugMon_Handler(void)
{
  /* USER CODE BEGIN DebugMonitor_IRQn 0 */

  /* USER CODE END DebugMonitor_IRQn 0 */
  /* USER CODE BEGIN DebugMonitor_IRQn 1 */

  /* USER CODE END DebugMonitor_IRQn 1 */
}

/**
  * @brief This function handles System tick timer.
  */
void SysTick_Handler(void)
{
  /* USER CODE BEGIN SysTick_IRQn 0 */

  /* USER CODE END SysTick_IRQn 0 */

  /* USER CODE BEGIN SysTick_IRQn 1 */

  /* USER CODE END SysTick_IRQn 1 */
}

/******************************************************************************/
/* STM32H7RSxx Peripheral Interrupt Handlers                                    */
/* Add here the Interrupt Handlers for the used peripherals.                  */
/* For the available peripheral interrupt handler names,                      */
/* please refer to the startup file (startup_stm32h7rsxx.s).                    */
/******************************************************************************/

/**
  * @brief This function handles TIM7 global interrupt.
  */
void TIM7_IRQHandler(void)
{
  /* USER CODE BEGIN TIM7_IRQn 0 */

  /* USER CODE END TIM7_IRQn 0 */
  HAL_TIM_IRQHandler(&htim7);
  /* USER CODE BEGIN TIM7_IRQn 1 */

  /* USER CODE END TIM7_IRQn 1 */
}

/**
  * @brief This function handles UART4 global interrupt.
  */
void UART4_IRQHandler(void)
{
  /* USER CODE BEGIN UART4_IRQn 0 */

  /* USER CODE END UART4_IRQn 0 */
  HAL_UART_IRQHandler(&huart4);
  /* USER CODE BEGIN UART4_IRQn 1 */

  /* USER CODE END UART4_IRQn 1 */
}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */
