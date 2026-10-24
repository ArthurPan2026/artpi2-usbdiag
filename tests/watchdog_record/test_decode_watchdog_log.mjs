import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const currentDir = path.dirname(fileURLToPath(import.meta.url));
const decoder = path.resolve(currentDir, '../../Tools/watchdog/decode_watchdog_log.mjs');
const crc32 = (data) => {
  let crc = 0xffffffff;
  for (const byte of data) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit += 1) {
      crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
    }
  }
  return (crc ^ 0xffffffff) >>> 0;
};

function refreshRecordChecksums(record) {
  const fault = record.subarray(256, 768);
  fault.writeUInt32LE(crc32(fault.subarray(0, 124)), 124);
  const flashRecord = record.subarray(0, 0x1000);
  const crcData = Buffer.from(flashRecord);
  crcData.fill(0, 3860, 3864);
  crcData.fill(0, 4092, 4096);
  flashRecord.writeUInt32LE(crc32(crcData), 3860);
}

function putWords(buffer, offset, values) {
  values.forEach((value, index) => buffer.writeUInt32LE(value >>> 0, offset + index * 4));
}

function makeRecord(faultKind = 4, failureReason = 4, formatVersion = 2) {
  const record = Buffer.alloc(0x1000);
  const header = Buffer.alloc(128);
  const prefix = Buffer.from('0123456789abcdef01234567', 'ascii');
  const fields = [
    0x57444752, formatVersion, 4, 9, 0x04000000, 1, 120, 1,
    1, 1, 1, 2500, 1, 0x70001235, 0x24001000, 0x24000100,
    0, 0, 0, 0, 0, 2400, 3, 0x12345678,
  ];
  putWords(header, 0, fields);
  prefix.copy(header, 96);
  header.writeUInt32LE(crc32(header.subarray(0, 120)), 120);
  header.writeUInt32LE(0x48445231, 124);
  header.copy(record, 0);

  const fault = Buffer.alloc(512);
  const faultWords = Array(31).fill(0);
  faultWords[0] = 0x57444752;
  faultWords[1] = formatVersion;
  faultWords[2] = faultKind;
  faultWords[3] = 0xfffffffd;
  faultWords[4] = 0x100;
  faultWords[5] = 0x80000000;
  faultWords[9] = 0x24002000;
  faultWords[13] = 2000;
  faultWords.splice(23, 8, 1, 2, 3, 4, 12, 0x24002000, 0x70001001, 0x01000000);
  putWords(fault, 0, faultWords);
  fault.writeUInt32LE(crc32(fault.subarray(0, 124)), 124);
  fault.writeUInt32LE(0x464c5431, 508);
  fault.copy(record, 256);

  const event = Buffer.alloc(32);
  putWords(event, 0, [1, 2500, 4, 1, 1, 0x70001235, 0x24001000]);
  event.writeUInt32LE(crc32(event.subarray(0, 28)), 28);
  event.copy(record, 768);
  const testMarker = Buffer.alloc(32);
  putWords(testMarker, 0, [2, 2600, 8, 0x54455354, 12,
    0x70001235, 0x24001000]);
  testMarker.writeUInt32LE(crc32(testMarker.subarray(0, 28)), 28);
  testMarker.copy(record, 800);

  const context = Buffer.alloc(128);
  const contextWords = Array(32).fill(0);
  contextWords[0] = 0x43545831;
  contextWords[1] = 6;
  contextWords[2] = 6500;
  contextWords[3] = 0x12345678;
  contextWords[4] = 0x24000100;
  contextWords[9] = 0x7000abcd;
  contextWords[10] = 0x70001235;
  contextWords[11] = 0x24002000;
  contextWords[12] = 0x20001000;
  contextWords[13] = 0xfffffffd;
  contextWords[14] = 0;
  contextWords[24] = 1 << 24;
  contextWords[25] = 120;
  contextWords[26] = 98;
  contextWords[27] = 2;
  putWords(context, 0, contextWords);
  Buffer.from('mongoose', 'ascii').copy(context, 20);
  context.writeUInt32LE(crc32(context.subarray(0, 120)), 120);
  context.writeUInt32LE(0x43545843, 124);
  context.copy(record, 564);

  const component = Buffer.alloc(36);
  putWords(component, 0, [3, 6400, 0x24001000, 2, 31,
    0x70004567, 0x24002000]);
  component.writeUInt32LE(crc32(component.subarray(0, 28)), 28);
  component.writeUInt32LE(0x434d0002, 32);
  component.copy(record, 256 + 128 + 9 * 4);

  const interruptedFrame = Buffer.alloc(36);
  putWords(interruptedFrame, 0, [6, 0x10, 0x11, 0x12, 0x13, 0x14, 0x01000000]);
  interruptedFrame.writeUInt32LE(crc32(interruptedFrame.subarray(0, 28)), 28);
  interruptedFrame.writeUInt32LE(0x46520001, 32);
  interruptedFrame.copy(record, 692);

  const firstFailure = Buffer.alloc(41 * 4);
  const failureWords = Array(41).fill(0);
  failureWords[0] = 0x46465331;
  failureWords[1] = 6;
  failureWords[2] = 6500;
  failureWords[3] = failureReason;
  failureWords[4] = failureReason === 4 ? 1 : faultKind;
  failureWords[5] = 5000;
  failureWords[6] = 6;
  failureWords[7] = 6500;
  failureWords[8] = 0x12345678;
  failureWords[9] = 0x24000100;
  failureWords[14] = 0x7000abcd;
  failureWords[15] = 0x70001235;
  failureWords[16] = 0x24002000;
  failureWords[17] = 0x20001000;
  failureWords[18] = 0xfffffffd;
  failureWords[29] = (1 << 27) | (1 << 5);
  failureWords[30] = 120;
  failureWords[33] = 0x10;
  failureWords[34] = 0x11;
  failureWords[35] = 0x12;
  failureWords[36] = 0x13;
  failureWords[37] = 0x14;
  failureWords[38] = 0x01000000;
  failureWords[40] = 0x46464331;
  putWords(firstFailure, 0, failureWords);
  Buffer.from('mongoose', 'ascii').copy(firstFailure, 10 * 4);
  firstFailure.writeUInt32LE(crc32(firstFailure.subarray(0, 39 * 4)), 39 * 4);
  firstFailure.writeUInt32LE(0x46464331, 40 * 4);
  firstFailure.copy(record, 3840 + (7 + 15) * 4);

  const incidentWords = Buffer.alloc(12);
  putWords(incidentWords, 0, [0x49494431, 0x12345678, 1]);
  putWords(record, 3840 + (7 + 11) * 4,
    [0x49494431, 0x12345678, 1, crc32(incidentWords)]);

  if (faultKind === 5) {
    const panicStack = Buffer.alloc(95 * 4);
    const panicWords = Array(95).fill(0);
    panicWords[0] = 0x504e5331;
    panicWords[1] = 1;
    panicWords[2] = 0x24002000;
    panicWords[3] = 12;
    panicWords[5] = 0x20;
    panicWords[6] = 0x21;
    panicWords[7] = 0x22;
    panicWords[8] = 0x23;
    panicWords[94] = 0x504e4331;
    putWords(panicStack, 0, panicWords);
    panicStack.writeUInt32LE(crc32(panicStack.subarray(0, 94 * 4)), 4 * 4);
    panicStack.writeUInt32LE(0x504e4331, 94 * 4);
    panicStack.copy(record, 256 + 128);
  }

  putWords(record, 3840, [0x57444752, formatVersion, 9, 0x53544731, 1, 0]);
  record.writeUInt32LE(0x464c5331, 4092);
  const crcData = Buffer.from(record);
  crcData.fill(0, 3860, 3864);
  crcData.fill(0, 4092, 4096);
  record.writeUInt32LE(crc32(crcData), 3860);
  return record;
}

