/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WPM_NV_JOURNAL_H
#define WPM_NV_JOURNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Journal v1 is a two-sector append log with 16-byte program records. */
#define WPM_NV_JOURNAL_SECTOR_COUNT 2U
#define WPM_NV_JOURNAL_PROGRAM_SIZE 16U
#define WPM_NV_JOURNAL_MAX_PAYLOAD 2048U

typedef enum wpm_nv_journal_status
{
    WPM_NV_JOURNAL_OK = 0,
    WPM_NV_JOURNAL_EMPTY = 1,
    WPM_NV_JOURNAL_ESCRUB = 2,
    WPM_NV_JOURNAL_EINVAL = -1,
    WPM_NV_JOURNAL_ECAPACITY = -2,
    WPM_NV_JOURNAL_ECORRUPT = -3,
    WPM_NV_JOURNAL_EFOREIGN = -4,
    WPM_NV_JOURNAL_EIO = -5,
    WPM_NV_JOURNAL_ENOSPACE = -6,
    WPM_NV_JOURNAL_EBUSY = -7,
    WPM_NV_JOURNAL_ESEQUENCE = -8
} wpm_nv_journal_status_t;

typedef struct wpm_nv_journal_io
{
    void *context;
    /* Addresses passed to read/program16 are offsets from the region start. */
    int (*read)(void *context, uint32_t offset, uint8_t *data, size_t length);
    int (*program16)(void *context, uint32_t offset,
                     const uint8_t data[WPM_NV_JOURNAL_PROGRAM_SIZE]);
    /* Erase one sector by index: 0 or 1. */
    int (*erase_sector)(void *context, uint32_t sector_index);
} wpm_nv_journal_io_t;

typedef struct wpm_nv_journal_geometry
{
    uint32_t sector_size;
    uint32_t max_payload;
    uint8_t sector_count;
    uint8_t program_unit;
    uint8_t erased_value;
} wpm_nv_journal_geometry_t;

/* Caller-owned state. Zero-initialize before the first init call. */
typedef struct wpm_nv_journal
{
    wpm_nv_journal_io_t io;
    wpm_nv_journal_geometry_t geometry;
    uint8_t initialized;
    uint8_t busy;
} wpm_nv_journal_t;

#define WPM_NV_JOURNAL_INITIALIZER { { 0 }, { 0 }, 0U, 0U }

/*
 * Initialize or reinitialize one journal instance. v1 requires exactly two
 * equal erase sectors, 16-byte program units, 0xFF erased bytes, and a
 * maximum payload no larger than WPM_NV_JOURNAL_MAX_PAYLOAD. Callers must
 * serialize operations; EBUSY detects callback reentry and is not a mutex.
 * There is no implicit hardware backend.
 */
int wpm_nv_journal_init(wpm_nv_journal_t *journal,
                        const wpm_nv_journal_io_t *io,
                        const wpm_nv_journal_geometry_t *geometry);

/*
 * Explicitly erase and verify every sector in this journal's geometry.
 * This is destructive and is never performed by init/load implicitly.
 * Returns OK only after every byte reads as the configured erased value.
 */
int wpm_nv_journal_reset(wpm_nv_journal_t *journal);

/* A blank region or committed zero-length payload returns EMPTY. */
int wpm_nv_journal_load(wpm_nv_journal_t *journal, uint8_t *out, size_t cap,
                        size_t *length, uint64_t *sequence);

/*
 * scrub_old writes and verifies the new committed value before retiring the
 * old sector. ESCRUB means the new value is committed but old-sector cleanup
 * remains pending. On an uncertain I/O error, reinitialize and load before
 * deciding whether to retry. CRC detects accidental damage; it is not
 * encryption or authentication.
 */
int wpm_nv_journal_commit(wpm_nv_journal_t *journal,
                          const uint8_t *payload, size_t length,
                          bool scrub_old, uint64_t *sequence);

#ifdef __cplusplus
}
#endif

#endif
