#!/usr/bin/env python3
"""Builds a manual page for each plug-in (samueljustice.github.io/plugins/<id>/) from its own README.

    manuals.py <out dir> <owner/repo>

Each plug-in's README stays the only place its manual lives. This has GitHub render it (the markdown API,
with GH_TOKEN when there is one), then wraps it in a page dressed like that plug-in's window: its colours,
its typefaces and the way it marks its panel headings. The plug-ins come from bundle/plugins.txt; one with no
look of its own in THEMES gets the plain one. Pictures stay in the repository and are linked from there.

MANUAL_CACHE=<dir> keeps GitHub's rendering of each README between runs (for working on the looks locally).
"""
import hashlib
import html
import json
import os
import pathlib
import re
import sys
import urllib.request

import releases

out, repo = sys.argv[1:3]
root = pathlib.Path(__file__).resolve().parents[2]
RAW = f"https://raw.githubusercontent.com/{repo}/main/"
BLOB = f"https://github.com/{repo}/blob/main/"
TREE = f"https://github.com/{repo}/tree/main/"

plugins = []  # (id, display name, folder)
for line in (root / "bundle/plugins.txt").read_text(encoding="utf-8").splitlines():
    if line.strip() and not line.startswith("#"):
        fields = line.split("|")
        plugins.append((fields[0].strip(), fields[2].strip(), fields[0].strip()))
# The toys (toys/<id>), from the landing page's Toys list: each gets a page too, with its own installers.
TOYS = {pid for pid, _, _ in releases.toys()}
plugins += releases.toys()
pages = {folder: pid for pid, _, folder in plugins}   # a folder in the repository -> its manual page

# The version of each plug-in in the installer, from the landing page's "every plug-in: A 1.2.3 · B ..." line.
versions = {}
m = re.search(r"every plug-in: (.*?)\.\s*<br", (root / "README.md").read_text(encoding="utf-8"))
if m:
    for item in m.group(1).split("·"):
        v = re.match(r"\s*(.+?)\s+(\d+(?:\.\d+)+)\s*$", item)
        if v:
            versions[v.group(1)] = v.group(2)


def render(markdown: str) -> str:
    cache = os.environ.get("MANUAL_CACHE")
    key = pathlib.Path(cache) / (hashlib.sha1(markdown.encode()).hexdigest() + ".html") if cache else None
    if key and key.exists():
        return key.read_text(encoding="utf-8")
    headers = {"Accept": "application/vnd.github+json", "Content-Type": "application/json"}
    if os.environ.get("GH_TOKEN"):
        headers["Authorization"] = "Bearer " + os.environ["GH_TOKEN"]
    req = urllib.request.Request("https://api.github.com/markdown", headers=headers,
                                 data=json.dumps({"text": markdown, "mode": "markdown"}).encode())
    text = urllib.request.urlopen(req, timeout=60).read().decode("utf-8")
    if key:
        key.parent.mkdir(parents=True, exist_ok=True)
        key.write_text(text, encoding="utf-8")
    return text


def text_of(fragment: str) -> str:
    return html.unescape(re.sub(r"<[^>]+>", "", fragment)).strip()


def resolve(folder: str, url: str) -> str:
    """A path written in <folder>/README.md, as a path from the repository's root ('' if it climbs out of it)."""
    parts = folder.split("/")
    for part in url.split("/"):
        if part == "..":
            if not parts:
                return ""
            parts.pop()
        elif part not in ("", "."):
            parts.append(part)
    return "/".join(parts)


def link(folder: str, url: str) -> str:
    """Where a link in <folder>/README.md goes from its manual page."""
    url = html.unescape(url)
    if url.startswith("#"):
        return url
    if re.match(rf"https://github\.com/{re.escape(repo)}/releases/latest/?$", url):
        return "../#get"                      # "It comes in Sammy J's Plugins": the download buttons
    m = re.match(rf"https://github\.com/{re.escape(repo)}/(?:blob|tree)/main/(.*)$", url)
    if m:
        url, folder = m.group(1), ""
    elif re.match(r"^(?:[a-z][a-z0-9+.-]*:|//)", url):
        return url
    path, _, frag = url.partition("#")
    path = resolve(folder, path) if folder else path.strip("/")
    frag = "#" + frag if frag else ""
    if path in ("", "README.md"):
        return "../" + frag                   # the landing page
    m = re.match(r"^(.+?)(?:/README\.md)?$", path)
    if m and m.group(1) in pages:
        return f"../{pages[m.group(1)]}/{frag}"   # another plug-in's manual
    return (BLOB if "." in path.rsplit("/", 1)[-1] else TREE) + path + frag


def picture(folder: str, url: str) -> str:
    url = html.unescape(url)
    if re.match(r"^(?:[a-z][a-z0-9+.-]*:|//|data:)", url):
        return url
    return RAW + resolve(folder, url)


