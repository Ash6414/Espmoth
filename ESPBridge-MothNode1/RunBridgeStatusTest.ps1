param(
  [string]$Port = "COM7",
  [int]$Baud = 115200,
  [string]$NodeId = "AUTO",
  [int]$MonitorSeconds = 180,
  [string]$CommandType = "MOTH_STATUS"
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

if (!(Test-Path -LiteralPath $dbPath)) {
  throw "Server database not found: $dbPath"
}

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

if (!$pythonPath) {
  throw "No usable Python runtime found for queuing bridge commands"
}

New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$logPath = Join-Path $logDir "bridge-status-$stamp.log"

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

db_path = sys.argv[1]
node_id = sys.argv[2]
command_type = sys.argv[3]
now = int(time.time())

conn = sqlite3.connect(db_path, timeout=10)
conn.execute("PRAGMA busy_timeout = 10000")
cur = conn.execute(
    "INSERT INTO commands (node_id, command_type, payload_json, status, created_at, expires_at) VALUES (?, ?, '{}', 'PENDING', ?, ?)",
    (node_id, command_type, now, now + 86400),
)
conn.commit()
print(cur.lastrowid)
conn.close()
"@

$queueScriptPath = Join-Path $logDir "queue-bridge-command-$stamp.py"
$queueScript | Set-Content -LiteralPath $queueScriptPath -Encoding UTF8

try {
  $commandId = (& $pythonPath $queueScriptPath $dbPath $NodeId $CommandType).Trim()
} finally {
  Remove-Item -LiteralPath $queueScriptPath -Force -ErrorAction SilentlyContinue
}

if (!$commandId) {
  throw "Failed to queue $CommandType command"
}

Write-Host "Queued $CommandType command id $commandId for $NodeId"
Write-Host "Resetting $Port and monitoring for up to $MonitorSeconds seconds..."
Write-Host "Log: $logPath"

$lines = New-Object System.Collections.Generic.List[string]
$result = "NO_RESULT"
$usbDebugCommandSent = $false
$ackSeen = $false
$commandError = $false

$portObj = [System.IO.Ports.SerialPort]::new($Port, $Baud, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
$portObj.ReadTimeout = 250
$portObj.DtrEnable = $true
$portObj.RtsEnable = $true

try {
  $portObj.Open()
  Start-Sleep -Milliseconds 100

  # Reset ESP32 via the same DTR/RTS sequence used by the manual COM7 tests.
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

      if ($clean -like "*TESTSTREAM*failed*" -or
          $clean -like "*TESTSTREAM*timeout*" -or
          $clean -like "*TESTSTREAM*CRC mismatch*" -or
          $clean -like "*TESTSTREAM*unknown_command*" -or
          $clean -like "USB_DEBUG_MOTH_LIST FAIL*") {
        $commandError = $true
      }

      if (!$usbDebugCommandSent -and $clean -like "USB_DEBUG_READY*") {
        Write-Host "Sending USB debug command: MOTH_STATUS"
        $portObj.WriteLine("MOTH_STATUS")
        $usbDebugCommandSent = $true
        continue
      }

      if ($clean -like "POST /v1/device/$NodeId/commands/$commandId/ack -> 200") {
        $ackSeen = $true
        if ($commandError) {
          $result = "COMMAND_ERROR"
        } else {
          $result = "PASS"
        }
        break
      }

      if ($clean -like "USB_DEBUG_MOTH_STATUS OK STATUS*") {
        $result = "PASS"
        break
      }

      if ($clean -like "USB_DEBUG_MOTH_STATUS FAIL*") {
        $result = "MOTH_ERROR"
        break
      }

      if ($clean -like "Bridge READY failed*") {
        $result = "MOTH_ERROR"
        break
      }

      if ($clean -like "Sleeping for*" -and $ackSeen) {
        $result = "PASS"
        break
      }

      if ($clean -like "Bridge READY timeout*") {
        if ($clean -match "rx_bytes=0" -and $clean -match "esp_req=1") {
          $result = "NO_AUDIOMOTH_UART"
        } else {
          $result = "TIMEOUT_WITH_ACTIVITY"
        }
        break
      }
    } catch [System.TimeoutException] {
    }
  }
} finally {
  if ($portObj.IsOpen) {
    $portObj.Close()
  }
}

if ($lines.Count -eq 0) {
  $lines.Add("NO_SERIAL_LINES")
}

$lines | Set-Content -LiteralPath $logPath -Encoding UTF8

$responseMessage = $null
if ($ackSeen) {
  $responseScript = @"
import json
import sqlite3
import sys

db_path = sys.argv[1]
command_id = int(sys.argv[2])
conn = sqlite3.connect(db_path, timeout=10)
row = conn.execute("SELECT response_json FROM commands WHERE id=?", (command_id,)).fetchone()
conn.close()
if row and row[0]:
    payload = json.loads(row[0])
    print(payload.get("message", ""))
"@
  try {
    $responseMessage = ($responseScript | & $pythonPath - $dbPath $commandId).Trim()
  } catch {
    $responseMessage = $null
  }
}

Write-Host ""
Write-Host "Bridge test result: $result"
$statusCapabilityOk = $true
if ($responseMessage) {
  Write-Host "AudioMoth response: $responseMessage"
  if ($CommandType -eq "MOTH_STATUS" -and $responseMessage -like "OK STATUS*") {
    if ($responseMessage -like "*proto=4*" -and
        $responseMessage -like "*pipe=1*" -and
        $responseMessage -like "*pipe_baud=230400*" -and
        $responseMessage -like "*pipe_bytes=65536*" -and
        $responseMessage -like "*pipe_frame=2048*" -and
        $responseMessage -like "*pipe_ack=1*") {
      Write-Host "AudioMoth protocol v4 ACKed pipe capability: OK"
    } else {
      $statusCapabilityOk = $false
      Write-Host "AudioMoth protocol v4 ACKed pipe capability: MISSING - flash CURRENT_AUDIOMOTH_FLASH\\audiomoth.bin and retry."
    }
  }
}

switch ($result) {
  "PASS" {
    if (-not $statusCapabilityOk) {
      Write-Host "AudioMoth replied over UART, but its pipe contract does not match this ESP build."
      exit 7
    }
    Write-Host "AudioMoth replied over UART and the queued $CommandType command was acknowledged."
    exit 0
  }
  "NO_AUDIOMOTH_UART" {
    Write-Host "ESP32 asserted ESP_REQ and sent PING, but received zero bytes. Check AudioMoth is flashed with the current READY-beacon bin, is running in CUSTOM mode, and has B9/B10 wired to GPIO32/GPIO33."
    exit 2
  }
  "TIMEOUT_WITH_ACTIVITY" {
    Write-Host "ESP32 saw some UART/pin activity, but did not complete the READY handshake."
    exit 3
  }
  "MOTH_ERROR" {
    Write-Host "AudioMoth returned an ERR line during bridge open."
    exit 4
  }
  "COMMAND_ERROR" {
    Write-Host "The queued $CommandType command was acknowledged, but its serial diagnostics reported a command-level failure."
    exit 6
  }
  default {
    Write-Host "No bridge summary was seen before the monitor timeout."
    exit 5
  }
}
