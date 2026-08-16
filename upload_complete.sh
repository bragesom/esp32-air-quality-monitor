#!/bin/bash
# Complete rebuild and upload script for AQ-Meter Pro ESP32 project
# This script cleans and rebuilds everything, then uploads both firmware and web files

echo "======================================================="
echo "  AQ-Meter Pro ESP32 - Complete Rebuild & Upload Tool"
echo "======================================================="
echo "This script will perform a complete rebuild and upload of all project files."

# Navigate to project directory
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
cd "$SCRIPT_DIR"

# Step 1: Compress web files (also runs via the pre: hook; explicit here for safety)
echo ""
echo "Step 1: Compressing web files..."
python compress_web_files.py

# Step 2: Build and upload firmware
echo ""
echo "Step 2: Building and uploading firmware..."
pio run -e esp32_3mb --target upload
if [ $? -ne 0 ]; then
    echo "Failed to upload firmware. Check connection and try again."
    exit 1
fi

# Step 3: Upload filesystem
echo ""
echo "Step 3: Uploading SPIFFS filesystem (web files)..."
sleep 2  # Wait a bit for ESP32 to settle
pio run -e esp32_3mb --target uploadfs
if [ $? -ne 0 ]; then
    echo "Failed to upload filesystem. Check connection and try again."
    exit 1
fi

# Step 4: Monitor output
echo ""
echo "Upload complete! Monitoring ESP32 output..."
echo "Press Ctrl+C to exit monitoring."
sleep 2  # Wait for ESP32 to restart
pio device monitor

echo "======================================================="
echo "  AQ-Meter Pro ESP32 - Upload Complete!"
echo "======================================================="
