#include "Measures.h"
#include "Analysis.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <numbers>
#include <numeric>

namespace dh
{

Biquad Biquad::highpass (double sr, double f0, double q)
{
    const double w0 = 2.0 * std::numbers::pi * f0 / sr, c = std::cos (w0), alpha = std::sin (w0) / (2.0 * q);
    const double a0 = 1.0 + alpha;
    Biquad b;
    b.b0 = (1.0 + c) / 2.0 / a0;
    b.b1 = -(1.0 + c) / a0;
    b.b2 = (1.0 + c) / 2.0 / a0;
    b.a1 = -2.0 * c / a0;
    b.a2 = (1.0 - alpha) / a0;
    return b;
}

Biquad Biquad::lowpass (double sr, double f0, double q)
{
    const double w0 = 2.0 * std::numbers::pi * f0 / sr, c = std::cos (w0), alpha = std::sin (w0) / (2.0 * q);
    const double a0 = 1.0 + alpha;
    Biquad b;
    b.b0 = (1.0 - c) / 2.0 / a0;
    b.b1 = (1.0 - c) / a0;
    b.b2 = (1.0 - c) / 2.0 / a0;
    b.a1 = -2.0 * c / a0;
    b.a2 = (1.0 - alpha) / a0;
    return b;
}

std::vector<double> highpass4 (const std::vector<float>& x, double sr, double hz)
{
    Biquad a = Biquad::highpass (sr, hz, 0.54119610), b = Biquad::highpass (sr, hz, 1.30656296);
    std::vector<double> y (x.size());
    for (std::size_t i = 0; i < x.size(); ++i)
        y[i] = b.tick (a.tick (x[i]));
    return y;
}

std::vector<double> bandpass8 (const std::vector<float>& x, double sr, double loHz, double hiHz)
{
    Biquad h1 = Biquad::highpass (sr, loHz, 0.54119610), h2 = Biquad::highpass (sr, loHz, 1.30656296);
    Biquad l1 = Biquad::lowpass (sr, hiHz, 0.54119610), l2 = Biquad::lowpass (sr, hiHz, 1.30656296);
    std::vector<double> y (x.size());
    for (std::size_t i = 0; i < x.size(); ++i)
        y[i] = l2.tick (l1.tick (h2.tick (h1.tick (x[i]))));
    return y;
}

std::vector<std::vector<double>> bandpass8 (const Audio& a, double loHz, double hiHz)
{
    std::vector<std::vector<double>> out;
    for (const auto& c : a.ch)
        out.push_back (bandpass8 (c, a.sampleRate, loHz, hiHz));
    return out;
}

//==============================================================================

const char* pitchClassName (int pc)
{
    static const char* const names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return names[((pc % 12) + 12) % 12];
}

std::string Note::name() const { return std::string (pitchClassName (pitchClass)) + std::to_string (octave); }

Note noteOf (double hz)
{
    Note n;
    if (! (hz > 0.0))
        return n;
    const double midi = 69.0 + 12.0 * std::log2 (hz / 440.0);
    const long nearest = std::lround (midi);
    n.cents = 100.0 * (midi - (double) nearest);
    n.pitchClass = (int) (((nearest % 12) + 12) % 12);
    n.octave = (int) std::floor ((double) nearest / 12.0) - 1;
    return n;
}

std::array<int, 12> pitchClassRanks (const std::array<float, 12>& profile)
{
    std::array<int, 12> order {};
    std::iota (order.begin(), order.end(), 0);
    std::stable_sort (order.begin(), order.end(), [&] (int a, int b) { return profile[(std::size_t) a] > profile[(std::size_t) b]; });
    std::array<int, 12> rank {};
    for (int i = 0; i < 12; ++i)
        rank[(std::size_t) order[(std::size_t) i]] = i + 1;
    return rank;
}

std::vector<NotchEvent> notchEvents (const RenderResult& r, const NotchRule& rule)
{
    std::vector<NotchEvent> done;
    const int nb = std::clamp (r.numBands, 0, dehonk::kMaxBands);
    std::vector<char> isOpen ((std::size_t) nb, 0);
    std::vector<double> belowSince ((std::size_t) nb, -1.0);   // when the cut went under closeDb, -1 while above
    std::vector<NotchEvent> open ((std::size_t) nb);
    for (const auto& f : r.frames)
        for (int k = 0; k < nb; ++k)
        {
            const auto ki = (std::size_t) k;
            const double c = f.cutDb[ki];
            auto& e = open[ki];
            if (! isOpen[ki])
            {
                if (c > rule.openDb)
                {
                    isOpen[ki] = 1;
                    belowSince[ki] = -1.0;
                    e = {};
                    e.band = k;
                    e.hz = r.bandHz[ki];
                    e.openSec = f.timeSec;
                    e.deepestDb = c;
                    e.profileAtOpen = f.pitchClass;
                    e.pcRankAtOpen = f.timeSec >= rule.rankFromSec ? pitchClassRanks (f.pitchClass)[(std::size_t) noteOf (e.hz).pitchClass] : 0;
                }
                continue;
            }
            e.deepestDb = std::max (e.deepestDb, c);
            if (c > rule.closeDb)
                belowSince[ki] = -1.0;
            else
            {
                if (belowSince[ki] < 0.0)
                    belowSince[ki] = f.timeSec;
                if (f.timeSec - belowSince[ki] >= rule.holdSec)
                {
                    e.closeSec = belowSince[ki];
                    done.push_back (e);
                    isOpen[ki] = 0;
                }
            }
        }
    const double end = r.frames.empty() ? 0.0 : r.frames.back().timeSec;
    for (int k = 0; k < nb; ++k)
        if (isOpen[(std::size_t) k])
        {
            open[(std::size_t) k].closeSec = belowSince[(std::size_t) k] >= 0.0 ? belowSince[(std::size_t) k] : end;
            done.push_back (open[(std::size_t) k]);
        }
    std::stable_sort (done.begin(), done.end(), [] (const NotchEvent& a, const NotchEvent& b) { return a.openSec < b.openSec; });
    return done;
}

//==============================================================================

BandPair bandPair (const Audio& in, const Audio& out, double loHz, double hiHz)
{
    BandPair p;
    p.sampleRate = in.sampleRate;
    p.in = bandpass8 (in, loHz, hiHz);
    p.out = bandpass8 (out, loHz, hiHz);
    return p;
}

namespace
{
/** Mean square over channels at sample i. */
double sq (const std::vector<std::vector<double>>& x, std::size_t i)
{
    double s = 0.0;
    for (const auto& c : x)
        s += c[i] * c[i];
    return s / (double) std::max<std::size_t> (1, x.size());
}

/** Envelope: RMS over winSec starting at every millisecond from a to b, dB. */
std::vector<double> envelopeDb (const std::vector<std::vector<double>>& x, double sr, std::size_t a, std::size_t b, double winSec)
{
    const std::size_t n = x.empty() ? 0 : x[0].size();
    const auto win = (std::size_t) std::llround (winSec * sr), hop = (std::size_t) std::llround (0.001 * sr);
    std::vector<double> e;
    for (std::size_t s = a; s + win <= std::min (b + win, n); s += hop)
    {
        double acc = 0.0;
        for (std::size_t i = s; i < s + win; ++i)
            acc += sq (x, i);
        e.push_back (powerToDb (acc / (double) win));
    }
    return e;
}

double peakBand (const std::vector<std::vector<double>>& x, std::size_t a, std::size_t b)
{
    double m = 0.0;
    for (const auto& c : x)
        for (std::size_t i = a; i < std::min (b, c.size()); ++i)
            m = std::max (m, std::abs (c[i]));
    return m;
}
} // namespace

double frontChangeDb (const BandPair& bp, std::size_t onset, double frontSec)
{
    const std::size_t end = onset + (std::size_t) std::llround (frontSec * bp.sampleRate);
    return ampToDb (peakBand (bp.out, onset, end) / std::max (1e-30, peakBand (bp.in, onset, end)));
}

ImpactFigures impactFigures (const BandPair& bp, std::size_t onset, std::size_t nextOnset, const ImpactSpec& spec)
{
    ImpactFigures f;
    const double sr = bp.sampleRate;
    const std::size_t n = bp.in.empty() ? 0 : bp.in[0].size();
    auto at = [sr] (double sec) { return (std::size_t) std::llround (sec * sr); };

    // Body.
    {
        const std::size_t end = std::min (n, onset + at (spec.bodySec));
        double ei = 0.0, eo = 0.0;
        for (std::size_t i = onset; i < end; ++i)
        {
            ei += sq (bp.in, i);
            eo += sq (bp.out, i);
        }
        f.bodyDb = powerToDb (eo / std::max (1e-300, ei));
    }
    f.frontDb = frontChangeDb (bp, onset, spec.frontSec);

    // Tail (see ImpactFigures).
    std::size_t end = std::min (n, onset + at (spec.tailMaxSec));
    if (nextOnset > onset)
        end = std::min (end, nextOnset);
    const std::size_t hop = std::max<std::size_t> (1, at (0.001));
    if (end <= onset + 20 * hop)
        return f;
    const std::size_t pre = onset > at (0.4) ? onset - at (0.4) : 0, preEnd = onset > at (0.03) ? onset - at (0.03) : onset;
    auto floorPower = [&] (const std::vector<std::vector<double>>& x) {
        double acc = 0.0;
        for (std::size_t i = pre; i < preEnd; ++i)
            acc += sq (x, i);
        return preEnd > pre ? acc / (double) (preEnd - pre) : 0.0;
    };
    const double floorIn = floorPower (bp.in), floorOut = floorPower (bp.out);
    f.floorInDb = powerToDb (floorIn);
    const double envSec = 0.001 * spec.envelopeMs;
    const auto ei = envelopeDb (bp.in, sr, onset, end, envSec), eo = envelopeDb (bp.out, sr, onset, end, envSec);
    if (ei.empty() || eo.empty())
        return f;
    f.peakInDb = *std::max_element (ei.begin(), ei.begin() + (std::ptrdiff_t) std::min<std::size_t> (ei.size(), 300));

    // The curve is followed only while the band envelope is floorMarginDb above the bed: the last
    // millisecond (counting from the onset) at which it still is.
    auto usableEnd = [&] (const std::vector<double>& e, double floorPow) {
        const double line = powerToDb (floorPow) + spec.floorMarginDb;
        std::size_t last = 0;
        for (std::size_t j = 0; j < e.size(); ++j)
            if (e[j] > line)
                last = j;
        return last;
    };
    // Backward-integrated energy above the bed, 1 ms bins, in dB against the total.
    const std::size_t bins = (end - onset) / hop;
    auto schroeder = [&] (const std::vector<std::vector<double>>& x, double floorPow) {
        std::vector<double> e (bins + 1, 0.0);
        for (std::size_t b = bins; b-- > 0;)
        {
            double acc = 0.0;
            for (std::size_t i = onset + b * hop; i < onset + (b + 1) * hop; ++i)
                acc += sq (x, i) - floorPow;
            e[b] = e[b + 1] + acc;
        }
        std::vector<double> db (bins, -400.0);
        for (std::size_t b = 0; b < bins; ++b)
            db[b] = e[0] > 0.0 && e[b] > 0.0 ? powerToDb (e[b] / e[0]) : -400.0;
        return db;
    };
    const auto ci = schroeder (bp.in, floorIn), co = schroeder (bp.out, floorOut);
    if (ci[0] != 0.0 || co[0] != 0.0)
        return f;   // no energy above the bed over the window (the bed got louder): not measurable
    const std::size_t ui = std::min (bins - 1, usableEnd (ei, floorIn)), uo = std::min (bins - 1, usableEnd (eo, floorOut));
    // The lowest level both curves reach while still usable, no further than fitStart + fall.
    const double lower = std::max ({ -(spec.fitStartDb + spec.tailFallDb), ci[ui], co[uo] });
    f.fallDb = std::max (0.0, -spec.fitStartDb - lower);
    if (f.fallDb < spec.minFallDb)
        return f;   // tailInMs and tailOutMs stay -1: not measurable
    // Least-squares line through the curve between -fitStartDb and lower; ms to fall tailFallDb.
    auto fit = [&] (const std::vector<double>& c, std::size_t last) {
        double s0 = 0, s1 = 0, s2 = 0, sy = 0, sxy = 0;
        for (std::size_t b = 0; b <= last; ++b)
            if (c[b] <= -spec.fitStartDb && c[b] >= lower)
            {
                const double x = (double) b;
                s0 += 1.0;
                s1 += x;
                s2 += x * x;
                sy += c[b];
                sxy += x * c[b];
            }
        const double det = s0 * s2 - s1 * s1;
        if (s0 < 5.0 || det <= 0.0)
            return -1.0;
        const double slope = (s0 * sxy - s1 * sy) / det;   // dB per ms
        return slope < 0.0 ? spec.tailFallDb / -slope : -1.0;
    };
    f.tailInMs = fit (ci, ui);
    f.tailOutMs = fit (co, uo);
    f.tailMeasurable = f.tailInMs > 0.0 && f.tailOutMs > 0.0;
    if (f.tailMeasurable)
        f.tailChangePct = 100.0 * (f.tailOutMs - f.tailInMs) / f.tailInMs;
    else
        f.tailInMs = f.tailOutMs = -1.0;
    return f;
}

//==============================================================================

LimiterResult referenceLimiter (const Audio& in, double ceilingDb, double lookaheadMs, double releaseMs)
{
    LimiterResult r;
    r.out = in;
    const std::size_t n = in.length();
    r.grDb.assign (n, 0.0f);
    if (n == 0)
        return r;
    const double ceiling = dbToAmp (ceilingDb);
    std::vector<double> req (n);   // gain in dB each sample needs on its own, <= 0
    for (std::size_t i = 0; i < n; ++i)
    {
        double p = 0.0;
        for (const auto& c : in.ch)
            p = std::max (p, (double) std::abs (c[i]));
        req[i] = p > ceiling ? ampToDb (ceiling / p) : 0.0;
    }
    const auto L = (std::size_t) std::llround (lookaheadMs * 0.001 * in.sampleRate);
    const double rel = 1.0 - std::exp (-1.0 / (releaseMs * 0.001 * in.sampleRate));
    // Sliding minimum of req over [i, i + L] with a monotonic deque.
    std::deque<std::size_t> dq;
    std::size_t next = 0;
    double g = 0.0, sum = 0.0, sumLim = 0.0;
    std::size_t limiting = 0;
    for (std::size_t i = 0; i < n; ++i)
    {
        while (next < n && next <= i + L)
        {
            while (! dq.empty() && req[dq.back()] >= req[next])
                dq.pop_back();
            dq.push_back (next++);
        }
        while (dq.front() < i)
            dq.pop_front();
        const double target = req[dq.front()];
        g = target < g ? target : g + rel * (target - g);
        g = std::min (g, req[i]);   // never above the ceiling, whatever the release has reached
        const double gain = dbToAmp (g);
        for (auto& c : r.out.ch)
            c[i] = (float) ((double) c[i] * gain);
        r.grDb[i] = (float) -g;
        sum += -g;
        if (-g > 0.01)   // the release never quite arrives; count reduction above 0.01 dB
        {
            sumLim += -g;
            ++limiting;
        }
        r.maxGrDb = std::max (r.maxGrDb, -g);
    }
    r.meanGrDb = sum / (double) n;
    r.meanGrWhileLimitingDb = limiting > 0 ? sumLim / (double) limiting : 0.0;
    r.pctLimiting = 100.0 * (double) limiting / (double) n;
    return r;
}

std::vector<double> shortTermDb (const Audio& a, double windowSec, double hopSec)
{
    std::vector<double> v;
    const auto win = (std::size_t) std::llround (windowSec * a.sampleRate), hop = (std::size_t) std::llround (hopSec * a.sampleRate);
    for (std::size_t s = 0; s + win <= a.length(); s += hop)
        v.push_back (powerToDb (meanSquare (a, s, s + win)));
    return v;
}

} // namespace dh
