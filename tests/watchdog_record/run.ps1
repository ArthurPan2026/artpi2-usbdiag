[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ArtifactDirectory
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$ArtifactDirectory = [IO.Path]::GetFullPath($ArtifactDirectory)
[void][IO.Directory]::CreateDirectory($ArtifactDirectory)
$BuildDirectory = Join-Path $ArtifactDirectory 'build-release'
$ConfigureLog = Join-Path $ArtifactDirectory 'configure.log'
$BuildLog = Join-Path $ArtifactDirectory 'build.log'
$CTestLog = Join-Path $ArtifactDirectory 'ctest.log'
$DecoderLog = Join-Path $ArtifactDirectory 'decoder.log'
$TraceDecoderLog = Join-Path $ArtifactDirectory 'trace-decoder.log'
$ReadbackSummaryLog = Join-Path $ArtifactDirectory 'readback-summary.log'
$ReadbackAcquisitionLog = Join-Path $ArtifactDirectory 'readback-acquisition.log'

& cmake -S $PSScriptRoot -B $BuildDirectory -G Ninja `
    -DCMAKE_BUILD_TYPE=Release 2>&1 | Set-Content -LiteralPath $ConfigureLog -Encoding utf8
if ($LASTEXITCODE -ne 0) { throw "Watchdog host configure failed; see $ConfigureLog." }

& cmake --build $BuildDirectory --parallel 4 2>&1 |
    Set-Content -LiteralPath $BuildLog -Encoding utf8
if ($LASTEXITCODE -ne 0) { throw "Watchdog host build failed; see $BuildLog." }

& ctest --test-dir $BuildDirectory --output-on-failure 2>&1 |
    Set-Content -LiteralPath $CTestLog -Encoding utf8
if ($LASTEXITCODE -ne 0) { throw "Watchdog host CTest failed; see $CTestLog." }

$Node = (Get-Command node -ErrorAction Stop).Source
& $Node (Join-Path $PSScriptRoot 'test_decode_watchdog_log.mjs') 2>&1 |
    Set-Content -LiteralPath $DecoderLog -Encoding utf8
if ($LASTEXITCODE -ne 0) { throw "Watchdog decoder test failed; see $DecoderLog." }

& $Node (Join-Path $PSScriptRoot 'test_decode_watchdog_trace.mjs') 2>&1 |
    Set-Content -LiteralPath $TraceDecoderLog -Encoding utf8
if ($LASTEXITCODE -ne 0) { throw "Watchdog trace decoder test failed; see $TraceDecoderLog." }

& $Node (Join-Path $PSScriptRoot 'test_summarize_watchdog_readback.mjs') 2>&1 |
    Set-Content -LiteralPath $ReadbackSummaryLog -Encoding utf8
if ($LASTEXITCODE -ne 0) { throw "Watchdog readback-summary test failed; see $ReadbackSummaryLog." }

& $Node (Join-Path $PSScriptRoot 'test_inspect_watchdog_faults.mjs') 2>&1 |
    Set-Content -LiteralPath $ReadbackAcquisitionLog -Encoding utf8
if ($LASTEXITCODE -ne 0) { throw "Watchdog acquisition failure test failed; see $ReadbackAcquisitionLog." }

$Report = [pscustomobject]@{
    status = 'PASS'
    build_type = 'Release'
    ctest = 'PASS'
    decoder = 'PASS'
    trace_decoder = 'PASS'
    readback_summary = 'PASS'
    readback_acquisition = 'PASS'
    checks_survive_ndebug = $true
}
$ReportPath = Join-Path $ArtifactDirectory 'watchdog-host-report.json'
$Report | ConvertTo-Json -Depth 4 |
    Set-Content -LiteralPath $ReportPath -Encoding utf8
Write-Host "Watchdog record host tests PASS; report: $ReportPath"
