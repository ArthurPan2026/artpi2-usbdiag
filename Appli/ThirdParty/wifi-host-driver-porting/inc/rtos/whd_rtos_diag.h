/* Optional, allocation-free diagnostics for the local WHD RTOS port. */
#ifndef WHD_RTOS_DIAG_H
#define WHD_RTOS_DIAG_H

#include <stdbool.h>
#include <stdint.h>

#include "whd.h"

#define WHD_RTOS_SDPCM_QUEUE_COUNT 5U

typedef struct
{
    bool watching;
    uint32_t snapshots;
    uint32_t current[WHD_RTOS_SDPCM_QUEUE_COUNT];
    uint32_t high_water[WHD_RTOS_SDPCM_QUEUE_COUNT];
    uint32_t enqueued[WHD_RTOS_SDPCM_QUEUE_COUNT];
    uint32_t dequeued[WHD_RTOS_SDPCM_QUEUE_COUNT];
} whd_rtos_sdpcm_queue_stats_t;

/* These calls are no-ops/return false when diagnostics or SDPCM are absent. */
bool whd_rtos_sdpcm_queue_watch_register(whd_driver_t driver);
void whd_rtos_sdpcm_queue_watch_unregister(whd_driver_t driver);
void whd_rtos_sdpcm_queue_stats_get(whd_rtos_sdpcm_queue_stats_t *stats);
void whd_rtos_sdpcm_queue_stats_reset(void);

#endif /* WHD_RTOS_DIAG_H */
