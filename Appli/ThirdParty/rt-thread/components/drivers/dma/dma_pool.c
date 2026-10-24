/*
 * ART-Pi2 STM32H7RS DMA mapping backend.
 *
 * The upstream RT-Thread dma_pool.c is designed for RT-Thread Smart's
 * Device-Model/OFW/MMU environment.  This project is a classic bare-metal
 * Cortex-M7 build, so the same public RT_DMA API is backed by the linker-owned
 * USB_STREAM_NC pool and the normal RT-Thread heap.  Physical and CPU
 * addresses are identical on this MCU; cache attributes are fixed by the
 * MPU, not by an MMU alias.
 */
#include <stdint.h>
#include <stddef.h>

#include <finsh.h>
#include <rtdevice.h>
#include <rtthread.h>
#include <rthw.h>

#include "stm32h7rsxx_hal.h"
#include "rt_dma_h7rs.h"

#define RT_DMA_H7RS_MAGIC              0x444D4132UL /* "DMA2" */
#define RT_DMA_H7RS_ALLOC_MAGIC        0x444D4232UL /* "DMB2" */
#define RT_DMA_H7RS_CACHE_LINE         32U
#define RT_DMA_H7RS_NC_AXI_BEGIN       0x24060000UL
#define RT_DMA_H7RS_NC_AXI_END         0x24070000UL
#define RT_DMA_H7RS_PSRAM_BEGIN        0x90000000UL
#define RT_DMA_H7RS_PSRAM_END          0x92000000UL

extern uint8_t __usb_stream_nc_start[];
extern uint8_t __usb_stream_nc_end[];

struct rt_dma_allocation_header
{
    rt_uint32_t magic;
    rt_uint32_t source;
    void *raw;
    rt_size_t raw_size;
};

/* The NC linker arena uses the official 5.3 memheap directly.  This keeps
 * DMA pool ownership independent from whichever system allocator is selected
 * for rt_malloc/rt_free in the A/B build profile. */
static struct rt_memheap s_dma_nc_pool;
static volatile rt_bool_t s_dma_initialized;
static volatile rt_uint32_t s_dma_allocations;
static volatile rt_uint32_t s_dma_frees;
static volatile rt_uint32_t s_dma_sync_out;
static volatile rt_uint32_t s_dma_sync_in;
static volatile rt_uint32_t s_dma_sync_bypass;
static volatile rt_uint32_t s_dma_owner_errors;

static rt_bool_t rt_dma_h7rs_range_valid(const void *address, rt_size_t size)
{
    rt_uintptr_t start;
    rt_uintptr_t end;

    if (address == RT_NULL || size == 0U)
    {
        return RT_FALSE;
    }
    start = (rt_uintptr_t)address;
    if (start > (rt_uintptr_t)-1 - size)
    {
        return RT_FALSE;
    }
    end = start + size;
    return end > start ? RT_TRUE : RT_FALSE;
}

static rt_bool_t rt_dma_h7rs_range_nocache(const void *address,
                                           rt_size_t size)
{
    rt_uintptr_t start;
    rt_uintptr_t end;

    if (!rt_dma_h7rs_range_valid(address, size))
    {
        return RT_FALSE;
    }
    start = (rt_uintptr_t)address;
    end = start + size;

    if (start >= RT_DMA_H7RS_NC_AXI_BEGIN &&
        end <= RT_DMA_H7RS_NC_AXI_END)
    {
        return RT_TRUE;
    }
    /* The current H7RS MPU map deliberately makes the whole 32-MB PSRAM
     * window non-cacheable because LTDC, GPU2D, USB and SDMMC can all master
     * it.  Keep this test centralized so a future split map changes one
     * policy point instead of every driver. */
    if (start >= RT_DMA_H7RS_PSRAM_BEGIN &&
        end <= RT_DMA_H7RS_PSRAM_END)
    {
        return RT_TRUE;
    }
    return RT_FALSE;
}

static rt_size_t rt_dma_h7rs_cache_span(const void *address, rt_size_t size,
                                        rt_uintptr_t *aligned_start)
{
    rt_uintptr_t start;
    rt_uintptr_t end;

    if (!rt_dma_h7rs_range_valid(address, size) || aligned_start == RT_NULL)
    {
        return 0U;
    }
    start = (rt_uintptr_t)address & ~(rt_uintptr_t)(RT_DMA_H7RS_CACHE_LINE - 1U);
    end = ((rt_uintptr_t)address + size + RT_DMA_H7RS_CACHE_LINE - 1U) &
          ~(rt_uintptr_t)(RT_DMA_H7RS_CACHE_LINE - 1U);
    if (end <= start || end - start > (rt_uintptr_t)-1)
    {
        return 0U;
    }
    *aligned_start = start;
    return (rt_size_t)(end - start);
}

