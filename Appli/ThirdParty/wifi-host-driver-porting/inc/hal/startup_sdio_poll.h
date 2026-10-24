#ifndef STARTUP_SDIO_POLL_H
#define STARTUP_SDIO_POLL_H

/*
 * Small, allocation-free startup poll gate shared by the SDIO port and host
 * tests. The caller owns status/error handling and the fallback sleep. Create
 * one gate per wait operation, before its polling loop, and do not reinitialize
 * it after yielding: that gives the loop one initial spin window only.
 *
 * Pass a DWT CYCCNT reader only when the counter is available and enabled.
 * A NULL reader or zero core clock disables spinning, so the caller can keep
 * using its existing sleep path. Cycle subtraction is modulo 2^32; configured
 * windows are capped at 2^31-1 cycles to keep the elapsed comparison
 * unambiguous across counter wrap.
 */

#include <stdint.h>

#define STARTUP_SDIO_COMMAND_SPIN_US 32U
#define STARTUP_SDIO_DATA_SPIN_US   128U

typedef uint32_t (*startup_sdio_cycle_reader_t)(void *context);

/* The caller checks error flags first. Data completion alone is insufficient:
 * the response must have completed before its command index is consumed. */
static inline uint8_t startup_sdio_transfer_finished(uint32_t status,
    uint32_t completion_mask, uint32_t active_mask)
{
    return (status & completion_mask) == completion_mask &&
           (status & active_mask) == 0U;
}

typedef struct
{
    uint32_t start_cycle;
    uint32_t window_cycles;
    startup_sdio_cycle_reader_t read_cycles;
    void *context;
    uint8_t window_open;
} startup_sdio_poll_gate_t;

static inline uint32_t startup_sdio_poll_cycles_for_us(
    uint32_t system_core_clock_hz,
    uint32_t window_us)
{
    uint64_t scaled_cycles;
    uint64_t rounded_cycles;

    if (system_core_clock_hz == 0U || window_us == 0U)
    {
        return 0U;
    }

    scaled_cycles = (uint64_t)system_core_clock_hz * (uint64_t)window_us;
    rounded_cycles = (scaled_cycles + UINT64_C(999999)) /
                     UINT64_C(1000000);
    if (rounded_cycles == 0U || rounded_cycles > UINT32_C(0x7fffffff))
    {
        return 0U;
    }

    return (uint32_t)rounded_cycles;
}

static inline void startup_sdio_poll_gate_begin(
    startup_sdio_poll_gate_t *gate,
    uint32_t system_core_clock_hz,
    uint32_t window_us,
    startup_sdio_cycle_reader_t read_cycles,
    void *context)
{
    uint32_t window_cycles;

    if (gate == (startup_sdio_poll_gate_t *)0)
    {
        return;
    }

    gate->start_cycle = 0U;
    gate->window_cycles = 0U;
    gate->read_cycles = (startup_sdio_cycle_reader_t)0;
    gate->context = context;
    gate->window_open = 0U;

    window_cycles = startup_sdio_poll_cycles_for_us(system_core_clock_hz,
                                                     window_us);
    if (window_cycles == 0U || read_cycles ==
        (startup_sdio_cycle_reader_t)0)
    {
        return;
    }

    gate->read_cycles = read_cycles;
    gate->window_cycles = window_cycles;
    gate->start_cycle = read_cycles(context);
    gate->window_open = 1U;
}

/* Return nonzero when the caller should take its existing sleep/yield path. */
static inline uint8_t startup_sdio_poll_gate_should_yield(
    startup_sdio_poll_gate_t *gate)
{
    uint32_t elapsed_cycles;

    if (gate == (startup_sdio_poll_gate_t *)0 || gate->window_open == 0U ||
        gate->read_cycles == (startup_sdio_cycle_reader_t)0)
    {
        return 1U;
    }

    /* Unsigned subtraction intentionally handles CYCCNT wraparound. */
    elapsed_cycles = gate->read_cycles(gate->context) - gate->start_cycle;
    if (elapsed_cycles < gate->window_cycles)
    {
        return 0U;
    }

    gate->window_open = 0U;
    return 1U;
}

#endif /* STARTUP_SDIO_POLL_H */