function runDecoder(journal, extraArgs = []) {
  const tempDir = fs.mkdtempSync(path.join(os.tmpdir(), 'artpi2-wdg-test-'));
  const input = path.join(tempDir, 'journal.bin');
  fs.writeFileSync(input, journal);
  const result = spawnSync(process.execPath,
    [decoder, input, '--pretty', ...extraArgs], {
    encoding: 'utf8',
    windowsHide: true,
  });
  fs.rmSync(tempDir, { recursive: true, force: true });
  return result;
}

function runDecoderWithUnmatchedElf(journal) {
  const tempDir = fs.mkdtempSync(path.join(os.tmpdir(), 'artpi2-wdg-elf-test-'));
  const input = path.join(tempDir, 'journal.bin');
  const elf = path.join(tempDir, 'unmatched.elf');
  fs.writeFileSync(input, journal);
  fs.writeFileSync(elf, Buffer.from('different build fingerprint', 'ascii'));
  const result = spawnSync(process.execPath,
    [decoder, input, '--pretty', '--elf', elf], {
      encoding: 'utf8',
      windowsHide: true,
    });
  fs.rmSync(tempDir, { recursive: true, force: true });
  return result;
}

const journal = Buffer.alloc(0x41000, 0xff);
makeRecord().copy(journal, 0);
let result = runDecoder(journal);
assert.equal(result.status, 0, result.stderr);
let decoded = JSON.parse(result.stdout);
assert.equal(decoded.schema, 'artpi2-watchdog-log-v2');
assert.equal(decoded.valid_records, 1);
assert.equal(decoded.records[0].fault.kind, 'hardfault');
assert.equal(decoded.records[0].fault.pc, '0x70001001');
assert.equal(decoded.records[0].fault.pc_valid, true);
assert.equal(decoded.records[0].fault.frame_valid, true);
assert.equal(decoded.records[0].fault.hardfault_debug_event, true);
assert.equal(decoded.records[0].fault.hardfault_forced, false);
assert.equal(decoded.records[0].fault.vector_table_bus_fault, false);
assert.equal(decoded.records[0].incident_id, '4600387192');
assert.equal(decoded.records[0].first_failure.reason, 'required_component_stale');
assert.equal(decoded.records[0].first_failure.subject, 1);
assert.equal(decoded.records[0].first_failure.subject_name, 'mongoose');
assert.equal(decoded.records[0].first_failure.thread_name, 'mongoose');
assert.equal(decoded.records[0].first_failure.frame_valid, true);
assert.equal(decoded.records[0].first_failure.pc, '0x7000ABCD');
assert.equal(decoded.records[0].events[0].event, 'core_progress');
assert.equal(decoded.records[0].events[1].diagnostic_subject, 'controlled_debug_test');
assert.equal(decoded.records[0].runtime_context.thread_name, 'mongoose');
assert.equal(decoded.records[0].runtime_context.pc, '0x7000ABCD');
assert.equal(decoded.records[0].runtime_context.pc_valid, true);
assert.equal(decoded.records[0].runtime_context.ltdc_frames, 120);
assert.equal(decoded.records[0].components[0].component, 'mongoose');
assert.equal(decoded.records[0].components[0].stage, 'active');
assert.equal(decoded.records[0].components[0].heartbeat_callsite_pc, '0x70004567');
assert.equal(decoded.records[0].interrupted_frame.sequence, 6);
assert.equal(decoded.records[0].interrupted_frame.r0, '0x00000010');
assert.equal(decoded.records[0].interrupted_frame.matches_runtime_context, true);

