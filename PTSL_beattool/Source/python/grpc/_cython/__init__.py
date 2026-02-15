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
