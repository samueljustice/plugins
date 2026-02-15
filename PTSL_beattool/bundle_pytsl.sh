#!/bin/bash
# Bundle py-ptsl into the app with all binary dependencies
APP_PYTHON_DIR="$1"

if [ -z "$APP_PYTHON_DIR" ]; then
    echo "Usage: $0 <app_python_directory>"
    exit 1
fi

echo "Bundling py-ptsl to $APP_PYTHON_DIR"

# Get Python site-packages directory
SITE_PACKAGES=$(python3 -c 'import site; print(site.getsitepackages()[0])')
echo "Site packages: $SITE_PACKAGES"

# Find py-ptsl
PTSL_PATH=$(python3 -c 'import ptsl; import os; print(os.path.dirname(ptsl.__file__))' 2>/dev/null)

if [ -z "$PTSL_PATH" ]; then
    echo "ERROR: py-ptsl not found. Installing..."
    pip3 install py-ptsl
    PTSL_PATH=$(python3 -c 'import ptsl; import os; print(os.path.dirname(ptsl.__file__))' 2>/dev/null)
fi

if [ -d "$PTSL_PATH" ]; then
    # Use rsync to preserve everything including symlinks and binary files
    rsync -a "$PTSL_PATH" "$APP_PYTHON_DIR/"
    echo "Bundled py-ptsl from $PTSL_PATH"
    
    # Bundle all required dependencies
    echo "Bundling dependencies..."
    
    # grpcio and its components
    if [ -d "$SITE_PACKAGES/grpc" ]; then
        rsync -a "$SITE_PACKAGES/grpc" "$APP_PYTHON_DIR/"
        echo "Bundled grpc"
    fi
    
    # google package (needed for protobuf)
    if [ -d "$SITE_PACKAGES/google" ]; then
        rsync -a "$SITE_PACKAGES/google" "$APP_PYTHON_DIR/"
        echo "Bundled google"
    fi
    
    # protobuf (check both possible locations)
    if [ -d "$SITE_PACKAGES/protobuf" ]; then
        rsync -a "$SITE_PACKAGES/protobuf" "$APP_PYTHON_DIR/"
        echo "Bundled protobuf"
    elif [ -d "$SITE_PACKAGES/google/protobuf" ]; then
        # Sometimes protobuf is installed under google package
        mkdir -p "$APP_PYTHON_DIR/google"
        rsync -a "$SITE_PACKAGES/google/protobuf" "$APP_PYTHON_DIR/google/"
        echo "Bundled google.protobuf"
    fi
    
    # Copy dist-info directories for proper package recognition
    for distinfo in "$SITE_PACKAGES"/grpcio*.dist-info "$SITE_PACKAGES"/protobuf*.dist-info; do
        if [ -d "$distinfo" ]; then
            rsync -a "$distinfo" "$APP_PYTHON_DIR/"
            echo "Bundled $(basename $distinfo)"
        fi
    done
    
    # Find and copy the cygrpc extension specifically
    echo "Looking for compiled extensions..."
    find "$SITE_PACKAGES/grpc" -name "*.so" | while read so_file; do
        rel_path="${so_file#$SITE_PACKAGES/}"
        target_dir="$APP_PYTHON_DIR/$(dirname $rel_path)"
        mkdir -p "$target_dir"
        cp "$so_file" "$target_dir/"
        echo "Copied extension: $rel_path"
    done
    
    # Create a special __init__.py for grpc._cython that will load cygrpc
    mkdir -p "$APP_PYTHON_DIR/grpc/_cython"
    cat > "$APP_PYTHON_DIR/grpc/_cython/__init__.py" << 'EOF'
# Auto-generated file to help load cygrpc extension

import sys
import os

# Get the directory containing this file
_this_dir = os.path.dirname(os.path.abspath(__file__))

# Ensure this directory is in sys.path for loading .so files
if _this_dir not in sys.path:
    sys.path.insert(0, _this_dir)

# Try different ways to import cygrpc
try:
    # First try relative import
    from . import cygrpc
except ImportError as e1:
    try:
        # Try absolute import with the .so extension name
        import importlib.util
        cygrpc_path = os.path.join(_this_dir, 'cygrpc.cpython-313-darwin.so')
        if os.path.exists(cygrpc_path):
            spec = importlib.util.spec_from_file_location("cygrpc", cygrpc_path)
            cygrpc = importlib.util.module_from_spec(spec)
            sys.modules['cygrpc'] = cygrpc
            sys.modules['grpc._cython.cygrpc'] = cygrpc
            spec.loader.exec_module(cygrpc)
        else:
            raise ImportError(f"Could not find cygrpc.so at {cygrpc_path}")
    except ImportError as e2:
        # Last resort - try direct import
        import cygrpc
        sys.modules['grpc._cython.cygrpc'] = cygrpc

# Make cygrpc available when importing from grpc._cython
__all__ = ['cygrpc']
EOF
    
    # Set up proper Python path in a wrapper script
    cat > "$APP_PYTHON_DIR/setup_paths.py" << 'EOF'
import sys
import os

# Add bundled packages to Python path
bundle_dir = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, bundle_dir)

# Set up proper imports for bundled grpc
os.environ['GRPC_PYTHON_BUILD_WITH_CYTHON'] = '1'

# Add the grpc/_cython directory specifically for .so loading
cython_dir = os.path.join(bundle_dir, 'grpc', '_cython')
if os.path.exists(cython_dir) and cython_dir not in sys.path:
    sys.path.insert(0, cython_dir)

# Force the grpc._cython module to be imported early
try:
    import grpc._cython
except ImportError:
    pass
EOF
    
else
    echo "ERROR: Could not find py-ptsl"
    exit 1
fi

echo "Bundle complete. Contents of $APP_PYTHON_DIR:"
ls -la "$APP_PYTHON_DIR/"