const faultTimeFirstFailureRecord = makeRecord();
const firstFailureOffset = 3840 + (7 + 15) * 4;
const firstFailureChunk = faultTimeFirstFailureRecord.subarray(
  firstFailureOffset, firstFailureOffset + 41 * 4);
firstFailureChunk.writeUInt32LE(0x70000000, 9 * 4);
firstFailureChunk.writeUInt32LE(0x00000002, 10 * 4);
firstFailureChunk.writeUInt32LE(0x00000400, 11 * 4);
firstFailureChunk.writeUInt32LE(0x00000001, 12 * 4);
firstFailureChunk.writeUInt32LE(0x00000040, 13 * 4);
firstFailureChunk.writeUInt32LE(
  ((1 << 31) | (1 << 27) | (1 << 24) | (1 << 5)) >>> 0, 29 * 4);
firstFailureChunk.writeUInt32LE(0x11, 30 * 4);
firstFailureChunk.writeUInt32LE(0x22, 31 * 4);
firstFailureChunk.writeUInt32LE(0x33, 32 * 4);
firstFailureChunk.writeUInt32LE(
  crc32(firstFailureChunk.subarray(0, 39 * 4)), 39 * 4);
refreshRecordChecksums(faultTimeFirstFailureRecord);
const faultTimeFirstFailureJournal = Buffer.alloc(0x41000, 0xff);
faultTimeFirstFailureRecord.copy(faultTimeFirstFailureJournal, 0);
result = runDecoder(faultTimeFirstFailureJournal);
assert.equal(result.status, 0, result.stderr);
decoded = JSON.parse(result.stdout);
assert.equal(decoded.records[0].first_failure.thread_name, null);
assert.equal(decoded.records[0].first_failure.pc_role,
  'validated stacked fault PC');
assert.equal(decoded.records[0].first_failure.fault_registers.dfsr,
  '0x00000002');
