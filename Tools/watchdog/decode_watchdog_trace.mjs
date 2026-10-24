#!/usr/bin/env node
import fs from 'node:fs';
import { createHash } from 'node:crypto';
import { spawnSync } from 'node:child_process';
import process from 'node:process';

const SIDECAR_SIZE = 0x10000;
const BANK_SIZE = 0x8000;
const BANK_COUNT = 2;
const HEADER_SIZE = 0x100;
const WDF_MAGIC = 0x57444631;
const WDF_VERSION = 1;
const WDF_COMMIT = 0x54524631;
const TRACE_MAGIC = 0x57545243;
const TRACE_VERSION = 1;
const TRACE_COMMIT = 0x54524331;
const TRACE_CAPACITY = 912;
const TRACE_HEADER_SIZE = 32;
const TRACE_SAMPLE_SIZE = 24;

function u32(buffer, offset) {
  return buffer.readUInt32LE(offset) >>> 0;
}

function crc32(buffer) {
  let crc = 0xffffffff;
  for (const byte of buffer) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit += 1) {
      crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
    }
  }
  return (crc ^ 0xffffffff) >>> 0;
}

function hex(value) {
  return `0x${(value >>> 0).toString(16).padStart(8, '0')}`;
}

function serialNewer(left, right) {
  const difference = (left - right) >>> 0;
  return difference !== 0 && difference < 0x80000000;
}

function nextSequence(sequence) {
  return sequence === 0xffffffff ? 1 : (sequence + 1) >>> 0;
}

function sequenceAge(newest, sequence) {
  if (sequence === newest) return 0;
  if (!serialNewer(newest, sequence)) return null;
  // Sequence zero is skipped by the writer when UINT32_MAX wraps to one.
  return newest < sequence
    ? ((0xffffffff - sequence) + newest) >>> 0
    : (newest - sequence) >>> 0;
}

function ringHeaderCrc(payload) {
  return crc32(payload.subarray(0, 5 * 4));
}

function scanRing(payload, bankHeader) {
  if (payload.length !== 0x55a0) return { error: 'wrong_ring_size' };
  const magic = u32(payload, 0);
  const version = u32(payload, 4);
  const capacity = u32(payload, 8);
  const bootSequence = u32(payload, 12);
  const fingerprintCrc32 = u32(payload, 16);
  const headerCrc32 = u32(payload, 20);
  const nextIndex = u32(payload, 24);
  const commit = u32(payload, 28);

  if (magic !== TRACE_MAGIC || version !== TRACE_VERSION ||
      capacity !== TRACE_CAPACITY || nextIndex >= capacity ||
      commit !== TRACE_COMMIT || ringHeaderCrc(payload) !== headerCrc32) {
    return { error: 'invalid_ring_header' };
  }
  if (bootSequence !== bankHeader.app_boot_sequence ||
      fingerprintCrc32 !== bankHeader.fingerprint_crc32) {
    return { error: 'ring_identity_mismatch' };
  }

  const sampleArray = payload.subarray(TRACE_HEADER_SIZE);
  if (sampleArray.length !== capacity * TRACE_SAMPLE_SIZE ||
      crc32(sampleArray) !== bankHeader.samples_crc32) {
    return { error: 'sample_array_crc_mismatch' };
  }

  const committed = [];
  for (let index = 0; index < capacity; index += 1) {
    const offset = TRACE_HEADER_SIZE + index * TRACE_SAMPLE_SIZE;
    const sequence = u32(payload, offset + 20);
    if (sequence === 0) continue;
    committed.push({
      index,
      sequence,
      cycle_count: u32(payload, offset),
      pc: u32(payload, offset + 4),
      lr: u32(payload, offset + 8),
      sp: u32(payload, offset + 12),
      packed_exc_xpsr: u32(payload, offset + 16),
    });
  }

  let newest = null;
  for (const sample of committed) {
    if (newest === null || serialNewer(sample.sequence, newest)) {
      newest = sample.sequence;
    }
  }
  if (newest === null) return { error: 'empty_trace_ring' };

  const inWindow = [];
  let rejectedCount = 0;
  for (const sample of committed) {
    const age = sequenceAge(newest, sample.sequence);
    if (age === null || age >= capacity) {
      rejectedCount += 1;
      continue;
    }
    sample.age = age;
    inWindow.push(sample);
  }
  inWindow.sort((a, b) => b.age - a.age); // oldest to newest

  const oldest = inWindow[0]?.sequence ?? 0;
  if (inWindow.length !== bankHeader.valid_count ||
      rejectedCount !== bankHeader.rejected_count ||
      oldest !== bankHeader.oldest_sequence ||
      newest !== bankHeader.newest_sequence) {
    return { error: 'trace_summary_mismatch' };
  }

  return {
    ring: {
      magic: hex(magic),
      version,
      capacity,
      boot_sequence: bootSequence,
      fingerprint_crc32: hex(fingerprintCrc32),
      next_index: nextIndex,
      valid_samples: inWindow.length,
      rejected_samples: rejectedCount,
      oldest_sequence: oldest,
      newest_sequence: newest,
      samples_crc32: hex(bankHeader.samples_crc32),
    },
    samples: inWindow.map(({ age, ...sample }) => sample),
  };
}

