param(
  [string]$Port = "COM7",
  [int]$Baud = 115200,
  [string]$NodeId = "BATNODE_001",
  [int]$MonitorSeconds = 180,
  [string]$CommandType = "MOTH_STATUS"
)

$ErrorActionPreference = "Stop"

$sketchDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = Resolve-Path (Join-Path $sketchDir "..\..\..")
$serverDir = Join-Path $projectRoot "Code\Server\MothServer-main\bat_node_system\server"
$dbPath = Join-Path $serverDir "bat_nodes_v2.db"
$pythonPath = Join-Path $serverDir ".venv\Scripts\python.exe"
$logDir = Join-Path $sketchDir "logs"

if (!(Test-Path -LiteralPath $dbPath)) {
  throw "Server database not found: $dbPath"
}

if (!(Test-Path -LiteralPath $pythonPath)) {
  throw "Server virtualenv Python not found: $pythonPath"
}

New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$logPath = Join-Path $logDir "bridge-status-$stamp.log"

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

      if ($clean -like "Bridge READY after*" -or $clean -like "MOTH >> OK BRIDGE_READY*" -or $clean -like "MOTH >> OK PONG*") {
        $result = "PASS"
        break
      }

      if ($clean -like "Bridge READY failed*") {
        $result = "MOTH_ERROR"
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

Write-Host ""
Write-Host "Bridge test result: $result"

switch ($result) {
  "PASS" {
    Write-Host "AudioMoth replied over UART. The bridge is alive."
    exit 0
  }
  "NO_AUDIOMOTH_UART" {
    Write-Host "ESP32 asserted ESP_REQ and sent PING, but received zero bytes. Check AudioMoth is flashed with the current READY-beacon bin, is running in CUSTOM mode, and has B9/B10 wired to GPIO16/GPIO17."
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
  default {
    Write-Host "No bridge summary was seen before the monitor timeout."
    exit 5
  }
}
