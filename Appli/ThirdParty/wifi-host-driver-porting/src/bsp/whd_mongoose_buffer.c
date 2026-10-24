/*
 * WHD buffer callbacks for the Mongoose network stack.
 *
 * WHD's SDPCM implementation stores its queue link in the first word of a
 * packet and grows/shrinks the visible packet at the front while it builds
 * headers.  This small owning buffer supplies the required headroom and a
 * little tail room for SDIO block padding without pulling lwIP into the
 * project.  TX/common buffers stay in non-cacheable PSRAM for the multi-node
 * staging path; a small AXI SRAM RX pool is reserved for direct IDMA with
 * explicit D-cache maintenance.
 */
#include <limits.h>
#include <stdint.h>
#include <string.h>

#include "whd_network_types.h"
#include "whd_types.h"

#include "whd_mongoose_buffer.h"
#include "rtthread.h"

#define WHD_MONGOOSE_BUFFER_MAGIC 0x57484242UL
#define WHD_MONGOOSE_BUFFER_TAIL  64U
#define WHD_MONGOOSE_POOL_COUNT   32U
#define WHD_MONGOOSE_POOL_CAPACITY 4096U
#define WHD_MONGOOSE_POOL_RAW_BYTES \
    (WHD_MONGOOSE_POOL_CAPACITY + 32U)
#define WHD_MONGOOSE_POOL_INVALID 0xffU
#define WHD_MONGOOSE_POOL_STORAGE_BIAS \
    ((32U - (WHD_LINK_HEADER & 31U)) & 31U)
#define WHD_MONGOOSE_POOL_KIND_PSRAM 0U
#define WHD_MONGOOSE_POOL_KIND_AXI_RX 1U
#define WHD_MONGOOSE_RX_POOL_COUNT 8U
#define WHD_MONGOOSE_RX_POOL_CAPACITY 4096U
#define WHD_MONGOOSE_RX_POOL_RAW_BYTES \
    (WHD_MONGOOSE_RX_POOL_CAPACITY + 32U)

struct whd_mongoose_buffer
{
    uint32_t magic;
    uint8_t *storage;
    void *allocation;
    uint16_t capacity;
    uint16_t offset;
    uint16_t length;
    uint8_t pool_index;
    uint8_t pool_kind;
    uint8_t reserved[2];
};

/* Fixed pools keep the common WLAN buffers in the dedicated non-cacheable
 * PSRAM arena.  The smaller RX pool is in cacheable AXI SRAM and uses the
 * two-byte bias to keep its payload destination word-aligned for direct IDMA
 * with explicit D-cache maintenance. */
static struct whd_mongoose_buffer s_packet_pool[WHD_MONGOOSE_POOL_COUNT]
    __attribute__((section(".wifi_sdio_nc"), aligned(32), used));
static uint8_t s_packet_pool_storage[WHD_MONGOOSE_POOL_COUNT]
                                    [WHD_MONGOOSE_POOL_RAW_BYTES]
    __attribute__((section(".wifi_sdio_nc"), aligned(32), used));
static volatile uint32_t s_packet_pool_used;
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
static uint32_t s_packet_pool_current;
static uint32_t s_packet_pool_high_water;
static uint32_t s_packet_pool_tx_hits;
static uint32_t s_packet_pool_rx_hits;
static uint32_t s_packet_pool_tx_malloc_attempts;
static uint32_t s_packet_pool_tx_malloc_successes;
static uint32_t s_packet_pool_tx_malloc_failures;
static uint32_t s_packet_pool_rx_malloc_attempts;
static uint32_t s_packet_pool_rx_malloc_successes;
static uint32_t s_packet_pool_rx_malloc_failures;
static uint32_t s_packet_pool_tx_fallback_pool_full;
static uint32_t s_packet_pool_tx_fallback_oversize;
static uint32_t s_packet_pool_rx_fallback_pool_full;
static uint32_t s_packet_pool_rx_fallback_oversize;
static uint32_t s_buffer_invalid_arguments;
static uint32_t s_packet_pool_tx_capacity_rejects;
static uint32_t s_packet_pool_rx_capacity_rejects;
#endif
static struct whd_mongoose_buffer s_direct_rx_pool[WHD_MONGOOSE_RX_POOL_COUNT]
    __attribute__((section(".bss.wifi_sdio_rx"), aligned(32), used));
static uint8_t s_direct_rx_pool_storage[WHD_MONGOOSE_RX_POOL_COUNT]
                                        [WHD_MONGOOSE_RX_POOL_RAW_BYTES]
    __attribute__((section(".bss.wifi_sdio_rx"), aligned(32), used));
