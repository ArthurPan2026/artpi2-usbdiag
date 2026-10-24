#!/usr/bin/env node
import fs from 'node:fs';
import path from 'node:path';
import { createHash } from 'node:crypto';
import { spawnSync } from 'node:child_process';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

const JOURNAL_SIZE = 0x41000;
const JOURNAL_SLOT_SIZE = 0x1000;
const JOURNAL_SLOT_COUNT = JOURNAL_SIZE / JOURNAL_SLOT_SIZE;
const TRACE_SIZE = 0x10000;
const TRACE_BANK_SIZE = 0x8000;
const RETAINED_SIZE = 0x1000;
const toolDirectory = path.dirname(fileURLToPath(import.meta.url));
const logDecoder = path.join(toolDirectory, 'decode_watchdog_log.mjs');
const traceDecoder = path.join(toolDirectory, 'decode_watchdog_trace.mjs');

function parseArguments(argv) {
  const args = {
    journal: null,
    trace: null,
    retainedFirst: null,
    retainedSecond: null,
    outputDirectory: null,
    appElf: null,
    bootElf: null,
    addr2line: null,
  };
  const keys = new Map([
    ['--journal', 'journal'],
    ['--trace', 'trace'],
    ['--retained-first', 'retainedFirst'],
    ['--retained-second', 'retainedSecond'],
    ['--output-directory', 'outputDirectory'],
    ['--app-elf', 'appElf'],
    ['--boot-elf', 'bootElf'],
    ['--addr2line', 'addr2line'],
  ]);
  for (let index = 0; index < argv.length; index += 1) {
    const key = keys.get(argv[index]);
    if (!key) throw new Error('Unknown argument: ' + argv[index]);
    const value = argv[++index];
    if (!value) throw new Error(argv[index - 1] + ' requires a value');
    args[key] = path.resolve(value);
  }
  for (const key of ['journal', 'trace', 'retainedFirst',
                     'retainedSecond', 'outputDirectory']) {
    if (!args[key]) throw new Error('Missing required argument: ' + key);
  }
  return args;
}

function sha256(bytes) {
  return createHash('sha256').update(bytes).digest('hex').toUpperCase();
}

function allBytesEqual(bytes, value) {
  return bytes.every(byte => byte === value);
}

function allErased(bytes) {
  return allBytesEqual(bytes, 0xff);
}

function allZero(bytes) {
  return allBytesEqual(bytes, 0);
}

function readSizedFile(filePath, expectedSize, label) {
  const bytes = fs.readFileSync(filePath);
  if (bytes.length !== expectedSize) {
    throw new Error(label + ' must be ' + expectedSize +
      ' bytes; got ' + bytes.length + ': ' + filePath);
  }
  return bytes;
}

function writeJson(filePath, value) {
  fs.writeFileSync(filePath, JSON.stringify(value, null, 2) + '\n',
    { encoding: 'utf8' });
}

function decoderArguments(elfs, addr2line) {
  const result = [];
  for (const elf of elfs) {
    if (!fs.existsSync(elf)) throw new Error('ELF file not found: ' + elf);
    result.push('--elf', elf);
  }
  if (addr2line) result.push('--addr2line', addr2line);
  return result;
}

function runDecoder(decoderPath, inputPath, extraArguments) {
  const result = spawnSync(process.execPath,
    [decoderPath, inputPath, '--pretty', ...extraArguments], {
      encoding: 'utf8',
      windowsHide: true,
      maxBuffer: 32 * 1024 * 1024,
    });
  if (result.error) {
    return {
      json: null,
      exitCode: null,
      stdout: result.stdout || '',
      stderr: result.error.message,
    };
  }
  const stdout = (result.stdout || '').trim();
  let json = null;
  try {
    json = JSON.parse(stdout);
  } catch {
    json = null;
  }
  return {
    json,
    exitCode: result.status,
    stdout,
    stderr: (result.stderr || '').trim(),
  };
}

