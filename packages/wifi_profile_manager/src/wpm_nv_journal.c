#include "wpm_nv_journal.h"

#include <string.h>

#define WPM_NV_JOURNAL_RECORD_MAGIC       0x31564E57UL
#define WPM_NV_JOURNAL_COMMIT_MAGIC       0x54494D43UL
#define WPM_NV_JOURNAL_FORMAT_VERSION     1U
#define WPM_NV_JOURNAL_FLAG_SCRUB         0x0001U
#define WPM_NV_JOURNAL_HEADER_SIZE        32U
#define WPM_NV_JOURNAL_COMMIT_SIZE        16U

typedef struct wpm_nv_journal_record
{
    uint32_t sector;
    uint32_t offset;
    uint32_t total_size;
    uint32_t length;
    uint32_t payload_crc;
    uint16_t flags;
    uint64_t sequence;
    int valid;
} wpm_nv_journal_record_t;

typedef struct wpm_nv_journal_sector_scan
{
    uint32_t append_offset;
    uint64_t committed_min;
    uint64_t committed_max;
    int blank;
    int unknown;
    int corrupt;
    int fatal;
} wpm_nv_journal_sector_scan_t;

typedef struct wpm_nv_journal_scan
{
    wpm_nv_journal_sector_scan_t sectors[WPM_NV_JOURNAL_SECTOR_COUNT];
    wpm_nv_journal_record_t latest;
    uint64_t scrub_sequence;
    int have_scrub;
    int duplicate_sequence;
} wpm_nv_journal_scan_t;

static uint32_t wpm_nv_journal_sector_address(
    const wpm_nv_journal_t *journal, uint32_t sector)
{
    return sector * journal->geometry.sector_size;
}

static int wpm_nv_journal_geometry_valid(
    const wpm_nv_journal_geometry_t *geometry)
{
    uint32_t padded_max;

    if (geometry == NULL ||
        geometry->sector_count != WPM_NV_JOURNAL_SECTOR_COUNT ||
        geometry->program_unit != WPM_NV_JOURNAL_PROGRAM_SIZE ||
        geometry->erased_value != 0xFFU ||
        geometry->sector_size < WPM_NV_JOURNAL_HEADER_SIZE +
                                WPM_NV_JOURNAL_COMMIT_SIZE ||
        (geometry->sector_size % WPM_NV_JOURNAL_PROGRAM_SIZE) != 0U ||
        geometry->sector_size > UINT32_MAX / WPM_NV_JOURNAL_SECTOR_COUNT ||
        geometry->max_payload > WPM_NV_JOURNAL_MAX_PAYLOAD)
    {
        return 0;
    }
    padded_max = (geometry->max_payload + WPM_NV_JOURNAL_PROGRAM_SIZE - 1U) &
                 ~(WPM_NV_JOURNAL_PROGRAM_SIZE - 1U);
    return padded_max <= geometry->sector_size -
                         WPM_NV_JOURNAL_HEADER_SIZE -
                         WPM_NV_JOURNAL_COMMIT_SIZE;
}

static uint16_t wpm_nv_journal_get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t wpm_nv_journal_get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t wpm_nv_journal_get_u64(const uint8_t *p)
{
    return (uint64_t)wpm_nv_journal_get_u32(p) |
           ((uint64_t)wpm_nv_journal_get_u32(p + 4) << 32);
}

static void wpm_nv_journal_put_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void wpm_nv_journal_put_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void wpm_nv_journal_put_u64(uint8_t *p, uint64_t value)
{
    wpm_nv_journal_put_u32(p, (uint32_t)value);
    wpm_nv_journal_put_u32(p + 4, (uint32_t)(value >> 32));
}

static uint32_t wpm_nv_journal_crc_update(uint32_t crc, const uint8_t *data,
                                   size_t length)
{
    size_t i;
    uint32_t bit;

    for (i = 0U; i < length; i++)
    {
        crc ^= data[i];
        for (bit = 0U; bit < 8U; bit++)
        {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1) ^ (0xEDB88320UL & mask);
        }
    }
    return crc;
}

static uint32_t wpm_nv_journal_crc(const uint8_t *data, size_t length)
{
    return wpm_nv_journal_crc_update(0xFFFFFFFFUL, data, length) ^ 0xFFFFFFFFUL;
}

static int wpm_nv_journal_is_erased(const uint8_t data[WPM_NV_JOURNAL_PROGRAM_SIZE])
{
    size_t i;

    for (i = 0U; i < WPM_NV_JOURNAL_PROGRAM_SIZE; i++)
    {
        if (data[i] != 0xFFU)
        {
            return 0;
        }
    }
    return 1;
}

