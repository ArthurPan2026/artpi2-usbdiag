[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$OutputPath,
    [string]$SerialNumber = $env:ARTPI2_STLINK_SERIAL,
    [string]$ProgrammerCli = $env:STM32_CUBE_PROGRAMMER_CLI
)

$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($SerialNumber)) {
    throw '请通过 -SerialNumber 或 ARTPI2_STLINK_SERIAL 指定目标 ST-Link。'
}

$cli = $ProgrammerCli
if (-not $cli) {
    $programmerCommand = Get-Command STM32_Programmer_CLI.exe -ErrorAction SilentlyContinue
    if ($programmerCommand) { $cli = $programmerCommand.Source }
}
if (-not $cli -or -not (Test-Path -LiteralPath $cli)) {
    throw '找不到 STM32_Programmer_CLI.exe。请设置 STM32_CUBE_PROGRAMMER_CLI 或将其加入 PATH。'
}
$cli = (Resolve-Path -LiteralPath $cli).Path

$resolvedOutput = [System.IO.Path]::GetFullPath($OutputPath)
if (Test-Path -LiteralPath $resolvedOutput) {
    throw "Output already exists; choose a new path: $resolvedOutput"
}
$parent = Split-Path -Parent $resolvedOutput
if (-not (Test-Path -LiteralPath $parent)) {
    New-Item -ItemType Directory -Path $parent | Out-Null
}

Write-Host 'Reading the XIP-mapped watchdog journal in SWD HotPlug mode.'
Write-Host 'The command does not request reset, run, erase, program, or external-loader execution.'
& $cli -c port=SWD "sn=$SerialNumber" mode=HOTPLUG `
    -u 0x73FBF000 0x41000 $resolvedOutput
if ($LASTEXITCODE -ne 0) {
    throw "Watchdog journal readback failed (exit $LASTEXITCODE)."
}
if ((Get-Item -LiteralPath $resolvedOutput).Length -ne 0x41000) {
    throw 'Watchdog journal readback length is not 260 KiB.'
}
Write-Host "Readback saved: $resolvedOutput"
$sha256 = [Security.Cryptography.SHA256]::Create()
try {
    $digest = [BitConverter]::ToString(
        $sha256.ComputeHash([IO.File]::ReadAllBytes($resolvedOutput))).Replace('-', '')
}
finally {
    $sha256.Dispose()
}
Write-Host "SHA-256: $digest"
