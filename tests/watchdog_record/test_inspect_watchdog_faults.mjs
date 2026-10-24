import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const script = path.join(repoRoot, 'Tools', 'watchdog',
  'inspect_watchdog_faults.ps1');
const tempRoot = fs.mkdtempSync(path.join(os.tmpdir(),
  'artpi2-watchdog-read-failure-'));
const fakeCli = path.join(tempRoot, 'mock-programmer.cmd');
const fakeCliScript = path.join(tempRoot, 'mock-programmer.mjs');
const outputDirectory = path.join(tempRoot, 'readback');

try {
  fs.writeFileSync(fakeCliScript, [
    "import fs from 'node:fs';",
    "const args = process.argv.slice(2);",
    "const read = args.indexOf('-u');",
    "if (read < 0) process.exit(80);",
    "const address = (args[read + 1] || '').toLowerCase();",
    "const length = Number(args[read + 2]);",
    "const destination = args[read + 3];",
    "if (address === '0x73fbf000') {",
    "  fs.writeFileSync(destination, Buffer.alloc(length, 0xff));",
    "  process.exit(0);",
    "}",
    "if (address === '0x73faf000') {",
    "  fs.writeFileSync(destination, Buffer.alloc(128, 0xff));",
    "  process.exit(7);",
    "}",
    "process.exit(81);",
    '',
  ].join('\r\n'), 'utf8');
  fs.writeFileSync(fakeCli, [
    '@echo off',
    'node "%~dp0mock-programmer.mjs" %*',
    'exit /b %ERRORLEVEL%',
    '',
  ].join('\r\n'), 'ascii');

  const selectedShell = process.env.ARTPI2_TEST_POWERSHELL_EXE || 'pwsh.exe';
  let result = spawnSync(selectedShell, [
    '-NoProfile',
    '-ExecutionPolicy', 'Bypass',
    '-File', script,
    '-SerialNumber', 'TEST-ONLY',
    '-ProgrammerCli', fakeCli,
    '-OutputDirectory', outputDirectory,
  ], { encoding: 'utf8', windowsHide: true, maxBuffer: 4 * 1024 * 1024 });
  if (!process.env.ARTPI2_TEST_POWERSHELL_EXE &&
      result.error?.code === 'ENOENT') {
    result = spawnSync('powershell.exe', [
      '-NoProfile',
      '-ExecutionPolicy', 'Bypass',
      '-File', script,
      '-SerialNumber', 'TEST-ONLY',
      '-ProgrammerCli', fakeCli,
      '-OutputDirectory', outputDirectory,
    ], { encoding: 'utf8', windowsHide: true, maxBuffer: 4 * 1024 * 1024 });
  }
  assert.equal(result.status, 1,
    'the mocked trace read failure should propagate as exit 1\n' +
    (result.stdout || '') + '\n' + (result.stderr || ''));

  const errorPath = path.join(outputDirectory, 'acquisition-error.json');
  assert.ok(fs.existsSync(errorPath), 'acquisition error report was not saved\n' +
    (result.stdout || '') + '\n' + (result.stderr || ''));
  const report = JSON.parse(fs.readFileSync(errorPath, 'utf8'));
  assert.equal(report.failed_stage, 'trace', JSON.stringify(report));
  assert.deepEqual(report.completed_files, ['watchdog-journal.bin']);
  assert.deepEqual(report.partial_files, ['watchdog-trace.bin']);
  assert.equal(fs.statSync(path.join(outputDirectory,
    'watchdog-journal.bin')).size, 0x41000);
  assert.equal(fs.statSync(path.join(outputDirectory,
    'watchdog-trace.bin')).size, 128);
  assert.ok(!report.completed_files.some(name => name.endsWith('.stlink.log')));
  console.log('watchdog acquisition failure retention test passed');
} finally {
  const resolvedTemp = path.resolve(tempRoot);
  const expectedParent = path.resolve(os.tmpdir()) + path.sep;
  assert.ok(resolvedTemp.toLowerCase().startsWith(expectedParent.toLowerCase()),
    'refusing to remove a test directory outside the system temp directory');
  fs.rmSync(resolvedTemp, { recursive: true, force: true });
}
