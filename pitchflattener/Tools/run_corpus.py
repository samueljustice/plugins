#!/usr/bin/env python3
"""Runs FlattenFile over the whole test corpus into a new dated listening folder.

    python3 pitchflattener/Tools/run_corpus.py [--label text] [--only substring] [--plots]
        [--workers 6]

Output goes to ~/Desktop/Pitch Test/<YYYY-MM-DD HHMM>[ label]/, with every recording's printout
collected in Results.txt in corpus order. Environment variables reach FlattenFile unchanged, so tuning
switches such as WIN=... can be set on the command line in front of this script.
"""

import argparse
import concurrent.futures
import datetime
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
# The tool that flattens one file: the plug-in's core.
PROTO = os.path.join(HERE, 'FlattenFile')
SPECPLOT = os.path.join(HERE, 'specplot')
DESKTOP = os.path.expanduser('~/Desktop')
PITCH_TEST = os.path.join(DESKTOP, 'Pitch Test')


def wavs_in(folder):
    if not os.path.isdir(folder):
        return []
    return sorted(os.path.join(folder, n) for n in os.listdir(folder)
                  if n.lower().endswith('.wav') and not n.startswith('.'))


def safe_name(name):
    # FlattenFile builds file names from this, so path separators must go.
    return name.replace('/', '-').replace(':', '-')


def corpus():
    """(display name, path) in the agreed order."""
    items = []
    for path in wavs_in(os.path.join(DESKTOP, 'ricos')):
        name = os.path.splitext(os.path.basename(path))[0]
        if name.startswith('SJFX '):
            name = name[len('SJFX '):]
        items.append((name, path))
    bmw = os.path.join(DESKTOP, 'Pitch Flattener BMW test', '0 Original input.wav')
    if os.path.isfile(bmw):
        items.append(('BMW run-up', bmw))
    for sub in ('horses', 'random'):
        for path in wavs_in(os.path.join(PITCH_TEST, 'sources', sub)):
            items.append((os.path.splitext(os.path.basename(path))[0], path))
    return [(safe_name(n), p) for n, p in items]


def build_description(label):
    """One line saying which build this is: FlattenFile's build time and the source revision."""
    built = datetime.datetime.fromtimestamp(os.path.getmtime(PROTO)).strftime('%Y-%m-%d %H:%M')
    try:
        git = lambda *a: subprocess.run(['git', '-C', HERE, *a], capture_output=True, text=True, timeout=10).stdout.strip()
        rev = git('rev-parse', '--short', 'HEAD')
        # The tool is often mid-edit, so say whether this folder differs from the commit.
        if rev and git('status', '--porcelain', '--', '.'):
            rev += ' with local changes'
    except (OSError, subprocess.SubprocessError):
        rev = ''
    parts = [f'FlattenFile built {built}']
    if rev:
        parts.append(f'source {rev}')
    if label:
        parts.append(f'label "{label}"')
    return 'Corpus run: ' + ', '.join(parts)


def run_one(name, path, folder, plots):
    """Runs FlattenFile (and optionally specplot) for one recording; returns what to report."""
    start = time.time()
    result = subprocess.run([PROTO, path, folder, name], capture_output=True, text=True)
    text = result.stdout
    if result.returncode != 0:
        text += f'    (FlattenFile failed with exit code {result.returncode}: {result.stderr.strip()})\n'
    if plots:
        plot_dir = os.path.join(folder, 'plots')
        command = [SPECPLOT, path, os.path.join(plot_dir, name + '.png')]
        lines = os.path.join(folder, name + '.lines.csv')
        has_lines = os.path.isfile(lines)
        if has_lines:
            command += ['--lines', lines]
        plot = subprocess.run(command, capture_output=True, text=True)
        if plot.returncode != 0:
            text += f'    (specplot failed: {plot.stderr.strip()})\n'
        if has_lines:
            shutil.move(lines, os.path.join(plot_dir, name + '.lines.csv'))
    return text, result.returncode, time.time() - start


def main():
    parser = argparse.ArgumentParser(description='Run FlattenFile over the test corpus.')
    parser.add_argument('--label', default='', help='added to the folder name and Results.txt')
    parser.add_argument('--only', default='', help='only recordings whose name contains this (any case)')
    parser.add_argument('--plots', action='store_true', help='also draw a spectrogram of each source into plots/')
    parser.add_argument('--workers', type=int, default=6)
    parser.add_argument('--main', action='store_true', help='leave out the split-up compilations (horses, random)')
    args = parser.parse_args()

    if not os.access(PROTO, os.X_OK):
        sys.exit(f'run_corpus: {PROTO} is missing or not executable; build it with pitchflattener/Tools/build_tools.sh')
    if args.plots and not os.access(SPECPLOT, os.X_OK):
        sys.exit(f'run_corpus: {SPECPLOT} is missing; build it with pitchflattener/Tools/build_tools.sh')

    items = corpus()
    if args.main:
        items = [(n, p) for n, p in items if not n.startswith(('horse ', 'random '))]
    if args.only:
        items = [(n, p) for n, p in items if args.only.lower() in n.lower()]
    if not items:
        sys.exit('run_corpus: no recordings to run')

    stamp = datetime.datetime.now().strftime('%Y-%m-%d %H%M')
    base = os.path.join(PITCH_TEST, stamp + (f' {safe_name(args.label)}' if args.label else ''))
    # Two runs in the same minute get separate folders rather than mixing their files.
    folder, n = base, 2
    while os.path.exists(folder):
        folder, n = f'{base} ({n})', n + 1
    os.makedirs(folder)
    if args.plots:
        os.makedirs(os.path.join(folder, 'plots'))

    print(f'{len(items)} recordings into {folder}')
    results = [None] * len(items)
    written = 0
    failures = 0
    report = open(os.path.join(folder, 'Results.txt'), 'w')
    report.write(build_description(args.label) + '\n\n')
    with report, concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as pool:
        futures = {pool.submit(run_one, name, path, folder, args.plots): k for k, (name, path) in enumerate(items)}
        for done, future in enumerate(concurrent.futures.as_completed(futures), 1):
            k = futures[future]
            text, code, seconds = future.result()
            results[k] = text
            failures += code != 0
            # The first result line is enough to spot a problem while the rest run.
            lines = [l.strip() for l in text.splitlines() if l.strip()]
            first = lines[1] if len(lines) > 1 else '(no output)'
            status = 'ok' if code == 0 else f'FAILED ({code})'
            print(f'[{done}/{len(items)}] {items[k][0]}: {status}, {seconds:.1f} s | {first[:100]}', flush=True)

            # Results.txt stays in corpus order: write every finished run up to the first one still going,
            # so an interrupted run still leaves a usable file.
            while written < len(results) and results[written] is not None:
                report.write(results[written] if results[written].endswith('\n') else results[written] + '\n')
                written += 1
            report.flush()

    if failures:
        print(f'{failures} recording(s) failed; see Results.txt')
    print(folder)
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
