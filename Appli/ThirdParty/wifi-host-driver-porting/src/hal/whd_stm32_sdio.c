/*
 * STM32H7RS SDMMC2 adapter for Infineon WHD (CYW43438, 2.4 GHz).
 *
 * CubeMX remains the owner of the pin alternate-function and peripheral
 * clock description in Core/Src/sdmmc.c.  This file only reuses its generated
 * HAL_SDIO_MspInit() routine and drives the SDMMC controller through ST's LL
 * SDMMC API.  The WHD protocol itself remains the unmodified official
 * Infineon SDIO implementation.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "stm32h7rsxx_hal.h"
#include "stm32h7rsxx_ll_sdmmc.h"

#include "cyhal_sdio.h"
#include "drv_gpio.h"
#include "drivers/dma.h"
#include "rt_dma_h7rs.h"
#include "rtthread.h"
#include "sdmmc.h"
#include "usb_perf.h"
#include "whd_mongoose_buffer.h"
#include "whd_stm32_sdio.h"
#include "startup_sdio_poll.h"
#include "startup_coordinator.h"

#define WHD_SDIO_CLOCK_INIT_HZ      400000U
#define WHD_SDIO_BLOCK_SIZE            64U
#define WHD_SDIO_DMA_NODE_SIZE      8192U
#define WHD_SDIO_DMA_NODE_COUNT        4U
#define WHD_SDIO_DMA_BUFFER_SIZE \
    (WHD_SDIO_DMA_NODE_SIZE * WHD_SDIO_DMA_NODE_COUNT)
#define WHD_SDIO_DMA_PAIR_COUNT         2U
#define WHD_SDIO_DIRECT_ENABLE          1U
#define WHD_SDIO_DIRECT_RX_ENABLE       0U
#define WHD_SDIO_DIRECT_ALIGNMENT       4U
#define WHD_SDIO_CMD_TIMEOUT_MS       100U
#define WHD_SDIO_DATA_TIMEOUT_MS     1000U
#define WHD_SDIO_ABORT_QUIESCE_POLLS  100U

/* SDIO R5 status flags in RESP1. */
#define WHD_SDIO_R5_ERROR             (1UL << 11)
#define WHD_SDIO_R5_FUNCTION_NUMBER   (1UL << 9)
#define WHD_SDIO_R5_OUT_OF_RANGE      (1UL << 8)

/* H7R/S exposes linked-list IDMA rather than the legacy H7 BASE0/BASE1
 * registers.  Each transfer uses four 8-KB linked buffers (up to 32 KB), and
 * two complete descriptor/data sets are rotated between transfers.  The
 * region is MPU non-cacheable PSRAM, so the SDMMC AHB master never observes a
 * stale D-cache line. */
static uint8_t s_sdio_dma_buffer[WHD_SDIO_DMA_PAIR_COUNT]
                              [WHD_SDIO_DMA_NODE_COUNT]
                              [WHD_SDIO_DMA_NODE_SIZE]
    __attribute__((section(".wifi_sdio_nc"), aligned(32), used));
static SDMMC_DMALinkNodeTypeDef s_sdio_dma_nodes[WHD_SDIO_DMA_PAIR_COUNT]
                                                 [WHD_SDIO_DMA_NODE_COUNT]
    __attribute__((section(".wifi_sdio_nc"), aligned(32), used));

static rt_mutex_t s_sdio_mutex;
static bool s_sdio_initialized;
static bool s_sdio_card_ready;
static bool s_sdio_host_4bit;
static bool s_sdio_high_speed_requested;
static uint32_t s_sdio_clock_hz;
static uint16_t s_sdio_block_size = WHD_SDIO_BLOCK_SIZE;
static uint32_t s_sdio_rca;
static uint32_t s_sdio_dma_pair;
static volatile uint32_t s_sdio_idma_direct_transfers;
static volatile uint32_t s_sdio_idma_linked_transfers;
static volatile uint32_t s_sdio_idma_direct_bytes;
static volatile uint32_t s_sdio_idma_linked_bytes;
static volatile uint32_t s_sdio_idma_padding_bytes;
static volatile uint32_t s_sdio_idma_errors;
static volatile uint32_t s_sdio_spin_commands, s_sdio_spin_data;
static volatile uint32_t s_sdio_poll_yields;
static volatile uint32_t s_sdio_timeout_count;
static volatile uint32_t s_sdio_timeout_abort_attempts;
static volatile uint32_t s_sdio_timeout_recoveries;
static volatile uint32_t s_sdio_timeout_recovery_failures;
static volatile uint32_t s_sdio_last_error_code;
static volatile uint32_t s_sdio_last_error_status;
static volatile uint32_t s_sdio_last_error_argument;
static volatile uint32_t s_sdio_last_error_data;
static volatile uint32_t s_sdio_last_error_length;
static volatile uint32_t s_sdio_last_error_transfer_length;
static volatile uint32_t s_sdio_last_error_response;
static volatile uint32_t s_sdio_last_error_direct;
static volatile uint32_t s_sdio_last_error_dcount;
static volatile uint32_t s_sdio_last_error_idma_control;
static volatile uint32_t s_sdio_last_error_idma_base;
static volatile uint32_t s_sdio_last_error_idma_size;
static volatile uint32_t s_sdio_last_error_idma_list;
static volatile uint32_t s_sdio_last_error_idma_link;
static volatile uint32_t s_sdio_last_error_dctrl;
static volatile uint32_t s_sdio_last_error_cmd;
static volatile uint32_t s_sdio_last_abort_result =
    WHD_STM32_SDIO_ABORT_NOT_ATTEMPTED;
static volatile uint32_t s_sdio_last_abort_function;
static volatile uint32_t s_sdio_last_abort_attempted;
static cyhal_sdio_irq_handler_t s_sdio_irq_handler;
static void *s_sdio_irq_arg;

typedef struct
{
    bool record;
    uint32_t start_cycles;
    uint32_t acquired_cycles;
} whd_sdio_mutex_timing_t;

typedef struct
{
    bool record;
    bool write;
    uint32_t start_cycles;
    uint32_t end_cycles;
} whd_sdio_cmd53_timing_t;

static uint32_t whd_sdio_read_cycles(void *context)
{
    (void)context;
    return DWT->CYCCNT;
}

#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
static void whd_sdio_profiled_mutex_take(whd_sdio_mutex_timing_t *timing)
{
    timing->record = !startup_radio_active();
    timing->start_cycles = timing->record ? usb_perf_now() : 0U;
    timing->acquired_cycles = 0U;
    (void)rt_mutex_take(s_sdio_mutex, RT_WAITING_FOREVER);
    if (timing->record)
    {
        timing->acquired_cycles = usb_perf_now();
    }
}

static void whd_sdio_profiled_mutex_release(
    whd_sdio_mutex_timing_t *timing)
{
    rt_mutex_release(s_sdio_mutex);
    if (timing->record)
    {
        usb_perf_record(USB_PERF_SDIO_MUTEX_WAIT, timing->start_cycles,
                        timing->acquired_cycles);
    }
}

#define WHD_SDIO_MUTEX_TAKE(timing) \
    whd_sdio_profiled_mutex_take(&(timing))
#define WHD_SDIO_MUTEX_RELEASE(timing) \
    whd_sdio_profiled_mutex_release(&(timing))
