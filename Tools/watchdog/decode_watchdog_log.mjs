#!/usr/bin/env node
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import process from 'node:process';

const RECORD_SIZE = 0x1000;
const JOURNAL_SIZE = 0x41000;
const SLOT_COUNT = JOURNAL_SIZE / RECORD_SIZE;
const MAGIC = 0x57444752;
const RECORD_VERSION = 2;
const FLASH_COMMIT = 0x464c5331;
const HEADER_COMMIT = 0x48445231;
const FAULT_COMMIT = 0x464c5431;
const EVENT_COUNT = 96;
const HEADER0 = 0;
const HEADER1 = 128;
const FAULT_OFFSET = 256;
const EVENT_OFFSET = 768;
const FOOTER_OFFSET = 3840;
const COMPONENT_WORDS = 9;
const COMPONENT_COUNT = 5;
const CONTEXT_WORDS = 32;
const FRAME_WORDS = 9;
const FRAME_COPIES = 2;
const COMPONENT_COMMIT_BASE = 0x434d0000;
const CONTEXT_MAGIC = 0x43545831;
const CONTEXT_COMMIT = 0x43545843;
const FRAME_COMMIT_BASE = 0x46520000;
const FIRST_FAILURE_BASE_WORD = 15;
const FIRST_FAILURE_WORDS = 41;
const FIRST_FAILURE_MAGIC = 0x46465331;
const FIRST_FAILURE_COMMIT = 0x46464331;
const PANIC_STACK_MAGIC = 0x504e5331;
const PANIC_STACK_COMMIT = 0x504e4331;
const FOOTER_RESERVED_OFFSET = FOOTER_OFFSET + 7 * 4;

const eventNames = new Map([
  [1, 'boot'], [2, 'reset_cause'], [3, 'core_ready'],
  [4, 'core_progress'], [5, 'operation_begin'],
  [6, 'operation_progress'], [7, 'operation_end'],
  [8, 'health_stale'], [9, 'fault'], [10, 'flash_commit'],
  [11, 'flash_failure'], [12, 'recovery'], [13, 'boot_failure'],
  [14, 'context_sample'], [15, 'health_rejected'], [16, 'feed_begin'],
  [17, 'feed_returned'], [18, 'feed_suppressed'],
]);
const componentNames = [
  'lvgl', 'mongoose', 'wifi_worker', 'usb_analyzer', 'pdf_worker',
];
const componentStages = new Map([
  [1, 'starting'], [2, 'active'], [3, 'idle'], [4, 'error'], [5, 'stopped'],
]);
const threadStates = new Map([
  [0, 'init'], [1, 'closed'], [2, 'ready'], [3, 'running'], [4, 'suspended'],
]);
const faultNames = new Map([
  [1, 'memmanage'], [2, 'busfault'], [3, 'usagefault'],
  [4, 'hardfault'], [5, 'assert'], [6, 'error_handler'],
  [7, 'boot_error'], [8, 'stack_overflow'], [9, 'nmi'],
  [10, 'unexpected'],
]);
const failureNames = new Map([
  [1, 'operation_timeout'], [2, 'initialization_timeout'],
  [3, 'core_progress_stale'], [4, 'required_component_stale'],
  [5, 'display_frames_stale'], [6, 'synchronous_fault'],
  [7, 'assertion'],
]);
const feedRejectReasons = new Map([
  [0, 'none'], [1, 'header_invalid'], [2, 'operation_timeout'],
  [3, 'initialization_timeout'], [4, 'core_progress_stale'],
  [5, 'required_component_stale'], [6, 'display_frames_stale'],
  [7, 'fatal_fault'], [8, 'other'],
]);

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

function u32(buffer, offset) {
  return buffer.readUInt32LE(offset);
}

function validHeader(record, offset) {
  const chunk = record.subarray(offset, offset + 128);
  if (chunk.length !== 128 || u32(chunk, 0) !== MAGIC ||
      u32(chunk, 4) !== RECORD_VERSION || u32(chunk, 124) !== HEADER_COMMIT ||
      crc32(chunk.subarray(0, 120)) !== u32(chunk, 120)) return null;
  const fields = Array.from({ length: 24 }, (_, i) => u32(chunk, i * 4));
  return {
    sequence: fields[2],
    boot_sequence: fields[3],
    reset_flags: fields[4],
    reset_count: fields[5],
    reset_window_second: fields[6],
    reset_window_valid: fields[7] !== 0,
    event_next_sequence: fields[8],
    event_count: fields[9],
    core_ready: fields[10] !== 0,
    last_core_tick: fields[11],
    last_core_progress: fields[12],
    last_core_pc: fields[13],
    last_core_sp: fields[14],
    last_core_thread: fields[15],
    active_operation: fields[16],
    active_stage: fields[17],
    active_started_tick: fields[18],
    active_timeout_ms: fields[19],
    active_progress: fields[20],
    supervisor_tick: fields[21],
    health_flags: fields[22],
    source_fingerprint_crc32: `0x${fields[23].toString(16).padStart(8, '0').toUpperCase()}`,
    source_fingerprint_prefix: chunk.subarray(96, 120).toString('ascii').trim(),
  };
}