assert.equal(decoded.records[0].first_failure.fault_registers.xspi2_cr,
  '0x00000022');

const inconsistentFirstFailure = Buffer.from(faultTimeFirstFailureRecord);
const inconsistentFirstFailureChunk = inconsistentFirstFailure.subarray(
  firstFailureOffset, firstFailureOffset + 41 * 4);
inconsistentFirstFailureChunk.writeUInt32LE(
  ((1 << 31) | (1 << 27) | (1 << 5)) >>> 0, 29 * 4);
inconsistentFirstFailureChunk.writeUInt32LE(
  crc32(inconsistentFirstFailureChunk.subarray(0, 39 * 4)), 39 * 4);
refreshRecordChecksums(inconsistentFirstFailure);
const inconsistentFirstFailureJournal = Buffer.alloc(0x41000, 0xff);
inconsistentFirstFailure.copy(inconsistentFirstFailureJournal, 0);
result = runDecoder(inconsistentFirstFailureJournal);
assert.equal(result.status, 0, result.stderr);
decoded = JSON.parse(result.stdout);
assert.equal(decoded.records[0].first_failure.pc_valid, false);
assert.equal(decoded.records[0].first_failure.pc_role,
  'fault-time PC metadata; no validated stacked frame is available');

const vectorTableFaultRecord = makeRecord();
vectorTableFaultRecord.writeUInt32LE(0x00000002, 256 + 5 * 4);
refreshRecordChecksums(vectorTableFaultRecord);
const vectorTableFaultJournal = Buffer.alloc(0x41000, 0xff);
vectorTableFaultRecord.copy(vectorTableFaultJournal, 0);
result = runDecoder(vectorTableFaultJournal);
assert.equal(result.status, 0, result.stderr);
decoded = JSON.parse(result.stdout);
assert.equal(decoded.records[0].fault.hardfault_debug_event, false);
assert.equal(decoded.records[0].fault.vector_table_bus_fault, true);

const faultTimeContextRecord = makeRecord();
const faultTimeContext = faultTimeContextRecord.subarray(564, 564 + 128);
const faultTimeWords = Array(32).fill(0);
faultTimeWords[0] = 0x43545831;
faultTimeWords[1] = 7;
faultTimeWords[2] = 7100;
faultTimeWords[3] = 0x87654321;
faultTimeWords[4] = 0x70000000;
faultTimeWords[5] = 0x00000002;
faultTimeWords[6] = 0x00000400;
faultTimeWords[7] = 0x00000001;
faultTimeWords[8] = 0x00000040;
faultTimeWords[9] = 0x20000ff1;
faultTimeWords[10] = 0;
faultTimeWords[11] = 0x240043f0;
faultTimeWords[12] = 0x2000ad88;
faultTimeWords[13] = 0xffffffe9;
faultTimeWords[14] = 3;
faultTimeWords[20] = 0x80000000;
faultTimeWords[24] = (1 << 31) | (1 << 5);
faultTimeWords[25] = 1;
faultTimeWords[26] = 2;
faultTimeWords[27] = 3;
faultTimeWords[28] = 4;
faultTimeWords[29] = 5;
putWords(faultTimeContext, 0, faultTimeWords);
faultTimeContext.writeUInt32LE(crc32(faultTimeContext.subarray(0, 120)), 120);
faultTimeContext.writeUInt32LE(0x43545843, 124);
refreshRecordChecksums(faultTimeContextRecord);
const faultTimeContextJournal = Buffer.alloc(0x41000, 0xff);
faultTimeContextRecord.copy(faultTimeContextJournal, 0);
result = runDecoder(faultTimeContextJournal);
assert.equal(result.status, 0, result.stderr);
decoded = JSON.parse(result.stdout);
assert.equal(decoded.records[0].runtime_context.thread_name, null);
assert.equal(decoded.records[0].runtime_context.pc_valid, false);
assert.equal(decoded.records[0].runtime_context.fault_registers.dfsr,
  '0x00000002');
assert.equal(decoded.records[0].runtime_context.fault_registers.vtor,
  '0x70000000');
assert.equal(decoded.records[0].runtime_context.fault_registers.xspi2_sr,
  '0x00000005');
assert.equal(decoded.records[0].runtime_context.ltdc_frames, null);

const legacyV1Journal = Buffer.alloc(0x41000, 0xff);
makeRecord(4, 4, 1).copy(legacyV1Journal, 0);
result = runDecoder(legacyV1Journal);
assert.equal(result.status, 0, result.stderr);
decoded = JSON.parse(result.stdout);
assert.equal(decoded.valid_records, 0);

