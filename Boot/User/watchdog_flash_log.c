#include "watchdog_flash_log.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "stm32h7rsxx_hal.h"
#include "usart.h"
#include "xspi.h"
#include "w35t51nwtbie.h"
#include "watchdog_record.h"
#include "watchdog_incident_id.h"
#include "watchdog_flash_policy.h"
#include "watchdog_h7r7_port.h"
#include "watchdog_trace_flash.h"

#define WATCHDOG_RTC_ASYNC_PREDIV     127U
#define WATCHDOG_RTC_SYNC_PREDIV      249U
#define WATCHDOG_RTC_INIT_SPIN_LIMIT  2000000U
#define WATCHDOG_BKPSRAM_READY_SPIN_LIMIT 2000000U

#define WATCHDOG_XSPI_OPI             W35T51NWTBIE_OPI_MODE
#define WATCHDOG_XSPI_DTR             W35T51NWTBIE_DTR_TRANSFER
#define WATCHDOG_XSPI_ADDR_SIZE       W35T51NWTBIE_4BYTES_SIZE
#define WATCHDOG_FLASH_FAILURE_XSPI_DETAIL 0x58535049UL /* XSPi */

extern XSPI_HandleTypeDef hxspi2;

/* The driver contains this DTR page-program entry point, but its public
 * prototype is commented out. Keep the declaration project-owned here. */
extern int32_t W35T51NWTBIE_PageProgramDTR(XSPI_HandleTypeDef *Ctx,
                                           uint8_t *pData,
                                           uint32_t WriteAddr,
                                           uint32_t Size);
extern int32_t W35T51NWTBIE_ReadStatusRegister(
    XSPI_HandleTypeDef *Ctx, W35T51NWTBIE_Interface_t Mode,
    W35T51NWTBIE_Transfer_t Rate, uint8_t *Value);

__attribute__((section(".watchdog_bkpsram"), aligned(32), used))
volatile watchdog_retained_record_t g_watchdog_retained_record;

__attribute__((aligned(32)))
static watchdog_retained_record_t s_flash_record;
static watchdog_flash_policy_state_t s_slot_state[WATCHDOG_FLASH_SECTOR_COUNT];
static uint32_t s_slot_sequence[WATCHDOG_FLASH_SECTOR_COUNT];
static uint64_t s_slot_incident_id[WATCHDOG_FLASH_SECTOR_COUNT];
static uint32_t s_latest_sequence;
static uint32_t s_latest_slot;
static uint32_t s_valid_slot_count;
static watchdog_flash_policy_counts_t s_slot_scan_counts;
static uint64_t s_latest_incident_id;
static uint32_t s_recovery_streak;
static uint32_t s_record_ready;
static uint32_t s_backup_memory_ready;
static uint32_t s_boot_error_recorded;
static uint32_t s_flash_error_stage;
static uint32_t s_flash_wren_state;
static uint32_t s_flash_wren_hal_error;
static uint32_t s_flash_wren_xspi_sr;
static uint32_t s_flash_wren_xspi_cr;

#define WATCHDOG_BOOT_DTCM_BEGIN       0x20000000UL
#define WATCHDOG_BOOT_DTCM_END         0x20010000UL
#define WATCHDOG_BOOT_AXI_BEGIN        0x24000000UL
#define WATCHDOG_BOOT_AXI_END          0x24050000UL
#define WATCHDOG_BOOT_STACK_ERROR_MASK ((1UL << 3) | (1UL << 4) | (1UL << 5) | \
                                        (1UL << 11) | (1UL << 12) | (1UL << 13))

static int watchdog_erase_slot(uint32_t slot);
static void watchdog_boot_barrier(void);
static void watchdog_boot_mark_abnormal(void);

static int boot_tamper_erase_triggered(uint32_t reset_flags)
{
    static const struct
    {
        uint32_t enable;
        uint32_t status;
        uint32_t potential_mode;
    } external_sources[] =
    {
        {TAMP_CR1_TAMP1E, TAMP_SR_TAMP1F, TAMP_CR2_TAMP1POM},
        {TAMP_CR1_TAMP2E, TAMP_SR_TAMP2F, TAMP_CR2_TAMP2POM},
        {TAMP_CR1_TAMP3E, TAMP_SR_TAMP3F, TAMP_CR2_TAMP3POM},
        {TAMP_CR1_TAMP4E, TAMP_SR_TAMP4F, TAMP_CR2_TAMP4POM},
        {TAMP_CR1_TAMP5E, TAMP_SR_TAMP5F, TAMP_CR2_TAMP5POM},
        {TAMP_CR1_TAMP6E, TAMP_SR_TAMP6F, TAMP_CR2_TAMP6POM},
        {TAMP_CR1_TAMP7E, TAMP_SR_TAMP7F, TAMP_CR2_TAMP7POM},
        {TAMP_CR1_TAMP8E, TAMP_SR_TAMP8F, TAMP_CR2_TAMP8POM}
    };
    static const struct
    {
        uint32_t enable;
        uint32_t status;
        uint32_t potential_mode;
    } internal_sources[] =
    {
        {TAMP_CR1_ITAMP1E,  TAMP_SR_ITAMP1F,  TAMP_CR3_ITAMP1POM},
        {TAMP_CR1_ITAMP2E,  TAMP_SR_ITAMP2F,  TAMP_CR3_ITAMP2POM},
        {TAMP_CR1_ITAMP3E,  TAMP_SR_ITAMP3F,  TAMP_CR3_ITAMP3POM},
        {TAMP_CR1_ITAMP4E,  TAMP_SR_ITAMP4F,  TAMP_CR3_ITAMP4POM},
        {TAMP_CR1_ITAMP5E,  TAMP_SR_ITAMP5F,  TAMP_CR3_ITAMP5POM},
        {TAMP_CR1_ITAMP6E,  TAMP_SR_ITAMP6F,  TAMP_CR3_ITAMP6POM},
        {TAMP_CR1_ITAMP7E,  TAMP_SR_ITAMP7F,  TAMP_CR3_ITAMP7POM},
        {TAMP_CR1_ITAMP8E,  TAMP_SR_ITAMP8F,  TAMP_CR3_ITAMP8POM},
        {TAMP_CR1_ITAMP9E,  TAMP_SR_ITAMP9F,  TAMP_CR3_ITAMP9POM},
        {TAMP_CR1_ITAMP11E, TAMP_SR_ITAMP11F, TAMP_CR3_ITAMP11POM},
        {TAMP_CR1_ITAMP15E, TAMP_SR_ITAMP15F, TAMP_CR3_ITAMP15POM}
#if defined(TAMP_CR1_ITAMP12E) && defined(TAMP_SR_ITAMP12F) && \
    defined(TAMP_CR3_ITAMP12POM)
        ,{TAMP_CR1_ITAMP12E, TAMP_SR_ITAMP12F, TAMP_CR3_ITAMP12POM}
#endif
#if defined(TAMP_CR1_ITAMP13E) && defined(TAMP_SR_ITAMP13F) && \
    defined(TAMP_CR3_ITAMP13POM)
        ,{TAMP_CR1_ITAMP13E, TAMP_SR_ITAMP13F, TAMP_CR3_ITAMP13POM}
#endif
    };
    uint32_t cr1 = TAMP->CR1;
    uint32_t cr2 = TAMP->CR2;
    uint32_t cr3 = TAMP->CR3;
    uint32_t status = TAMP->SR;
    int iwdg_reset = (reset_flags & RCC_RSR_IWDGRSTF) != 0U;
    size_t i;

    for (i = 0U; i < (sizeof(external_sources) /
                       sizeof(external_sources[0])); ++i)
    {
        const uint32_t enable = external_sources[i].enable;
        const uint32_t flag = external_sources[i].status;
        const uint32_t pom = external_sources[i].potential_mode;

        if (watchdog_h7r7_tamper_source_requests_erase(
                (cr1 & enable) != 0U, (status & flag) != 0U,
                (cr2 & pom) != 0U, iwdg_reset))
        {
            return 1;
        }
    }

    for (i = 0U; i < (sizeof(internal_sources) /
                       sizeof(internal_sources[0])); ++i)
    {
        const uint32_t enable = internal_sources[i].enable;
        const uint32_t flag = internal_sources[i].status;
        const uint32_t pom = internal_sources[i].potential_mode;

        if (watchdog_h7r7_tamper_source_requests_erase(
                (cr1 & enable) != 0U, (status & flag) != 0U,
                (cr3 & pom) != 0U, iwdg_reset))
        {
            return 1;
        }
    }

    return 0;
}

