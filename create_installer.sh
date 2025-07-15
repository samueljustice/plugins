#!/bin/bash

echo "======================================"
echo "Creating PKG Installer"
echo "======================================"

# Check if build exists
if [ ! -d "build/PitchFlattener_artefacts/Release/VST3/SammyJs Pitch Flattener.vst3" ] || [ ! -d "build/PitchFlattener_artefacts/Release/AU/SammyJs Pitch Flattener.component" ]; then
    echo "Error: Plugins not found. Please run ./build.sh first."
    exit 1
fi

# Create installer directories
rm -rf installer
mkdir -p installer/root/Library/Audio/Plug-Ins/VST3
mkdir -p installer/root/Library/Audio/Plug-Ins/Components
mkdir -p installer/scripts
mkdir -p installer/resources

# Copy plugins
echo "Copying plugins..."
cp -R "build/PitchFlattener_artefacts/Release/VST3/SammyJs Pitch Flattener.vst3" "installer/root/Library/Audio/Plug-Ins/VST3/"
cp -R "build/PitchFlattener_artefacts/Release/AU/SammyJs Pitch Flattener.component" "installer/root/Library/Audio/Plug-Ins/Components/"

# Sign the plugins first
DEVELOPER_ID=$(security find-identity -v -p codesigning | grep "Developer ID Application" | head -1 | awk '{print $2}')
if [ -n "$DEVELOPER_ID" ]; then
    echo "Signing plugins with Developer ID and hardened runtime..."
    codesign --force --deep --sign "$DEVELOPER_ID" --options runtime --timestamp "installer/root/Library/Audio/Plug-Ins/VST3/SammyJs Pitch Flattener.vst3"
    codesign --force --deep --sign "$DEVELOPER_ID" --options runtime --timestamp "installer/root/Library/Audio/Plug-Ins/Components/SammyJs Pitch Flattener.component"
else
    echo "Ad-hoc signing plugins..."
    codesign --force --deep --sign - "installer/root/Library/Audio/Plug-Ins/VST3/SammyJs Pitch Flattener.vst3"
    codesign --force --deep --sign - "installer/root/Library/Audio/Plug-Ins/Components/SammyJs Pitch Flattener.component"
fi

# Create postinstall script
cat > installer/scripts/postinstall << 'EOF'
#!/bin/bash

# Remove quarantine
xattr -dr com.apple.quarantine "/Library/Audio/Plug-Ins/VST3/SammyJs Pitch Flattener.vst3" 2>/dev/null || true
xattr -dr com.apple.quarantine "/Library/Audio/Plug-Ins/Components/SammyJs Pitch Flattener.component" 2>/dev/null || true

# Kill AudioComponentRegistrar to refresh AU cache
killall -9 AudioComponentRegistrar 2>/dev/null || true

# Set proper permissions
chmod -R 755 "/Library/Audio/Plug-Ins/VST3/SammyJs Pitch Flattener.vst3"
chmod -R 755 "/Library/Audio/Plug-Ins/Components/SammyJs Pitch Flattener.component"

exit 0
EOF

chmod +x installer/scripts/postinstall

# Create welcome message
cat > installer/resources/welcome.txt << 'EOF'
Welcome to the SammyJs Pitch Flattener installer!

This will install:
• SammyJs Pitch Flattener VST3
• SammyJs Pitch Flattener AU

The plugins will be installed to:
• /Library/Audio/Plug-Ins/VST3/
• /Library/Audio/Plug-Ins/Components/

After installation, restart your DAW to use the plugins.
EOF

# Create readme
cat > installer/resources/readme.txt << 'EOF'
SammyJs Pitch Flattener v2.1.0

A real-time pitch flattening plugin that removes Doppler effects and stabilizes pitch variations.

Features:
• Delta inversion pitch flattening
• Base pitch latching
• Advanced pitch detection with bandpass filtering
• High-quality pitch shifting using RubberBand
• Universal binary for Intel and Apple Silicon

