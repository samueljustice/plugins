#!/bin/sh
# Runs dehonk_realmix in one mode over mixes listed in corpus/library/sets.txt (one set per line,
# tab separated: mix, dialogue stem, then anything else).
#   tools/realmix_library.sh key|surround|snap|diff [pattern]
# With a pattern (as grep -E takes it) only the sets whose mix path matches are run, for example
#   tools/realmix_library.sh snap 'VGA_E3|NFA_EA_Apex|Div_Fantasy|MEA_NOV|EAGLE_E3|S2_DLC2'
# In "key" mode each mix is also given the stem of the set before it as a key that has nothing to
# do with it.
cd "$(dirname "$0")/.." || exit 1
mode="${1:-key}"
pattern="${2:-.}"
previous=""
while IFS="$(printf '\t')" read -r mix dx rest; do
    [ -f "$mix" ] && [ -f "$dx" ] || continue
    if printf '%s\n' "$mix" | grep -Eq "$pattern"; then
        case "$mode" in
            diff) ./build/dehonk_realmix diff "$mix" ;;
            key)  ./build/dehonk_realmix key "$mix" "$dx" ${previous:+"$previous"} ;;
            *)    ./build/dehonk_realmix "$mode" "$mix" "$dx" ;;
        esac
    fi
    previous="$dx"
done < corpus/library/sets.txt
