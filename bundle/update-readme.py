#!/usr/bin/env python3
"""Rewrites the "Get them all" block at the top of the root README to point at a just-published installer.

Usage: update-readme.py <README.md> <repository> <release tag> <macOS .dmg> <Windows .exe> <built> <releases.txt>

  repository   owner/name, e.g. samueljustice/plugins
  built        when the installer was built, as shown in the heading, e.g. "8 Nov 2026, 12:53 UTC"
  releases.txt one line per plug-in: id|tag|version (what bundle/resolve-releases.sh prints)

The block sits between the two markers below. Run by build-release_bundle.yml after it publishes, so the
buttons always download that build's files (whose names carry the build's date and time).
"""
import pathlib
import re
import sys

START = "<!-- get-them-all:start (written by .github/workflows/build-release_bundle.yml; edits here are replaced) -->"
END = "<!-- get-them-all:end -->"


def main() -> int:
    if len(sys.argv) != 8:
        print(__doc__)
        return 2
    readme, repo, tag, mac, win, built, releases = sys.argv[1:]
    here = pathlib.Path(__file__).parent

    # Display names, in installer order, from bundle/plugins.txt: id|prefix|display name|bundle|app
    names = {}
    prefixes = {}
    order = []
    for line in (here / "plugins.txt").read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        parts = line.split("|")
        names[parts[0]] = parts[2]
        prefixes[parts[0]] = parts[1]
        order.append(parts[0])
    versions = {}
    for line in pathlib.Path(releases).read_text(encoding="utf-8").splitlines():
        parts = line.strip().split("|")
        if len(parts) == 3:
            versions[parts[0]] = parts[2]
    inside = " · ".join(f"{names[i]} {versions[i]}" for i in order if i in versions)

    base = f"https://github.com/{repo}/releases/download/{tag}"
    block = "\n".join([
        START,
        "### Get them all",
        "",
        f"[![Get them all for macOS](https://img.shields.io/badge/Get_them_all-macOS-000000?style=for-the-badge&logo=apple&logoColor=white)]({base}/{mac})",
        f"[![Get them all for Windows](https://img.shields.io/badge/Get_them_all-Windows-0078D6?style=for-the-badge&logo=windows&logoColor=white)]({base}/{win})",
        "",
        f"<sub>One installer with the newest version of every plug-in: {inside}.<br>",
        "Apps, VST3, AU and signed AAX for Pro Tools. It replaces older copies, and comes with an uninstaller "
        "(macOS: on the disk image; Windows: Settings &gt; Apps).<br>",
        f"Built {built}. Older versions are on the <a href=\"https://github.com/{repo}/releases\">releases</a> page.</sub>",
        END,
    ])

    path = pathlib.Path(readme)
    text = path.read_text(encoding="utf-8")
    start, end = text.find(START), text.find(END)
    if start < 0 or end < start:
        print(f"{readme}: the get-them-all markers are missing or out of order", file=sys.stderr)
        return 1
    updated = text[:start] + block + text[end + len(END):]
    if updated != text:
        path.write_text(updated, encoding="utf-8")
        print(f"Updated {readme}:\n{block}")
    else:
        print(f"{readme} already up to date")

    return 0


if __name__ == "__main__":
    sys.exit(main())