void boot_watchdog_exception_capture(const uint32_t *frame,
                                     uint32_t exc_return,
                                     uint32_t ipsr)
{
    watchdog_fault_kind_t kind = WATCHDOG_FAULT_UNEXPECTED;
    const uint32_t *basic_frame = NULL;
    uintptr_t frame_address = (uintptr_t)frame;
    uintptr_t frame_size = 0U;
    uintptr_t frame_end;
    uintptr_t ram_end = 0U;

    switch (ipsr)
    {
    case 2U: kind = WATCHDOG_FAULT_NMI; break;
    case 3U: kind = WATCHDOG_FAULT_HARD; break;
    case 4U: kind = WATCHDOG_FAULT_MEMMANAGE; break;
    case 5U: kind = WATCHDOG_FAULT_BUS; break;
    case 6U: kind = WATCHDOG_FAULT_USAGE; break;
    default: kind = WATCHDOG_FAULT_UNEXPECTED; break;
    }

    if (s_record_ready != 0U && frame != NULL &&
        (frame_address & 3U) == 0U &&
        (exc_return & 0xFFFFFFE0U) == 0xFFFFFFE0U &&
        (SCB->CFSR & WATCHDOG_BOOT_STACK_ERROR_MASK) == 0U)
    {
        frame_size = (exc_return & (1UL << 4)) != 0U ? 32U : 104U;
        if (frame_address >= WATCHDOG_BOOT_AXI_BEGIN &&
            frame_address <= WATCHDOG_BOOT_AXI_END - frame_size)
        {
            ram_end = WATCHDOG_BOOT_AXI_END;
        }
        else if (frame_address >= WATCHDOG_BOOT_DTCM_BEGIN &&
                 frame_address <= WATCHDOG_BOOT_DTCM_END - frame_size)
        {
            ram_end = WATCHDOG_BOOT_DTCM_END;
        }
        frame_end = frame_address + frame_size;
        if (ram_end != 0U && frame_end <= ram_end)
        {
            basic_frame = frame +
                (((exc_return & (1UL << 4)) == 0U) ? 18U : 0U);
        }
    }

    if (s_record_ready != 0U)
    {
        watchdog_h7r7_record_fault(&g_watchdog_retained_record, kind,
                              exc_return, SCB->CFSR, SCB->HFSR,
                              SCB->MMFAR, SCB->BFAR,
                              (uint32_t)frame_address,
                              (uint32_t)(uintptr_t)basic_frame,
                              __get_PSP(), __get_MSP(), ipsr, 0U,
                              DWT->CYCCNT, __get_PRIMASK(),
                              __get_BASEPRI(), __get_CONTROL(),
                              basic_frame, 0U, 0U, 0U, 0U, ipsr);
        watchdog_boot_mark_abnormal();
        watchdog_boot_barrier();
        if (watchdog_fault_valid(&g_watchdog_retained_record.fault) &&
            watchdog_record_mark_fault_reset_requested(
                &g_watchdog_retained_record))
        {
            NVIC_SystemReset();
        }
    }

    /* If record storage was not yet available or the commit failed, preserve
     * the fail-stop and let the independent IWDG reset the MCU. */
    for (;;)
    {
        __NOP();
    }
}

enum
{
    WATCHDOG_FLASH_ERROR_BOUNDS = 1U,
    WATCHDOG_FLASH_ERROR_ERASE_WREN,
    WATCHDOG_FLASH_ERROR_ERASE_CMD,
    WATCHDOG_FLASH_ERROR_ERASE_BUSY,
    WATCHDOG_FLASH_ERROR_PROGRAM_WREN,
    WATCHDOG_FLASH_ERROR_PROGRAM_CMD,
    WATCHDOG_FLASH_ERROR_PROGRAM_BUSY,
    WATCHDOG_FLASH_ERROR_VERIFY_MAP,
    WATCHDOG_FLASH_ERROR_VERIFY_CRC,
    WATCHDOG_FLASH_ERROR_VERIFY_ABORT
};

static void watchdog_boot_barrier(void)
{
    __DSB();
    __ISB();
}

static void watchdog_boot_mark_abnormal(void)
{
    watchdog_record_header_t header;
    if (watchdog_record_active_header(&g_watchdog_retained_record,
                                      &header, NULL))
    {
        g_watchdog_retained_record.footer.magic = WATCHDOG_RECORD_MAGIC;
        g_watchdog_retained_record.footer.version = WATCHDOG_RECORD_VERSION;
        g_watchdog_retained_record.footer.stage_sequence = header.boot_sequence;
        g_watchdog_retained_record.footer.reserved[0] =
            WATCHDOG_PENDING_FLASH_MAGIC;
        watchdog_boot_barrier();
        g_watchdog_retained_record.footer.stage_commit = WATCHDOG_STAGE_COMMIT;
        watchdog_boot_barrier();
    }
}

