#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "wpm_nv_journal.h"

#define TEST_BASE_ADDRESS 0U
#define TEST_SECTOR_SIZE 8192U
#define TEST_FLASH_SIZE (TEST_SECTOR_SIZE * WPM_NV_JOURNAL_SECTOR_COUNT)

typedef struct mock_flash
{
    uint8_t bytes[TEST_FLASH_SIZE];
    uint8_t programmed[TEST_FLASH_SIZE / WPM_NV_JOURNAL_PROGRAM_SIZE];
    uint32_t sector_size;
    uint32_t rewrite_attempts;
    uint32_t program_calls;
    uint32_t erase_calls;
    uint32_t fail_program_at;
    uint32_t fail_erase_at;
    uint32_t fail_read_enabled;
    uint32_t fail_read_address;
    uint32_t fail_program_prefix;
    uint32_t fail_erase_prefix;
    uint32_t noop_erase_at;
    uint32_t power_off_on_failure;
    uint32_t power_off;
    uint32_t transient_read_address;
    uint32_t transient_read_enabled;
    uint32_t transient_read_count;
    uint32_t transient_read_at;
    uint32_t transient_read_error;
    uint32_t reenter_read;
    int reenter_result;
    uint32_t inject_foreign_on_commit;
} mock_flash_t;

static const char *g_test_name;
static wpm_nv_journal_t g_test_journal = WPM_NV_JOURNAL_INITIALIZER;

static int test_init(const wpm_nv_journal_io_t *io)
{
    const mock_flash_t *mock = (const mock_flash_t *)io->context;
    const wpm_nv_journal_geometry_t geometry = {
        mock->sector_size, WPM_NV_JOURNAL_MAX_PAYLOAD,
        WPM_NV_JOURNAL_SECTOR_COUNT, WPM_NV_JOURNAL_PROGRAM_SIZE, 0xFFU
    };
    return wpm_nv_journal_init(&g_test_journal, io, &geometry);
}

static int test_load(uint8_t *out, size_t cap, size_t *length,
                     uint64_t *sequence)
{
    return wpm_nv_journal_load(&g_test_journal, out, cap, length, sequence);
}

static int test_commit(const uint8_t *payload, size_t length, bool scrub_old,
                       uint64_t *sequence)
{
    return wpm_nv_journal_commit(&g_test_journal, payload, length, scrub_old,
                                 sequence);
}

static int test_reset(void)
{
    return wpm_nv_journal_reset(&g_test_journal);
}

