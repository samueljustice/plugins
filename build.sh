#!/bin/bash

echo "======================================"
echo "SammyJs Pitch Flattener Build & Install"
echo "======================================"

echo "Cleaning previous build..."
rm -rf build
mkdir build
cd build

echo "Configuring with CMake (Universal Binary for Intel & Apple Silicon)..."
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="x86_64;arm64"

echo "Building plugins (AU & VST3)..."
cmake --build . --config Release

if [ ! -d "PitchFlattener_artefacts/Release/VST3/SammyJs Pitch Flattener.vst3" ] || [ ! -d "PitchFlattener_artefacts/Release/AU/SammyJs Pitch Flattener.component" ]; then
    echo "Build failed!"
    exit 1
fi

echo "Build successful!"

echo "Code signing plugins..."
# Check if we have a Developer ID certificate
DEVELOPER_ID=$(security find-identity -v -p codesigning | grep "Developer ID Application" | head -1 | awk '{print $2}')

if [ -n "$DEVELOPER_ID" ]; then
    echo "Found Developer ID: $DEVELOPER_ID"
    echo "Signing with hardened runtime for distribution..."
    codesign --force --deep --sign "$DEVELOPER_ID" --options runtime --timestamp "PitchFlattener_artefacts/Release/VST3/SammyJs Pitch Flattener.vst3"
    codesign --force --deep --sign "$DEVELOPER_ID" --options runtime --timestamp "PitchFlattener_artefacts/Release/AU/SammyJs Pitch Flattener.component"
else
    echo "No Developer ID found, using ad-hoc signing..."
    codesign --force --deep --sign - "PitchFlattener_artefacts/Release/VST3/SammyJs Pitch Flattener.vst3"
    codesign --force --deep --sign - "PitchFlattener_artefacts/Release/AU/SammyJs Pitch Flattener.component"
fi

# Verify signing
echo "Verifying code signatures..."
codesign --verify --verbose "PitchFlattener_artefacts/Release/VST3/SammyJs Pitch Flattener.vst3"
codesign --verify --verbose "PitchFlattener_artefacts/Release/AU/SammyJs Pitch Flattener.component"

echo "Installing to system folders..."
echo "Creating plugin directories if they don't exist..."
mkdir -p "/Library/Audio/Plug-Ins/VST3"
mkdir -p "/Library/Audio/Plug-Ins/Components"

echo "Removing old versions..."
rm -rf "/Library/Audio/Plug-Ins/VST3/SammyJs Pitch Flattener.vst3"
rm -rf "/Library/Audio/Plug-Ins/Components/SammyJs Pitch Flattener.component"

echo "Installing VST3..."
cp -R "PitchFlattener_artefacts/Release/VST3/SammyJs Pitch Flattener.vst3" "/Library/Audio/Plug-Ins/VST3/"

echo "Installing AU..."
cp -R "PitchFlattener_artefacts/Release/AU/SammyJs Pitch Flattener.component" "/Library/Audio/Plug-Ins/Components/"

echo "Removing quarantine attributes..."
xattr -dr com.apple.quarantine "/Library/Audio/Plug-Ins/VST3/SammyJs Pitch Flattener.vst3" 2>/dev/null || true
xattr -dr com.apple.quarantine "/Library/Audio/Plug-Ins/Components/SammyJs Pitch Flattener.component" 2>/dev/null || true

echo "Refreshing Audio Component cache..."
killall -9 AudioComponentRegistrar 2>/dev/null || true

echo "======================================"
echo "Installation Complete!"
echo "======================================"
echo "VST3: /Library/Audio/Plug-Ins/VST3/"
echo "AU: /Library/Audio/Plug-Ins/Components/"
echo ""
echo "Verifying architectures:"
lipo -info "/Library/Audio/Plug-Ins/VST3/SammyJs Pitch Flattener.vst3/Contents/MacOS/SammyJs Pitch Flattener" 2>/dev/null
lipo -info "/Library/Audio/Plug-Ins/Components/SammyJs Pitch Flattener.component/Contents/MacOS/SammyJs Pitch Flattener" 2>/dev/null
echo ""
echo "Please restart your DAW."