function validPanicStack(record, firstEight, kindCode) {
  if (kindCode !== 5) return null;
  const offset = FAULT_OFFSET + 128;
  const chunk = record.subarray(offset, offset + 95 * 4);
  if (chunk.length !== 95 * 4) return null;
  const fields = Array.from({ length: 95 }, (_, i) => u32(chunk, i * 4));
  if (fields[0] !== PANIC_STACK_MAGIC || fields[1] !== 1 ||
      fields[94] !== PANIC_STACK_COMMIT || fields[3] > 96) return null;
  const checksumData = Buffer.from(chunk.subarray(0, 94 * 4));
  checksumData.fill(0, 4 * 4, 5 * 4);
  if (crc32(checksumData) !== fields[4]) return null;

  const total = fields[3];
  const stack = firstEight.slice(0, Math.min(total, 8));
  const tailCount = Math.max(0, total - 8);
  for (let i = 0; i < tailCount; i += 1) {
    stack.push(`0x${fields[5 + i].toString(16).padStart(8, '0').toUpperCase()}`);
  }
  return {
    captured_sp: `0x${fields[2].toString(16).padStart(8, '0').toUpperCase()}`,
    word_count: total,
    words: stack,
  };
}

function validFault(record) {
  const chunk = record.subarray(FAULT_OFFSET, FAULT_OFFSET + 512);
  if (chunk.length !== 512 || u32(chunk, 508) !== FAULT_COMMIT ||
      crc32(chunk.subarray(0, 124)) !== u32(chunk, 124) ||
      u32(chunk, 0) !== MAGIC || u32(chunk, 4) !== RECORD_VERSION) return null;
  const fields = Array.from({ length: 31 }, (_, i) => u32(chunk, i * 4));
  const frame = fields.slice(23, 31).map((v) => `0x${v.toString(16).padStart(8, '0').toUpperCase()}`);
  const isAssertion = fields[2] === 5;
  const excReturnValid = ((fields[3] & 0xFFFFFFE0) >>> 0) === 0xFFFFFFE0;
  const thumbStateValid = (fields[30] & (1 << 24)) !== 0;
  const stackedPcValid = fields[29] !== 0;
  const exceptionFrameValid = !isAssertion && excReturnValid &&
    thumbStateValid && stackedPcValid;
  let frameInvalidReason = null;
  if (isAssertion) frameInvalidReason = 'assertion_stack_prefix';
  else if (!excReturnValid) frameInvalidReason = 'invalid_exc_return';
  else if (!thumbStateValid) frameInvalidReason = 'xpsr_thumb_state_clear';
  else if (!stackedPcValid) frameInvalidReason = 'zero_stacked_pc';
  const cfsr = fields[4];
  const hfsr = fields[5];
  const rawPc = isAssertion ? null : frame[6];
  const rawLr = isAssertion ? null : frame[5];
  return {
    kind_code: fields[2],
    kind: faultNames.get(fields[2]) ?? `unknown_${fields[2]}`,
    exc_return: `0x${fields[3].toString(16).padStart(8, '0').toUpperCase()}`,
    cfsr: `0x${cfsr.toString(16).padStart(8, '0').toUpperCase()}`,
    hfsr: `0x${hfsr.toString(16).padStart(8, '0').toUpperCase()}`,
    mmfar_valid: (cfsr & (1 << 7)) !== 0,
    bfar_valid: (cfsr & (1 << 15)) !== 0,
    bus_fault_precision: (cfsr & (1 << 9)) !== 0 ? 'precise' :
      ((cfsr & (1 << 10)) !== 0 ? 'imprecise' : 'not_reported'),
    hardfault_debug_event: (hfsr & 0x80000000) !== 0,
    hardfault_forced: (hfsr & (1 << 30)) !== 0,
    vector_table_bus_fault: (hfsr & 0x00000002) !== 0,
    mmfar: `0x${fields[6].toString(16).padStart(8, '0').toUpperCase()}`,
    bfar: `0x${fields[7].toString(16).padStart(8, '0').toUpperCase()}`,
    frame_address: `0x${fields[8].toString(16).padStart(8, '0').toUpperCase()}`,
    basic_frame_address: `0x${fields[9].toString(16).padStart(8, '0').toUpperCase()}`,
    psp: `0x${fields[10].toString(16).padStart(8, '0').toUpperCase()}`,
    msp: `0x${fields[11].toString(16).padStart(8, '0').toUpperCase()}`,
    thread_id: `0x${fields[12].toString(16).padStart(8, '0').toUpperCase()}`,
    tick: fields[13],
    cycles: fields[14],
    primask: fields[15],
    basepri: fields[16],
    control: fields[17],
    file_address: `0x${fields[18].toString(16).padStart(8, '0').toUpperCase()}`,
    function_address: `0x${fields[19].toString(16).padStart(8, '0').toUpperCase()}`,
    expression_address: `0x${fields[20].toString(16).padStart(8, '0').toUpperCase()}`,
    caller_address: `0x${fields[21].toString(16).padStart(8, '0').toUpperCase()}`,
    caller_address_role: 'fault-capture helper call site; not the faulting instruction',
    line: fields[22],
    frame,
    frame_role: isAssertion ? 'assertion_stack_prefix' : 'cortex_m_exception_frame',
    frame_valid: isAssertion ? null : exceptionFrameValid,
    frame_invalid_reason: frameInvalidReason,
    pc_valid: exceptionFrameValid,
    pc_role: isAssertion ? 'assertion call site metadata' : exceptionFrameValid
      ? 'validated stacked exception PC; an imprecise bus fault may point after the original access'
      : 'unvalidated stacked PC bytes; raw frame is retained but location must not be attributed',
    raw_pc: rawPc,
    raw_lr: rawLr,
    pc: exceptionFrameValid ? rawPc : null,
    lr: exceptionFrameValid ? rawLr : null,
    sp: `0x${fields[9].toString(16).padStart(8, '0').toUpperCase()}`,
    panic_stack: validPanicStack(record, frame, fields[2]),
  };
}

