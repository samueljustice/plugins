#!/usr/bin/env python3
"""Propose impact onsets in a mix, as a starting point for a person to correct.

    python3 dehonk/Tests/tools/find_impacts.py mix.wav [-o labels.csv] [--max 20]
                                               [--jump-db 8] [--within-db 6] [--gap 1.5]

An onset is a moment where the broadband short-term level (10 ms RMS, every 5 ms, all channels)
jumps by at least --jump-db within 30 ms and lands within --within-db of the file's loudest
short-term level. Candidates are taken strongest first (by the loudest level in the 50 ms after
the jump), at least --gap seconds apart, up to --max. The labels file has the columns
time_seconds,kind (kind = impact), strongest first, the format dehonk_render --labels reads.

It knows nothing about what the sound is: a loud cut in the music or a door slam can be
proposed, and an impact that rises slowly or lands under a louder one is missed. Listen, move,
delete and add before using the file as corpus labels.

Reads 16, 24 and 32-bit PCM and 32 and 64-bit float WAV (plain or WAVE_FORMAT_EXTENSIBLE), any
number of channels. Standard library only.
"""

import argparse
import array
import math
import os
import struct
import sys


class WavError(Exception):
    pass


def read_wav(path):
    """Returns (sample_rate, channels) with channels a list of array('f'/'d'/'i'/'h') and a
    scale that maps the stored values to -1..1."""
    with open(path, "rb") as f:
        head = f.read(12)
        if len(head) < 12 or head[0:4] not in (b"RIFF", b"RF64") or head[8:12] != b"WAVE":
            raise WavError("not a WAV file")
        fmt = None
        data = None
        data_size_64 = None
        while True:
            ch = f.read(8)
            if len(ch) < 8:
                break
            cid, size = ch[0:4], struct.unpack("<I", ch[4:8])[0]
            if cid == b"ds64":
                body = f.read(size)
                if len(body) >= 16:
                    data_size_64 = struct.unpack("<Q", body[8:16])[0]
            elif cid == b"fmt ":
                fmt = f.read(size)
            elif cid == b"data":
                if size == 0xFFFFFFFF and data_size_64 is not None:
                    size = data_size_64
                data = f.read(size)
                break
            else:
                f.seek(size, 1)
            if size % 2:
                f.seek(1, 1)
    if fmt is None or data is None or len(fmt) < 16:
        raise WavError("no fmt or data chunk")
    tag, nch, sr, _, _, bits = struct.unpack("<HHIIHH", fmt[:16])
    if tag == 0xFFFE and len(fmt) >= 26:
        tag = struct.unpack("<H", fmt[24:26])[0]
    if nch < 1:
        raise WavError("no channels")
    width = bits // 8
    frames = len(data) // (width * nch)
    data = data[: frames * width * nch]
    if tag == 3 and bits == 32:
        a, scale = array.array("f"), 1.0
        a.frombytes(data)
    elif tag == 3 and bits == 64:
        a, scale = array.array("d"), 1.0
        a.frombytes(data)
    elif tag == 1 and bits == 16:
        a, scale = array.array("h"), 1.0 / 32768.0
        a.frombytes(data)
    elif tag == 1 and bits == 24:
        # Widen to 32 bits with the low byte zero, then read as int32.
        wide = bytearray(len(data) // 3 * 4)
        wide[1::4] = data[0::3]
        wide[2::4] = data[1::3]
        wide[3::4] = data[2::3]
        a, scale = array.array("i"), 1.0 / 2147483648.0
        a.frombytes(bytes(wide))
    elif tag == 1 and bits == 32:
        a, scale = array.array("i"), 1.0 / 2147483648.0
        a.frombytes(data)
    else:
        raise WavError("unsupported format: tag %d, %d bits" % (tag, bits))
    if sys.byteorder != "little":
        a.byteswap()
    chans = [a[c::nch] for c in range(nch)]
    return sr, chans, scale


def short_term_levels(sr, chans, scale, hop_s=0.005, win_hops=2):
    """10 ms RMS (two 5 ms hops) every 5 ms over all channels, dBFS."""
    hop = max(1, int(round(hop_s * sr)))
    n = len(chans[0])
    nhops = n // hop
    hop_energy = [0.0] * nhops
    sumprod = getattr(math, "sumprod", None)
    for c in chans:
        for j in range(nhops):
            seg = c[j * hop:(j + 1) * hop]
            hop_energy[j] += sumprod(seg, seg) if sumprod else sum(v * v for v in seg)
    norm = scale * scale / (hop * win_hops * len(chans))
    levels = []
    for j in range(nhops - win_hops + 1):
        e = sum(hop_energy[j:j + win_hops]) * norm
        levels.append(10.0 * math.log10(e) if e > 0 else -400.0)
    return hop, levels


def onset_time(sr, chans, scale, hop, lv, j, back, prev_db):
    """Refines a rise found at hop j to about a millisecond: the start of the first 5 ms window
    (every 1 ms) whose level is 3 dB above the loudest 10 ms level in the quiet before the rise."""
    k = j
    while k > j - back and lv[k - 1] > prev_db + 3.0:
        k -= 1
    quiet_db = max(lv[max(0, k - back):k]) if k > 0 else prev_db
    start = max(0, (k - 1) * hop)
    end = min(len(chans[0]), (j + 3) * hop)
    ms = max(1, int(round(0.001 * sr)))
    win = 5 * ms
    thresh = 10.0 ** ((quiet_db + 3.0) / 10.0) * win * len(chans) / (scale * scale)
    for a in range(start, end - win + 1, ms):
        e = 0.0
        for c in chans:
            seg = c[a:a + win]
            e += sum(v * v for v in seg)
        if e > thresh:
            return (a + win) / sr - 0.0025   # the rise is in the window's later half
    return k * hop / sr


def find_impacts(path, max_count=20, jump_db=8.0, within_db=6.0, gap_s=1.5):
    sr, chans, scale = read_wav(path)
    hop, lv = short_term_levels(sr, chans, scale)
    if not lv:
        return sr, [], -400.0
    loudest = max(lv)
    back = int(round(0.030 * sr / hop))     # 30 ms
    ahead = int(round(0.050 * sr / hop))    # 50 ms
    cands = []
    j = back
    while j < len(lv):
        prev = min(lv[j - back:j])
        if lv[j] - prev >= jump_db and lv[j] >= loudest - within_db:
            strength = max(lv[j:j + ahead + 1])
            cands.append((strength, onset_time(sr, chans, scale, hop, lv, j, back, prev)))
            j += ahead          # one candidate per rise
        else:
            j += 1
    cands.sort(key=lambda c: -c[0])
    chosen = []
    for strength, t in cands:
        if all(abs(t - u) >= gap_s for _, u in chosen):
            chosen.append((strength, t))
        if len(chosen) >= max_count:
            break
    return sr, chosen, loudest


def write_labels(path, chosen):
    d = os.path.dirname(os.path.abspath(path))
    os.makedirs(d, exist_ok=True)
    with open(path, "w") as f:
        f.write("time_seconds,kind\n")
        for _, t in chosen:
            f.write("%.3f,impact\n" % t)


def main():
    ap = argparse.ArgumentParser(description="Propose impact onsets in a WAV file (a starting point to correct by ear).")
    ap.add_argument("wav")
    ap.add_argument("-o", "--out", help="labels CSV to write (default: print only)")
    ap.add_argument("--max", type=int, default=20)
    ap.add_argument("--jump-db", type=float, default=8.0)
    ap.add_argument("--within-db", type=float, default=6.0)
    ap.add_argument("--gap", type=float, default=1.5)
    args = ap.parse_args()
    try:
        _, chosen, loudest = find_impacts(args.wav, args.max, args.jump_db, args.within_db, args.gap)
    except (OSError, WavError) as e:
        sys.exit("find_impacts: %s: %s" % (args.wav, e))
    print("%s: loudest short-term level %.1f dBFS; %d onsets proposed (strongest first):" % (args.wav, loudest, len(chosen)))
    for i, (strength, t) in enumerate(chosen):
        print("  %2d  %8.3f s  %6.1f dBFS" % (i + 1, t, strength))
    if args.out:
        write_labels(args.out, chosen)
        print("wrote %s" % args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
