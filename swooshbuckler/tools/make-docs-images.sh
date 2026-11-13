#!/bin/sh
# Draws every picture the manual uses, straight into docs/ and docs/images/.
#
#   tools/make-docs-images.sh [build dir]
#
# It builds SwooshbucklerUiShot if it is not there, renders the real editor offline with the demo sounds playing
# (nothing of yours is read or written: the tool keeps its own settings and recordings in a scratch folder), and
# writes the pictures at twice the window's size, so they stay sharp on a high-resolution screen while the manual
# shows them at half width.
#
# Run it after changing anything you can see, and look at what it wrote before committing. The tool checks as it
# draws - every text has to fit its box, be big enough, sit on no other text and follow the one rule for capitals -
# and stops with an error if one does not.

set -e

here=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-"$here/build"}
out=$(mktemp -d)
# Render Variations shows where its files would go, so the tool is given a short, plain scratch folder whose
# recordings folder is called Desktop: the line in that picture then reads like anyone else's.
work=${SWOOSHBUCKLER_DOCS_WORK:-/tmp/Swooshbuckler Docs}
trap 'rm -rf "$out"' EXIT

shot="$build/SwooshbucklerUiShot_artefacts/Release/SwooshbucklerUiShot"
[ -x "$shot" ] || shot="$build/SwooshbucklerUiShot_artefacts/SwooshbucklerUiShot"
if [ ! -x "$shot" ]; then
    cmake -S "$here" -B "$build" -DSWOOSHBUCKLER_TOOLS=ON
    cmake --build "$build" --target SwooshbucklerUiShot --parallel 8
    shot="$build/SwooshbucklerUiShot_artefacts/Release/SwooshbucklerUiShot"
    [ -x "$shot" ] || shot="$build/SwooshbucklerUiShot_artefacts/SwooshbucklerUiShot"
fi

rm -rf "$work"
"$shot" --out "$out" --scale 2.0 --docs --work "$work"
rm -rf "$work"

mkdir -p "$here/docs/images"
mv "$out/screenshot.png" "$here/docs/screenshot.png"
for f in header tracks track-lane track-master ride throw-keys bus-a bus-b hits output sea \
         morph-pad morph-rope modulation cables settings shortcuts takes variations about; do
    mv "$out/$f.png" "$here/docs/images/$f.png"
done

echo "Written to $here/docs. Look at them before you commit."
