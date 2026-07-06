param(
  [string]$Port = "COM9",
  [int]$Baud = 115200,
  [string]$NodeId = "AUTO",
  [int]$MonitorSeconds = 900,
  [int]$MaxFiles = 1,
  [int64]$MinFileBytes = 1000,
  [int64]$MaxFileBytes = 20971520,
  [switch]$FullSession
)

$ErrorActionPreference = "Stop"

$sketchDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = Resolve-Path (Join-Path $sketchDir "..\..\..")
$serverDir = Join-Path $projectRoot "Code\Server\MothServer-main\bat_node_system\server"
$dbPath = Join-Path $serverDir "bat_nodes_v2.db"
$pythonCandidates = @(
  (Join-Path $serverDir ".venv\Scripts\python.exe"),
  (Join-Path $env:USERPROFILE ".cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe"),
  "python"
)
$pythonPath = $null
$logDir = Join-Path $sketchDir "logs"

if (!(Test-Path -LiteralPath $dbPath)) { throw "Server database not found: $dbPath" }
foreach ($candidate in $pythonCandidates) {
  try {
    $probe = & $candidate -c "import sqlite3; print('ok')" 2>$null
    if ($LASTEXITCODE -eq 0 -and $probe -eq "ok") {
      $pythonPath = $candidate
      break
    }
  } catch {
  }
}

if (!$pythonPath) { throw "No usable Python runtime found for queuing upload commands" }

New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$logPath = Join-Path $logDir "fast-upload-$stamp.log"

$resolveNodeScript = @"
import sqlite3
import sys

db_path = sys.argv[1]
conn = sqlite3.connect(db_path, timeout=10)
conn.row_factory = sqlite3.Row
row = conn.execute(
    '''
    SELECT node_id FROM node_state
    WHERE node_id IS NOT NULL AND node_id != ''
    ORDER BY COALESCE(last_seen, updated_at, 0) DESC
    LIMIT 1
    '''
).fetchone()
if row is None:
    row = conn.execute(
        '''
        SELECT node_id FROM nodes
        WHERE active=1
        ORDER BY COALESCE(updated_at, created_at, 0) DESC
        LIMIT 1
        '''
    ).fetchone()
conn.close()
if row is None:
    raise SystemExit("No node_id found in database. Pass -NodeId explicitly.")
print(row["node_id"])
"@

if ([string]::IsNullOrWhiteSpace($NodeId) -or $NodeId -eq "AUTO") {
  $NodeId = ($resolveNodeScript | & $pythonPath - $dbPath).Trim()
  if (!$NodeId) { throw "Could not auto-detect node id" }
  Write-Host "Auto-selected latest node: $NodeId"
}

if ($MaxFiles -lt 1) { throw "-MaxFiles must be at least 1" }
if ($MinFileBytes -lt 0) { throw "-MinFileBytes cannot be negative" }
if ($MaxFileBytes -lt 0) { throw "-MaxFileBytes cannot be negative" }
if (!$FullSession -and $MaxFileBytes -gt 0 -and $MinFileBytes -gt $MaxFileBytes) {
  throw "-MinFileBytes cannot be greater than -MaxFileBytes"
}

if ($FullSession) {
  $uploadPayload = "{}"
  Write-Host "Upload filter: full session, all listed files"
} else {
  $uploadPayload = ([ordered]@{
    benchmark = $true
    max_files = $MaxFiles
    min_file_bytes = $MinFileBytes
    max_file_bytes = $MaxFileBytes
    prefer_smallest = $true
  } | ConvertTo-Json -Compress)
  Write-Host ("Upload filter: prefer smallest, max_files={0}, min_bytes={1}, max_bytes={2}" -f $MaxFiles, $MinFileBytes, $MaxFileBytes)
}
$uploadPayloadB64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($uploadPayload))

$queueScript = @"
import sqlite3
import sys
import time
import json
import base64