static uint32_t wpm_nv_journal_record_size(uint32_t length)
{
    uint32_t padded = (length + WPM_NV_JOURNAL_PROGRAM_SIZE - 1U) &
                      ~(WPM_NV_JOURNAL_PROGRAM_SIZE - 1U);
    return WPM_NV_JOURNAL_HEADER_SIZE + padded + WPM_NV_JOURNAL_COMMIT_SIZE;
}

static int wpm_nv_journal_read(wpm_nv_journal_t *journal, uint32_t offset,
                               uint8_t *data, size_t length)
{
    if (journal->io.read(journal->io.context, offset, data, length) != 0)
    {
        return WPM_NV_JOURNAL_EIO;
    }
    return WPM_NV_JOURNAL_OK;
}

static int wpm_nv_journal_read_payload_crc(wpm_nv_journal_t *journal,
                                           uint32_t address, uint32_t length,
                                           uint32_t *payload_crc)
{
    uint8_t word[WPM_NV_JOURNAL_PROGRAM_SIZE];
    uint32_t crc = 0xFFFFFFFFUL;
    uint32_t offset;
    uint32_t remaining = length;

    for (offset = 0U; offset <
         ((length + WPM_NV_JOURNAL_PROGRAM_SIZE - 1U) &
          ~(WPM_NV_JOURNAL_PROGRAM_SIZE - 1U));
         offset += WPM_NV_JOURNAL_PROGRAM_SIZE)
    {
        size_t data_bytes = remaining > WPM_NV_JOURNAL_PROGRAM_SIZE ?
                            WPM_NV_JOURNAL_PROGRAM_SIZE : remaining;
        size_t i;

        if (wpm_nv_journal_read(journal, address + offset, word,
                                sizeof(word)) != WPM_NV_JOURNAL_OK)
        {
            return WPM_NV_JOURNAL_EIO;
        }
        crc = wpm_nv_journal_crc_update(crc, word, data_bytes);
        for (i = data_bytes; i < WPM_NV_JOURNAL_PROGRAM_SIZE; i++)
        {
            if (word[i] != 0xFFU)
            {
                return WPM_NV_JOURNAL_ECORRUPT;
            }
        }
        remaining -= (uint32_t)data_bytes;
    }

    *payload_crc = crc ^ 0xFFFFFFFFUL;
    return WPM_NV_JOURNAL_OK;
}

static int wpm_nv_journal_note_record(wpm_nv_journal_scan_t *scan,
                               const wpm_nv_journal_record_t *record)
{
    wpm_nv_journal_sector_scan_t *sector_scan = &scan->sectors[record->sector];

    if (sector_scan->committed_min == 0U)
    {
        sector_scan->committed_min = record->sequence;
    }
    if (sector_scan->committed_max != 0U &&
        record->sequence <= sector_scan->committed_max)
    {
        sector_scan->fatal = 1;
    }
    if (record->sequence > sector_scan->committed_max)
    {
        sector_scan->committed_max = record->sequence;
    }
    if (scan->latest.valid && scan->latest.sequence == record->sequence)
    {
        scan->duplicate_sequence = 1;
        return WPM_NV_JOURNAL_OK;
    }
    if (!scan->latest.valid || record->sequence > scan->latest.sequence)
    {
        scan->latest = *record;
    }
    if ((record->flags & WPM_NV_JOURNAL_FLAG_SCRUB) != 0U &&
        (!scan->have_scrub || record->sequence > scan->scrub_sequence))
    {
        scan->have_scrub = 1;
        scan->scrub_sequence = record->sequence;
    }
    return WPM_NV_JOURNAL_OK;
}

