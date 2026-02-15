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