const retainedRecord = makeRecord();
result = runDecoder(retainedRecord);
assert.equal(result.status, 0, result.stderr);
decoded = JSON.parse(result.stdout);
assert.equal(decoded.schema, 'artpi2-watchdog-retained-v2');
assert.equal(decoded.retained_record_bytes, 0x1000);
assert.equal(decoded.records[0].fault.hardfault_debug_event, true);
assert.equal(decoded.records[0].fault.vector_table_bus_fault, false);

result = runDecoderWithUnmatchedElf(journal);
assert.equal(result.status, 0, result.stderr);
decoded = JSON.parse(result.stdout);
assert.equal(decoded.records[0].symbolization_status,
  'skipped_no_matching_elf');

const stackOverflowJournal = Buffer.alloc(0x41000, 0xff);
makeRecord(8).copy(stackOverflowJournal, 0);
result = runDecoder(stackOverflowJournal);
assert.equal(result.status, 0, result.stderr);
decoded = JSON.parse(result.stdout);
assert.equal(decoded.records[0].fault.kind, 'stack_overflow');

const assertionJournal = Buffer.alloc(0x41000, 0xff);
makeRecord(5).copy(assertionJournal, 0);
result = runDecoder(assertionJournal);
assert.equal(result.status, 0, result.stderr);
decoded = JSON.parse(result.stdout);
assert.equal(decoded.records[0].fault.kind, 'assert');
assert.equal(decoded.records[0].fault.pc, null);
assert.equal(decoded.records[0].fault.frame_valid, null);
assert.equal(decoded.records[0].fault.panic_stack.word_count, 12);
assert.deepEqual(decoded.records[0].fault.panic_stack.words.slice(8), [
  '0x00000020', '0x00000021', '0x00000022', '0x00000023',
]);

const syncFaultJournal = Buffer.alloc(0x41000, 0xff);
makeRecord(4, 6).copy(syncFaultJournal, 0);
result = runDecoder(syncFaultJournal);
assert.equal(result.status, 0, result.stderr);
decoded = JSON.parse(result.stdout);
assert.equal(decoded.records[0].first_failure.reason, 'synchronous_fault');
assert.match(decoded.records[0].first_failure.pc_role, /pre-fault sample/);
assert.equal(decoded.records[0].fault.pc, '0x70001001');

const feedCheckpointRecord = makeRecord();
const feedCheckpointCodes = [15, 16, 17, 18];
feedCheckpointCodes.forEach((code, index) => {
  const event = Buffer.alloc(32);
  const args = code === 15 ? [4, 0x17] :
    (code === 16 ? [42, 0x17] : [42, code === 17 ? 41 : 0x17]);
  putWords(event, 0, [3 + index, 8000 + index * 100, code,
    args[0], args[1], 0x70005679, 0x24003000]);
  event.writeUInt32LE(crc32(event.subarray(0, 28)), 28);
  event.copy(feedCheckpointRecord, 768 + (2 + index) * 32);
});
refreshRecordChecksums(feedCheckpointRecord);
const feedCheckpointJournal = Buffer.alloc(0x41000, 0xff);
feedCheckpointRecord.copy(feedCheckpointJournal, 0);
result = runDecoder(feedCheckpointJournal);
assert.equal(result.status, 0, result.stderr);
decoded = JSON.parse(result.stdout);
assert.equal(decoded.records[0].events[2].event, 'health_rejected');
assert.equal(decoded.records[0].events[2].reject_reason, 'core_progress_stale');
assert.equal(decoded.records[0].events[2].health_flags, '0x00000017');
assert.equal(decoded.records[0].events[3].event, 'feed_begin');
assert.equal(decoded.records[0].events[3].feed_attempt_sequence, 42);
assert.equal(decoded.records[0].events[4].event, 'feed_returned');
assert.equal(decoded.records[0].events[4].feed_return_sequence, 41);
assert.match(decoded.records[0].events[4].pc_role, /not confirmed/);
assert.equal(decoded.records[0].events[5].event, 'feed_suppressed');