static int wpm_nv_journal_scan_sector(wpm_nv_journal_t *journal,
                                      wpm_nv_journal_scan_t *scan,
                                      uint32_t sector)
{
    wpm_nv_journal_sector_scan_t *sector_scan = &scan->sectors[sector];
    uint32_t base = wpm_nv_journal_sector_address(journal, sector);
    uint32_t offset = 0U;
    uint8_t first[WPM_NV_JOURNAL_PROGRAM_SIZE];

    sector_scan->blank = 1;
    sector_scan->append_offset = journal->geometry.sector_size;

    while (offset + WPM_NV_JOURNAL_PROGRAM_SIZE <=
           journal->geometry.sector_size)
    {
        uint32_t magic;
        uint32_t length;
        uint32_t total_size;
        uint32_t payload_crc;
        uint8_t second[WPM_NV_JOURNAL_PROGRAM_SIZE];
        uint8_t commit[WPM_NV_JOURNAL_PROGRAM_SIZE];
        uint8_t crc_input[24];
        uint16_t version;
        uint16_t flags;
        uint64_t sequence;
        int payload_status;

        if (wpm_nv_journal_read(journal, base + offset, first,
                                sizeof(first)) != WPM_NV_JOURNAL_OK)
        {
            return WPM_NV_JOURNAL_EIO;
        }
        if (wpm_nv_journal_is_erased(first))
        {
            uint32_t tail;

            sector_scan->append_offset = offset;
            for (tail = offset + WPM_NV_JOURNAL_PROGRAM_SIZE;
                 tail + WPM_NV_JOURNAL_PROGRAM_SIZE <=
                     journal->geometry.sector_size;
                 tail += WPM_NV_JOURNAL_PROGRAM_SIZE)
            {
                uint8_t word[WPM_NV_JOURNAL_PROGRAM_SIZE];
                if (wpm_nv_journal_read(journal, base + tail, word,
                                        sizeof(word)) != WPM_NV_JOURNAL_OK)
                {
                    return WPM_NV_JOURNAL_EIO;
                }
                if (!wpm_nv_journal_is_erased(word))
                {
                    sector_scan->blank = 0;
                    sector_scan->unknown = 1;
                }
            }
            break;
        }

        sector_scan->blank = 0;
        magic = wpm_nv_journal_get_u32(first);
        if (magic != WPM_NV_JOURNAL_RECORD_MAGIC)
        {
            sector_scan->unknown = 1;
            break;
        }

        version = wpm_nv_journal_get_u16(first + 4);
        flags = wpm_nv_journal_get_u16(first + 6);
        if (version != WPM_NV_JOURNAL_FORMAT_VERSION ||
            (flags & (uint16_t)~WPM_NV_JOURNAL_FLAG_SCRUB) != 0U)
        {
            sector_scan->unknown = 1;
            break;
        }
        length = wpm_nv_journal_get_u32(first + 8);
        if (wpm_nv_journal_get_u32(first + 12) != ~length ||
            length > journal->geometry.max_payload)
        {
            sector_scan->corrupt = 1;
            sector_scan->fatal = 1;
            break;
        }

        total_size = wpm_nv_journal_record_size(length);
        if (total_size > journal->geometry.sector_size - offset)
        {
            sector_scan->corrupt = 1;
            sector_scan->fatal = 1;
            break;
        }
        sector_scan->append_offset = offset + total_size;

        /* An erased commit word is an interrupted, unpublished record. Its
         * entire slot is consumed, even if some payload words remain erased.
         * Any non-erased commit word is ambiguous until fully verified: do
         * not fallback to older credentials after damage to a scrub record. */
        if (wpm_nv_journal_read(
                journal,
                base + offset + total_size - WPM_NV_JOURNAL_COMMIT_SIZE,
                commit, sizeof(commit)) != WPM_NV_JOURNAL_OK)
        {
            return WPM_NV_JOURNAL_EIO;
        }
        if (wpm_nv_journal_is_erased(commit))
        {
            sector_scan->corrupt = 1;
            offset += total_size;
            continue;
        }

        if (wpm_nv_journal_read(journal,
                                base + offset + WPM_NV_JOURNAL_PROGRAM_SIZE,
                                second, sizeof(second)) != WPM_NV_JOURNAL_OK)
        {
            return WPM_NV_JOURNAL_EIO;
        }
        sequence = wpm_nv_journal_get_u64(second);
        memcpy(crc_input, first, WPM_NV_JOURNAL_PROGRAM_SIZE);
        memcpy(crc_input + WPM_NV_JOURNAL_PROGRAM_SIZE, second, 8U);
        if (sequence == 0U ||
            wpm_nv_journal_get_u32(second + 8) != wpm_nv_journal_crc(crc_input,
                                                       sizeof(crc_input)))
        {
            sector_scan->corrupt = 1;
            sector_scan->fatal = 1;
            offset += total_size;
            continue;
        }

        if (wpm_nv_journal_get_u32(commit) != WPM_NV_JOURNAL_COMMIT_MAGIC ||
            wpm_nv_journal_get_u64(commit + 4) != sequence ||
            wpm_nv_journal_get_u32(commit + 12) != wpm_nv_journal_get_u32(second + 12))
        {
            sector_scan->corrupt = 1;
            sector_scan->fatal = 1;
            offset += total_size;
            continue;
        }

        payload_status = wpm_nv_journal_read_payload_crc(journal,
            base + offset + WPM_NV_JOURNAL_HEADER_SIZE, length, &payload_crc);
        if (payload_status == WPM_NV_JOURNAL_EIO)
        {
            return WPM_NV_JOURNAL_EIO;
        }
        if (payload_status != WPM_NV_JOURNAL_OK ||
            payload_crc != wpm_nv_journal_get_u32(second + 12))
        {
            sector_scan->corrupt = 1;
            sector_scan->fatal = 1;
            offset += total_size;
            continue;
        }

        {
            wpm_nv_journal_record_t record;
            record.sector = sector;
            record.offset = offset;
            record.total_size = total_size;
            record.length = length;
            record.payload_crc = payload_crc;
            record.flags = flags;
            record.sequence = sequence;
            record.valid = 1;
            wpm_nv_journal_note_record(scan, &record);
        }
        offset += total_size;
    }

    return WPM_NV_JOURNAL_OK;
}

