"""Scorer for engine2 (and engine v1 for comparison). The hand map lives only here.
Definitions follow proto/score.py: beats scored = every whole beat between the first and last hand event,
hand time by linear interpolation between hand events; strict = same beat number; line = nearest auto grid line."""
import sys, json, os
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import engine_v1 as V
import engine2 as E2
SR = V.SR
S = sys.argv[1]; VERB = len(sys.argv) > 2
pairs = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'pairs.json')))
ms = lambda x: np.asarray(x) / SR * 1000
RULES = ('whole', 'half', 'bar', 'nbar')


def summ(d, dn, dh, hit):
    a = np.abs(d)
    return dict(nh=hit.sum(), h10=(a[hit] < 10).sum(), hl10=(np.abs(dn)[hit] < 10).sum(), n=len(a), med=np.median(a), p90=np.percentile(a, 90), w5=(a < 5).sum(), w10=(a < 10).sum(), w20=(a < 20).sum(),
                line10=(np.abs(dn) < 10).sum(), half10=(np.abs(dh) < 10).sum())


def fmt(s):
    return 'strict med %6.1f p90 %6.1f <5 %3.0f%% <10 %3.0f%% <20 %3.0f%% | line<10 %3.0f%% | half-line<10 %3.0f%%' % (
        s['med'], s['p90'], 100 * s['w5'] / s['n'], 100 * s['w10'] / s['n'], 100 * s['w20'] / s['n'], 100 * s['line10'] / s['n'], 100 * s['half10'] / s['n']) + ' | hand beats on a hit %d: strict<10 %3.0f%% line<10 %3.0f%%' % (s['nh'], 100 * s['h10'] / s['nh'], 100 * s['hl10'] / s['nh'])


def nearest(lines, H):
    return ms(np.array([lines[np.abs(lines - h).argmin()] - h for h in H]))


def hand_bounds(hs, hb, bpm, thr):
    """Runs of hand segments whose tempo is more than thr from the cue's median segment tempo (merged when adjacent)."""
    seg = np.array(bpm[:len(hs) - 1]); med = np.median(seg); out = []
    for i in range(len(seg)):
        if abs(seg[i] / med - 1) > thr:
            if out and out[-1]['i1'] == i:
                out[-1].update(i1=i + 1, t1=hs[i + 1] / SR, beats=hb[i + 1] - out[-1]['b0'])
            else:
                out.append(dict(i1=i + 1, t0=hs[i] / SR, t1=hs[i + 1] / SR, b0=hb[i], beats=hb[i + 1] - hb[i]))
    for o in out:
        o['bpm'] = o['beats'] / (o['t1'] - o['t0']) * 60
    return out