function validateBank(sidecar, bankIndex) {
  const base = bankIndex * BANK_SIZE;
  const header = sidecar.subarray(base, base + HEADER_SIZE);
  const words = Array.from({ length: 64 }, (_, index) => u32(header, index * 4));
  const reasons = [];

  if (words[0] !== WDF_MAGIC) reasons.push('bad_magic');
  if (words[1] !== WDF_VERSION) reasons.push('unsupported_version');
  if (words[2] !== HEADER_SIZE) reasons.push('bad_header_size');
  if (words[3] !== 0x55a0) reasons.push('bad_payload_size');
  if (words[4] !== words[6]) reasons.push('bank_sequence_mismatch');
  if (nextSequence(words[5]) !== words[6]) reasons.push('boot_sequence_mismatch');
  if (words[17] !== WDF_COMMIT) reasons.push('not_committed');
  if (words[18] !== bankIndex) reasons.push('wrong_bank_index');
  if (words[19] !== 0 || words.slice(20).some((word) => word !== 0)) {
    reasons.push('reserved_nonzero');
  }

  const crcInput = Buffer.concat([
    header.subarray(0, 16 * 4),
    header.subarray(18 * 4, 20 * 4),
  ]);
  const headerCrc32 = crc32(crcInput);
  if (headerCrc32 !== words[16]) reasons.push('header_crc_mismatch');

  const payload = sidecar.subarray(base + HEADER_SIZE,
                                    base + HEADER_SIZE + words[3]);
  const payloadCrc32 = crc32(payload);
  if (payloadCrc32 !== words[15]) reasons.push('payload_crc_mismatch');

  const bankHeader = {
    bank_index: bankIndex,
    bank_sequence: words[4],
    app_boot_sequence: words[5],
    reset_boot_sequence: words[6],
    reset_flags: words[7],
    journal_flash_sequence: words[8],
    fingerprint_crc32: words[9],
    valid_count: words[10],
    rejected_count: words[11],
    oldest_sequence: words[12],
    newest_sequence: words[13],
    samples_crc32: words[14],
    payload_crc32: words[15],
    header_crc32: words[16],
    commit: words[17],
  };

  const scan = reasons.length === 0 ? scanRing(payload, bankHeader) : null;
  if (scan?.error) reasons.push(scan.error);
  return {
    valid: reasons.length === 0,
    reasons,
    header: Object.fromEntries(Object.entries(bankHeader).map(([key, value]) =>
      [key, key.endsWith('_crc32') ? hex(value) : value])),
    samples: scan?.samples ?? [],
    ring: scan?.ring ?? null,
  };
}