static void watchdog_boot_log(const char *message)
{
    if (message != NULL && huart4.Instance != NULL)
    {
        (void)HAL_UART_Transmit(&huart4, (uint8_t *)message,
                                (uint16_t)strlen(message), 1000U);
    }
}

static void watchdog_boot_log_flash_error(uint32_t stage, uint16_t status)
{
    static const char digits[] = "0123456789ABCDEF";
    char line[] = "[BOOT] flash log failed stage=00 status=0000\r\n";
    char *stage_text = strstr(line, "stage=") + 6;
    char *status_text = strstr(line, "status=") + 7;

    stage_text[0] = digits[(stage >> 4U) & 0x0FU];
    stage_text[1] = digits[stage & 0x0FU];
    status_text[0] = digits[(status >> 12U) & 0x0FU];
    status_text[1] = digits[(status >> 8U) & 0x0FU];
    status_text[2] = digits[(status >> 4U) & 0x0FU];
    status_text[3] = digits[status & 0x0FU];
    watchdog_boot_log(line);
}

static uint32_t watchdog_bcd_to_binary(uint32_t value)
{
    return ((value >> 4U) * 10U) + (value & 0x0FU);
}

static int watchdog_rtc_enable_source(void)
{
    uint32_t spin = 0U;
    uint32_t source;

    HAL_PWR_EnableBkUpAccess();
    __HAL_RCC_RTCAPB_CLK_ENABLE();

    if ((RCC->BDCR & RCC_BDCR_RTCEN) == 0U)
    {
        RCC->CSR |= RCC_CSR_LSION;
        while ((RCC->CSR & RCC_CSR_LSIRDY) == 0U &&
               spin < WATCHDOG_RTC_INIT_SPIN_LIMIT)
        {
            ++spin;
        }
        if ((RCC->CSR & RCC_CSR_LSIRDY) == 0U)
        {
            return -1;
        }

        /* RTC is unused by the current firmware. Select LSI only when the RTC
         * is disabled; never reset the backup domain or erase retained data. */
        MODIFY_REG(RCC->BDCR, RCC_BDCR_RTCSEL, RCC_BDCR_RTCSEL_1);
        SET_BIT(RCC->BDCR, RCC_BDCR_RTCEN);
        watchdog_boot_barrier();
    }

    if ((RCC->BDCR & RCC_BDCR_RTCSEL) == RCC_BDCR_RTCSEL_1)
    {
        RCC->CSR |= RCC_CSR_LSION;
        spin = 0U;
        while ((RCC->CSR & RCC_CSR_LSIRDY) == 0U &&
               spin < WATCHDOG_RTC_INIT_SPIN_LIMIT)
        {
            ++spin;
        }
        if ((RCC->CSR & RCC_CSR_LSIRDY) == 0U)
        {
            return -1;
        }
    }
    else
    {
        source = RCC->BDCR & RCC_BDCR_RTCSEL;
        if (source == RCC_BDCR_RTCSEL_0 &&
            (RCC->BDCR & RCC_BDCR_LSERDY) == 0U)
        {
            return -1;
        }
        if (source == (RCC_BDCR_RTCSEL_0 | RCC_BDCR_RTCSEL_1) &&
            (RCC->CR & RCC_CR_HSERDY) == 0U)
        {
            return -1;
        }
        if (source == 0U)
        {
            return -1;
        }
    }
    return 0;
}

static int watchdog_rtc_initialize_if_needed(void)
{
    uint32_t spin = 0U;

    if (watchdog_rtc_enable_source() != 0)
    {
        return -1;
    }

    if ((RTC->ICSR & RTC_ICSR_INITS) == 0U)
    {
        RTC->WPR = 0xCAU;
        RTC->WPR = 0x53U;
        RTC->ICSR |= RTC_ICSR_INIT;
        while ((RTC->ICSR & RTC_ICSR_INITF) == 0U &&
               spin < WATCHDOG_RTC_INIT_SPIN_LIMIT)
        {
            ++spin;
        }
        if ((RTC->ICSR & RTC_ICSR_INITF) == 0U)
        {
            RTC->WPR = 0xFFU;
            return -1;
        }

        RTC->CR &= ~RTC_CR_FMT; /* 24-hour BCD calendar */
        RTC->PRER = (WATCHDOG_RTC_ASYNC_PREDIV << RTC_PRER_PREDIV_A_Pos) |
                    (WATCHDOG_RTC_SYNC_PREDIV << RTC_PRER_PREDIV_S_Pos);
        RTC->TR = 0U;
        /* 2000-01-01 Saturday 00:00:00; only relative elapsed time matters. */
        RTC->DR = (6U << RTC_DR_WDU_Pos) | (1U << RTC_DR_DU_Pos) |
                  (1U << RTC_DR_MU_Pos);
        RTC->ICSR &= ~RTC_ICSR_INIT;
        RTC->WPR = 0xFFU;
        watchdog_boot_barrier();
    }

    if ((RTC->ICSR & RTC_ICSR_RSF) == 0U)
    {
        RTC->ICSR &= ~RTC_ICSR_RSF;
        spin = 0U;
        while ((RTC->ICSR & RTC_ICSR_RSF) == 0U &&
               spin < WATCHDOG_RTC_INIT_SPIN_LIMIT)
        {
            ++spin;
        }
    }
    return (RTC->ICSR & RTC_ICSR_INITS) != 0U ? 0 : -1;
}

static int watchdog_rtc_seconds_of_day(uint32_t *seconds)
{
    uint32_t tr0;
    uint32_t tr1;
    uint32_t hours;
    uint32_t minutes;
    uint32_t secs;
    uint32_t spin;

    if (seconds == NULL || watchdog_rtc_initialize_if_needed() != 0)
    {
        return -1;
    }
    for (spin = 0U; spin < 128U; ++spin)
    {
        tr0 = RTC->TR;
        (void)RTC->DR;
        tr1 = RTC->TR;
        if (((tr0 ^ tr1) & 0x007F7F7FU) == 0U)
        {
            break;
        }
    }
    if (spin == 128U)
    {
        return -1;
    }

    secs = watchdog_bcd_to_binary((tr1 & RTC_TR_SU_Msk) >> RTC_TR_SU_Pos) +
           10U * watchdog_bcd_to_binary((tr1 & RTC_TR_ST_Msk) >> RTC_TR_ST_Pos);
    minutes = watchdog_bcd_to_binary((tr1 & RTC_TR_MNU_Msk) >> RTC_TR_MNU_Pos) +
              10U * watchdog_bcd_to_binary((tr1 & RTC_TR_MNT_Msk) >> RTC_TR_MNT_Pos);
    hours = watchdog_bcd_to_binary((tr1 & RTC_TR_HU_Msk) >> RTC_TR_HU_Pos) +
            10U * watchdog_bcd_to_binary((tr1 & RTC_TR_HT_Msk) >> RTC_TR_HT_Pos);
    if (secs >= 60U || minutes >= 60U || hours >= 24U)
    {
        return -1;
    }
    *seconds = hours * 3600U + minutes * 60U + secs;
    return 0;
}

