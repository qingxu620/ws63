param(
    [switch]$SkipInstall,
    [switch]$CreateDesktopShortcut
)

$ErrorActionPreference = "Stop"
$env:PYGAME_HIDE_SUPPORT_PROMPT = "1"
$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location -LiteralPath $ProjectRoot

$PythonCommand = Get-Command python -ErrorAction Stop
$Python = $PythonCommand.Source

function Invoke-Python {
    param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Arguments)
    & $Python @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Python command failed with exit code $LASTEXITCODE"
    }
}

Write-Host "Python: $Python"
if (-not $SkipInstall) {
    Write-Host "Installing build requirements..."
    Invoke-Python -m pip install --disable-pip-version-check -r requirements-build.txt
}

Write-Host "Generating the Windows icon..."
Invoke-Python tools\build_icon.py

Write-Host "Building the one-folder application..."
Invoke-Python -m PyInstaller --noconfirm --clean WS63_Laser_Host.spec

$ExePath = Join-Path $ProjectRoot "dist\WS63_Laser_Host\WS63_Laser_Host.exe"
if (-not (Test-Path -LiteralPath $ExePath -PathType Leaf)) {
    throw "Build completed without the expected executable: $ExePath"
}

$ReportPath = Join-Path $ProjectRoot "build\packaged_self_test.json"
Write-Host "Running packaged startup and serial enumeration self-test..."
$OriginalDataRoot = [Environment]::GetEnvironmentVariable(
    "WS63_LASER_HOST_DATA_DIR",
    "Process"
)
$env:WS63_LASER_HOST_DATA_DIR = Join-Path $ProjectRoot "build\packaged_self_test_data"
try {
    $Process = Start-Process `
        -FilePath $ExePath `
        -ArgumentList @("--self-test", "--self-test-report", "`"$ReportPath`"") `
        -Wait `
        -PassThru `
        -WindowStyle Hidden
}
finally {
    if ($null -eq $OriginalDataRoot) {
        Remove-Item Env:\WS63_LASER_HOST_DATA_DIR -ErrorAction SilentlyContinue
    }
    else {
        $env:WS63_LASER_HOST_DATA_DIR = $OriginalDataRoot
    }
}
if ($Process.ExitCode -ne 0) {
    if (Test-Path -LiteralPath $ReportPath) {
        Get-Content -Raw -LiteralPath $ReportPath
    }
    throw "Packaged self-test failed with exit code $($Process.ExitCode)"
}

$Report = Get-Content -Raw -LiteralPath $ReportPath | ConvertFrom-Json
if ($Report.status -ne "ok") {
    throw "Packaged self-test did not report success"
}

if ($CreateDesktopShortcut) {
    $Desktop = [Environment]::GetFolderPath("Desktop")
    $ShortcutPath = Join-Path $Desktop "WS63 Laser Host.lnk"
    $Shell = New-Object -ComObject WScript.Shell
    $Shortcut = $Shell.CreateShortcut($ShortcutPath)
    $Shortcut.TargetPath = $ExePath
    $Shortcut.WorkingDirectory = Split-Path -Parent $ExePath
    $Shortcut.IconLocation = "$ExePath,0"
    $Shortcut.Description = "WS63 Laser Host"
    $Shortcut.Save()
    Write-Host "Desktop shortcut: $ShortcutPath"
}

Write-Host ""
Write-Host "Build and self-test completed successfully."
Write-Host "Distribute this complete folder:"
Write-Host "  $(Split-Path -Parent $ExePath)"