#else
#define WHD_SDIO_MUTEX_TAKE(timing) \
    do { (void)sizeof(timing); \
         (void)rt_mutex_take(s_sdio_mutex, RT_WAITING_FOREVER); } while (0)
#define WHD_SDIO_MUTEX_RELEASE(timing) \
    do { (void)sizeof(timing); rt_mutex_release(s_sdio_mutex); } while (0)
#endif

static void whd_sdio_poll_begin(startup_sdio_poll_gate_t *gate,
                              uint32_t window_us)
{
    /* One bounded, interruptible initial window; never restart after sleeping.
     * Missing/disabled DWT preserves the original sleep path. */
    /* Step38 A/B candidate: retain the bounded startup window and make the
     * same short initial spin available during steady-state SDIO polls. The
     * existing 1 ms sleep fallback still applies after the window. */
    startup_sdio_cycle_reader_t reader =
        (DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) ? whd_sdio_read_cycles : NULL;
    startup_sdio_poll_gate_begin(gate, SystemCoreClock, window_us, reader, NULL);
    if (gate->window_open) {
        if (window_us == STARTUP_SDIO_COMMAND_SPIN_US) s_sdio_spin_commands++;
        else s_sdio_spin_data++;
    }
}

static void whd_sdio_poll_yield(startup_sdio_poll_gate_t *gate)
{
    if (startup_sdio_poll_gate_should_yield(gate)) {
        s_sdio_poll_yields++;
        rt_thread_mdelay(1U);
    }
}

static bool whd_sdio_deadline_expired(rt_tick_t start, uint32_t timeout_ms)
{
    rt_tick_t timeout = rt_tick_from_millisecond(timeout_ms);

    return (rt_int32_t)(rt_tick_get() - start) >= (rt_int32_t)timeout;
}

static void whd_sdio_clear_flags(void)
{
    SDMMC2->ICR = SDMMC_STATIC_FLAGS;
}

static uint32_t whd_sdio_idma_size_encode(uint32_t bytes)
{
    uint32_t units = (bytes + 31U) / 32U;

    return units << SDMMC_IDMABSIZE_IDMABNDT_Pos;
}

static bool whd_sdio_prepare_idma_linked_list(uint32_t pair,
                                              uint32_t transfer_length)
{
    SDMMC_DMALinkNodeTypeDef *nodes;
    uint32_t remaining = transfer_length;
    uint32_t index;

    if (pair >= WHD_SDIO_DMA_PAIR_COUNT || transfer_length == 0U ||
        transfer_length > WHD_SDIO_DMA_BUFFER_SIZE)
    {
        return false;
    }
    nodes = s_sdio_dma_nodes[pair];

    for (index = 0U; index < WHD_SDIO_DMA_NODE_COUNT; index++)
    {
        uint32_t node_bytes;
        uint32_t offset;

        if (remaining == 0U)
        {
            nodes[index].IDMABASER = (uint32_t)(uintptr_t)
                                      s_sdio_dma_buffer[pair][index];
            nodes[index].IDMABSIZE = whd_sdio_idma_size_encode(32U);
            nodes[index].IDMALAR = SDMMC_IDMALAR_ULS |
                                   SDMMC_IDMALAR_ABR;
            continue;
        }

        node_bytes = remaining > WHD_SDIO_DMA_NODE_SIZE ?
                     WHD_SDIO_DMA_NODE_SIZE : remaining;
        node_bytes = (node_bytes + 31U) & ~31U;
        nodes[index].IDMABASER = (uint32_t)(uintptr_t)
                                 s_sdio_dma_buffer[pair][index];
        nodes[index].IDMABSIZE = whd_sdio_idma_size_encode(node_bytes);
        nodes[index].IDMALAR = SDMMC_IDMALAR_ULS |
                               SDMMC_IDMALAR_ABR;
        remaining = remaining > WHD_SDIO_DMA_NODE_SIZE ?
                    remaining - WHD_SDIO_DMA_NODE_SIZE : 0U;

        if (remaining != 0U && index + 1U < WHD_SDIO_DMA_NODE_COUNT)
        {
            offset = (index + 1U) * sizeof(SDMMC_DMALinkNodeTypeDef);
            nodes[index].IDMALAR |= SDMMC_IDMALAR_ULA |
                                    (offset & SDMMC_IDMALAR_IDMALA);
        }
    }

    if (remaining != 0U)
    {
        return false;
    }

    /* The first node is programmed directly.  Subsequent nodes are loaded
     * from IDMABAR + IDMALA when ULA is set, as specified for H7R/S. */
    SDMMC2->IDMABAR = (uint32_t)(uintptr_t)&nodes[0];
    SDMMC2->IDMABASER = nodes[0].IDMABASER;
    SDMMC2->IDMABSIZE = nodes[0].IDMABSIZE;
    SDMMC2->IDMALAR = nodes[0].IDMALAR;
    __DMB();
    SDMMC2->IDMACTRL = SDMMC_ENABLE_IDMA_DOUBLE_BUFF0;
    __DSB();
    return true;
}

static void whd_sdio_prepare_idma_single_buffer(const uint8_t *data)
{
    /* The caller has already verified that this is a 4-byte aligned,
     * project-owned WHD buffer.  Cacheable pool buffers are maintained around
     * the transfer by whd_sdio_bulk_transfer_locked(). */
    SDMMC2->IDMABAR = 0U;
    SDMMC2->IDMALAR = 0U;
    SDMMC2->IDMABSIZE = 0U;
    __DMB();
    /* Keep the H7R/S single-buffer programming order used by ST's HAL:
     * enable IDMA first, then publish the buffer base address. */
    SDMMC2->IDMACTRL = SDMMC_ENABLE_IDMA_SINGLE_BUFF;
    SDMMC2->IDMABASER = (uint32_t)(uintptr_t)data;
    __DSB();
}

static void whd_sdio_cache_clean_invalidate(const uint8_t *data,
                                            uint32_t length)
{
    (void)rt_dma_sync_out_data(rt_dma_h7rs_device(), (void *)data, length,
                               RT_NULL, 0U);
    (void)rt_dma_sync_in_data(rt_dma_h7rs_device(), (void *)data, length,
                              (rt_ubase_t)(uintptr_t)data, 0U);
}

static void whd_sdio_cache_invalidate(const uint8_t *data, uint32_t length)
{
    (void)rt_dma_sync_in_data(rt_dma_h7rs_device(), (void *)data, length,
                              (rt_ubase_t)(uintptr_t)data, 0U);
}

static uint32_t whd_sdio_clock_divider(uint32_t source_hz, uint32_t target_hz)
{
    uint32_t div;

    if (source_hz == 0U || target_hz == 0U)
    {
        return 0x3ffU;
    }
    /* H7RS SDMMC_CK is source/(2*CLKDIV) for a non-zero divider.  Round up
     * the divider so the generated clock does not exceed the requested rate.
     * This is the same formula used by ST's HAL SD/SDIO drivers. */
    div = (source_hz + (2U * target_hz) - 1U) / (2U * target_hz);
    if (div == 0U)
    {
        div = 1U;
    }
    return div > 0x3ffU ? 0x3ffU : div;
}