int boot_watchdog_prepare(uint32_t reset_flags)
{
    watchdog_record_boot_context_t record_context;
    watchdog_record_policy_t record_policy;
    watchdog_record_header_t previous_header;
    uint32_t rtc_second = 0U;
    uint32_t reset_count = 0U;
    uint32_t spin = 0U;
    int rtc_valid;
    int tamper_erase_triggered;
    int previous_header_valid;

    s_backup_memory_ready = 0U;
    HAL_PWR_EnableBkUpAccess();
    __HAL_RCC_BKPRAM_CLK_ENABLE();
    __HAL_RCC_SBS_CLK_ENABLE();
    /* TAMP is on RTCAPB. Its clock must be enabled before the tamper source
     * configuration and pending flags can be sampled reliably. */
    __HAL_RCC_RTCAPB_CLK_ENABLE();
    watchdog_boot_barrier();
    tamper_erase_triggered = boot_tamper_erase_triggered(reset_flags);

    /* RM0477 SBS_MESR.MEF remains clear while a POR/tamper erase is in
     * progress. RCC_RSR flags are sticky until RMVF; the shared policy treats
     * PORRSTF as current only when no secondary reset-source flag is present. */
    while (watchdog_h7r7_backup_erase_wait_required(
               reset_flags, tamper_erase_triggered,
               __HAL_SBS_GET_MEMORIES_ERASE_STATUS() != 0U) &&
           spin < WATCHDOG_BKPSRAM_READY_SPIN_LIMIT)
    {
        ++spin;
    }
    if (watchdog_h7r7_backup_erase_wait_required(
            reset_flags, tamper_erase_triggered,
            __HAL_SBS_GET_MEMORIES_ERASE_STATUS() != 0U))
    {
        watchdog_boot_log("[BOOT] BKPSRAM erase not ready\r\n");
        return -1;
    }
    /* Boot is the sole gate that may first read/initialize the retained SRAM.
     * With no POR or configured erase-on-tamper event, MEF=0 is its reset
     * value, not evidence of an active erase. */
    s_backup_memory_ready = 1U;

    if (watchdog_h7r7_iwdg_start_or_refresh() != 0)
    {
        watchdog_boot_log("[BOOT] watchdog init failed\r\n");
        return -1;
    }

    /* POR may trigger hardware erase (covered by MEF above), while BOR may
     * leave VSW-powered BKPSRAM intact. Trust only CRC/commit-valid content;
     * otherwise initialize all words with aligned 32-bit writes. */
    previous_header_valid = watchdog_record_active_header(
        &g_watchdog_retained_record, &previous_header, NULL);
    if (!previous_header_valid)
    {
        /* A previously uninitialized or damaged record is never trusted. All
         * storage writes are aligned complete words for BKPSRAM ECC safety. */
        watchdog_record_initialize(&g_watchdog_retained_record, NULL,
                                   reset_flags);
    }

    /* Preserve a synchronous-fault trace request before the shared record
     * boot transition consumes the one-shot reset-request marker. The later
     * XSPI phase will snapshot and commit this ring after hardware setup. */
    (void)boot_watchdog_trace_flash_preserve_fault(reset_flags);

    rtc_valid = watchdog_rtc_seconds_of_day(&rtc_second) == 0;
    memset(&record_context, 0, sizeof(record_context));
    record_context.raw_reset_flags = reset_flags;
    record_context.reset_causes = watchdog_h7r7_reset_causes(reset_flags);
    record_context.previous_reset_causes = previous_header_valid
        ? watchdog_h7r7_reset_causes(previous_header.reset_flags) : 0U;
    record_context.rtc_second = rtc_second;
    record_context.rtc_valid = rtc_valid != 0 ? 1U : 0U;
    record_context.tick = 0U;
    record_policy.reset_window_seconds = WATCHDOG_RESET_WINDOW_SECONDS;
    watchdog_record_boot_begin(&g_watchdog_retained_record, NULL,
                               &record_context, &record_policy, &reset_count);
    s_recovery_streak = reset_count;
    if (reset_count >= WATCHDOG_RESET_STREAK_LIMIT)
    {
        watchdog_record_append_event(&g_watchdog_retained_record, 0U,
                                     WATCHDOG_EVENT_RECOVERY,
                                     reset_count, WATCHDOG_RESET_STREAK_LIMIT,
                                     0U, 0U);
    }
    if (!watchdog_header_valid(&g_watchdog_retained_record.headers[0]) &&
        !watchdog_header_valid(&g_watchdog_retained_record.headers[1]))
    {
        return -1;
    }
    s_record_ready = 1U;
    watchdog_boot_barrier();

    /* Reset-source flags are sticky. They are cleared only after the raw
     * value and reset streak have been committed to BKPSRAM. */
    RCC->RSR |= RCC_RSR_RMVF;
    watchdog_boot_barrier();
    return 0;
}

int boot_watchdog_reset_streak(void)
{
    return (int)s_recovery_streak;
}

int boot_watchdog_recovery_required(void)
{
    return (s_backup_memory_ready == 0U || s_record_ready == 0U ||
            s_recovery_streak >= WATCHDOG_RESET_STREAK_LIMIT) ? 1 : 0;
}