function validFirstFailure(record) {
  const offset = FOOTER_RESERVED_OFFSET + FIRST_FAILURE_BASE_WORD * 4;
  const chunk = record.subarray(offset,
                                offset + FIRST_FAILURE_WORDS * 4);
  if (chunk.length !== FIRST_FAILURE_WORDS * 4) return null;
  const fields = Array.from({ length: FIRST_FAILURE_WORDS },
    (_, i) => u32(chunk, i * 4));
  if (fields[0] !== FIRST_FAILURE_MAGIC ||
      fields[FIRST_FAILURE_WORDS - 1] !== FIRST_FAILURE_COMMIT ||
      crc32(chunk.subarray(0, 39 * 4)) !== fields[39]) return null;
  const nameBytes = chunk.subarray(10 * 4, 14 * 4);
  const contextFlags = fields[29];
  const faultRegistersValid =
    (contextFlags >>> 31) !== 0;
  const frameValid = (contextFlags & (1 << 27)) !== 0;
  const faultPcValid = (contextFlags & (1 << 24)) !== 0;
  const reason = failureNames.get(fields[3]) ?? `unknown_${fields[3]}`;
  let subjectName = null;
  if (fields[3] === 4) subjectName = componentNames[fields[4]] ?? 'unknown_component';
  else if (fields[3] === 5) subjectName = 'ltdc_frame_counter';
  else if (fields[3] === 6 || fields[3] === 7) {
    subjectName = faultNames.get(fields[4]) ?? `fault_${fields[4]}`;
  }
  else if (fields[3] === 1) subjectName = 'operation_id';
  else if (fields[3] === 3) subjectName = 'core_progress_id';
  return {
    sequence: fields[1],
    tick: fields[2],
    reason_code: fields[3],
    reason,
    subject: fields[4],
    subject_name: subjectName,
    pc_valid: faultPcValid,
    pc_role: faultRegistersValid && frameValid && faultPcValid
      ? 'validated stacked fault PC'
      : faultRegistersValid
        ? 'fault-time PC metadata; no validated stacked frame is available'
        : ((fields[3] === 6 || fields[3] === 7)
          ? 'pre-fault sample; use the fault frame and source metadata for the fault location'
          : 'CPU context at first health-failure detection; correlate with the failing component checkpoint'),
    age_ms: fields[5],
    context_sequence: fields[6],
    context_tick: fields[7],
    context_age_ticks: (fields[2] - fields[7]) >>> 0,
    context_cycles: fields[8],
    thread: faultRegistersValid ? null :
      `0x${fields[9].toString(16).padStart(8, '0').toUpperCase()}`,
    thread_name: faultRegistersValid ? null :
      nameBytes.toString('ascii').replace(/\0.*$/, '').trim(),
    pc: `0x${fields[14].toString(16).padStart(8, '0').toUpperCase()}`,
    lr: `0x${fields[15].toString(16).padStart(8, '0').toUpperCase()}`,
    psp: `0x${fields[16].toString(16).padStart(8, '0').toUpperCase()}`,
    msp: `0x${fields[17].toString(16).padStart(8, '0').toUpperCase()}`,
    exc_return: `0x${fields[18].toString(16).padStart(8, '0').toUpperCase()}`,
    ipsr: fields[19],
    primask: fields[20],
    basepri: fields[21],
    faultmask: fields[22],
    control: fields[23],
    cfsr: `0x${fields[24].toString(16).padStart(8, '0').toUpperCase()}`,
    hfsr: `0x${fields[25].toString(16).padStart(8, '0').toUpperCase()}`,
    mmfar: `0x${fields[26].toString(16).padStart(8, '0').toUpperCase()}`,
    bfar: `0x${fields[27].toString(16).padStart(8, '0').toUpperCase()}`,
    shcsr: `0x${fields[28].toString(16).padStart(8, '0').toUpperCase()}`,
    health_flags: `0x${contextFlags.toString(16).padStart(8, '0').toUpperCase()}`,
    frame_valid: frameValid,
    mmfar_valid: (contextFlags & (1 << 29)) !== 0,
    bfar_valid: (contextFlags & (1 << 30)) !== 0,
    hardware_frame: frameValid ? {
      r0: `0x${fields[33].toString(16).padStart(8, '0').toUpperCase()}`,
      r1: `0x${fields[34].toString(16).padStart(8, '0').toUpperCase()}`,
      r2: `0x${fields[35].toString(16).padStart(8, '0').toUpperCase()}`,
      r3: `0x${fields[36].toString(16).padStart(8, '0').toUpperCase()}`,
      r12: `0x${fields[37].toString(16).padStart(8, '0').toUpperCase()}`,
      xpsr: `0x${fields[38].toString(16).padStart(8, '0').toUpperCase()}`,
    } : null,
    peripherals: faultRegistersValid ? null : {
      ltdc_frames: fields[30],
      gpu2d_errors: fields[31],
      dma2d_errors: fields[32],
    },
    fault_registers: faultRegistersValid ? {
      vtor: `0x${fields[9].toString(16).padStart(8, '0').toUpperCase()}`,
      dfsr: `0x${fields[10].toString(16).padStart(8, '0').toUpperCase()}`,
      demcr: `0x${fields[11].toString(16).padStart(8, '0').toUpperCase()}`,
      dhcsr: `0x${fields[12].toString(16).padStart(8, '0').toUpperCase()}`,
      icsr: `0x${fields[13].toString(16).padStart(8, '0').toUpperCase()}`,
      icache_cr: `0x${fields[30].toString(16).padStart(8, '0').toUpperCase()}`,
      xspi2_cr: `0x${fields[31].toString(16).padStart(8, '0').toUpperCase()}`,
      xspi2_dcr2: `0x${fields[32].toString(16).padStart(8, '0').toUpperCase()}`,
    } : null,
  };
}

