$ErrorActionPreference = 'SilentlyContinue'
$ftpRoot = 'ftp://192.168.1.240:2121/user/devlog/system/sce_coredumps.0/'
$saveDir = Join-Path $PSScriptRoot 'captured_coredumps'
$statusPath = Join-Path $saveDir 'ftp-watch.status.txt'
$stopPath = Join-Path $PSScriptRoot 'stop_coredump_watch'
New-Item -ItemType Directory -Path $saveDir -Force | Out-Null
[System.IO.File]::WriteAllText($statusPath, ('Watcher started ' + (Get-Date -Format o) + ' path=' + $ftpRoot + [Environment]::NewLine), [System.Text.Encoding]::UTF8)
$lastError = ''
while (-not (Test-Path -LiteralPath $stopPath)) {
    try {
        $listing = & curl.exe --silent --show-error --user 'anonymous:anonymous' --max-time 3 $ftpRoot 2>&1
        if ($LASTEXITCODE -eq 0) {
            $lastError = ''
            foreach ($entry in $listing) {
                if ($entry -match '(PPSA99001_\d+)') {
                    $folder = $Matches[1]
                    $folderUrl = $ftpRoot + $folder + '/'
                    $files = & curl.exe --silent --show-error --user 'anonymous:anonymous' --max-time 3 $folderUrl 2>&1
                    if ($LASTEXITCODE -eq 0) {
                        foreach ($item in $files) {
                            if ($item -match '(prosperocore-[A-Za-z0-9_.-]+\.prosperodmp)') {
                                $name = $Matches[1]
                                $destination = Join-Path $saveDir $name
                                if (-not (Test-Path -LiteralPath $destination)) {
                                    [System.IO.File]::AppendAllText($statusPath, ('[' + (Get-Date -Format o) + '] downloading ' + $folder + '/' + $name + [Environment]::NewLine), [System.Text.Encoding]::UTF8)
                                    & curl.exe --silent --show-error --fail --user 'anonymous:anonymous' --max-time 240 --output $destination ($folderUrl + $name)
                                    if ($LASTEXITCODE -eq 0) {
                                        $length = (Get-Item -LiteralPath $destination).Length
                                        [System.IO.File]::AppendAllText($statusPath, ('[' + (Get-Date -Format o) + '] SAVED ' + $destination + ' bytes=' + $length + [Environment]::NewLine), [System.Text.Encoding]::UTF8)
                                    } else {
                                        Remove-Item -LiteralPath $destination -Force
                                        [System.IO.File]::AppendAllText($statusPath, ('[' + (Get-Date -Format o) + '] download failed for ' + $name + [Environment]::NewLine), [System.Text.Encoding]::UTF8)
                                    }
                                }
                            }
                        }
                    }
                }
            }
        } else {
            $message = ($listing | Out-String).Trim()
            if ($message -ne $lastError) {
                [System.IO.File]::AppendAllText($statusPath, ('[' + (Get-Date -Format o) + '] FTP waiting: ' + $message + [Environment]::NewLine), [System.Text.Encoding]::UTF8)
                $lastError = $message
            }
        }
    } catch {
        $message = $_.Exception.Message
        if ($message -ne $lastError) {
            [System.IO.File]::AppendAllText($statusPath, ('[' + (Get-Date -Format o) + '] watcher error: ' + $message + [Environment]::NewLine), [System.Text.Encoding]::UTF8)
            $lastError = $message
        }
    }
    Start-Sleep -Milliseconds 500
}
[System.IO.File]::AppendAllText($statusPath, ('[' + (Get-Date -Format o) + '] STOPPED' + [Environment]::NewLine), [System.Text.Encoding]::UTF8)