static int wpm_nv_journal_scan_all(wpm_nv_journal_t *journal,
                                   wpm_nv_journal_scan_t *scan)
{
    uint32_t sector;

    memset(scan, 0, sizeof(*scan));
    for (sector = 0U; sector < WPM_NV_JOURNAL_SECTOR_COUNT; sector++)
    {
        int result = wpm_nv_journal_scan_sector(journal, scan, sector);
        if (result != WPM_NV_JOURNAL_OK)
        {
            return result;
        }
    }
    if (scan->sectors[0].unknown || scan->sectors[1].unknown)
    {
        return WPM_NV_JOURNAL_EFOREIGN;
    }
    if (scan->duplicate_sequence || scan->sectors[0].fatal ||
        scan->sectors[1].fatal ||
        (scan->sectors[0].committed_min != 0U &&
         scan->sectors[1].committed_min != 0U &&
         scan->sectors[0].committed_max >= scan->sectors[1].committed_min &&
         scan->sectors[1].committed_max >= scan->sectors[0].committed_min))
    {
        return WPM_NV_JOURNAL_ECORRUPT;
    }
    return WPM_NV_JOURNAL_OK;
}

static int wpm_nv_journal_is_latest_scrub(const wpm_nv_journal_scan_t *scan)
{
    return scan->latest.valid && scan->have_scrub &&
           scan->latest.sequence == scan->scrub_sequence &&
           (scan->latest.flags & WPM_NV_JOURNAL_FLAG_SCRUB) != 0U;
}

static int wpm_nv_journal_has_unknown(const wpm_nv_journal_scan_t *scan)
{
    return scan->sectors[0].unknown || scan->sectors[1].unknown;
}

static int wpm_nv_journal_is_blank(const wpm_nv_journal_scan_t *scan)
{
    return scan->sectors[0].blank && scan->sectors[1].blank;
}

static int wpm_nv_journal_erase_sector(wpm_nv_journal_t *journal,
                                       uint32_t sector)
{
    uint32_t address = wpm_nv_journal_sector_address(journal, sector);
    uint32_t offset;
    uint8_t word[WPM_NV_JOURNAL_PROGRAM_SIZE];

    if (sector >= WPM_NV_JOURNAL_SECTOR_COUNT ||
        journal->io.erase_sector(journal->io.context, sector) != 0)
    {
        return WPM_NV_JOURNAL_EIO;
    }
    for (offset = 0U; offset < journal->geometry.sector_size;
         offset += WPM_NV_JOURNAL_PROGRAM_SIZE)
    {
        if (wpm_nv_journal_read(journal, address + offset, word,
                                sizeof(word)) != WPM_NV_JOURNAL_OK ||
            !wpm_nv_journal_is_erased(word))
        {
            return WPM_NV_JOURNAL_EIO;
        }
    }
    return WPM_NV_JOURNAL_OK;
}

static int wpm_nv_journal_resolve_scrub(wpm_nv_journal_t *journal,
                                        wpm_nv_journal_scan_t *scan)
{
    uint32_t retired_sector;
    int result;

    if (!wpm_nv_journal_is_latest_scrub(scan))
    {
        return WPM_NV_JOURNAL_OK;
    }

    retired_sector = WPM_NV_JOURNAL_SECTOR_COUNT - 1U - scan->latest.sector;
    if (scan->sectors[retired_sector].blank)
    {
        return WPM_NV_JOURNAL_OK;
    }
    if (wpm_nv_journal_has_unknown(scan))
    {
        return WPM_NV_JOURNAL_EFOREIGN;
    }

    result = wpm_nv_journal_erase_sector(journal, retired_sector);
    if (result != WPM_NV_JOURNAL_OK)
    {
        return WPM_NV_JOURNAL_ESCRUB;
    }
    result = wpm_nv_journal_scan_all(journal, scan);
    if (result != WPM_NV_JOURNAL_OK || !wpm_nv_journal_is_latest_scrub(scan) ||
        !scan->sectors[retired_sector].blank)
    {
        return WPM_NV_JOURNAL_ESCRUB;
    }
    return WPM_NV_JOURNAL_OK;
}