void boot_watchdog_record_error(uint32_t stage, int32_t status)
{
    if (s_record_ready == 0U || s_boot_error_recorded != 0U)
    {
        return;
    }
    s_boot_error_recorded = 1U;
    if (!watchdog_fault_valid(&g_watchdog_retained_record.fault))
    {
        watchdog_h7r7_record_fault(&g_watchdog_retained_record,
                              WATCHDOG_FAULT_BOOT_ERROR, 0U,
                              SCB->CFSR, SCB->HFSR, SCB->MMFAR, SCB->BFAR,
                              0U, 0U, __get_PSP(), __get_MSP(), 0U,
                              0U, 0U, __get_PRIMASK(), __get_BASEPRI(),
                              __get_CONTROL(), NULL, stage,
                              (uint32_t)status, 0U,
                              (uint32_t)(uintptr_t)__builtin_return_address(0),
                              stage);
    }
    watchdog_record_append_event(&g_watchdog_retained_record, 0U,
                                 WATCHDOG_EVENT_BOOT_FAILURE,
                                 stage, (uint32_t)status, 0U, 0U);
    watchdog_boot_mark_abnormal();
    watchdog_boot_barrier();
    /* At the recovery threshold, let boot_extmem_fail/Error_Handler enter the
     * recovery loop after this stage is committed. Resetting here would skip
     * that gate and can create an automatic software-reset loop. */
    if (s_recovery_streak >= WATCHDOG_RESET_STREAK_LIMIT)
    {
        return;
    }
    if (watchdog_fault_valid(&g_watchdog_retained_record.fault) &&
        watchdog_record_mark_fault_reset_requested(
            &g_watchdog_retained_record))
    {
        NVIC_SystemReset();
    }
    /* A failed marker must not be mistaken for a captured software fault on
     * the next boot. Stop refreshing and let the independent IWDG classify
     * and reset this failed boot instead. */
    for (;;)
    {
        __NOP();
    }
}

void boot_watchdog_record_assert(uint32_t file_address, uint32_t line)
{
    if (s_record_ready == 0U || s_boot_error_recorded != 0U)
    {
        return;
    }
    s_boot_error_recorded = 1U;
    watchdog_h7r7_record_fault(&g_watchdog_retained_record,
                          WATCHDOG_FAULT_ASSERT, 0U,
                          SCB->CFSR, SCB->HFSR, SCB->MMFAR, SCB->BFAR,
                          0U, 0U, __get_PSP(), __get_MSP(), 0U,
                          0U, DWT->CYCCNT, __get_PRIMASK(),
                          __get_BASEPRI(), __get_CONTROL(), NULL,
                          file_address, 0U, 0U,
                          (uint32_t)(uintptr_t)__builtin_return_address(0),
                          line);
    watchdog_boot_mark_abnormal();
    watchdog_boot_barrier();
    if (s_recovery_streak >= WATCHDOG_RESET_STREAK_LIMIT)
    {
        return;
    }
    if (watchdog_fault_valid(&g_watchdog_retained_record.fault) &&
        watchdog_record_mark_fault_reset_requested(
            &g_watchdog_retained_record))
    {
        NVIC_SystemReset();
    }
    for (;;)
    {
        __NOP();
    }
}

static void watchdog_invalidate_mapped_cache(uint32_t address, uint32_t size)
{
    uint32_t aligned_address = address & ~31U;
    uint32_t aligned_size = (size + (address - aligned_address) + 31U) & ~31U;
    SCB_InvalidateDCache_by_Addr((uint32_t *)(uintptr_t)aligned_address,
                                 (int32_t)aligned_size);
    __DSB();
    __ISB();
}

static int watchdog_flash_map_begin(void)
{
    if (W35T51NWTBIE_EnableMemoryMappedModeDTR(&hxspi2,
                                               W35T51NWTBIE_OPI_MODE) !=
        W35T51NWTBIE_OK)
    {
        return -1;
    }
    watchdog_invalidate_mapped_cache(WATCHDOG_FLASH_MAPPED_BASE,
                                     WATCHDOG_FLASH_SIZE);
    return 0;
}

static int watchdog_flash_map_end(void)
{
    __DSB();
    return W35T51NWTBIE_DisableMemoryMappedMode(&hxspi2) ==
                   W35T51NWTBIE_OK
               ? 0
               : -1;
}

static void watchdog_copy_mapped_record(uint32_t slot,
                                        watchdog_retained_record_t *out)
{
    const volatile uint32_t *source = (const volatile uint32_t *)(uintptr_t)(
        WATCHDOG_FLASH_MAPPED_BASE + slot * WATCHDOG_FLASH_SLOT_SIZE);
    uint32_t *destination = (uint32_t *)out;
    uint32_t i;
    watchdog_invalidate_mapped_cache(
        WATCHDOG_FLASH_MAPPED_BASE + slot * WATCHDOG_FLASH_SLOT_SIZE,
        WATCHDOG_FLASH_SLOT_SIZE);
    for (i = 0U; i < WATCHDOG_FLASH_SLOT_SIZE / sizeof(uint32_t); ++i)
    {
        destination[i] = source[i];
    }
}

static int watchdog_scan_log_ring(void)
{
    uint32_t i;
    int latest_valid = 0;

    memset(s_slot_state, WATCHDOG_FLASH_POLICY_READ_ERROR,
           sizeof(s_slot_state));
    memset(s_slot_sequence, 0, sizeof(s_slot_sequence));
    memset(s_slot_incident_id, 0, sizeof(s_slot_incident_id));
    memset(&s_slot_scan_counts, 0, sizeof(s_slot_scan_counts));
    s_latest_sequence = 0U;
    s_latest_slot = 0U;
    s_valid_slot_count = 0U;
    s_latest_incident_id = 0U;
    if (watchdog_flash_map_begin() != 0)
    {
        return -1;
    }

    for (i = 0U; i < WATCHDOG_FLASH_SECTOR_COUNT; ++i)
    {
        uint64_t incident_id = 0U;
        int has_incident_id;
        watchdog_copy_mapped_record(i, &s_flash_record);
        has_incident_id = watchdog_record_incident_id_valid(&s_flash_record,
                                                            &incident_id);
        s_slot_state[i] = watchdog_flash_policy_classify(
            (const uint8_t *)&s_flash_record, sizeof(s_flash_record), 0xFFU,
            watchdog_flash_record_valid(&s_flash_record));
        if (s_slot_state[i] == WATCHDOG_FLASH_POLICY_READ_ERROR)
        {
            (void)watchdog_flash_map_end();
            return -1;
        }
        /* A complete ID transaction in a partially written sector reserves
         * its high-water value even if the enclosing Flash record did not
         * reach its final commit marker before reset. */
        if (has_incident_id && incident_id > s_latest_incident_id)
        {
            s_latest_incident_id = incident_id;
        }
        if (s_slot_state[i] == WATCHDOG_FLASH_POLICY_VALID)
        {
            uint32_t sequence = s_flash_record.footer.flash_sequence;
            s_slot_sequence[i] = sequence;
            if (has_incident_id)
            {
                s_slot_incident_id[i] = incident_id;
            }
            s_valid_slot_count += 1U;
            if (!latest_valid ||
                (int32_t)(sequence - s_latest_sequence) > 0)
            {
                latest_valid = 1;
                s_latest_sequence = sequence;
                s_latest_slot = i;
            }
        }
    }

    if (watchdog_flash_map_end() != 0)
    {
        return -1;
    }

    if (watchdog_flash_policy_count(s_slot_state,
                                    WATCHDOG_FLASH_SECTOR_COUNT,
                                    &s_slot_scan_counts) != 0 ||
        s_slot_scan_counts.valid != s_valid_slot_count)
    {
        return -1;
    }

    return 0;
}