static volatile uint32_t s_direct_rx_pool_used;

static uint8_t *whd_mongoose_aligned_storage(uint8_t *raw)
{
    uintptr_t address = ((uintptr_t)raw + 31U) & ~(uintptr_t)31U;

    address += (32U - (WHD_LINK_HEADER & 31U)) & 31U;
    return (uint8_t *)address;
}

static struct whd_mongoose_buffer *whd_mongoose_pool_claim(
    whd_buffer_dir_t direction)
{
    rt_base_t level;
    uint32_t index;

#if !defined(WORK_EVIDENCE_DIAGNOSTICS) || !WORK_EVIDENCE_DIAGNOSTICS
    (void)direction;
#endif
    level = rt_hw_interrupt_disable();
    for (index = 0U; index < WHD_MONGOOSE_POOL_COUNT; index++)
    {
        uint32_t mask = 1UL << index;

        if ((s_packet_pool_used & mask) == 0U)
        {
            s_packet_pool_used |= mask;
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
            s_packet_pool_current++;
            if (s_packet_pool_current > s_packet_pool_high_water)
            {
                s_packet_pool_high_water = s_packet_pool_current;
            }
            if (direction == WHD_NETWORK_TX)
            {
                s_packet_pool_tx_hits++;
            }
            else
            {
                s_packet_pool_rx_hits++;
            }
#endif
            rt_hw_interrupt_enable(level);
            s_packet_pool[index].pool_index = (uint8_t)index;
            s_packet_pool[index].pool_kind = WHD_MONGOOSE_POOL_KIND_PSRAM;
            s_packet_pool[index].allocation = NULL;
            s_packet_pool[index].storage =
                s_packet_pool_storage[index] + WHD_MONGOOSE_POOL_STORAGE_BIAS;
            s_packet_pool[index].capacity = WHD_MONGOOSE_POOL_CAPACITY;
            return &s_packet_pool[index];
        }
    }
    rt_hw_interrupt_enable(level);
    return NULL;
}

#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
static void whd_mongoose_buffer_note_malloc_attempt(
    whd_buffer_dir_t direction, bool pool_full)
{
    rt_base_t level = rt_hw_interrupt_disable();

    if (direction == WHD_NETWORK_TX)
    {
        s_packet_pool_tx_malloc_attempts++;
        if (pool_full)
        {
            s_packet_pool_tx_fallback_pool_full++;
        }
        else
        {
            s_packet_pool_tx_fallback_oversize++;
        }
    }
    else
    {
        s_packet_pool_rx_malloc_attempts++;
        if (pool_full)
        {
            s_packet_pool_rx_fallback_pool_full++;
        }
        else
        {
            s_packet_pool_rx_fallback_oversize++;
        }
    }
    rt_hw_interrupt_enable(level);
}

static void whd_mongoose_buffer_note_malloc_result(
    whd_buffer_dir_t direction, bool succeeded)
{
    rt_base_t level = rt_hw_interrupt_disable();

    if (direction == WHD_NETWORK_TX)
    {
        if (succeeded)
        {
            s_packet_pool_tx_malloc_successes++;
        }
        else
        {
            s_packet_pool_tx_malloc_failures++;
        }
    }
    else
    {
        if (succeeded)
        {
            s_packet_pool_rx_malloc_successes++;
        }
        else
        {
            s_packet_pool_rx_malloc_failures++;
        }
    }
    rt_hw_interrupt_enable(level);
}

static void whd_mongoose_buffer_note_rejection(
    whd_buffer_dir_t direction, bool capacity)
{
    rt_base_t level = rt_hw_interrupt_disable();

    if (!capacity)
    {
        s_buffer_invalid_arguments++;
    }
    else if (direction == WHD_NETWORK_TX)
    {
        s_packet_pool_tx_capacity_rejects++;
    }
    else
    {
        s_packet_pool_rx_capacity_rejects++;
    }
    rt_hw_interrupt_enable(level);
}

