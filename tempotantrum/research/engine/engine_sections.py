"""Throwaway prototype, engine version 2: section-by-section beat mapping.

Public entry: map_cue(audio_path, first_hit_seconds, (bar, beat), bpm, rule='whole', confirm=None)
Nothing but those arguments reaches the engine. `confirm` is an optional callback that stands
in for the editor (time of a DETECTED restart hit, proposed beat) -> beat position or None.
"""
import os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import engine_v1 as V

SR = V.SR
C = dict(
    THR_ON=0.15,       # onset strength counted as a "confident" onset for gap finding
    THR_STRONG=0.30,   # strength a restart hit needs
    GAP_BEATS=1.5,     # onset-free stretch (beats) that ends a section
    MIN_SEC=3,         # a section needs this many snapped beats, else it is treated as part of the gap
    PIN_TOL_MS=10.0,
    RESCUE_MS=20.0,    # a strong onset this close to an unsnapped beat line is taken as that beat (0 = off)
    LOOK=12,           # beats of look-ahead when choosing a restart hit after a gap (3 bars)
    J_TOL_MS=15.0,     # onset-to-line tolerance in the support tests
    J_AFTER=8,         # jump test: beats compared after the change point (2 bars)
    J_AFTER_SHORT=4,   # least look-ahead (1 bar) allowed when the section ends sooner
    J_BEFORE=8,        # jump test: beats of history examined
    J_MARGIN=2.0,      # shifted grid support must beat the continuing grid by this factor
    J_MIN_SHIFT_MS=30.0,  # smallest phase shift called a jump (smaller ones the tracker absorbs)
    J_LOCK=0.6,        # share of the last beats that had an onset (grid was locked)
    J_AFTER_MIN=0.75,  # shifted grid: at least this share of lines has an onset (every line when the look-ahead is short)
    J_BEFORE_MAX=0.4,  # support the shifted phase had BEFORE the change point, relative to after
)

_cache = {}


def analyse(path):
    if path not in _cache:
        y = V.load(path)
        pk, st, _ = V.flux_onsets(y)
        on = V.refine(y, pk, 'hyb')
        o = np.argsort(on)
        _cache[path] = (len(y), on[o], st[o])
    return _cache[path]


# ------------------------------------------------------------------ helpers
def line_support(on, st, lines, tol):
    """Per grid line: strength of the strongest onset within tol (0 if none)."""
    out = np.zeros(len(lines))
    j = np.searchsorted(on, lines)
    for n, (g, i) in enumerate(zip(lines, j)):
        for c in (i - 1, i):
            if 0 <= c < len(on) and abs(on[c] - g) <= tol:
                out[n] = max(out[n], st[c])
    return out


def map_time(pb, pt, beats):
    beats = np.asarray(beats, float)
    out = np.interp(beats, pb, pt)
    if len(pb) > 1:
        lo = beats < pb[0]; hi = beats > pb[-1]
        out[lo] = pt[0] + (beats[lo] - pb[0]) * (pt[1] - pt[0]) / (pb[1] - pb[0])
        out[hi] = pt[-1] + (beats[hi] - pb[-1]) * (pt[-1] - pt[-2]) / (pb[-1] - pb[-2])
    return out


# ------------------------------------------------------------------ boundaries
def find_gap(on, st, t_start, period):
    """First onset-free stretch longer than GAP_BEATS after t_start. Returns (last onset before, first onset after) or None."""
    m = (st >= C['THR_ON']) & (on >= t_start - 1)
    o = on[m]
    d = np.diff(o)
    g = np.nonzero(d > C['GAP_BEATS'] * period)[0]
    if len(g) == 0:
        return None
    return o[g[0]], o[g[0] + 1]


def next_strong(on, st, t):
    m = (on >= t - 1) & (st >= C['THR_STRONG'])
    return on[m][0] if m.any() else None


def pick_start(on, st, t_first, t_end, period):
    """After a gap: the restart hit is the strong onset, within the first two beats of the new
    section, whose grid at the prior tempo is best supported by the following onsets."""
    tol = C['J_TOL_MS'] / 1000 * SR
    cand = on[(on >= t_first - 1) & (on <= t_first + 2 * period) & (st >= C['THR_STRONG'])]
    best = None
    for c in cand:
        L = int(min(C['LOOK'], (t_end - c) / period + 1))
        if L < 1:
            continue
        sup = max(line_support(on, st, c + np.arange(L) * period * (1 + e), tol).sum() for e in (-0.02, -0.01, 0, 0.01, 0.02))
        if best is None or sup > 1.1 * best[0]:  # an earlier hit keeps the place unless a later one is clearly better
            best = (sup, c)
    return best[1] if best else t_first