#define CHECK(condition) \
    do \
    { \
        if (!(condition)) \
        { \
            fprintf(stderr, "%s:%d: check failed: %s\n", \
                    g_test_name, __LINE__, #condition); \
            return 0; \
        } \
    } while (0)

static void mock_init(mock_flash_t *mock)
{
    memset(mock, 0, sizeof(*mock));
    memset(mock->bytes, 0xFF, sizeof(mock->bytes));
    mock->sector_size = TEST_SECTOR_SIZE;
}

static void mock_init_sector_size(mock_flash_t *mock, uint32_t sector_size)
{
    mock_init(mock);
    mock->sector_size = sector_size;
}

static uint8_t *mock_at(mock_flash_t *mock, uint32_t address)
{
    return mock->bytes + (address - TEST_BASE_ADDRESS);
}

static int mock_contains(const mock_flash_t *mock, const uint8_t *value,
                         size_t length)
{
    size_t offset;

    for (offset = 0U; offset + length <= sizeof(mock->bytes); offset++)
    {
        if (memcmp(mock->bytes + offset, value, length) == 0)
        {
            return 1;
        }
    }
    return 0;
}

static int mock_read(void *context, uint32_t address, uint8_t *data,
                     size_t length)
{
    mock_flash_t *mock = (mock_flash_t *)context;
    uint32_t end = TEST_BASE_ADDRESS +
                   (mock->sector_size * WPM_NV_JOURNAL_SECTOR_COUNT);

    if (mock->power_off)
    {
        return -1;
    }
    if (mock->reenter_read)
    {
        size_t ignored_length;
        uint64_t ignored_sequence;
        mock->reenter_read = 0U;
        mock->reenter_result = test_load(NULL, 0U, &ignored_length,
                                            &ignored_sequence);
    }

    if (data == NULL || length == 0U || (address & 0x0FU) != 0U ||
        (length & 0x0FU) != 0U ||
        address > end || length > (size_t)(end - address))
    {
        return -1;
    }
    if (mock->fail_read_enabled &&
        address <= mock->fail_read_address &&
        mock->fail_read_address < address + length)
    {
        return -1;
    }
    memcpy(data, mock_at(mock, address), length);
    if (mock->transient_read_enabled &&
        address == mock->transient_read_address &&
        ++mock->transient_read_count == mock->transient_read_at)
    {
        if (mock->transient_read_error)
        {
            return -1;
        }
        data[0] ^= 1U;
    }
    return 0;
}

static int mock_program16(void *context, uint32_t address,
                          const uint8_t data[WPM_NV_JOURNAL_PROGRAM_SIZE])
{
    mock_flash_t *mock = (mock_flash_t *)context;
    uint8_t *destination;
    size_t i;
    size_t write_bytes = WPM_NV_JOURNAL_PROGRAM_SIZE;
    uint32_t index;
    int failed = 0;

    if (mock->power_off)
    {
        return -1;
    }
    mock->program_calls++;
    if (data == NULL || (address & 0x0FU) != 0U ||
        address > TEST_BASE_ADDRESS +
                  (mock->sector_size * WPM_NV_JOURNAL_SECTOR_COUNT) -
                  WPM_NV_JOURNAL_PROGRAM_SIZE)
    {
        return -1;
    }
    if (mock->fail_program_at != 0U &&
        mock->program_calls == mock->fail_program_at)
    {
        mock->fail_program_at = 0U;
        write_bytes = mock->fail_program_prefix;
        failed = 1;
    }

    destination = mock_at(mock, address);
    index = (address - TEST_BASE_ADDRESS) / WPM_NV_JOURNAL_PROGRAM_SIZE;
    if (write_bytes != 0U && mock->programmed[index])
    {
        mock->rewrite_attempts++;
        return -1;
    }
    for (i = 0U; i < WPM_NV_JOURNAL_PROGRAM_SIZE; i++)
    {
        if ((uint8_t)(destination[i] & data[i]) != data[i])
        {
            return -1;
        }
    }
    for (i = 0U; i < write_bytes; i++)
    {
        destination[i] &= data[i];
    }
    if (write_bytes != 0U)
    {
        mock->programmed[index] = 1U;
    }
    if (write_bytes == 16U && mock->inject_foreign_on_commit &&
        data[0] == 'C' && data[1] == 'M' && data[2] == 'I' && data[3] == 'T')
    {
        mock->bytes[512U] = 0x12U;
        mock->inject_foreign_on_commit = 0U;
    }
    if (failed && mock->power_off_on_failure)
    {
        mock->power_off = 1U;
    }
    return failed ? -1 : 0;
}

static int mock_erase8k(void *context, uint32_t sector_address)
{
    mock_flash_t *mock = (mock_flash_t *)context;
    uint32_t end = TEST_BASE_ADDRESS +
                   (mock->sector_size * WPM_NV_JOURNAL_SECTOR_COUNT);

    if (mock->power_off)
    {
        return -1;
    }

    mock->erase_calls++;
    if (sector_address >= end ||
        ((sector_address - TEST_BASE_ADDRESS) % mock->sector_size) != 0U)
    {
        return -1;
    }
    if (mock->fail_erase_at != 0U &&
        mock->erase_calls == mock->fail_erase_at)
    {
        mock->fail_erase_at = 0U;
        memset(mock_at(mock, sector_address), 0xFF, mock->fail_erase_prefix);
        memset(mock->programmed +
               (sector_address - TEST_BASE_ADDRESS) / WPM_NV_JOURNAL_PROGRAM_SIZE,
               0, mock->fail_erase_prefix / WPM_NV_JOURNAL_PROGRAM_SIZE);
        if (mock->power_off_on_failure)
        {
            mock->power_off = 1U;
        }
        return -1;
    }
    if (mock->noop_erase_at == mock->erase_calls)
    {
        return 0;
    }
    memset(mock_at(mock, sector_address), 0xFF, mock->sector_size);
    memset(mock->programmed +
           (sector_address - TEST_BASE_ADDRESS) / WPM_NV_JOURNAL_PROGRAM_SIZE,
           0, mock->sector_size / WPM_NV_JOURNAL_PROGRAM_SIZE);
    return 0;
}

static int mock_erase_sector(void *context, uint32_t sector_index)
{
    if (sector_index >= WPM_NV_JOURNAL_SECTOR_COUNT)
    {
        return -1;
    }
    return mock_erase8k(context,
                        TEST_BASE_ADDRESS + sector_index *
                        ((mock_flash_t *)context)->sector_size);
}
static wpm_nv_journal_io_t mock_backend(mock_flash_t *mock)
{
    wpm_nv_journal_io_t backend;
    backend.context = mock;
    backend.read = mock_read;
    backend.program16 = mock_program16;
    backend.erase_sector = mock_erase_sector;
    return backend;
}

static int load_matches(const uint8_t *expected, size_t expected_length,
                        uint64_t expected_sequence, int expected_status)
{
    uint8_t output[WPM_NV_JOURNAL_MAX_PAYLOAD];
    size_t length = 999U;
    uint64_t sequence = 999U;
    int result = test_load(output, sizeof(output), &length, &sequence);

    CHECK(result == expected_status);
    CHECK(length == expected_length);
    CHECK(sequence == expected_sequence);
    if (expected_length != 0U)
    {
        CHECK(memcmp(output, expected, expected_length) == 0);
    }
    return 1;
}

static int test_empty_and_basic_round_trip(void)
{
    mock_flash_t mock;
    wpm_nv_journal_io_t backend;
    uint8_t input[37];
    uint64_t sequence = 0U;
    size_t i;

    mock_init(&mock);
    backend = mock_backend(&mock);
    CHECK(test_init(&backend) == WPM_NV_JOURNAL_EMPTY);
    CHECK(load_matches(NULL, 0U, 0U, WPM_NV_JOURNAL_EMPTY));
    for (i = 0U; i < sizeof(input); i++)
    {
        input[i] = (uint8_t)(i * 7U + 3U);
    }
    CHECK(test_commit(input, sizeof(input), false, &sequence) == WPM_NV_JOURNAL_OK);
    CHECK(sequence == 1U);
    CHECK(load_matches(input, sizeof(input), 1U, WPM_NV_JOURNAL_OK));
    CHECK(test_commit(input, WPM_NV_JOURNAL_MAX_PAYLOAD + 1U,
                         false, &sequence) == WPM_NV_JOURNAL_ECAPACITY);
    CHECK(test_commit(NULL, 1U, false, &sequence) == WPM_NV_JOURNAL_EINVAL);
    return 1;
}

static int test_explicit_reset_erases_and_verifies_both_sectors(void)
{
    mock_flash_t mock;
    wpm_nv_journal_io_t backend;
    size_t length;
    uint64_t sequence;

    mock_init(&mock);
    backend = mock_backend(&mock);
    CHECK(test_init(&backend) == WPM_NV_JOURNAL_EMPTY);
    memset(mock.bytes, 0x35, sizeof(mock.bytes));
    CHECK(test_reset() == WPM_NV_JOURNAL_OK);
    CHECK(mock.erase_calls == WPM_NV_JOURNAL_SECTOR_COUNT);
    CHECK(test_load(NULL, 0U, &length, &sequence) == WPM_NV_JOURNAL_EMPTY);
    CHECK(length == 0U && sequence == 0U);
    CHECK(mock_contains(&mock, (const uint8_t *)"\x35\x35\x35\x35", 4U) == 0);
    return 1;
}

static int test_explicit_reset_fails_closed_on_erase_or_readback_error(void)
{
    mock_flash_t mock;
    wpm_nv_journal_io_t backend;

    mock_init(&mock);
    backend = mock_backend(&mock);
    CHECK(test_init(&backend) == WPM_NV_JOURNAL_EMPTY);
    memset(mock.bytes, 0x35, sizeof(mock.bytes));
    mock.fail_erase_at = 2U;
    CHECK(test_reset() == WPM_NV_JOURNAL_EIO);
    CHECK(mock.erase_calls == 2U);
    CHECK(mock.bytes[0] == 0xFFU);
    CHECK(mock.bytes[TEST_SECTOR_SIZE] == 0x35U);

    mock_init(&mock);
    backend = mock_backend(&mock);
    CHECK(test_init(&backend) == WPM_NV_JOURNAL_EMPTY);
    memset(mock.bytes, 0x35, sizeof(mock.bytes));
    mock.noop_erase_at = 1U;
    CHECK(test_reset() == WPM_NV_JOURNAL_EIO);
    CHECK(mock.erase_calls == 1U);
    CHECK(mock.bytes[0] == 0x35U);
    CHECK(mock.bytes[TEST_SECTOR_SIZE] == 0x35U);

    mock_init(&mock);
    backend = mock_backend(&mock);
    CHECK(test_init(&backend) == WPM_NV_JOURNAL_EMPTY);
    memset(mock.bytes, 0x35, sizeof(mock.bytes));
    mock.fail_read_enabled = 1U;
    mock.fail_read_address = 0U;
    CHECK(test_reset() == WPM_NV_JOURNAL_EIO);
    CHECK(mock.erase_calls == 1U);
    return 1;
}

static int test_journal_v1_golden_bytes(void)
{
    static const uint8_t expected[] = {
        0x57,0x4E,0x56,0x31,0x01,0x00,0x00,0x00,
        0x07,0x00,0x00,0x00,0xF8,0xFF,0xFF,0xFF,
        0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0xAD,0xC4,0xEF,0xD5,0x79,0xB9,0xC9,0xB1,
        0x41,0x52,0x54,0x2D,0x50,0x69,0x32,0xFF,
        0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
        0x43,0x4D,0x49,0x54,0x01,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x79,0xB9,0xC9,0xB1
    };
    static const uint8_t payload[] = "ART-Pi2";
    mock_flash_t mock;
    wpm_nv_journal_io_t io;
    uint64_t sequence = 0U;

    mock_init(&mock);
    io = mock_backend(&mock);
    CHECK(test_init(&io) == WPM_NV_JOURNAL_EMPTY);
    CHECK(test_commit(payload, sizeof(payload) - 1U, false, &sequence) ==
          WPM_NV_JOURNAL_OK);
    CHECK(sequence == 1U);
    CHECK(memcmp(mock.bytes, expected, sizeof(expected)) == 0);
    return 1;
}

static int test_independent_instances_and_geometry(void)
{
    static const uint8_t payload_a[] = {0xA1U,0xA2U,0xA3U};
    static const uint8_t payload_b[] = {0xB1U,0xB2U};
    wpm_nv_journal_t journal_a = WPM_NV_JOURNAL_INITIALIZER;
    wpm_nv_journal_t journal_b = WPM_NV_JOURNAL_INITIALIZER;
    const wpm_nv_journal_geometry_t geometry_a = {
        8192U, 2048U, 2U, 16U, 0xFFU
    };
    const wpm_nv_journal_geometry_t geometry_b = {
        4096U, 2048U, 2U, 16U, 0xFFU
    };
    mock_flash_t mock_a;
    mock_flash_t mock_b;
    wpm_nv_journal_io_t io_a;
    wpm_nv_journal_io_t io_b;
    uint8_t output[8];
    size_t length;
    uint64_t sequence;

    mock_init_sector_size(&mock_a, geometry_a.sector_size);
    mock_init_sector_size(&mock_b, geometry_b.sector_size);
    io_a = mock_backend(&mock_a);
    io_b = mock_backend(&mock_b);
    CHECK(wpm_nv_journal_init(&journal_a, &io_a, &geometry_a) ==
          WPM_NV_JOURNAL_EMPTY);
    CHECK(wpm_nv_journal_init(&journal_b, &io_b, &geometry_b) ==
          WPM_NV_JOURNAL_EMPTY);
    CHECK(wpm_nv_journal_commit(&journal_a, payload_a, sizeof(payload_a),
                                false, &sequence) == WPM_NV_JOURNAL_OK);
    CHECK(sequence == 1U);
    CHECK(wpm_nv_journal_commit(&journal_b, payload_b, sizeof(payload_b),
                                false, &sequence) == WPM_NV_JOURNAL_OK);
    CHECK(sequence == 1U);

    CHECK(wpm_nv_journal_load(&journal_a, output, sizeof(output), &length,
                              &sequence) == WPM_NV_JOURNAL_OK);
    CHECK(length == sizeof(payload_a) && sequence == 1U);
    CHECK(memcmp(output, payload_a, sizeof(payload_a)) == 0);
    CHECK(wpm_nv_journal_load(&journal_b, output, sizeof(output), &length,
                              &sequence) == WPM_NV_JOURNAL_OK);
    CHECK(length == sizeof(payload_b) && sequence == 1U);
    CHECK(memcmp(output, payload_b, sizeof(payload_b)) == 0);

    CHECK(wpm_nv_journal_init(&journal_a, &io_a, &geometry_a) ==
          WPM_NV_JOURNAL_OK);
    CHECK(wpm_nv_journal_load(&journal_a, output, sizeof(output), &length,
                              &sequence) == WPM_NV_JOURNAL_OK);
    CHECK(length == sizeof(payload_a) &&
          memcmp(output, payload_a, sizeof(payload_a)) == 0);
    return 1;
}

static int test_invalid_init_clears_previous_backend(void)
{
    static const uint8_t payload[] = {0x51U,0x52U,0x53U};
    wpm_nv_journal_t journal = WPM_NV_JOURNAL_INITIALIZER;
    const wpm_nv_journal_geometry_t valid_geometry = {
        8192U, 2048U, 2U, 16U, 0xFFU
    };
    wpm_nv_journal_geometry_t invalid_geometry = valid_geometry;
    mock_flash_t mock;
    wpm_nv_journal_io_t io;
    uint32_t programs;
    uint32_t erases;
    size_t length = 99U;
    uint64_t sequence = 99U;

    mock_init(&mock);
    io = mock_backend(&mock);
    CHECK(wpm_nv_journal_init(&journal, &io, &valid_geometry) ==
          WPM_NV_JOURNAL_EMPTY);
    CHECK(wpm_nv_journal_commit(&journal, payload, sizeof(payload), false,
                                &sequence) == WPM_NV_JOURNAL_OK);
    programs = mock.program_calls;
    erases = mock.erase_calls;

    invalid_geometry.sector_count = 1U;
    CHECK(wpm_nv_journal_init(&journal, &io, &invalid_geometry) ==
          WPM_NV_JOURNAL_EINVAL);
    CHECK(wpm_nv_journal_load(&journal, NULL, 0U, &length, &sequence) ==
          WPM_NV_JOURNAL_EINVAL);
    CHECK(length == 0U && sequence == 0U);
    CHECK(wpm_nv_journal_commit(&journal, payload, sizeof(payload), false,
                                &sequence) == WPM_NV_JOURNAL_EINVAL);
    CHECK(mock.program_calls == programs && mock.erase_calls == erases);

    CHECK(wpm_nv_journal_init(&journal, &io, &valid_geometry) ==
          WPM_NV_JOURNAL_OK);
    CHECK(wpm_nv_journal_load(&journal, NULL, 0U, &length, &sequence) ==
          WPM_NV_JOURNAL_ECAPACITY);
    CHECK(length == sizeof(payload) && sequence == 1U);
    return 1;
}

static int test_crc_version_and_length_fail_closed(void)
{
    mock_flash_t mock;
    wpm_nv_journal_io_t backend;
    uint8_t payload[24];
    uint64_t sequence;
    int status;
    unsigned int variant;

    memset(payload, 0x5A, sizeof(payload));
    for (variant = 0U; variant < 3U; variant++)
    {
        mock_init(&mock);
        backend = mock_backend(&mock);
        CHECK(test_init(&backend) == WPM_NV_JOURNAL_EMPTY);
        CHECK(test_commit(payload, sizeof(payload), false,
                             &sequence) == WPM_NV_JOURNAL_OK);
        if (variant == 0U)
        {
            mock_at(&mock, TEST_BASE_ADDRESS)[4] = 2U;
        }
        else if (variant == 1U)
        {
            mock_at(&mock, TEST_BASE_ADDRESS + 8U)[0] ^= 0x01U;
        }
        else
        {
            mock_at(&mock, TEST_BASE_ADDRESS + 32U)[0] ^= 0x01U;
        }
        status = test_init(&backend);
        if (variant == 0U)
        {
            CHECK(status == WPM_NV_JOURNAL_EFOREIGN);
        }
        else
        {
            CHECK(status == WPM_NV_JOURNAL_ECORRUPT);
        }
    }
    return 1;
}

static int test_unknown_data_is_never_formatted(void)
{
    mock_flash_t mock;
    wpm_nv_journal_io_t backend;
    uint8_t before[TEST_FLASH_SIZE];
    uint64_t sequence;

    mock_init(&mock);
    mock.bytes[13] = 0x7EU;
    memcpy(before, mock.bytes, sizeof(before));
    backend = mock_backend(&mock);
    CHECK(test_init(&backend) == WPM_NV_JOURNAL_EFOREIGN);
    CHECK(test_commit(NULL, 0U, true, &sequence) == WPM_NV_JOURNAL_EFOREIGN);
    CHECK(mock.erase_calls == 0U);
    CHECK(mock.program_calls == 0U);
    CHECK(memcmp(before, mock.bytes, sizeof(before)) == 0);
    return 1;
}

static int test_interruption_at_each_record_word(void)
{
    mock_flash_t baseline;
    mock_flash_t trial;
    wpm_nv_journal_io_t backend;
    uint8_t old_payload[23];
    uint8_t new_payload[41];
    uint64_t sequence;
    uint32_t failure;
    uint32_t word_count;

    memset(old_payload, 0x31, sizeof(old_payload));
    memset(new_payload, 0xA7, sizeof(new_payload));
    mock_init(&baseline);
    backend = mock_backend(&baseline);
    CHECK(test_init(&backend) == WPM_NV_JOURNAL_EMPTY);
    CHECK(test_commit(old_payload, sizeof(old_payload), false,
                         &sequence) == WPM_NV_JOURNAL_OK);
    word_count = 2U + ((uint32_t)sizeof(new_payload) + 15U) / 16U + 1U;

    for (failure = 1U; failure <= word_count; failure++)
    {
        memcpy(&trial, &baseline, sizeof(trial));
        trial.program_calls = 0U;
        trial.erase_calls = 0U;
        trial.fail_program_at = failure;
        backend = mock_backend(&trial);
        CHECK(test_init(&backend) == WPM_NV_JOURNAL_OK);
        CHECK(test_commit(new_payload, sizeof(new_payload), false,
                             &sequence) == WPM_NV_JOURNAL_EIO);
        trial.fail_program_at = 0U;
        CHECK(test_init(&backend) == WPM_NV_JOURNAL_OK);
        CHECK(load_matches(old_payload, sizeof(old_payload), 1U,
                           WPM_NV_JOURNAL_OK));
    }
    return 1;
}

static int test_rotation_preserves_last_valid_record(void)
{
    mock_flash_t mock;
    wpm_nv_journal_io_t backend;
    uint8_t payload[WPM_NV_JOURNAL_MAX_PAYLOAD];
    uint8_t expected[WPM_NV_JOURNAL_MAX_PAYLOAD];
    uint64_t sequence = 0U;
    unsigned int revision;

    mock_init(&mock);
    backend = mock_backend(&mock);
    CHECK(test_init(&backend) == WPM_NV_JOURNAL_EMPTY);
    for (revision = 1U; revision <= 6U; revision++)
    {
        memset(payload, (int)revision, sizeof(payload));
        CHECK(test_commit(payload, sizeof(payload), false,
                             &sequence) == WPM_NV_JOURNAL_OK);
    }
    CHECK(sequence == 6U);
    memcpy(expected, payload, sizeof(expected));

    memset(payload, 7, sizeof(payload));
    mock.program_calls = 0U;
    mock.fail_program_at = 1U;
    CHECK(test_commit(payload, sizeof(payload), false,
                         &sequence) == WPM_NV_JOURNAL_EIO);
    mock.fail_program_at = 0U;
    CHECK(test_init(&backend) == WPM_NV_JOURNAL_OK);
    CHECK(load_matches(expected, sizeof(expected), 6U, WPM_NV_JOURNAL_OK));

    CHECK(test_commit(payload, sizeof(payload), false,
                         &sequence) == WPM_NV_JOURNAL_OK);
    CHECK(sequence == 7U);
    CHECK(load_matches(payload, sizeof(payload), 7U, WPM_NV_JOURNAL_OK));
    return 1;
}

static int test_scrub_interruptions_do_not_commit_clear_early(void)
{
    mock_flash_t baseline;
    mock_flash_t trial;
    wpm_nv_journal_io_t backend;
    uint8_t payload[64];
    uint64_t sequence;
    uint32_t failure;

    memset(payload, 0xC3, sizeof(payload));
    mock_init(&baseline);
    backend = mock_backend(&baseline);
    CHECK(test_init(&backend) == WPM_NV_JOURNAL_EMPTY);
    CHECK(test_commit(payload, sizeof(payload), false,
                         &sequence) == WPM_NV_JOURNAL_OK);

    for (failure = 1U; failure <= 3U; failure++)
    {
        memcpy(&trial, &baseline, sizeof(trial));
        trial.program_calls = 0U;
        trial.erase_calls = 0U;
        trial.fail_program_at = failure;
        backend = mock_backend(&trial);
        CHECK(test_init(&backend) == WPM_NV_JOURNAL_OK);
        CHECK(test_commit(NULL, 0U, true, &sequence) == WPM_NV_JOURNAL_EIO);
        trial.fail_program_at = 0U;
        CHECK(test_init(&backend) == WPM_NV_JOURNAL_OK);
        CHECK(load_matches(payload, sizeof(payload), 1U, WPM_NV_JOURNAL_OK));
    }
    return 1;
}

static int test_scrub_failure_is_explicit_and_cannot_fallback(void)
{
    mock_flash_t mock;
    wpm_nv_journal_io_t backend;
    uint8_t payload[WPM_NV_JOURNAL_MAX_PAYLOAD];
    uint64_t sequence = 0U;
    uint32_t i;
    size_t length = 1U;
    int scrub_result;

    for (i = 0U; i < sizeof(payload); i++)
    {
        payload[i] = (uint8_t)"old-WPA-secret"[i % 14U];
    }
    mock_init(&mock);
    backend = mock_backend(&mock);
    CHECK(test_init(&backend) == WPM_NV_JOURNAL_EMPTY);
    for (i = 0U; i < 6U; i++)
    {
        CHECK(test_commit(payload, sizeof(payload), false,
                             &sequence) == WPM_NV_JOURNAL_OK);
    }
    CHECK(sequence == 6U);
    mock.fail_erase_at = mock.erase_calls + 2U;
    scrub_result = test_commit(NULL, 0U, true, &sequence);
    if (scrub_result != WPM_NV_JOURNAL_ESCRUB)
    {
        fprintf(stderr, "scrub commit returned %d\n", scrub_result);
        CHECK(scrub_result == WPM_NV_JOURNAL_ESCRUB);
    }
    CHECK(sequence == 7U);
    CHECK(load_matches(NULL, 0U, 7U, WPM_NV_JOURNAL_ESCRUB));
    CHECK(mock_contains(&mock, payload, sizeof(payload)));
    mock.fail_erase_at = mock.erase_calls + 1U;
    CHECK(test_init(&backend) == WPM_NV_JOURNAL_ESCRUB);
    CHECK(load_matches(NULL, 0U, 7U, WPM_NV_JOURNAL_ESCRUB));
    CHECK(test_init(&backend) == WPM_NV_JOURNAL_OK);
    CHECK(load_matches(NULL, 0U, 7U, WPM_NV_JOURNAL_EMPTY));
    CHECK(!mock_contains(&mock, payload, sizeof(payload)));

    for (i = 0U; i + sizeof(payload) <= sizeof(mock.bytes); i++)
    {
        uint32_t j;
        int found = 1;
        for (j = 0U; j < sizeof(payload); j++)
        {
            if (mock.bytes[i + j] != payload[j])
            {
                found = 0;
                break;
            }
        }
        CHECK(!found);
    }
    CHECK(test_load(NULL, 0U, &length, &sequence) == WPM_NV_JOURNAL_EMPTY);
    mock_at(&mock, TEST_BASE_ADDRESS + 32U)[0] ^= 0x01U;
    CHECK(test_init(&backend) == WPM_NV_JOURNAL_ECORRUPT);
    CHECK(test_load(NULL, 0U, &length, &sequence) == WPM_NV_JOURNAL_ECORRUPT);
    return 1;
}

static int test_backend_read_error_maps_to_io(void)
{
    mock_flash_t mock;
    wpm_nv_journal_io_t backend;
    uint8_t payload[16];
    uint64_t sequence;

    memset(payload, 0x66, sizeof(payload));
    mock_init(&mock);
    backend = mock_backend(&mock);
    CHECK(test_init(&backend) == WPM_NV_JOURNAL_EMPTY);
    CHECK(test_commit(payload, sizeof(payload), false,
                         &sequence) == WPM_NV_JOURNAL_OK);
    mock.fail_read_address = TEST_BASE_ADDRESS + 32U;
    mock.fail_read_enabled = 1U;
    CHECK(test_init(&backend) == WPM_NV_JOURNAL_EIO);
    return 1;
}

static int test_backend_boundaries(void)
{
    mock_flash_t mock;
    wpm_nv_journal_io_t backend;
    uint8_t word[WPM_NV_JOURNAL_PROGRAM_SIZE];

    mock_init(&mock);
    backend = mock_backend(&mock);
    memset(word, 0x00, sizeof(word));
    CHECK(mock_program16(&mock, TEST_BASE_ADDRESS - 16U, word) != 0);
    CHECK(mock_program16(&mock, TEST_BASE_ADDRESS + 1U, word) != 0);
    CHECK(mock_program16(&mock,
                        TEST_BASE_ADDRESS + TEST_FLASH_SIZE - 16U,
                        word) == 0);
    CHECK(mock_program16(&mock,
                        TEST_BASE_ADDRESS + TEST_FLASH_SIZE,
                        word) != 0);
    CHECK(mock_erase8k(&mock, TEST_BASE_ADDRESS + 1U) != 0);
    CHECK(mock_erase8k(&mock,
                       TEST_BASE_ADDRESS + TEST_SECTOR_SIZE) == 0);
    CHECK(mock_erase8k(&mock,
                       TEST_BASE_ADDRESS + TEST_FLASH_SIZE) != 0);
    CHECK(backend.read(&mock, TEST_BASE_ADDRESS - 16U,
                       word, sizeof(word)) != 0);
    return 1;
}

static uint32_t fixture_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t fixture_crc(const uint8_t *p, size_t length)
{
    uint32_t crc = UINT32_MAX;
    size_t i;
    unsigned int bit;
    for (i = 0U; i < length; i++)
    {
        crc ^= p[i];
        for (bit = 0U; bit < 8U; bit++)
            crc = (crc >> 1) ^ ((crc & 1U) ? 0xEDB88320U : 0U);
    }
    return ~crc;
}

/* Format-v1 fixture mutation, independent of the engine's scan/write paths. */
static void fixture_sequence(mock_flash_t *mock, uint32_t offset, uint64_t seq)
{
    uint8_t *header = mock->bytes + offset;
    uint32_t length = fixture_u32(header + 8U);
    uint8_t *commit = header + 32U + ((length + 15U) & ~15U);
    uint8_t crc_input[24];
    uint32_t crc;
    unsigned int i;
    for (i = 0U; i < 8U; i++)
    {
        header[16U + i] = (uint8_t)(seq >> (i * 8U));
        commit[4U + i] = header[16U + i];
    }
    memcpy(crc_input, header, 24U);
    crc = fixture_crc(crc_input, sizeof(crc_input));
    for (i = 0U; i < 4U; i++)
        header[24U + i] = (uint8_t)(crc >> (i * 8U));
}

static int make_baseline(mock_flash_t *mock, const uint8_t *payload,
                          size_t length, unsigned int count)
{
    wpm_nv_journal_io_t backend;
    uint64_t seq;
    unsigned int i;
    mock_init(mock);
    backend = mock_backend(mock);
    CHECK(test_init(&backend) == WPM_NV_JOURNAL_EMPTY);
    for (i = 0U; i < count; i++)
        CHECK(test_commit(payload, length, false, &seq) == WPM_NV_JOURNAL_OK);
    return 1;
}

static int reject_without_mutation(mock_flash_t *mock, int status)
{
    wpm_nv_journal_io_t backend = mock_backend(mock);
    uint8_t output[WPM_NV_JOURNAL_MAX_PAYLOAD];
    size_t length = 999U;
    uint64_t seq = 999U;
    uint32_t programs = mock->program_calls;
    uint32_t erases = mock->erase_calls;
    CHECK(test_init(&backend) == status);
    CHECK(test_load(output, sizeof(output), &length, &seq) == status);
    CHECK(length == 0U && seq == 0U);
    CHECK(test_commit(NULL, 0U, true, &seq) == status);
    CHECK(mock->program_calls == programs && mock->erase_calls == erases);
    return 1;
}

static int test_scrub_never_authorizes_unknown_data(void)
{
    mock_flash_t mock;
    uint8_t old_value[32];
    uint64_t seq;
    unsigned int pending;
    memset(old_value, 0x71, sizeof(old_value));
    for (pending = 0U; pending < 2U; pending++)
    {
        CHECK(make_baseline(&mock, old_value, sizeof(old_value), 1U));
        if (pending) mock.fail_erase_at = mock.erase_calls + 1U;
        CHECK(test_commit(NULL, 0U, true, &seq) ==
              (pending ? WPM_NV_JOURNAL_ESCRUB : WPM_NV_JOURNAL_OK));
        mock.bytes[512U] = 0x12U;
        CHECK(reject_without_mutation(&mock, WPM_NV_JOURNAL_EFOREIGN));
        CHECK(mock.bytes[512U] == 0x12U);
    }
    return 1;
}

static int test_committed_scrub_damage_never_loads_old_credentials(void)
{
    mock_flash_t mock;
    uint8_t old_value[32];
    uint8_t replacement[17];
    uint8_t *record;
    uint64_t seq;
    unsigned int variant;
    memset(old_value, 0x71, sizeof(old_value));
    memset(replacement, 0x26, sizeof(replacement));
    for (variant = 0U; variant < 8U; variant++)
    {
        CHECK(make_baseline(&mock, old_value, sizeof(old_value), 1U));
        mock.fail_erase_at = mock.erase_calls + 1U;
        CHECK(test_commit(replacement, sizeof(replacement), true, &seq)
              == WPM_NV_JOURNAL_ESCRUB);
        CHECK(mock_contains(&mock, old_value, sizeof(old_value)));
        record = mock.bytes + TEST_SECTOR_SIZE;
        switch (variant)
        {
        case 0U: record[32U] ^= 1U; break;  /* payload CRC */
        case 1U: record[49U] = 0U; break; /* padding */
        case 2U: record[24U] ^= 1U; break; /* header CRC */
        case 3U: record[64U] ^= 1U; break; /* commit magic */
        case 4U: record[68U] ^= 1U; break; /* commit sequence */
        case 5U: record[8U] ^= 1U; break;  /* untrusted length */
        case 6U: record[4U] = 2U; break;   /* future version */
        default: record[6U] |= 2U; break; /* unknown flags */
        }
        CHECK(reject_without_mutation(&mock,
              variant >= 6U ? WPM_NV_JOURNAL_EFOREIGN : WPM_NV_JOURNAL_ECORRUPT));
    }
    return 1;
}

static int test_sequence_wrap_and_conflicts(void)
{
    mock_flash_t mock;
    wpm_nv_journal_io_t backend;
    uint8_t payload[WPM_NV_JOURNAL_MAX_PAYLOAD];
    uint64_t seq;
    uint32_t programs;
    uint32_t erases;
    unsigned int scrub;
    memset(payload, 0x31, sizeof(payload));
    CHECK(fixture_crc((const uint8_t *)"123456789", 9U) == 0xCBF43926U);
    CHECK(make_baseline(&mock, payload, 16U, 1U));
    fixture_sequence(&mock, 0U, UINT64_MAX - 1U);
    backend = mock_backend(&mock);
    CHECK(test_init(&backend) == WPM_NV_JOURNAL_OK);
    CHECK(test_commit(payload, 16U, false, &seq) == WPM_NV_JOURNAL_OK);
    CHECK(seq == UINT64_MAX);
    programs = mock.program_calls;
    erases = mock.erase_calls;
    for (scrub = 0U; scrub < 2U; scrub++)
    {
        CHECK(test_commit(payload, 16U, scrub != 0U, &seq)
              == WPM_NV_JOURNAL_ESEQUENCE);
        CHECK(seq == 0U);
    }
    CHECK(mock.program_calls == programs && mock.erase_calls == erases);
    CHECK(make_baseline(&mock, payload, 16U, 1U));
    mock.fail_erase_at = mock.erase_calls + 1U;
    CHECK(test_commit(payload, 16U, true, &seq) == WPM_NV_JOURNAL_ESCRUB);
    fixture_sequence(&mock, TEST_SECTOR_SIZE, UINT64_MAX);
    programs = mock.program_calls;
    erases = mock.erase_calls;
    CHECK(test_commit(payload, 16U, true, &seq) == WPM_NV_JOURNAL_ESEQUENCE);
    CHECK(mock.program_calls == programs && mock.erase_calls == erases);

    CHECK(make_baseline(&mock, payload, 16U, 3U));
    fixture_sequence(&mock, 128U, 1U); /* 1, 2, 1 is not monotonic */
    CHECK(reject_without_mutation(&mock, WPM_NV_JOURNAL_ECORRUPT));
    CHECK(make_baseline(&mock, payload, sizeof(payload), 6U));
    fixture_sequence(&mock, TEST_SECTOR_SIZE, 1U);
    CHECK(reject_without_mutation(&mock, WPM_NV_JOURNAL_ECORRUPT));
    return 1;
}

static int test_erase_success_requires_readback_before_programming(void)
{
    mock_flash_t mock;
    uint8_t payload[WPM_NV_JOURNAL_MAX_PAYLOAD];
    uint64_t seq;
    uint32_t programs;
    unsigned int scrub;
    memset(payload, 0x42, sizeof(payload));
    for (scrub = 0U; scrub < 2U; scrub++)
    {
        CHECK(make_baseline(&mock, payload, sizeof(payload), 6U));
        programs = mock.program_calls;
        mock.noop_erase_at = mock.erase_calls + 1U;
        CHECK(test_commit(payload, sizeof(payload), scrub != 0U, &seq)
              == WPM_NV_JOURNAL_EIO);
        CHECK(mock.program_calls == programs && mock.rewrite_attempts == 0U);
        CHECK(load_matches(payload, sizeof(payload), 6U, WPM_NV_JOURNAL_OK));
    }
    return 1;
}

static int test_load_rechecks_the_delivered_bytes(void)
{
    mock_flash_t mock;
    uint8_t payload[17];
    uint8_t out[32];
    size_t length;
    uint64_t seq;
    unsigned int io;
    memset(payload, 0x25, sizeof(payload));
    for (io = 0U; io < 2U; io++)
    {
        CHECK(make_baseline(&mock, payload, sizeof(payload), 1U));
        mock.transient_read_address = TEST_BASE_ADDRESS + 32U;
        mock.transient_read_enabled = 1U;
        mock.transient_read_at = 2U; /* scan succeeds; copy read changes */
        mock.transient_read_error = io;
        length = 999U; seq = 999U;
        CHECK(test_load(out, sizeof(out), &length, &seq) ==
              (io ? WPM_NV_JOURNAL_EIO : WPM_NV_JOURNAL_ECORRUPT));
        CHECK(length == 0U && seq == 0U);
    }
    return 1;
}

static int test_power_loss_after_every_program_word(void)
{
    static const size_t sizes[] = {0U, 1U, 15U, 16U, 17U, 888U, 2048U};
    mock_flash_t baseline;
    mock_flash_t trial;
    wpm_nv_journal_io_t backend;
    uint8_t old_value[32];
    uint8_t replacement[WPM_NV_JOURNAL_MAX_PAYLOAD];
    uint64_t seq;
    size_t s;
    uint32_t cut;
    unsigned int scrub;
    memset(old_value, 0x71, sizeof(old_value));
    memset(replacement, 0x25, sizeof(replacement));
    CHECK(make_baseline(&baseline, old_value, sizeof(old_value), 1U));
    for (scrub = 0U; scrub < 2U; scrub++)
    for (s = 0U; s < sizeof(sizes) / sizeof(sizes[0]); s++)
    {
        uint32_t words = 3U + (uint32_t)((sizes[s] + 15U) / 16U);
        for (cut = 1U; cut <= words; cut++)
        {
            trial = baseline;
            backend = mock_backend(&trial);
            CHECK(test_init(&backend) == WPM_NV_JOURNAL_OK);
            trial.fail_program_at = trial.program_calls + cut;
            trial.fail_program_prefix = 16U;
            trial.power_off_on_failure = 1U;
            CHECK(test_commit(replacement, sizes[s], scrub != 0U, &seq)
                  == WPM_NV_JOURNAL_EIO);
            trial.power_off = 0U;
            CHECK(test_init(&backend) == WPM_NV_JOURNAL_OK);
            if (cut == words)
            {
                CHECK(load_matches(replacement, sizes[s], 2U,
                                   sizes[s] ? WPM_NV_JOURNAL_OK : WPM_NV_JOURNAL_EMPTY));
                if (scrub) CHECK(!mock_contains(&trial, old_value, sizeof(old_value)));
            }
            else CHECK(load_matches(old_value, sizeof(old_value), 1U, WPM_NV_JOURNAL_OK));
            CHECK(test_commit(replacement, 17U, false, &seq) == WPM_NV_JOURNAL_OK);
            CHECK(trial.rewrite_attempts == 0U);
        }
    }
    return 1;
}

static int test_torn_commit_word_fails_closed(void)
{
    mock_flash_t baseline;
    mock_flash_t trial;
    wpm_nv_journal_io_t backend;
    uint8_t old_value[32];
    uint8_t replacement[17];
    uint64_t seq;
    uint32_t bytes;
    memset(old_value, 0x71, sizeof(old_value));
    memset(replacement, 0x25, sizeof(replacement));
    CHECK(make_baseline(&baseline, old_value, sizeof(old_value), 1U));
    for (bytes = 1U; bytes < 16U; bytes++)
    {
        trial = baseline;
        backend = mock_backend(&trial);
        CHECK(test_init(&backend) == WPM_NV_JOURNAL_OK);
        trial.fail_program_at = trial.program_calls + 5U;
        trial.fail_program_prefix = bytes;
        CHECK(test_commit(replacement, sizeof(replacement), true, &seq)
              == WPM_NV_JOURNAL_ECORRUPT);
        CHECK(reject_without_mutation(&trial, WPM_NV_JOURNAL_ECORRUPT));
        CHECK(trial.rewrite_attempts == 0U);
    }
    return 1;
}

static int test_partial_old_erase_never_formats_unknown_or_loads_old(void)
{
    mock_flash_t baseline;
    mock_flash_t trial;
    wpm_nv_journal_io_t backend;
    uint8_t old_value[64];
    uint8_t replacement[17];
    uint64_t seq;
    uint32_t words;
    memset(old_value, 0x71, sizeof(old_value));
    memset(replacement, 0x25, sizeof(replacement));
    CHECK(make_baseline(&baseline, old_value, sizeof(old_value), 1U));
    for (words = 0U; words <= TEST_SECTOR_SIZE / 16U; words++)
    {
        trial = baseline;
        backend = mock_backend(&trial);
        CHECK(test_init(&backend) == WPM_NV_JOURNAL_OK);
        trial.fail_erase_at = trial.erase_calls + 1U;
        trial.fail_erase_prefix = words * 16U;
        CHECK(test_commit(replacement, sizeof(replacement), true, &seq)
              == WPM_NV_JOURNAL_ESCRUB);
        CHECK(seq == 2U);
        if (words > 0U && words < 7U)
            CHECK(reject_without_mutation(&trial, WPM_NV_JOURNAL_EFOREIGN));
        else
        {
            CHECK(test_init(&backend) == WPM_NV_JOURNAL_OK);
            CHECK(load_matches(replacement, sizeof(replacement), 2U, WPM_NV_JOURNAL_OK));
            CHECK(!mock_contains(&trial, old_value, sizeof(old_value)));
        }
        CHECK(trial.rewrite_attempts == 0U);
    }
    return 1;
}

static int test_capacity_outputs_and_reentry(void)
{
    mock_flash_t mock;
    uint8_t payload[17];
    uint8_t out[16];
    size_t length;
    uint64_t seq;
    memset(payload, 0x25, sizeof(payload));
    CHECK(make_baseline(&mock, payload, sizeof(payload), 1U));
    CHECK(test_load(out, sizeof(out), &length, &seq) == WPM_NV_JOURNAL_ECAPACITY);
    CHECK(length == sizeof(payload) && seq == 1U);
    CHECK(test_load(NULL, 0U, &length, &seq) == WPM_NV_JOURNAL_ECAPACITY);
    CHECK(length == sizeof(payload) && seq == 1U);
    CHECK(test_load(NULL, sizeof(payload), &length, &seq) == WPM_NV_JOURNAL_EINVAL);
    CHECK(length == 0U && seq == 0U);
    mock.reenter_read = 1U;
    CHECK(test_load(out, sizeof(out), &length, &seq) == WPM_NV_JOURNAL_ECAPACITY);
    CHECK(mock.reenter_result == WPM_NV_JOURNAL_EBUSY);
    return 1;
}

static int test_nonempty_scrub_retry_is_idempotent_and_explicit(void)
{
    mock_flash_t mock;
    wpm_nv_journal_io_t backend;
    uint8_t old_value[32];
    uint8_t replacement[17];
    uint8_t different[17];
    uint64_t seq;
    uint32_t programs;
    memset(old_value, 0x71, sizeof(old_value));
    memset(replacement, 0x25, sizeof(replacement));
    memset(different, 0x26, sizeof(different));
    CHECK(make_baseline(&mock, old_value, sizeof(old_value), 1U));
    backend = mock_backend(&mock);
    mock.fail_erase_at = mock.erase_calls + 1U;
    CHECK(test_commit(replacement, sizeof(replacement), true, &seq)
          == WPM_NV_JOURNAL_ESCRUB);
    CHECK(seq == 2U);
    mock.fail_erase_at = mock.erase_calls + 1U;
    CHECK(test_init(&backend) == WPM_NV_JOURNAL_ESCRUB);
    CHECK(load_matches(replacement, sizeof(replacement), 2U, WPM_NV_JOURNAL_ESCRUB));
    programs = mock.program_calls;
    mock.fail_erase_at = mock.erase_calls + 1U;
    CHECK(test_commit(replacement, sizeof(replacement), true, &seq)
          == WPM_NV_JOURNAL_ESCRUB);
    CHECK(seq == 2U && mock.program_calls == programs);
    CHECK(load_matches(replacement, sizeof(replacement), 2U, WPM_NV_JOURNAL_ESCRUB));
    mock.fail_erase_at = mock.erase_calls + 1U;
    CHECK(test_commit(different, sizeof(different), true, &seq) == WPM_NV_JOURNAL_EIO);
    CHECK(seq == 0U && mock.program_calls == programs);
    CHECK(load_matches(replacement, sizeof(replacement), 2U, WPM_NV_JOURNAL_ESCRUB));
    CHECK(test_commit(replacement, sizeof(replacement), true, &seq) == WPM_NV_JOURNAL_OK);
    CHECK(seq == 2U && mock.program_calls == programs);
    CHECK(load_matches(replacement, sizeof(replacement), 2U, WPM_NV_JOURNAL_OK));
    CHECK(!mock_contains(&mock, old_value, sizeof(old_value)));
    CHECK(test_commit(replacement, sizeof(replacement), true, &seq) == WPM_NV_JOURNAL_OK);
    CHECK(seq == 2U && mock.program_calls == programs);
    CHECK(mock.rewrite_attempts == 0U);
    return 1;
}

static int test_rotation_power_loss_after_every_word(void)
{
    mock_flash_t baseline;
    mock_flash_t trial;
    wpm_nv_journal_io_t backend;
    uint8_t old_value[WPM_NV_JOURNAL_MAX_PAYLOAD];
    uint8_t replacement[WPM_NV_JOURNAL_MAX_PAYLOAD];
    uint32_t cut;
    uint64_t seq;
    memset(old_value, 0x71, sizeof(old_value));
    memset(replacement, 0x25, sizeof(replacement));
    CHECK(make_baseline(&baseline, old_value, sizeof(old_value), 6U));
    for (cut = 1U; cut <= 131U; cut++)
    {
        trial = baseline;
        backend = mock_backend(&trial);
        CHECK(test_init(&backend) == WPM_NV_JOURNAL_OK);
        trial.fail_program_at = trial.program_calls + cut;
        trial.fail_program_prefix = 16U;
        trial.power_off_on_failure = 1U;
        CHECK(test_commit(replacement, sizeof(replacement), false, &seq)
              == WPM_NV_JOURNAL_EIO);
        trial.power_off = 0U;
        CHECK(test_init(&backend) == WPM_NV_JOURNAL_OK);
        CHECK(load_matches(cut == 131U ? replacement : old_value,
                           sizeof(old_value), cut == 131U ? 7U : 6U, WPM_NV_JOURNAL_OK));
        CHECK(trial.rewrite_attempts == 0U);
    }
    return 1;
}

static int test_rotation_partial_erase_preserves_the_active_sector(void)
{
    mock_flash_t baseline;
    mock_flash_t trial;
    wpm_nv_journal_io_t backend;
    uint8_t old_value[WPM_NV_JOURNAL_MAX_PAYLOAD];
    uint8_t replacement[WPM_NV_JOURNAL_MAX_PAYLOAD];
    uint8_t active_before[TEST_SECTOR_SIZE];
    uint32_t words;
    uint64_t seq;
    memset(old_value, 0x71, sizeof(old_value));
    memset(replacement, 0x25, sizeof(replacement));
    CHECK(make_baseline(&baseline, old_value, sizeof(old_value), 6U));
    memcpy(active_before, baseline.bytes + TEST_SECTOR_SIZE, sizeof(active_before));
    for (words = 0U; words <= 512U; words++)
    {
        trial = baseline;
        backend = mock_backend(&trial);
        CHECK(test_init(&backend) == WPM_NV_JOURNAL_OK);
        trial.fail_erase_at = trial.erase_calls + 1U;
        trial.fail_erase_prefix = words * 16U;
        CHECK(test_commit(replacement, sizeof(replacement), false, &seq)
              == WPM_NV_JOURNAL_EIO);
        CHECK(seq == 0U);
        CHECK(memcmp(active_before, trial.bytes + TEST_SECTOR_SIZE,
                     sizeof(active_before)) == 0);
        if (words > 0U && words < 393U)
            CHECK(reject_without_mutation(&trial, WPM_NV_JOURNAL_EFOREIGN));
        else
        {
            CHECK(test_init(&backend) == WPM_NV_JOURNAL_OK);
            CHECK(load_matches(old_value, sizeof(old_value), 6U, WPM_NV_JOURNAL_OK));
        }
        CHECK(trial.rewrite_attempts == 0U);
    }
    return 1;
}

static int test_commit_status_loss_and_incomplete_payload_ecc(void)
{
    mock_flash_t mock;
    wpm_nv_journal_io_t backend;
    uint8_t old_value[32];
    uint8_t replacement[17];
    uint64_t seq;
    unsigned int scrub;
    memset(old_value, 0x71, sizeof(old_value));
    memset(replacement, 0x25, sizeof(replacement));
    for (scrub = 0U; scrub < 2U; scrub++)
    {
        CHECK(make_baseline(&mock, old_value, sizeof(old_value), 1U));
        mock.fail_program_at = mock.program_calls + 5U;
        mock.fail_program_prefix = 16U; /* commit word written; status lost */
        CHECK(test_commit(replacement, sizeof(replacement), scrub != 0U, &seq)
              == WPM_NV_JOURNAL_OK);
        CHECK(seq == 2U && mock.rewrite_attempts == 0U);
    }
    CHECK(make_baseline(&mock, old_value, sizeof(old_value), 1U));
    mock.fail_program_at = mock.program_calls + 3U;
    mock.fail_program_prefix = 1U;
    CHECK(test_commit(replacement, sizeof(replacement), false, &seq)
          == WPM_NV_JOURNAL_EIO);
    mock.fail_read_address = TEST_BASE_ADDRESS + 80U + 32U;
    mock.fail_read_enabled = 1U;
    backend = mock_backend(&mock);
    CHECK(test_init(&backend) == WPM_NV_JOURNAL_OK);
    CHECK(load_matches(old_value, sizeof(old_value), 1U, WPM_NV_JOURNAL_OK));
    CHECK(test_commit(replacement, sizeof(replacement), false, &seq)
          == WPM_NV_JOURNAL_OK);
    CHECK(seq == 2U && mock.rewrite_attempts == 0U);
    return 1;
}

static int test_foreign_data_during_commit_is_not_scrubbed(void)
{
    mock_flash_t mock;
    uint8_t old_value[32];
    uint8_t replacement[17];
    uint64_t seq;
    uint32_t erases;
    memset(old_value, 0x71, sizeof(old_value));
    memset(replacement, 0x25, sizeof(replacement));
    CHECK(make_baseline(&mock, old_value, sizeof(old_value), 1U));
    erases = mock.erase_calls;
    mock.inject_foreign_on_commit = 1U;
    CHECK(test_commit(replacement, sizeof(replacement), true, &seq)
          == WPM_NV_JOURNAL_EFOREIGN);
    CHECK(seq == 0U && mock.erase_calls == erases);
    CHECK(reject_without_mutation(&mock, WPM_NV_JOURNAL_EFOREIGN));
    CHECK(mock.bytes[512U] == 0x12U);
    return 1;
}

static int test_all_ff_payload_and_dense_empty_record_rotation(void)
{
    mock_flash_t mock;
    uint8_t payload[WPM_NV_JOURNAL_MAX_PAYLOAD];
    uint64_t seq;
    uint32_t programs;
    uint32_t erases;
    unsigned int i;
    memset(payload, 0xFF, sizeof(payload));
    CHECK(make_baseline(&mock, payload, sizeof(payload), 7U));
    CHECK(load_matches(payload, sizeof(payload), 7U, WPM_NV_JOURNAL_OK));
    CHECK(mock.rewrite_attempts == 0U);
    programs = mock.program_calls;
    erases = mock.erase_calls;
    CHECK(test_commit(payload, WPM_NV_JOURNAL_MAX_PAYLOAD + 1U, true, &seq)
          == WPM_NV_JOURNAL_ECAPACITY);
    CHECK(seq == 0U && mock.program_calls == programs && mock.erase_calls == erases);
    CHECK(make_baseline(&mock, NULL, 0U, 1U));
    for (i = 2U; i <= 341U; i++)
    {
        CHECK(test_commit(NULL, 0U, false, &seq) == WPM_NV_JOURNAL_OK);
        CHECK(seq == i);
    }
    CHECK(load_matches(NULL, 0U, 341U, WPM_NV_JOURNAL_EMPTY));
    CHECK(mock.rewrite_attempts == 0U);
    return 1;
}

static int test_read_faults_and_invalid_metadata_never_mutate(void)
{
    static const uint32_t addresses[] =
    {
        TEST_BASE_ADDRESS, TEST_BASE_ADDRESS + 16U,
        TEST_BASE_ADDRESS + 32U, TEST_BASE_ADDRESS + 48U,
        TEST_BASE_ADDRESS + 64U,
        TEST_BASE_ADDRESS + TEST_SECTOR_SIZE,
        TEST_BASE_ADDRESS + TEST_SECTOR_SIZE + 16U,
        TEST_BASE_ADDRESS + TEST_FLASH_SIZE - 16U
    };
    mock_flash_t mock;
    uint8_t payload[17];
    size_t a;
    unsigned int i;
    uint32_t too_long = WPM_NV_JOURNAL_MAX_PAYLOAD + 1U;
    memset(payload, 0x25, sizeof(payload));
    for (a = 0U; a < sizeof(addresses) / sizeof(addresses[0]); a++)
    {
        CHECK(make_baseline(&mock, payload, sizeof(payload), 1U));
        mock.fail_read_address = addresses[a];
        mock.fail_read_enabled = 1U;
        CHECK(reject_without_mutation(&mock, WPM_NV_JOURNAL_EIO));
    }
    CHECK(make_baseline(&mock, payload, sizeof(payload), 1U));
    mock.bytes[4U] = 2U;
    mock.bytes[12U] ^= 1U;
    CHECK(reject_without_mutation(&mock, WPM_NV_JOURNAL_EFOREIGN));
    CHECK(make_baseline(&mock, payload, sizeof(payload), 1U));
    for (i = 0U; i < 4U; i++)
    {
        mock.bytes[8U + i] = (uint8_t)(too_long >> (i * 8U));
        mock.bytes[12U + i] = (uint8_t)(~too_long >> (i * 8U));
    }
    CHECK(reject_without_mutation(&mock, WPM_NV_JOURNAL_ECORRUPT));
    return 1;
}

static int run_test(const char *name, int (*test)(void))
{
    g_test_name = name;
    if (!test())
    {
        return 0;
    }
    printf("PASS %s\n", name);
    return 1;
}

int main(void)
{
    unsigned int passed = 0U;

    passed += (unsigned int)run_test("empty and basic round trip",
                                    test_empty_and_basic_round_trip);
    passed += (unsigned int)run_test("explicit reset erases both sectors",
                                    test_explicit_reset_erases_and_verifies_both_sectors);
    passed += (unsigned int)run_test("explicit reset fails closed",
                                    test_explicit_reset_fails_closed_on_erase_or_readback_error);
    passed += (unsigned int)run_test("journal v1 golden bytes",
                                    test_journal_v1_golden_bytes);
    passed += (unsigned int)run_test("independent stores and geometry",
                                    test_independent_instances_and_geometry);
    passed += (unsigned int)run_test("invalid reinit clears previous backend",
                                    test_invalid_init_clears_previous_backend);
    passed += (unsigned int)run_test("CRC, version and length handling",
                                    test_crc_version_and_length_fail_closed);
    passed += (unsigned int)run_test("unknown data fails closed",
                                    test_unknown_data_is_never_formatted);
    passed += (unsigned int)run_test("interruption at each record word",
                                    test_interruption_at_each_record_word);
    passed += (unsigned int)run_test("rotation retains last valid record",
                                    test_rotation_preserves_last_valid_record);
    passed += (unsigned int)run_test("scrub write interruptions",
                                    test_scrub_interruptions_do_not_commit_clear_early);
    passed += (unsigned int)run_test("scrub failure and recovery",
                                    test_scrub_failure_is_explicit_and_cannot_fallback);
    passed += (unsigned int)run_test("backend ECC/I/O error",
                                    test_backend_read_error_maps_to_io);
    passed += (unsigned int)run_test("backend address boundaries",
                                    test_backend_boundaries);
    passed += (unsigned int)run_test("unknown data after pending/completed scrub",
                                    test_scrub_never_authorizes_unknown_data);
    passed += (unsigned int)run_test("damaged committed scrub cannot resurrect old",
                                    test_committed_scrub_damage_never_loads_old_credentials);
    passed += (unsigned int)run_test("sequence wrap and conflicts",
                                    test_sequence_wrap_and_conflicts);
    passed += (unsigned int)run_test("erase verified before programming",
                                    test_erase_success_requires_readback_before_programming);
    passed += (unsigned int)run_test("load revalidates delivered bytes",
                                    test_load_rechecks_the_delivered_bytes);
    passed += (unsigned int)run_test("power loss after every word and boundary length",
                                    test_power_loss_after_every_program_word);
    passed += (unsigned int)run_test("torn commit word fails closed",
                                    test_torn_commit_word_fails_closed);
    passed += (unsigned int)run_test("partial retired-sector erase fails closed",
                                    test_partial_old_erase_never_formats_unknown_or_loads_old);
    passed += (unsigned int)run_test("capacity and reentry",
                                    test_capacity_outputs_and_reentry);
    passed += (unsigned int)run_test("nonempty scrub retry keeps the committed value",
                                    test_nonempty_scrub_retry_is_idempotent_and_explicit);
    passed += (unsigned int)run_test("rotation power loss after every word",
                                    test_rotation_power_loss_after_every_word);
    passed += (unsigned int)run_test("rotation partial erase keeps the active sector",
                                    test_rotation_partial_erase_preserves_the_active_sector);
    passed += (unsigned int)run_test("lost commit status and incomplete payload ECC",
                                    test_commit_status_loss_and_incomplete_payload_ecc);
    passed += (unsigned int)run_test("foreign data injected during commit",
                                    test_foreign_data_during_commit_is_not_scrubbed);
    passed += (unsigned int)run_test("all-FF payload and dense empty-record rotation",
                                    test_all_ff_payload_and_dense_empty_record_rotation);
    passed += (unsigned int)run_test("read faults and invalid metadata block all writes",
                                    test_read_faults_and_invalid_metadata_never_mutate);
    if (passed != 30U)
    {
        fprintf(stderr, "%u/30 tests passed\n", passed);
        return EXIT_FAILURE;
    }
    printf("%u/30 tests passed\n", passed);
    return EXIT_SUCCESS;
}