static bool whd_sdio_apply_host(uint32_t clock_hz, bool host_4bit)
{
    SDMMC_InitTypeDef init;
    uint32_t source_hz;

    source_hz = HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_SDMMC12);
    /* clock_hz == 0 means “keep the final CubeMX ClockDiv”.  It is used
     * when the card has accepted CMD52 BUS_IF_CONTROL and the host changes
     * from the temporary 400 kHz/1-bit profile to the generated 4-bit
     * profile. */
    if (source_hz == 0U)
    {
        return false;
    }

    /* hsdio2.Init is populated by CubeMX before RT-Thread starts.  Reuse it
     * as the canonical static profile; only the SDIO-required temporary
     * startup clock and bus width are overridden by the WHD state machine.
     * A zero clock_hz means “use the CubeMX final ClockDiv”. */
    init = hsdio2.Init;
    init.BusWide = host_4bit ? SDMMC_BUS_WIDE_4B : SDMMC_BUS_WIDE_1B;
    if (clock_hz != 0U)
    {
        init.ClockDiv = whd_sdio_clock_divider(source_hz, clock_hz);
    }
    if (SDMMC_Init(SDMMC2, init) != HAL_OK)
    {
        return false;
    }
    s_sdio_clock_hz = init.ClockDiv == 0U ? source_hz :
                      source_hz / (2U * init.ClockDiv);
    s_sdio_host_4bit = host_4bit;
    return true;
}

static cy_rslt_t whd_sdio_wait_command(uint8_t command, bool response,
                                       bool allow_crc_error,
                                       uint32_t *response_value)
{
    rt_tick_t start = rt_tick_get();
    uint32_t status;
    startup_sdio_poll_gate_t poll;
    whd_sdio_poll_begin(&poll, STARTUP_SDIO_COMMAND_SPIN_US);

    for (;;)
    {
        status = SDMMC2->STA;
        if ((status & SDMMC_STA_CTIMEOUT) != 0U)
        {
            whd_sdio_clear_flags();
            return CYHAL_SDIO_RET_CMD_TIMEOUT;
        }
        if ((status & SDMMC_STA_CCRCFAIL) != 0U)
        {
            if (!allow_crc_error)
            {
                whd_sdio_clear_flags();
                return CYHAL_SDIO_RET_CMD_CRC_ERROR;
            }
            break;
        }
        if (response)
        {
            if ((status & SDMMC_STA_CMDREND) != 0U &&
                (status & SDMMC_STA_CPSMACT) == 0U)
            {
                break;
            }
        }
        else if ((status & SDMMC_STA_CMDSENT) != 0U)
        {
            break;
        }

        if (whd_sdio_deadline_expired(start, WHD_SDIO_CMD_TIMEOUT_MS))
        {
            whd_sdio_clear_flags();
            return CYHAL_SDIO_RET_CMD_TIMEOUT;
        }
        whd_sdio_poll_yield(&poll);
    }

    if (response)
    {
        /* CMD5 is an R4 response.  Its response does not carry the normal
         * command CRC semantics and some STM32 SDMMC revisions leave
         * RESPCMD unchanged when CCRCFAIL is asserted. */
        if ((!allow_crc_error || command != CYHAL_SDIO_CMD_IO_SEND_OP_COND) &&
            SDMMC2->RESPCMD != command)
        {
            whd_sdio_clear_flags();
            return CYHAL_SDIO_RET_CMD_IDX_ERROR;
        }
        if (response_value != NULL)
        {
            *response_value = SDMMC2->RESP1;
        }
    }
    whd_sdio_clear_flags();
    return CYHAL_SDIO_RET_NO_ERRORS;
}

static cy_rslt_t whd_sdio_send_command_locked(uint8_t command,
                                              uint32_t argument,
                                              bool response,
                                              bool allow_crc_error,
                                              uint32_t *response_value)
{
    SDMMC_CmdInitTypeDef init;

    if ((SDMMC2->STA & (SDMMC_STA_CPSMACT | SDMMC_STA_DPSMACT)) != 0U)
    {
        SDMMC2->CMD = 0U;
        SDMMC2->DCTRL = 0U;
        SDMMC2->IDMACTRL = SDMMC_DISABLE_IDMA;
        SDMMC2->DLEN = 0U;
    }
    __SDMMC_CMDTRANS_DISABLE(SDMMC2);
    whd_sdio_clear_flags();

    memset(&init, 0, sizeof(init));
    init.Argument = argument;
    init.CmdIndex = command;
    init.Response = response ? SDMMC_RESPONSE_SHORT : SDMMC_RESPONSE_NO;
    init.WaitForInterrupt = SDMMC_WAIT_NO;
    init.CPSM = SDMMC_CPSM_ENABLE;
    (void)SDMMC_SendCommand(SDMMC2, &init);
    return whd_sdio_wait_command(command, response, allow_crc_error,
                                 response_value);
}

static uint32_t whd_sdio_recovery_read_register(
    void *context, whd_stm32_sdio_recovery_register_t reg)
{
    (void)context;
    switch (reg)
    {
        case WHD_STM32_SDIO_REG_STA:       return SDMMC2->STA;
        case WHD_STM32_SDIO_REG_DCOUNT:    return SDMMC2->DCOUNT;
        case WHD_STM32_SDIO_REG_IDMACTRL:  return SDMMC2->IDMACTRL;
        case WHD_STM32_SDIO_REG_IDMABASER: return SDMMC2->IDMABASER;
        case WHD_STM32_SDIO_REG_IDMABSIZE: return SDMMC2->IDMABSIZE;
        case WHD_STM32_SDIO_REG_IDMALAR:   return SDMMC2->IDMALAR;
        case WHD_STM32_SDIO_REG_DCTRL:     return SDMMC2->DCTRL;
        case WHD_STM32_SDIO_REG_CMD:       return SDMMC2->CMD;
        case WHD_STM32_SDIO_REG_DLEN:      return SDMMC2->DLEN;
        case WHD_STM32_SDIO_REG_DTIMER:    return SDMMC2->DTIMER;
        case WHD_STM32_SDIO_REG_IDMABAR:   return SDMMC2->IDMABAR;
        default:                           return 0U;
    }
}

static void whd_sdio_recovery_write_register(
    void *context, whd_stm32_sdio_recovery_register_t reg, uint32_t value)
{
    (void)context;
    switch (reg)
    {
        case WHD_STM32_SDIO_REG_CMD:       SDMMC2->CMD = value; break;
        case WHD_STM32_SDIO_REG_DCTRL:     SDMMC2->DCTRL = value; break;
        case WHD_STM32_SDIO_REG_IDMACTRL:  SDMMC2->IDMACTRL = value; break;
        case WHD_STM32_SDIO_REG_DLEN:      SDMMC2->DLEN = value; break;
        case WHD_STM32_SDIO_REG_DTIMER:    SDMMC2->DTIMER = value; break;
        case WHD_STM32_SDIO_REG_IDMABAR:   SDMMC2->IDMABAR = value; break;
        case WHD_STM32_SDIO_REG_IDMABASER: SDMMC2->IDMABASER = value; break;
        case WHD_STM32_SDIO_REG_IDMALAR:   SDMMC2->IDMALAR = value; break;
        case WHD_STM32_SDIO_REG_IDMABSIZE: SDMMC2->IDMABSIZE = value; break;
        default: break;
    }
}

static void whd_sdio_recovery_clear_flags(void *context)
{
    (void)context;
    whd_sdio_clear_flags();
}

