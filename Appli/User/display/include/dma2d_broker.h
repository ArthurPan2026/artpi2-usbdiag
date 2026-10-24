#ifndef DMA2D_BROKER_H
#define DMA2D_BROKER_H

#include <rtthread.h>
#include <stdint.h>

typedef enum {
    DMA2D_BROKER_OWNER_NONE = 0,
    DMA2D_BROKER_OWNER_SERVICE,
    DMA2D_BROKER_OWNER_LVGL,
} dma2d_broker_owner_t;

int dma2d_broker_init(void);
int dma2d_broker_acquire(dma2d_broker_owner_t owner, rt_int32_t timeout);
void dma2d_broker_release(dma2d_broker_owner_t owner);
void dma2d_broker_clear_flags(void);
void dma2d_broker_set_error(dma2d_broker_owner_t owner, uint32_t error);
uint32_t dma2d_broker_last_error(dma2d_broker_owner_t owner);
uint32_t dma2d_broker_generation(void);
dma2d_broker_owner_t dma2d_broker_owner(void);
void dma2d_broker_irq(void);

#endif /* DMA2D_BROKER_H */
