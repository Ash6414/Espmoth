param(
  [string]$Port = "COM7",
  [int]$Baud = 115200,
  [string]$NodeId = "BATNODE_001",
  [int]$MonitorSeconds = 900
)

$ErrorActionPreference = "Stop"

$sketchDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = Resolve-Path (Join-Path $sketchDir "..\..\..")
$serverDir = Join-Path $projectRoot "Code\Server\MothServer-main\bat_node_system\server"
$dbPath = Join-Path $serverDir "bat_nodes_v2.db"
$pythonPath = Join-Path $serverDir ".venv\Scripts\python.exe"
$logDir = Join-Path $sketchDir "logs"

if (!(Test-Path -LiteralPath $dbPath)) { throw "Server database not found: $dbPath" }
if (!(Test-Path -LiteralPath $pythonPath)) { throw "Server Python not found: $pythonPath" }

New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$logPath = Join-Path $logDir "fast-upload-$stamp.log"

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
$baudLine = $lines | Where-Object { $_ -like "AudioMoth UART running at*" } | Select-Object -Last 1
$results = $lines | Where-Object { $_ -like "Completed *end_to_end=*" }
$listFailure = $lines | Where-Object { $_ -like "AudioMoth LIST *" } | Select-Object -Last 1

Write-Host ""
Write-Host "=== Fast upload benchmark ==="
if ($baudLine) { Write-Host $baudLine } else { Write-Host "Fast UART negotiation was not observed." }
if ($results) {
  $results | ForEach-Object { Write-Host $_ }
  exit 0
}
if ($listFailure) { Write-Host $listFailure }
Write-Host "No completed file transfer was measured. Ensure the AudioMoth SD card contains a file not already uploaded."
exit 2
