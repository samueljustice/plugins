#include "Internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <utility>

// Section-by-section beat mapping.
//
// The music is followed from the first hit at the hinted tempo until the beat stops: either the
// hits stop for more than a beat and a half (a gap), or the following bars fit a grid shifted in
// phase clearly better than the continuing one (a jump). The next section starts on its own first
// hit and is followed the same way. Inside a section every beat is snapped to a hit where there
// is one, and pins are added until the straight lines between pins pass within the tolerance of
// every snapped beat.
//
// Before a stretch is followed hit by hit, one constant tempo is tried for it (steadyHead). Where a
// constant grid has most of its beats on hits it is used as far as it holds, and the hits off it
// are left alone as syncopation. It ends where several beats in a row are off it on the same side.
// A section whose constant grid still fits the hits after a gap carries on across the gap.
//
// The constants are in detail::Tuning (Internal.h). Times are seconds throughout.
namespace tt
{
namespace
{

using detail::Tuning;

constexpr double eps = 1.0 / 48000.0;   // "strictly before/after" margin for time comparisons
constexpr double inf = std::numeric_limits<double>::infinity();

double roundHalfEven (double x)   { return std::nearbyint (x); }

double medianOf (std::vector<double> v)
{
    if (v.empty())
        return 0.0;
    std::sort (v.begin(), v.end());
    const auto n = v.size();
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

double medianDiff (const std::vector<double>& t, size_t from = 0)
{
    std::vector<double> d;
    for (size_t i = from + 1; i < t.size(); ++i)
        d.push_back (t[i] - t[i - 1]);
    return medianOf (std::move (d));
}

// The hits as two parallel arrays, sorted by time.
struct Hits
{
    std::vector<double> on;
    std::vector<double> st;

    int size() const   { return (int) on.size(); }
    int lowerBound (double t) const   { return (int) (std::lower_bound (on.begin(), on.end(), t) - on.begin()); }
    int upperBound (double t) const   { return (int) (std::upper_bound (on.begin(), on.end(), t) - on.begin()); }

    // Strength of the strongest of the two hits either side of `line` that lies within tol (0 if none).
    double support (double line, double tol) const
    {
        const int i = lowerBound (line);
        double best = 0.0;
        for (int c = i - 1; c <= i; ++c)
            if (c >= 0 && c < size() && std::abs (on[(size_t) c] - line) <= tol)
                best = std::max (best, st[(size_t) c]);
        return best;
    }

    // The strongest hit of at least minStrength within tol of t (the earliest of equals), or -1.
    int strongestNear (double t, double tol, double minStrength, double after = -inf) const
    {
        int best = -1;
        for (int c = lowerBound (t - tol); c < size() && on[(size_t) c] <= t + tol; ++c)
            if (st[(size_t) c] >= minStrength && on[(size_t) c] > after && (best < 0 || st[(size_t) c] > st[(size_t) best]))
                best = c;
        return best;
    }

    // The last hit strictly before t, or `fallback` if there is none.
    double lastBefore (double t, double fallback) const
    {
        const int i = lowerBound (t - eps);
        return i > 0 ? on[(size_t) i - 1] : fallback;
    }
};

std::vector<double> supportOnLines (const Hits& h, double first, double step, int count, double tol)
{
    std::vector<double> out ((size_t) std::max (0, count));
    for (int n = 0; n < count; ++n)
        out[(size_t) n] = h.support (first + n * step, tol);
    return out;
}

double sum (const std::vector<double>& v)
{
    double s = 0.0;
    for (double x : v)
        s += x;
    return s;
}

double shareAtLeast (const std::vector<double>& v, double threshold)
{
    if (v.empty())
        return 0.0;
    int n = 0;
    for (double x : v)
        n += x >= threshold;
    return (double) n / (double) v.size();
}

//==============================================================================
// Free tracking: a dynamic programme over the hits from the start, each step a whole number of
// beats (1 to 8) long. Returns beat number -> hit index.
std::map<int, int> trackFromHint (const std::vector<double>& on, const std::vector<double>& st,
                                  double t0, double period, double end)
{
    const int n = (int) on.size();
    std::map<int, int> path;
    int i0 = 0;
    for (int i = 1; i < n; ++i)
        if (std::abs (on[(size_t) i] - t0) < std::abs (on[(size_t) i0] - t0))
            i0 = i;

    // The start is the hit at the hint, or a silent node at the hint when no hit is there.
    const bool onHit = n > 0 && std::abs (on[(size_t) i0] - t0) < 0.030;
    const int start = onHit ? i0 : n;
    auto timeOf = [&] (int i) { return i < n ? on[(size_t) i] : t0; };
    auto strengthOf = [&] (int i) { return i < n ? st[(size_t) i] : 0.0; };

    std::vector<int> later;   // hits more than half a beat after the start, in time order
    for (int i = 0; i < n; ++i)
        if (on[(size_t) i] > timeOf (start) + 0.5 * period)
            later.push_back (i);

    struct Node { double cost = 0.0; int back = -1; int beat = 0; bool reached = false; };
    std::vector<Node> nodes ((size_t) n + 1);
    nodes[(size_t) start].reached = true;

    auto consider = [&] (int i, int j, Node& best)
    {
        const auto& from = nodes[(size_t) j];
        if (! from.reached || timeOf (j) >= timeOf (i))
            return;
        const double d = timeOf (i) - timeOf (j);
        const int beats = (int) roundHalfEven (d / period);
        if (beats < 1 || beats > 8)
            return;
        const double dev = (d - beats * period) * 1000.0;
        const double sig = 6.0 + 1.5 * beats;
        if (std::abs (dev) > 4.0 * sig)
            return;
        const double c = from.cost + (dev / sig) * (dev / sig) + 1.0 * (beats - 1) - 2.0 * strengthOf (i);
        if (! best.reached || c < best.cost)
        {
            best.reached = true;
            best.cost = c;
            best.back = j;
            best.beat = from.beat + beats;
        }
    };

    for (size_t a = 0; a < later.size(); ++a)
    {
        const int i = later[a];
        Node best;
        consider (i, start, best);
        const double earliest = timeOf (i) - 8.6 * period - 0.2;   // nothing further back can be 8 beats or fewer away
        size_t b = (size_t) (std::lower_bound (later.begin(), later.begin() + (long) a, earliest,
                                               [&] (int idx, double t) { return on[(size_t) idx] < t; }) - later.begin());
        for (; b < a; ++b)
            consider (i, later[b], best);
        if (best.reached)
            nodes[(size_t) i] = best;
    }

    int fin = start;
    double finCost = nodes[(size_t) start].cost + (end - timeOf (start)) / period;
    for (int i : later)
        if (nodes[(size_t) i].reached)
        {
            const double c = nodes[(size_t) i].cost + (end - timeOf (i)) / period;
            if (c < finCost)
            {
                finCost = c;
                fin = i;
            }
        }
    for (int i = fin; i >= 0; i = nodes[(size_t) i].back)
        if (i < n)
            path[nodes[(size_t) i].beat] = i;
    return path;
}

// For each line of a rough grid, choose a hit within +-window or none. Returns line index -> hit index.
std::map<int, int> snapToGrid (const std::vector<double>& on, const std::vector<double>& st,
                               const std::vector<double>& grid, double window)
{
    struct Node { int k; int i; double cost; int back; };
    std::vector<Node> nodes;
    std::vector<std::pair<int, int>> range (grid.size());   // per line: its nodes [first, last)
    const int K = (int) grid.size();
    for (int k = 0; k < K; ++k)
    {
        range[(size_t) k].first = (int) nodes.size();
        const int a = (int) (std::lower_bound (on.begin(), on.end(), grid[(size_t) k] - window) - on.begin());
        for (int i = a; i < (int) on.size() && on[(size_t) i] <= grid[(size_t) k] + window; ++i)
        {
            const double place = (on[(size_t) i] - grid[(size_t) k]) * 1000.0 / 40.0;
            Node node { k, i, 1.0 * k - 2.0 * st[(size_t) i] + place * place, -1 };
            for (int j = std::max (0, k - 8); j < k; ++j)
                for (int m = range[(size_t) j].first; m < range[(size_t) j].second; ++m)
                {
                    const auto& from = nodes[(size_t) m];
                    const double dev = ((on[(size_t) i] - on[(size_t) from.i]) - (grid[(size_t) k] - grid[(size_t) j])) * 1000.0;
                    const double sig = 5.0 + 1.5 * (k - j);
                    const double c = from.cost + (dev / sig) * (dev / sig) + 1.0 * (k - j - 1) - 2.0 * st[(size_t) i] + place * place;
                    if (c < node.cost)
                    {
                        node.cost = c;
                        node.back = m;
                    }
                }
            nodes.push_back (node);
        }
        range[(size_t) k].second = (int) nodes.size();
    }

    std::map<int, int> path;
    if (nodes.empty())
        return path;
    int fin = 0;
    for (int m = 1; m < (int) nodes.size(); ++m)
        if (nodes[(size_t) m].cost + (K - 1 - nodes[(size_t) m].k) < nodes[(size_t) fin].cost + (K - 1 - nodes[(size_t) fin].k))
            fin = m;
    for (int m = fin; m >= 0; m = nodes[(size_t) m].back)
        path[nodes[(size_t) m].k] = nodes[(size_t) m].i;
    return path;
}

// Beat times for beats 0..last from the snapped ones: straight lines between them, and outside
// them the tempo of the nearest (up to) eight snapped beats.
std::vector<double> fillBeats (const std::map<int, double>& snapped, int last)
{
    std::vector<int> ks;
    std::vector<double> ts;
    for (const auto& [k, t] : snapped)
    {
        ks.push_back (k);
        ts.push_back (t);
    }
    std::vector<double> out ((size_t) last + 1);
    const int m = (int) ks.size();
    const int n = std::min (8, m - 1);
    const double left = n >= 1 ? (ts[(size_t) n] - ts[0]) / (ks[(size_t) n] - ks[0]) : 0.0;
    const double right = n >= 1 ? (ts[(size_t) m - 1] - ts[(size_t) (m - 1 - n)]) / (ks[(size_t) m - 1] - ks[(size_t) (m - 1 - n)]) : 0.0;
    size_t seg = 0;
    for (int k = 0; k <= last; ++k)
    {
        if (k <= ks.front())
            out[(size_t) k] = ts.front() + (k - ks.front()) * left;
        else if (k >= ks.back())
            out[(size_t) k] = ts.back() + (k - ks.back()) * right;
        else
        {
            while (ks[seg + 1] < k)
                ++seg;
            out[(size_t) k] = ts[seg] + (ts[seg + 1] - ts[seg]) * (k - ks[seg]) / (double) (ks[seg + 1] - ks[seg]);
        }
    }
    return out;
}

// A rough grid for the next snap: at each beat, a straight line fitted through the snapped beats
// within four beats of it (when there are at least three).
std::vector<double> smoothGrid (const std::vector<double>& tt, const std::map<int, double>& snapped, int span = 4)
{
    auto out = tt;
    for (int k = 0; k < (int) tt.size(); ++k)
    {
        int count = 0;
        double sk = 0.0, st = 0.0;
        for (auto it = snapped.lower_bound (k - span); it != snapped.end() && it->first <= k + span; ++it)
        {
            ++count;
            sk += it->first;
            st += it->second;
        }
        if (count < 3)
            continue;
        const double mk = sk / count, mt = st / count;
        double num = 0.0, den = 0.0;
        for (auto it = snapped.lower_bound (k - span); it != snapped.end() && it->first <= k + span; ++it)
        {
            num += (it->first - mk) * (it->second - mt);
            den += (it->first - mk) * (it->first - mk);
        }
        out[(size_t) k] = mt + (den > 0.0 ? num / den : 0.0) * (k - mk);
    }
    return out;
}

// The beats of one tracked stretch, counted from 0 at its first hit.
struct Track
{
    std::vector<double> tt;       // time of beat k
    std::vector<char> conf;       // beat k sits on a hit
    std::vector<int> pins;        // the beats chosen as pins
    double period = 0.5;
    double fitPeriod = 0.0;       // the beat length of the constant grid, when one grid covers the whole stretch
    int lead = 0;                 // beats from a start that is on no hit up to the first hit followed

    int numConf() const
    {
        int n = 0;
        for (char c : conf)
            n += c != 0;
        return n;
    }
};

// Pin selection. Starts with the first and last snapped beats and keeps adding the pin that best
// repairs the worst beat until every snapped beat is within tol of the straight lines between pins.
// Among pins that repair it, a beat 1 is preferred, then the middle of the bar.
std::vector<int> choosePins (const std::vector<double>& tt, const std::vector<char>& conf, double phase, int beatsPerBar, double tol)
{
    const int n = (int) tt.size();
    std::vector<int> ci;
    for (int i = 0; i < n; ++i)
        if (conf[(size_t) i])
            ci.push_back (i);
    std::vector<int> A { ci.front(), ci.back() };

    auto priority = [&] (int i)
    {
        double b = std::fmod ((double) i - phase, (double) beatsPerBar);
        if (b < 0.0)
            b += beatsPerBar;
        const auto is = [] (double x, double y) { return std::equal_to<double>() (x, y); };   // exact on purpose
        return is (b, 0.0) ? 0 : ((beatsPerBar % 2 == 0 && is (b, beatsPerBar / 2)) ? 1 : 2);
    };
    // Largest error of the snapped beats a..b against the line from beat p to beat q.
    auto lineError = [&] (int p, int q, int a, int b)
    {
        double worst = 0.0;
        if (q == p)
            return worst;
        for (int i = a; i <= b; ++i)
            if (conf[(size_t) i])
                worst = std::max (worst, std::abs (tt[(size_t) p] + (tt[(size_t) q] - tt[(size_t) p]) * (i - p) / (double) (q - p) - tt[(size_t) i]));
        return worst;
    };

    for (;;)
    {
        int w = -1, wa = 0, wb = 0;
        double worst = 0.0;
        for (size_t s = 0; s + 1 < A.size(); ++s)
            for (int i = A[s] + 1; i < A[s + 1]; ++i)
                if (conf[(size_t) i])
                {
                    const double e = std::abs (tt[(size_t) A[s]] + (tt[(size_t) A[s + 1]] - tt[(size_t) A[s]]) * (i - A[s]) / (double) (A[s + 1] - A[s]) - tt[(size_t) i]);
                    if (e > worst)
                    {
                        worst = e;
                        w = i;
                        wa = A[s];
                        wb = A[s + 1];
                    }
                }
        if (w < 0 || worst <= tol)
            break;

        int best = -1, bestNotOk = 0, bestPrio = 0;
        double bestErr = 0.0;
        for (int c : ci)
        {
            if (c <= wa || c >= wb)
                continue;
            const double m = std::max (lineError (wa, c, wa, c), lineError (c, wb, c, wb));
            const int notOk = m <= std::max (tol, 0.5 * worst) ? 0 : 1;
            const int prio = notOk ? 0 : priority (c);
            if (best < 0 || std::tie (notOk, prio, m) < std::tie (bestNotOk, bestPrio, bestErr))
            {
                best = c;
                bestNotOk = notOk;
                bestPrio = prio;
                bestErr = m;
            }
        }
        if (best < 0)
            break;
        A.insert (std::upper_bound (A.begin(), A.end(), best), best);
    }
    return A;
}

//==============================================================================
struct Mapper
{
    Hits h;
    Tuning c;
    int beatsPerBar = 4;
    double pinTol = 0.010;

    // Is `to` a whole number of beats (up to a bar, and at least one) after `from`?
    bool wholeBeatsOn (double from, double to, double period) const
    {
        const double d = to - from;
        const double beats = std::round (d / period);
        return beats >= 1.0 && beats <= (double) std::max (4, beatsPerBar)
               && std::abs (d - beats * period) <= c.jTolMs / 1000.0 + 0.005 * d;
    }

    // The beat carries on across a hit-free stretch when the hit that ends it (index i) is a whole
    // number of beats after the hit before it, and another hit follows a whole number of beats after
    // that: music with a hit on every second beat, or once a bar.
    bool sparseBeatContinues (double prev, int i, double period) const
    {
        const double here = h.on[(size_t) i];
        if (! wholeBeatsOn (prev, here, period))
            return false;
        const double reach = here + ((double) std::max (4, beatsPerBar) + 0.5) * period;
        for (int j = i + 1; j < h.size() && h.on[(size_t) j] <= reach; ++j)
            if (h.st[(size_t) j] >= c.thrOn && wholeBeatsOn (here, h.on[(size_t) j], period))
                return true;
        return false;
    }

    // First stretch longer than gapBeats after tStart with no hit, or with hits that are not on
    // the continuing grid: (last hit before, first hit after).
    std::optional<std::pair<double, double>> findGap (double tStart, double period) const
    {
        double prev = 0.0;
        bool have = false;
        for (int i = h.lowerBound (tStart - eps); i < h.size(); ++i)
        {
            if (h.st[(size_t) i] < c.thrOn)
                continue;
            if (have && h.on[(size_t) i] - prev > c.gapBeats * period && ! sparseBeatContinues (prev, i, period))
                return std::make_pair (prev, h.on[(size_t) i]);
            prev = h.on[(size_t) i];
            have = true;
        }
        return std::nullopt;
    }

    std::optional<double> nextStrong (double t) const
    {
        for (int i = h.lowerBound (t - eps); i < h.size(); ++i)
            if (h.st[(size_t) i] >= c.thrStrong)
                return h.on[(size_t) i];
        return std::nullopt;
    }

    // After a gap: the restart hit is the strong hit, within the first two beats of the new section,
    // whose grid at the prior tempo is best supported by the following hits.
    double pickStart (double tFirst, double tEnd, double period) const
    {
        const double tol = c.jTolMs / 1000.0;
        bool have = false;
        double bestSupport = 0.0, best = tFirst;
        for (int i = h.lowerBound (tFirst - eps); i < h.size() && h.on[(size_t) i] <= tFirst + 2.0 * period; ++i)
        {
            if (h.st[(size_t) i] < c.thrStrong)
                continue;
            const double cand = h.on[(size_t) i];
            const int L = (int) std::min ((double) c.look, (tEnd - cand) / period + 1.0);
            if (L < 1)
                continue;
            double sup = 0.0;
            for (double e : { -0.02, -0.01, 0.0, 0.01, 0.02 })
                sup = std::max (sup, sum (supportOnLines (h, cand, period * (1.0 + e), L, tol)));
            if (! have || sup > 1.1 * bestSupport)   // an earlier hit keeps the place unless a later one is clearly better
            {
                have = true;
                bestSupport = sup;
                best = cand;
            }
        }
        return best;
    }

    // Change point right after beat time t? Compares hit support (summed strength on the grid lines)
    // for the continuing grid with every shifted grid that starts on a hit in the next two beats.
    // Returns the restart hit time.
    std::optional<double> jumpTest (double t, double P, int nlock, int N) const
    {
        const double tol = c.jTolMs / 1000.0;
        const double epsSet[] = { -0.01, 0.0, 0.01 };
        double cont = 0.0;
        for (double e : epsSet)
            cont = std::max (cont, sum (supportOnLines (h, t + P * (1.0 + e), P * (1.0 + e), N, tol)));
        if (shareAtLeast (supportOnLines (h, t, -P, nlock, tol), c.thrOn) < c.jLock)
            return std::nullopt;

        const double ms = c.jMinShiftMs / 1000.0;
        bool have = false;
        double bestScore = 0.0, best = 0.0;
        for (int i = h.upperBound (t + ms); i < h.size() && h.on[(size_t) i] < t + 2.0 * P - ms; ++i)
        {
            if (h.st[(size_t) i] < c.thrOn)
                continue;
            const double g0 = h.on[(size_t) i];
            if (std::abs (g0 - (t + P)) < ms)
                continue;
            if (have && std::abs (g0 - best - P) < 2.0 * tol)
                continue;   // same shifted grid one beat later: keep its first hit
            const double before = sum (supportOnLines (h, g0 - P, -P, c.jBefore, tol)) / c.jBefore;
            for (double e : epsSet)
            {
                const auto s = supportOnLines (h, g0, P * (1.0 + e), N, tol);
                const double total = sum (s);
                if (shareAtLeast (s, c.thrOn) >= (N >= c.jAfter ? c.jAfterMin : 1.0) && total >= c.jMargin * cont
                    && before <= c.jBeforeMax * total / std::max (1, N))
                {
                    const double score = total - cont;
                    if (! have || score > bestScore + 1.0e-9)
                    {
                        have = true;
                        bestScore = score;
                        best = g0;
                    }
                }
            }
        }
        return have ? std::optional<double> (best) : std::nullopt;
    }

    // Walk a stiff grid from the section start; at each locked beat ask jumpTest whether the following
    // bars fit a shifted grid clearly better than the continuing one. Returns the restart hit time.
    std::optional<double> findJump (double tStart, double tEnd, double period) const
    {
        double t = tStart, P = period;
        const double P0 = P;
        const double tol = c.jTolMs / 1000.0;
        if (h.strongestNear (tStart, 0.030, c.thrOn) < 0)   // a start placed on no hit: walk from the first hit after it
            for (int i = h.lowerBound (tStart); i < h.size() && h.on[(size_t) i] < tStart + 2.0 * P; ++i)
                if (h.st[(size_t) i] >= c.thrOn)
                {
                    t = h.on[(size_t) i];
                    break;
                }
        for (int k = 0; t + P * (c.jAfterShort + 1) <= tEnd; ++k)
        {
            if (k >= 3)
            {
                const int N = (int) std::min ((double) c.jAfter, (tEnd - t) / P - 1.0);   // fewer bars of look-ahead near the section end
                if (auto r = jumpTest (t, P, std::min (k + 1, c.jBefore), N))
                {
                    double g = *r;
                    for (int n = 0; n < 2; ++n)   // the shifted grid may have started a beat or two earlier
                    {
                        const int i = h.strongestNear (g - P, tol, c.thrOn, tStart + P);
                        if (i < 0)
                            break;
                        g = h.on[(size_t) i];
                    }
                    return g;
                }
            }
            const double next = t + P;
            const int i = h.strongestNear (next, std::min (0.025, 0.08 * P), c.thrOn);
            if (i >= 0)
            {
                const double err = h.on[(size_t) i] - next;
                t = next + 0.6 * err;
                P = std::clamp (P + 0.1 * err, 0.94 * P0, 1.06 * P0);
            }
            else
                t = next;
        }
        return std::nullopt;
    }

    // Track from tStart (beat 0) to tEnd. With exactStart, beat 0 stays on tStart whatever hit is near it.
    // When tStart is on no hit, the beat is also followed from the first strong hit after it, that hit
    // being given the nearest whole beat (at least 1), and the result that puts more beats on hits is used:
    // a start that is out of step with the hits after it would otherwise drag the first bars off them.
    // The same is done for a start the user gave (`givenStart`) that is on a hit nothing can be followed from.
    Track trackSection (double tStart, double tEnd, double period, double beat0, bool exactStart, bool givenStart) const
    {
        auto track = trackFrom (tStart, tEnd, period, beat0, exactStart);
        if (h.strongestNear (tStart, 0.030, 0.0) >= 0 && ! (givenStart && track.tt.size() < 2))
            return track;

        // The hit to follow from is chosen as a restart hit is after a gap.
        const auto strong = nextStrong (tStart + 0.030);
        if (! strong || *strong > tEnd + eps)
            return track;
        const double first = pickStart (*strong, tEnd, period);
        const int lead = std::max (1, (int) roundHalfEven ((first - tStart) / period));
        const auto later = trackFrom (first, tEnd, period, beat0 + lead, false);
        if (later.tt.size() < 2 || later.numConf() + 1 <= track.numConf())
            return track;   // following from tStart itself put at least as many beats on hits

        Track out;
        out.period = later.period;
        out.lead = lead;
        for (int k = 0; k < lead; ++k)
        {
            out.tt.push_back (tStart + (later.tt.front() - tStart) * k / (double) lead);
            out.conf.push_back (k == 0 ? 1 : 0);
        }
        out.tt.insert (out.tt.end(), later.tt.begin(), later.tt.end());
        out.conf.insert (out.conf.end(), later.conf.begin(), later.conf.end());
        out.pins.push_back (0);
        for (int a : later.pins)
            out.pins.push_back (a + lead);
        return out;
    }

    //==============================================================================
    // The constant-tempo test.

    // What a grid line has near it: nothing, a hit on it (within `near`), or only a hit off it (within `wide`).
    struct LineHit
    {
        enum Kind { none, on, off };
        Kind kind = none;
        double offset = 0.0;     // hit time less line time
        double strength = 0.0;
        double time = 0.0;
    };

    LineHit hitOnLine (double line, double near, double wide) const
    {
        LineHit out;
        for (int i = h.lowerBound (line - wide); i < h.size() && h.on[(size_t) i] <= line + wide; ++i)
        {
            if (h.st[(size_t) i] < c.thrOn)
                continue;
            const double d = h.on[(size_t) i] - line;
            const auto kind = std::abs (d) <= near ? LineHit::on : LineHit::off;
            if (out.kind == LineHit::none || (kind == LineHit::on && out.kind == LineHit::off)
                || (kind == out.kind && h.st[(size_t) i] > out.strength))
                out = { kind, d, h.st[(size_t) i], h.on[(size_t) i] };
        }
        return out;
    }

    double steadyWide (double P) const   { return std::max (pinTol, std::min (c.steadyWideMs / 1000.0, 0.2 * P)); }

    // The lines 0..L of the grid a + k * P. With `tied`, line 0 is the start of the stretch itself,
    // whatever is near it: the grid has to pass close to the start.
    std::vector<LineHit> steadyLines (double tStart, double a, double P, int L, bool tied) const
    {
        std::vector<LineHit> lines ((size_t) L + 1);
        if (tied)
            lines[0] = { LineHit::on, tStart - a, 1.0, tStart };
        for (int k = tied ? 1 : 0; k <= L; ++k)
            lines[(size_t) k] = hitOnLine (a + k * P, pinTol, steadyWide (P));
        return lines;
    }

    // Hit strength on lines 1..L of the grid, less for hits off the line.
    double steadySupport (double a, double P, int L) const
    {
        double total = 0.0;
        for (int k = 1; k <= L; ++k)
        {
            const double line = a + k * P;
            double best = 0.0;
            for (int i = h.lowerBound (line - pinTol); i < h.size() && h.on[(size_t) i] <= line + pinTol; ++i)
            {
                const double d = (h.on[(size_t) i] - line) / pinTol;
                if (h.st[(size_t) i] >= c.thrOn)
                    best = std::max (best, h.st[(size_t) i] * (1.0 - 0.5 * d * d));
            }
            total += best;
        }
        return total;
    }

    // The beat length within `range` (a share of P) whose grid from a is best supported over L lines.
    // P itself is kept unless another is better by a twentieth.
    double steadySearch (double a, double P, int L, double range, int steps) const
    {
        double best = P, bestScore = steadySupport (a, P, L) * 1.05;
        for (int j = -steps; j <= steps; ++j)
        {
            const double cand = P * (1.0 + range * j / steps);
            const double score = steadySupport (a, cand, L);
            if (score > bestScore)
            {
                bestScore = score;
                best = cand;
            }
        }
        return best;
    }

    // Least squares through the hits on lines 0..L (and the start, when tied), each weighted by its strength.
    void steadyFit (double tStart, double& a, double& P, int L, bool tied) const
    {
        for (int pass = 0; pass < 4; ++pass)
        {
            const auto lines = steadyLines (tStart, a, P, L, tied);
            double sw = 0.0, sk = 0.0, st = 0.0, skk = 0.0, skt = 0.0;
            for (int k = 0; k <= L; ++k)
            {
                const auto& line = lines[(size_t) k];
                if (line.kind != LineHit::on)
                    continue;
                const double w = line.strength, t = line.time - tStart;
                sw += w;
                sk += w * k;
                st += w * t;
                skk += w * k * k;
                skt += w * k * t;
            }
            const double den = sw * skk - sk * sk;
            if (den <= 1.0e-12)
                return;
            P = (sw * skt - sk * st) / den;
            a = tStart + (st - P * sk) / sw;
        }
    }

    struct SteadyScan
    {
        int on = 0, off = 0;
        int lastOn = 0;          // the last line with a hit on it
        int firstBad = -1;       // the first line of a run of beats off the grid on one side, or -1
    };

    // Counts the lines with a hit on them and off them, and looks for the place where the hits leave the
    // grid for good: steadyRun lines in a row that have a hit, or steadyRunStrong in a row that have a
    // strong one, all early or all late by more than a limit. One hit out of place is never enough.
    // The limit is steadyLimit of the pin tolerance. Where the hits on the grid sit very evenly (their
    // offsets hardly change from one to the next) it is less, down to steadyFloor of the tolerance,
    // so a slow drift in clean audio is seen early.
    SteadyScan steadyScan (const std::vector<LineHit>& lines) const
    {
        SteadyScan scan;
        std::vector<int> with;
        std::vector<double> steps;
        int prevOn = -1;
        for (int k = 0; k < (int) lines.size(); ++k)
        {
            const auto& line = lines[(size_t) k];
            if (line.kind == LineHit::none)
                continue;
            with.push_back (k);
            if (line.kind == LineHit::off)
            {
                ++scan.off;
                continue;
            }
            ++scan.on;
            scan.lastOn = k;
            if (prevOn >= 0 && k - prevOn <= 2)
                steps.push_back (std::abs (line.offset - lines[(size_t) prevOn].offset));
            prevOn = k;
        }
        double limit = c.steadyLimit * pinTol;
        if (steps.size() >= 6)
            limit = std::clamp (3.0 * 1.4826 * medianOf (steps) / std::sqrt (2.0), c.steadyFloor * pinTol, limit);

        // A run among all the lines with a hit, or a shorter one among those with a strong hit.
        std::vector<int> strong;
        for (int k : with)
            if (lines[(size_t) k].strength >= c.thrStrong)
                strong.push_back (k);
        auto firstRun = [&] (const std::vector<int>& set, int length)
        {
            const auto run = (size_t) std::max (1, length);
            for (size_t i = 0; i + run <= set.size(); ++i)
            {
                if (set[i + run - 1] - set[i] > 3 * (int) run)
                    continue;
                size_t late = 0, early = 0;
                for (size_t j = i; j < i + run; ++j)
                {
                    late += lines[(size_t) set[j]].offset > limit;
                    early += lines[(size_t) set[j]].offset < -limit;
                }
                if (late == run || early == run)
                    return set[i];
            }
            return -1;
        };
        const int inAll = firstRun (with, c.steadyRun), inStrong = firstRun (strong, c.steadyRunStrong);
        scan.firstBad = inAll < 0 ? inStrong : (inStrong < 0 ? inAll : std::min (inAll, inStrong));
        return scan;
    }

    // A grid tied to its start passes within the pin tolerance of it. One that is not tied (the start
    // was placed by hand, or is where an earlier grid ended) still has the start as its beat 0, and
    // passes within `reach` of it.
    bool steadyHolds (const SteadyScan& scan, double tStart, double a, double reach) const
    {
        return scan.firstBad < 0 && scan.on >= 3 * scan.off && std::abs (a - tStart) <= reach;
    }

    bool startsOnHit (double t) const   { return h.strongestNear (t, c.regularNear * pinTol, c.thrOn) >= 0; }

    // A constant grid from tStart, as far as it holds.
    struct Steady
    {
        double a = 0.0, P = 0.0;
        int last = 0;                    // the last line with a hit on it
        int on = 0;
        bool ended = false;              // the hits left the grid after `last`
        std::vector<LineHit> lines;      // lines 0..last
    };

    // Fits a constant grid over the first beats, then over twice as many, and so on to tEnd. When a
    // longer stretch does not hold, the last fit that did is carried forward and ends on the last hit
    // on it before the hits leave it. Nothing is returned when no stretch of steadyMinBeats holds.
    // A grid that is not tied to tStart may sit up to freeReach from it (35 ms, or a fifth of a beat
    // if that is less, when freeReach is not given).
    std::optional<Steady> steadyHead (double tStart, double tEnd, double period, bool tied, double freeReach = 0.0) const
    {
        if (! c.steady)
            return std::nullopt;
        auto linesTo = [&] (double a, double P) { return (int) std::floor ((tEnd + pinTol - a) / P); };
        if (linesTo (tStart, period) < c.steadyMinBeats)
            return std::nullopt;

        double a = tStart, P = period;
        const double reach = tied ? pinTol : (freeReach > 0.0 ? freeReach : steadyWide (P));
        if (! tied)
        {
            // Where the grid sits against the start: the place within reach that the first beats support best.
            const int L = std::min (c.steadyFirst, linesTo (a, P));
            double bestScore = steadySupport (a, P, L) * 1.05;
            for (double d = -reach; d <= reach; d += 0.001)
            {
                const double score = steadySupport (tStart + d, P, L);
                if (score > bestScore)
                {
                    bestScore = score;
                    a = tStart + d;
                }
            }
        }
        double heldA = 0.0, heldP = 0.0;
        int heldL = 0;
        bool whole = false;
        for (int L = std::min (c.steadyFirst, linesTo (a, P)); ;)
        {
            // Until a fit has held, the search is 3 percent either side of the tempo given. After that
            // it goes as far as moves the last line by 25 ms, in steps of 1 ms.
            double fa = a, fP = P;
            if (heldL == 0)
                fP = steadySearch (fa, fP, L, 0.03, std::max (1, (int) std::ceil (0.03 * L * fP / 0.001)));
            else
                fP = steadySearch (fa, fP, L, 0.025 / (L * fP), 25);
            steadyFit (tStart, fa, fP, L, tied);
            const bool holds = std::abs (fP / period - 1.0) < 0.08 && steadyHolds (steadyScan (steadyLines (tStart, fa, fP, L, tied)), tStart, fa, reach);
            if (! holds)
            {
                if (heldL == 0 && L > c.steadyMinBeats)
                {
                    L = c.steadyMinBeats;   // one more try over fewer beats
                    continue;
                }
                break;
            }
            heldA = a = fa;
            heldP = P = fP;
            heldL = L;
            const int all = linesTo (a, P);
            if (L >= all)
            {
                whole = true;
                break;
            }
            L = std::min (all, 2 * L);
        }
        if (heldL == 0)
            return std::nullopt;

        Steady out;
        out.a = heldA;
        out.P = heldP;
        int L = heldL;
        if (! whole)
        {
            // Carry the grid that held on to the end and stop before the hits leave it.
            L = linesTo (out.a, out.P);
            const auto scan = steadyScan (steadyLines (tStart, out.a, out.P, L, tied));
            L = scan.firstBad >= 0 ? std::max (heldL, scan.firstBad - 1) : heldL;
            out.ended = true;
            // The fit over the longer stretch is used if it holds there.
            double fa = out.a, fP = out.P;
            steadyFit (tStart, fa, fP, L, tied);
            if (steadyHolds (steadyScan (steadyLines (tStart, fa, fP, L, tied)), tStart, fa, reach))
            {
                out.a = fa;
                out.P = fP;
            }
        }
        out.lines = steadyLines (tStart, out.a, out.P, L, tied);
        const auto scan = steadyScan (out.lines);
        out.last = scan.lastOn;
        out.on = scan.on;
        out.lines.resize ((size_t) out.last + 1);
        if (out.on < c.steadyMinBeats || out.last < 1)
            return std::nullopt;
        return out;
    }

    // Track from tStart (beat 0) to tEnd: on a constant grid as far as one holds, then hit by hit.
    // With exactStart, beat 0 stays on tStart, and when no hit is at tStart the grid after it is fitted
    // to the hits alone. With freeReach, the grid may sit that far either side of tStart: used after a
    // grid that ended with strong hits still to come, which a jump in the beat leaves behind.
    Track trackFrom (double tStart, double tEnd, double period, double beat0, bool exactStart, double freeReach = 0.0) const
    {
        const auto steady = steadyHead (tStart, tEnd, period, freeReach <= 0.0 && (! exactStart || startsOnHit (tStart)), freeReach);
        if (! steady)
            return followHits (tStart, tEnd, period, beat0, exactStart);

        Track tr;
        tr.period = steady->P;
        tr.fitPeriod = steady->P;
        for (int k = 0; k <= steady->last; ++k)
        {
            tr.tt.push_back (k == 0 ? tStart : steady->a + k * steady->P);
            tr.conf.push_back (k == 0 || steady->lines[(size_t) k].kind == LineHit::on ? 1 : 0);
        }

        // A pin on a line of the grid goes on the line's hit when that is a strong one sitting close
        // to the line, and on the line itself otherwise, so a weak or loose hit does not tilt the grid.
        auto pinTime = [&] (int k)
        {
            const auto& line = steady->lines[(size_t) k];
            const bool onHit = line.kind == LineHit::on && line.strength >= c.thrStrong && std::abs (line.offset) <= c.regularNear * pinTol;
            return onHit ? line.time : steady->a + k * steady->P;
        };
        // The start is a pin wherever it is. When it is not close to the grid, the next beat on a hit is a pin too.
        tr.pins = { 0 };
        if (std::abs (tStart - steady->a) > c.regularNear * pinTol)
            for (int k = 1; k < steady->last; ++k)
                if (steady->lines[(size_t) k].kind == LineHit::on)
                {
                    tr.tt[(size_t) k] = pinTime (k);
                    tr.pins.push_back (k);
                    for (int j = 1; j < k; ++j)   // the beats between lie on the straight line from the start
                        tr.tt[(size_t) j] = tStart + (tr.tt[(size_t) k] - tStart) * j / (double) k;
                    break;
                }
        const double lastTime = pinTime (steady->last);
        tr.tt.back() = lastTime;

        // Where the hits leave the grid, and where strong hits come after its end, the beat is followed on from its last beat.
        int strongAfter = 0;
        for (int i = h.upperBound (lastTime + 0.5 * steady->P); i < h.size() && h.on[(size_t) i] <= tEnd + eps; ++i)
            strongAfter += h.st[(size_t) i] >= c.thrStrong;
        if (steady->ended || strongAfter >= c.minSec)
        {
            auto rest = trackFrom (lastTime, tEnd, steady->P, beat0 + steady->last, true, steady->ended ? 0.0 : 0.5 * steady->P);
            if (rest.tt.size() >= 2)
            {
                tr.tt.resize ((size_t) steady->last);
                tr.conf.resize ((size_t) steady->last);
                tr.tt.insert (tr.tt.end(), rest.tt.begin(), rest.tt.end());
                tr.conf.insert (tr.conf.end(), rest.conf.begin(), rest.conf.end());
                for (int p : rest.pins)
                    tr.pins.push_back (p + steady->last);
                tr.period = rest.period;
                tr.fitPeriod = 0.0;
                return tr;
            }
        }
        tr.pins.push_back (steady->last);
        return tr;
    }

    // The same, hit by hit all the way.
    Track followHits (double tStart, double tEnd, double period, double beat0, bool exactStart) const
    {
        const int a = h.lowerBound (tStart - 0.03), b = h.upperBound (tEnd + 0.03);
        const std::vector<double> o (h.on.begin() + a, h.on.begin() + std::max (a, b));
        const std::vector<double> s (h.st.begin() + a, h.st.begin() + std::max (a, b));

        Track trivial;
        trivial.tt = { tStart };
        trivial.conf = { 1 };
        trivial.pins = { 0 };
        trivial.period = period;
        if (o.size() < 3)
            return trivial;

        std::map<int, int> path;
        std::map<int, double> snapped;
        std::vector<double> tt;
        auto once = [&] (double per)
        {
            path = trackFromHint (o, s, tStart, per, tEnd);
            snapped.clear();
            for (const auto& [k, i] : path)
                snapped[k] = o[(size_t) i];
            snapped.emplace (0, tStart);
            tt = fillBeats (snapped, snapped.rbegin()->first);
        };
        once (period);
        if (snapped.size() >= 6)   // local tempo from the section's own hits
        {
            double mk = 0.0, mt = 0.0, num = 0.0, den = 0.0;
            for (const auto& [k, t] : snapped)
            {
                mk += k;
                mt += t;
            }
            mk /= (double) snapped.size();
            mt /= (double) snapped.size();
            for (const auto& [k, t] : snapped)
            {
                num += (k - mk) * (t - mt);
                den += (k - mk) * (k - mk);
            }
            const double per2 = num / den;
            if (std::abs (per2 / period - 1.0) < 0.08)
            {
                period = per2;
                once (period);
            }
        }
        if (tt.size() < 3 || path.size() < 2)
            return trivial;

        const int last = (int) tt.size() - 1;
        for (int pass = 0; pass < 2; ++pass)
        {
            snapped.clear();
            for (const auto& [k, i] : path)
                snapped[k] = o[(size_t) i];
            const auto grid = smoothGrid (tt, snapped);
            const double per = medianDiff (grid);
            path = snapToGrid (o, s, grid, std::min (0.035, 0.2 * per));
            if (path.empty())
                return trivial;
            snapped.clear();
            for (const auto& [k, i] : path)
                snapped[k] = o[(size_t) i];
            tt = fillBeats (snapped, last);
        }

        Track tr;
        tr.conf.assign (tt.size(), 0);
        for (const auto& [k, i] : path)
            tr.conf[(size_t) k] = 1;
        if (! tr.conf[0] || exactStart)
        {
            tr.conf[0] = 1;
            tt[0] = tStart;
        }
        auto strongNear = [&] (double t) -> int
        {
            int best = -1;
            for (size_t i = 0; i < o.size(); ++i)
                if (std::abs (o[i] - t) <= c.rescueMs / 1000.0 && s[i] >= c.thrStrong && (best < 0 || s[i] > s[(size_t) best]))
                    best = (int) i;
            return best;
        };
        if (c.rescueMs > 0.0)   // beats the snap left out although a strong hit sits close to the line
            for (size_t i = 0; i < tt.size(); ++i)
                if (! tr.conf[i])
                    if (const int j = strongNear (tt[i]); j >= 0)
                    {
                        tt[i] = o[(size_t) j];
                        tr.conf[i] = 1;
                    }
        size_t lastConf = 0;
        for (size_t i = 0; i < tt.size(); ++i)
            if (tr.conf[i])
                lastConf = i;
        tt.resize (lastConf + 1);
        tr.conf.resize (lastConf + 1);
        if (c.rescueMs > 0.0)   // follow strong hits beat by beat up to the end of the section
            while (tt.size() >= 3)
            {
                const double next = tt.back() + medianDiff (tt, tt.size() >= 5 ? tt.size() - 5 : 0);
                const int j = strongNear (next);
                if (j < 0 || o[(size_t) j] <= tt.back())
                    break;
                tt.push_back (o[(size_t) j]);
                tr.conf.push_back (1);
            }
        tr.tt = std::move (tt);
        if (tr.numConf() >= 2)
        {
            double phase = std::fmod (-beat0, (double) beatsPerBar);   // index, from this section's start, of a beat 1
            if (phase < 0.0)
                phase += beatsPerBar;
            tr.pins = choosePins (tr.tt, tr.conf, phase, beatsPerBar, pinTol);
            tr.period = medianDiff (tr.tt);
        }
        else
        {
            tr.pins = { 0 };
            tr.period = period;
        }
        return tr;
    }

    // Does the constant grid of the stretch from `from` to the gap still fit the hits after the gap?
    // It does when, of its lines over the four bars after the gap (or up to the next gap), at least
    // steadyCarry and at least a quarter have a hit on them, and no more than half as many have a
    // hit off them. `nextGap` is the gap after that, if there is one.
    // `tied` is false when `from` is a pin the user placed.
    bool gridCarriesOn (double from, bool tied, const std::pair<double, double>& gap, double period,
                        const std::optional<std::pair<double, double>>& nextGap) const
    {
        const auto steady = steadyHead (from, gap.first, period, tied || startsOnHit (from));
        if (! steady || steady->ended || steady->on < 2 * c.steadyMinBeats)
            return false;
        if (steady->lines.back().time < gap.first - 2.0 * steady->P)
            return false;   // the grid stopped well before the gap
        const double k0 = std::ceil ((gap.second - pinTol - steady->a) / steady->P);
        const double until = nextGap ? nextGap->first : inf;
        int on = 0, off = 0, count = 0;
        for (int n = 0; n < 4 * beatsPerBar; ++n)
        {
            const double line = steady->a + (k0 + n) * steady->P;
            if (line > until + pinTol)
                break;
            const auto hit = hitOnLine (line, pinTol, steadyWide (steady->P));
            ++count;
            on += hit.kind == LineHit::on;
            off += hit.kind == LineHit::off;
        }
        return on >= c.steadyCarry && 4 * on >= count && on >= 2 * off;
    }

    // Support for a grid of `lines` beats from t0 at period P: hit strength, less for hits off the line.
    double gridSupport (double t0, double P, int lines) const
    {
        const double tol = c.jTolMs / 1000.0;
        double total = 0.0;
        for (int n = 1; n < lines; ++n)
        {
            const double line = t0 + n * P;
            double best = 0.0;
            for (int i = h.lowerBound (line - tol); i < h.size() && h.on[(size_t) i] <= line + tol; ++i)
            {
                const double d = (h.on[(size_t) i] - line) / tol;
                best = std::max (best, h.st[(size_t) i] * (1.0 - 0.5 * d * d));
            }
            total += best;
        }
        return total;
    }

    // Settles the hinted tempo on the audio: the tempo near the hint whose grid from the first hit
    // is best supported over the first beats, then a least-squares fit through the hits on that grid.
    double refineTempo (double t0, double bpm) const
    {
        const double P0 = 60.0 / bpm;
        const auto gap = findGap (t0, P0);
        const double tEnd = gap ? gap->first : (h.on.empty() ? t0 : h.on.back());
        const int lines = std::min (c.refineBeats, (int) ((tEnd - t0) / P0)) + 1;
        if (lines < c.refineMinBeats + 1)
            return bpm;

        double best = P0, bestScore = gridSupport (t0, P0, lines) * c.refineKeepHint;
        const int steps = (int) std::lround (c.refineRange / c.refineStep);
        for (int k = -steps; k <= steps; ++k)
        {
            const double P = P0 * (1.0 + k * c.refineStep);
            const double score = gridSupport (t0, P, lines);
            if (score > bestScore)
            {
                bestScore = score;
                best = P;
            }
        }

        const double tol = c.jTolMs / 1000.0;
        double P = best;
        for (int pass = 0; pass < 4; ++pass)
        {
            double num = 0.0, den = 0.0;
            for (int n = 1; n < lines; ++n)
            {
                const int i = h.strongestNear (t0 + n * P, tol, 0.0);
                if (i < 0)
                    continue;
                num += h.st[(size_t) i] * n * (h.on[(size_t) i] - t0);
                den += h.st[(size_t) i] * n * n;
            }
            if (den <= 0.0)
                break;
            P = num / den;
        }
        return std::abs (P / best - 1.0) < 0.02 ? 60.0 / P : 60.0 / best;
    }
};

double bridgeBeat (double raw, double lastBeat, Options::Bridge rule)
{
    if (rule == Options::Bridge::halfBeat)
        return lastBeat + std::max (0.5, roundHalfEven (raw * 2.0) / 2.0);
    return lastBeat + std::max (1.0, roundHalfEven (raw));
}

std::string barBeatQuestion (double beat, int beatsPerBar)
{
    const double half = std::round (beat * 2.0) / 2.0;
    const int bar = (int) std::floor (half / beatsPerBar);
    const double inBar = half - bar * beatsPerBar + 1.0;
    char text[96];
    if (inBar == std::floor (inBar))
        std::snprintf (text, sizeof (text), "Beat restarts. Bar %d beat %d?", bar + 1, (int) inBar);
    else
        std::snprintf (text, sizeof (text), "Beat restarts. Bar %d beat %.1f?", bar + 1, inBar);
    return text;
}

} // namespace

//==============================================================================
// What is done with input that cannot be used as given:
//  - A tempo hint that is not a number, or outside 20 to 400 BPM, is treated as not given.
//  - A first-hit time that is not a number, or after the end of the music, is treated as not given.
//  - Hits and user pins whose time or beat is not a number are left out.
//  - User pins must move forward in both time and beat. Where two do not, the one given later in
//    the list is kept, the other is left out of the map, and a check says so at its time.
MapResult detail::mapCueWith (const std::vector<Onset>& onsetsIn, double durationIn, const Hint& hintIn,
                              const std::vector<Pin>& userPinsIn, const Options& options, const Tuning& tuning)
{
    MapResult result;
    for (auto o : onsetsIn)
        if (std::isfinite (o.time))
        {
            if (! std::isfinite (o.strength))
                o.strength = 0.0f;
            result.onsets.push_back (o);
        }

    Hint hint = hintIn;
    if (! std::isfinite (hint.bpm) || hint.bpm < 20.0 || hint.bpm > 400.0)
        hint.bpm = 0.0;
    double durationSeconds = (std::isfinite (durationIn) && durationIn > 0.0) ? durationIn : 0.0;
    for (const auto& o : result.onsets)
        durationSeconds = std::max (durationSeconds, o.time);
    if (! std::isfinite (hint.firstHitTime) || hint.firstHitTime > durationSeconds)
        hint.firstHitTime = -1.0;

    std::stable_sort (result.onsets.begin(), result.onsets.end(), [] (const Onset& a, const Onset& b) { return a.time < b.time; });

    Mapper m;
    m.c = tuning;
    m.beatsPerBar = std::max (1, hint.beatsPerBar);
    m.pinTol = std::max (0.5, options.pinToleranceMs) / 1000.0;
    for (const auto& o : result.onsets)
    {
        m.h.on.push_back (o.time);
        m.h.st.push_back ((double) o.strength);
    }
    const auto& h = m.h;
    const double snap = tuning.userSnapMs / 1000.0;

    // The user's pins, in time order, each marked user. A pin that does not move forward in both
    // time and beat from another is in conflict with it: the one given later is kept.
    // `used` marks the ones that start a stretch.
    std::vector<Pin> user;
    std::vector<double> leftOut;   // times of the user pins left out
    for (auto p : userPinsIn)
    {
        if (! std::isfinite (p.time) || ! std::isfinite (p.beat))
            continue;
        p.user = true;
        for (size_t i = user.size(); i-- > 0;)
        {
            const auto& q = user[i];
            const bool sameTime = std::abs (q.time - p.time) < 1.0e-6;
            const bool forward = ! sameTime && ((q.time < p.time) == (q.beat < p.beat)) && std::abs (q.beat - p.beat) > 1.0e-9;
            if (! forward)
            {
                if (! (sameTime && std::abs (q.beat - p.beat) < 1.0e-9))   // the same pin given twice is not worth a check
                    leftOut.push_back (q.time);
                user.erase (user.begin() + (long) i);
            }
        }
        user.push_back (p);
    }
    std::stable_sort (user.begin(), user.end(), [] (const Pin& a, const Pin& b) { return a.time < b.time; });
    std::vector<char> used (user.size(), 0);
    auto userPinNear = [&] (double t) -> int
    {
        int best = -1;
        for (int i = 0; i < (int) user.size(); ++i)
            if (! used[(size_t) i] && std::abs (user[(size_t) i].time - t) <= snap
                && (best < 0 || std::abs (user[(size_t) i].time - t) < std::abs (user[(size_t) best].time - t)))
                best = i;
        return best;
    };

    // Where the music starts.
    double t = hint.firstHitTime;
    if (t < 0.0)
    {
        t = h.on.empty() ? 0.0 : h.on.front();
        for (int i = 0; i < h.size(); ++i)
            if (h.st[(size_t) i] >= tuning.thrStrong)
            {
                t = h.on[(size_t) i];
                break;
            }
    }
    else if (! h.on.empty())
    {
        int i0 = h.lowerBound (t);
        if (i0 > 0 && (i0 == h.size() || t - h.on[(size_t) i0 - 1] <= h.on[(size_t) i0] - t))
            --i0;
        if (std::abs (h.on[(size_t) i0] - t) < 0.030)
            t = h.on[(size_t) i0];
    }
    double beat = (double) (hint.bar - 1) * m.beatsPerBar + (hint.beat - 1);
    int startUser = userPinNear (t);
    if (startUser >= 0)
    {
        used[(size_t) startUser] = 1;
        t = user[(size_t) startUser].time;
        beat = user[(size_t) startUser].beat;
    }
    result.firstHitTime = t;

    double bpm = hint.bpm;
    if (bpm <= 0.0)
        bpm = estimateTempo (result.onsets, durationSeconds);
    if (bpm <= 0.0)
        bpm = 120.0;
    if (tuning.refineTempo)
    {
        // The tempo is settled on a grid from a strong hit: the start, or the first one after it.
        double anchor = t;
        if (h.strongestNear (t, 0.030, tuning.thrStrong) < 0)
            anchor = m.nextStrong (t).value_or (t);
        bpm = m.refineTempo (anchor, bpm);
    }
    result.bpm = bpm;
    double period = 60.0 / bpm;

    // One tracked stretch: a section, or the part of a section from one user pin to the next.
    struct Part
    {
        Track track;
        double beat0 = 0.0;
        int userIndex = -1;
    };
    struct Sec
    {
        std::vector<Part> parts;
        Section::Kind kind = Section::Kind::first;
        bool confirmed = true;
        double proposedBeat = 0.0;
        double period = 0.5;

        double lastTime() const   { return parts.back().track.tt.back(); }
        double lastBeat() const   { return parts.back().beat0 + (double) (parts.back().track.tt.size() - 1); }
    };
    std::vector<Sec> sections;

    auto kind = Section::Kind::first;
    bool confirmed = true;
    double proposed = beat;

    for (int guard = 0; guard < 100000; ++guard)
    {
        // Where does this section end?
        auto gap = m.findGap (t, period);

        int userRestart = -1;   // the next section start the user has placed
        for (int i = 0; i < (int) user.size(); ++i)
            if (! used[(size_t) i] && user[(size_t) i].restart && user[(size_t) i].time > t + snap)
            {
                userRestart = i;
                break;
            }

        // A gap does not end the section when the section's constant grid, from its start or from the
        // user's last pin before the gap, still fits the hits after it.
        while (gap && tuning.steady)
        {
            if (userRestart >= 0 && user[(size_t) userRestart].time <= gap->second + snap)
                break;
            double from = t;
            for (int i = 0; i < (int) user.size(); ++i)
                if (! used[(size_t) i] && ! user[(size_t) i].restart && user[(size_t) i].time > from && user[(size_t) i].time < gap->first)
                    from = user[(size_t) i].time;
            const auto nextGap = m.findGap (gap->second, period);
            if (! m.gridCarriesOn (from, startUser < 0 && from <= t, *gap, period, nextGap))
                break;
            gap = nextGap;
        }
        double tEnd = gap ? gap->first : durationSeconds;
        if (userRestart >= 0 && tEnd >= user[(size_t) userRestart].time - eps)
        {
            tEnd = std::max (t, h.lastBefore (user[(size_t) userRestart].time, t));
            gap.reset();
        }

        std::optional<double> next;
        auto nextKind = Section::Kind::gap;
        if (auto jump = options.detectJumps ? m.findJump (t, tEnd, period) : std::nullopt)
        {
            tEnd = h.lastBefore (*jump, t);
            next = jump;
            nextKind = Section::Kind::jump;
        }
        else if (gap)
        {
            next = m.nextStrong (gap->second);
            if (next)
            {
                const auto g2 = m.findGap (*next, period);
                next = m.pickStart (*next, g2 ? g2->first : durationSeconds, period);
            }
        }

        int nextUser = -1;
        if (userRestart >= 0 && (! next || *next > user[(size_t) userRestart].time + snap))
        {
            next = user[(size_t) userRestart].time;
            nextKind = Section::Kind::user;
            nextUser = userRestart;
            tEnd = std::min (tEnd, std::max (t, h.lastBefore (*next, t)));
        }
        else if (next)
            nextUser = userPinNear (*next);
        if (nextUser >= 0)
            used[(size_t) nextUser] = 1;

        // The user's other pins in this section split it: each stretch is tracked from its own pin.
        Sec sec;
        sec.kind = kind;
        sec.confirmed = confirmed;
        sec.proposedBeat = proposed;
        std::vector<std::pair<double, int>> starts { { t, startUser } };
        const double reach = next ? *next - snap : inf;
        for (int i = 0; i < (int) user.size(); ++i)
            if (! used[(size_t) i] && ! user[(size_t) i].restart && user[(size_t) i].time > t + snap && user[(size_t) i].time < reach)
            {
                used[(size_t) i] = 1;
                starts.push_back ({ user[(size_t) i].time, i });
            }
        double partPeriod = period;
        int snappedBeats = 0;
        for (size_t p = 0; p < starts.size(); ++p)
        {
            const double from = starts[p].first;
            double to = tEnd;
            if (p + 1 < starts.size())
                to = std::min (to, h.lastBefore (starts[p + 1].first, from));
            to = std::max (to, from);

            Part part;
            part.userIndex = starts[p].second;
            part.beat0 = p == 0 ? beat : user[(size_t) part.userIndex].beat;
            const bool givenStart = part.userIndex >= 0 || (sections.empty() && p == 0 && hint.firstHitTime >= 0.0);
            part.track = m.trackSection (from, to, partPeriod, part.beat0, part.userIndex >= 0, givenStart);   // a user pin's time is kept exactly
            partPeriod = part.track.period;
            snappedBeats += part.track.numConf();
            sec.parts.push_back (std::move (part));
        }
        sec.period = partPeriod;

        const bool keep = snappedBeats >= tuning.minSec || sections.empty() || starts.size() > 1 || startUser >= 0;
        if (keep)
        {
            sections.push_back (std::move (sec));
            period = sections.back().period;
        }
        if (! next)
            break;

        const auto& prev = sections.back();
        const double raw = (*next - prev.lastTime()) / prev.period;
        proposed = bridgeBeat (raw, prev.lastBeat(), options.bridge);
        kind = (keep || nextKind == Section::Kind::user) ? nextKind : Section::Kind::gap;
        t = *next;
        beat = proposed;
        confirmed = false;
        startUser = nextUser;
        if (nextUser >= 0)
        {
            t = user[(size_t) nextUser].time;
            beat = user[(size_t) nextUser].beat;
            confirmed = true;
        }
    }

    // Sections, and the engine's pins.
    std::vector<Pin> pins;
    std::vector<double> onHitShare;   // per section: the share of its beats that sit on a strong hit (1 for a short one)
    for (size_t s = 0; s < sections.size(); ++s)
    {
        const auto& sec = sections[s];
        Section out;
        out.kind = sec.kind;
        out.confirmed = sec.confirmed;
        out.startTime = sec.parts.front().track.tt.front();
        out.startBeat = sec.parts.front().beat0;

        // The tempo and the end come from the beats that were followed. The stretch up to a user
        // pin is not one of them, nor is a user pin after the last beat followed.
        double beats = 0.0, seconds = 0.0;
        int numBeats = 0, numOnHit = 0, numOnStrongHit = 0;
        out.endTime = sec.parts.front().track.tt.back();
        for (const auto& part : sec.parts)
        {
            const auto& tr = part.track;
            if ((int) tr.tt.size() - tr.lead < 2)
                continue;
            beats += (double) ((int) tr.tt.size() - 1 - tr.lead);
            seconds += tr.tt.back() - tr.tt[(size_t) tr.lead];
            numBeats += (int) tr.tt.size() - tr.lead;
            for (size_t i = (size_t) tr.lead; i < tr.conf.size(); ++i)
            {
                numOnHit += tr.conf[i] != 0;
                numOnStrongHit += h.support (tr.tt[i], tuning.jTolMs / 1000.0) >= tuning.thrStrong;
            }
            out.endTime = tr.tt.back();
        }
        out.bpm = (beats >= 2.0 && seconds > 0.0) ? 60.0 * beats / seconds : 60.0 / sec.period;
        // Where every stretch is one constant grid, the tempo is that of the fits, by their lengths.
        double fitBeats = 0.0, fitSeconds = 0.0;
        bool allFitted = true;
        for (const auto& part : sec.parts)
        {
            const auto& tr = part.track;
            const double n = (double) ((int) tr.tt.size() - 1 - tr.lead);
            if (n < 1.0)
                continue;
            allFitted = allFitted && tr.fitPeriod > 0.0;
            fitBeats += n;
            fitSeconds += n * tr.fitPeriod;
        }
        if (allFitted && fitBeats >= 2.0)
            out.bpm = 60.0 * fitBeats / fitSeconds;
        result.sections.push_back (out);
        onHitShare.push_back (numBeats >= 2 * m.beatsPerBar ? (double) std::min (numOnHit, numOnStrongHit) / numBeats : 1.0);

        for (size_t p = 0; p < sec.parts.size(); ++p)
        {
            const auto& part = sec.parts[p];
            if (part.userIndex >= 0 && p == 0 && s > 0)
                user[(size_t) part.userIndex].restart = true;   // it is the first pin of a section
            for (int a : part.track.pins)
            {
                if (a == 0 && part.userIndex >= 0)
                    continue;   // the user's own pin stands here
                Pin pin;
                pin.beat = part.beat0 + a;
                pin.time = part.track.tt[(size_t) a];
                pin.restart = a == 0 && p == 0 && s > 0;
                pins.push_back (pin);
            }
        }
    }
    if (! sections.empty() && sections.front().parts.front().track.numConf() >= tuning.minSec)
        result.bpm = result.sections.front().bpm;

    // Every user pin is kept as given. An engine pin whose beat does not lie between the beats of
    // the user pins either side of it is dropped, and so is one that does not move forward.
    std::stable_sort (pins.begin(), pins.end(), [] (const Pin& a, const Pin& b) { return a.time < b.time; });
    std::vector<Pin> merged;
    std::vector<char> behind (user.size(), 0);   // user pins whose beat is not after beats followed before them
    {
        size_t u = 0;
        for (const auto& pin : pins)
        {
            while (u < user.size() && user[u].time <= pin.time)
                ++u;
            if (u > 0 && (pin.beat <= user[u - 1].beat || pin.time - user[u - 1].time < 1.0e-6))
                continue;
            if (u < user.size() && pin.beat >= user[u].beat)
            {
                if (pin.beat >= user[u].beat + 0.5)   // not just the beat the user's pin moves
                    behind[u] = 1;
                continue;
            }
            merged.push_back (pin);
        }
        merged.insert (merged.end(), user.begin(), user.end());
        std::stable_sort (merged.begin(), merged.end(), [] (const Pin& a, const Pin& b) { return a.time < b.time; });
    }
    for (const auto& pin : merged)
    {
        if (! pin.user && ! result.pins.empty() && (pin.beat <= result.pins.back().beat || pin.time - result.pins.back().time < 1.0e-6))
            continue;
        result.pins.push_back (pin);
    }

    // Regular pins: where two neighbouring pins of a section are more than pinEveryBars apart, pins
    // are added between them on beat 1 of a bar, each on a hit that sits on the grid there. A bar
    // with no such hit is passed over for the next. The hit has to be close enough to the grid that
    // the tempo either side of the new pin changes by no more than regularTempo, and within
    // regularNear of the tolerance: a strong hit if a bar has one, else any hit counted as a beat.
    // When no bar within reach has one that close, a strong hit anywhere within the tolerance is taken.
    if (options.pinEveryBars > 0 && result.pins.size() >= 2)
    {
        const double bar = (double) m.beatsPerBar, every = (double) options.pinEveryBars * bar;
        std::vector<Pin> out;
        for (size_t i = 0; i < result.pins.size(); ++i)
        {
            const Pin p = result.pins[i];
            out.push_back (p);
            if (i + 1 == result.pins.size())
                break;
            const Pin q = result.pins[i + 1];
            if (q.restart || q.time <= p.time || q.beat <= p.beat)
                continue;
            auto timeAt = [&] (double b) { return p.time + (q.time - p.time) * (b - p.beat) / (q.beat - p.beat); };
            // The hit of at least `strength` nearest the grid at beat b, if one is within `within`.
            auto hitAt = [&] (double b, double within, double strength)
            {
                const double line = timeAt (b);
                int best = -1;
                for (int j = h.lowerBound (line - within); j < h.size() && h.on[(size_t) j] <= line + within; ++j)
                    if (h.st[(size_t) j] >= strength && (best < 0 || std::abs (h.on[(size_t) j] - line) < std::abs (h.on[(size_t) best] - line)))
                        best = j;
                return best;
            };
            double last = p.beat, lastTime = p.time;
            double passedOver = 0.0;   // bars up to here, after the last pin, have been tried and have no hit to pin
            while (q.beat - last > every + 1.0e-9)
            {
                // The bar to try first: pinEveryBars on from the last pin, or half way to the next pin
                // when that would leave less than a bar before it. Bars are tried from that one
                // onwards, then back from it towards the last pin. When none of them has a hit to
                // pin, the bars after them are tried.
                double first = std::floor ((last + every) / bar + 1.0e-9) * bar;
                std::vector<double> bars;
                if (passedOver > last)
                    first = passedOver + bar;
                else if (q.beat - first < bar)
                    first = std::round (0.5 * (last + q.beat) / bar) * bar;
                for (double b = first; q.beat - b >= bar - 1.0e-9 && b - first < every; b += bar)
                    bars.push_back (b);
                if (bars.empty())
                    break;
                const double lastTried = bars.back();
                if (passedOver <= last)
                    for (double b = first - bar; b - last >= bar - 1.0e-9; b -= bar)
                        bars.push_back (b);

                int hit = -1;
                double at = 0.0;
                for (int pass = 0; pass < 3 && hit < 0; ++pass)
                    for (double b : bars)
                    {
                        if (b - last < bar - 1.0e-9)
                            continue;
                        const double shortest = std::min (timeAt (b) - lastTime, q.time - timeAt (b));
                        const double close = std::min (tuning.regularNear * m.pinTol, tuning.regularTempo * shortest);
                        if (pass == 2 && (b - last < 2.0 * bar || q.beat - b < 2.0 * bar))
                            continue;   // a hit that is less close needs two bars either side of it
                        hit = pass == 0 ? hitAt (b, close, tuning.thrStrong) : (pass == 1 ? hitAt (b, close, tuning.thrOn) : hitAt (b, m.pinTol, tuning.thrStrong));
                        if (hit >= 0 && h.on[(size_t) hit] > lastTime + 1.0e-6 && h.on[(size_t) hit] < q.time - 1.0e-6)
                        {
                            at = b;
                            break;
                        }
                        hit = -1;
                    }
                if (hit < 0)
                {
                    passedOver = lastTried;
                    continue;
                }
                Pin pin;
                pin.beat = at;
                pin.time = h.on[(size_t) hit];
                out.push_back (pin);
                last = at;
                lastTime = pin.time;
            }
        }
        result.pins = std::move (out);
    }

    // What the user should look at.
    auto addCheck = [&] (Check::Kind checkKind, double from, double to, const std::string& note)
    {
        Check check;
        check.kind = checkKind;
        check.startTime = from;
        check.endTime = to;
        check.note = note;
        result.checks.push_back (check);
    };
    auto seconds = [] (const char* format, double value)
    {
        char text[64];
        std::snprintf (text, sizeof (text), format, value);
        return std::string (text);
    };
    // Strong hits strictly between two times: how many, and the time of the last.
    auto strongBetween = [&] (double from, double to)
    {
        std::pair<int, double> found { 0, from };
        for (int i = h.upperBound (from + snap); i < h.size() && h.on[(size_t) i] < to - snap; ++i)
            if (h.st[(size_t) i] >= tuning.thrStrong)
            {
                ++found.first;
                found.second = h.on[(size_t) i];
            }
        return found;
    };

    for (size_t s = 0; s < result.sections.size(); ++s)
    {
        const auto& sec = result.sections[s];
        if (s > 0)
        {
            const auto& prev = result.sections[s - 1];
            if (! sec.confirmed)
                addCheck (Check::Kind::restart, prev.endTime, sec.startTime, barBeatQuestion (sections[s].proposedBeat, m.beatsPerBar));

            // The stretch between the two sections: nothing there, or hits the beat was not followed through.
            const double length = sec.startTime - prev.endTime;
            const bool long2 = length > 2.0 * m.beatsPerBar * sections[s - 1].period;
            const int strong = strongBetween (prev.endTime, sec.startTime).first;
            const bool long1 = length > m.beatsPerBar * sections[s - 1].period;
            if ((long1 && strong >= tuning.minSec + 1) || (long2 && strong > 0))
                addCheck (Check::Kind::unsure, prev.endTime, sec.startTime, seconds ("Beat not followed for %.1f s", length));
            else if (long2)
                addCheck (Check::Kind::noHits, prev.endTime, sec.startTime, seconds ("No hits for %.1f s", length));
        }

        // Few of the section's beats sit on a strong hit: the tempo is likely a wrong ratio of the real one.
        if (onHitShare[s] < tuning.unsureOnHit)
            addCheck (Check::Kind::unsure, sec.startTime, sec.endTime, "Few beats land on a hit. Check the tempo");
    }

    // Hits after the last beat followed.
    if (! result.sections.empty())
    {
        const auto& last = result.sections.back();
        const auto [strong, lastStrong] = strongBetween (last.endTime, inf);
        if (strong >= tuning.minSec + 1 && lastStrong - last.endTime > 2.0 * m.beatsPerBar * sections.back().period)
            addCheck (Check::Kind::unsure, last.endTime, lastStrong, seconds ("Beat not followed for %.1f s", lastStrong - last.endTime));
    }

    for (size_t u = 0; u < user.size(); ++u)
        if (behind[u])
            addCheck (Check::Kind::unsure, user[u].time, user[u].time, "The beats before this pin are left out. Its bar is not after theirs");

    for (double time : leftOut)
        addCheck (Check::Kind::unsure, time, time, "A pin placed here is left out. It does not fit the pins placed around it");

    std::stable_sort (result.checks.begin(), result.checks.end(), [] (const Check& a, const Check& b) { return a.startTime < b.startTime; });
    return result;
}

MapResult mapCue (const std::vector<Onset>& onsets, double durationSeconds, const Hint& hint,
                  const std::vector<Pin>& userPins, const Options& options)
{
    return detail::mapCueWith (onsets, durationSeconds, hint, userPins, options, detail::Tuning());
}

//==============================================================================
// A rough tempo from the spacing of the hits: every pair of hits up to four beats-at-60 apart adds
// to a histogram of intervals; a tempo scores by the histogram at one to four of its beats. Between
// tempos an octave apart that score alike, the one nearer 120 is taken.
double estimateTempo (const std::vector<Onset>& onsets, double durationSeconds)
{
    (void) durationSeconds;
    if (onsets.size() < 8)
        return 0.0;

    const double binSeconds = 0.001;
    const double maxInterval = 4.2;
    std::vector<double> hist ((size_t) (maxInterval / binSeconds) + 2, 0.0);
    for (size_t i = 0; i < onsets.size(); ++i)
        for (size_t j = i + 1; j < onsets.size(); ++j)
        {
            const double d = onsets[j].time - onsets[i].time;
            if (d >= maxInterval)
                break;
            if (d > 0.05)
                hist[(size_t) (d / binSeconds + 0.5)] += (double) onsets[i].strength * (double) onsets[j].strength;
        }

    // Smooth with a 4 ms deviation so nearly equal intervals add up.
    std::vector<double> smooth (hist.size(), 0.0);
    const int reach = 12;
    std::vector<double> kernel ((size_t) reach * 2 + 1);
    for (int k = -reach; k <= reach; ++k)
        kernel[(size_t) (k + reach)] = std::exp (-0.5 * (k / 4.0) * (k / 4.0));
    for (int i = 0; i < (int) hist.size(); ++i)
        if (hist[(size_t) i] != 0.0)
            for (int k = -reach; k <= reach; ++k)
                if (i + k >= 0 && i + k < (int) hist.size())
                    smooth[(size_t) (i + k)] += hist[(size_t) i] * kernel[(size_t) (k + reach)];

    auto at = [&] (double seconds)
    {
        const double x = seconds / binSeconds;
        const auto i = (size_t) x;
        if (i + 1 >= smooth.size())
            return 0.0;
        return smooth[i] + (smooth[i + 1] - smooth[i]) * (x - (double) i);
    };

    double best = 0.0, bestScore = 0.0, total = 0.0;
    int count = 0;
    for (double bpm = 70.0; bpm <= 190.0; bpm += 0.05)
    {
        const double P = 60.0 / bpm;
        const double comb = at (P) + 0.75 * at (2.0 * P) + 0.5 * at (3.0 * P) + 0.75 * at (4.0 * P);
        const double octaves = std::log2 (bpm / 120.0);
        const double score = comb * std::exp (-0.5 * octaves * octaves);
        total += score;
        ++count;
        if (score > bestScore)
        {
            bestScore = score;
            best = bpm;
        }
    }
    if (bestScore <= 0.0 || bestScore < 1.5 * total / count)
        return 0.0;
    return best;
}

} // namespace tt