static uint32_t watchdog_find_incident_duplicate(uint64_t incident_id)
{
    uint32_t i;
    for (i = 0U; i < WATCHDOG_FLASH_SECTOR_COUNT; ++i)
    {
        if (s_slot_state[i] == WATCHDOG_FLASH_POLICY_VALID &&
            incident_id != 0U &&
            s_slot_incident_id[i] == incident_id)
        {
            return i;
        }
    }
    return WATCHDOG_FLASH_SECTOR_COUNT;
}

static uint32_t watchdog_choose_log_slot(void)
{
    size_t target;
    size_t start;

    if (s_valid_slot_count >= WATCHDOG_FLASH_SECTOR_COUNT)
    {
        return WATCHDOG_FLASH_SECTOR_COUNT;
    }
    start = s_valid_slot_count == 0U
        ? 0U
        : ((size_t)s_latest_slot + 1U) % WATCHDOG_FLASH_SECTOR_COUNT;
    if (watchdog_flash_policy_choose_append_slot(
            s_slot_state, WATCHDOG_FLASH_SECTOR_COUNT, start, &target) == 0)
    {
        return (uint32_t)target;
    }
    return WATCHDOG_FLASH_SECTOR_COUNT;
}

/* Keep at most 64 durable records. The 65th sector is the transaction spare:
 * a new log is programmed and verified there before the oldest is erased. */
static int watchdog_prune_oldest_log(void)
{
    uint32_t i;
    uint32_t oldest_slot = WATCHDOG_FLASH_SECTOR_COUNT;
    uint32_t oldest_sequence = 0U;

    if (s_valid_slot_count <= WATCHDOG_FLASH_SLOT_COUNT)
    {
        return 0;
    }
    for (i = 0U; i < WATCHDOG_FLASH_SECTOR_COUNT; ++i)
    {
        if (s_slot_state[i] == WATCHDOG_FLASH_POLICY_VALID &&
            (oldest_slot == WATCHDOG_FLASH_SECTOR_COUNT ||
             (int32_t)(s_slot_sequence[i] - oldest_sequence) < 0))
        {
            oldest_slot = i;
            oldest_sequence = s_slot_sequence[i];
        }
    }
    if (oldest_slot == WATCHDOG_FLASH_SECTOR_COUNT ||
        watchdog_erase_slot(oldest_slot) != 0)
    {
        return -1;
    }
    s_slot_state[oldest_slot] = WATCHDOG_FLASH_POLICY_BLANK;
    s_valid_slot_count -= 1U;
    if (s_slot_scan_counts.valid > 0U)
    {
        --s_slot_scan_counts.valid;
    }
    ++s_slot_scan_counts.blank;
    return 0;
}

static int watchdog_flash_write_enable(void)
{
    uint8_t status[2] = {0xFFU, 0xFFU};
    int32_t result;

    s_flash_wren_state = 0U;
    s_flash_wren_hal_error = 0U;
    s_flash_wren_xspi_sr = 0U;
    s_flash_wren_xspi_cr = 0U;
    result = W35T51NWTBIE_WriteEnable(&hxspi2, WATCHDOG_XSPI_OPI,
                                      WATCHDOG_XSPI_DTR);
    if (result == W35T51NWTBIE_OK)
    {
        return 0;
    }

    /* Preserve the first failing state before a recovery status read can
     * change the HAL handle or replace its error details. */
    s_flash_wren_state = (uint32_t)hxspi2.State;
    s_flash_wren_hal_error = hxspi2.ErrorCode;
    if (hxspi2.Instance != NULL)
    {
        s_flash_wren_xspi_sr = hxspi2.Instance->SR;
        s_flash_wren_xspi_cr = hxspi2.Instance->CR;
    }

    /* The prior board capture showed WriteEnable() returning an error while
     * the status bytes were 0x02/0x02 (WEL set, BUSY clear). OPI DTR returns
     * the status byte on both data edges; require both copies to confirm the
     * latch before treating auto-poll's error as a false negative. */
    if (W35T51NWTBIE_ReadStatusRegister(&hxspi2, WATCHDOG_XSPI_OPI,
                                       WATCHDOG_XSPI_DTR, status) ==
            W35T51NWTBIE_OK &&
        hxspi2.State == HAL_XSPI_STATE_READY &&
        (status[0] & W35T51NWTBIE_SR_WEL) != 0U &&
        (status[0] & W35T51NWTBIE_SR_WIP) == 0U &&
        (status[1] & W35T51NWTBIE_SR_WEL) != 0U &&
        (status[1] & W35T51NWTBIE_SR_WIP) == 0U)
    {
        watchdog_boot_log("[BOOT] WREN poll error; status confirms WEL=1 WIP=0\r\n");
        return 0;
    }

    return -1;
}

static int watchdog_flash_wait_ready(void)
{
    return W35T51NWTBIE_AutoPollingMemReady(&hxspi2, WATCHDOG_XSPI_OPI,
                                            WATCHDOG_XSPI_DTR) ==
                   W35T51NWTBIE_OK
               ? 0
               : -1;
}

static int watchdog_program_page(uint32_t address, uint8_t *data,
                                 uint32_t length)
{
    if (length == 0U || length > W35T51NWTBIE_PAGE_SIZE ||
        (address & 0xFFU) + length > W35T51NWTBIE_PAGE_SIZE ||
        address < WATCHDOG_FLASH_OFFSET ||
        address + length > WATCHDOG_FLASH_OFFSET + WATCHDOG_FLASH_SIZE)
    {
        s_flash_error_stage = WATCHDOG_FLASH_ERROR_BOUNDS;
        return -1;
    }
    if (watchdog_flash_write_enable() != 0)
    {
        s_flash_error_stage = WATCHDOG_FLASH_ERROR_PROGRAM_WREN;
        return -1;
    }
    if (W35T51NWTBIE_PageProgramDTR(&hxspi2, data, address, length) !=
        W35T51NWTBIE_OK)
    {
        s_flash_error_stage = WATCHDOG_FLASH_ERROR_PROGRAM_CMD;
        return -1;
    }
    if (watchdog_flash_wait_ready() != 0)
    {
        s_flash_error_stage = WATCHDOG_FLASH_ERROR_PROGRAM_BUSY;
        return -1;
    }
    watchdog_h7r7_iwdg_refresh();
    return 0;
}

