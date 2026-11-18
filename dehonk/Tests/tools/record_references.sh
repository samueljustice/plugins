#!/bin/sh
# tools/record_references.sh <build dir> <arch>: rewrites golden_ref_<arch>.txt and golden_surround_<arch>.txt
# from the reference cores' own tools in that build folder.
set -e
B="$1"; A="$2"; G="$(dirname "$0")/../golden"
"$B/dehonk_golden_ref" "$B/golden_ref.txt"
"$B/dehonk_golden_ref2" --surround "$B/golden_ref2.txt"
{
cat <<H
# De-Honk golden mono and stereo hashes (Tests/src/GoldenCases.h), one "case hash" line each.
# Architecture: $A. Compiler: Apple clang 21.0.0, Release (-O3), as Tests/CMakeLists.txt builds it.
# Recorded from the core as it was before surround support (Tests/build/ref-dsp/, the
# dehonk_golden_ref tool). Mono and stereo output must not change: the golden_mono_stereo test
# fails if this file, or any case in it, is missing or different. Replace it only on purpose,
# from a dehonk_golden_ref build (see Tests/CMakeLists.txt).
# Recorded again from that same core on 18 November 2026, when Diff's cross-fade and level match
# changed (SPEC 13.17): the cases no longer engage Diff ("diff" is gone and "automation" does not
# switch it) and the hash no longer takes in DisplayFrame::diffOffsetDb. Everything else is as it was.
H
cat "$B/golden_ref.txt"
} > "$G/golden_ref_$A.txt"
{
cat <<H
# De-Honk golden surround hashes (Tests/src/GoldenCases.h, runAllSurround), one "case hash" line each.
# Architecture: $A. Compiler: Apple clang 21.0.0, Release (-O3), as Tests/CMakeLists.txt builds it.
# Recorded from the core as it was before Max Cut, Release and Impact Guard became controls
# (Tests/build/ref-dsp2/, the dehonk_golden_ref2 tool): 5.1 and 7.1.4, eleven settings at 48 kHz
# and three at 44.1 and 96 kHz. At Max Cut 8 dB, the release the Speed control used to give and
# Impact Guard 0 the current core must give exactly these: the golden_mono_stereo test fails if
# this file, or any case in it, is missing or different. Replace it only on purpose.
# Recorded again from that same core on 18 November 2026, when Diff's cross-fade and level match
# changed (SPEC 13.17): the cases no longer engage Diff ("diff" is gone and "automation" does not
# switch it) and the hash no longer takes in DisplayFrame::diffOffsetDb. Everything else is as it was.
H
grep "ch_layout_" "$B/golden_ref2.txt"
} > "$G/golden_surround_$A.txt"
wc -l "$G/golden_ref_$A.txt" "$G/golden_surround_$A.txt"