tot = {}; rule_ok = {r: 0 for r in RULES}; nb_in = 0; cats = {}
for p in pairs:
    path = S + '/session-study/mx/' + p['file']
    n, on, st = E2.analyse(path)
    ev = [e for e in p['evs'] if 0 <= e[0] < n]
    hs = np.array([e[0] for e in ev], float); hb = np.array([e[1] for e in ev]); hbpm = [e[2] for e in ev]
    B = np.arange(np.ceil(hb[0]), np.floor(hb[-1]) + 1); H = np.interp(B, hb, hs)
    bpm = round(p['evs'][0][2]); bar = int(hb[0] // 4) + 1; bt = int(hb[0] % 4) + 1
    print('\n=== %s  hint: %.3fs = bar %d|%d, %d bpm; hand events %d, beats scored %d' % (p['tag'], hs[0] / SR, bar, bt, bpm, len(hs), len(B)))
    HIT = np.array([np.abs(on[st >= 0.15] - h).min() for h in H]) <= 0.006 * SR  # hand beat has a transient within 6 ms
    res = {}
    # version 1 through this scorer
    y = V.load(path); pk, st1, _ = V.flux_onsets(y); on1 = V.refine(y, pk, 'hyb')
    r = V.run_hinted(y, on1, st1, hs[0], bpm)
    f = lambda b: V.map_time(r['kk'], r['tt'], r['A'], np.asarray(b) - hb[0])
    lb = np.arange(-20, len(r['kk']) + 20, 0.5); lines = V.map_time(r['kk'], r['tt'], r['A'], lb)
    res['v1'] = summ(ms(f(B) - H), nearest(lines[::2], H), nearest(lines, H), HIT)
    res['v1']['pins'] = sum(1 for a in r['A'] if H[0] - SR * 0.01 <= r['tt'][a] <= H[-1] + SR * 0.01)

    def conf_cb(t, prop):  # the editor: reads the bar/beat of the proposed restart hit off the hand map (to the half beat)
        if t * SR < hs[0] or t * SR > hs[-1]:
            return None
        return round(float(np.interp(t * SR, hs, hb)) * 2) / 2

    runs = [(r_, r_, None, True) for r_ in RULES] + [('nojump', 'whole', None, False), ('confirmed', 'whole', conf_cb, True)]
    out = {}
    for name, rule, cb, dj in runs:
        o = E2.map_cue(path, hs[0] / SR, (bar, bt), bpm, rule=rule, confirm=cb, detect_jumps=dj)
        out[name] = o
        pb, pt = o['pin_beats'], o['pin_times']
        d = ms(E2.map_time(pb, pt, B) - H)
        lb = np.arange(np.floor(pb[0]) - 20, np.ceil(pb[-1]) + 21, 0.5)
        lines = E2.map_time(pb, pt, lb)
        whole = lines[np.abs(lb - np.round(lb)) < 1e-6]
        res[name] = summ(d, nearest(whole, H), nearest(lines, H), HIT)
        res[name]['pins'] = int(((pt >= H[0] - SR * 0.01) & (pt <= H[-1] + SR * 0.01)).sum())
        cf = [b for b in o['bounds'] if b['confirmed']]
        res[name]['nconf'] = len(cf); res[name]['ncorr'] = sum(abs(b['beat'] - b['props']['whole']) > 1e-6 for b in cf)
        res[name]['d'] = d
    for k in res:
        print('  %-10s %s | pins %d vs hand %d%s' % (k, fmt(res[k]), res[k]['pins'], len(hs), (' | confirmations %d of which corrections %d' % (res[k]['nconf'], res[k]['ncorr'])) if k == 'confirmed' else ''))
        tot.setdefault(k, []).append(res[k])
    # boundaries
    strict6 = hand_bounds(hs, hb, hbpm, 0.06); loose = hand_bounds(hs, hb, hbpm, 0.035)
    o = out['whole']; used = set(); fa_h = 0; fa_b = 0
    print('  detected boundaries: beats from previous section\'s last beat to restart hit, by rule; then the hand map over the same stretch')
    for b in o['bounds']:
        inr = hs[0] <= b['t'] * SR <= hs[-1]
        hpos = float(np.interp(b['t'] * SR, hs, hb)); hprev = float(np.interp(b['t_prev'] * SR, hs, hb))
        hbeats = hpos - hprev; hfill = hbeats / (b['t'] - b['t_prev']) * 60
        match = [i for i, h in enumerate(loose) if b['t_prev'] - 0.3 <= h['t1'] and h['t0'] <= b['t'] + 0.3]
        used.update(match)
        okr = {r_: abs(b['props'][r_] - hpos) < 0.1 for r_ in RULES}
        if inr:
            nb_in += 1
            for r_ in RULES:
                rule_ok[r_] += okr[r_]
        if match:
            tag = 'HIT'
        elif not inr:
            tag = 'outside hand map'
        elif okr['whole']:
            tag = 'extra, harmless (hand grid passes through the same beat)'; fa_h += 1
        else:
            tag = 'extra, DISAGREES with hand'; fa_b += 1
        print('    %-4s %6.2f-%6.2fs raw %5.2f | whole %g half %g bar %g nbar %g | hand: %.2f beats, filler %.1f bpm, restart hit = hand beat %.2f | rules right: %s | %s' % (
            b['type'], b['t_prev'], b['t'], b['raw'], *(b['props'][r_] - b['last_beat'] for r_ in RULES), hbeats, hfill, hpos,
            ','.join(r_ for r_ in RULES if okr[r_]) or 'none', tag))
    s6 = [(h['t0'], h['t1']) for h in strict6]
    for i, h in enumerate(loose):
        is6 = any(a < h['t1'] and h['t0'] < b_ for a, b_ in s6)
        print('    hand boundary %6.2f-%6.2fs: %.2f beats at %.1f bpm (%s) -> %s' % (h['t0'], h['t1'], h['beats'], h['bpm'], '>6%' if is6 else '3.5-6%', 'detected' if i in used else 'MISSED'))
    tot.setdefault('_b', []).append((len(used), len(loose) - len(used), fa_h, fa_b, len(strict6)))
    # where the confirmed grid still disagrees: who is on a transient?
    o = out['confirmed']; d = res['confirmed']['d']; a = E2.map_time(o['pin_beats'], o['pin_times'], B)
    strong = st >= 0.15
    dh = ms(np.array([np.abs(on[strong] - h).min() for h in H])); da = ms(np.array([np.abs(on[strong] - x).min() for x in a]))
    bad = np.abs(d) > 10
    c = dict(hand_only=int((bad & (dh <= 6) & (da > 6)).sum()), auto_only=int((bad & (dh > 6) & (da <= 6)).sum()),
             both=int((bad & (dh <= 6) & (da <= 6)).sum()), neither=int((bad & (dh > 6) & (da > 6)).sum()))
    print('  confirmed, beats off by >10 ms: %d -> transient within 6 ms under hand only %d, auto only %d, both %d, neither %d' % (bad.sum(), c['hand_only'], c['auto_only'], c['both'], c['neither']))
    for k in c:
        cats[k] = cats.get(k, 0) + c[k]
    if VERB:
        for i in np.nonzero(bad)[0]:
            print('     b%6.1f %s t=%6.2f d=%+7.1f | hand->onset %5.1f | auto->onset %5.1f' % (B[i], 'EV' if B[i] in hb else '  ', H[i] / SR, d[i], dh[i], da[i]))
print('\n=== ALL CUES')
for k in tot:
    if k == '_b':
        continue
    n = sum(s['n'] for s in tot[k])
    print('  %-10s beats %d | strict <5 %3.0f%% <10 %3.0f%% <20 %3.0f%% | line<10 %3.0f%% | half-line<10 %3.0f%% | pins %d%s%s' % (
        k, n, 100 * sum(s['w5'] for s in tot[k]) / n, 100 * sum(s['w10'] for s in tot[k]) / n, 100 * sum(s['w20'] for s in tot[k]) / n,
        100 * sum(s['line10'] for s in tot[k]) / n, 100 * sum(s['half10'] for s in tot[k]) / n, sum(s['pins'] for s in tot[k]),
        ' | on-hit %d: strict<10 %3.0f%% line<10 %3.0f%%' % (sum(s['nh'] for s in tot[k]), 100 * sum(s['h10'] for s in tot[k]) / sum(s['nh'] for s in tot[k]), 100 * sum(s['hl10'] for s in tot[k]) / sum(s['nh'] for s in tot[k])),
        (' | confirmations %d, corrections %d' % (sum(s['nconf'] for s in tot[k]), sum(s['ncorr'] for s in tot[k]))) if k == 'confirmed' else ''))
b = np.sum(tot['_b'], axis=0)
print('  hand boundaries (>3.5%% from cue median tempo): %d, of which >6%%: %d | detected %d, missed %d | extra detections: harmless %d, disagreeing %d' % (b[0] + b[1], b[4], b[0], b[1], b[2], b[3]))
print('  rounding rules, restart beat equals hand beat (of %d detected boundaries inside the hand maps): %s' % (nb_in, ', '.join('%s %d' % (r_, rule_ok[r_]) for r_ in RULES)))
print('  confirmed, beats still off >10 ms by transient:', cats)