static int watchdog_erase_slot(uint32_t slot)
{
    uint32_t address = WATCHDOG_FLASH_OFFSET +
                       slot * WATCHDOG_FLASH_SLOT_SIZE;
    if (slot >= WATCHDOG_FLASH_SECTOR_COUNT ||
        address + WATCHDOG_FLASH_SLOT_SIZE >
            WATCHDOG_FLASH_OFFSET + WATCHDOG_FLASH_SIZE)
    {
        s_flash_error_stage = WATCHDOG_FLASH_ERROR_BOUNDS;
        return -1;
    }
    if (watchdog_flash_write_enable() != 0)
    {
        s_flash_error_stage = WATCHDOG_FLASH_ERROR_ERASE_WREN;
        return -1;
    }
    if (W35T51NWTBIE_BlockErase(&hxspi2, WATCHDOG_XSPI_OPI,
                                WATCHDOG_XSPI_DTR, WATCHDOG_XSPI_ADDR_SIZE,
                                address, W35T51NWTBIE_ERASE_4K) !=
        W35T51NWTBIE_OK)
    {
        s_flash_error_stage = WATCHDOG_FLASH_ERROR_ERASE_CMD;
        return -1;
    }
    if (watchdog_flash_wait_ready() != 0)
    {
        s_flash_error_stage = WATCHDOG_FLASH_ERROR_ERASE_BUSY;
        return -1;
    }
    watchdog_h7r7_iwdg_refresh();
    return 0;
}

static int watchdog_write_flash_record(uint32_t slot, uint32_t sequence)
{
    uint8_t page[W35T51NWTBIE_PAGE_SIZE] __attribute__((aligned(32)));
    uint32_t offset;
    uint32_t i;
    uint32_t base = WATCHDOG_FLASH_OFFSET +
                    slot * WATCHDOG_FLASH_SLOT_SIZE;

    if (slot >= WATCHDOG_FLASH_SECTOR_COUNT ||
        base + WATCHDOG_FLASH_SLOT_SIZE >
            WATCHDOG_FLASH_OFFSET + WATCHDOG_FLASH_SIZE)
    {
        s_flash_error_stage = WATCHDOG_FLASH_ERROR_BOUNDS;
        return -1;
    }
    if (watchdog_erase_slot(slot) != 0)
    {
        return -1;
    }

    {
        const volatile uint32_t *source_words =
            (const volatile uint32_t *)&g_watchdog_retained_record;
        uint32_t *destination_words = (uint32_t *)&s_flash_record;
        for (i = 0U; i < sizeof(s_flash_record) / sizeof(uint32_t); ++i)
        {
            destination_words[i] = source_words[i];
        }
    }
    watchdog_record_mark_flash_image(&s_flash_record, sequence);
    s_flash_record.footer.flash_commit = 0xFFFFFFFFUL;

    for (offset = 0U; offset < WATCHDOG_FLASH_SLOT_SIZE;
         offset += W35T51NWTBIE_PAGE_SIZE)
    {
        memcpy(page, ((const uint8_t *)&s_flash_record) + offset,
               sizeof(page));
        if (watchdog_program_page(base + offset, page, sizeof(page)) != 0)
        {
            return -1;
        }
    }

    /* The final 32-bit marker is a separate, last program operation. */
    {
        uint32_t commit = WATCHDOG_FLASH_COMMIT;
        if (watchdog_program_page(base + WATCHDOG_FLASH_SLOT_SIZE -
                                      sizeof(commit),
                                  (uint8_t *)&commit, sizeof(commit)) != 0)
        {
            return -1;
        }
    }
    return 0;
}

static int watchdog_verify_flash_record(uint32_t slot)
{
    int valid;
    if (watchdog_flash_map_begin() != 0)
    {
        s_flash_error_stage = WATCHDOG_FLASH_ERROR_VERIFY_MAP;
        return -1;
    }
    watchdog_copy_mapped_record(slot, &s_flash_record);
    valid = watchdog_flash_record_valid(&s_flash_record);
    if (watchdog_flash_map_end() != 0)
    {
        s_flash_error_stage = WATCHDOG_FLASH_ERROR_VERIFY_ABORT;
        return -1;
    }
    if (!valid)
    {
        s_flash_error_stage = WATCHDOG_FLASH_ERROR_VERIFY_CRC;
    }
    return valid ? 0 : -1;
}