function validRuntimeSidecar(record) {
  const components = [];
  const reservedOffset = FAULT_OFFSET + 128;
  for (let id = 0; id < COMPONENT_COUNT; id += 1) {
    const offset = reservedOffset + id * COMPONENT_WORDS * 4;
    const chunk = record.subarray(offset, offset + COMPONENT_WORDS * 4);
    if (chunk.length !== COMPONENT_WORDS * 4) continue;
    const fields = Array.from({ length: COMPONENT_WORDS }, (_, i) => u32(chunk, i * 4));
    if (fields[0] === 0 ||
        fields[8] !== ((COMPONENT_COMMIT_BASE | (id + 1)) >>> 0) ||
        crc32(chunk.subarray(0, 28)) !== fields[7]) continue;
    components.push({
      id,
      component: componentNames[id],
      sequence: fields[0],
      tick: fields[1],
      thread: `0x${fields[2].toString(16).padStart(8, '0').toUpperCase()}`,
      stage: componentStages.get(fields[3] & 0xff) ?? `unknown_${fields[3] & 0xff}`,
      stage_code: fields[3] & 0xff,
      thread_status: fields[3] >>> 8 & 0xff,
      thread_state: threadStates.get((fields[3] >>> 8) & 0x07) ?? 'unknown',
      current_priority: fields[3] >>> 16 & 0xff,
      initial_priority: fields[3] >>> 24 & 0xff,
      progress: fields[4],
      heartbeat_callsite_pc: `0x${fields[5].toString(16).padStart(8, '0').toUpperCase()}`,
      heartbeat_callsite_sp: `0x${fields[6].toString(16).padStart(8, '0').toUpperCase()}`,
      pc_role: 'component heartbeat caller; not a fault PC',
    });
  }

  const contextOffset = reservedOffset + COMPONENT_COUNT * COMPONENT_WORDS * 4;
  const chunk = record.subarray(contextOffset,
                                contextOffset + CONTEXT_WORDS * 4);
  if (chunk.length !== CONTEXT_WORDS * 4) {
    return { components, context: null };
  }
  const fields = Array.from({ length: CONTEXT_WORDS }, (_, i) => u32(chunk, i * 4));
  let context = null;
  if (fields[0] === CONTEXT_MAGIC && fields[31] === CONTEXT_COMMIT &&
      crc32(chunk.subarray(0, 120)) === fields[30]) {
    const nameBytes = chunk.subarray(20, 36);
    const faultRegistersValid = (fields[24] >>> 31) !== 0;
    const pcValid = (fields[24] & (1 << 24)) !== 0;
    const frameValid = (fields[24] & (1 << 27)) !== 0;
    context = {
      sequence: fields[1],
      tick: fields[2],
      cycles: fields[3],
      thread: `0x${fields[4].toString(16).padStart(8, '0').toUpperCase()}`,
      thread_name: faultRegistersValid ? null :
        nameBytes.toString('ascii').replace(/\0.*$/, '').trim(),
      pc: `0x${fields[9].toString(16).padStart(8, '0').toUpperCase()}`,
      lr: `0x${fields[10].toString(16).padStart(8, '0').toUpperCase()}`,
      psp: `0x${fields[11].toString(16).padStart(8, '0').toUpperCase()}`,
      msp: `0x${fields[12].toString(16).padStart(8, '0').toUpperCase()}`,
      exc_return: `0x${fields[13].toString(16).padStart(8, '0').toUpperCase()}`,
      ipsr: fields[14],
      primask: fields[15],
      basepri: fields[16],
      faultmask: fields[17],
      control: fields[18],
      cfsr: `0x${fields[19].toString(16).padStart(8, '0').toUpperCase()}`,
      hfsr: `0x${fields[20].toString(16).padStart(8, '0').toUpperCase()}`,
      mmfar: `0x${fields[21].toString(16).padStart(8, '0').toUpperCase()}`,
      bfar: `0x${fields[22].toString(16).padStart(8, '0').toUpperCase()}`,
      shcsr: `0x${fields[23].toString(16).padStart(8, '0').toUpperCase()}`,
      health_flags: `0x${fields[24].toString(16).padStart(8, '0').toUpperCase()}`,
      pc_valid: pcValid,
      pc_role: faultRegistersValid
        ? (pcValid && frameValid ? 'validated stacked fault PC'
          : 'fault-time PC metadata; stacked frame not validated')
        : 'periodic thread-context sample; not a fault PC',
      mmfar_valid: (fields[19] & (1 << 7)) !== 0,
      bfar_valid: (fields[19] & (1 << 15)) !== 0,
      frame_valid: frameValid,
      fault_status_note: faultRegistersValid
        ? 'fault-time register snapshot committed independently from first_failure'
        : 'sampled sticky status; use a synchronous fault record for attribution',
      thread_status: faultRegistersValid ? null : fields[29] & 0xff,
      thread_state: faultRegistersValid ? null :
        (threadStates.get(fields[29] & 0x07) ?? 'unknown'),
      current_priority: faultRegistersValid ? null : fields[29] >>> 8 & 0xff,
      initial_priority: faultRegistersValid ? null : fields[29] >>> 16 & 0xff,
      ltdc_frames: faultRegistersValid ? null : fields[25],
      ltdc_reloads: faultRegistersValid ? null : fields[26],
      gpu2d_errors: faultRegistersValid ? null : fields[27],
      dma2d_errors: faultRegistersValid ? null : fields[28],
      fault_registers: faultRegistersValid ? {
        vtor: `0x${fields[4].toString(16).padStart(8, '0').toUpperCase()}`,
        dfsr: `0x${fields[5].toString(16).padStart(8, '0').toUpperCase()}`,
        demcr: `0x${fields[6].toString(16).padStart(8, '0').toUpperCase()}`,
        dhcsr: `0x${fields[7].toString(16).padStart(8, '0').toUpperCase()}`,
        icsr: `0x${fields[8].toString(16).padStart(8, '0').toUpperCase()}`,
        icache_cr: `0x${fields[25].toString(16).padStart(8, '0').toUpperCase()}`,
        icache_sr: `0x${fields[26].toString(16).padStart(8, '0').toUpperCase()}`,
        xspi2_cr: `0x${fields[27].toString(16).padStart(8, '0').toUpperCase()}`,
        xspi2_dcr2: `0x${fields[28].toString(16).padStart(8, '0').toUpperCase()}`,
        xspi2_sr: `0x${fields[29].toString(16).padStart(8, '0').toUpperCase()}`,
      } : null,
    };
  }
  let interruptedFrame = null;
  const frameOffset = contextOffset + CONTEXT_WORDS * 4;
  for (let slot = 0; slot < FRAME_COPIES; slot += 1) {
    const offset = frameOffset + slot * FRAME_WORDS * 4;
    const frameChunk = record.subarray(offset, offset + FRAME_WORDS * 4);
    if (frameChunk.length !== FRAME_WORDS * 4) continue;
    const frameFields = Array.from({ length: FRAME_WORDS }, (_, i) => u32(frameChunk, i * 4));
    if (frameFields[0] === 0 ||
        frameFields[8] !== ((FRAME_COMMIT_BASE | (slot + 1)) >>> 0) ||
        crc32(frameChunk.subarray(0, 28)) !== frameFields[7]) continue;
    if (interruptedFrame === null ||
        ((frameFields[0] - interruptedFrame.sequence) | 0) > 0) {
      interruptedFrame = {
        sequence: frameFields[0],
        r0: `0x${frameFields[1].toString(16).padStart(8, '0').toUpperCase()}`,
        r1: `0x${frameFields[2].toString(16).padStart(8, '0').toUpperCase()}`,
        r2: `0x${frameFields[3].toString(16).padStart(8, '0').toUpperCase()}`,
        r3: `0x${frameFields[4].toString(16).padStart(8, '0').toUpperCase()}`,
        r12: `0x${frameFields[5].toString(16).padStart(8, '0').toUpperCase()}`,
        xpsr: `0x${frameFields[6].toString(16).padStart(8, '0').toUpperCase()}`,
      };
    }
  }
  if (interruptedFrame && context) {
    interruptedFrame.matches_runtime_context =
      interruptedFrame.sequence === context.sequence;
  }
  return { components, context, interrupted_frame: interruptedFrame };
}

