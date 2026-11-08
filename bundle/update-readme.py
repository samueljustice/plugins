#!/usr/bin/env python3
"""Rewrites the "Get them all" block at the top of the root README to point at a just-published installer.

Usage: update-readme.py <README.md> <repository> <release tag> <macOS file> <Windows file> <built> <releases.txt>

  repository   owner/name, e.g. samueljustice/plugins
  built        when the installer was built, as shown in the heading, e.g. "8 Nov 2026, 12:53 UTC"
  releases.txt one line per plug-in: id|tag|version (what bundle/resolve-releases.sh prints)

The block sits between the two markers below; everything else in the README is left alone. Run by
build-release_bundle.yml after it publishes, so the buttons always download that build's files (whose
names carry the build's date and time).
"""
import pathlib
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
    order = []
    for line in (here / "plugins.txt").read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        parts = line.split("|")
        names[parts[0]] = parts[2]
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
        f"### ⬇️ Get them all (last built {built})",
        "",
        f"[![Get them all for macOS](https://img.shields.io/badge/Get_them_all-macOS-000000?style=for-the-badge&logo=apple&logoColor=white)]({base}/{mac})",
        f"[![Get them all for Windows](https://img.shields.io/badge/Get_them_all-Windows-0078D6?style=for-the-badge&logo=windows&logoColor=white)]({base}/{win})",
        "",
        f"<sub>One installer with the newest version of every plug-in: {inside}. The apps, the VST3s and (on macOS) the AUs. "
        "It removes older copies first, so nothing shows up twice in your host.<br>",
        f"Want just one? Each plug-in's own download is below, or browse all <a href=\"https://github.com/{repo}/releases\">releases</a>.</sub>",
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