static void whd_sdio_recovery_barrier(void *context)
{
    (void)context;
    __DSB();
}

static void whd_sdio_recovery_wait_one_ms(void *context)
{
    (void)context;
    rt_thread_mdelay(1U);
}

static uint32_t whd_sdio_recovery_abort_io_function(void *context,
                                                     uint8_t function)
{
    uint32_t argument;
    uint32_t response = 0U;
    cy_rslt_t result;

    (void)context;

    /* ST's HAL_SDIO_AbortIOFunction writes the function number to CCCR
     * IO_ABORT (CCCR address 0x06) with CMD52. Use this port's 100-ms command
     * waiter for the same single abort so recovery stays tightly bounded. */
    argument = (1UL << 31U) | (SDMMC_SDIO_RAW << 27U) |
               (SDMMC_SDIO_CCCR4_SD_BYTE2 << 9U) |
               ((uint32_t)function & 0x07U);
    result = whd_sdio_send_command_locked(CYHAL_SDIO_CMD_IO_RW_DIRECT,
                                           argument, true, false, &response);
    if (result != CYHAL_SDIO_RET_NO_ERRORS)
    {
        return result;
    }
    if ((response & (SDMMC_SDIO_R5_ERRORBITS & ~WHD_SDIO_R5_FUNCTION_NUMBER)) != 0U)
    {
        return CYHAL_SDIO_RET_RESP_FLAG_ERROR;
    }
    if ((response & WHD_SDIO_R5_FUNCTION_NUMBER) != 0U)
    {
        return CYHAL_SDIO_FUNC_NOT_SUPPORTED;
    }
    if ((response & WHD_SDIO_R5_OUT_OF_RANGE) != 0U)
    {
        return CYHAL_SDIO_RET_RESP_FLAG_ERROR;
    }
    return CYHAL_SDIO_RET_NO_ERRORS;
}

static const whd_stm32_sdio_recovery_ops_t s_sdio_recovery_ops =
{
    NULL,
    whd_sdio_recovery_read_register,
    whd_sdio_recovery_write_register,
    whd_sdio_recovery_clear_flags,
    whd_sdio_recovery_barrier,
    whd_sdio_recovery_wait_one_ms,
    whd_sdio_recovery_abort_io_function
};

static cy_rslt_t whd_sdio_card_init_locked(void)
{
    cy_rslt_t result;
    uint32_t response = 0U;

    result = whd_sdio_send_command_locked(CYHAL_SDIO_CMD_GO_IDLE_STATE, 0U,
                                          false, false, NULL);
    if (result != CYHAL_SDIO_RET_NO_ERRORS)
    {
        return result;
    }

    /* CMD5 is an R4 response.  On STM32 SDMMC revisions used by H7RS the
     * response can set CCRCFAIL although RESP1 is valid; accept that exact
     * condition, as the official RT-Thread/Infineon ports do. */
    result = whd_sdio_send_command_locked(CYHAL_SDIO_CMD_IO_SEND_OP_COND,
                                          0U, true, true, &response);
    if (result != CYHAL_SDIO_RET_NO_ERRORS &&
        result != CYHAL_SDIO_RET_CMD_CRC_ERROR)
    {
        rt_kprintf("[WHD] SDIO CMD5 returned 0x%02lX, continue probe\r\n",
                   (unsigned long)result);
    }

    result = whd_sdio_send_command_locked(
        CYHAL_SDIO_CMD_SEND_RELATIVE_ADDR, 0U, true, false, &response);
    if (result != CYHAL_SDIO_RET_NO_ERRORS)
    {
        return result;
    }
    s_sdio_rca = response & 0xffff0000UL;

    result = whd_sdio_send_command_locked(CYHAL_SDIO_CMD_SELECT_CARD,
                                          s_sdio_rca, true, false,
                                          &response);
    if (result != CYHAL_SDIO_RET_NO_ERRORS)
    {
        return result;
    }
    s_sdio_card_ready = true;
    return CYHAL_SDIO_RET_NO_ERRORS;
}

static void whd_sdio_update_bus_after_cmd52(uint32_t argument)
{
    uint32_t rw = (argument >> 31) & 1U;
    uint32_t function = (argument >> 28) & 7U;
    uint32_t address = (argument >> 9) & 0x1ffffU;
    uint32_t value = argument & 0xffU;

    /* CCCR BUS_IF_CONTROL, data width = 4-bit.  This is intentionally done
     * only after the CMD52 has completed while the card is still in 1-bit
     * mode, matching the SDIO specification.  If high-speed mode was
     * requested, keep the clock at the startup rate until WHD writes EHS. */
    if (rw != 0U && function == 0U && address == 0x07U &&
        (value & 0x03U) == 0x02U && !s_sdio_host_4bit)
    {
        uint32_t clock_hz = s_sdio_high_speed_requested ?
                            WHD_SDIO_CLOCK_INIT_HZ : 0U;
        if (!whd_sdio_apply_host(clock_hz, true))
        {
            rt_kprintf("[WHD] failed to switch SDMMC2 to 4-bit mode\r\n");
        }
        else if (s_sdio_high_speed_requested)
        {
            rt_kprintf("[WHD] SDMMC2 bus width: 4-bit, waiting for SDIO EHS\r\n");
        }
    }

    /* CCCR SPEED_CONTROL, EHS=1.  This CMD52 is the protocol-defined point
     * at which a SDIO 2.0 card enters high-speed clocking.  Only then may the
     * host move from the safe startup clock to CubeMX's final ClockDiv. */
    if (rw != 0U && function == 0U && address == 0x13U &&
        (value & 0x02U) != 0U && s_sdio_high_speed_requested)
    {
        if (!whd_sdio_apply_host(0U, true))
        {
            rt_kprintf("[WHD] failed to switch SDMMC2 to CubeMX high-speed clock\r\n");
        }
        else
        {
            rt_kprintf("[WHD] SDMMC2 high-speed enabled: %lu Hz, 4-bit\r\n",
                       (unsigned long)s_sdio_clock_hz);
        }
    }
}

/* Called by the project-level WHD setup before the SDIO card is attached. */
void whd_stm32_sdio_set_high_speed(rt_bool_t enabled)
{
    s_sdio_high_speed_requested = enabled != RT_FALSE;
}

