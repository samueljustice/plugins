"""What build.py and manuals.py need to know about the repository's releases and its toys.

A toy (toys/<id>) is listed in the landing README's "## Toys" section and is released on its own, with
tags like <id>-v0.0.1 (pre-releases for test builds); bundle/plugins.txt names its tag prefix on its
"#test" line. Each release carries a macOS and a Windows installer, which the site links to.
"""
import json
import os
import pathlib
import re
import sys
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[2]


def all_releases(repo: str) -> list | None:
    """Every release, newest first (None if GitHub can't be reached)."""
    headers = {"Accept": "application/vnd.github+json"}
    if os.environ.get("GH_TOKEN"):
        headers["Authorization"] = "Bearer " + os.environ["GH_TOKEN"]
    out, page = [], 1
    try:
        while True:
            req = urllib.request.Request(f"https://api.github.com/repos/{repo}/releases?per_page=100&page={page}", headers=headers)
            batch = json.load(urllib.request.urlopen(req, timeout=20))
            out += batch
            if len(batch) < 100:
                return out
            page += 1
    except Exception as e:  # no network: the pages still look the releases up when they open
        print(f"Couldn't list releases: {e}", file=sys.stderr)
        return None


def tag_prefixes() -> dict:
    """id -> release tag prefix, from bundle/plugins.txt (its "#test" lines too)."""
    prefixes = {}
    for line in (ROOT / "bundle/plugins.txt").read_text(encoding="utf-8").splitlines():
        line = re.sub(r"^#test\s+", "", line.strip())
        if line and not line.startswith("#"):
            fields = [f.strip() for f in line.split("|")]
            if len(fields) > 1:
                prefixes[fields[0]] = fields[1]
    return prefixes


def toys() -> list:
    """The toys on the landing page, in its order: (id, name, folder), from "- **[Name](toys/<id>)**: ..."."""
    text = (ROOT / "README.md").read_text(encoding="utf-8")
    m = re.search(r"^## Toys\s*$(.*?)(?=^## |\Z)", text, re.S | re.M)
    found = []
    for name, path in re.findall(r"^\s*[-*]\s+\*\*\[([^\]]+)\]\(((?:\./)?toys/[A-Za-z0-9_-]+)/?\)\*\*", m.group(1) if m else "", re.M):
        folder = path.removeprefix("./")
        if (ROOT / folder / "README.md").exists():
            found.append((folder.split("/")[-1], name, folder))
    return found


def newest(releases: list | None, prefix: str) -> dict:
    """The newest release whose tag starts with prefix: {tag, version, mac, win, test} (empty if none)."""
    for r in releases or []:
        tag = r.get("tag_name", "")
        if tag.startswith(prefix) and not r.get("draft"):
            assets = {a["name"]: a["browser_download_url"] for a in r.get("assets", [])}
            pick = lambda suffix: next((u for n, u in assets.items() if n.endswith(suffix)), "")
            return {"tag": tag, "version": tag[len(prefix):], "url": r.get("html_url", ""), "test": bool(r.get("prerelease")),
                    "mac": pick("-macOS-Installer.pkg"), "win": pick("-Windows-Installer.exe")}
    return {}


# The pages ask GitHub again when they open (a new test build doesn't rebuild the site), and repoint these.
# Elements: [data-release-prefix="doomeq-v"] holding a.get-mac / a.get-win and a .ver span.
LIVE_SCRIPT = """
(function () {
  var boxes = document.querySelectorAll('[data-release-prefix]');
  if (!boxes.length || !window.fetch) return;
  fetch('https://api.github.com/repos/%(repo)s/releases?per_page=100', { headers: { Accept: 'application/vnd.github+json' } })
    .then(function (r) { return r.ok ? r.json() : []; })
    .then(function (list) {
      boxes.forEach(function (box) {
        var prefix = box.getAttribute('data-release-prefix');
        var r = list.filter(function (x) { return !x.draft && x.tag_name.indexOf(prefix) === 0; })[0];
        if (!r) return;
        (r.assets || []).forEach(function (a) {
          if (/-macOS-Installer\\.pkg$/.test(a.name)) box.querySelectorAll('.get-mac').forEach(function (e) { e.href = a.browser_download_url; });
          if (/-Windows-Installer\\.exe$/.test(a.name)) box.querySelectorAll('.get-win').forEach(function (e) { e.href = a.browser_download_url; });
        });
        box.querySelectorAll('.ver').forEach(function (e) { e.textContent = r.tag_name.slice(prefix.length); });
      });
    })
    .catch(function () {});
})();
"""
