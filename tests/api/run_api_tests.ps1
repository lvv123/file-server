<#
    API / integration tests for the file server.

    Boots a real server process against a throw-away data root, drives it over
    HTTP and asserts the documented contract (status codes, JSON envelope,
    Range semantics, audit behaviour, admin local-only rule).

    Usage:
        pwsh tests/api/run_api_tests.ps1
        pwsh tests/api/run_api_tests.ps1 -Only Range
        pwsh tests/api/run_api_tests.ps1 -KeepData          # keep the temp dir
        pwsh tests/api/run_api_tests.ps1 -DiskFullDir D:\  # enable the 507 test
#>
[CmdletBinding()]
param(
    [string]$Exe = "",
    [int]$Port = 0,
    [switch]$KeepData,
    [string]$Only = "",
    [string]$DiskFullDir = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

# Windows PowerShell does not load System.Net.Http by default.
Add-Type -AssemblyName System.Net.Http
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }

# --------------------------------------------------------------------------
# bookkeeping
# --------------------------------------------------------------------------
$script:Pass = 0
$script:Fail = 0
$script:Skip = 0
$script:Failures = New-Object System.Collections.ArrayList
$script:Only = $Only

function Write-Head([string]$text) {
    Write-Host ""
    Write-Host "== $text " ("=" * [Math]::Max(0, 70 - $text.Length)) -ForegroundColor Cyan
}

function Check([string]$name, [scriptblock]$body) {
    if ($script:Only -and $name -notlike "*$script:Only*") { return }
    try {
        & $body
        $script:Pass++
        Write-Host "[ PASS ] $name" -ForegroundColor Green
    } catch {
        $script:Fail++
        $msg = $_.Exception.Message
        [void]$script:Failures.Add("$name :: $msg")
        Write-Host "[ FAIL ] $name" -ForegroundColor Red
        Write-Host "         $msg" -ForegroundColor DarkRed
    }
}

function SkipTest([string]$name, [string]$why) {
    if ($script:Only -and $name -notlike "*$script:Only*") { return }
    $script:Skip++
    Write-Host "[ SKIP ] $name ($why)" -ForegroundColor Yellow
}

function Get-CallLine {
    # [0] Get-CallLine, [1] the Assert-* function, [2] its caller.
    $stack = Get-PSCallStack
    if ($stack.Count -gt 2) { return $stack[2].ScriptLineNumber }
    return 0
}

function Assert-True($cond, [string]$msg) {
    if (-not $cond) { throw "$msg (line $(Get-CallLine))" }
}

function Assert-Eq($actual, $expected, [string]$msg) {
    if ("$actual" -ne "$expected") {
        throw "$msg (expected '$expected', got '$actual') (line $(Get-CallLine))"
    }
}

function Assert-Contains([string]$haystack, [string]$needle, [string]$msg) {
    if ($haystack -notlike "*$needle*") {
        throw "$msg (looking for '$needle' in '$haystack') (line $(Get-CallLine))"
    }
}

# --------------------------------------------------------------------------
# HTTP helpers (HttpClient, so Range / custom headers behave)
# --------------------------------------------------------------------------
function New-ApiClient {
    $handler = New-Object System.Net.Http.HttpClientHandler
    $handler.AllowAutoRedirect = $false
    $handler.UseProxy = $false
    $handler.AutomaticDecompression = [System.Net.DecompressionMethods]::None
    $client = New-Object System.Net.Http.HttpClient($handler)
    $client.Timeout = [TimeSpan]::FromSeconds(180)
    $client.DefaultRequestHeaders.ExpectContinue = $false
    return $client
}

function Invoke-Rest {
    param(
        [string]$Method,
        [string]$Path,
        $Body = $null,
        [hashtable]$Headers = @{},
        [byte[]]$Raw = $null,
        [switch]$NoBody,
        [string]$BaseUrl = ""
    )

    $target = if ($BaseUrl -ne "") { $BaseUrl + $Path } else { $script:Base + $Path }
    $req = New-Object System.Net.Http.HttpRequestMessage(
        [System.Net.Http.HttpMethod]::new($Method), $target)

    if (-not $NoBody) {
        if ($null -ne $Raw) {
            $req.Content = New-Object -TypeName System.Net.Http.ByteArrayContent -ArgumentList (, $Raw)
        } elseif ($null -ne $Body) {
            $json = if ($Body -is [string]) { $Body } else { $Body | ConvertTo-Json -Compress -Depth 10 }
            $req.Content = New-Object System.Net.Http.StringContent(
                $json, [System.Text.Encoding]::UTF8, "application/json")
        }
    }

    foreach ($k in $Headers.Keys) {
        [void]$req.Headers.TryAddWithoutValidation($k, [string]$Headers[$k])
    }
    $req.Headers.TryAddWithoutValidation("User-Agent", "fsrv-api-tests/1.0") | Out-Null

    $resp = $script:Client.SendAsync($req).GetAwaiter().GetResult()
    try {
        $bytes = $resp.Content.ReadAsByteArrayAsync().GetAwaiter().GetResult()
        $text = [System.Text.Encoding]::UTF8.GetString($bytes)
        $json = $null
        if ($text.Length -gt 0) {
            try { $json = $text | ConvertFrom-Json } catch { }
        }
        $out = [pscustomobject]@{
            Status         = [int]$resp.StatusCode
            Headers        = $resp.Headers
            ContentHeaders = $resp.Content.Headers
            Body           = $text
            Bytes          = $bytes
            Json           = $json
        }
        return $out
    } finally {
        $resp.Dispose()
        $req.Dispose()
    }
}

function Get-ResponseHeader($res, [string]$name) {
    $vals = $null
    if ($res.Headers.TryGetValues($name, [ref]$vals)) { return @($vals)[0] }
    if ($res.ContentHeaders.TryGetValues($name, [ref]$vals)) { return @($vals)[0] }
    return $null
}

function Get-Data($res) {
    if ($null -eq $res.Json) { throw "response body is not JSON: $($res.Body)" }
    return $res.Json.data
}

function Assert-Ok($res, [int]$status = 200) {
    Assert-Eq $res.Status $status "unexpected status (body: $($res.Body))"
    Assert-True ($null -ne $res.Json) "body is not JSON: $($res.Body)"
    Assert-True ($res.Json.ok -eq $true) "ok is not true: $($res.Body)"
}

