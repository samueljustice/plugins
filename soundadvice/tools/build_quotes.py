#!/usr/bin/env python3
"""Builds Resources/quotes.json (the database embedded in the plug-in) from data/*.json.

Each data file holds {"people": [...], "entries": [...]} for one research group. This script
merges them, checks every entry, gives each a stable id and writes one compact file.

    python3 tools/build_quotes.py            # build
    python3 tools/build_quotes.py --check    # check only, write nothing

data/credits.json overrides people's credit lines. People named in data/omit.txt are left out, and so are entries whose ids are in
data/omit-entries.txt (one id per line; anything after # is a comment).
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
# Categories taken out of the plug-in: entries whose primary category is one of these are left out,
# and the tag is dropped from the rest.
DROPPED_CATEGORIES = {"foley"}
# Only advice from 2005 on: older interviews and essays are left out. Undated pages from these sites
# are known to be older material (1990s filmsound.org essays, a retirement-era bio).
MIN_YEAR = 2005
OLD_UNDATED_SITES = ("filmsound.org", "olli.berkeley.edu")
MAX_QUOTE_WORDS = 15
IMDB = re.compile(r"^https://(www\.|m\.)?imdb\.com/name/nm\d+/?$")


def id_text(text):
    """The text an entry's id is hashed from. Frozen: it strips every outer quote mark, as the
    original clean() did, so ids (and everything keyed on them) never change."""
    return re.sub(r"\s+", " ", str(text or "")).strip().strip("\"'“”‘’ ")


def clean(text):
    text = re.sub(r"\s+", " ", str(text or "")).strip()
    # Quotation marks are drawn by the plug-in, so strip a pair that wraps the whole text. A quote mark
    # that belongs to the words (like the opening of '"No" is a lazy answer') stays.
    quotes = "\"'“”‘’"
    while len(text) > 1 and text[0] in quotes and text[-1] in quotes and not any(q in text[1:-1] for q in quotes if q not in "'’"):
        text = text[1:-1].strip()
    return text


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
    by_imdb, alias = {}, {}
    seen_text = set()
    omit_file = DATA / "omit.txt"
    omit = set()
    if omit_file.exists():
        omit = {l.strip() for l in omit_file.read_text(encoding="utf-8").splitlines() if l.strip() and not l.startswith("#")}
    omitted = Counter()
    omit_entries_file = DATA / "omit-entries.txt"
    omit_ids = set()
    if omit_entries_file.exists():
        for line in omit_entries_file.read_text(encoding="utf-8").splitlines():
            line = line.split("#", 1)[0].strip()
            if line:
                omit_ids.add(line)
    cut_ids, all_ids = set(), set()
    # data/rewrites.json: {"<entry id>": "new text"}. Edits wording (e.g. to name who did it) while the
    # id stays tied to the original research text, so omit lists and saved sessions keep working.
    rewrites_file = DATA / "rewrites.json"
    rewrites = json.loads(rewrites_file.read_text(encoding="utf-8")) if rewrites_file.exists() else {}
    rewritten = set()
    # data/contexts.json: {"<entry id>": "War Horse"}. A quote can't be reworded, so when it's about a
    # specific production, the card shows "ON WAR HORSE" above it.
    contexts_file = DATA / "contexts.json"
    contexts = json.loads(contexts_file.read_text(encoding="utf-8")) if contexts_file.exists() else {}

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
            # The same IMDb page under two spellings ("Gary A. Rizzo" / "Gary Rizzo") is one person:
            # keep the name seen first and map the other onto it.
            if "imdb.com" in imdb and imdb in by_imdb and by_imdb[imdb] != name:
                alias[name] = by_imdb[imdb]
                continue
            if "imdb.com" in imdb:
                by_imdb.setdefault(imdb, name)
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
            who = alias.get(who, who)
            text = clean(e.get("text"))
            key_text = id_text(e.get("text"))
            all_ids.add(f"{norm(who.split()[-1]) or 'x'}-{hashlib.sha1(f'{who}|{key_text}'.encode()).hexdigest()[:6]}")
            kind = e.get("kind")
            raw_cats = list(dict.fromkeys(e.get("categories") or []))
            if raw_cats and raw_cats[0] in DROPPED_CATEGORIES:
                omitted["category " + raw_cats[0]] += 1
                continue
            cats = [c for c in raw_cats if c in CATEGORY_IDS]
            src = e.get("source") or {}
            where = f"{path.name}: {who}: {text[:50]!r}"

            if who in omit:
                omitted[who] += 1
                continue

            surname = norm(who.split()[-1]) or "x"
            digest = hashlib.sha1(f"{who}|{key_text}".encode()).hexdigest()[:6]
            entry_id = f"{surname}-{digest}"
            if entry_id in omit_ids:
                cut_ids.add(entry_id)
                continue
            if entry_id in rewrites:
                text = clean(rewrites[entry_id])
                rewritten.add(entry_id)

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
            if (year is not None and year < MIN_YEAR) or (year is None and any(site in src["url"] for site in OLD_UNDATED_SITES)):
                omitted["before " + str(MIN_YEAR)] += 1
                continue

            record_context = clean(contexts.get(entry_id, "")) if kind == "quote" else ""
            entries.append({
                "id": entry_id,
                "context": record_context,
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

    # data/credits.json overrides a person's on-screen credits ({"name": "credits"}), e.g. to show
    # only work from 2005 on. An empty string shows the role alone.
    credits_file = DATA / "credits.json"
    if credits_file.exists():
        for name, credits in json.loads(credits_file.read_text(encoding="utf-8")).items():
            if name in people:
                people[name]["credits"] = clean(credits)
            else:
                problems.append(f"credits.json: unknown person {name}")

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
    if omitted["before " + str(MIN_YEAR)]:
        print(f"omitted {omitted['before ' + str(MIN_YEAR)]} entries from sources before {MIN_YEAR}")
    for key in sorted(k for k in omitted if k.startswith("category ")):
        print(f"omitted {omitted[key]} entries whose main category is {key[9:]}")
    for who in sorted(omit):
        print(f"omitted {who} ({omitted[who]} entries)" if omitted[who] else f"omit.txt names {who!r}, who has no entries")
    if contexts:
        print(f"{sum(1 for e in entries if e.get('context'))} quotes have a context line")
        for stale in sorted(set(contexts) - all_ids):
            print(f"  contexts.json lists {stale}, which no longer exists")
    if rewrites:
        print(f"rewrote {len(rewritten)} entries from rewrites.json")
        for stale in sorted(set(rewrites) - all_ids):
            print(f"  rewrites.json lists {stale}, which no longer exists")
    if omit_ids:
        print(f"omitted {len(cut_ids)} entries listed in omit-entries.txt")
        for stale in sorted(omit_ids - all_ids):
            print(f"  omit-entries.txt lists {stale}, which no longer exists")
    if problems:
        print(f"\n{len(problems)} entries skipped:")
        for p in problems:
            print("  " + p)

    if not check_only:
        OUT.write_text(json.dumps(db, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
        print(f"\nwrote {OUT.relative_to(ROOT)} ({OUT.stat().st_size // 1024} KB)")


if __name__ == "__main__":
    main("--check" in sys.argv)
