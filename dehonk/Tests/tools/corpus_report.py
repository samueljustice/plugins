#!/usr/bin/env python3
"""Measure De-Honk on a folder of real mixes against SPEC section 13.

    python3 dehonk/Tests/tools/corpus_report.py [FOLDER] [--out DIR] [--render PATH]
                                                [--keep-renders] [-- dehonk_render options...]

FOLDER defaults to ~/Desktop/honkymixes. Only the WAV files directly in it are used (no
subfolders); a file dehonk_render or the WAV reader cannot read is skipped with a message.

For each mix:
  1. Labels: dehonk/Tests/corpus/labels/<name>.csv if it exists (time_seconds,kind, see
     dehonk/Tests/README.md); otherwise find_impacts.py proposes impact onsets, written to
     <out>/labels/<name>.csv. Those are a machine's guess, to be corrected by ear and copied
     to corpus/labels/ before the figures are trusted.
  2. A first pass with dehonk_render --threshold -60 --frames reads the programme level the
     core measures; the threshold is set 8 dB below its 85th percentile, so the busy moments
     are above it.
  3. A second pass at the plug-in's defaults with that threshold writes <out>/<name>.metrics.csv
     (with Body, Tail and Front per impact and the cut on any dialogue or music stretch),
     <out>/<name>.notches.csv and <out>/<name>.frames.csv.
Then <out>/summary.csv and <out>/summary.txt gather, per mix: Body, Tail and Front worst and
median over its impacts, how many notches opened, the share on the three strongest pitch
classes of the core's key profile and on ranks 4 to 12, and the mean deepest cut from 110 to
400 Hz inside and outside the busy moments (programme level above the threshold).

The renders are written to a temporary file and deleted unless --keep-renders is given.
Everything is written under --out (default dehonk/Tests/corpus/report, which git ignores):
session audio and anything made from it stays out of the repository. Standard library only.
"""

import argparse
import csv
import os
import statistics
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TESTS = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True  # no __pycache__ in the repository
import find_impacts  # noqa: E402


def default_render():
    for name in ("dehonk_render", "dehonk_render.exe"):
        for sub in ("", "Release", "RelWithDebInfo", "Debug"):
            path = os.path.join(TESTS, "build", sub, name)
            if os.path.isfile(path):
                return path
    return os.path.join(TESTS, "build", "dehonk_render")


def percentile(values, p):
    v = sorted(values)
    if not v:
        return float("nan")
    i = p * (len(v) - 1)
    lo = int(i)
    hi = min(lo + 1, len(v) - 1)
    return v[lo] + (v[hi] - v[lo]) * (i - lo)


def read_frames(path):
    """time, programme level, and the deepest cut between 110 and 400 Hz, per frame."""
    rows = []
    with open(path, newline="") as f:
        r = csv.reader(f)
        head = next(r)
        t_i, p_i = head.index("time_s"), head.index("program_db")
        bands = []
        for i, h in enumerate(head):
            if h.startswith("cut_") and h.endswith("hz"):
                hz = float(h[4:-2])
                if 110.0 <= hz <= 400.0:
                    bands.append(i)
        for row in r:
            rows.append((float(row[t_i]), float(row[p_i]), max((float(row[i]) for i in bands), default=0.0)))
    return rows


def run(cmd):
    p = subprocess.run(cmd, capture_output=True, text=True)
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def fmt(v, spec="%+.2f"):
    return "n/a" if v is None or v != v else spec % v