# ---- the looks ------------------------------------------------------------------------------------------
# Each: Google Fonts families, the plug-in's strapline, its colours and typefaces as CSS variables, and CSS
# of its own for what makes the window look like itself. --k is the colour of a heading's mark; headings are
# given k1..k6 in turn so a theme can colour them as the plug-in colours its panels.
INTER = "Inter:wght@400;500;600"
THEMES = {
    "swooshbuckler": {
        "fonts": ["Pirata+One", "IBM+Plex+Sans+Condensed:ital,wght@0,400;0,500;0,600;1,400"],
        "strap": "A whoosh-making rig",
        "vars": "--bg:#0a1b24; --panel:#0f2a35; --ink:#fff1d6; --dim:#8fa9b0; --line:#1f4654; --a1:#f2b63d; --a2:#3fcab1; --a3:#e8455c; --a4:#ffb169; --link:#f2b63d;"
                " --display:'Pirata One', 'IBM Plex Sans Condensed', serif; --body:'IBM Plex Sans Condensed', system-ui, sans-serif; --mono:ui-monospace, Menlo, Consolas, monospace; --radius:3px; --size:18px;",
        "css": """
  body { background: radial-gradient(1200px 500px at 70% -80px, #123746 0, transparent 70%), var(--bg); }
  .bar { border-bottom: 2px dashed var(--a1); }
  .mark, .title h1 { font-family: var(--display); font-weight: 400; letter-spacing: .02em; }
  .title h1 { font-size: clamp(54px, 9vw, 104px); color: var(--ink); }
  .title .strap { color: var(--a1); }
  .doc h2 { font: 400 clamp(34px, 4.2vw, 46px)/1 var(--display); letter-spacing: .02em; padding-top: 30px; border-top: 2px dashed var(--a1); }
  .doc h2::before { content: ""; display: inline-block; width: .62em; height: .62em; margin-right: .35em; border-radius: 3px; background: var(--k); }
  .doc h3 { font: 400 28px/1.1 var(--display); letter-spacing: .02em; color: var(--a1); }
  .doc h4 { font: 600 15px/1.3 var(--body); letter-spacing: .08em; text-transform: uppercase; color: var(--a2); }
  .k1 { --k: var(--a2); } .k2 { --k: var(--a1); } .k3 { --k: var(--a3); } .k4 { --k: var(--a4); } .k5 { --k: var(--a2); } .k6 { --k: var(--a3); }
  .doc th { color: var(--a1); }
  .toc a.on { color: var(--a1); border-color: var(--a1); }
  .button { background: var(--a1); color: #1a1405; border-color: var(--a1); }
""",
    },
    "soundadvice": {
        "fonts": ["Cormorant+Garamond:ital,wght@0,500;0,600;1,500"],
        "strap": "A page of advice",
        "vars": "--bg:#efe8da; --panel:#f6f1e6; --ink:#2b2622; --dim:#7b7166; --line:#cdc3b0; --a1:#9a4a35; --a2:#9a4a35; --a3:#9a4a35; --a4:#9a4a35; --link:#9a4a35;"
                " --display:'Cormorant Garamond', Georgia, serif; --body:'Cormorant Garamond', Georgia, serif; --mono:ui-monospace, Menlo, Consolas, monospace; --radius:0px; --size:21px;",
        "dark": "--bg:#19181b; --panel:#201f23; --ink:#ebe4d5; --dim:#958c80; --line:#3a3736; --a1:#dba55e; --a2:#dba55e; --a3:#dba55e; --a4:#dba55e; --link:#dba55e;",
        "css": """
  body { font-weight: 500; }
  .bar, .toc, .title .strap, .title .version, .doc h3, .doc th, .foot { letter-spacing: .22em; text-transform: uppercase; font-size: 13px; font-weight: 600; }
  .shell { outline: 1px solid var(--line); outline-offset: -14px; }
  .mark { letter-spacing: .28em; text-transform: uppercase; font-size: 13px; }
  .title { text-align: center; }
  .title .version { justify-content: center; }
  .title h1 { font: 500 clamp(52px, 8vw, 92px)/1 var(--display); }
  .title .strap { color: var(--a1); }
  .doc h2 { font: 600 15px/1.4 var(--display); letter-spacing: .3em; text-transform: uppercase; color: var(--a1); text-align: center; padding-top: 12px; }
  .doc h2::after { content: "\\25C7"; display: block; margin: 8px auto 0; width: 120px; font-size: 11px; letter-spacing: 0; line-height: 1;
    background: linear-gradient(var(--a1), var(--a1)) left center / 46px 1px no-repeat, linear-gradient(var(--a1), var(--a1)) right center / 46px 1px no-repeat; }
  .doc h3 { color: var(--ink); }
  .doc em, .doc blockquote { font-style: italic; }
  .doc strong { font-weight: 600; }
  .doc code { font-size: .72em; }
  .doc img { border: 0; box-shadow: 0 12px 40px rgba(60, 40, 20, .18); }
""",
    },
    "subbertone": {
        "fonts": ["Azeret+Mono:wght@400;500;700", INTER],
        "strap": "SammyJs / Subharmonic generator",
        "vars": "--bg:#080b11; --panel:#0f131b; --ink:#dfe6f0; --dim:#7e899b; --line:#1d2531; --a1:#5fd6e8; --a2:#f0688f; --a3:#a68cf5; --a4:#f2aa3d; --a5:#40c2b0; --link:#5fd6e8;"
                " --display:'Azeret Mono', ui-monospace, Menlo, monospace; --body:'Inter', system-ui, sans-serif; --mono:'Azeret Mono', ui-monospace, Menlo, monospace; --radius:8px; --size:17px;",
        "css": """
  .bar, .toc, .title .strap, .title .version, .foot { font-family: var(--display); letter-spacing: .14em; text-transform: uppercase; font-size: 11px; }
  .mark { font: 700 14px/1 var(--display); letter-spacing: .18em; text-transform: uppercase; }
  .title h1 { font: 700 clamp(34px, 6vw, 68px)/1 var(--display); letter-spacing: .14em; text-transform: uppercase; }
  .title::after { content: ""; display: block; height: 34px; margin-top: 26px; opacity: .9;
    background: repeating-linear-gradient(90deg, var(--a5) 0 34px, transparent 34px 40px, var(--a4) 40px 74px, transparent 74px 80px);
    -webkit-mask: linear-gradient(90deg, #000, transparent 85%); mask: linear-gradient(90deg, #000, transparent 85%); border-radius: 4px; max-width: 520px; }
  .doc h2 { font: 700 15px/1.4 var(--display); letter-spacing: .2em; text-transform: uppercase; padding: 14px 16px; background: var(--panel); border: 1px solid var(--line); border-radius: var(--radius); }
  .doc h2::before, .doc h3::before { content: ""; display: inline-block; width: 7px; height: 7px; border-radius: 50%; background: var(--k); margin-right: 12px; vertical-align: middle; position: relative; top: -1px; }
  .doc h3 { font: 500 13px/1.4 var(--display); letter-spacing: .18em; text-transform: uppercase; }
  .doc h3::before { background: var(--dim); }
  .doc h4 { font: 500 12px/1.4 var(--display); letter-spacing: .16em; text-transform: uppercase; color: var(--dim); }
  .k1 { --k: var(--a1); } .k2 { --k: var(--a2); } .k3 { --k: var(--a3); } .k4 { --k: var(--a5); } .k5 { --k: var(--a4); } .k6 { --k: var(--ink); }
  .doc th { font: 500 11px/1.4 var(--display); letter-spacing: .14em; text-transform: uppercase; color: var(--dim); }
  .button { background: #1d4b55; border-color: #2f7684; color: var(--ink); border-radius: 999px; }
""",
    },
    "pitchflattener": {
        "fonts": ["Silkscreen:wght@400;700", "Azeret+Mono:ital,wght@0,400;0,500;0,700;1,500;1,700", INTER],
        "strap": "SammyJs",
        "vars": "--bg:#070d18; --panel:#0d1727; --ink:#e1e9f5; --dim:#8090a8; --line:#22324a; --a1:#ff8a1f; --a2:#c6f03a; --a3:#29d0f2; --a4:#ff8a1f; --link:#29d0f2;"
                " --display:'Silkscreen', 'Azeret Mono', monospace; --body:'Inter', system-ui, sans-serif; --mono:'Azeret Mono', ui-monospace, Menlo, monospace; --radius:0px; --size:17px;",
        "css": """
  body { background: linear-gradient(rgba(41, 208, 242, .045) 1px, transparent 1px) 0 0 / 100% 28px, linear-gradient(90deg, rgba(41, 208, 242, .045) 1px, transparent 1px) 0 0 / 28px 100%, var(--bg); }
  .bar, .toc, .title .strap, .title .version, .foot { font: italic 500 11px/1.5 var(--mono); letter-spacing: .16em; text-transform: uppercase; }
  .mark { font: 400 15px/1 var(--display); letter-spacing: .12em; text-transform: uppercase; transform: skewX(-10deg); }
  .title h1 { font: 400 clamp(30px, 6.4vw, 72px)/1 var(--display); letter-spacing: .1em; text-transform: uppercase; transform: skewX(-10deg); transform-origin: left bottom; }
  .title h1::after { content: ""; display: block; height: 5px; margin-top: 14px; max-width: 560px; background: linear-gradient(90deg, var(--a3) 0 42%, var(--a2) 42%); }
  .doc h2 { font: 400 20px/1.3 var(--display); letter-spacing: .12em; text-transform: uppercase; color: var(--a1); padding: 14px 18px; background: var(--panel);
    border: 1.5px solid var(--a1); clip-path: polygon(0 0, 100% 0, 100% calc(100% - 12px), calc(100% - 12px) 100%, 0 100%); }
  .doc h2::before { content: "///"; margin-right: 12px; letter-spacing: -.12em; font-style: italic; }
  .doc h2.k2, .doc h2.k5 { color: var(--a2); border-color: var(--a2); }
  .doc h2.k3, .doc h2.k6 { color: var(--a3); border-color: var(--a3); }
  .doc h3 { font: italic 700 14px/1.4 var(--mono); letter-spacing: .16em; text-transform: uppercase; color: var(--a2); }
  .doc h3::before { content: ">> "; color: var(--dim); }
  .doc h4 { font: italic 500 12px/1.4 var(--mono); letter-spacing: .16em; text-transform: uppercase; color: var(--a3); }
  .doc strong { color: #fff; }
  .doc th { font: italic 500 11px/1.4 var(--mono); letter-spacing: .14em; text-transform: uppercase; color: var(--a1); }
  .toc a.on { color: var(--a1); border-color: var(--a1); }
  .button { background: var(--a1); border-color: var(--a1); color: #150a00; font: italic 700 13px/1 var(--mono); letter-spacing: .12em; text-transform: uppercase;
    clip-path: polygon(0 0, 100% 0, 100% calc(100% - 9px), calc(100% - 9px) 100%, 0 100%); }
""",
    },
    "reversinator": {
        "fonts": ["Azeret+Mono:wght@400;500;700", INTER],
        "strap": "SammyJs / Real-time reverser",
        "vars": "--bg:#151412; --panel:#22211f; --ink:#ece7dd; --dim:#918b80; --line:#0b0b0a; --a1:#3b9df2; --a2:#62d9a6; --a3:#d9a441; --a4:#dc6450; --a5:#f0609a; --a6:#b79cf0; --link:#3b9df2;"
                " --display:'Azeret Mono', ui-monospace, Menlo, monospace; --body:'Inter', system-ui, sans-serif; --mono:'Azeret Mono', ui-monospace, Menlo, monospace; --radius:3px; --size:17px;",
        "css": """
  .bar, .toc, .title .strap, .title .version, .foot { font-family: var(--display); letter-spacing: .14em; text-transform: uppercase; font-size: 11px; }
  .mark { font: 700 15px/1 var(--display); letter-spacing: .2em; text-transform: uppercase; }
  .title h1 { font: 700 clamp(30px, 6vw, 68px)/1 var(--display); letter-spacing: .14em; text-transform: uppercase; }
  .title h1::before { content: "\\2190"; color: var(--a1); margin-right: .3em; letter-spacing: 0; }
  .doc h2 { font: 700 15px/1.4 var(--display); letter-spacing: .2em; text-transform: uppercase; padding: 16px 34px; border: 1px solid var(--line); border-radius: var(--radius);
    background: radial-gradient(circle at 12px 12px, #6d6a64 0 3px, #111 3.5px 4.5px, transparent 5px), radial-gradient(circle at calc(100% - 12px) 12px, #6d6a64 0 3px, #111 3.5px 4.5px, transparent 5px),
      linear-gradient(#2a2927, #1f1e1c); box-shadow: inset 0 1px 0 rgba(255, 255, 255, .06), 0 2px 6px rgba(0, 0, 0, .5); }
  .doc h2::before { content: ""; display: inline-block; width: 9px; height: 9px; background: var(--k); margin-right: 14px; }
  .doc h3 { font: 700 13px/1.4 var(--display); letter-spacing: .18em; text-transform: uppercase; }
  .doc h4 { font: 500 12px/1.4 var(--display); letter-spacing: .16em; text-transform: uppercase; color: var(--dim); }
  .k1 { --k: var(--a2); } .k2 { --k: var(--a1); } .k3 { --k: var(--a3); } .k4 { --k: var(--a4); } .k5 { --k: var(--a5); } .k6 { --k: var(--a6); }
  .doc th { font: 500 11px/1.4 var(--display); letter-spacing: .14em; text-transform: uppercase; color: var(--dim); }
  .doc td code, .doc p code, .doc li code { background: #0c0c0b; color: var(--a3); }
  .button { background: linear-gradient(#4aa9f7, #2f8be0); border-color: #0b0b0a; color: #06121f; font: 700 12px/1 var(--display); letter-spacing: .14em; text-transform: uppercase; }
""",
    },
    "roarschach": {
        "fonts": ["Marcellus", "Source+Sans+3:ital,wght@0,400;0,600;1,400"],
        "strap": "Sammy J / Modular voice processor",
        "vars": "--bg:#e2d2c9; --panel:#eee3dc; --ink:#1b1514; --dim:#6c5c57; --line:#c5b1a6; --a1:#8e2424; --a2:#8e2424; --a3:#8e2424; --a4:#8e2424; --link:#8e2424;"
                " --display:'Marcellus', Georgia, serif; --body:'Source Sans 3', system-ui, sans-serif; --mono:ui-monospace, Menlo, Consolas, monospace; --radius:4px; --size:18px;",
        "css": """
  body { background: radial-gradient(900px 420px at 18% 40px, rgba(226, 122, 100, .38) 0, transparent 70%), radial-gradient(700px 360px at 78% 120px, rgba(222, 240, 246, .7) 0, transparent 70%), var(--bg); background-repeat: no-repeat; }
  .bar, .toc, .title .strap, .title .version, .foot { letter-spacing: .14em; text-transform: uppercase; font-size: 12px; font-weight: 600; }
  .mark { font: 400 17px/1 var(--display); letter-spacing: .12em; text-transform: uppercase; }
  .title h1 { font: 400 clamp(40px, 7.6vw, 92px)/1 var(--display); letter-spacing: .08em; text-transform: uppercase; }
  .doc { counter-reset: part; }
  .doc h2 { counter-increment: part; font: 400 24px/1.2 var(--display); letter-spacing: .1em; text-transform: uppercase; padding-bottom: 10px; border-bottom: 1px solid var(--ink); }
  .doc h2::before { content: counter(part, decimal-leading-zero); color: var(--a1); margin-right: 14px; padding-left: 20px;
    background: radial-gradient(circle at 6px 50%, var(--a1) 0 5px, transparent 5.5px) no-repeat; }
  .doc h3 { font: 400 17px/1.3 var(--display); letter-spacing: .12em; text-transform: uppercase; }
  .doc h4 { font: 600 13px/1.4 var(--body); letter-spacing: .12em; text-transform: uppercase; color: var(--dim); }
  .doc th { font: 600 12px/1.4 var(--body); letter-spacing: .12em; text-transform: uppercase; }
  .doc img { border-color: var(--line); }
  .button { background: var(--ink); border-color: var(--ink); color: var(--panel); font: 600 13px/1 var(--body); letter-spacing: .14em; text-transform: uppercase; }
""",
    },
    "tonalrecall": {
        "fonts": ["Space+Mono:wght@400;700", "Space+Grotesk:wght@400;500;700"],
        "strap": "SammyJs / Pitch matcher",
        "vars": "--bg:#0a1315; --panel:#0f1c1f; --ink:#d9e6e4; --dim:#70908f; --line:#1e3437; --a1:#45d3c5; --a2:#f2a73d; --a3:#45d3c5; --a4:#f2a73d; --link:#45d3c5;"
                " --display:'Space Mono', ui-monospace, Menlo, monospace; --body:'Space Grotesk', system-ui, sans-serif; --mono:'Space Mono', ui-monospace, Menlo, monospace; --radius:0px; --size:17px;",
        "css": """
  body { background: linear-gradient(rgba(69, 211, 197, .03) 1px, transparent 1px) 0 0 / 100% 4px, var(--bg); }
  .bar, .toc, .title .strap, .title .version, .foot { font-family: var(--display); letter-spacing: .2em; text-transform: uppercase; font-size: 11px; }
  .mark { font: 700 15px/1 var(--display); letter-spacing: .22em; text-transform: uppercase; }
  .title h1 { font: 700 clamp(30px, 6vw, 66px)/1 var(--display); letter-spacing: .14em; text-transform: uppercase; }
  .title h1::after { content: ""; display: block; height: 3px; width: 108px; margin-top: 16px; background: linear-gradient(90deg, var(--a1) 0 48px, transparent 48px 60px, var(--a2) 60px); }
  .doc { counter-reset: part; }
  .doc h2 { counter-increment: part; font: 700 16px/1.4 var(--display); letter-spacing: .2em; text-transform: uppercase; color: var(--k); padding: 16px 20px; border: 1px solid var(--k);
    background: linear-gradient(var(--k), var(--k)) 0 0 / 14px 3px no-repeat, linear-gradient(var(--k), var(--k)) 0 0 / 3px 14px no-repeat,
      linear-gradient(var(--k), var(--k)) 100% 100% / 14px 3px no-repeat, linear-gradient(var(--k), var(--k)) 100% 100% / 3px 14px no-repeat, var(--panel); }
  .doc h2::before { content: counter(part, decimal-leading-zero) "  "; white-space: pre; }
  .k1, .k3, .k5 { --k: var(--a1); } .k2, .k4, .k6 { --k: var(--a2); }
  .doc h3 { font: 700 13px/1.4 var(--display); letter-spacing: .2em; text-transform: uppercase; }
  .doc h4 { font: 400 12px/1.4 var(--display); letter-spacing: .2em; text-transform: uppercase; color: var(--dim); }
  .doc th { font: 400 11px/1.4 var(--display); letter-spacing: .18em; text-transform: uppercase; color: var(--dim); }
  .button { background: var(--a1); border-color: var(--a1); color: #04201d; font: 700 12px/1 var(--display); letter-spacing: .2em; text-transform: uppercase;
    clip-path: polygon(0 0, 100% 0, 100% calc(100% - 9px), calc(100% - 9px) 100%, 0 100%); }
""",
    },
    "doomeq": {
        "fonts": ["Pixelify+Sans:wght@500;700", "IBM+Plex+Mono:wght@400;600", INTER],
        "strap": "Six bands. One demon-infested display.",
        "vars": "--bg:#100e0c; --panel:#1a1613; --ink:#e8dcc0; --dim:#a89870; --line:#2e2822; --a1:#e8b84a; --a2:#d22a1c; --a3:#f4d442; --a4:#e8503c; --link:#f4d442;"
                " --bl:#a49c8a; --bd:#0a0908; --display:'Pixelify Sans', 'IBM Plex Mono', monospace; --body:'Inter', system-ui, sans-serif; --mono:'IBM Plex Mono', ui-monospace, Menlo, monospace; --radius:0px; --size:17px;",
        "css": """
  body { background:
      radial-gradient(1100px 380px at 50% -120px, rgba(210, 42, 28, .28) 0, transparent 70%),
      repeating-linear-gradient(0deg, rgba(255, 255, 255, .018) 0 2px, transparent 2px 4px),
      repeating-linear-gradient(90deg, rgba(0, 0, 0, .25) 0 1px, transparent 1px 64px),
      repeating-linear-gradient(0deg, rgba(0, 0, 0, .25) 0 1px, transparent 1px 64px), var(--bg); }
  .bar, .toc, .title .strap, .title .version, .foot { font: 600 11px/1.5 var(--mono); letter-spacing: .14em; text-transform: uppercase; }
  .bar { border-bottom: 2px solid var(--bd); box-shadow: 0 1px 0 #3a332b; }
  .mark { font: 700 16px/1 var(--display); letter-spacing: .08em; text-transform: uppercase; color: var(--a1) !important; }
  .title h1 { font: 700 clamp(46px, 9vw, 108px)/.95 var(--display); letter-spacing: .02em; text-transform: uppercase;
    background: linear-gradient(180deg, #f4d442 0%, #e8b84a 38%, #d22a1c 72%, #7a120a 100%); -webkit-background-clip: text; background-clip: text; color: transparent;
    filter: drop-shadow(3px 3px 0 #000); }
  .title::after { content: ""; display: block; height: 8px; max-width: 420px; margin-top: 22px;
    background: linear-gradient(90deg, #e8503c 0 16.66%, #f09030 0 33.33%, #eed040 0 50%, #70cc58 0 66.66%, #4cb4e0 0 83.33%, #b078e8 0); box-shadow: 2px 2px 0 #000; }
  .title .strap { color: var(--a2); }
  .doc h2 { font: 700 22px/1.25 var(--display); letter-spacing: .06em; text-transform: uppercase; color: var(--a1); text-shadow: 2px 2px 0 #000;
    padding: 14px 40px; position: relative; border: 3px solid; border-color: var(--bl) var(--bd) var(--bd) var(--bl);
    background: radial-gradient(circle at 14px 50%, #c8bfa8 0 3px, #3a332b 3.5px 4.5px, transparent 5px),
      radial-gradient(circle at calc(100% - 14px) 50%, #c8bfa8 0 3px, #3a332b 3.5px 4.5px, transparent 5px),
      repeating-linear-gradient(45deg, rgba(255, 255, 255, .025) 0 3px, transparent 3px 7px), linear-gradient(#3b3229, #241e19); }
  .doc h2.k2, .doc h2.k5 { color: var(--a3); }
  .doc h2.k3, .doc h2.k6 { color: #e8503c; }
  .doc h3 { font: 700 17px/1.3 var(--display); letter-spacing: .06em; text-transform: uppercase; color: var(--a3); }
  .doc h3::before { content: "\\25B6  "; color: var(--a2); font-size: .8em; white-space: pre; }
  .doc h4 { font: 600 12px/1.4 var(--mono); letter-spacing: .14em; text-transform: uppercase; color: var(--dim); }
  .doc strong { color: #fff3d6; }
  .doc code, .doc kbd { color: var(--a3); background: var(--bd); border-color: #3a332b; }
  .doc img { border: 3px solid; border-color: var(--bl) var(--bd) var(--bd) var(--bl); border-radius: 0; }
  .doc th { font: 600 11px/1.4 var(--mono); letter-spacing: .14em; text-transform: uppercase; color: var(--a1); }
  .doc li::marker { color: var(--a2); }
  .toc a.on { color: var(--a3); border-color: var(--a2); }
  .button { font: 700 13px/1 var(--display); letter-spacing: .08em; text-transform: uppercase; color: #1a0a04; padding: 13px 18px;
    background: linear-gradient(#f4d442, #e8b84a 55%, #c8902a); border: 3px solid; border-color: #fff0a8 #6a4a10 #6a4a10 #fff0a8; border-radius: 0; }
  .button.get-win { background: linear-gradient(#e8503c, #d22a1c 55%, #8a160c); color: #fff3d6; border-color: #ff9a80 #4a0804 #4a0804 #ff9a80; }
""",
    },
}
PLAIN = {
    "fonts": [INTER],
    "strap": "",
    "vars": "--bg:#0f0e0c; --panel:#191815; --ink:#ece7dc; --dim:#9a9488; --line:#2e2b26; --a1:#e0183f; --a2:#e0183f; --a3:#e0183f; --a4:#e0183f; --link:#ece7dc;"
            " --display:'Inter', system-ui, sans-serif; --body:'Inter', system-ui, sans-serif; --mono:ui-monospace, Menlo, Consolas, monospace; --radius:2px; --size:17px;",
    "css": "",
}

