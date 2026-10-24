/* Project-local SDMMC2/IDMA diagnostics for the WHD adapter. */
#ifndef WHD_STM32_SDIO_H
#define WHD_STM32_SDIO_H

#include <stdbool.h>
#include <stdint.h>

#define WHD_STM32_SDIO_ABORT_NOT_ATTEMPTED UINT32_MAX

typedef enum
{
    WHD_STM32_SDIO_REG_STA,
    WHD_STM32_SDIO_REG_DCOUNT,
    WHD_STM32_SDIO_REG_IDMACTRL,
    WHD_STM32_SDIO_REG_IDMABASER,
    WHD_STM32_SDIO_REG_IDMABSIZE,
    WHD_STM32_SDIO_REG_IDMALAR,
    WHD_STM32_SDIO_REG_DCTRL,
    WHD_STM32_SDIO_REG_CMD,
    WHD_STM32_SDIO_REG_DLEN,
    WHD_STM32_SDIO_REG_DTIMER,
    WHD_STM32_SDIO_REG_IDMABAR
} whd_stm32_sdio_recovery_register_t;

typedef struct
{
    void *context;
    uint32_t (*read_register)(void *context,
                              whd_stm32_sdio_recovery_register_t reg);
    void (*write_register)(void *context,
                           whd_stm32_sdio_recovery_register_t reg,
                           uint32_t value);
    void (*clear_flags)(void *context);
    void (*barrier)(void *context);
    void (*wait_one_ms)(void *context);
    uint32_t (*abort_io_function)(void *context, uint8_t function);
} whd_stm32_sdio_recovery_ops_t;

typedef struct
{
    uint32_t status;
    uint32_t data_count;
    uint32_t idma_control;
    uint32_t idma_base;
    uint32_t idma_size;
    uint32_t idma_list;
    uint32_t idma_link;
    uint32_t data_control;
    uint32_t command;
    uint32_t abort_result;
    uint8_t function;
    uint8_t abort_attempted;
} whd_stm32_sdio_recovery_evidence_t;

typedef enum
{
    WHD_STM32_SDIO_RECOVERY_NOT_NEEDED,
    WHD_STM32_SDIO_RECOVERY_SUCCEEDED,
    WHD_STM32_SDIO_RECOVERY_FAILED
} whd_stm32_sdio_recovery_result_t;

/* Shared cleanup policy used by the hardware port and host register-mock tests. */
static inline void whd_stm32_sdio_cleanup_transfer(
    const whd_stm32_sdio_recovery_ops_t *ops,
    bool data_timeout,
    uint32_t cmdtrans_mask,
    uint32_t fifo_reset_value)
{
    uint32_t command;

    if (data_timeout)
    {
        ops->write_register(ops->context, WHD_STM32_SDIO_REG_CMD, 0U);
    }
    else
    {
        command = ops->read_register(ops->context, WHD_STM32_SDIO_REG_CMD);
        ops->write_register(ops->context, WHD_STM32_SDIO_REG_CMD,
                            command & ~cmdtrans_mask);
    }

    ops->write_register(ops->context, WHD_STM32_SDIO_REG_DCTRL, 0U);
    ops->write_register(ops->context, WHD_STM32_SDIO_REG_IDMACTRL, 0U);
    ops->write_register(ops->context, WHD_STM32_SDIO_REG_DLEN, 0U);
    ops->write_register(ops->context, WHD_STM32_SDIO_REG_DTIMER, 0U);
    ops->write_register(ops->context, WHD_STM32_SDIO_REG_IDMABAR, 0U);
    ops->write_register(ops->context, WHD_STM32_SDIO_REG_IDMABASER, 0U);
    ops->write_register(ops->context, WHD_STM32_SDIO_REG_IDMALAR, 0U);
    ops->write_register(ops->context, WHD_STM32_SDIO_REG_IDMABSIZE, 0U);
    if (data_timeout)
    {
        ops->write_register(ops->context, WHD_STM32_SDIO_REG_DCTRL,
                            fifo_reset_value);
    }
    ops->clear_flags(ops->context);
    ops->barrier(ops->context);
}

/* Common transfer completion: normal cleanup, or bounded CMD53 timeout
 * recovery after snapshotting evidence and quiescing local DMA/DPSM/FIFO. */
