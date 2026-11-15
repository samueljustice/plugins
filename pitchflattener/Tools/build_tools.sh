#!/bin/sh
# Builds the listening-test tools without CMake. Run from the repository root.
#   FlattenFile   flattens one WAV with the plug-in's core
#   specplot      draws a spectrogram of a WAV with tracked pitch lines on top
set -e
P=pitchflattener
clang++ -std=c++20 -O2 -Wall -Wextra -I$P/Source -I$P/Tools $P/Tools/FlattenFile.cpp $P/Source/core/Flatten.cpp $P/Source/core/HarmonicTracker.cpp \
  $P/Source/core/HindsightTracker.cpp $P/Source/core/RidgeTracker.cpp -o $P/Tools/FlattenFile
clang++ -std=c++20 -O2 -I$P/Tools $P/Tools/specplot.cpp -lz -o $P/Tools/specplot