int rt_dma_h7rs_init(void)
{
    rt_size_t pool_size;

    if (s_dma_initialized)
    {
        return RT_EOK;
    }

    pool_size = (rt_size_t)((rt_uintptr_t)__usb_stream_nc_end -
                            (rt_uintptr_t)__usb_stream_nc_start);
    if (pool_size < 256U)
    {
        rt_kprintf("[DMA] NC pool is too small: %lu bytes\r\n",
                   (unsigned long)pool_size);
        return -RT_ENOMEM;
    }

    if (rt_memheap_init(&s_dma_nc_pool, "dman",
                        __usb_stream_nc_start, pool_size) != RT_EOK)
    {
        rt_kprintf("[DMA] NC pool init failed at 0x%08lx (%lu bytes)\r\n",
                   (unsigned long)(rt_uintptr_t)__usb_stream_nc_start,
                   (unsigned long)pool_size);
        return -RT_ENOMEM;
    }
    s_dma_initialized = RT_TRUE;
    rt_kprintf("[DMA] RT-Thread DMA contract ready, NC pool=0x%08lx..0x%08lx\r\n",
               (unsigned long)(rt_uintptr_t)__usb_stream_nc_start,
               (unsigned long)(rt_uintptr_t)__usb_stream_nc_end);
    return RT_EOK;
}

static void *rt_dma_h7rs_alloc_from_pool(rt_size_t size, rt_uint32_t source,
                                         rt_size_t *raw_size)
{
    void *raw;
    rt_uintptr_t aligned;
    struct rt_dma_allocation_header *header;
    rt_size_t requested;

    if (raw_size == RT_NULL || size > (rt_size_t)-1 -
                                  sizeof(*header) - RT_DMA_H7RS_CACHE_LINE)
    {
        return RT_NULL;
    }
    requested = size + sizeof(*header) + RT_DMA_H7RS_CACHE_LINE;
    raw = source == 1U ? rt_memheap_alloc(&s_dma_nc_pool, requested) :
                         rt_malloc(requested);
    if (raw == RT_NULL)
    {
        return RT_NULL;
    }
    aligned = ((rt_uintptr_t)raw + sizeof(*header) +
               RT_DMA_H7RS_CACHE_LINE - 1U) &
              ~(rt_uintptr_t)(RT_DMA_H7RS_CACHE_LINE - 1U);
    header = (struct rt_dma_allocation_header *)(aligned - sizeof(*header));
    header->magic = RT_DMA_H7RS_ALLOC_MAGIC;
    header->source = source;
    header->raw = raw;
    header->raw_size = requested;
    *raw_size = requested;
    return (void *)aligned;
}

static void *rt_dma_h7rs_alloc(struct rt_device *dev, rt_size_t size,
                               rt_ubase_t *dma_handle, rt_ubase_t flags)
{
    void *buffer;
    rt_uint32_t source;
    rt_size_t raw_size = 0U;

    RT_UNUSED(dev);
    if (size == 0U || rt_dma_h7rs_init() != RT_EOK)
    {
        return RT_NULL;
    }

    /* NOCACHE is the coherent allocation contract.  WT is accepted as a
     * portable request, but this no-MMU port returns a non-cacheable buffer
     * rather than pretending to create a cacheable alias. */
    source = (flags & (RT_DMA_F_NOCACHE | RT_DMA_F_WT)) != 0U ? 1U : 0U;
    buffer = rt_dma_h7rs_alloc_from_pool(size, source, &raw_size);
    if (buffer == RT_NULL)
    {
        return RT_NULL;
    }
    if (dma_handle != RT_NULL)
    {
        *dma_handle = (rt_ubase_t)(rt_uintptr_t)buffer;
    }
    s_dma_allocations++;
    return buffer;
}

static struct rt_dma_allocation_header *rt_dma_h7rs_header(void *cpu_addr)
{
    struct rt_dma_allocation_header *header;

    if (cpu_addr == RT_NULL ||
        ((rt_uintptr_t)cpu_addr & (RT_DMA_H7RS_CACHE_LINE - 1U)) != 0U)
    {
        return RT_NULL;
    }
    header = (struct rt_dma_allocation_header *)
             ((rt_uint8_t *)cpu_addr - sizeof(*header));
    return header->magic == RT_DMA_H7RS_ALLOC_MAGIC ? header : RT_NULL;
}