const invalidExceptionFrameJournal = Buffer.alloc(0x41000, 0xff);
const invalidExceptionFrameRecord = makeRecord();
invalidExceptionFrameRecord.writeUInt32LE(0x90c03b60, 256 + 30 * 4);
refreshRecordChecksums(invalidExceptionFrameRecord);
invalidExceptionFrameRecord.copy(invalidExceptionFrameJournal, 0);
result = runDecoder(invalidExceptionFrameJournal);
assert.equal(result.status, 0, result.stderr);
decoded = JSON.parse(result.stdout);
assert.equal(decoded.valid_records, 1);
assert.equal(decoded.records[0].fault.kind, 'hardfault');
assert.equal(decoded.records[0].fault.frame_valid, false);
assert.equal(decoded.records[0].fault.frame_invalid_reason,
  'xpsr_thumb_state_clear');
assert.equal(decoded.records[0].fault.pc_valid, false);
assert.equal(decoded.records[0].fault.pc, null);
assert.equal(decoded.records[0].fault.raw_pc, '0x70001001');

const badFirstFailure = Buffer.from(journal);
badFirstFailure[3840 + (7 + 15) * 4 + 5 * 4] ^= 1;
const badFailureRecord = badFirstFailure.subarray(0, 0x1000);
const badFailureCrcData = Buffer.from(badFailureRecord);
badFailureCrcData.fill(0, 3860, 3864);
badFailureCrcData.fill(0, 4092, 4096);
badFailureRecord.writeUInt32LE(crc32(badFailureCrcData), 3860);
result = runDecoder(badFirstFailure);
assert.equal(result.status, 0, result.stderr);
decoded = JSON.parse(result.stdout);
assert.equal(decoded.records[0].first_failure, null);

const badPanicStack = Buffer.from(assertionJournal);
badPanicStack[256 + 128 + 5 * 4] ^= 1;
const badPanicRecord = badPanicStack.subarray(0, 0x1000);
const badPanicCrcData = Buffer.from(badPanicRecord);
badPanicCrcData.fill(0, 3860, 3864);
badPanicCrcData.fill(0, 4092, 4096);
badPanicRecord.writeUInt32LE(crc32(badPanicCrcData), 3860);
result = runDecoder(badPanicStack);
assert.equal(result.status, 0, result.stderr);
decoded = JSON.parse(result.stdout);
assert.equal(decoded.records[0].fault.panic_stack, null);

const badCommit = Buffer.from(journal);
badCommit.writeUInt32LE(0xffffffff, 4092);
result = runDecoder(badCommit);
assert.equal(result.status, 0, result.stderr);
decoded = JSON.parse(result.stdout);
assert.equal(decoded.valid_records, 0);

const badEvent = Buffer.from(journal);
badEvent[768 + 16] ^= 1;
const badRecord = badEvent.subarray(0, 0x1000);
const crcData = Buffer.from(badRecord);
crcData.fill(0, 3860, 3864);
crcData.fill(0, 4092, 4096);
badRecord.writeUInt32LE(crc32(crcData), 3860);
result = runDecoder(badEvent);
assert.equal(result.status, 0, result.stderr);
decoded = JSON.parse(result.stdout);
assert.equal(decoded.valid_records, 1);
assert.deepEqual(decoded.records[0].events.map((event) => event.event), ['health_stale']);

const badContext = Buffer.from(journal);
badContext[564 + 36] ^= 1;
const badContextRecord = badContext.subarray(0, 0x1000);
const badContextCrcData = Buffer.from(badContextRecord);
badContextCrcData.fill(0, 3860, 3864);
badContextCrcData.fill(0, 4092, 4096);
badContextRecord.writeUInt32LE(crc32(badContextCrcData), 3860);
result = runDecoder(badContext);
assert.equal(result.status, 0, result.stderr);
decoded = JSON.parse(result.stdout);
assert.equal(decoded.records[0].runtime_context, null);
assert.equal(decoded.records[0].components[0].component, 'mongoose');
assert.equal(decoded.records[0].components.length, 1);

const badFrame = Buffer.from(journal);
badFrame[692 + 4] ^= 1;
const badFrameRecord = badFrame.subarray(0, 0x1000);
const badFrameCrcData = Buffer.from(badFrameRecord);
badFrameCrcData.fill(0, 3860, 3864);
badFrameCrcData.fill(0, 4092, 4096);
badFrameRecord.writeUInt32LE(crc32(badFrameCrcData), 3860);
result = runDecoder(badFrame);
assert.equal(result.status, 0, result.stderr);
decoded = JSON.parse(result.stdout);
assert.equal(decoded.records[0].runtime_context.pc_valid, true);
assert.equal(decoded.records[0].interrupted_frame, null);

console.log('watchdog log decoder: PASS');
