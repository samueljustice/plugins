"""Throwaway prototype: onsets -> rough grid -> DP snap -> sparse anchors."""
import numpy as np, soundfile as sf, librosa
from scipy.signal import find_peaks
from scipy.ndimage import maximum_filter1d, uniform_filter1d

SR = 48000
HOP = 240  # 5 ms
RESET = 9.0  # capped interval cost = price of a phase jump (music edit)


def load(path):
    y, sr = sf.read(path, always_2d=True)
    assert sr == SR
    return y.mean(axis=1).astype(np.float32)


# ---------------------------------------------------------------- onsets
def flux_onsets(y):
    X = np.abs(librosa.stft(y, n_fft=1024, hop_length=HOP, window='hann', center=True))
    mel = librosa.feature.melspectrogram(S=X ** 2, sr=SR, n_mels=64, fmin=30, fmax=16000)
    L = np.log1p(100 * np.sqrt(mel))
    d = np.maximum(L[:, 2:] - L[:, :-2], 0)  # lag 2 frames = 10 ms
    d = np.pad(d, ((0, 0), (2, 0)))
    fr = librosa.mel_frequencies(n_mels=64, fmin=30, fmax=16000)
    flux = np.zeros(d.shape[1])
    for lo, hi in ((0, 150), (150, 800), (800, 4000), (4000, 99999)):
        b = d[(fr >= lo) & (fr < hi)].sum(axis=0)
        flux += b / (b.mean() + 1e-9)
    loc = uniform_filter1d(flux, 100)  # 0.5 s local mean
    pk, _ = find_peaks(flux, distance=6)
    pk = pk[flux[pk] > 1.3 * loc[pk] + 0.3 * np.median(flux)]
    s = flux[pk] / np.percentile(flux[pk], 90)
    return pk * HOP, np.clip(s, 0, 1.5) / 1.5, flux


def refine(y, peaks, mode):
    """Move each flux peak (samples) to the attack start. mode: 'flux' (none), 'last10', 'last20', 'first20', 'slope'."""
    if mode == 'flux':
        return peaks.astype(float)
    if mode == 'fluxshift':
        return peaks - 0.0024 * SR
    if mode == 'hyb':
        a = refine(y, peaks, 'last10'); b = peaks - 0.0024 * SR
        return np.where(np.abs(a - b) <= 0.006 * SR, a, b)
    env = maximum_filter1d(np.abs(y), 48)  # 1 ms peak-hold of rectified signal
    env = uniform_filter1d(env, 24)
    out = []
    ms = SR // 1000
    for p in peaks:
        a, b = max(0, p - 12 * ms), min(len(y), p + 25 * ms)
        k = a + int(np.argmax(env[a:b]))
        f0, f1 = max(0, p - 60 * ms), max(1, p - 30 * ms)
        floor = np.median(env[f0:f1])
        short = mode.endswith('s') and mode != 'slopes'
        m2 = mode[:-1] if short else mode
        lo = max(0, k - (15 if short else 30) * ms)
        seg = env[lo:k + 1]
        if m2 == 'slope':
            dd = np.diff(uniform_filter1d(seg, 96))
            out.append(lo + int(np.argmax(dd)) if len(dd) else p)
            continue
        frac = 0.1 if m2 == 'last10' else 0.2
        thr = floor + frac * (env[k] - floor)
        below = np.nonzero(seg < thr)[0]
        if len(below) == 0:
            out.append(lo)
        elif m2 == 'first20':
            above = np.nonzero(seg >= thr)[0]
            out.append(lo + above[0] if len(above) else p)
        else:
            out.append(lo + below[-1] + 1)
    return np.array(out, float)