function Assert-Error($res, [int]$status, [string]$code) {
    Assert-Eq $res.Status $status "unexpected status (body: $($res.Body))"
    Assert-True ($null -ne $res.Json) "body is not JSON: $($res.Body)"
    Assert-True ($res.Json.ok -eq $false) "ok is not false: $($res.Body)"
    Assert-Eq $res.Json.error.code $code "unexpected error code"
}

# Raw socket request: needed for "no Content-Length at all", which HttpClient
# refuses to produce.
function Send-RawRequest([string]$text) {
    $tcp = New-Object System.Net.Sockets.TcpClient
    $tcp.Connect("127.0.0.1", $script:Port)
    $stream = $tcp.GetStream()
    $payload = [System.Text.Encoding]::ASCII.GetBytes($text)
    $stream.Write($payload, 0, $payload.Length)
    $stream.Flush()
    # Do NOT half-close the socket: cpp-httplib refuses to write a response once
    # the peer's FIN is visible (SocketStream::is_writable -> is_socket_alive),
    # so a half-closed request would come back empty no matter what the server
    # decided. Reading with a timeout gives the same "no Content-Length" test
    # without tripping that framework behaviour.
    $stream.ReadTimeout = 5000

    $ms = New-Object System.IO.MemoryStream
    $buf = New-Object byte[] 16384
    try {
        while (($n = $stream.Read($buf, 0, $buf.Length)) -gt 0) { $ms.Write($buf, 0, $n) }
    } catch [System.IO.IOException] {
        # read timeout: the server answered but kept the connection open
    }
    $tcp.Close()

    $raw = [System.Text.Encoding]::UTF8.GetString($ms.ToArray())
    $status = 0
    if ($raw -match '^HTTP/1\.[01] (\d{3})') { $status = [int]$Matches[1] }
    return [pscustomobject]@{ Status = $status; Raw = $raw }
}

# --------------------------------------------------------------------------
# byte helpers
# --------------------------------------------------------------------------
function New-PatternBytes([int]$n) {
    $chunk = New-Object byte[] 256
    for ($i = 0; $i -lt 256; $i++) { $chunk[$i] = [byte]$i }
    $data = New-Object byte[] $n
    $off = 0
    while ($off -lt $n) {
        $len = [Math]::Min(256, $n - $off)
        [Array]::Copy($chunk, 0, $data, $off, $len)
        $off += $len
    }
    return , $data
}

function Get-Slice([byte[]]$src, [int]$start, [int]$len) {
    $d = New-Object byte[] $len
    [Array]::Copy($src, $start, $d, 0, $len)
    return , $d
}

function Get-Sha256([byte[]]$data) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        return ([System.BitConverter]::ToString($sha.ComputeHash($data))).Replace("-", "").ToLowerInvariant()
    } finally { $sha.Dispose() }
}