/** Initialize the SDMMC peripheral without invoking CubeMX SD-card init. */
cy_rslt_t cyhal_sdio_init(cyhal_sdio_t *obj)
{
    cy_rslt_t result;
    whd_sdio_mutex_timing_t mutex_timing;

    if (obj == NULL)
    {
        return CYHAL_SDIO_RSLT_ERR_BAD_PARAM;
    }
    if (s_sdio_initialized)
    {
        return CYHAL_SDIO_RET_NO_ERRORS;
    }

    s_sdio_idma_direct_transfers = 0U;
    s_sdio_idma_linked_transfers = 0U;
    s_sdio_idma_direct_bytes = 0U;
    s_sdio_idma_linked_bytes = 0U;
    s_sdio_idma_padding_bytes = 0U;
    s_sdio_idma_errors = 0U;
    s_sdio_spin_commands = s_sdio_spin_data = s_sdio_poll_yields = 0U;
    s_sdio_timeout_count = 0U;
    s_sdio_timeout_abort_attempts = 0U;
    s_sdio_timeout_recoveries = 0U;
    s_sdio_timeout_recovery_failures = 0U;
    s_sdio_last_error_code = 0U;
    s_sdio_last_error_status = 0U;
    s_sdio_last_error_argument = 0U;
    s_sdio_last_error_data = 0U;
    s_sdio_last_error_length = 0U;
    s_sdio_last_error_transfer_length = 0U;
    s_sdio_last_error_response = 0U;
    s_sdio_last_error_direct = 0U;
    s_sdio_last_error_dcount = 0U;
    s_sdio_last_error_idma_control = 0U;
    s_sdio_last_error_idma_base = 0U;
    s_sdio_last_error_idma_size = 0U;
    s_sdio_last_error_idma_list = 0U;
    s_sdio_last_error_idma_link = 0U;
    s_sdio_last_error_dctrl = 0U;
    s_sdio_last_error_cmd = 0U;
    s_sdio_last_abort_result = WHD_STM32_SDIO_ABORT_NOT_ATTEMPTED;
    s_sdio_last_abort_function = 0U;
    s_sdio_last_abort_attempted = 0U;
    s_sdio_dma_pair = 0U;

    s_sdio_mutex = rt_mutex_create("whd_sd", RT_IPC_FLAG_PRIO);
    if (s_sdio_mutex == NULL)
    {
        return CYHAL_SDIO_RET_NO_SP_ERRORS;
    }
    memset(obj, 0, sizeof(*obj));

    /* WL_REG_ON is supplied by the board port.  The module requires a real low
     * interval on warm reset, not merely a host-controller reset. */
    rt_base_t reg_on = rt_pin_get(CYBSP_REG_ON_PIN_NAME);
    rt_pin_mode(reg_on, PIN_MODE_OUTPUT);
    rt_pin_write(reg_on, PIN_LOW);
    rt_thread_mdelay(100U);

    hsdio2.Instance = SDMMC2;
    HAL_SDIO_MspInit(&hsdio2);
    HAL_NVIC_DisableIRQ(SDMMC2_IRQn);
    NVIC_ClearPendingIRQ(SDMMC2_IRQn);

    __HAL_RCC_SDMMC2_FORCE_RESET();
    __DSB();
    __HAL_RCC_SDMMC2_RELEASE_RESET();
    SDMMC2->POWER = 0U;
    SDMMC2->CMD = 0U;
    SDMMC2->DCTRL = 0U;
    SDMMC2->IDMACTRL = SDMMC_DISABLE_IDMA;
    SDMMC2->DLEN = 0U;
    whd_sdio_clear_flags();

    if (!whd_sdio_apply_host(WHD_SDIO_CLOCK_INIT_HZ, false))
    {
        return CYHAL_SDIO_RET_CMD_TIMEOUT;
    }
    SDMMC2->POWER = SDMMC_POWER_PWRCTRL;
    rt_pin_write(reg_on, PIN_HIGH);
    rt_thread_mdelay(160U);

    WHD_SDIO_MUTEX_TAKE(mutex_timing);
    result = whd_sdio_card_init_locked();
    WHD_SDIO_MUTEX_RELEASE(mutex_timing);
    if (result != CYHAL_SDIO_RET_NO_ERRORS)
    {
        rt_kprintf("[WHD] SDMMC2 card probe failed: 0x%02lX\r\n",
                   (unsigned long)result);
        return result;
    }

    s_sdio_initialized = true;
    rt_kprintf("[WHD] SDMMC2 SDIO ready: %lu Hz, 1-bit\r\n",
               (unsigned long)s_sdio_clock_hz);
    return CYHAL_SDIO_RET_NO_ERRORS;
}

void cyhal_sdio_free(cyhal_sdio_t *obj)
{
    whd_sdio_mutex_timing_t mutex_timing;

    (void)obj;
    if (!s_sdio_initialized || s_sdio_mutex == NULL)
    {
        return;
    }
    WHD_SDIO_MUTEX_TAKE(mutex_timing);
    SDMMC2->DCTRL = 0U;
    SDMMC2->IDMACTRL = SDMMC_DISABLE_IDMA;
    SDMMC2->DLEN = 0U;
    SDMMC2->POWER = 0U;
    whd_sdio_clear_flags();
    WHD_SDIO_MUTEX_RELEASE(mutex_timing);
    rt_pin_write(rt_pin_get(CYBSP_REG_ON_PIN_NAME), PIN_LOW);
    s_sdio_initialized = false;
    s_sdio_card_ready = false;
    s_sdio_host_4bit = false;
    s_sdio_high_speed_requested = false;
    s_sdio_dma_pair = 0U;
    s_sdio_irq_handler = NULL;
    s_sdio_irq_arg = NULL;
}

void whd_stm32_sdio_get_stats(whd_stm32_sdio_stats_t *stats)
{
    if (stats == NULL)
    {
        return;
    }
    stats->clock_hz = s_sdio_clock_hz;
    stats->bus_width = s_sdio_host_4bit ? 4U : 1U;
    stats->direct_transfers = s_sdio_idma_direct_transfers;
    stats->linked_transfers = s_sdio_idma_linked_transfers;
    stats->direct_bytes = s_sdio_idma_direct_bytes;
    stats->linked_bytes = s_sdio_idma_linked_bytes;
    stats->padding_bytes = s_sdio_idma_padding_bytes;
    stats->errors = s_sdio_idma_errors;
    stats->spin_commands = s_sdio_spin_commands;
    stats->spin_data = s_sdio_spin_data;
    stats->poll_yields = s_sdio_poll_yields;
    stats->last_error_code = s_sdio_last_error_code;
    stats->last_error_status = s_sdio_last_error_status;
    stats->last_error_argument = s_sdio_last_error_argument;
    stats->last_error_data = s_sdio_last_error_data;
    stats->last_error_length = s_sdio_last_error_length;
    stats->last_error_transfer_length = s_sdio_last_error_transfer_length;
    stats->last_error_response = s_sdio_last_error_response;
    stats->last_error_direct = s_sdio_last_error_direct;
    stats->timeouts = s_sdio_timeout_count;
    stats->timeout_abort_attempts = s_sdio_timeout_abort_attempts;
    stats->timeout_recoveries = s_sdio_timeout_recoveries;
    stats->timeout_recovery_failures = s_sdio_timeout_recovery_failures;
    stats->last_error_dcount = s_sdio_last_error_dcount;
    stats->last_error_idma_control = s_sdio_last_error_idma_control;
    stats->last_error_idma_base = s_sdio_last_error_idma_base;
    stats->last_error_idma_size = s_sdio_last_error_idma_size;
    stats->last_error_idma_list = s_sdio_last_error_idma_list;
    stats->last_error_idma_link = s_sdio_last_error_idma_link;
    stats->last_error_dctrl = s_sdio_last_error_dctrl;
    stats->last_error_cmd = s_sdio_last_error_cmd;
    stats->last_abort_result = s_sdio_last_abort_result;
    stats->last_abort_function = (uint8_t)s_sdio_last_abort_function;
    stats->last_abort_attempted = (uint8_t)s_sdio_last_abort_attempted;
}

