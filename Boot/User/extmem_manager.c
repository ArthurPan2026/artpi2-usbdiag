#include "extmem_manager.h"

#include "aps256xx.h"
#include "main.h"
#include "usart.h"
#include "w35t51nwtbie.h"
#include "watchdog_flash_log.h"
#include "watchdog_h7r7_port.h"
#include "watchdog_record.h"
#include "watchdog_trace_flash.h"
#include "xspi.h"
#include "boot_vector_validation.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define BOOT_APPLICATION_ADDRESS 0x70000000U
#define BOOT_PSRAM_ADDRESS       0x90000000U
#define BOOT_PSRAM_TEST_VALUE    0x13579BDFU

extern XSPI_HandleTypeDef hxspi1;
extern XSPI_HandleTypeDef hxspi2;

static XSPI_HSCalTypeDef s_psram_calibration;

static void boot_log(const char *message)
{
    if (message != NULL)
    {
        (void)HAL_UART_Transmit(&huart4, (uint8_t *)message,
                                (uint16_t)strlen(message), 1000U);
    }
}

static int boot_configure_option_bytes(void)
{
    FLASH_OBProgramInitTypeDef ob = {0};

    boot_log("[BOOT] option bytes\r\n");
    if (HAL_FLASH_Unlock() != HAL_OK || HAL_FLASH_OB_Unlock() != HAL_OK)
    {
        return -1;
    }

    ob.OptionType = OPTIONBYTE_USER;
    ob.USERType = OB_USER_IWDG_SW | OB_USER_NRST_STOP |
                  OB_USER_NRST_STDBY | OB_USER_VDDIO_HSLV |
                  OB_USER_IWDG_STOP | OB_USER_IWDG_STDBY |
                  OB_USER_XSPI1_HSLV | OB_USER_XSPI2_HSLV |
                  OB_USER_I2C_NI3C;
    ob.USERConfig1 = OB_IWDG_SW | OB_STOP_NORST |
                     OB_STANDBY_NORST | OB_VDDIO_HSLV_DISABLE |
                     OB_IWDG_STOP_RUN | OB_IWDG_STDBY_RUN |
                     OB_XSPI1_HSLV_ENABLE | OB_XSPI2_HSLV_ENABLE;
    ob.USERConfig2 = OB_I2C_NI3C_I2C;

    if (HAL_FLASHEx_OBProgram(&ob) != HAL_OK)
    {
        (void)HAL_FLASH_OB_Lock();
        (void)HAL_FLASH_Lock();
        return -1;
    }

    (void)HAL_FLASH_OB_Lock();
    (void)HAL_FLASH_Lock();
    return 0;
}

static int boot_xspi1_init(void)
{
    XSPIM_CfgTypeDef manager = {0};
    uint32_t clock_hz;

    /* CubeMX initializes this handle before USER CODE 2.  Release that
       temporary configuration so the APS256XX settings below are applied. */
    if (hxspi1.State != HAL_XSPI_STATE_RESET &&
        HAL_XSPI_DeInit(&hxspi1) != HAL_OK)
    {
        return -1;
    }

    clock_hz = HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_XSPI1);

    hxspi1.Instance = XSPI1;
    hxspi1.Init.FifoThresholdByte = 2U;
    hxspi1.Init.MemoryMode = HAL_XSPI_SINGLE_MEM;
    hxspi1.Init.MemoryType = HAL_XSPI_MEMTYPE_APMEM_16BITS;
    /* APS256XX is a 256-Mbit (32-MByte) PSRAM.  The XSPI DEVSIZE field is
       encoded in bits, so HAL_XSPI_SIZE_256MB is the correct H7RS macro. */
    hxspi1.Init.MemorySize = HAL_XSPI_SIZE_256MB;
    hxspi1.Init.ChipSelectHighTimeCycle = 1U;
    hxspi1.Init.FreeRunningClock = HAL_XSPI_FREERUNCLK_DISABLE;
    hxspi1.Init.ClockMode = HAL_XSPI_CLOCK_MODE_0;
    hxspi1.Init.WrapSize = HAL_XSPI_WRAP_32_BYTES;
    hxspi1.Init.ClockPrescaler = 0U;
    hxspi1.Init.SampleShifting = HAL_XSPI_SAMPLE_SHIFT_NONE;
    /* DHQC is deprecated and not supported by STM32H7RS XSPI. */
    hxspi1.Init.DelayHoldQuarterCycle = HAL_XSPI_DHQC_DISABLE;
    hxspi1.Init.ChipSelectBoundary = HAL_XSPI_BONDARYOF_8KB;
    hxspi1.Init.MaxTran = 0U;
    hxspi1.Init.Refresh = ((2U * clock_hz) / 1000000U) - 4U;
    hxspi1.Init.MemorySelect = HAL_XSPI_CSSEL_NCS1;

    if (HAL_XSPI_Init(&hxspi1) != HAL_OK)
    {
        return -1;
    }

    manager.nCSOverride = HAL_XSPI_CSSEL_OVR_NCS1;
    manager.IOPort = HAL_XSPIM_IOPORT_1;
    manager.Req2AckTime = 1U;
    return HAL_XSPIM_Config(&hxspi1, &manager,
                            HAL_XSPI_TIMEOUT_DEFAULT_VALUE) == HAL_OK ? 0 : -1;
}