static int wpm_nv_journal_write_record(wpm_nv_journal_t *journal,
                                uint32_t sector, uint32_t offset,
                                uint64_t sequence, const uint8_t *payload,
                                uint32_t length, int scrub)
{
    uint8_t first[WPM_NV_JOURNAL_PROGRAM_SIZE];
    uint8_t second[WPM_NV_JOURNAL_PROGRAM_SIZE];
    uint8_t word[WPM_NV_JOURNAL_PROGRAM_SIZE];
    uint8_t commit[WPM_NV_JOURNAL_PROGRAM_SIZE];
    uint8_t crc_input[24];
    uint32_t payload_crc = wpm_nv_journal_crc(payload, length);
    uint32_t padded = (length + WPM_NV_JOURNAL_PROGRAM_SIZE - 1U) &
                      ~(WPM_NV_JOURNAL_PROGRAM_SIZE - 1U);
    uint32_t base = wpm_nv_journal_sector_address(journal, sector) + offset;
    uint32_t pos;
    int result;

    memset(first, 0xFF, sizeof(first));
    wpm_nv_journal_put_u32(first, WPM_NV_JOURNAL_RECORD_MAGIC);
    wpm_nv_journal_put_u16(first + 4, WPM_NV_JOURNAL_FORMAT_VERSION);
    wpm_nv_journal_put_u16(first + 6, scrub ? WPM_NV_JOURNAL_FLAG_SCRUB : 0U);
    wpm_nv_journal_put_u32(first + 8, length);
    wpm_nv_journal_put_u32(first + 12, ~length);

    memset(second, 0xFF, sizeof(second));
    wpm_nv_journal_put_u64(second, sequence);
    memcpy(crc_input, first, sizeof(first));
    memcpy(crc_input + sizeof(first), second, 8U);
    wpm_nv_journal_put_u32(second + 8, wpm_nv_journal_crc(crc_input, sizeof(crc_input)));
    wpm_nv_journal_put_u32(second + 12, payload_crc);

    result = journal->io.program16(journal->io.context, base, first);
    if (result != 0)
    {
        return WPM_NV_JOURNAL_EIO;
    }
    result = journal->io.program16(journal->io.context,
                                   base + WPM_NV_JOURNAL_PROGRAM_SIZE,
                                   second);
    if (result != 0)
    {
        return WPM_NV_JOURNAL_EIO;
    }

    for (pos = 0U; pos < padded; pos += WPM_NV_JOURNAL_PROGRAM_SIZE)
    {
        size_t copy_length = length - pos > WPM_NV_JOURNAL_PROGRAM_SIZE ?
                             WPM_NV_JOURNAL_PROGRAM_SIZE : (size_t)(length - pos);
        memset(word, 0xFF, sizeof(word));
        if (copy_length > 0U)
        {
            memcpy(word, payload + pos, copy_length);
        }
        result = journal->io.program16(
            journal->io.context,
            base + WPM_NV_JOURNAL_HEADER_SIZE + pos,
            word);
        if (result != 0)
        {
            return WPM_NV_JOURNAL_EIO;
        }
    }

    memset(commit, 0xFF, sizeof(commit));
    wpm_nv_journal_put_u32(commit, WPM_NV_JOURNAL_COMMIT_MAGIC);
    wpm_nv_journal_put_u64(commit + 4, sequence);
    wpm_nv_journal_put_u32(commit + 12, payload_crc);
    result = journal->io.program16(
        journal->io.context,
        base + WPM_NV_JOURNAL_HEADER_SIZE + padded,
        commit);
    if (result != 0)
    {
        return WPM_NV_JOURNAL_EIO;
    }
    return WPM_NV_JOURNAL_OK;
}

static int wpm_nv_journal_recheck_commit(wpm_nv_journal_t *journal,
                                  uint64_t sequence, uint32_t length,
                                  uint32_t payload_crc, int scrub,
                                  wpm_nv_journal_scan_t *scan)
{
    int result = wpm_nv_journal_scan_all(journal, scan);

    if (result != WPM_NV_JOURNAL_OK)
    {
        return result;
    }
    if (scan->latest.valid && scan->latest.sequence == sequence &&
        scan->latest.length == length &&
        scan->latest.payload_crc == payload_crc &&
        (((scan->latest.flags & WPM_NV_JOURNAL_FLAG_SCRUB) != 0U) == (scrub != 0)))
    {
        return WPM_NV_JOURNAL_OK;
    }
    return WPM_NV_JOURNAL_EIO;
}

