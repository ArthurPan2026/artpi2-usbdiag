/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ARTPI2_WATCHDOG_INCIDENT_ID_H
#define ARTPI2_WATCHDOG_INCIDENT_ID_H

#include "watchdog_record.h"

/* Journal transaction helpers. The V2 footer wire offsets are intentionally
 * private to the record/Boot implementations; this header only shares APIs. */
int watchdog_record_incident_id_valid(
    const volatile watchdog_retained_record_t *record, uint64_t *incident_id);
void watchdog_record_incident_id_commit(
    volatile watchdog_retained_record_t *record, uint64_t incident_id);
void watchdog_record_incident_id_clear(
    volatile watchdog_retained_record_t *record);
int watchdog_record_incident_id_next(uint64_t latest_id, uint64_t *next_id);

#endif