void whd_mongoose_buffer_get_stats(whd_mongoose_buffer_stats_t *stats)
{
    rt_base_t level;

    if (stats == NULL)
    {
        return;
    }
    level = rt_hw_interrupt_disable();
    stats->current = s_packet_pool_current;
    stats->high_water = s_packet_pool_high_water;
    stats->tx_hits = s_packet_pool_tx_hits;
    stats->rx_hits = s_packet_pool_rx_hits;
    stats->tx_malloc_attempts = s_packet_pool_tx_malloc_attempts;
    stats->tx_malloc_successes = s_packet_pool_tx_malloc_successes;
    stats->tx_malloc_failures = s_packet_pool_tx_malloc_failures;
    stats->rx_malloc_attempts = s_packet_pool_rx_malloc_attempts;
    stats->rx_malloc_successes = s_packet_pool_rx_malloc_successes;
    stats->rx_malloc_failures = s_packet_pool_rx_malloc_failures;
    stats->tx_fallback_pool_full = s_packet_pool_tx_fallback_pool_full;
    stats->tx_fallback_oversize = s_packet_pool_tx_fallback_oversize;
    stats->rx_fallback_pool_full = s_packet_pool_rx_fallback_pool_full;
    stats->rx_fallback_oversize = s_packet_pool_rx_fallback_oversize;
    stats->invalid_arguments = s_buffer_invalid_arguments;
    stats->tx_capacity_rejects = s_packet_pool_tx_capacity_rejects;
    stats->rx_capacity_rejects = s_packet_pool_rx_capacity_rejects;
    rt_hw_interrupt_enable(level);
}

void whd_mongoose_buffer_reset_stats(void)
{
    rt_base_t level = rt_hw_interrupt_disable();

    /* Preserve live allocations; establish the current occupancy as baseline. */
    s_packet_pool_high_water = s_packet_pool_current;
    s_packet_pool_tx_hits = 0U;
    s_packet_pool_rx_hits = 0U;
    s_packet_pool_tx_malloc_attempts = 0U;
    s_packet_pool_tx_malloc_successes = 0U;
    s_packet_pool_tx_malloc_failures = 0U;
    s_packet_pool_rx_malloc_attempts = 0U;
    s_packet_pool_rx_malloc_successes = 0U;
    s_packet_pool_rx_malloc_failures = 0U;
    s_packet_pool_tx_fallback_pool_full = 0U;
    s_packet_pool_tx_fallback_oversize = 0U;
    s_packet_pool_rx_fallback_pool_full = 0U;
    s_packet_pool_rx_fallback_oversize = 0U;
    s_buffer_invalid_arguments = 0U;
    s_packet_pool_tx_capacity_rejects = 0U;
    s_packet_pool_rx_capacity_rejects = 0U;
    rt_hw_interrupt_enable(level);
}
#else
void whd_mongoose_buffer_get_stats(whd_mongoose_buffer_stats_t *stats)
{
    if (stats != NULL)
    {
        memset(stats, 0, sizeof(*stats));
    }
}

void whd_mongoose_buffer_reset_stats(void)
{
}
#endif

static struct whd_mongoose_buffer *whd_mongoose_rx_pool_claim(void)
{
    rt_base_t level;
    uint32_t index;

    level = rt_hw_interrupt_disable();
    for (index = 0U; index < WHD_MONGOOSE_RX_POOL_COUNT; index++)
    {
        uint32_t mask = 1UL << index;

        if ((s_direct_rx_pool_used & mask) == 0U)
        {
            s_direct_rx_pool_used |= mask;
            rt_hw_interrupt_enable(level);
            s_direct_rx_pool[index].pool_index = (uint8_t)index;
            s_direct_rx_pool[index].pool_kind =
                WHD_MONGOOSE_POOL_KIND_AXI_RX;
            s_direct_rx_pool[index].allocation = NULL;
            s_direct_rx_pool[index].storage =
                s_direct_rx_pool_storage[index] +
                WHD_MONGOOSE_POOL_STORAGE_BIAS;
            s_direct_rx_pool[index].capacity =
                WHD_MONGOOSE_RX_POOL_CAPACITY;
            return &s_direct_rx_pool[index];
        }
    }
    rt_hw_interrupt_enable(level);
    return NULL;
}