function writeDecodeResult(outputPath, result, emptyValue) {
  let saved;
  if (emptyValue) {
    saved = emptyValue;
  } else if (result.json !== null) {
    saved = result.json;
  } else {
    saved = {
      status: 'CORRUPT_OR_UNDECODABLE',
      decoder_exit_code: result.exitCode,
      decoder_error: result.stderr || 'Decoder did not emit valid JSON',
      decoder_output: result.stdout || null,
    };
  }
  writeJson(outputPath, saved);
  return saved;
}

function summarizeJournal(bytes, decoded, decodeResult) {
  const records = Array.isArray(decoded?.records) ? decoded.records : [];
  const validSlots = new Set(records.map(record => record.slot));
  const dirtySlots = [];
  for (let slot = 0; slot < JOURNAL_SLOT_COUNT; slot += 1) {
    const begin = slot * JOURNAL_SLOT_SIZE;
    const section = bytes.subarray(begin, begin + JOURNAL_SLOT_SIZE);
    if (!allErased(section)) dirtySlots.push(slot);
  }
  const invalidSlots = dirtySlots.filter(slot => !validSlots.has(slot));
  let status;
  if (allErased(bytes)) status = 'EMPTY';
  else if (records.length > 0 && invalidSlots.length === 0) status = 'VALID';
  else if (records.length > 0) status = 'PARTIAL';
  else status = 'CORRUPT_OR_INCOMPLETE';

  const summarizedRecords = records.map(record => ({
    slot: record.slot,
    flash_sequence: record.flash_sequence,
    incident_id: record.incident_id,
    reset_source: record.reset_source,
    reset_flags: record.reset_flags,
    reset_count: record.reset_count,
    fault_kind: record.fault?.kind || null,
    first_failure: record.first_failure?.reason || null,
    event_count: record.events?.length || 0,
    event_types: (record.events || []).map(event => event.event),
  }));
  return {
    status,
    valid_records: records.length,
    dirty_slots: dirtySlots,
    invalid_slots: invalidSlots,
    records: summarizedRecords,
    decoder_exit_code: decodeResult.exitCode,
  };
}

function summarizeTrace(bytes, decoded, decodeResult) {
  const banks = Array.isArray(decoded?.banks) ? decoded.banks : [];
  const bankStates = banks.map((bank, index) => {
    const begin = index * TRACE_BANK_SIZE;
    const raw = bytes.subarray(begin, begin + TRACE_BANK_SIZE);
    return {
      index,
      valid: bank.valid === true,
      blank: allErased(raw),
      reasons: bank.reasons || [],
      bank_sequence: bank.header?.bank_sequence ?? null,
      valid_samples: bank.header?.valid_count ?? 0,
    };
  });
  const validBanks = bankStates.filter(bank => bank.valid);
  const dirtyInvalidBanks = bankStates.filter(bank => !bank.valid && !bank.blank);
  let status;
  if (allErased(bytes)) status = 'EMPTY';
  else if (validBanks.length === 0) status = 'CORRUPT_OR_INCOMPLETE';
  else if (dirtyInvalidBanks.length > 0) status = 'PARTIAL';
  else status = 'VALID';

  return {
    status,
    selected_bank: decoded?.selected_bank ?? null,
    valid_banks: validBanks.length,
    dirty_invalid_banks: dirtyInvalidBanks.map(bank => bank.index),
    banks: bankStates,
    valid_samples: decoded?.trace?.ring?.valid_samples ??
      decoded?.trace?.samples?.length ?? 0,
    boot_sequence: decoded?.trace?.ring?.boot_sequence ??
      decoded?.trace?.app_boot_sequence ?? null,
    app_boot_sequence: decoded?.trace?.app_boot_sequence ?? null,
    reset_boot_sequence: decoded?.trace?.reset_boot_sequence ?? null,
    decoder_exit_code: decodeResult.exitCode,
  };
}