# ---------------------------------------------------------------- rough grids
def track_from_hint(on, st, t0, period, end):
    """Mode B pass 1: free tracking DP over onsets. Returns dict beat_index -> onset index."""
    i0 = int(np.argmin(np.abs(on - t0)))
    t = np.append(on, t0)  # virtual start node at the hint
    s = np.append(st, 0)
    start = i0 if abs(on[i0] - t0) < 0.030 * SR else len(on)
    order = [start] + [i for i in range(len(on)) if on[i] > t[start] + 0.5 * period]
    cost = {start: 0.0}; back = {start: None}; beat = {start: 0}
    for i in order[1:]:
        best = None
        for j in order:
            if j == i or j not in cost or t[j] >= t[i]:
                continue
            d = t[i] - t[j]
            n = int(round(d / period))
            if n < 1 or n > 8:
                continue
            dev = (d - n * period) / SR * 1000
            sig = 6.0 + 1.5 * n
            c = cost[j] + min((dev / sig) ** 2, RESET) + 1.0 * (n - 1) - 2.0 * s[i]
            if best is None or c < best[0]:
                best = (c, j, beat[j] + n)
        if best:
            cost[i], back[i], beat[i] = best
    fin = min(cost, key=lambda i: cost[i] + 1.0 * (end - t[i]) / period)
    path = {}
    while fin is not None:
        if fin < len(on):
            path[beat[fin]] = fin
        fin = back[fin]
    return path, (None if start < len(on) else t0)


def snap(on, st, grid, W):
    """Generic DP: per grid beat pick an onset within +-W or 'none'. Penalises interval deviating from the rough grid's."""
    K = len(grid)
    cands = [np.nonzero(np.abs(on - g) <= W)[0] for g in grid]
    nodes = [(k, i) for k in range(K) for i in cands[k]]
    cost = {}; back = {}
    for k, i in nodes:
        best = (1.0 * k - 2.0 * st[i] + ((on[i] - grid[k]) / SR * 1000 / 40) ** 2, None)
        for j in range(max(0, k - 8), k):
            for i2 in cands[j]:
                if (j, i2) not in cost:
                    continue
                dev = ((on[i] - on[i2]) - (grid[k] - grid[j])) / SR * 1000
                sig = 5.0 + 1.5 * (k - j)
                c = cost[(j, i2)] + min((dev / sig) ** 2, RESET) + 1.0 * (k - j - 1) - 2.0 * st[i] \
                    + ((on[i] - grid[k]) / SR * 1000 / 40) ** 2
                if c < best[0]:
                    best = (c, (j, i2))
        cost[(k, i)], back[(k, i)] = best
    if not nodes:
        return {}
    fin = min(nodes, key=lambda n: cost[n] + 1.0 * (K - 1 - n[0]))
    path = {}
    while fin is not None:
        path[fin[0]] = fin[1]
        fin = back[fin]
    return path


def fill(path_times, k0, k1):
    """beat index -> time for k0..k1, linear interpolation/extrapolation from snapped beats."""
    ks = np.array(sorted(path_times)); ts = np.array([path_times[k] for k in ks])
    kk = np.arange(k0, k1 + 1)
    out = np.interp(kk, ks, ts)
    n = min(8, len(ks) - 1)
    if n >= 1:
        pl = (ts[n] - ts[0]) / (ks[n] - ks[0]); pr = (ts[-1] - ts[-1 - n]) / (ks[-1] - ks[-1 - n])
        out[kk < ks[0]] = ts[0] + (kk[kk < ks[0]] - ks[0]) * pl
        out[kk > ks[-1]] = ts[-1] + (kk[kk > ks[-1]] - ks[-1]) * pr
    return kk, out


def smooth_grid(kk, tt, snapped, span=2):
    """Robust local straight-line fit through snapped beats -> rough grid for the next pass."""
    ks = np.array(sorted(snapped)); ts = np.array([snapped[k] for k in ks])
    out = tt.copy()
    for n, k in enumerate(kk):
        m = np.abs(ks - k) <= span
        if m.sum() >= 3:
            a, b = np.polyfit(ks[m], ts[m], 1)
            out[n] = a * k + b
    return out