function validEvents(record) {
  const events = [];
  for (let index = 0; index < EVENT_COUNT; index += 1) {
    const offset = EVENT_OFFSET + index * 32;
    const chunk = record.subarray(offset, offset + 32);
    const fields = Array.from({ length: 8 }, (_, i) => u32(chunk, i * 4));
    if (fields[0] === 0 || fields[2] === 0 || crc32(chunk.subarray(0, 28)) !== fields[7]) continue;
    const event = {
      sequence: fields[0],
      tick: fields[1],
      event: eventNames.get(fields[2]) ?? `unknown_${fields[2]}`,
      arg0: fields[3],
      arg1: fields[4],
      pc: `0x${fields[5].toString(16).padStart(8, '0').toUpperCase()}`,
      sp: `0x${fields[6].toString(16).padStart(8, '0').toUpperCase()}`,
    };
    if (fields[2] === 8) {
      if ((fields[3] & 0xffff0000) === 0x434d0000) {
        event.stale_component = componentNames[(fields[3] & 0xffff) - 1] ?? 'unknown';
        event.stale_age_ms = fields[4];
        event.pc_role = 'last component checkpoint API location';
      } else if (fields[3] === 0x4c544443) {
        event.stale_component = 'ltdc';
        event.stale_age_ticks = fields[4];
      } else if (fields[3] === 0x54455354) {
        event.diagnostic_subject = 'controlled_debug_test';
        event.test_id = fields[4];
      }
    }
    if (fields[2] === 15) {
      event.reject_reason_code = fields[3];
      event.reject_reason = feedRejectReasons.get(fields[3]) ??
        `unknown_${fields[3]}`;
      event.health_flags = `0x${fields[4].toString(16).padStart(8, '0').toUpperCase()}`;
      event.pc_role = 'supervisor health-check caller; not a fault PC';
    } else if (fields[2] === 16) {
      event.feed_attempt_sequence = fields[3];
      event.health_flags = `0x${fields[4].toString(16).padStart(8, '0').toUpperCase()}`;
      event.pc_role = 'IWDG refresh callsite before refresh; not a fault PC';
    } else if (fields[2] === 17 || fields[2] === 18) {
      event.feed_attempt_sequence = fields[3];
      if (fields[2] === 17) event.feed_return_sequence = fields[4];
      else event.health_flags = `0x${fields[4].toString(16).padStart(8, '0').toUpperCase()}`;
      event.pc_role = fields[2] === 17
        ? 'IWDG refresh returned in software; hardware acceptance is not confirmed'
        : 'IWDG refresh intentionally suppressed by diagnostic test';
    }
    if (fields[2] === 11 && fields[3] === 0x58535049) {
      event.diagnostic_subject = 'xspi_write_enable_state';
      event.hal_state = fields[4] & 0xffff;
      event.hal_error = fields[4] >>> 16;
      event.xspi_sr = `0x${fields[5].toString(16).padStart(8, '0').toUpperCase()}`;
      event.xspi_cr = `0x${fields[6].toString(16).padStart(8, '0').toUpperCase()}`;
    }
    events.push(event);
  }
  events.sort((a, b) => a.sequence - b.sequence);
  return events;
}

