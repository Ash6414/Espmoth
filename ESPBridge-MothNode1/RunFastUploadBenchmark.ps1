param(
  [string]$Port = "COM7",
  [int]$Baud = 115200,
  [string]$NodeId = "AUTO",
  [int]$MonitorSeconds = 900
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

$queueScript = @"
import sqlite3
import sys
import time

db_path, node_id = sys.argv[1], sys.argv[2]
now = int(time.time())
conn = sqlite3.connect(db_path, timeout=10)
conn.execute("PRAGMA busy_timeout = 10000")
row = conn.execute(
    "INSERT INTO commands (node_id, command_type, payload_json, status, created_at, expires_at) VALUES (?, 'UPLOAD_NOW', '{}', 'PENDING', ?, ?)",
    (node_id, now, now + 3600),
)
conn.commit()
print(row.lastrowid)
conn.close()
"@

$commandId = ($queueScript | & $pythonPath - $dbPath $NodeId).Trim()
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
$baudLine = $lines | Where-Object { $_ -like "AudioMoth fast payload mode armed at*" } | Select-Object -Last 1
$streamLines = $lines | Where-Object { $_ -like "GETSTREAM * bytes at offset *" }
$streamUnavailable = $lines | Where-Object { $_ -like "AudioMoth GETSTREAM is unavailable*" } | Select-Object -First 1
$streamFatal = $lines | Where-Object {
  $_ -like "GETSTREAM failed fatally*" -or
  $_ -like "GETSTREAM *timeout*" -or
  $_ -like "GETSTREAM CRC mismatch*" -or
  $_ -like "GETSTREAM *control resync failed*"
} | Select-Object -First 1
$results = $lines | Where-Object { $_ -like "Completed *end_to_end=*" }
$listFailure = $lines | Where-Object { $_ -like "AudioMoth LIST *" } | Select-Object -Last 1

Write-Host ""
Write-Host "=== Fast upload benchmark ==="
if ($streamLines.Count -gt 0) {
  $streamBytes = 0L
  foreach ($line in $streamLines) {
    if ($line -match "GETSTREAM ([0-9]+) bytes") {
      $streamBytes += [int64]$Matches[1]
    }
  }
  Write-Host ("GETSTREAM used: {0} streamed block(s), {1:N0} bytes" -f $streamLines.Count, $streamBytes)
} elseif ($streamUnavailable) {
  Write-Host "GETSTREAM unavailable; benchmark used the 115200-baud fallback path."
  Write-Host $streamUnavailable
  if ($streamUnavailable -like "*unsupported_baud*") {
    Write-Host "AudioMoth firmware does not support the ESP's requested stream baud. Flash CURRENT_AUDIOMOTH_FLASH\\audiomoth.bin and retry."
  }
} elseif ($streamFatal) {
  Write-Host "GETSTREAM failed before completion:"
  Write-Host $streamFatal
} elseif ($baudLine) {
  Write-Host $baudLine
} else {
  Write-Host "No fast UART mode was observed."
}
if ($results) {
  $results | ForEach-Object { Write-Host $_ }
  exit 0
}
if ($streamFatal) {
  Write-Host "First GETSTREAM failure:"
  Write-Host $streamFatal
}
if ($listFailure) { Write-Host $listFailure }
Write-Host "No completed file transfer was measured. Ensure the AudioMoth SD card contains a file not already uploaded."
exit 2