bool whd_mongoose_buffer_is_sdio_dma_safe(const void *data,
                                         size_t payload_length,
                                         size_t transfer_length)
{
    uintptr_t begin;
    uintptr_t end;
    uint32_t index;

    if (data == NULL || payload_length == 0U ||
        transfer_length < payload_length || (transfer_length & 3U) != 0U)
    {
        return false;
    }
    begin = (uintptr_t)data;
    end = begin + transfer_length;
    if (end < begin || (begin & 3U) != 0U)
    {
        return false;
    }

    /* Only a project-owned slot has the reserved tail for block padding. */
    for (index = 0U; index < WHD_MONGOOSE_POOL_COUNT; index++)
    {
        uintptr_t pool_begin =
            (uintptr_t)s_packet_pool_storage[index] +
            WHD_MONGOOSE_POOL_STORAGE_BIAS;
        uintptr_t pool_end = pool_begin + WHD_MONGOOSE_POOL_CAPACITY;

        if (begin >= pool_begin && end <= pool_end)
        {
            return true;
        }
    }
    for (index = 0U; index < WHD_MONGOOSE_RX_POOL_COUNT; index++)
    {
        uintptr_t pool_begin =
            (uintptr_t)s_direct_rx_pool_storage[index] +
            WHD_MONGOOSE_POOL_STORAGE_BIAS;
        uintptr_t pool_end = pool_begin + WHD_MONGOOSE_RX_POOL_CAPACITY;

        if (begin >= pool_begin && end <= pool_end)
        {
            return true;
        }
    }
    return false;
}

bool whd_mongoose_buffer_is_sdio_dma_cacheable(const void *data,
                                               size_t transfer_length)
{
    uintptr_t begin;
    uintptr_t end;
    uint32_t index;

    if (data == NULL || transfer_length == 0U)
    {
        return false;
    }
    begin = (uintptr_t)data;
    end = begin + transfer_length;
    if (end < begin)
    {
        return false;
    }
    /* This predicate intentionally reports ownership of the cacheable RX
     * pool even when the current WHD payload is not word-sized.  The SDIO
     * adapter separately checks alignment and rounded block bounds before it
     * selects direct IDMA. */
    for (index = 0U; index < WHD_MONGOOSE_RX_POOL_COUNT; index++)
    {
        uintptr_t pool_begin =
            (uintptr_t)s_direct_rx_pool_storage[index] +
            WHD_MONGOOSE_POOL_STORAGE_BIAS;
        uintptr_t pool_end = pool_begin + WHD_MONGOOSE_RX_POOL_CAPACITY;

        if (begin >= pool_begin && end <= pool_end)
        {
            return true;
        }
    }
    return false;
}

static struct whd_mongoose_buffer *whd_mongoose_buffer_from_handle(
    whd_buffer_t buffer)
{
    struct whd_mongoose_buffer *packet =
        (struct whd_mongoose_buffer *)buffer;

    if (packet == NULL || packet->magic != WHD_MONGOOSE_BUFFER_MAGIC ||
        packet->storage == NULL || packet->offset > packet->capacity ||
        packet->length > (uint16_t)(packet->capacity - packet->offset))
    {
        return NULL;
    }
    return packet;
}

static whd_result_t whd_mongoose_host_buffer_get(whd_buffer_t *buffer,
                                                 whd_buffer_dir_t direction,
                                                 uint16_t size,
                                                 uint32_t timeout_ms)
{
    struct whd_mongoose_buffer *packet;
    uint32_t capacity;
    uint32_t allocation_size;

    (void)timeout_ms;
    if (buffer == NULL)
    {
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
        whd_mongoose_buffer_note_rejection(direction, false);
#endif
        return WHD_BADARG;
    }

    capacity = (uint32_t)size + (uint32_t)WHD_LINK_HEADER +
               WHD_MONGOOSE_BUFFER_TAIL;
    if (capacity > UINT16_MAX)
    {
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
        whd_mongoose_buffer_note_rejection(direction, true);
#endif
        return WHD_BUFFER_ALLOC_FAIL;
    }
    packet = NULL;
    if (direction == WHD_NETWORK_RX &&
        capacity <= WHD_MONGOOSE_RX_POOL_CAPACITY)
    {
        packet = whd_mongoose_rx_pool_claim();
    }
    if (packet == NULL && capacity <= WHD_MONGOOSE_POOL_CAPACITY)
    {
        packet = whd_mongoose_pool_claim(direction);
    }
    allocation_size = sizeof(*packet) + capacity + 33U;
    if (packet == NULL)
    {
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
        whd_mongoose_buffer_note_malloc_attempt(
            direction, capacity <= WHD_MONGOOSE_POOL_CAPACITY);
#endif
        packet = (struct whd_mongoose_buffer *)rt_malloc(allocation_size);
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
        whd_mongoose_buffer_note_malloc_result(direction, packet != NULL);
#endif
        if (packet != NULL)
        {
            packet->pool_index = WHD_MONGOOSE_POOL_INVALID;
            packet->pool_kind = WHD_MONGOOSE_POOL_INVALID;
        }
    }
    if (packet == NULL)
    {
        *buffer = NULL;
        return WHD_BUFFER_ALLOC_FAIL;
    }

    packet->magic = WHD_MONGOOSE_BUFFER_MAGIC;
    if (packet->pool_index == WHD_MONGOOSE_POOL_INVALID)
    {
        packet->allocation = packet;
        packet->storage = whd_mongoose_aligned_storage(
            (uint8_t *)(packet + 1));
        packet->capacity = (uint16_t)capacity;
    }
    packet->offset = (uint16_t)WHD_LINK_HEADER;
    packet->length = size;
    memset(packet->storage, 0, packet->capacity);
    *buffer = (whd_buffer_t)packet;
    return WHD_SUCCESS;
}