def main():
    argv = sys.argv[1:]
    passthrough = []
    if "--" in argv:
        cut = argv.index("--")
        argv, passthrough = argv[:cut], argv[cut + 1:]
    ap = argparse.ArgumentParser(description="De-Honk on a folder of real mixes: impacts, notches against the key, busy moments.")
    ap.add_argument("folder", nargs="?", default=os.path.expanduser("~/Desktop/honkymixes"))
    ap.add_argument("--out", default=os.path.join(TESTS, "corpus", "report"))
    ap.add_argument("--labels", default=os.path.join(TESTS, "corpus", "labels"), help="folder of corrected labels, <name>.csv")
    ap.add_argument("--render", default=default_render())
    ap.add_argument("--keep-renders", action="store_true")
    args = ap.parse_args(argv)

    if not os.path.isfile(args.render):
        sys.exit("dehonk_render not found at %s; build dehonk/Tests first" % args.render)
    if not os.path.isdir(args.folder):
        sys.exit("no such folder: %s" % args.folder)
    out = os.path.abspath(args.out)
    os.makedirs(os.path.join(out, "labels"), exist_ok=True)

    wavs = sorted(f for f in os.listdir(args.folder)
                  if f.lower().endswith(".wav") and not f.startswith("._") and os.path.isfile(os.path.join(args.folder, f)))
    if not wavs:
        sys.exit("no .wav files in %s" % args.folder)

    summary = []
    for f in wavs:
        src = os.path.join(args.folder, f)
        name = os.path.splitext(f)[0]
        print("== %s" % f, flush=True)
        # 1. Labels.
        labels = os.path.join(args.labels, name + ".csv")
        label_source = "corrected"
        if not os.path.isfile(labels):
            labels = os.path.join(out, "labels", name + ".csv")
            label_source = "proposed by find_impacts"
            try:
                _, chosen, _ = find_impacts.find_impacts(src)
            except (OSError, find_impacts.WavError) as e:
                print("   skipped: cannot read: %s" % e)
                continue
            find_impacts.write_labels(labels, chosen)
            print("   %d impact onsets proposed -> %s" % (len(chosen), labels))
        # 2. Threshold from the programme level.
        tmp_wav = os.path.join(out, name + ".render.wav")
        frames1 = os.path.join(out, name + ".level.frames.csv")
        code, msg = run([args.render, src, tmp_wav, "--threshold", "-60", "--frames", frames1] + passthrough)
        if code != 0:
            print("   skipped: %s" % msg.strip())
            continue
        levels = [p for _, p, _ in read_frames(frames1)]
        os.remove(frames1)
        p85 = percentile(levels, 0.85)
        threshold = max(-60.0, min(0.0, p85 - 8.0))
        print("   programme level 85th percentile %.1f dBFS -> threshold %.1f dBFS" % (p85, threshold))
        # 3. The measured render.
        metrics = os.path.join(out, name + ".metrics.csv")
        notches = os.path.join(out, name + ".notches.csv")
        frames = os.path.join(out, name + ".frames.csv")
        code, msg = run([args.render, src, tmp_wav, "--threshold", "%.2f" % threshold, "--metrics", metrics, "--notch-log", notches,
                         "--labels", labels, "--frames", frames] + passthrough)
        if not args.keep_renders and os.path.exists(tmp_wav):
            os.remove(tmp_wav)
        if code != 0:
            print("   skipped: %s" % msg.strip())
            continue
        print("   " + msg.strip().replace(src, f, 1))

        body, tail, front = [], [], []
        dialogue = []
        with open(metrics, newline="") as m:
            for row in csv.DictReader(m):
                if row["row"] == "impact":
                    body.append(float(row["body_db"]))
                    front.append(float(row["front_db"]))
                    if row["tail_change_pct"]:
                        tail.append(float(row["tail_change_pct"]))
                elif row["row"] == "dialogue":
                    dialogue.append((float(row["mean_cut_110_400_db"]), float(row["max_cut_110_400_db"])))
        ranks = []  # 0 = opened before the key profile had filled (no rank)
        with open(notches, newline="") as n:
            for row in csv.DictReader(n):
                ranks.append(int(row["pitch_class_rank"]) if row["pitch_class_rank"] else 0)
        ranked = [r for r in ranks if r > 0]
        fr = read_frames(frames)
        busy = [c for _, p, c in fr if p > threshold]
        calm = [c for _, p, c in fr if p <= threshold]

        def worst(v):
            return max(v, key=abs) if v else float("nan")

        rec = {
            "mix": f,
            "labels": label_source,
            "threshold_db": threshold,
            "impacts": len(body),
            "body_worst_db": worst(body), "body_median_db": statistics.median(body) if body else float("nan"),
            "tail_measurable": len(tail),
            "tail_worst_pct": worst(tail), "tail_median_pct": statistics.median(tail) if tail else float("nan"),
            "front_worst_db": worst(front), "front_median_db": statistics.median(front) if front else float("nan"),
            "notches": len(ranks),
            "notches_unranked": len(ranks) - len(ranked),
            "share_top3": (sum(1 for r in ranked if r <= 3) / len(ranked)) if ranked else float("nan"),
            "share_rank4_12": (sum(1 for r in ranked if r >= 4) / len(ranked)) if ranked else float("nan"),
            "busy_share": len(busy) / max(1, len(fr)),
            "cut_110_400_busy_db": statistics.fmean(busy) if busy else float("nan"),
            "cut_110_400_rest_db": statistics.fmean(calm) if calm else float("nan"),
            "dialogue_stretches": len(dialogue),
            "dialogue_mean_cut_db": statistics.fmean(d[0] for d in dialogue) if dialogue else float("nan"),
            "dialogue_max_cut_db": max((d[1] for d in dialogue), default=float("nan")),
        }
        summary.append(rec)

    if not summary:
        sys.exit("nothing measured")
    keys = list(summary[0].keys())
    with open(os.path.join(out, "summary.csv"), "w", newline="") as s:
        w = csv.DictWriter(s, fieldnames=keys)
        w.writeheader()
        for rec in summary:
            w.writerow({k: ("%.4f" % v if isinstance(v, float) else v) for k, v in rec.items()})

    lines = ["De-Honk corpus report: defaults, threshold 8 dB below each mix's 85th-percentile programme level.",
             "Body: 110-400 Hz energy over 1.5 s after each impact, out against in. Tail: decay time of that band",
             "(a line fitted to the backward-integrated energy above the mix underneath, 5 to 25 dB down, or less",
             "where the mix underneath is close), change in percent; only where at least 8 dB of decay is there to",
             "fit. Front: peak of that band in the first 30 ms. Worst is the largest change either way. Notches: cut",
             "above 1 dB in one band, closed once under 0.5 dB for 100 ms; rank of the note in the core's key profile",
             "when it opened, left out of the shares when it opened before the profile had filled (Tuning::keySeconds).",
             "Cut 110-400: the deepest cut in that range per display frame, averaged; busy = programme level above",
             "the threshold.",
             ""]
    for r in summary:
        lines.append("%s" % r["mix"])
        lines.append("  labels %s; threshold %.1f dBFS; busy %.0f%% of the time" % (r["labels"], r["threshold_db"], 100 * r["busy_share"]))
        lines.append("  %d impacts: Body worst %s dB, median %s dB | Tail worst %s, median %s (%d measurable) | Front worst %s dB, median %s dB"
                     % (r["impacts"], fmt(r["body_worst_db"]), fmt(r["body_median_db"]), fmt(r["tail_worst_pct"], "%+.1f%%"),
                        fmt(r["tail_median_pct"], "%+.1f%%"), r["tail_measurable"], fmt(r["front_worst_db"]), fmt(r["front_median_db"])))
        lines.append("  %d notches opened (%d before the key profile filled, not ranked): %s of the rest on the three strongest pitch classes, %s on ranks 4 to 12"
                     % (r["notches"], r["notches_unranked"], fmt(100 * r["share_top3"], "%.0f%%"), fmt(100 * r["share_rank4_12"], "%.0f%%")))
        lines.append("  mean deepest cut 110-400 Hz: %s dB in the busy moments, %s dB in the rest"
                     % (fmt(r["cut_110_400_busy_db"], "%.2f"), fmt(r["cut_110_400_rest_db"], "%.2f")))
        if r["dialogue_stretches"]:
            lines.append("  dialogue (%d stretches): mean cut %s dB, max %s dB" % (r["dialogue_stretches"], fmt(r["dialogue_mean_cut_db"], "%.2f"),
                                                                                    fmt(r["dialogue_max_cut_db"], "%.2f")))
        lines.append("")
    text = "\n".join(lines)
    with open(os.path.join(out, "summary.txt"), "w") as s:
        s.write(text)
    print()
    print(text)
    print("written to %s" % out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