db_path, node_id, payload_b64 = sys.argv[1], sys.argv[2], sys.argv[3]
payload_json = base64.b64decode(payload_b64).decode("utf-8")
json.loads(payload_json)
now = int(time.time())
conn = sqlite3.connect(db_path, timeout=10)
conn.execute("PRAGMA busy_timeout = 10000")
row = conn.execute(
    "INSERT INTO commands (node_id, command_type, payload_json, status, created_at, expires_at) VALUES (?, 'UPLOAD_NOW', ?, 'PENDING', ?, ?)",
    (node_id, payload_json, now, now + 3600),
)
conn.commit()
print(row.lastrowid)
conn.close()
"@

$commandId = ($queueScript | & $pythonPath - $dbPath $NodeId $uploadPayloadB64).Trim()
if (!$commandId) { throw "Could not queue UPLOAD_NOW" }

Write-Host "Queued UPLOAD_NOW command $commandId for $NodeId"
Write-Host "Resetting $Port and monitoring for up to $MonitorSeconds seconds"
Write-Host "Log: $logPath"

$lines = New-Object System.Collections.Generic.List[string]
$portObj = [System.IO.Ports.SerialPort]::new($Port, $Baud, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
$portObj.ReadTimeout = 250
$portObj.DtrEnable = $true
$portObj.RtsEnable = $true

try {
  $portObj.Open()
  Start-Sleep -Milliseconds 100
  $portObj.DtrEnable = $false
  $portObj.RtsEnable = $true
  Start-Sleep -Milliseconds 100
  $portObj.RtsEnable = $false

  $start = Get-Date
  while (((Get-Date) - $start).TotalSeconds -lt $MonitorSeconds) {
    try {
      $line = $portObj.ReadLine().TrimEnd("`r", "`n")
      $lines.Add($line)
      Write-Host $line
      if ($line -like "Sleeping for*") { break }
    } catch [System.TimeoutException] {
    }
  }
} finally {
  if ($portObj.IsOpen) { $portObj.Close() }
}

$lines | Set-Content -LiteralPath $logPath -Encoding UTF8
$pipeLines = $lines | Where-Object { $_ -like "GETPIPE * bytes at offset *" }
$pipeStatusMissing = $lines | Where-Object {
  $_ -like "AudioMoth STATUS lacks pipe capability fields*" -or
  $_ -like "AudioMoth STATUS lacks protocol v4 ACKed pipe fields*"
} | Select-Object -First 1
$pipeCapabilityMismatch = $lines | Where-Object {
  $_ -like "AudioMoth pipe capability mismatch:*"
} | Select-Object -First 1
$pipeUnavailable = $lines | Where-Object { $_ -like "AudioMoth GETPIPE is unavailable*" } | Select-Object -First 1
$busyStuckHigh = $lines | Where-Object {
  $_ -like "Bridge READY timeout*" -and
  $_ -match "rx_bytes=0" -and
  $_ -match "busy_low_seen=0" -and
  $_ -match "busy_now=1" -and
  $_ -match "esp_req=1"
} | Select-Object -First 1
$noAudioMothUart = $lines | Where-Object {
  $_ -like "Bridge READY timeout*" -and
  $_ -match "rx_bytes=0" -and
  $_ -match "esp_req=1"
} | Select-Object -First 1
$pipeFatal = $lines | Where-Object {
  $_ -like "GETPIPE failed*" -or
  $_ -like "GETPIPE *timeout*" -or
  $_ -like "GETPIPE CRC mismatch*" -or
  $_ -like "GETPIPE *mismatch*"
} | Select-Object -First 1
$fallbackDisabled = $lines | Where-Object {
  $_ -like "GETPIPE required for production upload; rejecting 115200-baud GET fallback*"
} | Select-Object -First 1
$getLines = $lines | Where-Object { $_ -like "GET failed at offset *" }
$results = $lines | Where-Object { $_ -like "Completed *end_to_end=*" }
$listFailure = $lines | Where-Object { $_ -like "AudioMoth LIST *" } | Select-Object -Last 1

Write-Host ""
Write-Host "=== Fast upload benchmark ==="
if ($pipeLines.Count -gt 0) {
  $pipeBytes = 0L
  foreach ($line in $pipeLines) {
    if ($line -match "GETPIPE ([0-9]+) bytes") {
      $pipeBytes += [int64]$Matches[1]
    }
  }
  Write-Host ("GETPIPE used: {0} piped block(s), {1:N0} bytes" -f $pipeLines.Count, $pipeBytes)
} elseif ($pipeUnavailable) {
  Write-Host "GETPIPE unavailable."
  Write-Host $pipeUnavailable
} elseif ($busyStuckHigh) {
  Write-Host "AudioMoth never entered bridge service: MOTH_BUSY stayed high and UART returned zero bytes."
  Write-Host $busyStuckHigh
  Write-Host "Check AudioMoth is flashed with CURRENT_AUDIOMOTH_FLASH\\audiomoth.bin, switched back to CUSTOM/run mode, and PA8/MOTH_BUSY is not shorted high."
} elseif ($noAudioMothUart) {
  Write-Host "AudioMoth UART returned zero bytes while ESP_REQ was asserted."
  Write-Host $noAudioMothUart
  Write-Host "Check the AudioMoth bridge firmware, CUSTOM/run mode, and PB9/PB10 wiring."
} elseif ($fallbackDisabled) {
  Write-Host "GETPIPE did not complete and the slow 115200-baud GET recovery path is disabled in production firmware."
  Write-Host $fallbackDisabled
} elseif ($pipeStatusMissing) {
  Write-Host "GETPIPE was skipped because the AudioMoth STATUS line lacks protocol v4 ACKed pipe capability fields."
  Write-Host $pipeStatusMissing
  Write-Host "Flash CURRENT_AUDIOMOTH_FLASH\\audiomoth.bin and retry."
} elseif ($pipeCapabilityMismatch) {
  Write-Host "GETPIPE was skipped because ESP32 and AudioMoth disagree on pipe capability."
  Write-Host $pipeCapabilityMismatch
  Write-Host "The ESP32 expects the stable 115200-baud ACKed pipe. Flash CURRENT_AUDIOMOTH_FLASH\\audiomoth.bin and retry."
} elseif ($pipeFatal) {
  Write-Host "GETPIPE failed before completion:"
  Write-Host $pipeFatal
} elseif ($getLines.Count -gt 0) {
  Write-Host "Legacy 115200-baud GET path emitted a failure; production upload requires GETPIPE."
  Write-Host ($getLines | Select-Object -First 1)
} else {
  Write-Host "No completed GETPIPE upload was observed."
}
if ($results) {
  $results | ForEach-Object { Write-Host $_ }
  exit 0
}
if ($pipeFatal) {
  Write-Host "First GETPIPE failure:"
  Write-Host $pipeFatal
}
if ($fallbackDisabled) {
  Write-Host "Fallback disabled:"
  Write-Host $fallbackDisabled
}
if ($busyStuckHigh) {
  Write-Host "Busy-high bridge failure:"
  Write-Host $busyStuckHigh
}
if ($noAudioMothUart -and -not $busyStuckHigh) {
  Write-Host "Zero-byte UART bridge failure:"
  Write-Host $noAudioMothUart
}
if ($pipeCapabilityMismatch) {
  Write-Host "First pipe capability mismatch:"
  Write-Host $pipeCapabilityMismatch
}
if ($getLines.Count -gt 0) {
  Write-Host "First GET fallback failure:"
  Write-Host ($getLines | Select-Object -First 1)
}
if ($listFailure) { Write-Host $listFailure }
Write-Host "No completed file transfer was measured. Ensure the AudioMoth SD card contains a file not already uploaded."
exit 2