function validFlashRecord(record) {
  if (record.length !== RECORD_SIZE) return null;
  const footer = record.subarray(FOOTER_OFFSET);
  const magic = u32(footer, 0);
  const version = u32(footer, 4);
  const stageSequence = u32(footer, 8);
  const stageCommit = u32(footer, 12);
  const flashSequence = u32(footer, 16);
  const storedCrc = u32(footer, 20);
  if (magic !== MAGIC || version !== RECORD_VERSION || flashSequence === 0 ||
      u32(record, 4092) !== FLASH_COMMIT) return null;
  const crcData = Buffer.from(record);
  crcData.fill(0, FOOTER_OFFSET + 20, FOOTER_OFFSET + 24);
  crcData.fill(0, 4092, 4096);
  if (crc32(crcData) !== storedCrc) return null;
  const headers = [validHeader(record, HEADER0), validHeader(record, HEADER1)]
    .filter((entry) => entry !== null);
  if (headers.length === 0) return null;
  const header = headers.reduce((best, current) =>
    current.sequence > best.sequence ? current : best);
  const runtimeSidecar = validRuntimeSidecar(record);
  const firstFailure = validFirstFailure(record);
  const incidentLow = u32(record, FOOTER_RESERVED_OFFSET + 12 * 4);
  const incidentHigh = u32(record, FOOTER_RESERVED_OFFSET + 13 * 4);
  const incidentCrc = u32(record, FOOTER_RESERVED_OFFSET + 14 * 4);
  const incidentMarker = u32(record, FOOTER_RESERVED_OFFSET + 11 * 4);
  const incidentWords = Buffer.alloc(12);
  incidentWords.writeUInt32LE(0x49494431, 0);
  incidentWords.writeUInt32LE(incidentLow, 4);
  incidentWords.writeUInt32LE(incidentHigh, 8);
  const incidentId = incidentMarker === 0x49494431 &&
    incidentLow + incidentHigh !== 0 && crc32(incidentWords) === incidentCrc
    ? ((BigInt(incidentHigh) << 32n) | BigInt(incidentLow)).toString()
    : null;
  const isIwdgReset = (header.reset_flags & 0x04000000) !== 0;
  return {
    incident_id: incidentId,
    stage_sequence: stageSequence,
    stage_commit: `0x${stageCommit.toString(16).padStart(8, '0').toUpperCase()}`,
    flash_sequence: flashSequence,
    reset_flags: `0x${header.reset_flags.toString(16).padStart(8, '0').toUpperCase()}`,
    reset_source: isIwdgReset ? 'IWDG' : 'other_or_non_watchdog',
    reset_without_exception_frame: isIwdgReset && validFault(record) === null,
    reset_count: header.reset_count,
    source_fingerprint_crc32: header.source_fingerprint_crc32,
    source_fingerprint_prefix: header.source_fingerprint_prefix,
    header,
    fault: validFault(record),
    events: validEvents(record),
    runtime_context: runtimeSidecar.context,
    components: runtimeSidecar.components,
    interrupted_frame: runtimeSidecar.interrupted_frame,
    first_failure: firstFailure,
  };
}

