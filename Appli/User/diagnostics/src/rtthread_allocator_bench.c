/*
 * RT-Thread 5.3 allocator A/B benchmark.
 *
 * The command intentionally measures the allocator selected at build time;
 * it does not replace the system heap or create a second compatibility API.
 * Run it once after boot on each CMake allocator profile and compare the
 * reported p99 allocation/free latency, high-water mark, and integrity result.
 */
#include <stdint.h>
#include <stdlib.h>

#include <rtthread.h>
#include <finsh.h>

#include "stm32h7rsxx.h"

#ifndef WORK_RTTHREAD_ALLOCATOR
#define WORK_RTTHREAD_ALLOCATOR 0
#endif

#define ALLOC_BENCH_SLOTS          32U
#define ALLOC_BENCH_DEFAULT_ROUNDS 24U
#define ALLOC_BENCH_MAX_SAMPLES    (ALLOC_BENCH_SLOTS * 32U)

extern uint32_t SystemCoreClock;

static rt_uint32_t s_alloc_cycles[ALLOC_BENCH_MAX_SAMPLES];
static rt_uint32_t s_free_cycles[ALLOC_BENCH_MAX_SAMPLES];
static rt_uint32_t s_sort_scratch[ALLOC_BENCH_MAX_SAMPLES];

static const char *allocator_name(void)
{
#if WORK_RTTHREAD_ALLOCATOR == 0
    return "smallmem";
#elif WORK_RTTHREAD_ALLOCATOR == 1
    return "memheap";
#elif WORK_RTTHREAD_ALLOCATOR == 2
    return "slab";
#else
    return "unknown";
#endif
}

static rt_uint32_t bench_cycles(void)
{
    return DWT->CYCCNT;
}

static rt_size_t bench_size(rt_uint32_t slot, rt_uint32_t round)
{
    /* Mix small objects, cache-line-sized objects, and medium buffers. */
    return 16U + ((slot * 97U + round * 53U) % 4081U);
}

static void sort_cycles(rt_uint32_t *values, rt_uint32_t count)
{
    rt_uint32_t i;

    for (i = 1U; i < count; i++)
    {
        rt_uint32_t value = values[i];
        rt_uint32_t j = i;

        while (j > 0U && values[j - 1U] > value)
        {
            values[j] = values[j - 1U];
            j--;
        }
        values[j] = value;
    }
}

static rt_uint32_t percentile99(const rt_uint32_t *values, rt_uint32_t count)
{
    rt_uint32_t i;
    rt_uint32_t index;

    if (count == 0U)
    {
        return 0U;
    }
    for (i = 0U; i < count; i++)
    {
        s_sort_scratch[i] = values[i];
    }
    sort_cycles(s_sort_scratch, count);
    index = (count * 99U + 99U) / 100U;
    if (index == 0U)
    {
        index = 1U;
    }
    return s_sort_scratch[index - 1U];
}

static void bench_print_stats(const char *kind,
                              const rt_uint32_t *values,
                              rt_uint32_t count)
{
    rt_uint32_t i;
    rt_uint32_t min_value = UINT32_MAX;
    rt_uint32_t max_value = 0U;
    uint64_t total = 0ULL;

    for (i = 0U; i < count; i++)
    {
        if (values[i] < min_value)
        {
            min_value = values[i];
        }
        if (values[i] > max_value)
        {
            max_value = values[i];
        }
        total += values[i];
    }
    if (count == 0U)
    {
        min_value = 0U;
    }

    rt_kprintf("[ALLOC] %s samples=%lu min=%lu avg=%lu p99=%lu max=%lu cycles\r\n",
               kind,
               (unsigned long)count,
               (unsigned long)min_value,
               (unsigned long)(total / (count == 0U ? 1U : count)),
               (unsigned long)percentile99(values, count),
               (unsigned long)max_value);
}

