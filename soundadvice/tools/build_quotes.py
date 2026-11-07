#!/usr/bin/env python3
"""Builds Resources/quotes.json (the database embedded in the plug-in) from data/*.json.

Each data file holds {"people": [...], "entries": [...]} for one research group. This script
merges them, checks every entry, gives each a stable id and writes one compact file.

    python3 tools/build_quotes.py            # build
    python3 tools/build_quotes.py --check    # check only, write nothing

People named in data/omit.txt are left out.
"""

import hashlib
import json
import re
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DATA = ROOT / "data"
OUT = ROOT / "Resources" / "quotes.json"

CATEGORIES = [
    ("mixing", "Mixing"),
    ("action", "Action & Impact"),
    ("trailers", "Trailers"),
    ("creatures", "Creatures & Voices"),
    ("worlds", "Ambience & Worlds"),
    ("foley", "Foley"),
    ("dialogue", "Dialogue"),
    ("recording", "Field Recording"),
    ("weapons", "Weapons & Vehicles"),
    ("scifi", "Sci-Fi & Magic"),
    ("horror", "Horror & Tension"),
    ("silence", "Silence & Restraint"),
    ("music", "Music & Sound"),
    ("games", "Games"),
    ("story", "Story & Emotion"),
    ("craft", "Craft & Technique"),
    ("collaboration", "Collaboration & Career"),
    ("creativity", "Creativity & Teams"),
]
CATEGORY_IDS = {c for c, _ in CATEGORIES}
MAX_QUOTE_WORDS = 15
IMDB = re.compile(r"^https://(www\.|m\.)?imdb\.com/name/nm\d+/?$")


def clean(text):
    text = re.sub(r"\s+", " ", str(text or "")).strip()
    # Quotation marks are drawn by the plug-in, so strip any that came with the words.
    return text.strip("\"'“”‘’ ")


def smarten(text):
    """Typographic quotes and apostrophes, and an en dash for spaced hyphens."""
    text = re.sub(r'(^|[\s(\[])"', "\\1\u201c", text)
    text = text.replace('"', "\u201d")
    text = re.sub(r"(^|[\s(\[])'", "\\1\u2018", text)
    text = text.replace("'", "\u2019")
    return text.replace(" - ", " \u2013 ").replace("--", "\u2013")


def finish_quote(text):
    """Close a cut cleanly: no dangling comma, a full stop if needed, inner quotes balanced."""
    text = text.rstrip(" ,;:\u2013\u2014-")
    closer = ""
    m = re.search(r"[.!?\u2026\u2019\u201d]+$", text)
    if m:
        closer, text = m.group(0), text[: m.start()]
    if text.count("\u2018") > text.count("\u2019") - len(re.findall(r"\w\u2019\w", text)) - len(re.findall(r"s\u2019(\s|$)", text)):
        closer = closer.replace("\u2019", "")
        if not re.search(r"[.!?\u2026]", closer):
            closer = "." + closer
        closer += "\u2019"
    elif not re.search(r"[.!?\u2026]", closer):
        closer = "." + closer if not closer else closer
    return text + closer


def short_credits(credits, limit=3):
    parts = [c.strip() for c in re.split(r"[;,]\s*", credits) if c.strip()]
    return ", ".join(parts[:limit])


def norm(text):
    return re.sub(r"[^a-z0-9]+", "", text.lower())


def main(check_only):
    people, entries, problems = {}, [], []
    seen_text = set()
    omit_file = DATA / "omit.txt"
    omit = set()
    if omit_file.exists():
        omit = {l.strip() for l in omit_file.read_text(encoding="utf-8").splitlines() if l.strip() and not l.startswith("#")}
    omitted = Counter()

    for path in sorted(DATA.glob("*.json")):
        data = json.loads(path.read_text(encoding="utf-8"))
        for p in data.get("people", []):
            name = clean(p.get("name"))
            imdb = (p.get("imdb") or "").strip()
            if "imdb.com" in imdb:
                if not IMDB.match(imdb):
                    problems.append(f"{path.name}: {name}: odd IMDb url {imdb}")
                if not imdb.endswith("/"):
                    imdb += "/"
                imdb = imdb.replace("://m.imdb", "://www.imdb").replace("://imdb", "://www.imdb")
            elif imdb and not imdb.startswith("https://"):
                problems.append(f"{path.name}: {name}: profile link should be https: {imdb}")
            record = {"name": name, "role": clean(p.get("role")), "credits": short_credits(clean(p.get("credits"))), "imdb": imdb}
            if name in people:
                # Same person researched twice: keep the fuller record.
                old = people[name]
                for k, v in record.items():
                    if not old.get(k) and v:
                        old[k] = v
            else:
                people[name] = record

        for e in data.get("entries", []):
            who = clean(e.get("person"))
            text = clean(e.get("text"))
            kind = e.get("kind")
            cats = [c for c in dict.fromkeys(e.get("categories") or []) if c in CATEGORY_IDS]
            src = e.get("source") or {}
            where = f"{path.name}: {who}: {text[:50]!r}"

            if who in omit:
                omitted[who] += 1
                continue

            if who not in people:
                problems.append(f"{where}: person not in people list")
                continue
            if kind not in ("quote", "insight"):
                problems.append(f"{where}: kind {kind!r}")
                continue
            if not cats:
                problems.append(f"{where}: no known category in {e.get('categories')}")
                continue
            if not (src.get("url") or "").startswith("http"):
                problems.append(f"{where}: no source url")
                continue
            if kind == "quote" and len(text.split()) > MAX_QUOTE_WORDS:
                problems.append(f"{where}: quote is {len(text.split())} words (max {MAX_QUOTE_WORDS})")
                continue
            if norm(text) in seen_text:
                continue  # the same words gathered twice
            seen_text.add(norm(text))

            year = src.get("year")
            try:
                year = int(year) if year not in (None, "") else None
            except (TypeError, ValueError):
                year = None

            surname = norm(who.split()[-1]) or "x"
            digest = hashlib.sha1(f"{who}|{text}".encode()).hexdigest()[:6]
            entries.append({
                "id": f"{surname}-{digest}",
                "person": who,
                "kind": kind,
                "text": finish_quote(smarten(text[:1].upper() + text[1:])) if kind == "quote" else smarten(text),
                "categories": cats,
                "source": {
                    "title": clean(src.get("title")),
                    "publication": clean(src.get("publication")),
                    "url": src["url"].strip(),
                    "year": year,
                },
            })

    used = {e["person"] for e in entries}
    db = {
        "categories": [{"id": c, "name": n} for c, n in CATEGORIES],
        "people": sorted((p for p in people.values() if p["name"] in used), key=lambda p: p["name"]),
        "entries": entries,
    }

    counts = Counter(c for e in entries for c in e["categories"])
    kinds = Counter(e["kind"] for e in entries)
    print(f"{len(entries)} entries ({kinds['quote']} quotes, {kinds['insight']} field notes) from {len(db['people'])} people")
    for c, n in CATEGORIES:
        print(f"  {n:<24} {counts[c]}")
    for who in sorted(omit):
        print(f"omitted {who} ({omitted[who]} entries)" if omitted[who] else f"omit.txt names {who!r}, who has no entries")
    if problems:
        print(f"\n{len(problems)} entries skipped:")
        for p in problems:
            print("  " + p)

    if not check_only:
        OUT.write_text(json.dumps(db, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
        print(f"\nwrote {OUT.relative_to(ROOT)} ({OUT.stat().st_size // 1024} KB)")


if __name__ == "__main__":
    main("--check" in sys.argv)