static void rt_dma_h7rs_free(struct rt_device *dev, rt_size_t size,
                             void *cpu_addr, rt_ubase_t dma_handle,
                             rt_ubase_t flags)
{
    struct rt_dma_allocation_header *header;

    RT_UNUSED(dev);
    RT_UNUSED(size);
    RT_UNUSED(dma_handle);
    RT_UNUSED(flags);
    header = rt_dma_h7rs_header(cpu_addr);
    if (header == RT_NULL || header->raw == RT_NULL)
    {
        return;
    }
    header->magic = 0U;
    if (header->source == 1U)
    {
        rt_memheap_free(header->raw);
    }
    else
    {
        rt_free(header->raw);
    }
    s_dma_frees++;
}

static rt_err_t rt_dma_h7rs_sync_out_default(void *data, rt_size_t size,
                                              rt_ubase_t *dma_handle,
                                              rt_ubase_t flags)
{
    rt_uintptr_t aligned_start;
    rt_size_t span;

    if (!rt_dma_h7rs_range_valid(data, size))
    {
        return -RT_EINVAL;
    }
    if (dma_handle != RT_NULL)
    {
        *dma_handle = (rt_ubase_t)(rt_uintptr_t)data;
    }
    if ((flags & RT_DMA_F_NOCACHE) != 0U ||
        rt_dma_h7rs_range_nocache(data, size))
    {
        __DMB();
        __DSB();
        s_dma_sync_bypass++;
        return RT_EOK;
    }
    span = rt_dma_h7rs_cache_span(data, size, &aligned_start);
    if (span == 0U)
    {
        return -RT_EINVAL;
    }
    if ((SCB->CCR & SCB_CCR_DC_Msk) != 0U)
    {
        rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH,
                             (void *)aligned_start, (int)span);
    }
    __DMB();
    __DSB();
    s_dma_sync_out++;
    return RT_EOK;
}

static rt_err_t rt_dma_h7rs_sync_in_default(void *data, rt_size_t size,
                                             rt_ubase_t dma_handle,
                                             rt_ubase_t flags)
{
    rt_uintptr_t aligned_start;
    rt_size_t span;

    RT_UNUSED(dma_handle);
    if (!rt_dma_h7rs_range_valid(data, size))
    {
        return -RT_EINVAL;
    }
    if ((flags & RT_DMA_F_NOCACHE) != 0U ||
        rt_dma_h7rs_range_nocache(data, size))
    {
        __DMB();
        __DSB();
        s_dma_sync_bypass++;
        return RT_EOK;
    }
    span = rt_dma_h7rs_cache_span(data, size, &aligned_start);
    if (span == 0U)
    {
        return -RT_EINVAL;
    }
    __DSB();
    if ((SCB->CCR & SCB_CCR_DC_Msk) != 0U)
    {
        rt_hw_cpu_dcache_ops(RT_HW_CACHE_INVALIDATE,
                             (void *)aligned_start, (int)span);
    }
    __DMB();
    s_dma_sync_in++;
    return RT_EOK;
}

static rt_err_t rt_dma_h7rs_sync_out(struct rt_device *dev, void *data,
                                     rt_size_t size, rt_ubase_t *dma_handle,
                                     rt_ubase_t flags)
{
    RT_UNUSED(dev);
    return rt_dma_h7rs_sync_out_default(data, size, dma_handle, flags);
}

static rt_err_t rt_dma_h7rs_sync_in(struct rt_device *dev, void *out_data,
                                    rt_size_t size, rt_ubase_t dma_handle,
                                    rt_ubase_t flags)
{
    RT_UNUSED(dev);
    return rt_dma_h7rs_sync_in_default(out_data, size, dma_handle, flags);
}

/* One formal mapping vtable is shared by the H7RS DMA owner device and by
 * the no-DM default path.  Drivers may still install a different official
 * rt_dma_map_ops vtable on their own rt_device when a future CubeMX/HAL
 * peripheral needs special handling. */
static const struct rt_dma_map_ops s_h7rs_dma_map_ops =
{
    rt_dma_h7rs_alloc,
    rt_dma_h7rs_free,
    rt_dma_h7rs_sync_out,
    rt_dma_h7rs_sync_in,
};

static struct rt_device s_h7rs_dma_owner =
{
    .dma_ops = &s_h7rs_dma_map_ops,
};