# ---------------------------------------------------------------- anchors
def anchors(kk, tt, conf, phase, tol=0.004 * SR):
    """Greedy insertion. conf: bool per beat. phase: index (mod 4) of beat 1. Returns sorted list of positions into kk."""
    ci = np.nonzero(conf)[0]
    A = [ci[0], ci[-1]]

    def err(A):
        return np.abs(np.interp(kk, kk[A], tt[A]) - tt)

    def prio(i):
        b = (kk[i] - phase) % 4
        return 0 if b == 0 else (1 if b == 2 else 2)
    while True:
        e = err(A); e[~conf] = 0
        w = int(np.argmax(e))
        if e[w] <= tol:
            break
        a = max(x for x in A if x < w); b = min(x for x in A if x > w)
        best = None
        for c in ci[(ci > a) & (ci < b)]:
            A2 = sorted(A + [c]); e2 = err(A2); e2[~conf] = 0
            m = e2[a:b + 1].max()
            ok = m <= max(tol, 0.5 * e[w])
            key = (not ok, prio(c) if ok else 0, m)
            if best is None or key < best[0]:
                best = (key, c)
        A = sorted(A + [best[1]])
    return A


def map_time(kk, tt, A, beats):
    """Piecewise-constant-tempo map from anchors, last/first segment tempo extended. beats may be fractional."""
    ka, ta = kk[A].astype(float), tt[A]
    out = np.interp(beats, ka, ta)
    beats = np.asarray(beats, float)
    if len(ka) > 1:
        lo = beats < ka[0]; hi = beats > ka[-1]
        out[lo] = ta[0] + (beats[lo] - ka[0]) * (ta[1] - ta[0]) / (ka[1] - ka[0])
        out[hi] = ta[-1] + (beats[hi] - ka[-1]) * (ta[-1] - ta[-2]) / (ka[-1] - ka[-2])
    return out


# ---------------------------------------------------------------- full pipelines
def finish(on, st, kk, tt, path, phase, passes=2):
    for _ in range(passes):
        snapped = {kk[0] + k: on[i] for k, i in path.items()}
        g = smooth_grid(kk, tt, snapped)
        per = np.median(np.diff(g))
        path = snap(on, st, g, min(0.035 * SR, 0.2 * per))
        snapped = {kk[0] + k: on[i] for k, i in path.items()}
        _, tt = fill(snapped, kk[0], kk[-1])
    conf = np.zeros(len(kk), bool); conf[list(path)] = True
    A = anchors(kk, tt, conf, phase)
    return dict(kk=kk, tt=tt, conf=conf, A=A)


def run_hinted(y, on, st, t0, bpm):
    per = 60.0 / bpm * SR
    path, _ = track_from_hint(on, st, t0, per, len(y))
    pt = {k: on[i] for k, i in path.items()}
    pt.setdefault(0, t0)
    k1 = int((len(y) - t0) / per) + 2
    kk, tt = fill(pt, 0, max(k1, max(pt)))
    keep = tt < len(y)
    kk, tt = kk[keep], tt[keep]
    path0 = {k: i for k, i in path.items() if k <= kk[-1]}
    return finish(on, st, kk, tt, path0, phase=0)


def run_auto(y, on, st, beats):
    """beats: rough beat times in samples from any tracker."""
    tt = np.asarray(beats, float); per = np.median(np.diff(tt))
    full = [tt[0]]
    for b in tt[1:]:  # fill holes the tracker left, so beat indices count beats
        n = max(1, int(round((b - full[-1]) / per)))
        full += list(full[-1] + (b - full[-1]) * np.arange(1, n + 1) / n)
    tt = np.array(full); kk = np.arange(len(tt))
    path = snap(on, st, tt, min(0.07 * SR, 0.25 * per))
    # bar phase: the beat-of-4 with the largest summed onset strength
    sc = np.zeros(4)
    for k, i in path.items():
        sc[k % 4] += st[i]
    return finish(on, st, kk, tt, path, phase=int(np.argmax(sc)))
