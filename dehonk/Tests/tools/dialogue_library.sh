#!/bin/sh
# Runs dehonk_dialogue --csv over every mix and dialogue stem listed in corpus/library/sets.txt
# (one set per line, tab separated: mix, dialogue stem, then anything else), and prints one line
# of figures per set. Extra arguments go to dehonk_dialogue, for example
#   tools/dialogue_library.sh --key-mono-sum > build/dialogue_monosum.csv
# The columns are those of dehonk_dialogue --csv: name, seconds, share of bands of frames that are
# the dialogue's and that are not, then mean cut without and with the key for: the dialogue's
# bands, those of them where a cut was open, mixed bands, bands that are not dialogue, those of
# them where a cut was open, the deepest cut in a dialogue band per frame; then the key's trim at
# the 10th, 50th and 90th percentile.
cd "$(dirname "$0")/.." || exit 1
while IFS="$(printf '\t')" read -r mix dx rest; do
    [ -f "$mix" ] && [ -f "$dx" ] || continue
    ./build/dehonk_dialogue "$mix" "$dx" --csv "$@" 2>/dev/null
done < corpus/library/sets.txt