struct rt_device *rt_dma_h7rs_device(void)
{
    return &s_h7rs_dma_owner;
}

static const struct rt_dma_map_ops *rt_dma_h7rs_get_ops(struct rt_device *dev)
{
    if (dev != RT_NULL && dev->dma_ops != RT_NULL)
    {
        return (const struct rt_dma_map_ops *)dev->dma_ops;
    }
    return &s_h7rs_dma_map_ops;
}

void *rt_dma_alloc(struct rt_device *dev, rt_size_t size,
                   rt_ubase_t *dma_handle, rt_ubase_t flags)
{
    const struct rt_dma_map_ops *ops = rt_dma_h7rs_get_ops(dev);

    if (ops->alloc != RT_NULL)
    {
        return ops->alloc(dev, size, dma_handle, flags);
    }
    return RT_NULL;
}

void rt_dma_free(struct rt_device *dev, rt_size_t size, void *cpu_addr,
                 rt_ubase_t dma_handle, rt_ubase_t flags)
{
    const struct rt_dma_map_ops *ops = rt_dma_h7rs_get_ops(dev);

    if (ops->free != RT_NULL)
    {
        ops->free(dev, size, cpu_addr, dma_handle, flags);
    }
}

rt_err_t rt_dma_sync_out_data(struct rt_device *dev, void *data,
                              rt_size_t size, rt_ubase_t *dma_handle,
                              rt_ubase_t flags)
{
    const struct rt_dma_map_ops *ops = rt_dma_h7rs_get_ops(dev);

    if (ops->sync_out_data != RT_NULL)
    {
        return ops->sync_out_data(dev, data, size, dma_handle, flags);
    }
    return -RT_ENOSYS;
}

rt_err_t rt_dma_sync_in_data(struct rt_device *dev, void *out_data,
                             rt_size_t size, rt_ubase_t dma_handle,
                             rt_ubase_t flags)
{
    const struct rt_dma_map_ops *ops = rt_dma_h7rs_get_ops(dev);

    if (ops->sync_in_data != RT_NULL)
    {
        return ops->sync_in_data(dev, out_data, size, dma_handle, flags);
    }
    return -RT_ENOSYS;
}

struct rt_dma_pool *rt_dma_pool_install(rt_region_t *region)
{
    RT_UNUSED(region);
    return RT_NULL;
}

rt_err_t rt_dma_pool_extract(rt_size_t cma_size, rt_size_t coherent_pool_size)
{
    RT_UNUSED(cma_size);
    RT_UNUSED(coherent_pool_size);
    return rt_dma_h7rs_init();
}

int rt_dma_buffer_init(rt_dma_buffer_t *buffer, void *cpu_addr,
                       rt_size_t size, rt_ubase_t dma_addr,
                       rt_ubase_t flags)
{
    if (buffer == RT_NULL || !rt_dma_h7rs_range_valid(cpu_addr, size))
    {
        return -RT_EINVAL;
    }
    buffer->magic = RT_DMA_H7RS_MAGIC;
    buffer->cpu_addr = cpu_addr;
    buffer->dma_addr = dma_addr != 0U ? dma_addr :
                       (rt_ubase_t)(rt_uintptr_t)cpu_addr;
    buffer->size = size;
    buffer->flags = flags;
    buffer->owner = RT_DMA_BUFFER_CPU_OWNED;
    return RT_EOK;
}

int rt_dma_buffer_prepare_for_device(rt_dma_buffer_t *buffer)
{
    if (buffer == RT_NULL || buffer->magic != RT_DMA_H7RS_MAGIC ||
        buffer->owner != RT_DMA_BUFFER_CPU_OWNED)
    {
        s_dma_owner_errors++;
        return -RT_EBUSY;
    }
    if (rt_dma_sync_out_data(rt_dma_h7rs_device(), buffer->cpu_addr,
                             buffer->size,
                             &buffer->dma_addr, buffer->flags) != RT_EOK)
    {
        return -RT_ERROR;
    }
    __DMB();
    buffer->owner = RT_DMA_BUFFER_DEVICE_OWNED;
    return RT_EOK;
}

int rt_dma_buffer_complete_for_cpu(rt_dma_buffer_t *buffer)
{
    if (buffer == RT_NULL || buffer->magic != RT_DMA_H7RS_MAGIC ||
        buffer->owner != RT_DMA_BUFFER_DEVICE_OWNED)
    {
        s_dma_owner_errors++;
        return -RT_EBUSY;
    }
    if (rt_dma_sync_in_data(rt_dma_h7rs_device(), buffer->cpu_addr,
                            buffer->size,
                            buffer->dma_addr, buffer->flags) != RT_EOK)
    {
        return -RT_ERROR;
    }
    __DMB();
    buffer->owner = RT_DMA_BUFFER_CPU_OWNED;
    return RT_EOK;
}