function summarizeRetained(bytes, decoded, decodeResult) {
  if (allErased(bytes)) {
    return { status: 'EMPTY', record: null, decoder_exit_code: 0 };
  }
  if (allZero(bytes)) {
    return { status: 'CLEARED_OR_UNINITIALIZED', record: null,
      decoder_exit_code: 0 };
  }
  const record = decoded?.records?.[0] ?? null;
  if (!record) {
    return {
      status: 'CORRUPT_OR_TORN',
      record: null,
      decoder_exit_code: decodeResult.exitCode,
      decoder_error: decodeResult.stderr || null,
    };
  }
  const footer = record.retained_footer || {};
  const resetFlags = Number.parseInt(record.reset_flags || '0', 16) >>> 0;
  const watchdogReset =
    (resetFlags & (0x04000000 | 0x10000000)) !== 0;
  const pendingFlash =
    Number.parseInt(footer.pending_flash_magic || '0', 16) === 0x504e4431;
  const faultResetRequested =
    Number.parseInt(footer.fault_reset_requested || '0', 16) === 0x46525331;
  const faultEvidence = Boolean(record.fault || record.first_failure ||
    record.incident_id || watchdogReset || pendingFlash || faultResetRequested ||
    (record.events || []).some(event => event.event === 'fault'));
  const eventCounts = {};
  for (const event of record.events || []) {
    eventCounts[event.event] = (eventCounts[event.event] || 0) + 1;
  }
  const retainedSummary = {
    has_fault_evidence: faultEvidence,
    incident_id: record.incident_id,
    reset_flags: record.reset_flags,
    reset_count: record.reset_count,
    boot_sequence: record.header?.boot_sequence ?? null,
    core_ready: record.header?.core_ready ?? null,
    active_operation: record.header?.active_operation ?? null,
    active_stage: record.header?.active_stage ?? null,
    fault_kind: record.fault?.kind || null,
    first_failure: record.first_failure?.reason || null,
    event_counts: eventCounts,
    source_fingerprint_crc32: record.source_fingerprint_crc32,
  };
  return {
    status: 'VALID',
    ...retainedSummary,
    record: retainedSummary,
  };
}

function formatSummary(summary) {
  const lines = [
    'ART-Pi2 watchdog fault readback',
    'Captured: ' + summary.captured_at,
    'Overall: ' + summary.overall_status,
    'Journal: ' + summary.journal.status + ', records=' +
      summary.journal.valid_records + ', invalid_slots=' +
      summary.journal.invalid_slots.length,
    'Trace: ' + summary.trace.status + ', valid_banks=' +
      summary.trace.valid_banks + ', samples=' + summary.trace.valid_samples,
    'BKPSRAM: ' + summary.bkpsram.status + ', reads=' +
      summary.bkpsram.consistency,
  ];
  if (summary.bkpsram.record?.has_fault_evidence) {
    lines.push('BKPSRAM fault evidence: fault=' +
      (summary.bkpsram.record.fault_kind || 'none') + ', first_failure=' +
      (summary.bkpsram.record.first_failure || 'none') + ', reset_flags=' +
      (summary.bkpsram.record.reset_flags || 'unknown'));
  }
  for (const warning of summary.warnings) lines.push('Warning: ' + warning);
  lines.push('Readbacks: ' + summary.output_directory);
  return lines.join('\n') + '\n';
}