function validRetainedRecord(record) {
  if (record.length !== RECORD_SIZE) return null;
  const headers = [validHeader(record, HEADER0), validHeader(record, HEADER1)]
    .filter((entry) => entry !== null);
  if (headers.length === 0) return null;
  const header = headers.reduce((best, current) =>
    current.sequence > best.sequence ? current : best);
  const runtimeSidecar = validRuntimeSidecar(record);
  const firstFailure = validFirstFailure(record);
  const reserved = FOOTER_RESERVED_OFFSET;
  const pendingFlash = u32(record, reserved);
  const faultResetRequested = u32(record, reserved + 4);
  const incidentLow = u32(record, reserved + 12 * 4);
  const incidentHigh = u32(record, reserved + 13 * 4);
  const incidentCrc = u32(record, reserved + 14 * 4);
  const incidentMarker = u32(record, reserved + 11 * 4);
  const incidentWords = Buffer.alloc(12);
  incidentWords.writeUInt32LE(0x49494431, 0);
  incidentWords.writeUInt32LE(incidentLow, 4);
  incidentWords.writeUInt32LE(incidentHigh, 8);
  const incidentId = incidentMarker === 0x49494431 &&
    (incidentLow !== 0 || incidentHigh !== 0) &&
    crc32(incidentWords) === incidentCrc
    ? ((BigInt(incidentHigh) << 32n) | BigInt(incidentLow)).toString()
    : null;
  const footer = record.subarray(FOOTER_OFFSET);
  return {
    incident_id: incidentId,
    reset_flags: `0x${header.reset_flags.toString(16).padStart(8, '0').toUpperCase()}`,
    reset_count: header.reset_count,
    source_fingerprint_crc32: header.source_fingerprint_crc32,
    source_fingerprint_prefix: header.source_fingerprint_prefix,
    header,
    fault: validFault(record),
    events: validEvents(record),
    runtime_context: runtimeSidecar.context,
    components: runtimeSidecar.components,
    interrupted_frame: runtimeSidecar.interrupted_frame,
    first_failure: firstFailure,
    retained_footer: {
      stage_sequence: u32(footer, 8),
      stage_commit: `0x${u32(footer, 12).toString(16).padStart(8, '0').toUpperCase()}`,
      flash_sequence: u32(footer, 16),
      flash_crc32: `0x${u32(footer, 20).toString(16).padStart(8, '0').toUpperCase()}`,
      recovery_clear_magic: `0x${u32(footer, 24).toString(16).padStart(8, '0').toUpperCase()}`,
      pending_flash_magic: `0x${pendingFlash.toString(16).padStart(8, '0').toUpperCase()}`,
      fault_reset_requested: `0x${faultResetRequested.toString(16).padStart(8, '0').toUpperCase()}`,
      incident_id_valid: incidentId !== null,
      flash_commit: `0x${u32(footer, 252).toString(16).padStart(8, '0').toUpperCase()}`,
    },
  };
}

function normalizeAddress(address) {
  const value = Number.parseInt(address, 16) >>> 0;
  return `0x${(value & 0xfffffffe).toString(16).padStart(8, '0').toUpperCase()}`;
}

function symbolize(records, elfPaths, addr2linePath) {
  const elfFiles = elfPaths.map((elfPath) => ({
    path: elfPath,
    data: fs.readFileSync(elfPath),
  }));
  const groups = new Map();
  for (const record of records) {
    const prefix = Buffer.from(record.source_fingerprint_prefix, 'ascii');
    const elfIndexes = [];
    if (prefix.length > 0) {
      for (let index = 0; index < elfFiles.length; index += 1) {
        if (elfFiles[index].data.indexOf(prefix) >= 0) elfIndexes.push(index);
      }
    }
    if (elfIndexes.length === 0) {
      record.symbolization_status = 'skipped_no_matching_elf';
      continue;
    }
    record.symbolization_status = 'pending';
    const groupKey = elfIndexes.join(',');
    let group = groups.get(groupKey);
    if (!group) {
      group = { elfIndexes, records: [], addresses: new Set() };
      groups.set(groupKey, group);
    }
    group.records.push(record);
    const addresses = group.addresses;
    if (record.fault) {
      for (const address of [record.fault.pc, record.fault.lr,
        record.fault.caller_address]) {
        if (address) addresses.add(normalizeAddress(address));
      }
    }
    for (const event of record.events) addresses.add(normalizeAddress(event.pc));
    if (record.runtime_context) {
      if (!record.runtime_context.fault_registers ||
          record.runtime_context.frame_valid) {
        addresses.add(normalizeAddress(record.runtime_context.pc));
        addresses.add(normalizeAddress(record.runtime_context.lr));
      }
    }
    for (const component of record.components) {
      addresses.add(normalizeAddress(component.heartbeat_callsite_pc));
    }
    if (record.first_failure) {
      if (!record.first_failure.fault_registers ||
          record.first_failure.frame_valid) {
        addresses.add(normalizeAddress(record.first_failure.pc));
        addresses.add(normalizeAddress(record.first_failure.lr));
      }
    }
    const lastPc = `0x${record.header.last_core_pc.toString(16).padStart(8, '0').toUpperCase()}`;
    addresses.add(normalizeAddress(lastPc));
  }

  const command = addr2linePath ?? 'arm-none-eabi-addr2line';
  for (const group of groups.values()) {
    const unique = [...group.addresses];
    if (unique.length === 0) continue;
    const symbols = new Map();
    for (const elfIndex of group.elfIndexes) {
      const elfPath = elfFiles[elfIndex].path;
      const result = spawnSync(command, ['-e', elfPath, '-f', '-C', '-a', ...unique],
        { encoding: 'utf8', windowsHide: true });
      if (result.error || result.status !== 0) {
        throw new Error(result.stderr || result.error?.message || 'addr2line failed');
      }
      const lines = result.stdout.split(/\r?\n/);
      for (let i = 0; i + 2 < lines.length; i += 3) {
        const address = lines[i].trim();
        const symbol = `${lines[i + 1].trim()} at ${lines[i + 2].trim()}`;
        if (address && symbol !== '?? at ??:0' && symbol !== '?? at ??:?') {
          symbols.set(address.toUpperCase().replace(/^0X/, '0x'), symbol);
        }
      }
    }

    for (const record of group.records) {
      if (record.fault) {
        for (const key of ['pc', 'lr', 'caller_address']) {
          if (record.fault[key]) {
            record.fault[`${key}_symbol`] =
              symbols.get(normalizeAddress(record.fault[key])) ?? '??';
          }
        }
      }
      for (const event of record.events) {
        event.pc_symbol = symbols.get(normalizeAddress(event.pc)) ?? '??';
      }
      if (record.runtime_context) {
        if (record.runtime_context.fault_registers &&
            !record.runtime_context.frame_valid) {
          record.runtime_context.pc_symbol = null;
          record.runtime_context.lr_symbol = null;
        }
        else {
          record.runtime_context.pc_symbol =
            symbols.get(normalizeAddress(record.runtime_context.pc)) ?? '??';
          record.runtime_context.lr_symbol =
            symbols.get(normalizeAddress(record.runtime_context.lr)) ?? '??';
        }
      }
      for (const component of record.components) {
        component.heartbeat_callsite_pc_symbol =
          symbols.get(normalizeAddress(component.heartbeat_callsite_pc)) ?? '??';
      }
      if (record.first_failure) {
        if (record.first_failure.fault_registers &&
            !record.first_failure.frame_valid) {
          record.first_failure.pc_symbol = null;
          record.first_failure.lr_symbol = null;
        }
        else {
          record.first_failure.pc_symbol =
            symbols.get(normalizeAddress(record.first_failure.pc)) ?? '??';
          record.first_failure.lr_symbol =
            symbols.get(normalizeAddress(record.first_failure.lr)) ?? '??';
        }
      }
      const lastPc = `0x${record.header.last_core_pc.toString(16).padStart(8, '0').toUpperCase()}`;
      record.header.last_core_pc_symbol = symbols.get(normalizeAddress(lastPc)) ?? '??';
      record.symbolization_status = 'complete';
    }
  }
}

