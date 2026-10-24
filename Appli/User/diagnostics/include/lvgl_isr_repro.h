/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WORK_LVGL_ISR_REPRO_H
#define WORK_LVGL_ISR_REPRO_H

#include <rtthread.h>

#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
void lvgl_isr_repro_systick(void);
#endif

#endif
