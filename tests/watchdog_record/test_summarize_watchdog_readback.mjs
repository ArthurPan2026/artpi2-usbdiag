import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

const JOURNAL_SIZE = 0x41000;
const TRACE_SIZE = 0x10000;
const RETAINED_SIZE = 0x1000;
const TRACE_PAYLOAD_SIZE = 0x55a0;
const TRACE_HEADER_SIZE = 0x100;
const TRACE_CAPACITY = 912;
const WDF_MAGIC = 0x57444631;
const WDF_COMMIT = 0x54524631;
const TRACE_MAGIC = 0x57545243;
const TRACE_COMMIT = 0x54524331;
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const summarizer = path.join(root, 'Tools', 'watchdog',
  'summarize_watchdog_readback.mjs');
const temp = fs.mkdtempSync(path.join(os.tmpdir(), 'artpi2-watchdog-summary-'));

function crc32(bytes) {
  let crc = 0xffffffff;
  for (const byte of bytes) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit += 1) {
      crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
    }
  }
  return (crc ^ 0xffffffff) >>> 0;
}

function putWord(bytes, offset, value) {
  bytes.writeUInt32LE(value >>> 0, offset);
}

function putWords(bytes, offset, values) {
  values.forEach((value, index) => putWord(bytes, offset + index * 4, value));
}

function buildRetainedRecord() {
  const record = Buffer.alloc(RETAINED_SIZE, 0);
  const header = Buffer.alloc(128, 0);
  putWords(header, 0, [
    0x57444752, 2, 1, 7, 0, 0, 0, 0, 1, 0, 1, 100, 1,
    0x70001001, 0x24001000, 0x24000100, 0, 0, 0, 0, 0, 100, 31,
    0x12345678,
  ]);
  Buffer.from('0123456789abcdef01234567', 'ascii').copy(header, 96);
  putWord(header, 120, crc32(header.subarray(0, 120)));
  putWord(header, 124, 0x48445231);
  header.copy(record, 0);
  return record;
}

function buildJournal() {
  const journal = Buffer.alloc(JOURNAL_SIZE, 0xff);
  const record = buildRetainedRecord();
  putWords(record, 3840, [0x57444752, 2, 7, 0x53544731, 1, 0]);
  putWord(record, 4092, 0x464c5331);
  const crcInput = Buffer.from(record);
  crcInput.fill(0, 3860, 3864);
  crcInput.fill(0, 4092, 4096);
  putWord(record, 3860, crc32(crcInput));
  record.copy(journal, 0);
  return journal;
}

function buildTraceSidecar(dirtySecondBank = false) {
  const sidecar = Buffer.alloc(TRACE_SIZE, 0xff);
  const trace = Buffer.alloc(TRACE_PAYLOAD_SIZE, 0);
  putWords(trace, 0, [
    TRACE_MAGIC, 1, TRACE_CAPACITY, 10, 0x12345678,
  ]);
  putWord(trace, 20, crc32(trace.subarray(0, 20)));
  putWords(trace, 24, [4, TRACE_COMMIT]);
  for (let index = 0; index < 4; index += 1) {
    const offset = 32 + index * 24;
    putWord(trace, offset, 1000 + index * 600000);
    putWord(trace, offset + 4, 0x70010000 + index * 2);
    putWord(trace, offset + 8, 0x70020001);
    putWord(trace, offset + 12, 0x24004000 + index * 8);
    putWord(trace, offset + 16, 0xc1000000);
    putWord(trace, offset + 20, index + 1);
  }
  const samplesCrc = crc32(trace.subarray(32));
  const payloadCrc = crc32(trace);
  const header = Buffer.alloc(TRACE_HEADER_SIZE, 0);
  putWords(header, 0, [
    WDF_MAGIC, 1, TRACE_HEADER_SIZE, TRACE_PAYLOAD_SIZE,
    11, 10, 11, 0x04400000, 7, 0x12345678,
    4, 0, 1, 4, samplesCrc, payloadCrc,
    0, WDF_COMMIT, 0, 0,
  ]);
  const crcInput = Buffer.concat([
    header.subarray(0, 16 * 4),
    header.subarray(18 * 4, 20 * 4),
  ]);
  putWord(header, 16 * 4, crc32(crcInput));
  header.copy(sidecar, 0);
  trace.copy(sidecar, TRACE_HEADER_SIZE);
  if (dirtySecondBank) sidecar[TRACE_SIZE / 2] = 0;
  return sidecar;
}

function runCase(name, values) {
  const directory = path.join(temp, name);
  const outputDirectory = path.join(directory, 'output');
  fs.mkdirSync(outputDirectory, { recursive: true });
  const files = {
    journal: path.join(directory, 'journal.bin'),
    trace: path.join(directory, 'trace.bin'),
    retainedFirst: path.join(directory, 'retained-first.bin'),
    retainedSecond: path.join(directory, 'retained-second.bin'),
  };
  fs.writeFileSync(files.journal, values.journal);
  fs.writeFileSync(files.trace, values.trace);
  fs.writeFileSync(files.retainedFirst, values.retainedFirst);
  fs.writeFileSync(files.retainedSecond, values.retainedSecond);
  const result = spawnSync(process.execPath, [
    summarizer,
    '--journal', files.journal,
    '--trace', files.trace,
    '--retained-first', files.retainedFirst,
    '--retained-second', files.retainedSecond,
    '--output-directory', outputDirectory,
  ], { encoding: 'utf8', windowsHide: true, maxBuffer: 32 * 1024 * 1024 });
  const reportPath = path.join(outputDirectory, 'summary.json');
  assert.ok(fs.existsSync(reportPath), name + ' did not preserve summary.json: ' + result.stderr);
  return { result, summary: JSON.parse(fs.readFileSync(reportPath, 'utf8')),
    outputDirectory };
}

