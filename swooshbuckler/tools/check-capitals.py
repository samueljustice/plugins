#!/usr/bin/env python3
"""Reads every user-facing string literal in Source/ and flags the ones that break the one capitalisation rule.

The rule: Title Case for control names, knob captions, buttons, menus and their items, tags, chips and panel
titles ("Low End", "Skip Silence", "Energy Bed", "Take Turns"); sentence case only for full sentences (panel
subtitles, hints, tooltips, status messages). No ALL-CAPS labels anywhere except single-letter chips (A, B, M, S)
and the unit-less acronyms below.

This is a reading aid, not a gate: SwooshbucklerUi checks what is actually drawn, with its real box. Run it from
anywhere:  python3 tools/check-capitals.py [source dir]
"""

import re
import sys
from pathlib import Path

ALLOWED_CAPS = {
    "GR", "LFO", "MIDI", "CC", "WAV", "AIFF", "FLAC", "MP3", "CPU", "DAW", "AU", "VST3", "AAX", "DC", "ID",
    "A", "B", "M", "S", "H", "L", "R", "T", "F", "K", "I", "OK", "UI", "US", "PDF", "SVG", "PNG", "FFT",
    "RMS", "EQ", "HZ", "DB", "JUCE", "SDK", "URL", "HTML", "CSV", "II", "III", "IV",
}

# Words that stay lower-case inside a Title Case name.
MINOR = {"a", "an", "the", "and", "or", "of", "to", "in", "on", "for", "per", "with", "at", "by", "from", "as", "is"}

LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
WORD = re.compile(r"[A-Za-z][A-Za-z'\-]*")


# Where a literal is a name the user reads as a label, not a fragment of a sentence being built up.
NAMES_A_CONTROL = re.compile(
    r"setButtonText|setText\(|addItem|addSectionHeader|addSubMenu|setTooltip|caption\s*=|"
    r"juce::TextButton|juce::ToggleButton|juce::ComboBox|setName\(|->name\s*=|\.name\s*="
)


def is_sentence(text: str) -> bool:
    """A full sentence (and so sentence case): it ends in . ! or ? or runs over six words."""
    stripped = text.strip()
    return stripped.endswith((".", "!", "?")) or len(WORD.findall(stripped)) > 6


def is_fragment(text: str) -> bool:
    """Part of a line being joined together, not a label of its own."""
    return text != text.strip() or text.endswith((":", ",", "-"))


def check(text: str, labelled: bool):
    """What is wrong with this label, if anything."""
    faults = []
    words = WORD.findall(text)
    if not words:
        return faults
    for word in words:
        if word.isupper() and len(word) > 1 and word not in ALLOWED_CAPS:
            faults.append(f"ALL-CAPS word '{word}'")
    # Only a literal that is plainly a label of its own is held to Title Case: a fragment being joined into a
    # sentence is checked by what is drawn (SwooshbucklerUi), with its real box and its real wording.
    if labelled and not is_sentence(text) and not is_fragment(text):
        for index, word in enumerate(words):
            if index == 0 and word[0].islower():
                faults.append(f"starts lower-case: '{word}'")
            elif index > 0 and word[0].islower() and word.lower() not in MINOR:
                faults.append(f"lower-case word in a name: '{word}'")
    return faults


def main() -> int:
    root = Path(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent / "Source")
    total = 0
    for path in sorted(root.rglob("*")):
        if path.suffix not in (".cpp", ".h"):
            continue
        for number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            bare = line.strip()
            if bare.startswith("//") or bare.startswith("*") or bare.startswith("/*"):
                continue
            if "#include" in bare:
                continue
            for match in LITERAL.finditer(line):
                text = match.group(1)
                if len(text) < 2 or "\\n" in text or "%" in text:
                    continue  # tooltips and formats are checked by what is drawn, not here
                if not re.search(r"[A-Za-z]", text):
                    continue
                for fault in check(text, bool(NAMES_A_CONTROL.search(line))):
                    print(f"{path.name}:{number}: {fault}  ->  \"{text}\"")
                    total += 1
    print(f"{total} to look at")
    return 0


if __name__ == "__main__":
    sys.exit(main())