# --------------------------------------------------------------------------
# server lifecycle
# --------------------------------------------------------------------------
function Start-ApiServer {
    $stderr = Join-Path $script:WorkDir "server.err.log"
    $stdout = Join-Path $script:WorkDir "server.out.log"
    $proc = Start-Process -FilePath $script:Exe `
        -ArgumentList @("--config", $script:ConfigPath) `
        -PassThru -WindowStyle Hidden `
        -RedirectStandardError $stderr -RedirectStandardOutput $stdout

    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($proc.HasExited) {
            $log = ""
            if (Test-Path $stderr) { $log = Get-Content $stderr -Raw }
            throw "server exited during startup (code $($proc.ExitCode)): $log"
        }
        try {
            $r = Invoke-Rest -Method GET -Path "/admin/status"
            if ($r.Status -eq 200) { return $proc }
        } catch { }
        Start-Sleep -Milliseconds 200
    }
    throw "server did not become ready within 30s"
}

function Stop-ApiServer($proc) {
    if ($null -eq $proc) { return }
    if (-not $proc.HasExited) {
        $proc.Kill()
        [void]$proc.WaitForExit(10000)
    }
    # Belt and braces: make sure no leftover instance still holds the temp data
    # directory, otherwise the cleanup at the end of the run fails.
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    while ([DateTime]::UtcNow -lt $deadline) {
        $stale = @(Get-CimInstance Win32_Process -Filter "Name='file-server.exe'" -ErrorAction SilentlyContinue |
                   Where-Object { $_.CommandLine -and $_.CommandLine.Contains($script:WorkDir) })
        if ($stale.Count -eq 0) { break }
        foreach ($p in $stale) { Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue }
        Start-Sleep -Milliseconds 200
    }
    Start-Sleep -Milliseconds 300
}

# --------------------------------------------------------------------------
# setup
# --------------------------------------------------------------------------
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
if ($Exe -eq "") { $Exe = Join-Path $repoRoot "build\Debug\file-server.exe" }
if (-not (Test-Path $Exe)) { throw "server binary not found: $Exe (build it first)" }

if ($Port -eq 0) {
    $listener = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, 0)
    $listener.Start()
    $Port = [int]$listener.LocalEndpoint.Port
    $listener.Stop()
}
$script:Port = $Port
$script:Base = "http://127.0.0.1:$Port"

$script:WorkDir = Join-Path ([System.IO.Path]::GetTempPath()) ("fsrv-api-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
$dataDir = Join-Path $script:WorkDir "data"
$filesDir = Join-Path $dataDir "files"
New-Item -ItemType Directory -Path $dataDir -Force | Out-Null

$script:ConfigPath = Join-Path $script:WorkDir "config.json"
$config = [ordered]@{
    port                 = $Port
    bind_address         = "0.0.0.0"
    data_root            = $dataDir
    chunk_size_mb        = 1
    session_ttl_hours    = 24
    audit_retention_days = 30
    audit_max_size_mb    = 64
    admin_allow_ips      = @("127.0.0.1")
}
($config | ConvertTo-Json -Depth 5) | Set-Content -Path $script:ConfigPath -Encoding UTF8

$script:Client = New-ApiClient
$server = $null

Write-Host "server   : $Exe"
Write-Host "port     : $Port"
Write-Host "data dir : $dataDir"

# --------------------------------------------------------------------------
# fixtures
# --------------------------------------------------------------------------
$CHUNK = 1MB
$TEST_SIZE = 5MB + 12345          # last chunk is partial
$TEST_CHUNKS = [int][Math]::Ceiling($TEST_SIZE / $CHUNK)
$script:BigData = New-PatternBytes $TEST_SIZE
$script:BigSha = Get-Sha256 $script:BigData

$ORIGIN = "/api/upload/init"
$CHUNK_URL = "/api/upload/chunk"

function New-Upload([string]$path, [long]$size) {
    $r = Invoke-Rest -Method POST -Path $ORIGIN -Body @{ path = $path; total_size = $size }
    Assert-Ok $r 200
    return (Get-Data $r)
}

function Send-Chunk([string]$sid, [int]$index, [byte[]]$payload) {
    return Invoke-Rest -Method PUT -Path $CHUNK_URL -Raw $payload -Headers @{
        "X-Session-Id" = $sid
        "X-Chunk-Index" = [string]$index
    }
}

function Upload-Bytes([string]$path, [byte[]]$data, [int]$chunkSize = $CHUNK, [int]$fromChunk = 0, [int]$toChunk = -1) {
    $init = New-Upload $path $data.Length
    $sid = $init.session_id
    $count = [int]$init.total_chunks
    if ($toChunk -lt 0) { $toChunk = $count - 1 }
    for ($i = $fromChunk; $i -le $toChunk; $i++) {
        $start = $i * $chunkSize
        $len = [Math]::Min($chunkSize, $data.Length - $start)
        $slice = Get-Slice $data $start $len
        $r = Send-Chunk $sid $i $slice
        Assert-Ok $r 200
    }
    # Landing the file is part of the helper's contract: callers assert on the
    # file that appears on disk, not on the upload session.
    $r = Complete-Upload $sid
    Assert-Ok $r 200
    return $sid
}

function Complete-Upload([string]$sid) {
    return Invoke-Rest -Method POST -Path "/api/upload/complete" -Body @{ session_id = $sid }
}

function Get-ServerFile([string]$rel) {
    return (Join-Path $filesDir ($rel -replace '/', '\'))
}

function Read-ServerFile([string]$rel) {
    return [System.IO.File]::ReadAllBytes((Get-ServerFile $rel))
}

# ==========================================================================
try {
    $server = Start-ApiServer

    # ---------------------------------------------------------------- IT-A
    Write-Head "IT-A file and directory management"

    Check "IT-A1 list root returns an entries array" {
        $r = Invoke-Rest -Method GET -Path "/api/files?path=/"
        Assert-Ok $r 200
        $d = Get-Data $r
        Assert-Eq $d.path "/" "root path echo"
        Assert-True ($d.entries -is [array] -or $null -eq $d.entries) "entries is not an array"
    }

    Check "IT-A2 mkdir creates nested directories" {
        $r = Invoke-Rest -Method POST -Path "/api/mkdir" -Body @{ path = "/docs/a/b" }
        Assert-Ok $r 200
        Assert-Eq (Get-Data $r).created $true "created flag"
        Assert-True (Test-Path (Get-ServerFile "docs/a/b")) "directory missing on disk"
    }

    Check "IT-A3 mkdir on an existing directory is idempotent" {
        $r = Invoke-Rest -Method POST -Path "/api/mkdir" -Body @{ path = "/docs/a/b" }
        Assert-Ok $r 200
        Assert-Eq (Get-Data $r).created $false "created flag should be false"
    }

    Check "IT-A4 list shows created directories with metadata" {
        $r = Invoke-Rest -Method GET -Path "/api/files?path=/docs"
        Assert-Ok $r 200
        $entries = @((Get-Data $r).entries)
        Assert-Eq $entries.Count 1 "entry count"
        Assert-Eq $entries[0].name "a" "entry name"
        Assert-Eq $entries[0].is_dir $true "is_dir flag"
        Assert-True ($entries[0].mtime -gt 0) "mtime missing"
    }

    Check "IT-A5 rename a file" {
        $r = Invoke-Rest -Method POST -Path "/api/mkdir" -Body @{ path = "/docs" }
        Assert-Ok $r 200
        $null = Upload-Bytes "/tmp-rename.txt" ([System.Text.Encoding]::UTF8.GetBytes("hello"))
        $r = Invoke-Rest -Method POST -Path "/api/rename" -Body @{ from = "/tmp-rename.txt"; to = "/docs/renamed.txt" }
        Assert-Ok $r 200
        Assert-True (Test-Path (Get-ServerFile "docs/renamed.txt")) "renamed file missing"
        Assert-True (-not (Test-Path (Get-ServerFile "tmp-rename.txt"))) "source still present"
    }

    Check "IT-A6 rename onto an existing target is rejected with 409" {
        $null = Upload-Bytes "/docs/conflict-a.txt" ([System.Text.Encoding]::UTF8.GetBytes("a"))
        $null = Upload-Bytes "/docs/conflict-b.txt" ([System.Text.Encoding]::UTF8.GetBytes("b"))
        $r = Invoke-Rest -Method POST -Path "/api/rename" -Body @{ from = "/docs/conflict-a.txt"; to = "/docs/conflict-b.txt" }
        Assert-Error $r 409 "TARGET_EXISTS"
    }

    Check "IT-A7 rename a directory moves its children in the index" {
        $r = Invoke-Rest -Method POST -Path "/api/mkdir" -Body @{ path = "/movable/sub" }
        Assert-Ok $r 200
        $null = Upload-Bytes "/movable/sub/child.txt" ([System.Text.Encoding]::UTF8.GetBytes("child"))
        $r = Invoke-Rest -Method POST -Path "/api/rename" -Body @{ from = "/movable"; to = "/moved" }
        Assert-Ok $r 200
        $r = Invoke-Rest -Method GET -Path "/api/files?path=/moved/sub"
        Assert-Ok $r 200
        Assert-Eq (@((Get-Data $r).entries))[0].name "child.txt" "child not visible under the new path"
        $r = Invoke-Rest -Method GET -Path "/api/search?q=child"
        Assert-Ok $r 200
        Assert-Eq (Get-Data $r).results[0].path "/moved/sub/child.txt" "search index not updated after rename"
    }

    Check "IT-A8 deleting a non-empty directory is rejected with 409" {
        $r = Invoke-Rest -Method DELETE -Path "/api/files?path=/docs"
        Assert-Error $r 409 "DIRECTORY_NOT_EMPTY"
    }

    Check "IT-A9 deleting a file removes it from disk and index" {
        $r = Invoke-Rest -Method DELETE -Path "/api/files?path=/moved/sub/child.txt"
        Assert-Ok $r 200
        Assert-True (-not (Test-Path (Get-ServerFile "moved/sub/child.txt"))) "file still on disk"
        $r = Invoke-Rest -Method GET -Path "/api/search?q=child"
        Assert-Ok $r 200
        Assert-Eq @((Get-Data $r).results).Count 0 "index still has the deleted file"
    }

    Check "IT-A10 deleting a missing path returns 404" {
        $r = Invoke-Rest -Method DELETE -Path "/api/files?path=/nope/nothing.txt"
        Assert-Error $r 404 "NOT_FOUND"
    }

    Check "IT-A11 deleting an empty directory succeeds" {
        $r = Invoke-Rest -Method POST -Path "/api/mkdir" -Body @{ path = "/empty-dir" }
        Assert-Ok $r 200
        $r = Invoke-Rest -Method DELETE -Path "/api/files?path=/empty-dir"
        Assert-Ok $r 200
        Assert-True (-not (Test-Path (Get-ServerFile "empty-dir"))) "directory still on disk"
    }

    Check "IT-A12 resync indexes files copied in behind the server's back" {
        $dropped = Get-ServerFile "dropped/late.txt"
        New-Item -ItemType Directory -Path (Split-Path $dropped) -Force | Out-Null
        [System.IO.File]::WriteAllText($dropped, "hand copied")

        # Not visible before the rescan: the index only learns about it on demand.
        $r = Invoke-Rest -Method GET -Path "/api/files?path=/dropped"
        Assert-Ok $r 200
        Assert-Eq (@((Get-Data $r).entries).Length) 0 "the file appeared without a rescan"

        $r = Invoke-Rest -Method GET -Path "/api/resync"
        Assert-Ok $r 200
        $d = Get-Data $r
        Assert-True ($d.scanned -ge 1) "scanned should count the new entries"

        $r = Invoke-Rest -Method GET -Path "/api/files?path=/dropped"
        Assert-Ok $r 200
        $entries = @((Get-Data $r).entries)
        Assert-Eq $entries.Length 1 "resync did not pick the file up"
        Assert-Eq $entries[0].name "late.txt" "wrong entry"
        Assert-Eq $entries[0].size 11 "size should come from disk"
    }

    Check "IT-A13 resync drops entries whose file is gone" {
        Remove-Item (Split-Path (Get-ServerFile "dropped/late.txt")) -Recurse -Force
        $r = Invoke-Rest -Method GET -Path "/api/resync"
        Assert-Ok $r 200
        Assert-True ((Get-Data $r).removed -ge 2) "expected the file and its directory to be dropped"

        $r = Invoke-Rest -Method GET -Path "/api/files?path=/dropped"
        Assert-Error $r 404 "DIR_NOT_FOUND"
    }

    # ---------------------------------------------------------------- IT-B
    Write-Head "IT-B chunked upload"

    $script:UploadSid = $null
    $script:UploadInit = $null

    Check "IT-B1 init returns chunk geometry" {
        $d = New-Upload "/big/pattern.bin" $TEST_SIZE
        $script:UploadSid = $d.session_id
        $script:UploadInit = $d
        Assert-Eq $d.chunk_size $CHUNK "chunk_size"
        Assert-Eq $d.total_chunks $TEST_CHUNKS "total_chunks"
        Assert-Eq $d.total_size $TEST_SIZE "total_size"
        Assert-True ($null -ne $d.session_id) "session_id missing"
        Assert-True ($null -ne $d.expires_at) "expires_at missing"
    }

    Check "IT-B2 every chunk is accepted" {
        for ($i = 0; $i -lt $TEST_CHUNKS; $i++) {
            $start = $i * $CHUNK
            $len = [Math]::Min($CHUNK, $TEST_SIZE - $start)
            $r = Send-Chunk $script:UploadSid $i (Get-Slice $script:BigData $start $len)
            Assert-Ok $r 200
            $d = Get-Data $r
            Assert-Eq $d.index $i "index echo"
            Assert-Eq $d.received ($i + 1) "received count"
        }
    }

    Check "IT-B3 session status reports the received chunk indexes" {
        $r = Invoke-Rest -Method GET -Path "/api/upload/session?id=$($script:UploadSid)"
        Assert-Ok $r 200
        $d = Get-Data $r
        Assert-Eq @($d.received).Count $TEST_CHUNKS "received count"
        Assert-Eq (@($d.received) -join ",") ((0..($TEST_CHUNKS - 1)) -join ",") "received indexes"
        Assert-Eq $d.path "/big/pattern.bin" "session path"
    }

    Check "IT-B4 complete merges the chunks byte-for-byte" {
        $r = Complete-Upload $script:UploadSid
        Assert-Ok $r 200
        $d = Get-Data $r
        Assert-Eq $d.size $TEST_SIZE "reported size"
        Assert-Eq $d.path "/big/pattern.bin" "reported path"
        $disk = Read-ServerFile "big/pattern.bin"
        Assert-Eq $disk.Length $TEST_SIZE "size on disk"
        Assert-Eq (Get-Sha256 $disk) $script:BigSha "content hash"
    }

    Check "IT-B5 completing with missing chunks returns 409" {
        $init = New-Upload "/partial.bin" $TEST_SIZE
        $r = Send-Chunk $init.session_id 0 (Get-Slice $script:BigData 0 $CHUNK)
        Assert-Ok $r 200
        $r = Complete-Upload $init.session_id
        Assert-Error $r 409 "UPLOAD_INCOMPLETE"
        Assert-True (-not (Test-Path (Get-ServerFile "partial.bin"))) "incomplete upload must not land"
        $r2 = Invoke-Rest -Method GET -Path "/api/upload/session?id=$($init.session_id)"
        Assert-Ok $r2 200
        Assert-Eq (@(Get-Data $r2).received -join ",") "0" "session must survive a failed complete"
    }

    Check "IT-B6 an out-of-range chunk index returns 400" {
        $init = New-Upload "/oob.bin" 10
        $r = Send-Chunk $init.session_id 5 ([byte[]](1, 2, 3))
        Assert-Error $r 400 "CHUNK_INDEX_OUT_OF_RANGE"
    }

    Check "IT-B7 a chunk whose length contradicts the declaration returns 413" {
        $init = New-Upload "/mismatch.bin" 100
        $r = Send-Chunk $init.session_id 0 ([byte[]]::new(50))
        Assert-Error $r 413 "CHUNK_SIZE_MISMATCH"
        $r = Invoke-Rest -Method GET -Path "/api/upload/session?id=$($init.session_id)"
        Assert-Ok $r 200
        Assert-Eq @((Get-Data $r).received).Count 0 "rejected chunk must not be recorded"
    }

    Check "IT-B8 a chunk without Content-Length returns 411" {
        $init = New-Upload "/nolength.bin" 100
        $req = "PUT /api/upload/chunk HTTP/1.1`r`n" +
               "Host: 127.0.0.1:$($script:Port)`r`n" +
               "X-Session-Id: $($init.session_id)`r`n" +
               "X-Chunk-Index: 0`r`n" +
               "Connection: close`r`n`r`n"
        $raw = Send-RawRequest $req
        Assert-Eq $raw.Status 411 "status"
        Assert-Contains $raw.Raw "LENGTH_REQUIRED" "error code in body"
    }

    Check "IT-B9 an unknown session id returns 404" {
        $r = Send-Chunk "00000000-0000-4000-8000-000000000000" 0 ([byte[]](1, 2, 3))
        Assert-Error $r 404 "SESSION_NOT_FOUND"
    }

    Check "IT-B10 a zero-byte file uploads and lands empty" {
        $init = New-Upload "/zero.bin" 0
        Assert-Eq $init.total_chunks 0 "total_chunks for an empty file"
        $r = Complete-Upload $init.session_id
        Assert-Ok $r 200
        Assert-Eq (Get-Data $r).size 0 "reported size"
        Assert-True (Test-Path (Get-ServerFile "zero.bin")) "empty file missing"
        Assert-Eq (Get-Item (Get-ServerFile "zero.bin")).Length 0 "size on disk"
    }

    Check "IT-B11 re-uploading the same path overwrites atomically" {
        $null = Upload-Bytes "/docs/overwrite.txt" ([System.Text.Encoding]::UTF8.GetBytes("first"))
        $null = Upload-Bytes "/docs/overwrite.txt" ([System.Text.Encoding]::UTF8.GetBytes("second version"))
        $text = [System.Text.Encoding]::UTF8.GetString((Read-ServerFile "docs/overwrite.txt"))
        Assert-Eq $text "second version" "content was not replaced"
    }

    Check "IT-B12 init with a traversing target is rejected with 403" {
        $r = Invoke-Rest -Method POST -Path "/api/upload/init" -Body @{ path = "/../escape.bin"; total_size = 10 }
        Assert-Error $r 403 "PATH_TRAVERSAL"
        Assert-True (-not (Test-Path (Join-Path $dataDir "..\escape.bin"))) "escape.bin leaked outside the data root"
    }

    Check "IT-B13 init with a negative size returns 400" {
        $r = Invoke-Rest -Method POST -Path "/api/upload/init" -Body @{ path = "/neg.bin"; total_size = -5 }
        Assert-Error $r 400 "INVALID_TOTAL_SIZE"
    }

    # ---------------------------------------------------------------- IT-C
    Write-Head "IT-C resume across a server restart"

    Check "IT-C1 an interrupted upload resumes after a restart" {
        $init = New-Upload "/resume.bin" $TEST_SIZE
        $sid = $init.session_id
        for ($i = 0; $i -lt 2; $i++) {
            $r = Send-Chunk $sid $i (Get-Slice $script:BigData ($i * $CHUNK) $CHUNK)
            Assert-Ok $r 200
        }

        Stop-ApiServer $server
        $server = Start-ApiServer

        $r = Invoke-Rest -Method GET -Path "/api/upload/session?id=$sid"
        Assert-Ok $r 200
        Assert-Eq (@((Get-Data $r).received) -join ",") "0,1" "received chunks after restart"

        for ($i = 2; $i -lt $TEST_CHUNKS; $i++) {
            $start = $i * $CHUNK
            $len = [Math]::Min($CHUNK, $TEST_SIZE - $start)
            $r = Send-Chunk $sid $i (Get-Slice $script:BigData $start $len)
            Assert-Ok $r 200
        }
        $r = Complete-Upload $sid
        Assert-Ok $r 200
        Assert-Eq (Get-Sha256 (Read-ServerFile "resume.bin")) $script:BigSha "content hash after resume"
    }

    # ---------------------------------------------------------------- IT-D
    Write-Head "IT-D download and Range"

    Check "IT-D1 a full download returns 200 with the right headers" {
        $r = Invoke-Rest -Method GET -Path "/api/download?path=/big/pattern.bin"
        Assert-Eq $r.Status 200 "status"
        Assert-Eq $r.Bytes.Length $TEST_SIZE "body length"
        Assert-Eq (Get-ResponseHeader $r "Content-Length") "$TEST_SIZE" "Content-Length"
        Assert-Eq (Get-ResponseHeader $r "Accept-Ranges") "bytes" "Accept-Ranges"
        Assert-Contains (Get-ResponseHeader $r "Content-Disposition") "attachment" "disposition"
        Assert-Eq (Get-Sha256 $r.Bytes) $script:BigSha "body hash"
    }

    Check "IT-D2 bytes=0-99 returns 206 with one exact range" {
        $r = Invoke-Rest -Method GET -Path "/api/download?path=/big/pattern.bin" -Headers @{ Range = "bytes=0-99" }
        Assert-Eq $r.Status 206 "status"
        Assert-Eq (Get-ResponseHeader $r "Content-Range") "bytes 0-99/$TEST_SIZE" "Content-Range"
        Assert-Eq $r.Bytes.Length 100 "body length"
        Assert-Eq (Get-Sha256 $r.Bytes) (Get-Sha256 (Get-Slice $script:BigData 0 100)) "body content"
    }

    Check "IT-D3 a suffix range returns 206 with the tail bytes" {
        $r = Invoke-Rest -Method GET -Path "/api/download?path=/big/pattern.bin" -Headers @{ Range = "bytes=-100" }
        Assert-Eq $r.Status 206 "status"
        Assert-Eq (Get-ResponseHeader $r "Content-Range") "bytes $($TEST_SIZE - 100)-$($TEST_SIZE - 1)/$TEST_SIZE" "Content-Range"
        Assert-Eq (Get-Sha256 $r.Bytes) (Get-Sha256 (Get-Slice $script:BigData ($TEST_SIZE - 100) 100)) "body content"
    }

    Check "IT-D4 an open-ended range returns 206 to EOF" {
        $r = Invoke-Rest -Method GET -Path "/api/download?path=/big/pattern.bin" -Headers @{ Range = "bytes=1048570-" }
        Assert-Eq $r.Status 206 "status"
        Assert-Eq $r.Bytes.Length ($TEST_SIZE - 1048570) "body length"
        Assert-Eq (Get-ResponseHeader $r "Content-Range") "bytes 1048570-$($TEST_SIZE - 1)/$TEST_SIZE" "Content-Range"
    }

    Check "IT-D5 a multi-range request returns 416" {
        $r = Invoke-Rest -Method GET -Path "/api/download?path=/big/pattern.bin" -Headers @{ Range = "bytes=0-1,5-6" }
        Assert-Eq $r.Status 416 "status"
        Assert-Eq (Get-ResponseHeader $r "Content-Range") "bytes */$TEST_SIZE" "Content-Range"
    }

    Check "IT-D6 a range beyond EOF returns 416" {
        $r = Invoke-Rest -Method GET -Path "/api/download?path=/big/pattern.bin" -Headers @{ Range = "bytes=$($TEST_SIZE + 10)-" }
        Assert-Eq $r.Status 416 "status"
        Assert-Eq (Get-ResponseHeader $r "Content-Range") "bytes */$TEST_SIZE" "Content-Range"
    }

    Check "IT-D7 downloading a directory returns 400" {
        $r = Invoke-Rest -Method GET -Path "/api/download?path=/docs"
        Assert-Error $r 400 "TARGET_IS_DIRECTORY"
    }

    Check "IT-D8 downloading a missing file returns 404" {
        $r = Invoke-Rest -Method GET -Path "/api/download?path=/docs/ghost.bin"
        Assert-Error $r 404 "FILE_NOT_FOUND"
    }

    Check "IT-D9 downloading through a traversal returns 403" {
        $r = Invoke-Rest -Method GET -Path "/api/download?path=/../../Windows/win.ini"
        Assert-Error $r 403 "PATH_TRAVERSAL"
    }

    # ---------------------------------------------------------------- IT-E
    Write-Head "IT-E search"

    Check "IT-E1 search finds a file by substring" {
        $r = Invoke-Rest -Method GET -Path "/api/search?q=pattern"
        Assert-Ok $r 200
        $paths = @((Get-Data $r).results) | ForEach-Object { $_.path }
        Assert-True ($paths -contains "/big/pattern.bin") "pattern.bin not found: $($paths -join ', ')"
    }

    Check "IT-E2 search is case insensitive" {
        $r = Invoke-Rest -Method GET -Path "/api/search?q=PATTERN"
        Assert-Ok $r 200
        Assert-True (@((Get-Data $r).results).Count -ge 1) "uppercase query found nothing"
    }

    Check "IT-E3 search never returns directories" {
        $r = Invoke-Rest -Method GET -Path "/api/search?q=docs"
        Assert-Ok $r 200
        foreach ($hit in @((Get-Data $r).results)) {
            Assert-True (-not $hit.name.Contains("docs") -or $hit.name -ne "docs") "directory leaked into results"
        }
    }

    Check "IT-E4 an empty query returns no results" {
        $r = Invoke-Rest -Method GET -Path "/api/search?q=%20"
        Assert-Ok $r 200
        Assert-Eq @((Get-Data $r).results).Count 0 "empty query must not dump the index"
    }

    Check "IT-E5 the limit parameter is honoured" {
        $r = Invoke-Rest -Method GET -Path "/api/search?q=.bin&limit=1"
        Assert-Ok $r 200
        Assert-True (@((Get-Data $r).results).Count -le 1) "limit ignored"
        Assert-Eq (Get-Data $r).limit 1 "limit echo"
    }

    Check "IT-E6 LIKE wildcards are matched literally" {
        $r = Invoke-Rest -Method GET -Path "/api/search?q=%25"
        Assert-Ok $r 200
        Assert-Eq @((Get-Data $r).results).Count 0 "percent sign must not behave as a wildcard"
    }

    # ---------------------------------------------------------------- IT-F
    Write-Head "IT-F path traversal"

    Check "IT-F1 traversal attempts are refused" {
        $attempts = @(
            "/../../Windows/win.ini",
            "/..%2F..%2FWindows%2Fwin.ini",
            "/..%5C..%5CWindows%5Cwin.ini",
            "/docs/../../etc/passwd",
            "%2e%2e%2f%2e%2e%2fsecret.txt",
            "..\..\windows\win.ini"
        )
        foreach ($a in $attempts) {
            $r = Invoke-Rest -Method GET -Path "/api/files?path=$a"
            Assert-True ($r.Status -eq 403 -or $r.Status -eq 400) "expected 403/400 for '$a', got $($r.Status)"
        }
    }

    Check "IT-F2 an absolute windows path is refused" {
        $r = Invoke-Rest -Method GET -Path "/api/files?path=C:%5CWindows"
        Assert-True ($r.Status -eq 400 -or $r.Status -eq 403) "expected 403/400, got $($r.Status)"
    }

    Check "IT-F3 a NUL byte in the path is refused" {
        $r = Invoke-Rest -Method GET -Path "/api/files?path=/a%00b"
        Assert-True ($r.Status -eq 400 -or $r.Status -eq 403) "expected 403/400, got $($r.Status)"
    }

    Check "IT-F4 nothing escaped the data root" {
        $escaped = Get-ChildItem -Path (Join-Path $dataDir "..") -Filter "*.bin" -ErrorAction SilentlyContinue
        Assert-True ($null -eq $escaped) "files leaked outside files/"
    }

    # ---------------------------------------------------------------- IT-G
    Write-Head "IT-G audit log"

    Check "IT-G1 audit records every business action" {
        $r = Invoke-Rest -Method GET -Path "/admin/audit?limit=500"
        Assert-Ok $r 200
        $actions = @((Get-Data $r).records) | ForEach-Object { $_.action }
        foreach ($expected in @("upload_init", "upload", "download", "delete", "rename", "mkdir", "search", "list")) {
            Assert-True ($actions -contains $expected) "no audit record for action '$expected'"
        }
    }

    Check "IT-G2 audit records carry the documented fields" {
        $r = Invoke-Rest -Method GET -Path "/admin/audit?limit=5"
        Assert-Ok $r 200
        $rec = @((Get-Data $r).records)[0]
        foreach ($f in @("id", "time", "ts", "client_ip", "action", "path", "result", "duration_ms", "user_agent")) {
            Assert-True ($null -ne $rec.PSObject.Properties[$f]) "field '$f' missing"
        }
        Assert-True ($rec.ts -gt 0) "ts missing"
        Assert-True ($rec.duration_ms -ge 0) "duration_ms invalid"
        Assert-Contains $rec.user_agent "fsrv-api-tests" "user agent not recorded"
    }

    Check "IT-G3 failed operations are audited with result=fail" {
        $r = Invoke-Rest -Method DELETE -Path "/api/files?path=/definitely-missing.txt"
        Assert-Eq $r.Status 404 "status"
        $r = Invoke-Rest -Method GET -Path "/admin/audit?limit=20"
        Assert-Ok $r 200
        $failed = @(@((Get-Data $r).records) | Where-Object { $_.result -eq "fail" -and $_.path -like "*definitely-missing*" })
        Assert-True ($failed.Count -ge 1) "failed delete was not audited"
        Assert-True ($null -ne $failed[0].error -and $failed[0].error.Length -gt 0) "error message missing"
    }

    Check "IT-G4 fifty concurrent writes produce fifty audit rows" {
        $before = (Get-Data (Invoke-Rest -Method GET -Path "/admin/audit?limit=1")).total
        $tasks = New-Object System.Collections.ArrayList
        for ($i = 0; $i -lt 50; $i++) {
            $content = New-Object System.Net.Http.StringContent(
                (@{ path = "/concurrent/dir$i" } | ConvertTo-Json -Compress),
                [System.Text.Encoding]::UTF8, "application/json")
            $req = New-Object System.Net.Http.HttpRequestMessage(
                [System.Net.Http.HttpMethod]::Post, "$($script:Base)/api/mkdir")
            $req.Content = $content
            [void]$tasks.Add($script:Client.SendAsync($req))
        }
        [System.Threading.Tasks.Task]::WaitAll([System.Threading.Tasks.Task[]]$tasks.ToArray())

        $statuses = @()
        foreach ($t in $tasks) {
            $resp = $t.GetAwaiter().GetResult()
            $statuses += [int]$resp.StatusCode
            $resp.Dispose()
        }
        $bad = @($statuses | Where-Object { $_ -ne 200 })
        Assert-Eq $bad.Count 0 "non-200 responses: $($bad -join ',')"

        $after = (Get-Data (Invoke-Rest -Method GET -Path "/admin/audit?limit=1")).total
        Assert-True (($after - $before) -ge 50) "audit rows did not grow by 50 (before=$before after=$after)"

        $r = Invoke-Rest -Method GET -Path "/admin/audit?limit=500"
        Assert-Ok $r 200
        $mine = @(@((Get-Data $r).records) | Where-Object { $_.action -eq "mkdir" -and $_.path -like "/concurrent/dir*" })
        Assert-Eq $mine.Count 50 "concurrent mkdir audit rows"
        foreach ($rec in $mine) { Assert-Eq $rec.result "success" "unexpected result for $($rec.path)" }
    }

    # ---------------------------------------------------------------- IT-H
    Write-Head "IT-H admin API (local only)"

    Check "IT-H1 status reports disk, counts and uptime" {
        $r = Invoke-Rest -Method GET -Path "/admin/status"
        Assert-Ok $r 200
        $d = Get-Data $r
        Assert-True ($d.version.Length -gt 0) "version missing"
        Assert-True ($d.disk.total -gt 0) "disk total missing"
        Assert-True ($d.disk.free -ge 0) "disk free missing"
        Assert-True ($d.file_count -ge 1) "file_count missing"
        Assert-True ($null -ne $d.active_sessions) "active_sessions missing"
        Assert-True ($null -ne $d.recent_audits) "recent_audits missing"
        Assert-True ($d.uptime_seconds -ge 0) "uptime_seconds missing"
    }

    Check "IT-H2 audit query paginates newest first" {
        $r = Invoke-Rest -Method GET -Path "/admin/audit?limit=10"
        Assert-Ok $r 200
        $records = @((Get-Data $r).records)
        Assert-True ($records.Count -le 10) "limit ignored"
        for ($i = 1; $i -lt $records.Count; $i++) {
            Assert-True ($records[$i - 1].id -gt $records[$i].id) "records are not newest-first"
        }
    }

    Check "IT-H3 admin delete removes a file" {
        $null = Upload-Bytes "/admin-delete-me.txt" ([System.Text.Encoding]::UTF8.GetBytes("x"))
        $r = Invoke-Rest -Method DELETE -Path "/admin/files?path=/admin-delete-me.txt"
        Assert-Ok $r 200
        Assert-True (-not (Test-Path (Get-ServerFile "admin-delete-me.txt"))) "file still on disk"
        $r = Invoke-Rest -Method GET -Path "/admin/audit?limit=10"
        Assert-True ((@(@((Get-Data $r).records) | Where-Object { $_.action -eq "admin_delete" })).Count -ge 1) "admin_delete not audited"
    }

    $lanIp = $null
    try {
        $lanIp = (Get-NetIPAddress -AddressFamily IPv4 -ErrorAction Stop |
                  Where-Object { $_.IPAddress -notlike "127.*" -and $_.IPAddress -notlike "169.254.*" } |
                  Select-Object -First 1).IPAddress
    } catch { $lanIp = $null }

    if ($lanIp) {
        Check "IT-H4 the admin API is refused for a non-loopback peer" {
            $r = Invoke-Rest -Method GET -Path "/admin/status" -BaseUrl "http://${lanIp}:$($script:Port)"
            Assert-Eq $r.Status 403 "status for peer $lanIp"
            Assert-Eq $r.Json.error.code "ADMIN_FORBIDDEN" "error code"
        }

        Check "IT-H5 a spoofed X-Forwarded-For cannot unlock the admin API" {
            $r = Invoke-Rest -Method GET -Path "/admin/status" -BaseUrl "http://${lanIp}:$($script:Port)" `
                -Headers @{ "X-Forwarded-For" = "127.0.0.1" }
            Assert-Eq $r.Status 403 "spoofed X-Forwarded-For must be ignored"
        }
    } else {
        SkipTest "IT-H4 the admin API is refused for a non-loopback peer" "no LAN IPv4 address on this host"
        SkipTest "IT-H5 a spoofed X-Forwarded-For cannot unlock the admin API" "no LAN IPv4 address on this host"
    }

    Check "IT-H6 the admin API is allowed from loopback" {
        $r = Invoke-Rest -Method GET -Path "/admin/status"
        Assert-Eq $r.Status 200 "loopback must be allowed"
    }

    # ---------------------------------------------------------------- IT-P
    Write-Head "IT-P protocol and error envelope"

    Check "IT-P1 an unknown api endpoint returns JSON 404" {
        $r = Invoke-Rest -Method GET -Path "/api/does-not-exist"
        Assert-Eq $r.Status 404 "status"
        Assert-True ($null -ne $r.Json) "404 body is not JSON"
        Assert-Eq $r.Json.ok $false "ok flag"
        Assert-Eq $r.Json.error.code "ENDPOINT_NOT_FOUND" "error code"
    }

    Check "IT-P2 malformed JSON returns 400 INVALID_JSON" {
        $r = Invoke-Rest -Method POST -Path "/api/mkdir" -Body "{not json"
        Assert-Error $r 400 "INVALID_JSON"
    }

    Check "IT-P3 a missing field returns 400 MISSING_FIELD" {
        $r = Invoke-Rest -Method POST -Path "/api/mkdir" -Body @{ other = "x" }
        Assert-Error $r 400 "MISSING_FIELD"
    }

    Check "IT-P4 wrong field type returns 400" {
        $r = Invoke-Rest -Method POST -Path "/api/upload/init" -Body @{ path = "/x.bin"; total_size = "ten" }
        Assert-True ($r.Status -eq 400) "expected 400, got $($r.Status)"
    }

    Check "IT-P5 an oversized JSON body returns 413" {
        $big = New-Object byte[] (9MB)
        $r = Invoke-Rest -Method POST -Path "/api/mkdir" -Raw $big
        Assert-Eq $r.Status 413 "status"
    }

    Check "IT-P6 json responses are typed as json" {
        $r = Invoke-Rest -Method GET -Path "/api/files?path=/"
        $ct = Get-ResponseHeader $r "Content-Type"
        Assert-Contains $ct "application/json" "Content-Type"
    }

    Check "IT-P7 the embedded web ui is served" {
        $r = Invoke-Rest -Method GET -Path "/"
        Assert-Eq $r.Status 200 "status"
        Assert-Contains (Get-ResponseHeader $r "Content-Type") "text/html" "Content-Type"
        Assert-Contains $r.Body "<!DOCTYPE html" "html body"
    }

    Check "IT-P8 unknown static paths fall back to 404 JSON" {
        $r = Invoke-Rest -Method GET -Path "/nope/nothing.js"
        Assert-Eq $r.Status 404 "status"
    }

    # ---------------------------------------- concurrent replace while reading
    Write-Head "IT-R replacing or deleting a file that is being downloaded"

    Check "IT-R1 overwriting a file while it is being downloaded succeeds" {
        $data = New-PatternBytes (3 * 1024 * 1024)
        Upload-Bytes "/live/held.bin" $data | Out-Null

        # Hold the file open: send the request and never read the body, so the
        # server blocks writing and keeps its handle on the file.
        $tcp = New-Object System.Net.Sockets.TcpClient
        $tcp.Connect("127.0.0.1", $script:Port)
        $stream = $tcp.GetStream()
        $req = "GET /api/download?path=/live/held.bin HTTP/1.1`r`nHost: 127.0.0.1`r`nConnection: close`r`n`r`n"
        $bytes = [System.Text.Encoding]::ASCII.GetBytes($req)
        try {
            $stream.Write($bytes, 0, $bytes.Length)
            $stream.Flush()
            Start-Sleep -Milliseconds 300

            $replacement = New-PatternBytes (64 * 1024)
            Upload-Bytes "/live/held.bin" $replacement | Out-Null
        } finally {
            $tcp.Close()
        }

        $back = Read-ServerFile "/live/held.bin"
        Assert-Eq $back.Length $replacement.Length "size after overwrite"
    }

    Check "IT-R2 deleting a file while it is being downloaded succeeds" {
        $data = New-PatternBytes (3 * 1024 * 1024)
        Upload-Bytes "/live2/doomed.bin" $data | Out-Null

        $tcp = New-Object System.Net.Sockets.TcpClient
        $tcp.Connect("127.0.0.1", $script:Port)
        $stream = $tcp.GetStream()
        $req = "GET /api/download?path=/live2/doomed.bin HTTP/1.1`r`nHost: 127.0.0.1`r`nConnection: close`r`n`r`n"
        $bytes = [System.Text.Encoding]::ASCII.GetBytes($req)
        try {
            $stream.Write($bytes, 0, $bytes.Length)
            $stream.Flush()
            Start-Sleep -Milliseconds 300

            $r = Invoke-Rest -Method DELETE -Path "/api/files?path=/live2/doomed.bin"
            Assert-Ok $r
        } finally {
            $tcp.Close()
        }

        # A separate directory, so IT-R1's replacement file cannot leak into the
        # count and mask a real failure here.
        $list = Invoke-Rest -Method GET -Path "/api/files?path=/live2"
        Assert-Eq @((Get-Data $list).entries).Count 0 "directory is empty"
    }

    # ---------------------------------------------------------------- disk
    Write-Head "IT-Q disk full"

    if ($DiskFullDir -ne "") {
        SkipTest "IT-Q1 a full disk yields 507 and keeps the session" "requires a manually prepared near-full volume"
    } else {
        SkipTest "IT-Q1 a full disk yields 507 and keeps the session" "pass -DiskFullDir <near-full-volume> to enable"
    }

} finally {
    Stop-ApiServer $server
    if (-not $KeepData) {
        $script:Client.Dispose()
        try { Remove-Item -Path $script:WorkDir -Recurse -Force -ErrorAction Stop } catch {
            Write-Host "warning: could not remove $($script:WorkDir): $($_.Exception.Message)"
        }
    } else {
        $script:Client.Dispose()
        Write-Host ""
        Write-Host "data kept at: $($script:WorkDir)"
    }
}

# --------------------------------------------------------------------------
# summary
# --------------------------------------------------------------------------
Write-Host ""
Write-Host ("=" * 72)
Write-Host ("passed: {0}   failed: {1}   skipped: {2}" -f $script:Pass, $script:Fail, $script:Skip)
if ($script:Fail -gt 0) {
    Write-Host ""
    Write-Host "failures:" -ForegroundColor Red
    foreach ($f in $script:Failures) { Write-Host "  - $f" -ForegroundColor Red }
}
Write-Host ("=" * 72)
if ($script:Fail -gt 0) { exit 1 } else { exit 0 }
