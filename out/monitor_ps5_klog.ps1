$ErrorActionPreference = 'SilentlyContinue'
$target = '192.168.1.240'
$port = 3232
$logPath = Join-Path $PSScriptRoot ('ps5_klog_live_' + (Get-Date -Format 'yyyy-MM-dd_HH-mm-ss') + '.txt')
$statusPath = [System.IO.Path]::ChangeExtension($logPath, '.status.txt')
$stopPath = Join-Path $PSScriptRoot 'stop_klog_monitor'
[System.IO.File]::WriteAllText($statusPath, ('Monitor started ' + (Get-Date -Format o) + ' target=' + $target + ':' + $port + [Environment]::NewLine), [System.Text.Encoding]::UTF8)
$lastError = ''
while (-not (Test-Path -LiteralPath $stopPath)) {
    $client = New-Object System.Net.Sockets.TcpClient
    $stream = $null
    $file = $null
    try {
        $async = $client.BeginConnect($target, $port, $null, $null)
        if (-not $async.AsyncWaitHandle.WaitOne(1200)) { throw 'connect timeout' }
        $client.EndConnect($async)
        [System.IO.File]::AppendAllText($statusPath, ('[' + (Get-Date -Format o) + '] CONNECTED ' + $target + ':' + $port + [Environment]::NewLine), [System.Text.Encoding]::UTF8)
        $lastError = ''
        $stream = $client.GetStream()
        $file = New-Object System.IO.FileStream($logPath, [System.IO.FileMode]::Append, [System.IO.FileAccess]::Write, [System.IO.FileShare]::ReadWrite)
        $buffer = New-Object byte[] 16384
        while (-not (Test-Path -LiteralPath $stopPath)) {
            if ($stream.DataAvailable) {
                $count = $stream.Read($buffer, 0, $buffer.Length)
                if ($count -le 0) { break }
                $file.Write($buffer, 0, $count)
                $file.Flush($true)
                [System.IO.File]::AppendAllText($statusPath, ('[' + (Get-Date -Format o) + '] received ' + $count + ' bytes' + [Environment]::NewLine), [System.Text.Encoding]::UTF8)
            } elseif ($client.Client.Poll(0, [System.Net.Sockets.SelectMode]::SelectRead) -and $client.Client.Available -eq 0) {
                break
            } else {
                Start-Sleep -Milliseconds 40
            }
        }
        [System.IO.File]::AppendAllText($statusPath, ('[' + (Get-Date -Format o) + '] DISCONNECTED' + [Environment]::NewLine), [System.Text.Encoding]::UTF8)
    } catch {
        $message = $_.Exception.Message
        if ($message -ne $lastError) {
            [System.IO.File]::AppendAllText($statusPath, ('[' + (Get-Date -Format o) + '] waiting: ' + $message + [Environment]::NewLine), [System.Text.Encoding]::UTF8)
            $lastError = $message
        }
        Start-Sleep -Milliseconds 500
    } finally {
        if ($file) { $file.Dispose() }
        if ($stream) { $stream.Dispose() }
        if ($client) { $client.Close() }
    }
}
[System.IO.File]::AppendAllText($statusPath, ('[' + (Get-Date -Format o) + '] STOPPED' + [Environment]::NewLine), [System.Text.Encoding]::UTF8)