def jump_test(on, st, t, P, nlock, N):
    """Change point right after beat time t? Compares onset support (summed onset strength on the grid
    lines) for the continuing grid with every shifted grid that starts on an onset in the next two beats.
    Returns (score, restart hit time) or None."""
    tol = C['J_TOL_MS'] / 1000 * SR
    nb = np.arange(1, C['J_BEFORE'] + 1)
    eps_set = (-0.01, 0, 0.01)
    cont = max(line_support(on, st, t + np.arange(1, N + 1) * P * (1 + e), tol).sum() for e in eps_set)
    cb = line_support(on, st, t - np.arange(nlock) * P, tol)
    if (cb >= C['THR_ON']).mean() < C['J_LOCK']:
        return None
    ms = C['J_MIN_SHIFT_MS'] / 1000 * SR
    best = None
    for i in np.nonzero((on > t + ms) & (on < t + 2 * P - ms) & (st >= C['THR_ON']))[0]:
        g0 = on[i]
        if abs(g0 - (t + P)) < ms:
            continue
        if best is not None and abs(g0 - best[1] - P) < 2 * tol:
            continue  # same shifted grid one beat later: keep its first hit
        before = line_support(on, st, g0 - nb * P, tol).mean()
        for e in eps_set:
            s = line_support(on, st, g0 + np.arange(N) * P * (1 + e), tol)
            if (s >= C['THR_ON']).mean() >= (C['J_AFTER_MIN'] if N >= C['J_AFTER'] else 1.0) and s.sum() >= C['J_MARGIN'] * cont \
                    and before <= C['J_BEFORE_MAX'] * s.mean():
                sc = s.sum() - cont
                if best is None or sc > best[0] + 1e-9:
                    best = (sc, g0)
    return best


def find_jump(on, st, t_start, t_end, period):
    """Walk a stiff grid from the section start; at each locked beat ask jump_test whether the following
    bars fit a shifted grid clearly better than the continuing one. Returns the restart hit time or None."""
    t, P, k = float(t_start), float(period), 0
    P0 = P
    tol = C['J_TOL_MS'] / 1000 * SR
    while t + P * (C['J_AFTER_SHORT'] + 1) <= t_end:
        if k >= 3:
            N = int(min(C['J_AFTER'], (t_end - t) / P - 1))  # fewer bars of look-ahead near the section end
            r = jump_test(on, st, t, P, min(k + 1, C['J_BEFORE']), N)
            if r:
                g = r[1]
                for _ in range(2):  # the shifted grid may have started a beat or two earlier
                    c = np.nonzero((np.abs(on - (g - P)) <= tol) & (st >= C['THR_ON']) & (on > t_start + P))[0]
                    if not len(c):
                        break
                    g = on[c[np.argmax(st[c])]]
                return g
        nxt = t + P
        w = min(0.025 * SR, 0.08 * P)
        c = np.nonzero((np.abs(on - nxt) <= w) & (st >= C['THR_ON']))[0]
        if len(c):
            err = on[c[np.argmax(st[c])]] - nxt
            t = nxt + 0.6 * err
            P = float(np.clip(P + 0.1 * err, 0.94 * P0, 1.06 * P0))
        else:
            t = nxt
        k += 1
    return None