int wpm_nv_journal_init(wpm_nv_journal_t *journal,
                        const wpm_nv_journal_io_t *io,
                        const wpm_nv_journal_geometry_t *geometry)
{
    wpm_nv_journal_scan_t scan;
    int result;

    if (journal == NULL)
    {
        return WPM_NV_JOURNAL_EINVAL;
    }
    if (journal->busy)
    {
        return WPM_NV_JOURNAL_EBUSY;
    }
    journal->busy = 1U;
    journal->initialized = 0U;
    memset(&journal->io, 0, sizeof(journal->io));
    memset(&journal->geometry, 0, sizeof(journal->geometry));
    if (io == NULL || io->read == NULL || io->program16 == NULL ||
        io->erase_sector == NULL || !wpm_nv_journal_geometry_valid(geometry))
    {
        journal->busy = 0U;
        return WPM_NV_JOURNAL_EINVAL;
    }
    journal->io = *io;
    journal->geometry = *geometry;
    journal->initialized = 1U;

    result = wpm_nv_journal_scan_all(journal, &scan);
    if (result != WPM_NV_JOURNAL_OK)
    {
        journal->busy = 0U;
        return result;
    }
    if (wpm_nv_journal_is_latest_scrub(&scan))
    {
        if (scan.sectors[scan.latest.sector].unknown)
        {
            journal->busy = 0U;
            return WPM_NV_JOURNAL_EFOREIGN;
        }
        result = wpm_nv_journal_resolve_scrub(journal, &scan);
        if (result != WPM_NV_JOURNAL_OK)
        {
            journal->busy = 0U;
            return result;
        }
    }
    else if (wpm_nv_journal_has_unknown(&scan))
    {
        journal->busy = 0U;
        return WPM_NV_JOURNAL_EFOREIGN;
    }

    result = !scan.latest.valid && wpm_nv_journal_is_blank(&scan) ?
             WPM_NV_JOURNAL_EMPTY :
             (scan.latest.valid ? WPM_NV_JOURNAL_OK : WPM_NV_JOURNAL_ECORRUPT);
    journal->busy = 0U;
    return result;
}

int wpm_nv_journal_reset(wpm_nv_journal_t *journal)
{
    uint32_t sector;
    int result = WPM_NV_JOURNAL_OK;

    if (journal == NULL || !journal->initialized)
    {
        return WPM_NV_JOURNAL_EINVAL;
    }
    if (journal->busy)
    {
        return WPM_NV_JOURNAL_EBUSY;
    }

    journal->busy = 1U;
    for (sector = 0U; sector < journal->geometry.sector_count; sector++)
    {
        result = wpm_nv_journal_erase_sector(journal, sector);
        if (result != WPM_NV_JOURNAL_OK)
        {
            break;
        }
    }
    journal->busy = 0U;
    return result;
}

int wpm_nv_journal_load(wpm_nv_journal_t *journal, uint8_t *out, size_t cap,
                        size_t *len, uint64_t *sequence)
{
    wpm_nv_journal_scan_t scan;
    wpm_nv_journal_record_t record;
    uint32_t base;
    uint32_t offset;
    uint32_t remaining;
    uint32_t delivered_crc = 0xFFFFFFFFUL;
    uint8_t word[WPM_NV_JOURNAL_PROGRAM_SIZE];
    int result;
    int scrub_pending = 0;

    if (journal == NULL || len == NULL || sequence == NULL)
    {
        return WPM_NV_JOURNAL_EINVAL;
    }
    *len = 0U;
    *sequence = 0U;
    if (!journal->initialized)
    {
        return WPM_NV_JOURNAL_EINVAL;
    }
    if (journal->busy)
    {
        return WPM_NV_JOURNAL_EBUSY;
    }
    journal->busy = 1U;
    result = wpm_nv_journal_scan_all(journal, &scan);
    if (result != WPM_NV_JOURNAL_OK)
    {
        journal->busy = 0U;
        return result;
    }
    if (!scan.latest.valid)
    {
        result = wpm_nv_journal_is_blank(&scan) ? WPM_NV_JOURNAL_EMPTY :
                 (wpm_nv_journal_has_unknown(&scan) ? WPM_NV_JOURNAL_EFOREIGN :
                  WPM_NV_JOURNAL_ECORRUPT);
        journal->busy = 0U;
        return result;
    }

    scrub_pending = wpm_nv_journal_is_latest_scrub(&scan) &&
                    !scan.sectors[1U - scan.latest.sector].blank;
    if (wpm_nv_journal_has_unknown(&scan))
    {
        journal->busy = 0U;
        return WPM_NV_JOURNAL_EFOREIGN;
    }
    record = scan.latest;
    if (record.length == 0U)
    {
        *len = 0U;
        *sequence = record.sequence;
        journal->busy = 0U;
        return scrub_pending ? WPM_NV_JOURNAL_ESCRUB : WPM_NV_JOURNAL_EMPTY;
    }
    if (cap < record.length)
    {
        *len = record.length;
        *sequence = record.sequence;
        journal->busy = 0U;
        return WPM_NV_JOURNAL_ECAPACITY;
    }
    if (out == NULL)
    {
        journal->busy = 0U;
        return WPM_NV_JOURNAL_EINVAL;
    }

    base = wpm_nv_journal_sector_address(journal, record.sector) + record.offset +
           WPM_NV_JOURNAL_HEADER_SIZE;
    remaining = record.length;
    for (offset = 0U; offset <
         ((record.length + WPM_NV_JOURNAL_PROGRAM_SIZE - 1U) &
          ~(WPM_NV_JOURNAL_PROGRAM_SIZE - 1U));
         offset += WPM_NV_JOURNAL_PROGRAM_SIZE)
    {
        size_t copy_length = remaining > WPM_NV_JOURNAL_PROGRAM_SIZE ?
                             WPM_NV_JOURNAL_PROGRAM_SIZE : remaining;
        size_t i;
        if (wpm_nv_journal_read(journal, base + offset, word,
                                sizeof(word)) != WPM_NV_JOURNAL_OK)
        {
            journal->busy = 0U;
            return WPM_NV_JOURNAL_EIO;
        }
        delivered_crc = wpm_nv_journal_crc_update(delivered_crc, word, copy_length);
        for (i = copy_length; i < WPM_NV_JOURNAL_PROGRAM_SIZE; i++)
        {
            if (word[i] != 0xFFU)
            {
                journal->busy = 0U;
                return WPM_NV_JOURNAL_ECORRUPT;
            }
        }
        memcpy(out + (record.length - remaining), word, copy_length);
        remaining -= (uint32_t)copy_length;
    }
    if ((delivered_crc ^ 0xFFFFFFFFUL) != record.payload_crc)
    {
        journal->busy = 0U;
        return WPM_NV_JOURNAL_ECORRUPT;
    }
    *len = record.length;
    *sequence = record.sequence;
    journal->busy = 0U;
    return scrub_pending ? WPM_NV_JOURNAL_ESCRUB : WPM_NV_JOURNAL_OK;
}