try {
  const erasedJournal = Buffer.alloc(JOURNAL_SIZE, 0xff);
  const erasedTrace = Buffer.alloc(TRACE_SIZE, 0xff);
  const erasedRetained = Buffer.alloc(RETAINED_SIZE, 0xff);
  const empty = runCase('empty', {
    journal: erasedJournal,
    trace: erasedTrace,
    retainedFirst: erasedRetained,
    retainedSecond: erasedRetained,
  });
  assert.equal(empty.result.status, 0, empty.result.stderr);
  assert.equal(empty.summary.overall_status, 'NO_STORED_INCIDENTS');
  assert.equal(empty.summary.journal.status, 'EMPTY');
  assert.equal(empty.summary.trace.status, 'EMPTY');
  assert.equal(empty.summary.bkpsram.status, 'EMPTY');

  const validRetained = buildRetainedRecord();
  const valid = runCase('valid-single-bank', {
    journal: buildJournal(),
    trace: buildTraceSidecar(false),
    retainedFirst: validRetained,
    retainedSecond: validRetained,
  });
  assert.equal(valid.result.status, 0, valid.result.stderr + '\n' + valid.result.stdout);
  assert.equal(valid.summary.overall_status, 'INCIDENTS_FOUND');
  assert.equal(valid.summary.journal.status, 'VALID');
  assert.equal(valid.summary.journal.valid_records, 1);
  assert.equal(valid.summary.trace.status, 'VALID');
  assert.equal(valid.summary.trace.valid_banks, 1);
  assert.equal(valid.summary.trace.dirty_invalid_banks.length, 0);
  assert.equal(valid.summary.trace.boot_sequence, 10);
  assert.equal(valid.summary.trace.app_boot_sequence, 10);
  assert.equal(valid.summary.trace.reset_boot_sequence, 11);
  assert.equal(valid.summary.bkpsram.status, 'VALID');
  assert.equal(valid.summary.bkpsram.consistency, 'IDENTICAL_READS');

  const retainedResidual = Buffer.from(validRetained);
  putWord(retainedResidual, 3868, 0x12345678);
  putWord(retainedResidual, 3872, 0x87654321);
  const residual = runCase('retained-nonmagic-residual', {
    journal: erasedJournal,
    trace: erasedTrace,
    retainedFirst: retainedResidual,
    retainedSecond: retainedResidual,
  });
  assert.equal(residual.result.status, 0, residual.result.stderr);
  assert.equal(residual.summary.bkpsram.record.has_fault_evidence, false);
  assert.equal(residual.summary.overall_status, 'NO_STORED_INCIDENTS');

  const pendingRetained = Buffer.from(validRetained);
  putWord(pendingRetained, 3868, 0x504e4431);
  const pending = runCase('retained-valid-pending-marker', {
    journal: erasedJournal,
    trace: erasedTrace,
    retainedFirst: pendingRetained,
    retainedSecond: pendingRetained,
  });
  assert.equal(pending.result.status, 0, pending.result.stderr);
  assert.equal(pending.summary.bkpsram.record.has_fault_evidence, true);
  assert.equal(pending.summary.overall_status, 'INCIDENTS_FOUND');

  const faultResetRetained = Buffer.from(validRetained);
  putWord(faultResetRetained, 3872, 0x46525331);
  const faultReset = runCase('retained-valid-fault-reset-marker', {
    journal: erasedJournal,
    trace: erasedTrace,
    retainedFirst: faultResetRetained,
    retainedSecond: faultResetRetained,
  });
  assert.equal(faultReset.result.status, 0, faultReset.result.stderr);
  assert.equal(faultReset.summary.bkpsram.record.has_fault_evidence, true);
  assert.equal(faultReset.summary.overall_status, 'INCIDENTS_FOUND');

  const corruptJournal = Buffer.from(erasedJournal);
  corruptJournal[0] = 0;
  const dirtyTrace = Buffer.from(erasedTrace);
  dirtyTrace[10] = 0;
  const tornRetained = Buffer.alloc(RETAINED_SIZE, 0);
  const corrupt = runCase('corrupt', {
    journal: corruptJournal,
    trace: dirtyTrace,
    retainedFirst: tornRetained,
    retainedSecond: validRetained,
  });
  assert.equal(corrupt.result.status, 2);
  assert.equal(corrupt.summary.overall_status, 'PARTIAL_OR_CORRUPT');
  assert.equal(corrupt.summary.journal.status, 'CORRUPT_OR_INCOMPLETE');
  assert.equal(corrupt.summary.trace.status, 'CORRUPT_OR_INCOMPLETE');
  assert.equal(corrupt.summary.bkpsram.status, 'VALID');
  assert.equal(corrupt.summary.bkpsram.consistency, 'CHANGED_BETWEEN_READS');
  assert.ok(corrupt.summary.warnings.some(item => item.includes('BKPSRAM changed')));
  assert.ok(fs.existsSync(path.join(corrupt.outputDirectory,
    'watchdog-journal.json')));

  const partialTrace = runCase('partial-trace-bank', {
    journal: erasedJournal,
    trace: buildTraceSidecar(true),
    retainedFirst: validRetained,
    retainedSecond: validRetained,
  });
  assert.equal(partialTrace.result.status, 2);
  assert.equal(partialTrace.summary.trace.status, 'PARTIAL');
  assert.equal(partialTrace.summary.trace.valid_banks, 1);
  assert.deepEqual(partialTrace.summary.trace.dirty_invalid_banks, [1]);

  console.log('watchdog readback summary classification tests passed');
} finally {
  fs.rmSync(temp, { recursive: true, force: true });
}
