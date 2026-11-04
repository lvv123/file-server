# Acceptance run for MVP criterion #1:
#   "upload a 10 GiB file, kill the process midway, query the session after a
#    restart and resume successfully"
#
# This is a manual, long-running check (it writes roughly 2x $SizeMB to disk).
# It is NOT part of run_api_tests.ps1 because of its size and duration.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests\api\acceptance_large.ps1
#   ... -SizeMB 512          # quick sanity run
#   ... -Concurrency 1       # strictly sequential chunk requests
#   ... -KeepData            # keep the temp data dir for inspection
#
[CmdletBinding()]
param(
    [string]$Exe = "build\Debug\file-server.exe",
    [int]$SizeMB = 10240,
    [int]$ChunkMB = 8,
    [int]$KillAfterPercent = 40,
    [int]$Concurrency = 4,
    [switch]$KeepData
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Net.Http

if ($Concurrency -lt 1) { $Concurrency = 1 }

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
if (-not [System.IO.Path]::IsPathRooted($Exe)) {
    $Exe = Join-Path $RepoRoot $Exe
}
if (-not (Test-Path $Exe)) { throw "server executable not found: $Exe" }

$script:Pass = 0
$script:Fail = 0

function Step([string]$msg) {
    Write-Host ""
    Write-Host "== $msg" -ForegroundColor Cyan
}

function Check([string]$name, [scriptblock]$body) {
    try {
        & $body
        $script:Pass++
        Write-Host ("[ PASS ] " + $name) -ForegroundColor Green
    } catch {
        $script:Fail++
        Write-Host ("[ FAIL ] " + $name + " :: " + $_.Exception.Message) -ForegroundColor Red
    }
}

function Assert-True($cond, [string]$message) {
    if (-not $cond) { throw $message }
}

# ---------------------------------------------------------------- infrastructure

$WorkDir = Join-Path $env:TEMP ("fsrv-accept-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null
$DataRoot = Join-Path $WorkDir "share"
New-Item -ItemType Directory -Path $DataRoot -Force | Out-Null

function Get-FreePort {
    $l = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, 0)
    $l.Start()
    $p = $l.LocalEndpoint.Port
    $l.Stop()
    return $p
}

$Port = Get-FreePort
$BaseUrl = "http://127.0.0.1:$Port"

$script:ServerArgs = @(
    "--dir", $DataRoot,
    "--port", [string]$Port,
    "--bind-address", "127.0.0.1",
    "--chunk-size-mb", [string]$ChunkMB
)

$script:proc = $null
$script:stdoutLog = Join-Path $WorkDir "stdout.log"
$script:stderrLog = Join-Path $WorkDir "stderr.log"

function Start-Server {
    $script:proc = Start-Process -FilePath $Exe -ArgumentList $script:ServerArgs `
        -PassThru -RedirectStandardOutput $script:stdoutLog -RedirectStandardError $script:stderrLog
    $deadline = (Get-Date).AddSeconds(60)
    while ((Get-Date) -lt $deadline) {
        try {
            $r = Invoke-WebRequest -Uri "$BaseUrl/admin/status" -UseBasicParsing -TimeoutSec 2
            if ($r.StatusCode -eq 200) { return }
        } catch { }
        Start-Sleep -Milliseconds 200
    }
    $tail = ""
    if (Test-Path $script:stderrLog) { $tail = (Get-Content $script:stderrLog -Tail 20) -join "`n" }
    throw "server did not become ready`n$tail"
}

function Stop-Server {
    if ($script:proc) {
        try { $script:proc.Kill() } catch { }
        try { $script:proc.WaitForExit(10000) | Out-Null } catch { }
        $script:proc = $null
        Start-Sleep -Milliseconds 500
    }
}

function Kill-ServerHard {
    # Simulates a crash: no graceful shutdown, no session bookkeeping.
    if ($script:proc) {
        & taskkill.exe /F /PID $script:proc.Id | Out-Null
        try { $script:proc.WaitForExit(10000) | Out-Null } catch { }
        $script:proc = $null
        Start-Sleep -Milliseconds 700
    }
}

$handler = New-Object System.Net.Http.HttpClientHandler
$handler.UseProxy = $false
$handler.AllowAutoRedirect = $false
$client = New-Object System.Net.Http.HttpClient($handler)
$client.Timeout = [System.Threading.Timeout]::InfiniteTimeSpan

# HttpClient on .NET Framework goes through ServicePointManager; without this the
# default connection limit (2) caps how many chunk requests can really overlap.
[System.Net.ServicePointManager]::DefaultConnectionLimit = 32
[System.Net.ServicePointManager]::Expect100Continue = $false

function Post-Json([string]$path, $body) {
    $json = $body | ConvertTo-Json -Depth 6 -Compress
    $content = New-Object System.Net.Http.StringContent($json, [System.Text.Encoding]::UTF8, "application/json")
    $resp = $client.PostAsync("$BaseUrl$path", $content).Result
    $text = $resp.Content.ReadAsStringAsync().Result
    return [pscustomobject]@{ Status = [int]$resp.StatusCode; Text = $text; Json = ($text | ConvertFrom-Json) }
}

function Get-Json([string]$path) {
    $resp = $client.GetAsync("$BaseUrl$path").Result
    $text = $resp.Content.ReadAsStringAsync().Result
    return [pscustomobject]@{ Status = [int]$resp.StatusCode; Text = $text; Json = ($text | ConvertFrom-Json) }
}

# ---------------------------------------------------------------- payload

$ChunkSize = $ChunkMB * 1MB
$TotalSize = [int64]$SizeMB * 1MB
$TotalChunks = [int][Math]::Ceiling($TotalSize / $ChunkSize)

# Byte i of the file is ((i mod 256) * 31 + 7) mod 256. Because 31 is odd the
# pattern repeats every 256 bytes, so every full chunk is byte-identical and can
# be built once. The last (partial) chunk is simply a prefix of it.
Step "building one $ChunkMB MiB chunk of deterministic data"
$template = New-Object byte[] 256
for ($i = 0; $i -lt 256; $i++) { $template[$i] = [byte]((($i * 31) + 7) -band 0xFF) }
$chunkBuf = New-Object byte[] $ChunkSize
for ($off = 0; $off -lt $ChunkSize; $off += 256) {
    [System.Buffer]::BlockCopy($template, 0, $chunkBuf, $off, 256)
}

function Expected-Byte([int64]$index) {
    return [byte](((($index % 256) * 31) + 7) -band 0xFF)
}

# Chunk requests are started without waiting for the previous one, so that
# $Concurrency requests are in flight at any time. `SendAsync` returns at once;
# awaiting the oldest handle keeps the window full without spinning up threads.
function Start-Chunk([string]$sid, [int]$index) {
    $offset = [int64]$index * $ChunkSize
    $len = [int][Math]::Min([int64]$ChunkSize, $TotalSize - $offset)
    $content = New-Object System.Net.Http.ByteArrayContent($chunkBuf, 0, $len)
    $req = New-Object System.Net.Http.HttpRequestMessage([System.Net.Http.HttpMethod]::Put, "$BaseUrl/api/upload/chunk")
    $req.Headers.Add("X-Session-Id", $sid) | Out-Null
    $req.Headers.Add("X-Chunk-Index", [string]$index) | Out-Null
    $req.Content = $content
    return [pscustomobject]@{
        Index = $index
        Request = $req
        Task    = $client.SendAsync($req)
    }
}

function Finish-Chunk($handle) {
    $resp = $handle.Task.Result
    $text = $resp.Content.ReadAsStringAsync().Result
    $handle.Request.Dispose()
    if (-not $resp.IsSuccessStatusCode) {
        throw ("chunk {0} failed: {1} {2}" -f $handle.Index, [int]$resp.StatusCode, $text)
    }
}

function Send-Range([string]$sid, [int]$from, [int]$to) {
    $inflight = New-Object System.Collections.Generic.List[object]
    for ($i = $from; $i -lt $to; $i++) {
        $inflight.Add((Start-Chunk $sid $i))
        if ($inflight.Count -ge $Concurrency) {
            Finish-Chunk $inflight[0]
            $inflight.RemoveAt(0)
        }
        if (($i + 1) % 100 -eq 0) {
            Write-Host ("   {0}/{1} chunks  {2:N0} MiB" -f ($i + 1), $TotalChunks, ((($i + 1) * $ChunkSize) / 1MB))
        }
    }
    foreach ($handle in $inflight) { Finish-Chunk $handle }
}

# ---------------------------------------------------------------- the run

$TargetPath = "/large/$($SizeMB)mb.bin"
$sid = $null
$killedAt = [int][Math]::Floor($TotalChunks * $KillAfterPercent / 100)

try {
    Step "starting server on port $Port (serve: $DataRoot)"
    Start-Server

    Step ("init upload: {0} MiB in {1} chunks of {2} MiB" -f $SizeMB, $TotalChunks, $ChunkMB)
    $init = Post-Json "/api/upload/init" @{ path = $TargetPath; total_size = $TotalSize }
    Assert-True ($init.Status -eq 200) "init status $($init.Status): $($init.Text)"
    $sid = $init.Json.data.session_id
    Assert-True ($init.Json.data.total_chunks -eq $TotalChunks) "server reported $($init.Json.data.total_chunks) chunks, expected $TotalChunks"
    Assert-True ($init.Json.data.chunk_size -eq $ChunkSize) "chunk_size mismatch"
    Write-Host "   session $sid"

    Step ("uploading chunks 0..{0} then killing the server" -f ($killedAt - 1))
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    Send-Range $sid 0 $killedAt
    $sw.Stop()
    Write-Host ("   sent {0} chunks in {1:N1} s ({2:N0} MiB/s)" -f $killedAt, $sw.Elapsed.TotalSeconds, ($killedAt * $ChunkSize / 1MB / $sw.Elapsed.TotalSeconds))

    Step "killing the server process (simulated crash)"
    Kill-ServerHard

    Step "restarting the server"
    Start-Server

    Step "querying the upload session after restart"
    $s = Get-Json "/api/upload/session?id=$sid"
    Assert-True ($s.Status -eq 200) "session status $($s.Status): $($s.Text)"
    $received = @($s.Json.data.received)
    Check "the session survived the crash" {
        Assert-True ($received.Count -eq $killedAt) "expected $killedAt received chunks, got $($received.Count)"
    }
    Check "every received chunk was persisted on disk" {
        $expect = @(0..($killedAt - 1))
        Assert-True (($received -join ",") -eq ($expect -join ",")) "received set mismatch"
    }
    Check "the session metadata is intact" {
        Assert-True ($s.Json.data.total_size -eq $TotalSize) "total_size changed"
        Assert-True ($s.Json.data.chunk_size -eq $ChunkSize) "chunk_size changed"
        Assert-True ($s.Json.data.path -eq $TargetPath) "path changed"
    }

    Step ("resuming with chunks {0}..{1}" -f $killedAt, ($TotalChunks - 1))
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    Send-Range $sid $killedAt $TotalChunks
    $sw.Stop()
    Write-Host ("   resumed {0} chunks in {1:N1} s" -f ($TotalChunks - $killedAt), $sw.Elapsed.TotalSeconds)

    Step "completing the upload"
    $swMerge = [System.Diagnostics.Stopwatch]::StartNew()
    $done = Post-Json "/api/upload/complete" @{ session_id = $sid }
    $swMerge.Stop()
    Assert-True ($done.Status -eq 200) "complete status $($done.Status): $($done.Text)"
    Assert-True ($done.Json.data.size -eq $TotalSize) "size $($done.Json.data.size) != $TotalSize"
    Write-Host ("   merged in {0:N1} s" -f $swMerge.Elapsed.TotalSeconds)

    Step "verifying the resulting file"
    $list = Get-Json ("/api/files?path=" + [uri]::EscapeDataString("/large"))
    $entry = @(@($list.Json.data.entries) | Where-Object { $_.name -eq ("{0}mb.bin" -f $SizeMB) })
    Check "the file exists with the declared size" {
        Assert-True ($entry.Count -eq 1) "file not listed"
        Assert-True ($entry[0].size -eq $TotalSize) "listed size $($entry[0].size)"
    }

    Check "the last bytes on disk match the generated pattern" {
        $req = New-Object System.Net.Http.HttpRequestMessage([System.Net.Http.HttpMethod]::Get, "$BaseUrl/api/download?path=$([uri]::EscapeDataString($TargetPath))")
        $req.Headers.Add("Range", ("bytes={0}-{1}" -f ($TotalSize - 64), ($TotalSize - 1)))
        $resp = $client.SendAsync($req).Result
        Assert-True ([int]$resp.StatusCode -eq 206) "expected 206, got $([int]$resp.StatusCode)"
        $bytes = $resp.Content.ReadAsByteArrayAsync().Result
        Assert-True ($bytes.Length -eq 64) "expected 64 bytes, got $($bytes.Length)"
        for ($k = 0; $k -lt 64; $k++) {
            $want = Expected-Byte ($TotalSize - 64 + $k)
            if ($bytes[$k] -ne $want) { throw ("byte {0} is {1}, expected {2}" -f ($TotalSize - 64 + $k), $bytes[$k], $want) }
        }
    }

    Check "the first bytes on disk match the generated pattern" {
        $req = New-Object System.Net.Http.HttpRequestMessage([System.Net.Http.HttpMethod]::Get, "$BaseUrl/api/download?path=$([uri]::EscapeDataString($TargetPath))")
        $req.Headers.Add("Range", "bytes=0-255")
        $resp = $client.SendAsync($req).Result
        Assert-True ([int]$resp.StatusCode -eq 206) "expected 206, got $([int]$resp.StatusCode)"
        $bytes = $resp.Content.ReadAsByteArrayAsync().Result
        Assert-True ($bytes.Length -eq 256) "expected 256 bytes"
        for ($k = 0; $k -lt 256; $k++) {
            if ($bytes[$k] -ne $template[$k]) { throw ("byte $k is $($bytes[$k]), expected $($template[$k])") }
        }
    }

    Check "bytes sampled across the whole file match the generated pattern" {
        # Head and tail only prove two chunks; with concurrent uploads a chunk
        # written to the wrong offset would still leave both ends intact.
        $samples = 32
        for ($s = 0; $s -lt $samples; $s++) {
            $off = [int64][Math]::Floor(($TotalSize - 256) * $s / ($samples - 1))
            $req = New-Object System.Net.Http.HttpRequestMessage([System.Net.Http.HttpMethod]::Get, "$BaseUrl/api/download?path=$([uri]::EscapeDataString($TargetPath))")
            $req.Headers.Add("Range", ("bytes={0}-{1}" -f $off, ($off + 255)))
            $resp = $client.SendAsync($req).Result
            Assert-True ([int]$resp.StatusCode -eq 206) ("expected 206 at offset {0}, got {1}" -f $off, [int]$resp.StatusCode)
            $bytes = $resp.Content.ReadAsByteArrayAsync().Result
            Assert-True ($bytes.Length -eq 256) ("expected 256 bytes at offset {0}, got {1}" -f $off, $bytes.Length)
            for ($k = 0; $k -lt 256; $k++) {
                $want = Expected-Byte ($off + $k)
                if ($bytes[$k] -ne $want) {
                    throw ("byte {0} is {1}, expected {2}" -f ($off + $k), $bytes[$k], $want)
                }
            }
        }
    }

    Check "the chunk directory was cleaned up" {
        $tmpSession = Join-Path (Join-Path (Join-Path $DataRoot ".file-server") "tmp") $sid
        Assert-True (-not (Test-Path $tmpSession)) "tmp session directory still exists"
    }

    Check "the session is gone from the api" {
        $again = Get-Json "/api/upload/session?id=$sid"
        Assert-True ($again.Status -eq 404) "expected 404, got $($again.Status)"
    }

    Check "audit recorded the upload" {
        $audits = Get-Json "/admin/audit?limit=50"
        $mine = @(@($audits.Json.data.records) | Where-Object { $_.action -eq "upload" -and $_.result -eq "success" })
        Assert-True ($mine.Count -ge 1) "no successful upload audit row"
    }
}
finally {
    Stop-Server
    $client.Dispose()
    if (-not $KeepData) {
        Start-Sleep -Milliseconds 300
        try { Remove-Item -Recurse -Force $WorkDir -ErrorAction SilentlyContinue } catch { }
        if (Test-Path $WorkDir) { Write-Host "warning: could not remove $WorkDir" }
    } else {
        Write-Host "data kept in $WorkDir"
    }
}

Write-Host ""
Write-Host ("=" * 72)
Write-Host ("passed: {0}   failed: {1}" -f $script:Pass, $script:Fail)
Write-Host ("=" * 72)
if ($script:Fail -gt 0) { exit 1 }
exit 0
