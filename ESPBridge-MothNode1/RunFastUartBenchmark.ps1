param(
  [string]$Port = "COM7",
  [int]$Baud = 115200,
  [string]$NodeId = "AUTO",
  [int]$MonitorSeconds = 420
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

if (!$pythonPath) { throw "No usable Python runtime found for queuing UART benchmark command" }

New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$logPath = Join-Path $logDir "fast-uart-test-$stamp.log"

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
cur = conn.execute(
    "INSERT INTO commands (node_id, command_type, payload_json, status, created_at, expires_at) VALUES (?, 'MOTH_TEST_STREAM', '{}', 'PENDING', ?, ?)",
    (node_id, now, now + 3600),
)
conn.commit()
print(cur.lastrowid)
conn.close()
"@

$commandId = ($queueScript | & $pythonPath - $dbPath $NodeId).Trim()
if (!$commandId) { throw "Could not queue MOTH_TEST_STREAM" }

Write-Host "Queued MOTH_TEST_STREAM command $commandId for $NodeId"
Write-Host "Resetting $Port and monitoring for up to $MonitorSeconds seconds"
Write-Host "Log: $logPath"

$lines = New-Object System.Collections.Generic.List[string]
$result = "NO_RESULT"
$summaryLine = $null
$unsupportedLine = $null
$failureLine = $null
$ackSeen = $false
$usbDebugCommandSent = $false

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
      $line = $portObj.ReadLine()
      if ($null -eq $line) { continue }
      $clean = $line.TrimEnd("`r", "`n")
      $lines.Add($clean)
      Write-Host $clean

      if (!$usbDebugCommandSent -and $clean -like "USB_DEBUG_READY*") {
        Write-Host "Server command delivery is unavailable; sending USB debug command: STREAM_TEST"
        $portObj.WriteLine("STREAM_TEST")
        $usbDebugCommandSent = $true
        continue
      }

      if ($clean -like "*TESTSTREAM*unknown_command*") {
        $unsupportedLine = $clean
        $result = "UNSUPPORTED"
      } elseif ($clean -like "MOTH_TEST_STREAM OK bytes=*") {
        $summaryLine = $clean
        $result = "PASS"
  } elseif ($clean -like "USB_DEBUG_MOTH_TEST_STREAM MOTH_TEST_STREAM ok bytes=*") {
    $summaryLine = $clean
    $result = "PASS"
  } elseif ($clean -like "*control resync failed after fast stream*") {
    $failureLine = $clean
    if ($result -ne "UNSUPPORTED") { $result = "OLD_MOTH_BIN" }
  } elseif ($clean -like "*TESTSTREAM*failed*" -or
            $clean -like "*TESTSTREAM*timeout*" -or
            $clean -like "*TESTSTREAM*CRC mismatch*" -or
                $clean -like "*TESTSTREAM*payload pattern mismatch*") {
        $failureLine = $clean
        if ($result -ne "UNSUPPORTED") { $result = "FAILED" }
      }

      if ($clean -like "POST /v1/device/$NodeId/commands/$commandId/ack -> 200") {
        $ackSeen = $true
        break
      }
      if ($clean -like "USB_DEBUG_MOTH_TEST_STREAM *") { break }
      if ($clean -like "Sleeping for*") { break }
    } catch [System.TimeoutException] {
    }
  }
} finally {
  if ($portObj.IsOpen) { $portObj.Close() }
}

if ($lines.Count -eq 0) { $lines.Add("NO_SERIAL_LINES") }
$lines | Set-Content -LiteralPath $logPath -Encoding UTF8

Write-Host ""
Write-Host "=== Fast UART benchmark ==="
if ($failureLine -and $failureLine -match "failed baud=([0-9]+).*max_ok_baud=([0-9]+).*max_ok_kib_s=([0-9.]+)") {
  Write-Host "Highest stable UART rate found before failure:"
  Write-Host $failureLine
  Write-Host ("Max stable: {0} baud at {1} KiB/s; next failed: {2} baud" -f $Matches[2], $Matches[3], $Matches[1])
  exit 0
}

if ($summaryLine) {
  Write-Host $summaryLine
  if ($summaryLine -match "bytes=([0-9]+) baud=([0-9]+) ms=([0-9]+) rate=([0-9.]+) KiB/s crc=([0-9A-Fa-f]+)") {
    Write-Host ("Validated {0:N0} bytes at {1} baud in {2} ms: {3} KiB/s, crc={4}" -f [int64]$Matches[1], $Matches[2], $Matches[3], $Matches[4], $Matches[5])
  } elseif ($summaryLine -match "bytes=([0-9]+) baud=([0-9]+) ms=([0-9]+) kib_s=([0-9.]+) crc=([0-9A-Fa-f]+)") {
    Write-Host ("Validated {0:N0} bytes at {1} baud in {2} ms: {3} KiB/s, crc={4}" -f [int64]$Matches[1], $Matches[2], $Matches[3], $Matches[4], $Matches[5])
  }
  if ($summaryLine -match "failed_higher_baud=([0-9]+)") {
    Write-Host ("Higher baud failed before fallback: {0}" -f $Matches[1])
  }
  exit 0
}

if ($unsupportedLine) {
  Write-Host "AudioMoth does not support TESTSTREAM yet:"
  Write-Host $unsupportedLine
  Write-Host "Flash the staged CURRENT_AUDIOMOTH_FLASH\\audiomoth.bin, return AudioMoth to CUSTOM mode, then rerun this benchmark."
  exit 6
}

if ($failureLine -and $failureLine -like "*control resync failed after fast stream*") {
  Write-Host "Fast data moved, but AudioMoth did not return to 115200 command mode:"
  Write-Host $failureLine
  Write-Host "Flash Code\\Moth Firmware\\CURRENT_AUDIOMOTH_FLASH\\audiomoth.bin, return AudioMoth to CUSTOM mode, then rerun this benchmark."
  exit 6
}

if ($failureLine) {
  Write-Host "Fast UART diagnostic failed:"
  Write-Host $failureLine
  exit 7
}

if ($ackSeen) {
  Write-Host "Command was acknowledged, but no MOTH_TEST_STREAM result line was observed."
  exit 8
}

Write-Host "No benchmark result was observed before timeout."
exit 9