static int boot_xspi2_init(void)
{
    XSPIM_CfgTypeDef manager = {0};

    /* CubeMX initializes this handle before USER CODE 2.  Release that
       temporary configuration so the W35T51NWTBIE settings below are applied. */
    if (hxspi2.State != HAL_XSPI_STATE_RESET &&
        HAL_XSPI_DeInit(&hxspi2) != HAL_OK)
    {
        return -1;
    }

    hxspi2.Instance = XSPI2;
    hxspi2.Init.FifoThresholdByte = 4U;
    hxspi2.Init.MemoryMode = HAL_XSPI_SINGLE_MEM;
    hxspi2.Init.MemoryType = HAL_XSPI_MEMTYPE_MICRON;
    /* W35T51NWTBIE is a 512-Mbit (64-MByte) HyperFlash. */
    hxspi2.Init.MemorySize = HAL_XSPI_SIZE_512MB;
    hxspi2.Init.ChipSelectHighTimeCycle = 2U;
    hxspi2.Init.FreeRunningClock = HAL_XSPI_FREERUNCLK_DISABLE;
    hxspi2.Init.ClockMode = HAL_XSPI_CLOCK_MODE_0;
    hxspi2.Init.WrapSize = HAL_XSPI_WRAP_NOT_SUPPORTED;
    /* Diagnostic timing-margin trial for the 200 MHz DTR XIP path: 50 MHz. */
    hxspi2.Init.ClockPrescaler = 3U;
    hxspi2.Init.SampleShifting = HAL_XSPI_SAMPLE_SHIFT_NONE;
    /* DHQC is deprecated and not supported by STM32H7RS XSPI. */
    hxspi2.Init.DelayHoldQuarterCycle = HAL_XSPI_DHQC_DISABLE;
    hxspi2.Init.ChipSelectBoundary = HAL_XSPI_BONDARYOF_NONE;
    hxspi2.Init.MaxTran = 0U;
    hxspi2.Init.Refresh = 0U;
    hxspi2.Init.MemorySelect = HAL_XSPI_CSSEL_NCS1;

    if (HAL_XSPI_Init(&hxspi2) != HAL_OK)
    {
        return -1;
    }

    manager.nCSOverride = HAL_XSPI_CSSEL_OVR_NCS1;
    manager.IOPort = HAL_XSPIM_IOPORT_2;
    manager.Req2AckTime = 1U;
    return HAL_XSPIM_Config(&hxspi2, &manager,
                            HAL_XSPI_TIMEOUT_DEFAULT_VALUE) == HAL_OK ? 0 : -1;
}

static int boot_flash_command_init(void)
{
    uint8_t id[3] = {0};

    boot_log("[BOOT] flash SPI ID\r\n");
    if (W35T51NWTBIE_ReadID(&hxspi2,
                            W35T51NWTBIE_SPI_MODE,
                            W35T51NWTBIE_STR_TRANSFER,
                            id, W35T51NWTBIE_3BYTES_SIZE) != W35T51NWTBIE_OK)
    {
        return -1;
    }

    boot_log("[BOOT] flash enter OPI DTR\r\n");
    if (W35T51NWTBIE_EnterOctal_DTR_Mode(&hxspi2) != W35T51NWTBIE_OK)
    {
        return -1;
    }

    if (HAL_XSPI_DeInit(&hxspi2) != HAL_OK || boot_xspi2_init() != 0)
    {
        return -1;
    }

    boot_log("[BOOT] flash OPI reinit/read ID\r\n");
    if (W35T51NWTBIE_ReadID(&hxspi2,
                            W35T51NWTBIE_OPI_MODE,
                            W35T51NWTBIE_DTR_TRANSFER,
                            id, W35T51NWTBIE_4BYTES_SIZE) != W35T51NWTBIE_OK)
    {
        return -1;
    }

    return W35T51NWTBIE_OK;
}

