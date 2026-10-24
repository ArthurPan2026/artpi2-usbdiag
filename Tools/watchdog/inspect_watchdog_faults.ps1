[CmdletBinding()]
param(
    [string]$SerialNumber = $env:ARTPI2_STLINK_SERIAL,
    [string]$ProgrammerCli = $env:STM32_CUBE_PROGRAMMER_CLI,
    [string]$OutputDirectory,
    [string]$AppElf,
    [string]$BootElf,
    [string]$Addr2Line
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))

if ([string]::IsNullOrWhiteSpace($SerialNumber)) {
    throw '请通过 -SerialNumber 或 ARTPI2_STLINK_SERIAL 指定目标 ST-Link。'
}

if ([string]::IsNullOrWhiteSpace($ProgrammerCli)) {
    $programmerCommand = Get-Command STM32_Programmer_CLI.exe -ErrorAction SilentlyContinue
    if ($programmerCommand) { $ProgrammerCli = $programmerCommand.Source }
}
if (-not $ProgrammerCli -or
    -not (Test-Path -LiteralPath $ProgrammerCli -PathType Leaf)) {
    throw '找不到 STM32_Programmer_CLI.exe。请设置 STM32_CUBE_PROGRAMMER_CLI 或将其加入 PATH。'
}
$ProgrammerCli = (Resolve-Path -LiteralPath $ProgrammerCli).Path

$NodeCommand = Get-Command node.exe -ErrorAction SilentlyContinue
if (-not $NodeCommand) {
    $NodeCommand = Get-Command node -ErrorAction SilentlyContinue
}
if (-not $NodeCommand) {
    throw '找不到 node.exe；请安装 Node.js 或将其加入 PATH。'
}
$Node = $NodeCommand.Source

foreach ($optionalFile in @($AppElf, $BootElf, $Addr2Line)) {
    if (-not [string]::IsNullOrWhiteSpace($optionalFile) -and
        -not (Test-Path -LiteralPath $optionalFile -PathType Leaf)) {
        throw "指定的符号化文件不存在: $optionalFile"
    }
}
if ((-not [string]::IsNullOrWhiteSpace($AppElf) -or
     -not [string]::IsNullOrWhiteSpace($BootElf)) -and
    [string]::IsNullOrWhiteSpace($Addr2Line)) {
    $addr2lineCommand = Get-Command arm-none-eabi-addr2line.exe -ErrorAction SilentlyContinue
    if (-not $addr2lineCommand) {
        $addr2lineCommand = Get-Command arm-none-eabi-addr2line -ErrorAction SilentlyContinue
    }
    if (-not $addr2lineCommand) {
        throw '提供 ELF 时需要 arm-none-eabi-addr2line；请安装工具链或用 -Addr2Line 指定路径。'
    }
    $Addr2Line = $addr2lineCommand.Source
}
if (-not [string]::IsNullOrWhiteSpace($Addr2Line)) {
    $Addr2Line = (Resolve-Path -LiteralPath $Addr2Line).Path
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $OutputDirectory = Join-Path $ProjectRoot ('output\watchdog-readback-' + $stamp)
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $OutputDirectory) {
    throw "输出目录已存在；为保护旧证据，请指定一个新目录: $OutputDirectory"
}
[void][IO.Directory]::CreateDirectory($OutputDirectory)

$readStages = @(
    [pscustomobject]@{
        Name = 'journal'
        Script = 'read_watchdog_log.ps1'
        File = 'watchdog-journal.bin'
        Length = 0x41000
    },
    [pscustomobject]@{
        Name = 'trace'
        Script = 'read_watchdog_trace.ps1'
        File = 'watchdog-trace.bin'
        Length = 0x10000
    },
    [pscustomobject]@{
        Name = 'bkpsram_read1'
        Script = 'read_bkpsram_snapshot.ps1'
        File = 'watchdog-bkpsram-read1.bin'
        Length = 0x1000
    },
    [pscustomobject]@{
        Name = 'bkpsram_read2'
        Script = 'read_bkpsram_snapshot.ps1'
        File = 'watchdog-bkpsram-read2.bin'
        Length = 0x1000
    }
)
$failedStage = $null
$failureMessage = $null
$completedFiles = [Collections.Generic.List[string]]::new()