static void whd_mongoose_buffer_release(whd_buffer_t buffer,
                                        whd_buffer_dir_t direction)
{
    struct whd_mongoose_buffer *packet =
        whd_mongoose_buffer_from_handle(buffer);

    (void)direction;
    if (packet != NULL)
    {
        packet->magic = 0U;
        if (packet->pool_index != WHD_MONGOOSE_POOL_INVALID)
        {
            rt_base_t level = rt_hw_interrupt_disable();
            if (packet->pool_kind == WHD_MONGOOSE_POOL_KIND_AXI_RX)
            {
                s_direct_rx_pool_used &= ~(1UL << packet->pool_index);
            }
            else
            {
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
                uint32_t mask = 1UL << packet->pool_index;

                if ((s_packet_pool_used & mask) != 0U)
                {
                    s_packet_pool_used &= ~mask;
                    if (s_packet_pool_current != 0U)
                    {
                        s_packet_pool_current--;
                    }
                }
#else
                s_packet_pool_used &= ~(1UL << packet->pool_index);
#endif
            }
            rt_hw_interrupt_enable(level);
        }
        else
        {
            rt_free(packet->allocation);
        }
    }
}

static uint8_t *whd_mongoose_buffer_data(whd_buffer_t buffer)
{
    struct whd_mongoose_buffer *packet =
        whd_mongoose_buffer_from_handle(buffer);

    return packet == NULL ? NULL : packet->storage + packet->offset;
}

static uint16_t whd_mongoose_buffer_size(whd_buffer_t buffer)
{
    struct whd_mongoose_buffer *packet =
        whd_mongoose_buffer_from_handle(buffer);

    return packet == NULL ? 0U : packet->length;
}

static whd_result_t whd_mongoose_buffer_set_size(whd_buffer_t buffer,
                                                uint16_t size)
{
    struct whd_mongoose_buffer *packet =
        whd_mongoose_buffer_from_handle(buffer);

    if (packet == NULL || size > (uint16_t)(packet->capacity - packet->offset))
    {
        return WHD_PMK_WRONG_LENGTH;
    }
    packet->length = size;
    return WHD_SUCCESS;
}

static whd_result_t whd_mongoose_buffer_add_remove_at_front(
    whd_buffer_t *buffer, int32_t add_remove_amount)
{
    struct whd_mongoose_buffer *packet;
    uint32_t amount;

    if (buffer == NULL)
    {
        return WHD_BADARG;
    }
    packet = whd_mongoose_buffer_from_handle(*buffer);
    if (packet == NULL)
    {
        return WHD_PMK_WRONG_LENGTH;
    }

    if (add_remove_amount >= 0)
    {
        amount = (uint32_t)add_remove_amount;
        if (amount > packet->length)
        {
            return WHD_PMK_WRONG_LENGTH;
        }
        packet->offset = (uint16_t)(packet->offset + amount);
        packet->length = (uint16_t)(packet->length - amount);
    }
    else
    {
        amount = (uint32_t)(-(add_remove_amount + 1)) + 1U;
        if (amount > packet->offset ||
            amount > (uint32_t)packet->capacity - packet->offset -
                         packet->length)
        {
            return WHD_PMK_WRONG_LENGTH;
        }
        packet->offset = (uint16_t)(packet->offset - amount);
        packet->length = (uint16_t)(packet->length + amount);
    }
    return WHD_SUCCESS;
}

struct whd_buffer_funcs whd_buffer_ops =
{
    .whd_host_buffer_get = whd_mongoose_host_buffer_get,
    .whd_buffer_release = whd_mongoose_buffer_release,
    .whd_buffer_get_current_piece_data_pointer = whd_mongoose_buffer_data,
    .whd_buffer_get_current_piece_size = whd_mongoose_buffer_size,
    .whd_buffer_set_size = whd_mongoose_buffer_set_size,
    .whd_buffer_add_remove_at_front = whd_mongoose_buffer_add_remove_at_front
};
