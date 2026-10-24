/* Project-local WHD buffer pool helpers. */
#ifndef WHD_MONGOOSE_BUFFER_H
#define WHD_MONGOOSE_BUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
    uint32_t current;
    uint32_t high_water;
    uint32_t tx_hits;
    uint32_t rx_hits;
    uint32_t tx_malloc_attempts;
    uint32_t tx_malloc_successes;
    uint32_t tx_malloc_failures;
    uint32_t rx_malloc_attempts;
    uint32_t rx_malloc_successes;
    uint32_t rx_malloc_failures;
    uint32_t tx_fallback_pool_full;
    uint32_t tx_fallback_oversize;
    uint32_t rx_fallback_pool_full;
    uint32_t rx_fallback_oversize;
    uint32_t invalid_arguments;
    uint32_t tx_capacity_rejects;
    uint32_t rx_capacity_rejects;
} whd_mongoose_buffer_stats_t;

void whd_mongoose_buffer_get_stats(whd_mongoose_buffer_stats_t *stats);
void whd_mongoose_buffer_reset_stats(void);

bool whd_mongoose_buffer_is_sdio_dma_safe(const void *data,
                                         size_t payload_length,
                                         size_t transfer_length);

bool whd_mongoose_buffer_is_sdio_dma_cacheable(const void *data,
                                               size_t transfer_length);

#endif /* WHD_MONGOOSE_BUFFER_H */
