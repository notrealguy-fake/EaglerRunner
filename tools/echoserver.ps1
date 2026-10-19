# echoserver.ps1 - tiny TCP + UDP echo servers used to verify EaglerRunner networking.
#
#   TCP echo on 127.0.0.1:25599
#   UDP echo on 127.0.0.1:25598
#
# Usage:  powershell -ExecutionPolicy Bypass -File tools/echoserver.ps1 [-Seconds 60]
param([int]$Seconds = 60)

$tcpPort = 25599
$udpPort = 25598

$tcp = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $tcpPort)
$tcp.Start()

$udp = [System.Net.Sockets.UdpClient]::new($udpPort)

$deadline = (Get-Date).AddSeconds($Seconds)
Write-Host "TCP echo listening on 127.0.0.1:$tcpPort"
Write-Host "UDP echo listening on 127.0.0.1:$udpPort"
Write-Host "Stopping in $Seconds seconds..."

$udpEndPoint = [System.Net.IPEndPoint]::new([System.Net.IPAddress]::Any, 0)

while ((Get-Date) -lt $deadline) {
  # --- TCP ---
  if ($tcp.Pending()) {
    $client = $tcp.AcceptTcpClient()
    $stream = $client.GetStream()
    $stream.ReadTimeout = 3000
    try {
      $buffer = New-Object byte[] 4096
      $read = $stream.Read($buffer, 0, $buffer.Length)
      if ($read -gt 0) {
        Write-Host "TCP <- $read byte(s)"
        $stream.Write($buffer, 0, $read)   # echo back
        $stream.Flush()
      }
    } catch { Write-Host "TCP error: $_" }
    Start-Sleep -Milliseconds 50
    $client.Close()
  }

  # --- UDP ---
  if ($udp.Available -gt 0) {
    $data = $udp.Receive([ref]$udpEndPoint)
    Write-Host "UDP <- $($data.Length) byte(s) from $($udpEndPoint.Address)"
    $udp.Send($data, $data.Length, $udpEndPoint) | Out-Null
  }

  Start-Sleep -Milliseconds 40
}

$tcp.Stop()
$udp.Close()
Write-Host "Echo servers stopped."