static int wpm_nv_journal_payload_matches_record(
                                          wpm_nv_journal_t *journal,
                                          const wpm_nv_journal_record_t *record,
                                          const uint8_t *payload, size_t length,
                                          int *matches)
{
    uint32_t offset;
    uint32_t base = wpm_nv_journal_sector_address(journal, record->sector) +
                    record->offset +
                    WPM_NV_JOURNAL_HEADER_SIZE;
    uint32_t crc = 0xFFFFFFFFUL;
    uint8_t word[WPM_NV_JOURNAL_PROGRAM_SIZE];

    *matches = 0;
    if (length != record->length)
    {
        return WPM_NV_JOURNAL_OK;
    }
    *matches = 1;
    for (offset = 0U; offset < record->length; offset += WPM_NV_JOURNAL_PROGRAM_SIZE)
    {
        size_t bytes = record->length - offset > WPM_NV_JOURNAL_PROGRAM_SIZE ?
                       WPM_NV_JOURNAL_PROGRAM_SIZE : record->length - offset;
        size_t i;
        if (wpm_nv_journal_read(journal, base + offset, word,
                                sizeof(word)) != WPM_NV_JOURNAL_OK)
        {
            return WPM_NV_JOURNAL_EIO;
        }
        crc = wpm_nv_journal_crc_update(crc, word, bytes);
        if (memcmp(word, payload + offset, bytes) != 0)
        {
            *matches = 0;
        }
        for (i = bytes; i < WPM_NV_JOURNAL_PROGRAM_SIZE; i++)
        {
            if (word[i] != 0xFFU)
            {
                return WPM_NV_JOURNAL_ECORRUPT;
            }
        }
    }
    return (crc ^ 0xFFFFFFFFUL) == record->payload_crc ?
           WPM_NV_JOURNAL_OK : WPM_NV_JOURNAL_ECORRUPT;
}