For more information, visit:
https://www.sweetjusticesound.com
EOF

# Create license
cat > installer/resources/license.txt << 'EOF'
SammyJs Pitch Flattener - End User License Agreement

Copyright (c) 2025 Samuel Justice

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software to use it for personal and commercial purposes, subject to
the following conditions:

1. The software is provided "as is", without warranty of any kind.
2. The author shall not be liable for any damages arising from the use of this software.
3. This notice shall be included with all distributions of the software.

This plugin uses the JUCE framework and RubberBand library.
Please refer to their respective licenses for terms of use.
EOF

# Build the component package
echo "Building component package..."
pkgbuild --root installer/root \
         --scripts installer/scripts \
         --identifier "com.samueljustice.pitchflattener.pkg" \
         --version "2.1.0" \
         --install-location "/" \
         installer/SammyJsPitchFlattener.pkg

# Create distribution XML
cat > installer/distribution.xml << 'EOF'
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>SammyJs Pitch Flattener</title>
    <organization>com.samueljustice</organization>
    <domains enable_localSystem="true"/>
    <options customize="never" require-scripts="true" rootVolumeOnly="true" />
    
    <!-- Define documents displayed at various steps -->
    <welcome file="welcome.txt" mime-type="text/plain" />
    <readme file="readme.txt" mime-type="text/plain" />
    <license file="license.txt" mime-type="text/plain" />
    
    <!-- List all component packages -->
    <pkg-ref id="com.samueljustice.pitchflattener.pkg">
        <bundle-version/>
    </pkg-ref>
    
    <!-- List them again here. They can now be organized as a hierarchy if you want. -->
    <choices-outline>
        <line choice="com.samueljustice.pitchflattener.pkg"/>
    </choices-outline>
    
    <!-- Define each choice above -->
    <choice id="com.samueljustice.pitchflattener.pkg" visible="false">
        <pkg-ref id="com.samueljustice.pitchflattener.pkg"/>
    </choice>
    
    <!-- Designate the component packages to install. -->
    <pkg-ref id="com.samueljustice.pitchflattener.pkg">SammyJsPitchFlattener.pkg</pkg-ref>
    
</installer-gui-script>
EOF

# Build the distribution package
echo "Building distribution package..."
productbuild --distribution installer/distribution.xml \
             --resources installer/resources \
             --package-path installer \
             "SammyJs_Pitch_Flattener_Installer.pkg"

# Sign the installer package
INSTALLER_ID=$(security find-identity -v -p basic | grep "Developer ID Installer" | head -1 | awk '{print $2}')
if [ -n "$INSTALLER_ID" ]; then
    echo "Signing installer with Developer ID Installer certificate..."
    productsign --sign "$INSTALLER_ID" \
                "SammyJs_Pitch_Flattener_Installer.pkg" \
                "SammyJs_Pitch_Flattener_Installer_Signed.pkg"
    rm "SammyJs_Pitch_Flattener_Installer.pkg"
    mv "SammyJs_Pitch_Flattener_Installer_Signed.pkg" "SammyJs_Pitch_Flattener_Installer.pkg"
    
    echo "Verifying installer signature..."
    pkgutil --check-signature "SammyJs_Pitch_Flattener_Installer.pkg"
else
    echo "No Developer ID Installer certificate found."
    echo "The installer package is unsigned and will show security warnings."
fi

# Clean up
rm -rf installer

echo ""
echo "======================================"
echo "Installer creation complete!"
echo "======================================"
echo "Installer: SammyJs_Pitch_Flattener_Installer.pkg"
echo ""

if [ -z "$INSTALLER_ID" ]; then
    echo "⚠️  WARNING: Installer is unsigned."
    echo "For signed installers, you need:"
    echo "1. An Apple Developer account"
    echo "2. A Developer ID Installer certificate"
    echo ""
    echo "Users can still install by right-clicking and selecting 'Open'"
fi