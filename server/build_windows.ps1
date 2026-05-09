$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $ScriptDir

$Go = "C:\Program Files\Go\bin\go.exe"
if (-not (Test-Path $Go)) {
    $Go = "go"
}

Write-Host "Running Go tests..."
& $Go test ./...

Write-Host "Building medgenet-go.exe..."
New-Item -ItemType Directory -Force -Path "dist" | Out-Null
& $Go build -o "dist\medgenet-go.exe" .

if (-not (Test-Path "dist\server.ini")) {
    Copy-Item "server.example.ini" "dist\server.ini"
}

Write-Host ""
Write-Host "Build complete: dist\medgenet-go.exe"
Write-Host "Config:         dist\server.ini"
Write-Host ""
Write-Host "Run from an Administrator PowerShell:"
Write-Host "  cd `"$ScriptDir\dist`""
Write-Host "  .\medgenet-go.exe"
