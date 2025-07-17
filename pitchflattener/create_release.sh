#!/bin/bash

# PitchFlattener Release Builder
# This script builds and packages the plugin for release

set -e  # Exit on error

# Configuration
VERSION="1.2.5"
PLUGIN_NAME="PitchFlattener"
TAG_NAME="pitchflattener-v${VERSION}"

echo "=== PitchFlattener Release Builder v${VERSION} ==="
echo

# Check if we're in the right directory
if [ ! -f "CMakeLists.txt" ]; then
    echo "Error: Must run from the pitchflattener directory"
    exit 1
fi

# Clean previous builds
echo "1. Cleaning previous builds..."
rm -rf build
rm -rf release_artifacts
mkdir -p release_artifacts

# Configure and build
echo "2. Configuring CMake..."
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="x86_64;arm64"

echo "3. Building plugins..."
cmake --build build --config Release -j8

# Create artifact directories
echo "4. Collecting artifacts..."
mkdir -p release_artifacts/macOS-VST3
mkdir -p release_artifacts/macOS-AU

# Copy VST3
if [ -d "build/PitchFlattener_artefacts/Release/VST3" ]; then
    cp -R build/PitchFlattener_artefacts/Release/VST3/*.vst3 release_artifacts/macOS-VST3/
    echo "   ✓ VST3 copied"
else
    echo "   ✗ VST3 not found"
fi

# Copy AU
if [ -d "build/PitchFlattener_artefacts/Release/AU" ]; then
    cp -R build/PitchFlattener_artefacts/Release/AU/*.component release_artifacts/macOS-AU/
    echo "   ✓ AU copied"
else
    echo "   ✗ AU not found"
fi

# Create ZIPs
echo "5. Creating release packages..."
cd release_artifacts

# VST3 package
if [ -d "macOS-VST3" ]; then
    zip -r "${PLUGIN_NAME}-${VERSION}-macOS-VST3.zip" macOS-VST3
    echo "   ✓ VST3 package created"
fi

# AU package
if [ -d "macOS-AU" ]; then
    zip -r "${PLUGIN_NAME}-${VERSION}-macOS-AU.zip" macOS-AU
    echo "   ✓ AU package created"
fi

# Combined package
zip -r "${PLUGIN_NAME}-${VERSION}-macOS-All.zip" macOS-VST3 macOS-AU
echo "   ✓ Combined package created"

cd ..

# Create release notes
echo "6. Creating release notes..."
cat > release_artifacts/RELEASE_NOTES.md << EOF
# PitchFlattener v${VERSION}

## What's New
- Added About dialog with version information and update checking
- Improved UI scaling system with proper proportional scaling
- Fixed tooltip positioning issues with new help text system
- Simplified preset system to only create default preset
- All controls now support double-click to reset to default values
- Hard flatten button repositioned for better layout

## Installation

### macOS
1. Download the appropriate package:
   - \`${PLUGIN_NAME}-${VERSION}-macOS-All.zip\` - Contains both VST3 and AU
   - \`${PLUGIN_NAME}-${VERSION}-macOS-VST3.zip\` - VST3 only
   - \`${PLUGIN_NAME}-${VERSION}-macOS-AU.zip\` - AU only

2. Extract the ZIP file

3. Copy the plugins to your plugin folder:
   - VST3: \`/Library/Audio/Plug-Ins/VST3/\` or \`~/Library/Audio/Plug-Ins/VST3/\`
   - AU: \`/Library/Audio/Plug-Ins/Components/\` or \`~/Library/Audio/Plug-Ins/Components/\`

4. Restart your DAW

## System Requirements
- macOS 10.13 or later
- Universal Binary (Intel + Apple Silicon)

## Technologies Used
- JUCE Framework
- Rubber Band Library (pitch shifting)
- WORLD Vocoder DIO (pitch detection)
- YIN Algorithm (pitch detection)

## Support
- Email: sam@sweetjusticesound.com
- Website: https://www.sweetjusticesound.com
EOF

echo
echo "=== Release preparation complete! ==="
echo
echo "Artifacts created in: release_artifacts/"
ls -la release_artifacts/*.zip
echo
echo "Next steps:"
echo "1. Test the plugins from release_artifacts/"
echo "2. Create and push git tag:"
echo "   git tag ${TAG_NAME}"
echo "   git push origin ${TAG_NAME}"
echo "3. Create GitHub release with the ZIP files"
echo "   OR use GitHub CLI:"
echo "   gh release create ${TAG_NAME} release_artifacts/*.zip --title \"PitchFlattener v${VERSION}\" --notes-file release_artifacts/RELEASE_NOTES.md"
echo