function parseArgs(argv) {
  const options = { pretty: false, elfs: [], addr2line: null, input: null };
  for (let i = 0; i < argv.length; i += 1) {
    const item = argv[i];
    if (item === '--pretty') options.pretty = true;
    else if (item === '--elf') {
      const elf = argv[++i];
      if (!elf) throw new Error('--elf requires a path');
      options.elfs.push(elf);
    }
    else if (item === '--addr2line') options.addr2line = argv[++i];
    else if (item === '--help' || item === '-h') options.help = true;
    else if (!options.input) options.input = item;
    else throw new Error(`unexpected argument: ${item}`);
  }
  return options;
}

function main() {
  const args = parseArgs(process.argv.slice(2));
  if (args.help || !args.input) {
    console.log('Usage: node decode_watchdog_log.mjs <4-KiB-retained-or-260-KiB-journal.bin> [--pretty] [--elf matching-app.elf] [--elf matching-boot.elf] [--addr2line path]');
    return args.help ? 0 : 2;
  }
  const inputPath = path.resolve(args.input);
  const data = fs.readFileSync(inputPath);
  if (data.length === RECORD_SIZE) {
    const record = validRetainedRecord(data);
    if (record === null) {
      throw new Error('retained record has no valid V2 header');
    }
    if (args.elfs.length) {
      symbolize([record], args.elfs.map((elf) => path.resolve(elf)), args.addr2line);
    }
    console.log(JSON.stringify({
      schema: 'artpi2-watchdog-retained-v2',
      retained_record_bytes: RECORD_SIZE,
      records: [record],
    }, null, args.pretty ? 2 : 0));
    return 0;
  }
  if (data.length !== JOURNAL_SIZE) {
    throw new Error(`input must be exactly ${RECORD_SIZE} bytes (retained record) or ${JOURNAL_SIZE} bytes (journal); got ${data.length}`);
  }
  const records = [];
  for (let slot = 0; slot < SLOT_COUNT; slot += 1) {
    const record = validFlashRecord(data.subarray(slot * RECORD_SIZE,
                                                 (slot + 1) * RECORD_SIZE));
    if (record) {
      record.slot = slot;
      records.push(record);
    }
  }
  records.sort((a, b) => a.flash_sequence - b.flash_sequence);
  if (args.elfs.length && records.length) {
    symbolize(records, args.elfs.map((elf) => path.resolve(elf)), args.addr2line);
  }
  console.log(JSON.stringify({
    schema: 'artpi2-watchdog-log-v2',
    journal_bytes: JOURNAL_SIZE,
    valid_records: records.length,
    records,
  }, null, args.pretty ? 2 : 0));
  return 0;
}

try {
  process.exitCode = main();
} catch (error) {
  console.error(`watchdog log decode failed: ${error.message}`);
  process.exitCode = 2;
}
