param(
    [ValidateSet('packages','app','check','clean')]
    [string]$Mode = 'packages',
    [string]$Distro = 'Ubuntu-24.04'
)

$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Split-Path -Parent $MyInvocation.MyCommand.Path)).Path

if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) {
    throw 'wsl.exe was not found. Install WSL2 on Windows 11 first.'
}

$installed = & wsl.exe -l -q 2>$null |
    ForEach-Object { $_.Trim() } |
    Where-Object { $_ }
if ($installed -notcontains $Distro) {
    throw "WSL distribution '$Distro' is not installed. Install Ubuntu 24.04, then rerun this build."
}

# Do not pass a raw Windows path such as D:\project to Linux wslpath.
# PowerShell/WSL argument translation can turn it into D:project.  Resolve
# ordinary Windows drive paths explicitly to the WSL /mnt/<drive>/ form.
if ($Root -match '^(?<drive>[A-Za-z]):\\(?<rest>.*)$') {
    $drive = $Matches['drive'].ToLowerInvariant()
    $rest = $Matches['rest'] -replace '\\', '/'
    $WslRoot = "/mnt/$drive/$rest"
}
elseif ($Root -match '^\\\\wsl\\$\\(?<wslDistro>[^\\]+)\\(?<rest>.*)$') {
    throw 'The project must be launched from a normal Windows filesystem path, not from a \\wsl$ UNC path.'
}
else {
    throw "Unsupported project path '$Root'. Put the project on a Windows drive such as C:\\ or D:\\ so WSL2 can build it."
}

Write-Host "==> ProsperoRadio Modernized / WSL2 $Distro / mode=$Mode"
Write-Host "    Windows root: $Root"
Write-Host "    WSL root:     $WslRoot"
Write-Host "    Linux build:  ~/.cache/prospero-radio-modernized"

# The Windows project directory is only the control/output surface.
# The actual Git checkout and native PS5 build are performed on WSL's
# Linux filesystem so Git can create locks, preserve executable bits,
# and use normal Unix filesystem semantics.
$command = "cd '$WslRoot' && bash ./build-vulkan.sh '$Mode'"
& wsl.exe -d $Distro -- bash -lc $command
if ($LASTEXITCODE -ne 0) {
    throw "WSL build failed with exit code $LASTEXITCODE."
}