static int boot_flash_memory_map_enable(void)
{
    boot_log("[BOOT] flash memory map\r\n");
    return W35T51NWTBIE_EnableMemoryMappedModeDTR(&hxspi2,
                                                   W35T51NWTBIE_OPI_MODE) ==
                   W35T51NWTBIE_OK
               ? 0
               : -1;
}

static int boot_psram_init(void)
{
    uint8_t reg[8] = {0};

    boot_log("[BOOT] PSRAM reset/config\r\n");
    if (APS256XX_Reset(&hxspi1) != APS256XX_OK)
    {
        return -1;
    }
    HAL_Delay(20U);

    if (APS256XX_ReadReg(&hxspi1, APS256XX_MR0_ADDRESS, reg, 5U) != APS256XX_OK)
    {
        return -1;
    }
    reg[0] = (uint8_t)((reg[0] & (uint8_t)~(APS256XX_MR0_LATENCY_TYPE |
                                             APS256XX_MR0_READ_LATENCY_CODE |
                                             APS256XX_MR0_DRIVE_STRENGTH)) |
                       APS256XX_MR0_LATENCY_TYPE_FIXED |
                       APS256XX_MR0_RLC_7 | APS256XX_MR0_DS_FULL);
    if (APS256XX_WriteReg(&hxspi1, APS256XX_MR0_ADDRESS, reg[0]) != APS256XX_OK)
    {
        return -1;
    }

    if (APS256XX_ReadReg(&hxspi1, APS256XX_MR4_ADDRESS, reg, 5U) != APS256XX_OK)
    {
        return -1;
    }
    reg[0] = (uint8_t)((reg[0] & (uint8_t)~(APS256XX_MR4_WRITE_LATENCY_CODE |
                                             APS256XX_MR4_RF_RATE |
                                             APS256XX_MR4_PASR)) |
                       APS256XX_MR4_WLC_7 | APS256XX_MR4_RF_4X |
                       APS256XX_MR4_PASR_FULL);
    if (APS256XX_WriteReg(&hxspi1, APS256XX_MR4_ADDRESS, reg[0]) != APS256XX_OK)
    {
        return -1;
    }

    if (APS256XX_ReadReg(&hxspi1, APS256XX_MR8_ADDRESS, reg, 5U) != APS256XX_OK)
    {
        return -1;
    }
    reg[0] = (uint8_t)((reg[0] & (uint8_t)~(APS256XX_MR8_X8_X16 |
                                             APS256XX_MR8_RBX |
                                             APS256XX_MR8_BT |
                                             APS256XX_MR8_BL)) |
                       APS256XX_MR8_X16 |
                       APS256XX_MR8_HYBRID_NONE_BURST_WRAP |
                       APS256XX_MR8_RBX_NOT_SUPPORT |
                       APS256XX_MR8_BL_16_BYTES);
    if (APS256XX_WriteReg(&hxspi1, APS256XX_MR8_ADDRESS, reg[0]) != APS256XX_OK)
    {
        return -1;
    }

    HAL_XSPI_GetDelayValue(&hxspi1, &s_psram_calibration);
    return APS256XX_EnableMemoryMappedMode(&hxspi1, 7U, 7U,
                                            HAL_XSPI_DATA_16_LINES, 0U) ==
                   APS256XX_OK
               ? 0
               : -1;
}

static int boot_psram_test(void)
{
    volatile uint32_t *memory = (volatile uint32_t *)BOOT_PSRAM_ADDRESS;

    memory[0] = BOOT_PSRAM_TEST_VALUE;
    __DSB();
    return memory[0] == BOOT_PSRAM_TEST_VALUE ? 0 : -1;
}

static int boot_extmem_fail(uint32_t stage, int32_t status,
                            const char *message)
{
    boot_log(message);
    boot_watchdog_record_error(stage, status);
    if (boot_watchdog_reset_streak() >= (int)WATCHDOG_RESET_STREAK_LIMIT)
    {
        boot_watchdog_recovery_loop();
    }
    return -1;
}

