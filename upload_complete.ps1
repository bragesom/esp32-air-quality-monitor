# Complete rebuild and upload script for AQ-Meter Pro ESP32 project
# This script cleans and rebuilds everything, then uploads both firmware and web files

# Function for colored output
function Write-ColorOutput($ForegroundColor) {
    $previousForegroundColor = $host.UI.RawUI.ForegroundColor
    $host.UI.RawUI.ForegroundColor = $ForegroundColor
    if ($args) {
        Write-Output $args
    } else {
        $input | Write-Output
    }
    $host.UI.RawUI.ForegroundColor = $previousForegroundColor
}

# Navigate to project directory
$scriptPath = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $scriptPath

Write-ColorOutput Green "======================================================="
Write-ColorOutput Green "  AQ-Meter Pro ESP32 - Complete Rebuild & Upload Tool"
Write-ColorOutput Green "======================================================="

# Step 1: Compress web files (also runs via the pre: hook; explicit here for safety)
Write-ColorOutput Yellow "Step 1: Compressing web files..."
python compress_web_files.py

# Step 2: Build and upload firmware
Write-ColorOutput Yellow "Step 2: Building and uploading firmware..."
pio run -e esp32_3mb --target upload
if ($LASTEXITCODE -ne 0) {
    Write-ColorOutput Red "Failed to upload firmware. Check connection and try again."
    exit 1
}

# Step 3: Upload filesystem
Write-ColorOutput Yellow "Step 3: Uploading SPIFFS filesystem (web files)..."
Start-Sleep -Seconds 2  # Wait a bit for ESP32 to settle
pio run -e esp32_3mb --target uploadfs
if ($LASTEXITCODE -ne 0) {
    Write-ColorOutput Red "Failed to upload filesystem. Check connection and try again."
    exit 1
}

# Step 4: Monitor output
Write-ColorOutput Green "Upload complete! Monitoring ESP32 output..."
Write-ColorOutput Yellow "Press Ctrl+C to exit monitoring."
Start-Sleep -Seconds 2  # Wait for ESP32 to restart
pio device monitor

Write-ColorOutput Green "======================================================="
Write-ColorOutput Green "  AQ-Meter Pro ESP32 - Upload Complete!"
Write-ColorOutput Green "======================================================="
