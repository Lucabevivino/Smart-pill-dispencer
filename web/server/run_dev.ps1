# run_dev.ps1 - Start the MedClip Unified Server (Frontend + Firebase Backend)
param (
    [int]$Port = 8000,
    [switch]$NoSeed
)

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
Set-Location $ScriptDir

Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host "  Starting MedClip Server (Frontend + Cloud Firestore)    " -ForegroundColor Cyan
Write-Host "==========================================================" -ForegroundColor Cyan

# Check Python
$PythonCmd = Get-Command python -ErrorAction SilentlyContinue
if (-not $PythonCmd) {
    Write-Host "Error: Python is not found in PATH. Please install Python 3.9+." -ForegroundColor Red
    exit 1
}

# Check firebase-admin
& python -c "import firebase_admin" 2>$null
if ($LASTEXITCODE -ne 0) {
    Write-Host "Warning: 'firebase-admin' package is not installed." -ForegroundColor Yellow
    Write-Host "Installing dependencies: pip install firebase-admin..." -ForegroundColor Yellow
    & pip install firebase-admin
}

Write-Host "Local Web App: http://localhost:$Port" -ForegroundColor Green
Write-Host "Default Login: patient@medclip.local / medclip123" -ForegroundColor Green
Write-Host ""
Write-Host "To test events, open a second terminal and run:" -ForegroundColor Yellow
Write-Host "  cd web\server" -ForegroundColor Yellow
Write-Host "  python simulate_clip.py taken A1F3B2" -ForegroundColor Yellow
Write-Host "  python simulate_clip.py taken ESP_001 --endpoint log_pill" -ForegroundColor Yellow
Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host ""

if ($NoSeed) {
    & python server.py --port $Port
} else {
    & python server.py --seed --port $Port
}
