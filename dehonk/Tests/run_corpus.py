#!/usr/bin/env python3
"""Render every WAV under a folder through dehonk_render and collect the metrics.

    python3 dehonk/Tests/run_corpus.py [--in DIR] [--out DIR] [--csv FILE] [--frames] [--snap]
                                       [--render PATH] [-- dehonk_render options...]

The folder tree under --in is mirrored under --out: corpus/dry/mix/a.wav renders to
corpus/out/mix/a.wav, with its metrics in corpus/out/mix/a.metrics.csv (and a.frames.csv
with --frames). A metrics file has one row per analysis band (row = band) and one per
module (row = module: Woof, Tub, Fog, Bark, with the level change over its range and its
deepest notch over time) and one for Snap (row = click: the level change in its band, its cut
over time, the share of time above 1 and 3 dB, and how many separate events). All metrics are
joined into one CSV with the file's path as the first column, and the module and click rows
alone into a second one (default <out>/modules.csv).

--snap switches the Snap module on for every file (the same as "-- --snap 1").

Anything after "--" is passed to dehonk_render for every file, for example
    -- --bark 1 --threshold -20
    -- --woof 0 --tub 1 --fog 1 --bark 0 --edges 60,120,220,400,1500
    -- --depth 0.7 --max-cut 9 --release 250 --impact-guard 0

Standard library only.
"""

import argparse
import csv
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def default_render():
    for name in ("dehonk_render", "dehonk_render.exe"):
        for sub in ("", "Release", "RelWithDebInfo", "Debug"):
            path = os.path.join(HERE, "build", sub, name)
            if os.path.isfile(path):
                return path
    return os.path.join(HERE, "build", "dehonk_render")


def main():
    argv = sys.argv[1:]
    passthrough = []
    if "--" in argv:
        cut = argv.index("--")
        argv, passthrough = argv[:cut], argv[cut + 1:]

    ap = argparse.ArgumentParser(description="Render a folder of WAVs through dehonk_render.")
    ap.add_argument("--in", dest="indir", default=os.path.join(HERE, "corpus", "dry"),
                    help="folder to search for .wav files (default: dehonk/Tests/corpus/dry)")
    ap.add_argument("--out", dest="outdir", default=os.path.join(HERE, "corpus", "out"),
                    help="folder for renders and metrics (default: dehonk/Tests/corpus/out)")
    ap.add_argument("--csv", dest="csvpath", default=None,
                    help="combined metrics CSV (default: <out>/metrics.csv)")
    ap.add_argument("--modules-csv", dest="modpath", default=None,
                    help="combined module rows (default: <out>/modules.csv)")
    ap.add_argument("--frames", action="store_true", help="also write each file's display frames")
    ap.add_argument("--snap", action="store_true", help="switch the Snap module on (passes --snap 1)")
    ap.add_argument("--render", default=default_render(), help="path to dehonk_render")
    args = ap.parse_args(argv)

    if not os.path.isfile(args.render):
        sys.exit("dehonk_render not found at %s; build dehonk/Tests first (see README.md)" % args.render)
    if not os.path.isdir(args.indir):
        sys.exit("no such folder: %s" % args.indir)

    indir = os.path.abspath(args.indir)
    outdir = os.path.abspath(args.outdir)
    csvpath = args.csvpath or os.path.join(outdir, "metrics.csv")
    modpath = args.modpath or os.path.join(outdir, "modules.csv")

    wavs = []
    for root, dirs, files in os.walk(indir):
        # Never descend into the output folder if it sits inside the input folder.
        dirs[:] = sorted(d for d in dirs if os.path.abspath(os.path.join(root, d)) != outdir)
        for f in sorted(files):
            if f.lower().endswith(".wav") and not f.startswith("._"):
                wavs.append(os.path.join(root, f))
    if not wavs:
        sys.exit("no .wav files under %s" % indir)

    header = None
    rows = []
    failures = 0
    for src in wavs:
        rel = os.path.relpath(src, indir)
        dst = os.path.join(outdir, rel)
        stem = os.path.splitext(dst)[0]
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        metrics = stem + ".metrics.csv"
        cmd = [args.render, src, dst, "--metrics", metrics]
        if args.frames:
            cmd += ["--frames", stem + ".frames.csv"]
        if args.snap:
            cmd += ["--snap", "1"]
        cmd += passthrough
        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode != 0:
            failures += 1
            print("FAILED %s\n%s" % (rel, (proc.stderr or proc.stdout).strip()), file=sys.stderr)
            continue
        print(proc.stdout.strip().replace(src, rel, 1))
        with open(metrics, newline="") as f:
            reader = csv.reader(f)
            file_header = next(reader, None)
            if file_header is None:
                continue
            if header is None:
                header = ["file"] + file_header
            for row in reader:
                rows.append([rel] + row)

    if header is not None:
        os.makedirs(os.path.dirname(csvpath) or ".", exist_ok=True)
        with open(csvpath, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(header)
            w.writerows(rows)
        kind = header.index("row") if "row" in header else None
        if kind is not None:
            os.makedirs(os.path.dirname(modpath) or ".", exist_ok=True)
            with open(modpath, "w", newline="") as f:
                w = csv.writer(f)
                w.writerow(header)
                w.writerows(r for r in rows if r[kind] in ("module", "click"))
        print("%d of %d files rendered; metrics in %s%s" % (len(wavs) - failures, len(wavs), csvpath,
              "; module and click rows in %s" % modpath if kind is not None else ""))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