static void allocator_bench_cmd(int argc, char **argv)
{
    void *slots[ALLOC_BENCH_SLOTS];
    rt_size_t sizes[ALLOC_BENCH_SLOTS];
    rt_uint32_t rounds = ALLOC_BENCH_DEFAULT_ROUNDS;
    rt_uint32_t alloc_count = 0U;
    rt_uint32_t free_count = 0U;
    rt_uint32_t round;
    rt_uint32_t slot;
    rt_uint32_t failures = 0U;
    rt_uint32_t corruptions = 0U;
    rt_size_t total_before = 0U;
    rt_size_t used_before = 0U;
    rt_size_t max_before = 0U;
    rt_size_t total_after = 0U;
    rt_size_t used_after = 0U;
    rt_size_t max_after = 0U;

    if (argc >= 2)
    {
        unsigned long requested = strtoul(argv[1], RT_NULL, 0);
        if (requested >= 1UL && requested <= 32UL)
        {
            rounds = (rt_uint32_t)requested;
        }
    }

    if (SystemCoreClock == 0U)
    {
        rt_kprintf("[ALLOC] SystemCoreClock is zero\r\n");
        return;
    }

    if ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) == 0U)
    {
        rt_kprintf("[ALLOC] shared DWT counter is unavailable; benchmark skipped\r\n");
        return;
    }
    rt_memory_info(&total_before, &used_before, &max_before);
    rt_memset(slots, 0, sizeof(slots));

    rt_kprintf("[ALLOC] profile=%s rounds=%lu clock=%luHz before=%lu/%lu/%lu\r\n",
               allocator_name(),
               (unsigned long)rounds,
               (unsigned long)SystemCoreClock,
               (unsigned long)total_before,
               (unsigned long)used_before,
               (unsigned long)max_before);

    for (round = 0U; round < rounds; round++)
    {
        for (slot = 0U; slot < ALLOC_BENCH_SLOTS; slot++)
        {
            rt_uint32_t start;
            rt_uint32_t end;
            rt_uint8_t *bytes;

            sizes[slot] = bench_size(slot, round);
            start = bench_cycles();
            slots[slot] = rt_malloc(sizes[slot]);
            end = bench_cycles();
            if (alloc_count < ALLOC_BENCH_MAX_SAMPLES)
            {
                s_alloc_cycles[alloc_count++] = end - start;
            }
            if (slots[slot] == RT_NULL)
            {
                failures++;
                continue;
            }

            bytes = (rt_uint8_t *)slots[slot];
            bytes[0] = (rt_uint8_t)(slot ^ round ^ 0x5AU);
            bytes[sizes[slot] - 1U] = (rt_uint8_t)(slot ^ round ^ 0xA5U);
        }

        /* Create holes, then refill them with a different size. */
        for (slot = 0U; slot < ALLOC_BENCH_SLOTS; slot += 2U)
        {
            if (slots[slot] != RT_NULL)
            {
                rt_uint32_t start = bench_cycles();
                rt_free(slots[slot]);
                rt_uint32_t end = bench_cycles();

                if (free_count < ALLOC_BENCH_MAX_SAMPLES)
                {
                    s_free_cycles[free_count++] = end - start;
                }
                slots[slot] = RT_NULL;
            }
        }
        for (slot = 0U; slot < ALLOC_BENCH_SLOTS; slot += 2U)
        {
            sizes[slot] = bench_size(slot + 11U, round + 3U);
            slots[slot] = rt_malloc(sizes[slot]);
            if (slots[slot] == RT_NULL)
            {
                failures++;
            }
        }

        for (slot = 0U; slot < ALLOC_BENCH_SLOTS; slot++)
        {
            if (slots[slot] != RT_NULL)
            {
                rt_uint8_t *bytes = (rt_uint8_t *)slots[slot];
                if (bytes[0] != (rt_uint8_t)(slot ^ round ^ 0x5AU) &&
                    (slot & 1U) != 0U)
                {
                    corruptions++;
                }
                rt_free(slots[slot]);
                slots[slot] = RT_NULL;
            }
        }
    }

    rt_memory_info(&total_after, &used_after, &max_after);
    bench_print_stats("alloc", s_alloc_cycles, alloc_count);
    bench_print_stats("free", s_free_cycles, free_count);
    rt_kprintf("[ALLOC] result=%s failures=%lu corruptions=%lu after=%lu/%lu/%lu\r\n",
               failures == 0U && corruptions == 0U ? "PASS" : "FAIL",
               (unsigned long)failures,
               (unsigned long)corruptions,
               (unsigned long)total_after,
               (unsigned long)used_after,
               (unsigned long)max_after);
}
MSH_CMD_EXPORT(allocator_bench_cmd, run RT-Thread 5.3 allocator benchmark);
