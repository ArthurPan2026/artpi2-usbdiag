import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

const SIDECAR_SIZE = 0x10000;
const HEADER_SIZE = 0x100;
const TRACE_SIZE = 0x55a0;
const TRACE_CAPACITY = 912;
const WDF_MAGIC = 0x57444631;
const WDF_COMMIT = 0x54524631;
const TRACE_MAGIC = 0x57545243;
const TRACE_COMMIT = 0x54524331;

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

function writeWord(buffer, wordIndex, value) {
  buffer.writeUInt32LE(value >>> 0, wordIndex * 4);
}

function buildSidecar() {
  const sidecar = Buffer.alloc(SIDECAR_SIZE, 0xff);
  const bankBase = 0;
  const trace = Buffer.alloc(TRACE_SIZE, 0);

  writeWord(trace, 0, TRACE_MAGIC);
  writeWord(trace, 1, 1);
  writeWord(trace, 2, TRACE_CAPACITY);
  writeWord(trace, 3, 10);
  writeWord(trace, 4, 0x12345678);
  writeWord(trace, 5, crc32(trace.subarray(0, 20)));
  writeWord(trace, 6, 4);
  writeWord(trace, 7, TRACE_COMMIT);

  for (let index = 0; index < 4; index += 1) {
    const offset = 32 + index * 24;
    trace.writeUInt32LE(1000 + index * 600000, offset);
    trace.writeUInt32LE(0x70010000 + index * 2, offset + 4);
    trace.writeUInt32LE(0x70020001, offset + 8);
    trace.writeUInt32LE(0x24004000 + index * 8, offset + 12);
    trace.writeUInt32LE(0xc1000000, offset + 16);
    trace.writeUInt32LE(index + 1, offset + 20);
  }

  const sampleArray = trace.subarray(32);
  const samplesCrc = crc32(sampleArray);
  const payloadCrc = crc32(trace);
  const header = Buffer.alloc(HEADER_SIZE, 0);
  const words = [
    WDF_MAGIC, 1, HEADER_SIZE, TRACE_SIZE,
    11, 10, 11, 0x04400000, 7, 0x12345678,
    4, 0, 1, 4, samplesCrc, payloadCrc,
    0, WDF_COMMIT, 0, 0,
  ];
  words.forEach((value, index) => writeWord(header, index, value));
  const headerCrcInput = Buffer.concat([
    header.subarray(0, 16 * 4),
    header.subarray(18 * 4, 20 * 4),
  ]);
  writeWord(header, 16, crc32(headerCrcInput));

  header.copy(sidecar, bankBase);
  trace.copy(sidecar, bankBase + HEADER_SIZE);
  return sidecar;
}

const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const decoder = path.join(repoRoot, 'Tools', 'watchdog', 'decode_watchdog_trace.mjs');
const fixturePath = path.join(os.tmpdir(), `watchdog-trace-fixture-${process.pid}.bin`);
const corruptPath = path.join(os.tmpdir(), `watchdog-trace-corrupt-${process.pid}.bin`);

const fixture = buildSidecar();
fs.writeFileSync(fixturePath, fixture);
const valid = spawnSync(process.execPath, [decoder, fixturePath, '--pretty'],
  { encoding: 'utf8', windowsHide: true });
assert.equal(valid.status, 0, valid.stderr);
const report = JSON.parse(valid.stdout);
assert.equal(report.selected_bank, 0);
assert.equal(report.trace.valid_count, 4);
assert.equal(report.trace.newest_sequence, 4);
assert.equal(report.trace.samples.length, 4);
assert.equal(report.trace.samples[0].pc, 0x70010000);
assert.equal(report.trace.samples.at(-1).lr, 0x70020001);
assert.equal(report.trace.samples.at(-1).sequence, 4);
assert.equal(report.banks[1].valid, false);

const corrupt = Buffer.from(fixture);
corrupt[HEADER_SIZE + TRACE_SIZE - 1] ^= 1;
fs.writeFileSync(corruptPath, corrupt);
const invalid = spawnSync(process.execPath, [decoder, corruptPath],
  { encoding: 'utf8', windowsHide: true });
assert.equal(invalid.status, 2);
const invalidReport = JSON.parse(invalid.stdout);
assert.equal(invalidReport.banks[0].valid, false);
assert.ok(invalidReport.banks[0].reasons.includes('payload_crc_mismatch'));

console.log('watchdog trace decoder fixture and CRC rejection passed');
