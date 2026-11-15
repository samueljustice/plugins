#!/usr/bin/env python3
"""Splits a long compilation WAV into its separate sounds at the silences between them.

    python3 pitchflattener/Tools/split_sounds.py in.wav outFolder [--prefix name]
        [--threshold -90] [--min-gap 0.25] [--dry-run]

A gap is a stretch at least --min-gap seconds long whose peak level stays at or below --threshold
dBFS. The compilations this is for have about half a second of digital silence between sounds,
while the quietest moments inside a sound rarely stay that low for longer than a tenth of a second.
Each piece keeps the original format (the fmt chunk is copied as it is) with about 50 ms of
padding either side where the gap allows.
"""

import argparse
import array
import math
import os
import struct
import sys

BLOCK_SECONDS = 0.010   # level is measured per 10 ms block, then the edges refined to the sample
PAD_SECONDS = 0.050
MIN_PIECE_SECONDS = 0.15
CHUNK_BLOCKS = 2000     # blocks read per chunk: about 20 s, a few MB at a time


class WavInfo:
    def __init__(self, path):
        with open(path, 'rb') as f:
            head = f.read(12)
            if len(head) < 12 or head[:4] != b'RIFF' or head[8:12] != b'WAVE':
                sys.exit(f'split_sounds: {path} is not a WAV file')
            self.fmt = None
            pos = 12
            file_size = os.path.getsize(path)
            while pos + 8 <= file_size:
                f.seek(pos)
                cid, size = struct.unpack('<4sI', f.read(8))
                if cid == b'fmt ':
                    self.fmt = f.read(size)
                elif cid == b'data':
                    self.data_offset = pos + 8
                    self.data_size = min(size, file_size - pos - 8)
                    break
                pos += 8 + size + (size & 1)
            else:
                sys.exit(f'split_sounds: no data chunk in {path}')
        if self.fmt is None:
            sys.exit(f'split_sounds: no fmt chunk in {path}')
        tag, self.channels, self.rate, _, self.frame_bytes, self.bits = struct.unpack('<HHIIHH', self.fmt[:16])
        if tag == 0xFFFE and len(self.fmt) >= 26:
            tag = struct.unpack('<H', self.fmt[24:26])[0]
        self.is_float = tag == 3
        self.sample_bytes = self.bits // 8
        if self.is_float and self.bits != 32 or not self.is_float and self.bits not in (16, 24, 32):
            sys.exit(f'split_sounds: unsupported format ({self.bits}-bit, tag {tag})')
        self.frames = self.data_size // self.frame_bytes
        self.full_scale = 1.0 if self.is_float else float(1 << (self.bits - 1))

    def decode(self, raw):
        """Raw interleaved bytes to an array of numbers, without a Python object per sample."""
        if self.is_float:
            return array.array('f', raw)
        if self.bits == 16:
            return array.array('h', raw)
        if self.bits == 32:
            return array.array('i', raw)
        # 24-bit: widen to 32 by putting each 3-byte sample in the top of a 4-byte word.
        # Strided slice copies run in C, so this stays fast for whole minutes of audio.
        n = len(raw) // 3
        wide = bytearray(n * 4)
        wide[1::4] = raw[0::3]
        wide[2::4] = raw[1::3]
        wide[3::4] = raw[2::3]
        values = array.array('i', bytes(wide))
        return values  # scaled by 256; peak() undoes it

    def peak(self, values):
        if not values:
            return 0.0
        p = max(max(values), -min(values))
        if not self.is_float and self.bits == 24:
            p /= 256.0
        return p / self.full_scale


def block_peaks(path, info, block_frames):
    """Peak level (0..1) of every block, read in chunks."""
    peaks = []
    step = block_frames * info.channels
    with open(path, 'rb') as f:
        f.seek(info.data_offset)
        remaining = info.frames * info.frame_bytes
        while remaining > 0:
            raw = f.read(min(remaining, CHUNK_BLOCKS * block_frames * info.frame_bytes))
            if not raw:
                break
            remaining -= len(raw)
            raw = raw[:len(raw) - len(raw) % info.frame_bytes]
            values = info.decode(raw)
            for i in range(0, len(values), step):
                peaks.append(info.peak(values[i:i + step]))
    return peaks