cy_rslt_t cyhal_sdio_configure(cyhal_sdio_t *obj,
                               const cyhal_sdio_cfg_t *config)
{
    bool result;
    whd_sdio_mutex_timing_t mutex_timing;

    (void)obj;
    if (config == NULL || s_sdio_mutex == NULL || config->frequencyhal_hz == 0U)
    {
        return CYHAL_SDIO_RSLT_ERR_BAD_PARAM;
    }
    if (config->block_size != 0U)
    {
        s_sdio_block_size = config->block_size;
    }
    WHD_SDIO_MUTEX_TAKE(mutex_timing);
    result = whd_sdio_apply_host(config->frequencyhal_hz, s_sdio_host_4bit);
    WHD_SDIO_MUTEX_RELEASE(mutex_timing);
    return result ? CYHAL_SDIO_RET_NO_ERRORS : CYHAL_SDIO_RSLT_ERR_CLOCK;
}

cy_rslt_t cyhal_sdio_send_cmd(const cyhal_sdio_t *obj,
                              cyhal_transfer_t direction,
                              cyhal_sdio_command_t command,
                              uint32_t argument, uint32_t *response)
{
    cy_rslt_t result;
    uint32_t response_value = 0U;
    whd_sdio_mutex_timing_t mutex_timing;

    (void)obj;
    (void)direction;
    if (!s_sdio_initialized || !s_sdio_card_ready || s_sdio_mutex == NULL)
    {
        return CYHAL_SDIO_RET_NO_SP_ERRORS;
    }
    WHD_SDIO_MUTEX_TAKE(mutex_timing);
    result = whd_sdio_send_command_locked((uint8_t)command, argument, true,
                                          false, &response_value);
    if (result == CYHAL_SDIO_RET_NO_ERRORS &&
        command == CYHAL_SDIO_CMD_IO_RW_DIRECT)
    {
        whd_sdio_update_bus_after_cmd52(argument);
    }
    WHD_SDIO_MUTEX_RELEASE(mutex_timing);
    if (result != CYHAL_SDIO_RET_NO_ERRORS)
    {
        return result;
    }

    if (command == CYHAL_SDIO_CMD_IO_RW_DIRECT)
    {
        if ((response_value & WHD_SDIO_R5_ERROR) != 0U)
        {
            return CYHAL_SDIO_RET_RESP_FLAG_ERROR;
        }
        if ((response_value & WHD_SDIO_R5_FUNCTION_NUMBER) != 0U)
        {
            return CYHAL_SDIO_FUNC_NOT_SUPPORTED;
        }
        if ((response_value & WHD_SDIO_R5_OUT_OF_RANGE) != 0U)
        {
            return CYHAL_SDIO_RET_RESP_FLAG_ERROR;
        }
    }
    if (response != NULL)
    {
        *response = response_value;
    }
    return CYHAL_SDIO_RET_NO_ERRORS;
}