int boot_watchdog_flash_log_flush(void)
{
    watchdog_record_header_t header;
    unsigned active_slot;
    uint32_t duplicate_slot = WATCHDOG_FLASH_SECTOR_COUNT;
    uint32_t next_sequence;
    uint32_t target_slot;
    uint64_t incident_id = 0U;
    uint64_t next_incident_id = 0U;
    int incident_id_valid;
    uint8_t status_register[2] = {0xFFU, 0xFFU};
    uint16_t status_word = 0xFFFFU;

    if (!watchdog_record_active_header(&g_watchdog_retained_record,
                                       &header, &active_slot))
    {
        return -1;
    }

    /* A recent exception or an IWDG reset is persistent work. A normal reset
     * does not generate a flash write, preventing wear in healthy operation. */
    if ((header.reset_flags & RCC_RSR_IWDGRSTF) == 0U &&
        g_watchdog_retained_record.footer.reserved[0] !=
            WATCHDOG_PENDING_FLASH_MAGIC &&
        !watchdog_fault_valid(&g_watchdog_retained_record.fault))
    {
        return 0;
    }

    if (watchdog_scan_log_ring() != 0)
    {
        watchdog_boot_log("[BOOT] watchdog journal scan failed\r\n");
        return -1;
    }

    incident_id_valid = watchdog_record_incident_id_valid(
        &g_watchdog_retained_record, &incident_id);
    /* A retained transaction ID that is older than the current journal high
     * water mark but absent from the ring may have been pruned. Give this
     * uncommitted retry a fresh ID rather than reusing a historical one. */
    if (!incident_id_valid ||
        (incident_id <= s_latest_incident_id &&
         watchdog_find_incident_duplicate(incident_id) >=
             WATCHDOG_FLASH_SECTOR_COUNT))
    {
        if (!watchdog_record_incident_id_next(s_latest_incident_id,
                                              &next_incident_id))
        {
            watchdog_boot_log("[BOOT] watchdog incident ID exhausted\r\n");
            return -1;
        }
        incident_id = next_incident_id;
        watchdog_record_incident_id_commit(&g_watchdog_retained_record,
                                           incident_id);
    }
    duplicate_slot = watchdog_find_incident_duplicate(incident_id);
    if (duplicate_slot < WATCHDOG_FLASH_SECTOR_COUNT)
    {
        /* Power may have failed after the new sector commit but before the
         * old sector reclamation. The committed duplicate is safe; restore
         * the 64-record ring geometry before clearing the retained staging. */
        if (watchdog_prune_oldest_log() != 0)
        {
            return -1;
        }
        watchdog_record_mark_flash_committed(&g_watchdog_retained_record,
                                              s_slot_sequence[duplicate_slot]);
        return 0;
    }

    /* Recover a completed append whose prior boot did not finish the old
     * sector erase. Every valid sector has a commit marker and a verified CRC. */
    if (watchdog_prune_oldest_log() != 0)
    {
        watchdog_boot_log("[BOOT] watchdog journal recovery prune failed\r\n");
        return -1;
    }

    next_sequence = s_valid_slot_count == 0U ? 1U : s_latest_sequence + 1U;
    if (next_sequence == 0U)
    {
        next_sequence = 1U;
    }
    target_slot = watchdog_choose_log_slot();
    if (target_slot >= WATCHDOG_FLASH_SECTOR_COUNT)
    {
        watchdog_boot_log("[BOOT] watchdog journal has no scratch sector\r\n");
        return -1;
    }

    s_flash_error_stage = 0U;
    if (watchdog_write_flash_record(target_slot, next_sequence) != 0 ||
        watchdog_verify_flash_record(target_slot) != 0)
    {
        if (W35T51NWTBIE_ReadStatusRegister(&hxspi2, WATCHDOG_XSPI_OPI,
                                             WATCHDOG_XSPI_DTR,
                                             status_register) ==
            W35T51NWTBIE_OK)
        {
            status_word = (uint16_t)(((uint16_t)status_register[0] << 8U) |
                                     status_register[1]);
        }
        watchdog_record_append_event(&g_watchdog_retained_record, 0U,
                                     WATCHDOG_EVENT_FLASH_FAILURE,
                                     ((target_slot & 0xFFFFU) << 16U) |
                                         (s_flash_error_stage & 0xFFFFU),
                                     status_word, 0U, 0U);
        if (s_flash_error_stage == WATCHDOG_FLASH_ERROR_ERASE_WREN ||
            s_flash_error_stage == WATCHDOG_FLASH_ERROR_PROGRAM_WREN)
        {
            watchdog_record_append_event(
                &g_watchdog_retained_record, 0U,
                WATCHDOG_EVENT_FLASH_FAILURE,
                WATCHDOG_FLASH_FAILURE_XSPI_DETAIL,
                ((s_flash_wren_hal_error & 0xFFFFU) << 16U) |
                    (s_flash_wren_state & 0xFFFFU),
                s_flash_wren_xspi_sr, s_flash_wren_xspi_cr);
        }
        watchdog_boot_log_flash_error(s_flash_error_stage, status_word);
        watchdog_boot_log("[BOOT] watchdog journal write failed; staging retained\r\n");
        return -1;
    }

    /* Refresh the scan state and only reclaim an older log after the newly
     * committed image has passed both the Flash CRC and mapped readback. */
    if (watchdog_scan_log_ring() != 0 ||
        watchdog_prune_oldest_log() != 0)
    {
        watchdog_record_append_event(&g_watchdog_retained_record, 0U,
                                     WATCHDOG_EVENT_FLASH_FAILURE,
                                     target_slot, next_sequence, 0U, 0U);
        watchdog_boot_log("[BOOT] watchdog journal cleanup failed; staging retained\r\n");
        return -1;
    }

    watchdog_record_append_event(&g_watchdog_retained_record, 0U,
                                 WATCHDOG_EVENT_FLASH_COMMIT,
                                 target_slot, next_sequence, 0U, 0U);
    watchdog_record_mark_flash_committed(&g_watchdog_retained_record,
                                          next_sequence);
    watchdog_boot_log("[BOOT] watchdog journal record committed\r\n");
    return 0;
}

void boot_watchdog_recovery_loop(void)
{
    static const char command[] = "RECOVER";
    char received[sizeof(command) - 1U];
    uint32_t matched = 0U;
    uint8_t byte;

    if (s_backup_memory_ready == 0U)
    {
        /* Do not let RECOVER read or mutate ECC-protected BKPSRAM while a
         * genuine POR/tamper erase is still active. Once hardware reports
         * completion, restart and let boot_watchdog_prepare initialize it. */
        watchdog_boot_log("[BOOT] BKPSRAM erase pending; waiting for hardware\r\n");
        for (;;)
        {
            watchdog_h7r7_iwdg_refresh();
            if (__HAL_SBS_GET_MEMORIES_ERASE_STATUS() != 0U)
            {
                NVIC_SystemReset();
            }
            HAL_Delay(100U);
        }
    }

    if (s_record_ready == 0U)
    {
        /* Do not accept RECOVER when the retained record failed validation or
         * the watchdog could not be initialized; no record mutation is safe. */
        watchdog_boot_log("[BOOT] watchdog record unavailable; service via SWD\r\n");
        for (;;)
        {
            watchdog_h7r7_iwdg_refresh();
            HAL_Delay(100U);
        }
    }

    if (huart4.Instance == NULL)
    {
        /* A clock/peripheral failure can occur before UART4 exists. Stop
         * automatic app restarts safely; recovery then requires SWD reflashing. */
        for (;;)
        {
            watchdog_h7r7_iwdg_refresh();
            __NOP();
        }
    }

    watchdog_boot_log("[BOOT] recovery state; send RECOVER then Enter to clear watchdog streak\r\n");
    for (;;)
    {
        HAL_StatusTypeDef status = HAL_UART_Receive(&huart4, &byte, 1U, 100U);
        watchdog_h7r7_iwdg_refresh();
        if (status != HAL_OK)
        {
            continue;
        }
        if (byte == '\r' || byte == '\n')
        {
            if (matched == sizeof(command) - 1U &&
                memcmp(received, command, sizeof(command) - 1U) == 0)
            {
                watchdog_record_clear_recovery(&g_watchdog_retained_record);
                RCC->RSR |= RCC_RSR_RMVF;
                watchdog_boot_log("[BOOT] recovery cleared; restarting\r\n");
                watchdog_boot_barrier();
                NVIC_SystemReset();
            }
            matched = 0U;
            continue;
        }
        if (matched < sizeof(received))
        {
            received[matched++] = (char)byte;
        }
        else
        {
            matched = 0U;
        }
    }
}