function symbolize(samples, elf, addr2line) {
  if (!elf) return;
  const addresses = [...new Set(samples.flatMap((sample) =>
    [sample.pc, sample.lr].filter((address) => address !== 0)
      .map((address) => (address & ~1) >>> 0)))];
  if (addresses.length === 0) return;

  const result = spawnSync(addr2line,
    ['-f', '-C', '-e', elf, ...addresses.map((address) => hex(address))],
    { encoding: 'utf8', windowsHide: true, maxBuffer: 8 * 1024 * 1024 });
  if (result.error || result.status !== 0) {
    throw new Error(`addr2line failed: ${result.error?.message ?? result.stderr}`);
  }
  const lines = result.stdout.trimEnd().split(/\r?\n/);
  if (lines.length < addresses.length * 2) {
    throw new Error('addr2line returned fewer results than requested');
  }
  const symbols = new Map();
  addresses.forEach((address, index) => {
    symbols.set(address, `${lines[index * 2]} at ${lines[index * 2 + 1]}`);
  });
  for (const sample of samples) {
    if (sample.pc !== 0) sample.pc_symbol = symbols.get((sample.pc & ~1) >>> 0);
    if (sample.lr !== 0) sample.lr_symbol = symbols.get((sample.lr & ~1) >>> 0);
  }
}

function parseArgs(argv) {
  const result = { input: null, elf: null, addr2line: 'arm-none-eabi-addr2line', pretty: false };
  for (let index = 0; index < argv.length; index += 1) {
    const arg = argv[index];
    if (arg === '--elf') result.elf = argv[++index];
    else if (arg === '--addr2line') result.addr2line = argv[++index];
    else if (arg === '--pretty') result.pretty = true;
    else if (arg.startsWith('-')) throw new Error(`unknown option: ${arg}`);
    else if (result.input === null) result.input = arg;
    else throw new Error(`unexpected argument: ${arg}`);
  }
  return result;
}

function main() {
  const args = parseArgs(process.argv.slice(2));
  if (!args.input) {
    throw new Error('Usage: node decode_watchdog_trace.mjs <64-KiB-sidecar.bin> [--elf matching-app.elf] [--addr2line path] [--pretty]');
  }
  const bytes = fs.readFileSync(args.input);
  if (bytes.length !== SIDECAR_SIZE) {
    throw new Error(`expected 64 KiB sidecar, got ${bytes.length} bytes`);
  }

  const banks = Array.from({ length: BANK_COUNT }, (_, index) =>
    validateBank(bytes, index));
  const validBanks = banks.map((bank, index) => ({ ...bank, index }))
    .filter((bank) => bank.valid);
  if (validBanks.length === 0) {
    const output = { schema: 'artpi2-watchdog-trace-v1', banks };
    process.stdout.write(`${JSON.stringify(output, null, args.pretty ? 2 : 0)}\n`);
    process.exitCode = 2;
    return;
  }

  let selected = validBanks[0];
  for (const bank of validBanks.slice(1)) {
    if (serialNewer(bank.header.bank_sequence, selected.header.bank_sequence)) {
      selected = bank;
    }
  }
  if (args.elf) symbolize(selected.samples, args.elf, args.addr2line);

  const output = {
    schema: 'artpi2-watchdog-trace-v1',
    input: args.input,
    sidecar_bytes: bytes.length,
    sidecar_sha256: createHash('sha256').update(bytes).digest('hex'),
    selected_bank: selected.index,
    banks: banks.map(({ samples, ring, ...bank }) => bank),
    trace: { ...selected.header, ring: selected.ring, samples: selected.samples },
  };
  process.stdout.write(`${JSON.stringify(output, null, args.pretty ? 2 : 0)}\n`);
}

try {
  main();
} catch (error) {
  process.stderr.write(`${error.message}\n`);
  process.exitCode = 1;
}
