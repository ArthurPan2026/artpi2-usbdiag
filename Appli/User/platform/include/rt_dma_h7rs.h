/*
 * ART-Pi2 H7RS DMA memory contract.
 *
 * RT-Thread's upstream DMA API supplies allocation and cache mapping
 * primitives.  This small H7RS-facing layer adds the part that a classic
 * Cortex-M system still needs explicitly: a checked CPU/device ownership
 * transition for buffers shared with USB, SDMMC2 or a graphics engine.
 */
#ifndef WORK_RT_DMA_H7RS_H
#define WORK_RT_DMA_H7RS_H

#include <drivers/dma.h>

typedef enum
{
    RT_DMA_BUFFER_CPU_OWNED = 0,
    RT_DMA_BUFFER_DEVICE_OWNED,
} rt_dma_buffer_owner_t;

typedef struct rt_dma_buffer
{
    rt_uint32_t magic;
    void *cpu_addr;
    rt_ubase_t dma_addr;
    rt_size_t size;
    rt_ubase_t flags;
    rt_dma_buffer_owner_t owner;
} rt_dma_buffer_t;

int rt_dma_h7rs_init(void);

/* The project-owned H7RS DMA device carries the official 5.3 mapping vtable.
 * Pass it to rt_dma_alloc/free/sync_* instead of relying on an old NULL-device
 * compatibility path. */
struct rt_device *rt_dma_h7rs_device(void);

int rt_dma_buffer_init(rt_dma_buffer_t *buffer, void *cpu_addr,
                       rt_size_t size, rt_ubase_t dma_addr,
                       rt_ubase_t flags);
int rt_dma_buffer_prepare_for_device(rt_dma_buffer_t *buffer);
int rt_dma_buffer_complete_for_cpu(rt_dma_buffer_t *buffer);
int rt_dma_buffer_release_device(rt_dma_buffer_t *buffer);
int rt_dma_buffer_acquire_cpu(rt_dma_buffer_t *buffer);

void rt_dma_h7rs_status(void);

#endif /* WORK_RT_DMA_H7RS_H */
