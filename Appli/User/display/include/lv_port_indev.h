/*
 * lv_port_indev.h - LVGL input device port (GT911 touch)
 */
#ifndef LV_PORT_INDEV_H
#define LV_PORT_INDEV_H

int lvgl_indev_init(void);

/* Read the already-buffered touch events from the LVGL owner thread.  The
 * GT911 I2C transaction is performed by its RT-Thread worker, never here. */
void lvgl_indev_poll(void);

#endif /* LV_PORT_INDEV_H */
