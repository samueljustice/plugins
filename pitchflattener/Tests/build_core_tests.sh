#!/bin/sh
# Builds and runs the core tests without CMake. Run from the repository root.
set -e
P=pitchflattener
clang++ -std=c++20 -O2 -Wall -Wextra -I$P/Source $P/Tests/CoreTests.cpp $P/Source/core/Flatten.cpp $P/Source/core/HarmonicTracker.cpp \
  $P/Source/core/HindsightTracker.cpp $P/Source/core/RidgeTracker.cpp -o $P/Tests/CoreTests
$P/Tests/CoreTests