function main() {
  const args = parseArguments(process.argv.slice(2));
  if (!fs.existsSync(args.outputDirectory)) {
    fs.mkdirSync(args.outputDirectory, { recursive: true });
  }
  for (const elf of [args.appElf, args.bootElf, args.addr2line]) {
    if (elf && !fs.existsSync(elf)) throw new Error('File not found: ' + elf);
  }

  const journalBytes = readSizedFile(args.journal, JOURNAL_SIZE, 'Journal');
  const traceBytes = readSizedFile(args.trace, TRACE_SIZE, 'Trace sidecar');
  const retainedFirstBytes = readSizedFile(args.retainedFirst, RETAINED_SIZE,
    'First BKPSRAM capture');
  const retainedSecondBytes = readSizedFile(args.retainedSecond, RETAINED_SIZE,
    'Second BKPSRAM capture');

  const logElfs = [];
  if (args.appElf) logElfs.push(args.appElf);
  if (args.bootElf) logElfs.push(args.bootElf);
  const logDecodeArgs = decoderArguments(logElfs, args.addr2line);
  const traceDecodeArgs = decoderArguments(
    args.appElf ? [args.appElf] : [], args.addr2line);
  const logDecoderResult = runDecoder(logDecoder, args.journal, logDecodeArgs);
  const traceDecoderResult = runDecoder(traceDecoder, args.trace, traceDecodeArgs);
  const retainedFirstDecoderResult = allErased(retainedFirstBytes) ||
      allZero(retainedFirstBytes) ? {
        json: null, exitCode: 0, stdout: '', stderr: '',
      } : runDecoder(logDecoder, args.retainedFirst, logDecodeArgs);
  const retainedSecondDecoderResult = allErased(retainedSecondBytes) ||
      allZero(retainedSecondBytes) ? {
        json: null, exitCode: 0, stdout: '', stderr: '',
      } : runDecoder(logDecoder, args.retainedSecond, logDecodeArgs);

  const journalJsonPath = path.join(args.outputDirectory, 'watchdog-journal.json');
  const traceJsonPath = path.join(args.outputDirectory, 'watchdog-trace.json');
  const retainedFirstJsonPath = path.join(args.outputDirectory,
    'watchdog-bkpsram-read1.json');
  const retainedSecondJsonPath = path.join(args.outputDirectory,
    'watchdog-bkpsram-read2.json');

  const journalDecoded = writeDecodeResult(journalJsonPath, logDecoderResult,
    allErased(journalBytes) ? {
      schema: 'artpi2-watchdog-log-v2',
      status: 'EMPTY',
      valid_records: 0,
      records: [],
    } : null);
  const traceDecoded = writeDecodeResult(traceJsonPath, traceDecoderResult,
    allErased(traceBytes) ? {
      schema: 'artpi2-watchdog-trace-v1',
      status: 'EMPTY',
      banks: [],
      trace: null,
    } : null);

  const retainedFirstDecoded = writeDecodeResult(retainedFirstJsonPath,
    retainedFirstDecoderResult,
    allErased(retainedFirstBytes) ? {
      schema: 'artpi2-watchdog-retained-v2',
      status: 'EMPTY',
      records: [],
    } : allZero(retainedFirstBytes) ? {
      schema: 'artpi2-watchdog-retained-v2',
      status: 'CLEARED_OR_UNINITIALIZED',
      records: [],
    } : null);
  const retainedSecondDecoded = writeDecodeResult(retainedSecondJsonPath,
    retainedSecondDecoderResult,
    allErased(retainedSecondBytes) ? {
      schema: 'artpi2-watchdog-retained-v2',
      status: 'EMPTY',
      records: [],
    } : allZero(retainedSecondBytes) ? {
      schema: 'artpi2-watchdog-retained-v2',
      status: 'CLEARED_OR_UNINITIALIZED',
      records: [],
    } : null);

  const journalSummary = summarizeJournal(journalBytes, journalDecoded,
    logDecoderResult);
  const traceSummary = summarizeTrace(traceBytes, traceDecoded,
    traceDecoderResult);
  const retainedFirstSummary = summarizeRetained(retainedFirstBytes,
    retainedFirstDecoded, retainedFirstDecoderResult);
  const retainedSecondSummary = summarizeRetained(retainedSecondBytes,
    retainedSecondDecoded, retainedSecondDecoderResult);
  const retainedFirstHash = sha256(retainedFirstBytes);
  const retainedSecondHash = sha256(retainedSecondBytes);
  const retainedConsistency = retainedFirstHash === retainedSecondHash
    ? 'IDENTICAL_READS'
    : 'CHANGED_BETWEEN_READS';

  const retainedRecord = retainedSecondSummary.record ||
    retainedFirstSummary.record || null;
  const retainedStatus = retainedFirstSummary.status === 'CORRUPT_OR_TORN' ||
      retainedSecondSummary.status === 'CORRUPT_OR_TORN'
    ? 'CORRUPT_OR_TORN'
    : (retainedFirstSummary.status === 'VALID' ||
       retainedSecondSummary.status === 'VALID')
      ? 'VALID'
      : (retainedFirstSummary.status === 'CLEARED_OR_UNINITIALIZED' ||
         retainedSecondSummary.status === 'CLEARED_OR_UNINITIALIZED')
        ? 'CLEARED_OR_UNINITIALIZED' : 'EMPTY';

  const warnings = [];
  const corrupt = journalSummary.status === 'CORRUPT_OR_INCOMPLETE' ||
    journalSummary.status === 'PARTIAL' ||
    traceSummary.status === 'CORRUPT_OR_INCOMPLETE' ||
    traceSummary.status === 'PARTIAL' ||
    retainedStatus === 'CORRUPT_OR_TORN';
  if (retainedConsistency === 'CHANGED_BETWEEN_READS') {
    warnings.push('BKPSRAM changed between two HotPlug reads; each read is non-atomic while firmware is running.');
  }
  if (retainedStatus === 'CORRUPT_OR_TORN') {
    warnings.push('At least one BKPSRAM capture has no valid record header; keep both raw captures for diagnosis.');
  }
  if (journalSummary.invalid_slots.length > 0) {
    warnings.push('Journal contains dirty slots that did not decode as committed records.');
  }
  if (traceSummary.dirty_invalid_banks.length > 0) {
    warnings.push('Trace sidecar contains a nonblank bank without a valid commit and CRC.');
  }

  const incidentFound = journalSummary.valid_records > 0 ||
    traceSummary.valid_banks > 0 ||
    Boolean(retainedRecord?.has_fault_evidence);
  const summary = {
    schema: 'artpi2-watchdog-readback-summary-v1',
    captured_at: new Date().toISOString(),
    overall_status: corrupt ? 'PARTIAL_OR_CORRUPT' :
      incidentFound ? 'INCIDENTS_FOUND' : 'NO_STORED_INCIDENTS',
    output_directory: args.outputDirectory,
    symbolization: {
      app_elf: args.appElf,
      boot_elf: args.bootElf,
      addr2line: args.addr2line,
    },
    raw_files: {
      journal: {
        file: path.basename(args.journal),
        bytes: journalBytes.length,
        sha256: sha256(journalBytes),
      },
      trace: {
        file: path.basename(args.trace),
        bytes: traceBytes.length,
        sha256: sha256(traceBytes),
      },
      bkpsram_read1: {
        file: path.basename(args.retainedFirst),
        bytes: retainedFirstBytes.length,
        sha256: retainedFirstHash,
      },
      bkpsram_read2: {
        file: path.basename(args.retainedSecond),
        bytes: retainedSecondBytes.length,
        sha256: retainedSecondHash,
      },
    },
    journal: journalSummary,
    trace: traceSummary,
    bkpsram: {
      status: retainedStatus,
      consistency: retainedConsistency,
      note: 'Two matching hashes show only that both reads returned identical bytes; SWD HotPlug does not make the 4-KiB record an atomic snapshot.',
      first_read: retainedFirstSummary,
      second_read: retainedSecondSummary,
      record: retainedRecord,
    },
    warnings,
  };

  const summaryPath = path.join(args.outputDirectory, 'summary.json');
  const textPath = path.join(args.outputDirectory, 'summary.txt');
  writeJson(summaryPath, summary);
  fs.writeFileSync(textPath, formatSummary(summary), { encoding: 'utf8' });
  process.stdout.write(formatSummary(summary));
  return corrupt ? 2 : 0;
}

try {
  process.exitCode = main();
} catch (error) {
  process.stderr.write('watchdog readback summary failed: ' +
    (error?.stack || error?.message || String(error)) + '\n');
  process.exitCode = 1;
}