int boot_extmem_init(uint32_t reset_flags)
{
    int option_bytes_result;
    int result;
    int watchdog_journal_result;

    boot_log("[BOOT] external memory init\r\n");
    option_bytes_result = boot_configure_option_bytes();
    watchdog_h7r7_iwdg_refresh();
    if (option_bytes_result != 0)
    {
        return boot_extmem_fail(BOOT_WATCHDOG_STAGE_OPTION_BYTES,
                                option_bytes_result,
                                "[BOOT] FAIL option bytes\r\n");
    }
    boot_log("[BOOT] XSPI1 init\r\n");
    result = boot_xspi1_init();
    watchdog_h7r7_iwdg_refresh();
    if (result != 0)
    {
        return boot_extmem_fail(BOOT_WATCHDOG_STAGE_XSPI1, result,
                                "[BOOT] FAIL XSPI1 init\r\n");
    }
    boot_log("[BOOT] XSPI2 init\r\n");
    result = boot_xspi2_init();
    watchdog_h7r7_iwdg_refresh();
    if (result != 0)
    {
        return boot_extmem_fail(BOOT_WATCHDOG_STAGE_XSPI2, result,
                                "[BOOT] FAIL XSPI2 init\r\n");
    }
    result = boot_flash_command_init();
    watchdog_h7r7_iwdg_refresh();
    if (result != 0)
    {
        return boot_extmem_fail(BOOT_WATCHDOG_STAGE_FLASH_COMMAND, result,
                                "[BOOT] FAIL Flash command init\r\n");
    }
    if (boot_watchdog_trace_flash_prepare(reset_flags) != 0)
    {
        boot_log("[BOOT] watchdog trace eligibility unavailable\r\n");
    }
    watchdog_journal_result = boot_watchdog_flash_log_flush();
    if (watchdog_journal_result != 0)
    {
        boot_log("[BOOT] watchdog log remains staged\r\n");
    }
    watchdog_h7r7_iwdg_refresh();
    if (boot_watchdog_trace_flash_flush(reset_flags,
                                        watchdog_journal_result) != 0)
    {
        boot_log("[BOOT] watchdog trace sidecar flush failed\r\n");
    }
    watchdog_h7r7_iwdg_refresh();
    result = boot_flash_memory_map_enable();
    watchdog_h7r7_iwdg_refresh();
    if (result != 0)
    {
        return boot_extmem_fail(BOOT_WATCHDOG_STAGE_FLASH_MAP, result,
                                "[BOOT] FAIL Flash memory map\r\n");
    }
    if (boot_watchdog_reset_streak() >= (int)WATCHDOG_RESET_STREAK_LIMIT)
    {
        boot_watchdog_recovery_loop();
    }
    result = boot_psram_init();
    watchdog_h7r7_iwdg_refresh();
    if (result != 0)
    {
        return boot_extmem_fail(BOOT_WATCHDOG_STAGE_PSRAM_INIT, result,
                                "[BOOT] FAIL PSRAM\r\n");
    }
    result = boot_psram_test();
    watchdog_h7r7_iwdg_refresh();
    if (result != 0)
    {
        return boot_extmem_fail(BOOT_WATCHDOG_STAGE_PSRAM_TEST, result,
                                "[BOOT] FAIL PSRAM test\r\n");
    }
    boot_log("[BOOT] external memory ready\r\n");
    return 0;
}

void boot_jump_to_application(void)
{
    typedef void (*application_entry_t)(void);
    uint32_t application_msp;
    uint32_t application_reset;
    application_entry_t entry;

    __disable_irq();
    HAL_SuspendTick();
    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL = 0U;

    for (uint32_t irq = 0U; irq < 8U; irq++)
    {
        NVIC->ICER[irq] = 0xFFFFFFFFU;
        NVIC->ICPR[irq] = 0xFFFFFFFFU;
    }

    /* The official H7RS boot flow disables the caches before reading the
       external vector table.  This prevents a speculative/stale cache line
       from being used for the first application instruction fetch. */
    SCB_CleanDCache();
    SCB_DisableDCache();
    SCB_DisableICache();
    __DSB();
    __ISB();

    application_msp = *(volatile uint32_t *)BOOT_APPLICATION_ADDRESS;
    application_reset =
        *(volatile uint32_t *)(BOOT_APPLICATION_ADDRESS + 4U);
    entry = (application_entry_t)application_reset;

    if (!boot_application_msp_is_valid(application_msp) ||
        (application_reset & 1U) == 0U)
    {
        Error_Handler();
    }

    SCB->VTOR = BOOT_APPLICATION_ADDRESS;
    __DSB();
    __ISB();
    boot_log("[BOOT] jump Appli\r\n");
    __set_CONTROL(0U);
    __set_MSP(application_msp);
    __DSB();
    __ISB();
    entry();

    while (1)
    {
    }
}