static cy_rslt_t whd_sdio_bulk_transfer_locked(cyhal_transfer_t direction,
                                               uint32_t argument,
                                               const uint32_t *data,
                                               uint16_t length,
                                               uint32_t *response,
                                               whd_sdio_cmd53_timing_t *timing)
{
    SDMMC_DataInitTypeDef data_init;
    SDMMC_CmdInitTypeDef command_init;
    uint32_t block_mode;
    uint32_t count;
    uint32_t transfer_length;
    uint32_t status;
    uint32_t response_value = 0U;
    uint32_t pair;
    uint32_t offset;
    whd_stm32_sdio_recovery_evidence_t recovery_evidence = {0};
    whd_stm32_sdio_recovery_result_t recovery_result;
    uint8_t *byte_data = (uint8_t *)(uintptr_t)data;
    rt_tick_t start;
    startup_sdio_poll_gate_t poll;
    bool write = direction == CYHAL_WRITE;
    bool direct_dma;
    bool dma_pool;
    bool direct_cacheable;
    cy_rslt_t result = CYHAL_SDIO_RET_NO_ERRORS;

#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    timing->record = false;
    timing->write = write;
    timing->start_cycles = 0U;
    timing->end_cycles = 0U;
#else
    (void)timing;
#endif

    if (data == NULL || length == 0U)
    {
        return CYHAL_SDIO_RSLT_ERR_BAD_PARAM;
    }

    block_mode = (argument >> 27) & 1U;
    count = argument & 0x1ffU;
    if (count == 0U)
    {
        count = 512U;
    }
    transfer_length = block_mode ? count * (uint32_t)s_sdio_block_size : count;
    if (transfer_length < length || transfer_length > WHD_SDIO_DMA_BUFFER_SIZE)
    {
        return CYHAL_SDIO_RSLT_ERR_BAD_PARAM;
    }

    dma_pool = whd_mongoose_buffer_is_sdio_dma_cacheable(
        byte_data, transfer_length);
    direct_dma = (WHD_SDIO_DIRECT_ENABLE != 0U) && !write &&
                 (WHD_SDIO_DIRECT_RX_ENABLE != 0U) &&
                 (((uintptr_t)byte_data & (WHD_SDIO_DIRECT_ALIGNMENT - 1U)) == 0U) &&
                 dma_pool &&
                 whd_mongoose_buffer_is_sdio_dma_safe(byte_data, length,
                                                       transfer_length);
    direct_cacheable = direct_dma && dma_pool;
    pair = 0U;
    if (direct_dma)
    {
        /* Direct mode is RX-only.  Any rounded RX block remains inside the
         * dedicated slot and is ignored by WHD's visible packet length. */
    }
    else
    {
        pair = s_sdio_dma_pair++ % WHD_SDIO_DMA_PAIR_COUNT;
        if (write)
        {
            uint32_t data_remaining = length;
            uint32_t transfer_remaining = transfer_length;

            for (uint32_t index = 0U;
                 index < WHD_SDIO_DMA_NODE_COUNT && transfer_remaining != 0U;
                 index++)
            {
                uint32_t node_transfer = transfer_remaining >
                                         WHD_SDIO_DMA_NODE_SIZE ?
                                         WHD_SDIO_DMA_NODE_SIZE :
                                         transfer_remaining;
                uint32_t node_copy = data_remaining > node_transfer ?
                                     node_transfer : data_remaining;

                if (node_copy != 0U)
                {
                    memcpy(s_sdio_dma_buffer[pair][index],
                           (const uint8_t *)data + (length - data_remaining),
                           node_copy);
                }
                if (node_copy < node_transfer)
                {
                    memset(s_sdio_dma_buffer[pair][index] + node_copy, 0,
                           node_transfer - node_copy);
                }
                data_remaining -= node_copy;
                transfer_remaining -= node_transfer;
            }
        }
    }

    if (direct_cacheable)
    {
        /* Preserve CPU-written WHD metadata sharing the first D-cache line,
         * then invalidate it so SDMMC2 becomes the sole writer. */
        whd_sdio_cache_clean_invalidate(byte_data, transfer_length);
    }

    SDMMC2->DCTRL = 0U;
    SDMMC2->IDMACTRL = SDMMC_DISABLE_IDMA;
    SDMMC2->DLEN = 0U;
    whd_sdio_clear_flags();

    memset(&data_init, 0, sizeof(data_init));
    data_init.DataTimeOut = SDMMC_DATATIMEOUT;
    data_init.DataLength = transfer_length;
    data_init.DataBlockSize = block_mode ? SDMMC_DATABLOCK_SIZE_64B :
                                           SDMMC_DATABLOCK_SIZE_1B;
    data_init.TransferDir = write ? SDMMC_TRANSFER_DIR_TO_CARD :
                                    SDMMC_TRANSFER_DIR_TO_SDMMC;
    data_init.TransferMode = block_mode ? SDMMC_TRANSFER_MODE_BLOCK :
                                           SDMMC_TRANSFER_MODE_SDIO;
    data_init.DPSM = SDMMC_DPSM_DISABLE;
    (void)SDMMC_ConfigData(SDMMC2, &data_init);
    __SDMMC_CMDTRANS_ENABLE(SDMMC2);
    if (direct_dma)
    {
        whd_sdio_prepare_idma_single_buffer(byte_data);
        s_sdio_idma_direct_transfers++;
        s_sdio_idma_direct_bytes += transfer_length;
    }
    else if (!whd_sdio_prepare_idma_linked_list(pair, transfer_length))
    {
        whd_stm32_sdio_cleanup_transfer(&s_sdio_recovery_ops, false,
                                         SDMMC_CMD_CMDTRANS,
                                         SDMMC_DCTRL_FIFORST);
        s_sdio_idma_errors++;
        s_sdio_last_error_code = CYHAL_SDIO_RSLT_ERR_BAD_PARAM;
        s_sdio_last_error_status = SDMMC2->STA;
        s_sdio_last_error_argument = argument;
        s_sdio_last_error_data = (uint32_t)(uintptr_t)byte_data;
        s_sdio_last_error_length = length;
        s_sdio_last_error_transfer_length = transfer_length;
        s_sdio_last_error_response = 0U;
        s_sdio_last_error_direct = 0U;
        return CYHAL_SDIO_RSLT_ERR_BAD_PARAM;
    }
    else
    {
        s_sdio_idma_linked_transfers++;
        s_sdio_idma_linked_bytes += transfer_length;
    }

    memset(&command_init, 0, sizeof(command_init));
    command_init.Argument = argument;
    command_init.CmdIndex = CYHAL_SDIO_CMD_IO_RW_EXTENDED;
    command_init.Response = SDMMC_RESPONSE_SHORT;
    command_init.WaitForInterrupt = SDMMC_WAIT_NO;
    command_init.CPSM = SDMMC_CPSM_ENABLE;
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    timing->record = !startup_radio_active();
    if (timing->record)
    {
        timing->start_cycles = usb_perf_now();
    }
#endif
    (void)SDMMC_SendCommand(SDMMC2, &command_init);

    start = rt_tick_get();
    whd_sdio_poll_begin(&poll, STARTUP_SDIO_DATA_SPIN_US);
    for (;;)
    {
        status = SDMMC2->STA;
        if ((status & (SDMMC_STA_CTIMEOUT | SDMMC_STA_CCRCFAIL |
                       SDMMC_STA_DCRCFAIL | SDMMC_STA_DTIMEOUT |
                       SDMMC_STA_TXUNDERR | SDMMC_STA_RXOVERR |
                       SDMMC_STA_IDMATE)) != 0U)
        {
            if ((status & SDMMC_STA_CTIMEOUT) != 0U)
            {
                result = CYHAL_SDIO_RET_CMD_TIMEOUT;
            }
            else if ((status & SDMMC_STA_CCRCFAIL) != 0U)
            {
                result = CYHAL_SDIO_RET_CMD_CRC_ERROR;
            }
            else if ((status & SDMMC_STA_DTIMEOUT) != 0U)
            {
                result = CYHAL_SDIO_RET_DAT_TIMEOUT;
            }
            else if ((status & SDMMC_STA_DCRCFAIL) != 0U)
            {
                result = CYHAL_SDIO_RET_DAT_CRC_ERROR;
            }
            else
            {
                result = CYHAL_SDIO_RET_NO_SP_ERRORS;
            }
            break;
        }
        /* DATAEND can precede response completion for a short CMD53. Do not
         * inspect RESPCMD/RESP1 until both state machines and the response
         * are complete, otherwise a fast poll reads the preceding response. */
        if (startup_sdio_transfer_finished(status,
            SDMMC_STA_DATAEND | SDMMC_STA_CMDREND,
            SDMMC_STA_DPSMACT | SDMMC_STA_CPSMACT))
        {
            if (SDMMC2->RESPCMD != CYHAL_SDIO_CMD_IO_RW_EXTENDED)
            {
                result = CYHAL_SDIO_RET_CMD_IDX_ERROR;
            }
            else
            {
                response_value = SDMMC2->RESP1;
            }
            break;
        }
        if (whd_sdio_deadline_expired(start, WHD_SDIO_DATA_TIMEOUT_MS))
        {
            result = CYHAL_SDIO_RET_DAT_TIMEOUT;
            break;
        }
        whd_sdio_poll_yield(&poll);
    }
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    if (timing->record)
    {
        timing->end_cycles = usb_perf_now();
    }
#endif

    recovery_result = whd_stm32_sdio_finish_transfer(
        &s_sdio_recovery_ops, result == CYHAL_SDIO_RET_DAT_TIMEOUT,
        argument, SDMMC_STA_CPSMACT | SDMMC_STA_DPSMACT,
        SDMMC_CMD_CMDTRANS, SDMMC_DCTRL_FIFORST,
        WHD_SDIO_ABORT_QUIESCE_POLLS, CYHAL_SDIO_RET_NO_ERRORS,
        &recovery_evidence);
    if (recovery_result != WHD_STM32_SDIO_RECOVERY_NOT_NEEDED)
    {
        s_sdio_timeout_count++;
        s_sdio_last_error_status = recovery_evidence.status;
        s_sdio_last_error_dcount = recovery_evidence.data_count;
        s_sdio_last_error_idma_control = recovery_evidence.idma_control;
        s_sdio_last_error_idma_base = recovery_evidence.idma_base;
        s_sdio_last_error_idma_size = recovery_evidence.idma_size;
        s_sdio_last_error_idma_list = recovery_evidence.idma_list;
        s_sdio_last_error_idma_link = recovery_evidence.idma_link;
        s_sdio_last_error_dctrl = recovery_evidence.data_control;
        s_sdio_last_error_cmd = recovery_evidence.command;
        s_sdio_last_abort_result = recovery_evidence.abort_result;
        s_sdio_last_abort_function = recovery_evidence.function;
        s_sdio_last_abort_attempted = recovery_evidence.abort_attempted;
        if (recovery_evidence.abort_attempted != 0U)
        {
            s_sdio_timeout_abort_attempts++;
        }
        if (recovery_result == WHD_STM32_SDIO_RECOVERY_SUCCEEDED)
        {
            s_sdio_timeout_recoveries++;
        }
        else
        {
            s_sdio_timeout_recovery_failures++;
            /* The CMD53 may have partially reached the function. Stop
             * issuing further SDIO commands until the upper WHD worker
             * performs its established WL_REG_ON power-reset/reinit path. */
            s_sdio_card_ready = false;
        }

        rt_kprintf("[SDIO_TIMEOUT] sta=0x%08lx dcount=%lu\r\n",
                   (unsigned long)recovery_evidence.status,
                   (unsigned long)recovery_evidence.data_count);
        rt_kprintf("[SDIO_TIMEOUT] idma=0x%08lx buf=0x%08lx bytes=%lu\r\n",
                   (unsigned long)recovery_evidence.idma_control,
                   (unsigned long)recovery_evidence.idma_base,
                   (unsigned long)recovery_evidence.idma_size);
        rt_kprintf("[SDIO_TIMEOUT] list=0x%08lx link=0x%08lx arg=0x%08lx fn=%u\r\n",
                   (unsigned long)recovery_evidence.idma_list,
                   (unsigned long)recovery_evidence.idma_link,
                   (unsigned long)argument,
                   (unsigned int)recovery_evidence.function);
        rt_kprintf("[SDIO_TIMEOUT] abort=%u/0x%08lx recovered=%lu failed=%lu\r\n",
                   (unsigned int)recovery_evidence.abort_attempted,
                   (unsigned long)recovery_evidence.abort_result,
                   (unsigned long)s_sdio_timeout_recoveries,
                   (unsigned long)s_sdio_timeout_recovery_failures);
    }

    if (direct_cacheable)
    {
        /* Invalidate once more after SDMMC2 has completed. */
        whd_sdio_cache_invalidate(byte_data, transfer_length);
    }

    if (result == CYHAL_SDIO_RET_NO_ERRORS)
    {
        if ((response_value & WHD_SDIO_R5_ERROR) != 0U)
        {
            result = CYHAL_SDIO_RET_RESP_FLAG_ERROR;
        }
        else if ((response_value & WHD_SDIO_R5_FUNCTION_NUMBER) != 0U)
        {
            result = CYHAL_SDIO_FUNC_NOT_SUPPORTED;
        }
        else if ((response_value & WHD_SDIO_R5_OUT_OF_RANGE) != 0U)
        {
            result = CYHAL_SDIO_RET_RESP_FLAG_ERROR;
        }
    }
    if (result == CYHAL_SDIO_RET_NO_ERRORS)
    {
        if (!write && !direct_dma)
        {
            uint32_t data_remaining = length;
            uint32_t transfer_remaining = transfer_length;

            offset = 0U;
            for (uint32_t index = 0U;
                 index < WHD_SDIO_DMA_NODE_COUNT && transfer_remaining != 0U;
                 index++)
            {
                uint32_t node_transfer = transfer_remaining >
                                         WHD_SDIO_DMA_NODE_SIZE ?
                                         WHD_SDIO_DMA_NODE_SIZE :
                                         transfer_remaining;
                uint32_t node_copy = data_remaining > node_transfer ?
                                     node_transfer : data_remaining;

                if (node_copy != 0U)
                {
                    memcpy((uint8_t *)data + offset,
                           s_sdio_dma_buffer[pair][index], node_copy);
                    offset += node_copy;
                }
                data_remaining -= node_copy;
                transfer_remaining -= node_transfer;
            }
        }
        if (response != NULL)
        {
            *response = response_value;
        }
    }
    else
    {
        s_sdio_idma_errors++;
        s_sdio_last_error_code = result;
        if (recovery_result == WHD_STM32_SDIO_RECOVERY_NOT_NEEDED)
        {
            s_sdio_last_error_status = status;
        }
        s_sdio_last_error_argument = argument;
        s_sdio_last_error_data = (uint32_t)(uintptr_t)byte_data;
        s_sdio_last_error_length = length;
        s_sdio_last_error_transfer_length = transfer_length;
        s_sdio_last_error_response = response_value;
        s_sdio_last_error_direct = direct_dma ? 1U : 0U;
    }
    /* A fast completed firmware block must not turn the bootstrap into an
     * unbroken CPU hog. Keep a bounded 2-ms cooperative opportunity for
     * large boot blocks only; small register operations stay fast. Normal
     * network traffic and already-yielded/disabled-DWT waits are unchanged. */
    if (result == CYHAL_SDIO_RET_NO_ERRORS && poll.window_open &&
        length >= 64U && startup_radio_active()) {
        s_sdio_poll_yields++;
        rt_thread_mdelay(2U);
    }
    return result;
}