Write-Host '读取看门狗现场（SWD HotPlug，只读；不执行复位、擦除或编程）。'
Write-Host ('输出目录: ' + $OutputDirectory)
foreach ($stage in $readStages) {
    $failedStage = $stage.Name
    $reader = Join-Path $PSScriptRoot $stage.Script
    $readback = Join-Path $OutputDirectory $stage.File
    Write-Host ('--- ' + $stage.Name + ' ---')
    try {
        & $reader -OutputPath $readback -SerialNumber $SerialNumber -ProgrammerCli $ProgrammerCli
        if (-not (Test-Path -LiteralPath $readback -PathType Leaf)) {
            throw '读取脚本未生成预期的原始读回文件。'
        }
        $actualLength = (Get-Item -LiteralPath $readback).Length
        if ($actualLength -ne $stage.Length) {
            throw ("读回文件长度错误：预期 " + $stage.Length +
                "，实际 " + $actualLength + "。")
        }
    }
    catch {
        $failureMessage = $_.Exception.Message
        break
    }
    if ($LASTEXITCODE -ne 0) {
        $failureMessage = "底层读取脚本返回退出码 $LASTEXITCODE。"
        break
    }
    $completedFiles.Add($stage.File)
}

if ($failureMessage) {
    $partialFiles = @(
        foreach ($stage in $readStages) {
            $readback = Join-Path $OutputDirectory $stage.File
            if ((Test-Path -LiteralPath $readback -PathType Leaf) -and
                -not $completedFiles.Contains($stage.File)) {
                $stage.File
            }
        }
    )
    $partial = [ordered]@{
        schema = 'artpi2-watchdog-readback-acquisition-v1'
        captured_at = [DateTime]::UtcNow.ToString('o')
        status = 'READ_FAILED'
        failed_stage = $failedStage
        error = $failureMessage
        output_directory = $OutputDirectory
        completed_files = @($completedFiles.ToArray())
        partial_files = $partialFiles
    }
    $partialJson = $partial | ConvertTo-Json -Depth 5
    [IO.File]::WriteAllText(
        (Join-Path $OutputDirectory 'acquisition-error.json'),
        $partialJson + [Environment]::NewLine,
        [Text.UTF8Encoding]::new($false))
    Write-Host ("读取在阶段 '" + $failedStage + "' 失败；已保留此前成功读回的文件。") -ForegroundColor Red
    Write-Host ('详情: ' + (Join-Path $OutputDirectory 'acquisition-error.json'))
    exit 1
}

$summaryArguments = @(
    (Join-Path $PSScriptRoot 'summarize_watchdog_readback.mjs'),
    '--journal', (Join-Path $OutputDirectory 'watchdog-journal.bin'),
    '--trace', (Join-Path $OutputDirectory 'watchdog-trace.bin'),
    '--retained-first', (Join-Path $OutputDirectory 'watchdog-bkpsram-read1.bin'),
    '--retained-second', (Join-Path $OutputDirectory 'watchdog-bkpsram-read2.bin'),
    '--output-directory', $OutputDirectory
)
if (-not [string]::IsNullOrWhiteSpace($AppElf)) {
    $summaryArguments += @('--app-elf', (Resolve-Path -LiteralPath $AppElf).Path)
}
if (-not [string]::IsNullOrWhiteSpace($BootElf)) {
    $summaryArguments += @('--boot-elf', (Resolve-Path -LiteralPath $BootElf).Path)
}
if (-not [string]::IsNullOrWhiteSpace($Addr2Line)) {
    $summaryArguments += @('--addr2line', $Addr2Line)
}

& $Node @summaryArguments
$summaryExitCode = $LASTEXITCODE
if ($summaryExitCode -ne 0) {
    Write-Host ("解码摘要返回退出码 " + $summaryExitCode + "；原始读回和解码信息保留在 " + $OutputDirectory) -ForegroundColor Red
}
exit $summaryExitCode