def find_edge(path, info, first_frame, frames, threshold, last):
    """First (or last) frame in a range whose level is above the threshold."""
    with open(path, 'rb') as f:
        f.seek(info.data_offset + first_frame * info.frame_bytes)
        values = info.decode(f.read(frames * info.frame_bytes))
    ch = info.channels
    order = range(len(values) // ch - 1, -1, -1) if last else range(len(values) // ch)
    for i in order:
        if info.peak(values[i * ch:(i + 1) * ch]) > threshold:
            return first_frame + i
    return first_frame + (frames - 1 if last else 0)


def write_piece(path, src, info, start, end):
    data_bytes = (end - start) * info.frame_bytes
    fmt = info.fmt + (b'\0' if len(info.fmt) & 1 else b'')
    with open(path, 'wb') as out, open(src, 'rb') as f:
        out.write(struct.pack('<4sI4s', b'RIFF', 4 + 8 + len(fmt) + 8 + data_bytes + (data_bytes & 1), b'WAVE'))
        out.write(struct.pack('<4sI', b'fmt ', len(info.fmt)) + fmt)
        out.write(struct.pack('<4sI', b'data', data_bytes))
        f.seek(info.data_offset + start * info.frame_bytes)
        remaining = data_bytes
        while remaining > 0:
            raw = f.read(min(remaining, 1 << 22))
            out.write(raw)
            remaining -= len(raw)
        if data_bytes & 1:
            out.write(b'\0')


def to_db(level):
    return 20 * math.log10(level) if level > 0 else float('-inf')


def main():
    parser = argparse.ArgumentParser(description='Split a compilation WAV at the silences between sounds.')
    parser.add_argument('input')
    parser.add_argument('out_folder')
    parser.add_argument('--prefix', help='piece name prefix (default: the input file name)')
    parser.add_argument('--threshold', type=float, default=-90.0, help='silence level in dBFS peak (default -90)')
    parser.add_argument('--min-gap', type=float, default=0.25, help='shortest silence that separates sounds, seconds')
    parser.add_argument('--dry-run', action='store_true', help='print the gaps and pieces without writing')
    args = parser.parse_args()

    info = WavInfo(args.input)
    prefix = args.prefix or os.path.splitext(os.path.basename(args.input))[0]
    rate = info.rate
    block = max(1, round(BLOCK_SECONDS * rate))
    threshold = 10 ** (args.threshold / 20)
    peaks = block_peaks(args.input, info, block)

    # Runs of quiet blocks; the long ones are gaps between sounds.
    quiet_runs, run_start = [], None
    for i, p in enumerate(peaks + [1.0]):
        if p <= threshold and run_start is None:
            run_start = i
        elif p > threshold and run_start is not None:
            quiet_runs.append((run_start, i))
            run_start = None
    min_gap_blocks = math.ceil(args.min_gap * rate / block)
    gaps = [(a, b) for a, b in quiet_runs if b - a >= min_gap_blocks]

    # The histogram shows whether the threshold and minimum gap sit in a clear valley.
    edges = [0.01, 0.02, 0.05, 0.1, 0.2, 0.25, 0.5, 1.0, 2.0, 5.0, math.inf]
    counts = [0] * len(edges)
    for a, b in quiet_runs:
        length = (b - a) * block / rate
        counts[next(k for k, e in enumerate(edges) if length <= e + 1e-9)] += 1
    print(f'Quiet stretches (peak <= {args.threshold:g} dBFS) by length:')
    lower = 0.0
    for e, c in zip(edges, counts):
        label = f'{lower:g}-{e:g} s' if e != math.inf else f'> {lower:g} s'
        print(f'  {label:>12}  {c:5d}  {"#" * min(c, 60)}')
        lower = e

    # Sounds lie between gaps; refine their edges to the sample, then pad into the gap.
    regions, prev_end = [], 0
    for a, b in gaps + [(len(peaks), len(peaks))]:
        if a > prev_end:
            regions.append((prev_end, a))
        prev_end = b
    pieces = []
    for n, (a, b) in enumerate(regions):
        first = find_edge(args.input, info, a * block, min(block, info.frames - a * block), threshold, last=False)
        last_block_start = (b - 1) * block
        last = find_edge(args.input, info, last_block_start, min(block, info.frames - last_block_start), threshold, last=True)
        # Padding never reaches past the middle of the gap, so neighbours never share audio.
        before = regions[n - 1][1] * block if n > 0 else 0
        after = regions[n + 1][0] * block if n + 1 < len(regions) else info.frames
        start = max(first - round(PAD_SECONDS * rate), (before + first) // 2 if n > 0 else 0)
        end = min(last + 1 + round(PAD_SECONDS * rate), (after + last + 1) // 2 if n + 1 < len(regions) else info.frames)
        if (end - start) / rate < MIN_PIECE_SECONDS:
            continue
        pieces.append((start, end, max(peaks[a:b])))

    if not args.dry_run:
        os.makedirs(args.out_folder, exist_ok=True)
    print(f'\n{len(pieces)} pieces from {os.path.basename(args.input)} '
          f'({info.channels} ch, {rate} Hz, {info.bits}-bit{" float" if info.is_float else ""}, {info.frames / rate:.1f} s)')
    print('  piece    start s   length s   peak dBFS')
    for k, (start, end, peak) in enumerate(pieces, 1):
        print(f'  {k:5d}  {start / rate:9.3f}  {(end - start) / rate:9.3f}  {to_db(peak):10.1f}')
        if not args.dry_run:
            write_piece(os.path.join(args.out_folder, f'{prefix} {k:03d}.wav'), args.input, info, start, end)
    if pieces:
        lengths = sorted((e - s) / rate for s, e, _ in pieces)
        print(f'Lengths: min {lengths[0]:.2f} s, median {lengths[len(lengths) // 2]:.2f} s, max {lengths[-1]:.2f} s')


if __name__ == '__main__':
    main()