int wpm_nv_journal_commit(wpm_nv_journal_t *journal,
                           const uint8_t *payload, size_t len, bool scrub_old,
                           uint64_t *sequence)
{
    wpm_nv_journal_scan_t scan;
    uint32_t active_sector;
    uint32_t target_sector;
    uint32_t offset;
    uint64_t next_sequence;
    uint32_t payload_crc;
    int scrub = scrub_old ? 1 : 0;
    int retry_same = 0;
    int result;

    if (journal == NULL || sequence == NULL)
    {
        return WPM_NV_JOURNAL_EINVAL;
    }
    *sequence = 0U;
    if (payload == NULL && len != 0U)
    {
        return WPM_NV_JOURNAL_EINVAL;
    }
    if (!journal->initialized)
    {
        return WPM_NV_JOURNAL_EINVAL;
    }
    if (len > journal->geometry.max_payload)
    {
        return WPM_NV_JOURNAL_ECAPACITY;
    }
    if (journal->busy)
    {
        return WPM_NV_JOURNAL_EBUSY;
    }
    journal->busy = 1U;

    result = wpm_nv_journal_scan_all(journal, &scan);
    if (result != WPM_NV_JOURNAL_OK)
    {
        journal->busy = 0U;
        return result;
    }
    /* Exhaustion must not trigger even a pending cleanup in commit(). init()
     * can still finish a known scrub transaction without allocating a seq. */
    if (scan.latest.valid && scan.latest.sequence == UINT64_MAX)
    {
        journal->busy = 0U;
        return WPM_NV_JOURNAL_ESEQUENCE;
    }
    if (wpm_nv_journal_is_latest_scrub(&scan))
    {
        uint64_t existing_sequence = scan.latest.sequence;
        if (scan.sectors[scan.latest.sector].unknown)
        {
            journal->busy = 0U;
            return WPM_NV_JOURNAL_EFOREIGN;
        }
        result = wpm_nv_journal_payload_matches_record(journal, &scan.latest,
                                                payload, len,
                                                &retry_same);
        if (result != WPM_NV_JOURNAL_OK)
        {
            journal->busy = 0U;
            return result;
        }
        result = wpm_nv_journal_resolve_scrub(journal, &scan);
        if (result != WPM_NV_JOURNAL_OK)
        {
            if (result == WPM_NV_JOURNAL_ESCRUB && retry_same)
            {
                *sequence = existing_sequence;
            }
            journal->busy = 0U;
            return result == WPM_NV_JOURNAL_ESCRUB && !retry_same ?
                   WPM_NV_JOURNAL_EIO : result;
        }
        if (scrub && retry_same)
        {
            *sequence = existing_sequence;
            journal->busy = 0U;
            return WPM_NV_JOURNAL_OK;
        }
    }
    else if (wpm_nv_journal_has_unknown(&scan))
    {
        journal->busy = 0U;
        return WPM_NV_JOURNAL_EFOREIGN;
    }

    if (!scan.latest.valid)
    {
        if (!wpm_nv_journal_is_blank(&scan))
        {
            journal->busy = 0U;
            return WPM_NV_JOURNAL_ECORRUPT;
        }
        next_sequence = 1U;
        active_sector = 0U;
        offset = 0U;
    }
    else
    {
        if (scan.latest.sequence == UINT64_MAX)
        {
            journal->busy = 0U;
            return WPM_NV_JOURNAL_ESEQUENCE;
        }
        next_sequence = scan.latest.sequence + 1U;
        active_sector = scan.latest.sector;
        offset = scan.sectors[active_sector].append_offset;
    }

    if (scrub)
    {
        target_sector = scan.latest.valid ?
                        WPM_NV_JOURNAL_SECTOR_COUNT - 1U - active_sector : 0U;
        if (scan.sectors[target_sector].unknown)
        {
            journal->busy = 0U;
            return WPM_NV_JOURNAL_EFOREIGN;
        }
        if (!scan.sectors[target_sector].blank)
        {
            result = wpm_nv_journal_erase_sector(journal, target_sector);
            if (result != WPM_NV_JOURNAL_OK)
            {
                journal->busy = 0U;
                return result;
            }
        }
        active_sector = target_sector;
        offset = 0U;
    }
    else if (wpm_nv_journal_record_size((uint32_t)len) >
             journal->geometry.sector_size - offset)
    {
        target_sector = WPM_NV_JOURNAL_SECTOR_COUNT - 1U - active_sector;
        if (scan.sectors[target_sector].unknown)
        {
            journal->busy = 0U;
            return WPM_NV_JOURNAL_EFOREIGN;
        }
        if (!scan.sectors[target_sector].blank)
        {
            result = wpm_nv_journal_erase_sector(journal, target_sector);
            if (result != WPM_NV_JOURNAL_OK)
            {
                journal->busy = 0U;
                return result;
            }
        }
        active_sector = target_sector;
        offset = 0U;
    }

    (void)wpm_nv_journal_write_record(journal, active_sector, offset,
                               next_sequence,
                               payload, (uint32_t)len, scrub);
    payload_crc = wpm_nv_journal_crc(payload, len);
    result = wpm_nv_journal_recheck_commit(journal, next_sequence,
                                    (uint32_t)len,
                                    payload_crc, scrub, &scan);
    if (result != WPM_NV_JOURNAL_OK)
    {
        journal->busy = 0U;
        return result == WPM_NV_JOURNAL_EFOREIGN || result == WPM_NV_JOURNAL_ECORRUPT ?
               result : WPM_NV_JOURNAL_EIO;
    }

    *sequence = next_sequence;
    if (scrub)
    {
        target_sector = WPM_NV_JOURNAL_SECTOR_COUNT - 1U - active_sector;
        if (!scan.sectors[target_sector].blank)
        {
            result = wpm_nv_journal_erase_sector(journal, target_sector);
            if (result != WPM_NV_JOURNAL_OK ||
                wpm_nv_journal_scan_all(journal, &scan) != WPM_NV_JOURNAL_OK ||
                !wpm_nv_journal_is_latest_scrub(&scan) ||
                !scan.sectors[target_sector].blank)
            {
                journal->busy = 0U;
                return WPM_NV_JOURNAL_ESCRUB;
            }
        }
    }
    journal->busy = 0U;
    return WPM_NV_JOURNAL_OK;
}