# What every manual page shares: the layout and the plain dress of each element, all in the theme's variables.
BASE = """
  * { box-sizing: border-box; }
  html { scroll-behavior: smooth; scroll-padding-top: 76px; }
  body { margin: 0; background: var(--bg); color: var(--ink); font: 400 var(--size)/1.6 var(--body); -webkit-font-smoothing: antialiased; }
  a { color: var(--link); text-underline-offset: 3px; text-decoration-thickness: 1px; }
  .bar { position: sticky; top: 0; z-index: 5; display: flex; align-items: center; gap: 12px 24px; flex-wrap: wrap; padding: 14px max(20px, calc((100vw - 1240px) / 2));
    background: color-mix(in srgb, var(--bg) 90%, transparent); backdrop-filter: blur(8px); border-bottom: 1px solid var(--line); color: var(--dim); font-size: 13px; }
  .bar a { color: var(--dim); text-decoration: none; }
  .bar a:hover { color: var(--ink); }
  .bar .mark { color: var(--ink); margin-right: auto; }
  .shell { max-width: 1240px; margin: 0 auto; padding: 0 20px 80px; display: grid; grid-template-columns: 230px minmax(0, 1fr); gap: 0 56px; }
  .title { grid-column: 1 / -1; padding: 64px 0 44px; }
  .title h1 { margin: 0; font: 700 clamp(40px, 7vw, 84px)/1 var(--display); overflow-wrap: anywhere; }
  .title .strap { margin: 0 0 16px; color: var(--dim); font-size: 13px; }
  .title .version { margin: 20px 0 0; color: var(--dim); font-size: 13px; display: flex; flex-wrap: wrap; align-items: center; gap: 12px 20px; }
  .title { text-wrap: balance; }
  .button { display: inline-block; padding: 12px 18px; border: 1px solid var(--ink); border-radius: var(--radius); background: var(--ink); color: var(--bg); text-decoration: none; font-weight: 600; letter-spacing: inherit; }
  .button:hover { filter: brightness(1.12); }
  .title .get { display: inline-flex; flex-wrap: wrap; gap: 10px; }
  .toc { position: sticky; top: 76px; align-self: start; max-height: calc(100vh - 96px); overflow: auto; font-size: 13px; padding-bottom: 20px; }
  .toc ol { list-style: none; margin: 0; padding: 0; display: flex; flex-direction: column; gap: 2px; }
  .toc a { display: block; padding: 5px 0 5px 12px; color: var(--dim); text-decoration: none; border-left: 2px solid var(--line); line-height: 1.35; }
  .toc a:hover { color: var(--ink); }
  .toc a.on { color: var(--ink); border-color: var(--a1); }
  .doc { min-width: 0; max-width: 46rem; }
  .doc > :first-child { margin-top: 0; }
  .doc p, .doc ul, .doc ol, .doc table, .doc pre, .doc blockquote, .doc details { margin: 0 0 1.05em; }
  .doc h2 { margin: 64px 0 22px; font: 700 28px/1.2 var(--display); text-wrap: balance; }
  .doc h3 { margin: 40px 0 12px; font: 700 20px/1.3 var(--display); text-wrap: balance; }
  .doc h4 { margin: 28px 0 8px; font: 700 16px/1.3 var(--display); }
  .doc ul, .doc ol { padding-left: 1.3em; }
  .doc li { margin: .3em 0; }
  .doc li::marker { color: var(--dim); }
  .doc img { max-width: 100%; height: auto; display: block; border: 1px solid var(--line); border-radius: var(--radius); }
  .doc p > a > img, .doc p > img { margin: 6px 0; }
  .doc p[align="center"] img { margin-inline: auto; }
  .doc td img { display: inline-block; }
  .doc img[src*="img.shields.io"] { display: inline-block; border: 0; border-radius: 0; }
  .doc code, .doc kbd { font: 400 .84em/1.4 var(--mono); background: var(--panel); border: 1px solid var(--line); border-radius: 3px; padding: .1em .35em; }
  .doc pre { background: var(--panel); border: 1px solid var(--line); border-radius: var(--radius); padding: 14px 16px; overflow: auto; }
  .doc :not(pre) > code { overflow-wrap: anywhere; }
  .doc pre code { background: none; border: 0; padding: 0; font-size: 14px; overflow-wrap: normal; }
  .doc .scroll { overflow-x: auto; margin: 0 0 1.05em; border: 1px solid var(--line); border-radius: var(--radius); background: var(--panel); }
  .doc table { border-collapse: collapse; width: 100%; margin: 0; font-size: .92em; }
  .doc th, .doc td { text-align: left; vertical-align: top; padding: 9px 14px; border-bottom: 1px solid var(--line); }
  .doc tr:last-child td { border-bottom: 0; }
  .doc blockquote { margin-left: 0; padding: 2px 0 2px 18px; border-left: 2px solid var(--a1); color: var(--dim); }
  .doc hr { border: 0; border-top: 1px solid var(--line); margin: 40px 0; }
  .doc details { border: 1px solid var(--line); border-radius: var(--radius); padding: 10px 14px; background: var(--panel); }
  .doc summary { cursor: pointer; }
  .foot { max-width: 1240px; margin: 0 auto; padding: 22px 20px 40px; border-top: 1px solid var(--line); color: var(--dim); font-size: 13px; display: flex; flex-wrap: wrap; gap: 8px 24px; }
  .foot a { color: inherit; }
  @media (max-width: 900px) {
    .shell { grid-template-columns: minmax(0, 1fr); }
    .title { padding: 40px 0 28px; }
    .toc { position: static; max-height: none; margin-bottom: 36px; padding: 0; }
    .toc ol { flex-direction: row; flex-wrap: wrap; gap: 6px 8px; }
    .toc a { border: 1px solid var(--line); border-radius: var(--radius); padding: 5px 9px; }
  }
  @media (prefers-reduced-motion: reduce) { html { scroll-behavior: auto; } }
"""