int rt_dma_buffer_release_device(rt_dma_buffer_t *buffer)
{
    return rt_dma_buffer_prepare_for_device(buffer);
}

int rt_dma_buffer_acquire_cpu(rt_dma_buffer_t *buffer)
{
    return rt_dma_buffer_complete_for_cpu(buffer);
}

void rt_dma_h7rs_status(void)
{
    rt_kprintf("[DMA] ready=%d alloc=%lu free=%lu sync_out=%lu sync_in=%lu "
               "bypass=%lu owner_err=%lu\r\n",
               s_dma_initialized ? 1 : 0,
               (unsigned long)s_dma_allocations,
               (unsigned long)s_dma_frees,
               (unsigned long)s_dma_sync_out,
               (unsigned long)s_dma_sync_in,
               (unsigned long)s_dma_sync_bypass,
               (unsigned long)s_dma_owner_errors);
}

#ifdef RT_USING_FINSH
static void dma_contract_test_cmd(void)
{
    rt_dma_buffer_t buffer;
    rt_ubase_t dma_addr = 0U;
    rt_ubase_t cached_dma_addr = 0U;
    rt_uint8_t *data;
    rt_uint8_t *cached_data;
    int result;

    data = (rt_uint8_t *)rt_dma_alloc(rt_dma_h7rs_device(), 256U, &dma_addr,
                                      RT_DMA_F_NOCACHE);
    if (data == RT_NULL)
    {
        rt_kprintf("[DMA] contract test allocation failed\r\n");
        return;
    }
    for (rt_uint32_t index = 0U; index < 256U; index++)
    {
        data[index] = (rt_uint8_t)(index ^ 0x5AU);
    }

    result = rt_dma_buffer_init(&buffer, data, 256U, dma_addr,
                                RT_DMA_F_NOCACHE);
    if (result == RT_EOK)
    {
        result = rt_dma_buffer_prepare_for_device(&buffer);
    }
    if (result == RT_EOK)
    {
        result = rt_dma_buffer_complete_for_cpu(&buffer);
    }
    if (result == RT_EOK && data[127U] != (rt_uint8_t)(127U ^ 0x5AU))
    {
        result = -RT_ERROR;
    }
    rt_dma_free(rt_dma_h7rs_device(), 256U, data, dma_addr,
                RT_DMA_F_NOCACHE);

    /* Exercise the streaming path as well.  The default allocation is from
     * the cacheable RT-Thread heap, so prepare/complete must perform real
     * D-Cache maintenance rather than taking the non-cacheable fast path. */
    cached_data = (rt_uint8_t *)rt_dma_alloc(rt_dma_h7rs_device(), 256U,
                                             &cached_dma_addr, 0U);
    if (cached_data == RT_NULL ||
        rt_dma_buffer_init(&buffer, cached_data, 256U, cached_dma_addr,
                           0U) != RT_EOK)
    {
        result = -RT_ENOMEM;
    }
    else
    {
        for (rt_uint32_t index = 0U; index < 256U; index++)
        {
            cached_data[index] = (rt_uint8_t)(index ^ 0xA5U);
        }
        if (rt_dma_buffer_prepare_for_device(&buffer) != RT_EOK ||
            rt_dma_buffer_complete_for_cpu(&buffer) != RT_EOK ||
            cached_data[127U] != (rt_uint8_t)(127U ^ 0xA5U))
        {
            result = -RT_ERROR;
        }
    }
    if (cached_data != RT_NULL)
    {
        rt_dma_free(rt_dma_h7rs_device(), 256U, cached_data,
                    cached_dma_addr, 0U);
    }
    rt_kprintf("[DMA] contract test %s (NC+cache owner CPU->DEVICE->CPU)\r\n",
               result == RT_EOK ? "PASS" : "FAIL");
}
MSH_CMD_EXPORT(dma_contract_test_cmd, test RT-Thread H7RS DMA ownership contract);
#endif

#ifdef RT_USING_FINSH
static void dma_status_cmd(void)
{
    rt_dma_h7rs_status();
}
MSH_CMD_EXPORT(dma_status_cmd, show RT-Thread H7RS DMA contract status);
#endif
