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

if (-not $ProgrammerCli) {
    $programmerCommand = Get-Command STM32_Programmer_CLI.exe -ErrorAction SilentlyContinue
    if ($programmerCommand) { $ProgrammerCli = $programmerCommand.Source }
}
if (-not $ProgrammerCli -or -not (Test-Path -LiteralPath $ProgrammerCli)) {
    throw '找不到 STM32_Programmer_CLI.exe。请设置 STM32_CUBE_PROGRAMMER_CLI 或将其加入 PATH。'
}
$ProgrammerCli = (Resolve-Path -LiteralPath $ProgrammerCli).Path

$resolvedOutput = [System.IO.Path]::GetFullPath($OutputPath)
if (Test-Path -LiteralPath $resolvedOutput) {
    throw "输出文件已存在；请指定新路径以保护原始现场: $resolvedOutput"
}
$parent = Split-Path -Parent $resolvedOutput
if (-not (Test-Path -LiteralPath $parent)) {
    New-Item -ItemType Directory -Path $parent | Out-Null
}

Write-Host 'Reading the XIP-mapped watchdog trace sidecar in SWD HotPlug mode.'
Write-Host 'This command does not request reset, run, erase, program, or external-loader execution.'
& $ProgrammerCli -c port=SWD "sn=$SerialNumber" mode=HOTPLUG `
    -u 0x73FAF000 0x10000 $resolvedOutput
if ($LASTEXITCODE -ne 0) {
    throw "Watchdog trace sidecar readback failed (exit $LASTEXITCODE)."
}
if ((Get-Item -LiteralPath $resolvedOutput).Length -ne 0x10000) {
    throw 'Watchdog trace sidecar readback length is not 64 KiB.'
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