static inline whd_stm32_sdio_recovery_result_t
whd_stm32_sdio_finish_transfer(
    const whd_stm32_sdio_recovery_ops_t *ops,
    bool data_timeout,
    uint32_t cmd53_argument,
    uint32_t active_mask,
    uint32_t cmdtrans_mask,
    uint32_t fifo_reset_value,
    uint32_t max_waits,
    uint32_t abort_success_result,
    whd_stm32_sdio_recovery_evidence_t *evidence)
{
    uint32_t waits = 0U;

    if (!data_timeout)
    {
        whd_stm32_sdio_cleanup_transfer(ops, false, cmdtrans_mask,
                                         fifo_reset_value);
        return WHD_STM32_SDIO_RECOVERY_NOT_NEEDED;
    }

    evidence->status = ops->read_register(ops->context, WHD_STM32_SDIO_REG_STA);
    evidence->data_count = ops->read_register(ops->context,
                                               WHD_STM32_SDIO_REG_DCOUNT);
    evidence->idma_control = ops->read_register(ops->context,
                                                 WHD_STM32_SDIO_REG_IDMACTRL);
    evidence->idma_base = ops->read_register(ops->context,
                                              WHD_STM32_SDIO_REG_IDMABASER);
    evidence->idma_size = ops->read_register(ops->context,
                                              WHD_STM32_SDIO_REG_IDMABSIZE);
    evidence->idma_list = ops->read_register(ops->context,
                                              WHD_STM32_SDIO_REG_IDMABAR);
    evidence->idma_link = ops->read_register(ops->context,
                                              WHD_STM32_SDIO_REG_IDMALAR);
    evidence->data_control = ops->read_register(ops->context,
                                                 WHD_STM32_SDIO_REG_DCTRL);
    evidence->command = ops->read_register(ops->context,
                                            WHD_STM32_SDIO_REG_CMD);
    evidence->function = (uint8_t)((cmd53_argument >> 28U) & 0x07U);
    evidence->abort_attempted = 0U;
    evidence->abort_result = WHD_STM32_SDIO_ABORT_NOT_ATTEMPTED;

    whd_stm32_sdio_cleanup_transfer(ops, true, cmdtrans_mask,
                                     fifo_reset_value);

    for (;;)
    {
        if ((ops->read_register(ops->context, WHD_STM32_SDIO_REG_STA) &
             active_mask) == 0U)
        {
            break;
        }
        if (waits == max_waits)
        {
            return WHD_STM32_SDIO_RECOVERY_FAILED;
        }
        ops->wait_one_ms(ops->context);
        waits++;
    }

    evidence->abort_attempted = 1U;
    evidence->abort_result = ops->abort_io_function(ops->context,
                                                     evidence->function);
    if (evidence->abort_result == abort_success_result)
    {
        return WHD_STM32_SDIO_RECOVERY_SUCCEEDED;
    }

    /* Also clear any host command state left by a failed CMD52 abort. */
    whd_stm32_sdio_cleanup_transfer(ops, true, cmdtrans_mask,
                                     fifo_reset_value);
    return WHD_STM32_SDIO_RECOVERY_FAILED;
}

typedef struct
{
    uint32_t clock_hz;
    uint32_t direct_transfers;
    uint32_t linked_transfers;
    uint32_t direct_bytes;
    uint32_t linked_bytes;
    uint32_t padding_bytes;
    uint32_t errors;
    uint32_t spin_commands;
    uint32_t spin_data;
    uint32_t poll_yields;
    uint32_t last_error_code;
    uint32_t last_error_status;
    uint32_t last_error_argument;
    uint32_t last_error_data;
    uint32_t last_error_length;
    uint32_t last_error_transfer_length;
    uint32_t last_error_response;
    uint8_t last_error_direct;
    uint8_t bus_width;
    uint32_t timeouts;
    uint32_t timeout_abort_attempts;
    uint32_t timeout_recoveries;
    uint32_t timeout_recovery_failures;
    uint32_t last_error_dcount;
    uint32_t last_error_idma_control;
    uint32_t last_error_idma_base;
    uint32_t last_error_idma_size;
    uint32_t last_error_idma_list;
    uint32_t last_error_idma_link;
    uint32_t last_error_dctrl;
    uint32_t last_error_cmd;
    uint32_t last_abort_result;
    uint8_t last_abort_function;
    uint8_t last_abort_attempted;
} whd_stm32_sdio_stats_t;

void whd_stm32_sdio_get_stats(whd_stm32_sdio_stats_t *stats);

#endif /* WHD_STM32_SDIO_H */
