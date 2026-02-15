#!/bin/bash
set -e  # Exit on error

echo "PTSL Beat Tool - Release Build Script"
echo "===================================="

# Get the directory containing this script
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
cd "$SCRIPT_DIR"

# Define output directory
OUTPUT_DIR="releases"
BUILD_DIR="build"

# Clean ALL old build artifacts and output
echo "Cleaning workspace..."
echo "  Removing old builds and temporary files..."
rm -rf "$BUILD_DIR"
rm -rf "$OUTPUT_DIR"
rm -rf "PTSL Beat Tool.app"  # Remove any stray app in root
rm -f "PTSL_Beat_Tool.dmg"    # Remove any stray DMG in root
rm -rf python_bundle         # Remove Python bundle directory
rm -f test_ptsl.py          # Remove test files

# Also clean CMake cache files if they exist
rm -f CMakeCache.txt
rm -rf CMakeFiles

# Show what's left in the directory
echo "  Current directory contents:"
ls -la | grep -E "(\.app|\.dmg|build|release|python_bundle)" || echo "    (No build artifacts found)"

# Create fresh build directory
echo ""
echo "Creating fresh build directory..."
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Configure with CMake
echo "Configuring with CMake..."
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="x86_64;arm64" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="11.0"

# Build
echo ""
echo "Building..."
cmake --build . --config Release -j8

# Verify the build
echo "Verifying build..."
if [ ! -f "PTSL Beat Tool.app/Contents/MacOS/PTSL Beat Tool" ]; then
    echo "ERROR: Executable not found!"
    exit 1
fi

if [ ! -f "PTSL Beat Tool.app/Contents/Resources/python/ptsl_client.py" ]; then
    echo "ERROR: Python script not found!"
    exit 1
fi

# Bundle Python Framework (if not already done by CMake)
if [ ! -d "PTSL Beat Tool.app/Contents/Frameworks/Python.framework" ]; then
    echo ""
    echo "Bundling Python Framework..."
    cd ..
    ./bundle_python_framework.sh "$BUILD_DIR/PTSL Beat Tool.app"
    cd "$BUILD_DIR"
fi

# Check code signature
echo "Checking code signature..."
codesign_output=$(codesign -dv "PTSL Beat Tool.app" 2>&1)
if echo "$codesign_output" | grep -q "Sweet Justice Sound Ltd"; then
    echo "Code signature verified!"
    echo "$codesign_output" | grep "Authority"
else
    echo "WARNING: Code signature not from expected developer"
    echo "$codesign_output"
fi

# Create release directory
echo ""
echo "Creating release directory..."
cd ..
mkdir -p "$OUTPUT_DIR"

# Copy app to releases directory
echo "Copying app to $OUTPUT_DIR..."
cp -R "$BUILD_DIR/PTSL Beat Tool.app" "$OUTPUT_DIR/"

# Verify the copy
if [ ! -d "$OUTPUT_DIR/PTSL Beat Tool.app" ]; then
    echo "ERROR: Failed to copy app to release directory!"
    exit 1
fi

# Create DMG
echo ""
echo "Creating DMG..."
cd "$OUTPUT_DIR"
hdiutil create -volname "PTSL Beat Tool" -srcfolder "PTSL Beat Tool.app" -ov -format UDZO "PTSL_Beat_Tool.dmg"

# Sign the DMG
echo "Signing DMG..."
codesign --force --sign "Developer ID Application: Sweet Justice Sound Ltd (85DN8C9FPR)" "PTSL_Beat_Tool.dmg"

# Final summary
echo ""
echo "======================================"
echo "Build Complete!"
echo "======================================"
echo ""
echo "Output location: $(pwd)"
echo "  App: PTSL Beat Tool.app"
echo "  DMG: PTSL_Beat_Tool.dmg"
echo ""
echo "Directory contents:"
ls -la
echo ""
echo "To test the app, run:"
echo "  open '$(pwd)/PTSL Beat Tool.app'"
echo ""
echo "To install the app:"
echo "  cp -R '$(pwd)/PTSL Beat Tool.app' /Applications/"