cy_rslt_t cyhal_sdio_bulk_transfer(cyhal_sdio_t *obj,
                                   cyhal_transfer_t direction,
                                   uint32_t argument, const uint32_t *data,
                                   uint16_t length, uint32_t *response)
{
    cy_rslt_t result;
    whd_sdio_mutex_timing_t mutex_timing;
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    whd_sdio_cmd53_timing_t cmd53_timing = {0};
#endif

    (void)obj;
    if (!s_sdio_initialized || !s_sdio_card_ready || s_sdio_mutex == NULL)
    {
        return CYHAL_SDIO_RET_NO_SP_ERRORS;
    }
    WHD_SDIO_MUTEX_TAKE(mutex_timing);
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    result = whd_sdio_bulk_transfer_locked(direction, argument, data, length,
                                            response, &cmd53_timing);
#else
    result = whd_sdio_bulk_transfer_locked(direction, argument, data, length,
                                            response, NULL);
#endif
    WHD_SDIO_MUTEX_RELEASE(mutex_timing);
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    if (cmd53_timing.record)
    {
        usb_perf_record(cmd53_timing.write ? USB_PERF_SDIO_CMD53_WRITE :
                        USB_PERF_SDIO_CMD53_READ,
                        cmd53_timing.start_cycles, cmd53_timing.end_cycles);
    }
#endif
    return result;
}

cy_rslt_t cyhal_sdio_transfer_async(cyhal_sdio_t *obj,
                                    cyhal_transfer_t direction,
                                    uint32_t argument, const uint32_t *data,
                                    uint16_t length)
{
    /* WHD uses the synchronous path for both control and packet transfers.
     * Keeping this explicit avoids an unimplemented asynchronous operation
     * being mistaken for a successful DMA submission. */
    return cyhal_sdio_bulk_transfer(obj, direction, argument, data, length,
                                    NULL);
}

bool cyhal_sdio_is_busy(const cyhal_sdio_t *obj)
{
    (void)obj;
    return (SDMMC2->STA & SDMMC_STA_DPSMACT) != 0U;
}

cy_rslt_t cyhal_sdio_abort_async(const cyhal_sdio_t *obj)
{
    whd_sdio_mutex_timing_t mutex_timing = {0};

    (void)obj;
    if (s_sdio_mutex != NULL)
    {
        WHD_SDIO_MUTEX_TAKE(mutex_timing);
    }
    whd_stm32_sdio_cleanup_transfer(&s_sdio_recovery_ops, false,
                                     SDMMC_CMD_CMDTRANS,
                                     SDMMC_DCTRL_FIFORST);
    if (s_sdio_mutex != NULL)
    {
        WHD_SDIO_MUTEX_RELEASE(mutex_timing);
    }
    return CYHAL_SDIO_RET_NO_ERRORS;
}

void cyhal_sdio_register_irq(cyhal_sdio_t *obj,
                             cyhal_sdio_irq_handler_t handler,
                             void *handler_arg)
{
    (void)obj;
    s_sdio_irq_handler = handler;
    s_sdio_irq_arg = handler_arg;
}

void cyhal_sdio_irq_enable(cyhal_sdio_t *obj, cyhal_sdio_irq_event_t event,
                           bool enable)
{
    (void)obj;
    (void)event;
    (void)enable;
    /* CYW43438 uses the dedicated active-low HOST_WAKE OOB line.  The
     * project-local cyhal_gpio.c polls and coalesces that line on H7RS, so
     * enabling the SDMMC DAT1 interrupt here would be both redundant and
     * unsafe while the generated SDMMC IRQ is intentionally disabled. */
    (void)s_sdio_irq_handler;
    (void)s_sdio_irq_arg;
}
