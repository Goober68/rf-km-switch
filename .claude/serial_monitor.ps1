param([string]$PortName = 'COM7', [int]$Baud = 115200)

$port = $null
while ($true) {
  try {
    if ($port -eq $null -or -not $port.IsOpen) {
      try { if ($port) { $port.Close() } } catch {}
      $port = New-Object System.IO.Ports.SerialPort $PortName, $Baud, 'None', 8, 'One'
      $port.ReadTimeout = 500
      $port.DtrEnable = $true
      $port.Open()
      Write-Output "[monitor] opened $PortName @ $Baud"
    }
    try {
      $line = $port.ReadLine()
      if ($line) { Write-Output $line }
    } catch [TimeoutException] {}
  } catch {
    Write-Output ("[monitor] " + $_.Exception.Message)
    Start-Sleep -Milliseconds 500
  }
}
