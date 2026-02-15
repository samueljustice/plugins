#!/bin/bash

# Bundle Python Framework Script
# This script bundles the Python interpreter and all required libraries

set -e

if [ $# -lt 1 ]; then
    echo "Usage: $0 <app_bundle_path>"
    exit 1
fi

APP_BUNDLE="$1"
# Remove any trailing ".." from the path
APP_BUNDLE=$(echo "$APP_BUNDLE" | sed 's/\/\.\.$//')
FRAMEWORKS_DIR="$APP_BUNDLE/Contents/Frameworks"
PYTHON_VERSION="3.13"
PYTHON_FRAMEWORK="/opt/homebrew/opt/python@${PYTHON_VERSION}/Frameworks/Python.framework"

echo "Bundling Python Framework..."

# Create Frameworks directory
mkdir -p "$FRAMEWORKS_DIR"

# Copy Python framework
if [ -d "$PYTHON_FRAMEWORK" ]; then
    echo "Copying Python framework from Homebrew..."
    echo "Source: $PYTHON_FRAMEWORK"
    echo "Destination: $FRAMEWORKS_DIR"
    cp -Rv "$PYTHON_FRAMEWORK" "$FRAMEWORKS_DIR/"
    
    # Update the framework to use relative paths
    PYTHON_DYLIB="$FRAMEWORKS_DIR/Python.framework/Versions/Current/Python"
    
    # Fix the install name
    install_name_tool -id "@executable_path/../Frameworks/Python.framework/Versions/Current/Python" "$PYTHON_DYLIB"
    
    # Create a symlink to python3 in MacOS directory
    mkdir -p "$APP_BUNDLE/Contents/MacOS"
    ln -sf "../Frameworks/Python.framework/Versions/Current/bin/python3" "$APP_BUNDLE/Contents/MacOS/python3"
    
    # Make sure the Python executable has proper permissions
    chmod +x "$FRAMEWORKS_DIR/Python.framework/Versions/Current/bin/python3"
    
    echo "Python framework bundled successfully"
else
    echo "Error: Python framework not found at $PYTHON_FRAMEWORK"
    exit 1
fi

# Update the main executable to link to bundled Python
MAIN_EXECUTABLE="$APP_BUNDLE/Contents/MacOS/PTSL Beat Tool"
if [ -f "$MAIN_EXECUTABLE" ]; then
    # Check if it links to Python
    if otool -L "$MAIN_EXECUTABLE" | grep -q "Python.framework"; then
        echo "Updating main executable to use bundled Python..."
        install_name_tool -change "/opt/homebrew/opt/python@${PYTHON_VERSION}/Frameworks/Python.framework/Versions/Current/Python" \
            "@executable_path/../Frameworks/Python.framework/Versions/Current/Python" \
            "$MAIN_EXECUTABLE"
    fi
fi

echo "Bundle complete!"