SCRIPT = """
// Marks, in the contents list, the part of the manual you are reading.
(function () {
  var links = Array.prototype.slice.call(document.querySelectorAll('.toc a'));
  if (!links.length || !('IntersectionObserver' in window)) return;
  var heads = links.map(function (a) { return document.getElementById(decodeURIComponent(a.hash.slice(1))); });
  function mark() {
    var at = 0;
    heads.forEach(function (h, i) { if (h && h.getBoundingClientRect().top < 140) at = i; });
    links.forEach(function (a, i) { a.classList.toggle('on', i === at); });
  }
  var waiting = false;
  addEventListener('scroll', function () { if (!waiting) { waiting = true; requestAnimationFrame(function () { waiting = false; mark(); }); } }, { passive: true });
  mark();
})();
"""


def build(pid: str, name: str, folder: str) -> str:
    theme = THEMES.get(pid, PLAIN)
    doc = render((root / folder / "README.md").read_text(encoding="utf-8"))

    # GitHub's headings: <div class="markdown-heading"><hN ...>text</hN><a id="user-content-slug" ...>...</a></div>
    kinds = iter(range(10 ** 6))

    def heading(m: re.Match) -> str:
        level, inner, anchor = m.group(1), m.group(2), m.group(3)
        klass = f' class="k{next(kinds) % 6 + 1}"' if level == "2" else ""
        return f'<h{level} id="{anchor}"{klass}>{inner}</h{level}>'

    doc = re.sub(r'<div class="markdown-heading"[^>]*>\s*<h([1-6])[^>]*>(.*?)</h\1>\s*<a id="user-content-([^"]*)"[^>]*>.*?</a>\s*</div>', heading, doc, flags=re.S)
    doc = re.sub(r'\bid="user-content-', 'id="', doc)

    # The title is drawn by the page; a contents list at the top of the README is the page's own contents list.
    doc = re.sub(r"<h1[^>]*>.*?</h1>", "", doc, count=1, flags=re.S)
    first_h2 = doc.find("<h2")
    lead = doc if first_h2 < 0 else doc[:first_h2]
    for m in re.finditer(r"<ul>(?:(?!<ul>).|<ul>(?:(?!</ul>).)*</ul>)*?</ul>", lead, re.S):
        items = re.findall(r"<li>", m.group(0))
        anchors = re.findall(r'<a href="#', m.group(0))
        if len(items) >= 3 and len(items) == len(anchors) and not text_of(re.sub(r"<a [^>]*>.*?</a>", "", m.group(0), flags=re.S)):
            doc = doc.replace(m.group(0), "", 1)
            break

    doc = re.sub(r'(<a\b[^>]*?\bhref=)"([^"]*)"', lambda m: f'{m.group(1)}"{html.escape(link(folder, m.group(2)))}"', doc)
    doc = re.sub(r'(<(?:img|source|video)\b[^>]*?\bsrc=)"([^"]*)"', lambda m: f'{m.group(1)}"{html.escape(picture(folder, m.group(2)))}"', doc)
    # A picture's own link is to the file in the repository: point it at the picture itself.
    doc = re.sub(r'<a\b[^>]*?href="[^"]*"[^>]*>\s*(<img\b[^>]*?\bsrc="([^"]*)"[^>]*>)\s*</a>',
                 lambda m: f'<a href="{m.group(2)}" target="_blank" rel="noopener">{m.group(1)}</a>', doc)
    doc = re.sub(r'(<img\b[^>]*?)\s*style="[^"]*"', r"\1", doc)
    seen = iter(range(10 ** 6))
    doc = re.sub(r"<img\b", lambda m: "<img" if next(seen) == 0 else '<img loading="lazy"', doc)
    doc = re.sub(r"<table\b.*?</table>", lambda m: f'<div class="scroll">{m.group(0)}</div>', doc, flags=re.S)

    toc = "".join(f'<li><a href="#{a}">{html.escape(text_of(t))}</a></li>'
                  for a, t in re.findall(r'<h2 id="([^"]*)"[^>]*>(.*?)</h2>', doc, re.S))
    title = re.sub(r"^SammyJs\s+", "", name)
    version = versions.get(name, "")
    fonts = "&".join("family=" + f for f in theme["fonts"])
    live = releases.LIVE_SCRIPT % {"repo": repo} if pid in TOYS else ""
    if pid in TOYS:
        # A toy isn't in the installer: its own installers, from its newest release (the page checks again on load).
        prefix = prefixes.get(pid, pid + "-v")
        r = releases.newest(RELEASES, prefix)
        latest = f"https://github.com/{repo}/releases?q={pid}"
        ver = r.get("version", "")
        get_it = (f'<span class="get" id="download" data-release-prefix="{html.escape(prefix)}">'
                  f'<a class="button get-mac" href="{html.escape(r.get("mac") or latest)}">Download for macOS</a>'
                  f'<a class="button get-win" href="{html.escape(r.get("win") or latest)}">Download for Windows</a></span>'
                  f'<span>{"Test build" if r.get("test", True) else "Version"} <span class="ver">{html.escape(ver)}</span></span>'
                  f'<span>A toy: not in the installer</span>')
        get_link = "#download"
    else:
        get_it = (f'<a class="button" href="../#get">Get it in the installer</a>'
                  + (f"<span>Version {html.escape(version)} in the installer</span>" if version else "") + "<span>The manual</span>")
        get_link = "../#get"
    dark = theme.get("dark", "")
    first_p = re.search(r"<p>(.*?)</p>", doc, re.S)
    described = text_of(first_p.group(1)) if first_p else f"The manual for {title}."
    described = described if len(described) <= 300 else described[:297].rsplit(" ", 1)[0] + "..."

    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{html.escape(title)}: the manual</title>