# ------------------------------------------------------------------ one section
def track_section(on, st, t_start, t_end, period, beat0):
    """Track from the section's first hit. Returns dict(kk relative beats, tt samples, conf, A anchors, period)."""
    m = (on >= t_start - 0.03 * SR) & (on <= t_end + 0.03 * SR)
    o, s = on[m], st[m]

    def once(per):
        path, _ = V.track_from_hint(o, s, t_start, per, t_end)
        pt = {k: o[i] for k, i in path.items()}
        pt.setdefault(0, t_start)
        kk, tt = V.fill(pt, 0, max(pt))
        return path, pt, kk, tt

    trivial = dict(kk=np.array([0]), tt=np.array([float(t_start)]), conf=np.array([True]), A=[0], period=period)
    if len(o) < 3:
        return trivial
    path, pt, kk, tt = once(period)
    ks = np.array(sorted(pt)); ts = np.array([pt[k] for k in ks])
    if len(ks) >= 6:  # local tempo from the section's own onsets
        per2 = np.polyfit(ks, ts, 1)[0]
        if abs(per2 / period - 1) < 0.08:
            period = per2
            path, pt, kk, tt = once(period)
    if len(kk) < 3 or len(path) < 2:
        return trivial
    for _ in range(2):
        snapped = {kk[0] + k: o[i] for k, i in path.items()}
        g = V.smooth_grid(kk, tt, snapped)
        per = np.median(np.diff(g))
        path = V.snap(o, s, g, min(0.035 * SR, 0.2 * per))
        if not path:
            return trivial
        snapped = {kk[0] + k: o[i] for k, i in path.items()}
        _, tt = V.fill(snapped, kk[0], kk[-1])
    conf = np.zeros(len(kk), bool); conf[list(path)] = True
    if not conf[0]:
        conf[0] = True; tt[0] = t_start
    if C['RESCUE_MS'] > 0:  # beats the snap left out although a strong onset sits close to the line
        for i in np.nonzero(~conf)[0]:
            c = np.nonzero((np.abs(o - tt[i]) <= C['RESCUE_MS'] / 1000 * SR) & (s >= C['THR_STRONG']))[0]
            if len(c):
                tt[i] = o[c[np.argmax(s[c])]]; conf[i] = True
    last = int(np.nonzero(conf)[0][-1])
    kk, tt, conf = kk[:last + 1], tt[:last + 1], conf[:last + 1]
    if C['RESCUE_MS'] > 0:  # follow strong hits beat by beat up to the end of the section
        while len(tt) >= 3:
            nxt = tt[-1] + np.median(np.diff(tt[-5:]))
            c = np.nonzero((np.abs(o - nxt) <= C['RESCUE_MS'] / 1000 * SR) & (s >= C['THR_STRONG']))[0]
            if not len(c):
                break
            kk = np.append(kk, kk[-1] + 1); tt = np.append(tt, o[c[np.argmax(s[c])]]); conf = np.append(conf, True)
    if conf.sum() >= 2:
        phase = (-beat0) % 4  # relative index of a beat 1
        A = V.anchors(kk, tt, conf, phase, tol=C['PIN_TOL_MS'] / 1000 * SR)
        per = np.median(np.diff(tt))
    else:
        A = [0]; per = period
    return dict(kk=kk, tt=tt, conf=conf, A=list(A), period=float(per))


def bridge(raw, last_beat, rule):
    if rule == 'half':
        return last_beat + max(0.5, round(raw * 2) / 2)
    target = last_beat + max(1, round(raw))
    if rule == 'bar':
        return float(np.ceil(target / 4 - 1e-9) * 4)
    if rule == 'nbar':  # nearest bar line to where the old grid would put the hit
        b = round((last_beat + raw) / 4) * 4
        return float(b if b > last_beat else b + 4)
    return float(target)


# ------------------------------------------------------------------ whole cue
def map_cue(path, first_hit_s, barbeat, bpm, rule='whole', confirm=None, detect_jumps=True):
    n, on, st = analyse(path)
    beat = (barbeat[0] - 1) * 4.0 + (barbeat[1] - 1)
    t = first_hit_s * SR
    i0 = int(np.argmin(np.abs(on - t)))
    if abs(on[i0] - t) < 0.030 * SR:
        t = on[i0]
    period = 60.0 / bpm * SR
    sections, bounds = [], []
    pend = None  # boundary waiting for its section
    while True:
        # where does this section end?
        gap = find_gap(on, st, t, period)
        t_end = gap[0] if gap else float(n)
        jump = find_jump(on, st, t, t_end, period) if detect_jumps else None
        if jump is not None:
            t_end = on[on < jump - 1][-1]
            nxt, typ = jump, 'jump'
        elif gap:
            nxt, typ = next_strong(on, st, gap[1]), 'gap'
            if nxt is not None:
                g2 = find_gap(on, st, nxt, period)
                nxt = pick_start(on, st, nxt, g2[0] if g2 else float(n), period)
        else:
            nxt, typ = None, None
        sec = track_section(on, st, t, t_end, period, beat)
        keep = sec['conf'].sum() >= C['MIN_SEC'] or not sections
        if keep:
            sec['beat0'] = beat
            sections.append(sec)
            if pend:
                bounds.append(pend)
            period = sec['period']
        if nxt is None:
            break
        prev = sections[-1]
        last_t = prev['tt'][-1]; last_b = prev['beat0'] + prev['kk'][-1]
        raw = (nxt - last_t) / prev['period']
        props = {r: bridge(raw, last_b, r) for r in ('whole', 'half', 'bar', 'nbar')}
        b = props[rule]
        confirmed = False
        if confirm is not None:
            cb = confirm(nxt / SR, b)
            if cb is not None and cb > last_b:
                b, confirmed = cb, True
        pend = dict(t_prev=last_t / SR, t=nxt / SR, type=typ if keep else 'gap', raw=raw, last_beat=last_b,
                    props=props, beat=b, confirmed=confirmed, tempo=60 * SR / prev['period'])
        t, beat = nxt, b
    pb, pt = [], []
    for s in sections:
        for a in s['A']:
            pb.append(s['beat0'] + s['kk'][a]); pt.append(s['tt'][a])
    return dict(pin_beats=np.array(pb, float), pin_times=np.array(pt, float), sections=sections, bounds=bounds, n=n)
