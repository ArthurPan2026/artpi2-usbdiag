[CmdletBinding()]
param(
    [Uri]$BoardUri = $env:ARTPI2_BOARD_URI,
    [ValidateRange(2, 30)]
    [int]$TimeoutSeconds = 6
)

$ErrorActionPreference = 'Stop'
if (-not $BoardUri) {
    throw '请通过 -BoardUri 或 ARTPI2_BOARD_URI 指定板端 HTTP 地址。'
}
if ($BoardUri.Scheme -notin @('http', 'https')) {
    throw 'BoardUri 必须使用 http 或 https。'
}
$Base = $BoardUri.GetLeftPart([System.UriPartial]::Authority).TrimEnd('/')
$Client = [System.Net.Http.HttpClient]::new()
$Client.Timeout = [TimeSpan]::FromSeconds($TimeoutSeconds)

function Get-ResponseBytes([string]$Url) {
    $response = $null
    try {
        $response = $Client.GetAsync($Url).GetAwaiter().GetResult()
        $bytes = $response.Content.ReadAsByteArrayAsync().GetAwaiter().GetResult()
        return [pscustomobject]@{
            Status = [int]$response.StatusCode
            Bytes = [byte[]]$bytes
            ContentType = [string]$response.Content.Headers.ContentType
        }
    }
    catch {
        throw "板端 HTTP 请求失败：$Url ($($_.Exception.Message))"
    }
    finally {
        if ($response) { $response.Dispose() }
    }
}

function Get-Json([string]$Path) {
    $response = Get-ResponseBytes "$Base$Path"
    if ($response.Status -ne 200) {
        throw "GET $Path 返回 HTTP $($response.Status)"
    }
    return [System.Text.Encoding]::UTF8.GetString($response.Bytes) |
        ConvertFrom-Json
}

function Get-Crc32([byte[]]$Bytes) {
    [uint64]$mask = 4294967295
    [uint64]$polynomial = 3988292384
    [uint64]$crc = $mask
    foreach ($byte in $Bytes) {
        $crc = ($crc -bxor [uint64]$byte) -band $mask
        for ($bit = 0; $bit -lt 8; $bit++) {
            if (($crc -band 1) -ne 0) {
                $crc = (($crc -shr 1) -bxor $polynomial) -band $mask
            }
            else {
                $crc = ($crc -shr 1) -band $mask
            }
        }
    }
    return ('{0:X8}' -f [uint32](($crc -bxor $mask) -band $mask))
}

function Read-PagedBytes([string]$Path, [int]$ExpectedLength) {
    $received = [byte[]]::new($ExpectedLength)
    $offset = 0
    while ($offset -lt $ExpectedLength) {
        $response = Get-ResponseBytes "$Base$Path/$offset"
        if ($response.Status -ne 200 -or $response.Bytes.Length -eq 0 -or
            $response.Bytes.Length -gt ($ExpectedLength - $offset)) {
            throw "分页读取失败，offset=$offset，HTTP $($response.Status)"
        }
        [System.Buffer]::BlockCopy($response.Bytes, 0, $received, $offset,
                                   $response.Bytes.Length)
        $offset += $response.Bytes.Length
    }
    return ,$received
}

try {
    $page = Get-ResponseBytes "$Base/"
    if ($page.Status -ne 200 -or $page.ContentType -notmatch 'text/html') {
        throw 'Web 首页没有正常响应。'
    }

    $build = Get-Json '/api/build'
    if ($build.profile -ne 'release' -or $build.build_type -ne 'Release' -or
        $build.source_fingerprint -notmatch '^[A-Fa-f0-9]{64}$' -or
        $build.rt_thread -ne '5.3.0' -or $build.lvgl -ne '9.6.0') {
        throw '板端构建身份不是有效的 Release 镜像。'
    }

    $wifi = Get-Json '/api/wifi/status'
    if (-not $wifi.radio_ready -or $wifi.phase -ne 'online' -or
        [string]::IsNullOrWhiteSpace([string]$wifi.ip)) {
        throw '板端 Wi-Fi 尚未联网或未取得 IP。'
    }

    $usb = Get-Json '/api/usb/manifest'
    if (-not $usb.present -or -not $usb.enumeration_complete) {
        throw 'USB HID 尚未完成枚举。'
    }
    $interfaces = Get-Json '/api/usb/interfaces'
    $hidCount = @($interfaces.items | Where-Object { [int]$_.class_code -eq 3 }).Count
    if ($hidCount -lt 1) {
        throw '当前 USB 设备未报告 HID 接口。'
    }

    $manifest = Get-Json '/api/evidence'
    if ($manifest.schema_version -ne 2 -or $manifest.page_size -ne 4096 -or
        $manifest.token -notmatch '^[0-9A-Fa-f]{8}-[0-9A-Fa-f]{8}-[0-9A-Fa-f]{8}$' -or
        $manifest.total_bytes -le 0 -or $manifest.total_bytes -gt 131072 -or
        $manifest.raw_event_count -lt 1) {
        throw 'USB 证据 manifest 的格式、容量或事件数量无效。'
    }

    $summaryBytes = Read-PagedBytes "/api/evidence/page/$($manifest.token)" `
        ([int]$manifest.total_bytes)
    if ((Get-Crc32 $summaryBytes) -ne
        $manifest.json_crc32.Substring(2).ToUpperInvariant()) {
        throw 'USB 证据摘要 JSON CRC32 不匹配。'
    }
    $snapshot = [System.Text.Encoding]::UTF8.GetString($summaryBytes) |
        ConvertFrom-Json
    if ($snapshot.schema_version -ne 2 -or $snapshot.parser_version -ne 2 -or
        $snapshot.ruleset_version -ne 3 -or
        $snapshot.snapshot_id -ne $manifest.snapshot_id -or
        $snapshot.revision -ne $manifest.revision -or
        $snapshot.raw_events.Count -ne $manifest.raw_event_count) {
        throw 'USB 证据摘要与 manifest 不属于同一份快照。'
    }

    $payloadCount = 0
    foreach ($event in $snapshot.raw_events) {
        $length = [int]$event.captured_length
        if ($length -le 0) { continue }
        $eventBytes = Read-PagedBytes `
            "/api/evidence/event/$($manifest.token)/$($event.sequence)" $length
        if ((Get-Crc32 $eventBytes) -ne
            $event.payload_crc32.Substring(2).ToUpperInvariant()) {
            throw "USB 原始事件 payload CRC32 不匹配，sequence=$($event.sequence)。"
        }
        $payloadCount++
    }

    Write-Output 'PASS: Web 首页与 Release 构建身份。'
    Write-Output 'PASS: Wi-Fi 在线，真实 HID 已枚举。'
    Write-Output ("PASS: 证据摘要 CRC32、{0} 个原始 payload CRC32；HID 接口数={1}，快照={2}/{3}。" -f
        $payloadCount, $hidCount, $manifest.snapshot_id, $manifest.revision)
}
finally {
    $Client.Dispose()
}