<meta name="description" content="{html.escape(described)}">
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?{fonts}&display=swap" rel="stylesheet">
<style>
  :root {{ {theme["vars"]} }}
  {f"@media (prefers-color-scheme: dark) {{ :root {{ {dark} }} }}" if dark else ""}
{BASE}
{theme["css"]}
</style>
</head>
<body>
<div class="bar">
  <a href="../">&larr; Sammy J's Plugins</a>
  <span class="mark">{html.escape(title)}</span>
  <a href="{get_link}">Get it</a>
  <a href="{BLOB}{folder}/README.md">On GitHub</a>
</div>
<div class="shell">
  <header class="title">
    {f'<p class="strap">{html.escape(theme["strap"])}</p>' if theme["strap"] else ""}
    <h1>{html.escape(title)}</h1>
    <p class="version">{get_it}</p>
  </header>
  <nav class="toc" aria-label="Contents"><ol>{toc}</ol></nav>
  <main class="doc">
{doc}
  </main>
</div>
<div class="foot">
  <span>Made by Samuel Justice</span>
  <a href="../">Sammy J's Plugins</a>
  <a href="https://www.sweetjusticesound.com" target="_blank" rel="noopener">www.sweetjusticesound.com</a>
</div>
<script>{SCRIPT}{live}</script>
</body>
</html>
"""


prefixes = releases.tag_prefixes()
RELEASES = releases.all_releases(repo) if TOYS else None
outdir = pathlib.Path(out)
for pid, name, folder in plugins:
    if not (root / folder / "README.md").exists():
        continue
    page = build(pid, name, folder)
    (outdir / pid).mkdir(parents=True, exist_ok=True)
    (outdir / pid / "index.html").write_text(page, encoding="utf-8")
    print(f"Wrote {outdir / pid / 'index.html'} ({len(page) // 1024} KB)")
