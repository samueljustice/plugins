#include "OfflinePitchTracker.h"

#include <algorithm>
#include <array>
#include <functional>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <utility>

namespace subbertone::offline
{
    namespace
    {
        constexpr double c_pi = std::numbers::pi;

        constexpr double c_rateHeadroom = 1.05;          // buffers up to 5 % faster than workingRate are analysed as they are
        constexpr float  c_maxInput = 1.0e4f;            // |x| at or above this (+80 dBFS), NaN and inf are silence, as in the engine
        constexpr double c_levelSeconds = 0.020;         // silence check window: the engine's level gate time constant
        constexpr double c_digitalSilenceRms = 1.0e-8;   // -160 dBFS: silent whatever the settings say
        constexpr double c_straightBridgeSeconds = 0.100; // gaps up to this long are bridged with a straight line ...
        constexpr double c_bridgeSlopeSeconds = 0.050;   // ... longer ones carry on the slopes measured over this much either side
        constexpr double c_bridgeOvershoot = 1.0 / 12.0; // ... and never go more than a semitone (in octaves) past either end
        constexpr double c_jumpOctaves = 1.0 / 12.0;     // Summary::jumps counts steps larger than a semitone
        constexpr int    c_progressFrames = 64;          // frames between progress reports
        constexpr int    c_cancelSamples = 4096;         // output samples between cancel checks while decimating

        bool isCancelled(const std::atomic<bool>* cancel) noexcept
        {
            return cancel != nullptr && cancel->load(std::memory_order_relaxed);
        }

        float cleanSample(float x) noexcept
        {
            return std::abs(x) < c_maxInput ? x : 0.0f; // NaN fails the comparison too
        }

        double gainFromDb(double db) noexcept
        {
            return std::pow(10.0, db / 20.0);
        }

        // One stage's own 0..1 progress, mapped onto its share of the whole analysis.
        struct Stage
        {
            const Progress* report = nullptr;
            float from = 0.0f;
            float to = 1.0f;

            void operator()(float p) const
            {
                if (report != nullptr && *report)
                    (*report)(from + (to - from) * std::clamp(p, 0.0f, 1.0f));
            }
        };

        // Out-of-range settings would make the frame and lag arrays absurdly large
        // (or the arithmetic undefined), so they are pulled back to something usable.
        Settings sanitised(Settings s)
        {
            const Settings defaults;
            auto finiteOr = [](double v, double fallback) { return std::isfinite(v) ? v : fallback; };

            s.hopSeconds = std::clamp(s.hopSeconds, 0.001, 0.1);
            s.workingRate = std::clamp(finiteOr(s.workingRate, defaults.workingRate), 2000.0, 96000.0);
            s.lowestHz = std::clamp(finiteOr(s.lowestHz, defaults.lowestHz), 10.0, 4000.0);
            s.highestHz = std::clamp(finiteOr(s.highestHz, defaults.highestHz), s.lowestHz * 1.5, 8000.0);
            s.silenceDbfs = std::isnan(s.silenceDbfs) ? defaults.silenceDbfs : std::min(s.silenceDbfs, 0.0);
            s.gateDb = std::isnan(s.gateDb) ? defaults.gateDb : std::min(s.gateDb, 0.0f);
            s.parallelLines = std::clamp(s.parallelLines, 0, 16);
            s.bridgeSeconds = std::clamp(finiteOr(s.bridgeSeconds, 0.0), 0.0, 10.0);
            s.maxSemitonesPerSecond = std::max(0.0, finiteOr(s.maxSemitonesPerSecond, 0.0));
            return s;
        }

        //==============================================================================
        // Shared signal helpers
        //==============================================================================

        // The samples both methods read: the caller's buffer when it can be used as it
        // is, otherwise a cleaned or decimated copy (`copy` then owns `data`).
        struct Signal
        {
            const float* data = nullptr;
            int length = 0;
            double rate = 0.0;
            std::vector<float> copy;
        };

        // In-place complex FFT on separate re / im arrays, length 2^k. The twiddle
        // factor of each butterfly group is updated by a complex multiply per step.
        void fftRadix2(double* re, double* im, std::size_t n) noexcept
        {
            for (std::size_t i = 1, j = 0; i < n; ++i)
            {
                std::size_t bit = n >> 1;

                for (; (j & bit) != 0; bit >>= 1)
                    j ^= bit;

                j ^= bit;

                if (i < j)
                {
                    std::swap(re[i], re[j]);
                    std::swap(im[i], im[j]);
                }
            }

            for (std::size_t len = 2; len <= n; len <<= 1)
            {
                const double angle = -2.0 * c_pi / static_cast<double>(len);
                const double stepRe = std::cos(angle);
                const double stepIm = std::sin(angle);
                const std::size_t half = len / 2;

                for (std::size_t block = 0; block < n; block += len)
                {
                    double wRe = 1.0;
                    double wIm = 0.0;

                    for (std::size_t k = 0; k < half; ++k)
                    {
                        const std::size_t a = block + k;
                        const std::size_t b = a + half;
                        const double tRe = re[b] * wRe - im[b] * wIm;
                        const double tIm = re[b] * wIm + im[b] * wRe;
                        re[b] = re[a] - tRe;
                        im[b] = im[a] - tIm;
                        re[a] += tRe;
                        im[a] += tIm;

                        const double nextRe = wRe * stepRe - wIm * stepIm;
                        wIm = wRe * stepIm + wIm * stepRe;
                        wRe = nextRe;
                    }
                }
            }
        }

        // Integer-factor downsampler. The anti-alias filter is a sinc with a Blackman
        // window, 32 * factor + 1 taps long, normalised to 0 dB at DC, with its corner
        // at 45 % of the output rate; it is evaluated only at the retained samples.
        bool decimate(const float* x, int n, int factor, std::vector<float>& y,
                      const std::atomic<bool>* cancel, const Stage& stage)
        {
            const int half = 16 * factor;
            const int taps = 2 * half + 1;
            const double cutoff = 0.45 / factor; // as a fraction of the input rate
            std::vector<double> kernel(static_cast<std::size_t>(taps));
            double gain = 0.0;

            for (int i = 0; i < taps; ++i)
            {
                const int k = i - half;
                const double sinc = (k == 0) ? 2.0 * cutoff : std::sin(2.0 * c_pi * cutoff * k) / (c_pi * k);
                const double blackman = 0.42 - 0.5 * std::cos(2.0 * c_pi * i / (taps - 1))
                                      + 0.08 * std::cos(4.0 * c_pi * i / (taps - 1));
                kernel[static_cast<std::size_t>(i)] = sinc * blackman;
                gain += kernel[static_cast<std::size_t>(i)];
            }

            for (auto& tap : kernel)
                tap /= gain;

            const int outLength = n / factor;
            y.assign(static_cast<std::size_t>(outLength), 0.0f);

            for (int o = 0; o < outLength; ++o)
            {
                if (o % c_cancelSamples == 0)
                {
                    if (isCancelled(cancel))
                        return false;

                    stage(static_cast<float>(o) / static_cast<float>(outLength));
                }

                const int centre = o * factor;
                const int first = std::max(0, centre - half);
                const int last = std::min(n - 1, centre + half);
                double acc = 0.0;

                for (int j = first; j <= last; ++j)
                    acc += kernel[static_cast<std::size_t>(j - centre + half)] * cleanSample(x[j]);

                y[static_cast<std::size_t>(o)] = static_cast<float>(acc);
            }

            return true;
        }

        // Rounding the factor up keeps Engine's 2^k-sample window near 0.34 s at every
        // host rate. Rounded down, 44.1 kHz would be analysed at 14.7 kHz with a 0.56 s
        // window, which smears a fast run-up (59.9 % instead of 92.7 % of hops within
        // 50 cents of an engine run-up's order-4 line). The headroom lets Learn's captures,
        // already decimated to 10.7-12.6 kHz, through as they are: no second filter, and
        // no copy.
        bool prepare(const float* mono, int numSamples, double sampleRate, double workingRate, Signal& signal,
                     const std::atomic<bool>* cancel, const Stage& stage)
        {
            const double ratio = sampleRate / (workingRate * c_rateHeadroom);
            const int factor = static_cast<int>(std::clamp(std::ceil(ratio - 1e-9), 1.0, 65536.0));
            signal.rate = sampleRate / factor;

            if (factor > 1)
            {
                if (!decimate(mono, numSamples, factor, signal.copy, cancel, stage))
                    return false;
            }
            else if (std::all_of(mono, mono + numSamples, [](float v) { return std::abs(v) < c_maxInput; }))
            {
                signal.data = mono;
                signal.length = numSamples;
                return true;
            }
            else
            {
                signal.copy.resize(static_cast<std::size_t>(numSamples));
                std::transform(mono, mono + numSamples, signal.copy.begin(), cleanSample);
            }

            signal.data = signal.copy.data();
            signal.length = static_cast<int>(signal.copy.size());
            return true;
        }

        // One flag per frame: 1 where the level around the frame centre is below the
        // threshold. Both methods and the gap filling read the same flags, so they agree
        // on where the notes stop. The level is the RMS about the window's own mean: a DC
        // offset is not sound (the engine's level gate never sees it either).
        bool findSilence(const Signal& x, int numFrames, double hopSeconds, double thresholdRms,
                         const std::atomic<bool>* cancel, std::vector<std::uint8_t>& silent)
        {
            const int window = std::max(1, static_cast<int>(std::lround(c_levelSeconds * x.rate)));
            const double threshold = std::max(thresholdRms, c_digitalSilenceRms);
            silent.assign(static_cast<std::size_t>(numFrames), 0);

            for (int f = 0; f < numFrames; ++f)
            {
                if (f % c_progressFrames == 0 && isCancelled(cancel))
                    return false;

                const int start = static_cast<int>(std::lround(f * hopSeconds * x.rate)) - window / 2;
                const int first = std::max(0, start);
                const int last = std::min(x.length, start + window);
                double sum = 0.0;
                double squares = 0.0;

                for (int i = first; i < last; ++i)
                {
                    const double v = x.data[i];
                    sum += v;
                    squares += v * v;
                }

                const double mean = sum / window;
                const double level = std::sqrt(std::max(0.0, squares / window - mean * mean));
                silent[static_cast<std::size_t>(f)] = level < threshold ? 1 : 0;
            }

            return true;
        }

        //==============================================================================
        // Tonal: YIN dips + Viterbi path
        //==============================================================================

        struct Dip
        {
            double hz;
            double cost;  // this frame's own Viterbi charge (smaller wins)
            float depth;  // normalised difference at the dip: 0 = perfectly periodic
        };

        constexpr int c_dipsKept = 12;
        constexpr int c_tonalStates = c_dipsKept + 1; // the dips plus "unpitched"

        bool runTonal(const Signal& x, const std::vector<std::uint8_t>& silent, const Settings& s, bool voting,
                      const std::atomic<bool>* cancel, const Stage& stage, std::vector<Frame>& frames)
        {
            const int n = x.length;
            const double rate = x.rate;
            const int numFrames = static_cast<int>(frames.size());

            if (isCancelled(cancel))
                return false;

            const int lagMin = std::max(2, static_cast<int>(std::floor(rate / s.highestHz)));
            const int lagMax = std::max(lagMin + 2, static_cast<int>(std::ceil(rate / s.lowestHz)));
            const int window = lagMax;             // samples summed for each lag
            const int reach = window + lagMax + 1; // samples one hop looks at

            // Samples outside the buffer are silence, for the energies and the lag products alike.
            auto sampleAt = [&](int i) { return (i >= 0 && i < n) ? x.data[i] : 0.0f; };

            std::vector<Dip> dips(static_cast<std::size_t>(numFrames) * c_dipsKept);
            std::vector<std::uint8_t> dipCount(static_cast<std::size_t>(numFrames), 0);
            std::vector<double> diff(static_cast<std::size_t>(lagMax) + 2);
            std::vector<double> norm(static_cast<std::size_t>(lagMax) + 2);
            std::vector<Dip> found;
            std::vector<double> votes;
            found.reserve(static_cast<std::size_t>(lagMax));

            for (int f = 0; f < numFrames; ++f)
            {
                if (isCancelled(cancel))
                    return false;

                if (f % c_progressFrames == 0)
                    stage(0.9f * static_cast<float>(f) / static_cast<float>(numFrames));

                if (silent[static_cast<std::size_t>(f)] != 0)
                    continue;

                const int centre = static_cast<int>(std::lround(f * s.hopSeconds * rate));
                const int start = centre - reach / 2;
                const bool inside = start >= 0 && start + window + lagMax <= n;

                // Difference function: d(lag) = e(start) + e(start + lag) - 2 * sum x[i] x[i + lag],
                // where e is the energy of `window` samples. The shifted window's energy is kept
                // up to date one sample at a time as the lag grows.
                double e0 = 0.0;

                for (int j = 0; j < window; ++j)
                {
                    const double v = sampleAt(start + j);
                    e0 += v * v;
                }

                double shifted = e0;
                diff[0] = 0.0;

                for (int lag = 1; lag <= lagMax; ++lag)
                {
                    const double leaving = sampleAt(start + lag - 1);
                    const double entering = sampleAt(start + lag - 1 + window);
                    shifted += entering * entering - leaving * leaving;
                    double r = 0.0;

                    if (inside)
                    {
                        const float* p = x.data + start;
                        const float* q = p + lag;

                        for (int j = 0; j < window; ++j)
                            r += static_cast<double>(p[j]) * q[j];
                    }
                    else
                    {
                        for (int j = 0; j < window; ++j)
                            r += static_cast<double>(sampleAt(start + j)) * sampleAt(start + j + lag);
                    }

                    diff[static_cast<std::size_t>(lag)] = std::max(0.0, e0 + shifted - 2.0 * r);
                }

                // Normalise each lag by the mean difference up to it, so lag 0 cannot win.
                norm[0] = 1.0;
                double running = 0.0;

                for (int lag = 1; lag <= lagMax; ++lag)
                {
                    running += diff[static_cast<std::size_t>(lag)];
                    norm[static_cast<std::size_t>(lag)] = running > 0.0 ? diff[static_cast<std::size_t>(lag)] * lag / running : 1.0;
                }

                // Every local minimum under 0.95 is a candidate period, refined by a parabola
                // through its neighbours. Longer periods pay a little, so a dip at twice the
                // period (an octave too low) does not win a tie.
                found.clear();

                for (int lag = lagMin; lag < lagMax; ++lag)
                {
                    const double v = norm[static_cast<std::size_t>(lag)];

                    if (v < norm[static_cast<std::size_t>(lag) - 1] && v <= norm[static_cast<std::size_t>(lag) + 1] && v < 0.95)
                    {
                        const double left = norm[static_cast<std::size_t>(lag) - 1];
                        const double right = norm[static_cast<std::size_t>(lag) + 1];
                        const double curvature = left - 2.0 * v + right;
                        const double offset = std::abs(curvature) > 1e-12 ? 0.5 * (left - right) / curvature : 0.0;
                        const double period = lag + std::clamp(offset, -0.5, 0.5);
                        const double penalty = s.longPeriodPenalty * std::log2(period / lagMin);
                        found.push_back({ rate / period, v + penalty, static_cast<float>(v) });
                    }
                }

                if (voting && !found.empty())
                {
                    // Voting instead of depth: thresholds th = 0.01 .. 0.99, each weighted
                    // th * (1 - th)^17. Every threshold votes for the shortest-period dip that
                    // goes under it, or, if none does, gives 1 % of its weight to the deepest
                    // dip. A candidate's cost becomes -log10 of its vote fraction. (`found` is
                    // still sorted by period here.)
                    votes.assign(found.size(), 0.0);
                    double total = 0.0;

                    for (int i = 1; i < 100; ++i)
                    {
                        const double threshold = i / 100.0;
                        const double weight = threshold * std::pow(1.0 - threshold, 17.0);
                        total += weight;
                        int winner = -1;

                        for (std::size_t k = 0; k < found.size(); ++k)
                        {
                            if (found[k].depth < threshold)
                            {
                                winner = static_cast<int>(k);
                                break;
                            }
                        }

                        if (winner < 0)
                        {
                            std::size_t deepest = 0;

                            for (std::size_t k = 1; k < found.size(); ++k)
                                if (found[k].depth < found[deepest].depth)
                                    deepest = k;

                            votes[deepest] += 0.01 * weight;
                        }
                        else
                        {
                            votes[static_cast<std::size_t>(winner)] += weight;
                        }
                    }

                    for (std::size_t k = 0; k < found.size(); ++k)
                    {
                        const double share = votes[k] / std::max(1e-12, total);
                        const double penalty = s.longPeriodPenalty * std::log2((rate / found[k].hz) / lagMin);
                        found[k].cost = 0.3 * -std::log10(share + 1e-3) + penalty; // full share = 0, none = 0.9
                    }
                }

                // Cheapest first; equal costs keep the shorter period first, so the order (and the
                // result) never depends on how the standard library sorts ties.
                std::sort(found.begin(), found.end(), [](const Dip& a, const Dip& b)
                {
                    if (a.cost < b.cost)
                        return true;

                    if (b.cost < a.cost)
                        return false;

                    return a.hz > b.hz;
                });

                const int kept = std::min(c_dipsKept, static_cast<int>(found.size()));
                std::copy(found.begin(), found.begin() + kept, dips.begin() + static_cast<std::ptrdiff_t>(f) * c_dipsKept);
                dipCount[static_cast<std::size_t>(f)] = static_cast<std::uint8_t>(kept);
            }

            // Viterbi: state k < count is dip k of that hop, state count is "unpitched".
            // Moving between pitched states costs moveCost per octave; switching between
            // pitched and unpitched costs voicingChangeCost; staying unpitched is free.
            const double infinity = std::numeric_limits<double>::infinity();
            const double stepLimit = s.maxSemitonesPerSecond > 0.0 ? s.maxSemitonesPerSecond / 12.0 * s.hopSeconds + 0.01 : 0.0;
            std::vector<std::int8_t> cameFrom(static_cast<std::size_t>(numFrames) * c_tonalStates, -1);
            std::array<double, c_tonalStates> previous {};
            std::array<double, c_tonalStates> current {};
            int previousStates = 0;

            for (int f = 0; f < numFrames; ++f)
            {
                if (f % c_progressFrames == 0 && isCancelled(cancel))
                    return false;

                const Dip* here = &dips[static_cast<std::size_t>(f) * c_dipsKept];
                const int count = dipCount[static_cast<std::size_t>(f)];
                const int states = count + 1;

                for (int k = 0; k < states; ++k)
                {
                    const bool pitched = k < count;
                    const double local = pitched ? here[k].cost : (silent[static_cast<std::size_t>(f)] != 0 ? 0.0 : s.unpitchedCost);

                    if (f == 0)
                    {
                        current[static_cast<std::size_t>(k)] = local;
                        continue;
                    }

                    const Dip* before = &dips[static_cast<std::size_t>(f - 1) * c_dipsKept];
                    const int beforeCount = dipCount[static_cast<std::size_t>(f) - 1];
                    double best = infinity;
                    int from = -1;

                    for (int p = 0; p < previousStates; ++p)
                    {
                        const bool wasPitched = p < beforeCount;
                        double step = 0.0;

                        if (pitched && wasPitched)
                        {
                            const double octaves = std::abs(std::log2(here[k].hz / before[p].hz));
                            step = s.moveCost * octaves;

                            if (stepLimit > 0.0 && octaves > stepLimit)
                                step += 100.0 * (octaves - stepLimit); // faster than the slew limit: all but ruled out
                        }
                        else if (pitched != wasPitched)
                        {
                            step = s.voicingChangeCost;
                        }

                        const double total = previous[static_cast<std::size_t>(p)] + step;

                        if (total < best)
                        {
                            best = total;
                            from = p;
                        }
                    }

                    current[static_cast<std::size_t>(k)] = best + local;
                    cameFrom[static_cast<std::size_t>(f) * c_tonalStates + static_cast<std::size_t>(k)] = static_cast<std::int8_t>(from);
                }

                previous = current;
                previousStates = states;
            }

            // Walk back from the cheapest final state.
            std::vector<int> path(static_cast<std::size_t>(numFrames), 0);
            path.back() = static_cast<int>(std::min_element(previous.begin(), previous.begin() + previousStates) - previous.begin());

            for (int f = numFrames - 1; f > 0; --f)
                path[static_cast<std::size_t>(f) - 1] = cameFrom[static_cast<std::size_t>(f) * c_tonalStates + static_cast<std::size_t>(path[static_cast<std::size_t>(f)])];

            for (int f = 0; f < numFrames; ++f)
            {
                const int k = path[static_cast<std::size_t>(f)];
                const Dip* here = &dips[static_cast<std::size_t>(f) * c_dipsKept];

                if (k >= 0 && k < dipCount[static_cast<std::size_t>(f)])
                {
                    frames[static_cast<std::size_t>(f)].hz = static_cast<float>(here[k].hz);
                    frames[static_cast<std::size_t>(f)].confidence = std::clamp(1.0f - here[k].depth, 0.0f, 1.0f);
                }
            }

            // Median of the pitched hops among the five around each pitched hop: removes one-hop glitches.
            std::vector<float> smoothed(static_cast<std::size_t>(numFrames));

            for (int f = 0; f < numFrames; ++f)
            {
                smoothed[static_cast<std::size_t>(f)] = frames[static_cast<std::size_t>(f)].hz;

                if (smoothed[static_cast<std::size_t>(f)] <= 0.0f)
                    continue;

                float near[5];
                int m = 0;

                for (int o = -2; o <= 2; ++o)
                {
                    const int g = f + o;

                    if (g >= 0 && g < numFrames && frames[static_cast<std::size_t>(g)].hz > 0.0f)
                        near[m++] = frames[static_cast<std::size_t>(g)].hz;
                }

                std::nth_element(near, near + m / 2, near + m);
                smoothed[static_cast<std::size_t>(f)] = near[m / 2];
            }

            for (int f = 0; f < numFrames; ++f)
                frames[static_cast<std::size_t>(f)].hz = smoothed[static_cast<std::size_t>(f)];

            return true;
        }

        //==============================================================================
        // Engine: frame-to-frame shift -> contour -> vertical placement
        //==============================================================================

        constexpr int c_binsPerOctave = 192;    // log-frequency grid, 6.25 cents per bin
        constexpr float c_compactScale = 32.0f; // 8-bit grid: steps of 1/32 (log units), up to 7.97

        // Whitened spectrogram on the log-frequency grid, one row per hop. Either plain
        // floats or 8-bit fixed point: a quarter of the memory (15 MB per minute instead
        // of 61 MB at the default range). Whitened levels measured under 3 on an engine
        // run-up and under 6 on clean synthetic tones, so the 8-bit range does not clip.
        class LineGrid
        {
        public:
            LineGrid(int numFrames, int numBins, bool compact)
                : m_bins(numBins), m_compact(compact)
            {
                const auto cells = static_cast<std::size_t>(numFrames) * static_cast<std::size_t>(numBins);

                if (compact)
                    m_small.assign(cells, 0);
                else
                    m_full.assign(cells, 0.0f);
            }

            void setRow(int f, const float* values)
            {
                const std::size_t base = static_cast<std::size_t>(f) * static_cast<std::size_t>(m_bins);

                if (m_compact)
                {
                    for (int g = 0; g < m_bins; ++g)
                        m_small[base + static_cast<std::size_t>(g)] = static_cast<std::uint8_t>(std::min(255.0f, std::round(values[g] * c_compactScale)));
                }
                else
                {
                    std::copy(values, values + m_bins, m_full.begin() + static_cast<std::ptrdiff_t>(base));
                }
            }

            // Row f as floats; a compact grid decodes into `scratch` (m_bins long).
            const float* row(int f, float* scratch) const noexcept
            {
                const std::size_t base = static_cast<std::size_t>(f) * static_cast<std::size_t>(m_bins);

                if (!m_compact)
                    return m_full.data() + base;

                for (int g = 0; g < m_bins; ++g)
                    scratch[g] = static_cast<float>(m_small[base + static_cast<std::size_t>(g)]) / c_compactScale;

                return scratch;
            }

            float cell(int f, int g) const noexcept
            {
                const std::size_t i = static_cast<std::size_t>(f) * static_cast<std::size_t>(m_bins) + static_cast<std::size_t>(g);
                return m_compact ? static_cast<float>(m_small[i]) / c_compactScale : m_full[i];
            }

            // Level at a fractional bin (linear between bins), 0 off the grid.
            double at(int f, double g) const noexcept
            {
                if (g < 0.0 || g >= m_bins - 1)
                    return 0.0;

                const int i = static_cast<int>(g);
                const double frac = g - i;
                return cell(f, i) * (1.0 - frac) + cell(f, i + 1) * frac;
            }

        private:
            int m_bins = 0;
            bool m_compact = false;
            std::vector<float> m_full;
            std::vector<std::uint8_t> m_small;
        };
        // What Auto may look at once Engine has run: the whitened spectrogram, while it is
        // still in memory. The callback returns false when it was cancelled.
        struct GridView
        {
            const LineGrid* grid = nullptr;
            double lowHz = 0.0; // frequency of bin 0
            int bins = 0;

            double binOf(double hz) const noexcept { return std::log2(hz / lowHz) * c_binsPerOctave; }
        };

        using GridUse = std::function<bool(const GridView&)>;

        constexpr double c_onPartialLevel = 0.4; // whitened level (log units) at which a frame of the line counts as on a partial
        constexpr float c_trustedShare = 0.8f;   // Analysis::engineOnGrid needs this share of audible frames on a partial (below it the line is seldom one partial)

        //==============================================================================
        // Engine: contour from the partial pattern
        //==============================================================================

        constexpr int c_patternHop = 2;     // frames per contour step
        constexpr int c_patternBins = 2;    // grid bins per contour state
        constexpr int c_patternReach = 6;   // states the contour may move in one step without a break
        constexpr int c_patternJump = 2 * c_patternReach + 1; // back-pointer code of a break
        constexpr int c_patternPoints = 32;        // peaks of the pattern that are matched
        constexpr int c_patternPasses = 2;         // contour searches: the clearest frame's pattern, then the re-learnt one
        constexpr double c_valleyShare = 0.5;      // weight of the gap between two peaks, as a share of their mean
        constexpr double c_bandPower = 0.5;        // how far a state is excused for the peaks it puts off the grid (0 = not, 1 = fully)
        constexpr float c_breakCost = 48.0f;       // a move of any size, in units of one frame's perfect match
        constexpr float c_silenceBreak = 0.25f;    // ... times this after 30 ms of silence
        constexpr float c_moveCost = 0.25f;        // per state away from the move step 3 expects (scaled by how far that is trusted)
        constexpr double c_steadyFloor = 0.2;      // step 2's match (normalised correlation) at and below which its shift is ignored
        constexpr double c_steadyTop = 0.5;        // ... and at and above which it is trusted in full

        struct PatternPoint
        {
            int offset;    // grid bins from the pattern's origin
            float weight;  // > 0 on a partial, < 0 in the gap between two
        };

        // Summing frame-to-frame shifts (step 3) random-walks on steady notes and
        // carries every mis-measured shift (a gear change, a lift-off, a loop seam)
        // to the end of the buffer. This step measures the position of the entire
        // set of partials in every frame against one pattern instead:
        //   - the pattern starts as the clearest frame's row, and is then re-learnt as
        //     the average of all rows lined up along the contour found with it;
        //   - on a log-frequency grid the partials of one source keep their spacing
        //     whatever the pitch, and no other shift lines up all of them, so the
        //     match has one best shift per frame (gaps between partials count
        //     against a shift, which is what rules out the octaves);
        //   - a Viterbi search then picks the contour: small moves are cheap, and a
        //     move of any size (a break) costs a fixed charge, less after silence.
        // The contour is absolute, so nothing accumulates, and a pass with gear
        // changes comes out as one line with a step at each change.
        bool patternContour(const LineGrid& grid, int gridBins, const std::vector<std::uint8_t>& silent,
                            const std::vector<float>& steadiness, const Settings& s,
                            const std::atomic<bool>* cancel, std::vector<double>& shape, int& breaks, float& match)
        {
            const int numFrames = static_cast<int>(shape.size());
            const int steps = (numFrames + c_patternHop - 1) / c_patternHop;
            const int reachBins = static_cast<int>(std::ceil(std::log2(s.highestHz / s.lowestHz) * c_binsPerOctave));
            const int centre = reachBins / c_patternBins;   // state of "no shift"
            const int states = 2 * centre + 1;
            const int patternLength = gridBins + 2 * reachBins; // pattern index = offset + reachBins

            const int silenceSteps = std::max(1, static_cast<int>(std::lround(0.03 / (s.hopSeconds * c_patternHop))));

            std::vector<float> scratch(static_cast<std::size_t>(gridBins));
            std::vector<float> row(static_cast<std::size_t>(gridBins));

            // A frame's row, smoothed over +-2 bins so a partial is found from a state grid of 2 bins.
            auto smoothRow = [&](int f)
            {
                const float* v = grid.row(f, scratch.data());

                for (int g = 0; g < gridBins; ++g)
                {
                    const float a = g >= 2 ? v[g - 2] : 0.0f;
                    const float b = g >= 1 ? v[g - 1] : 0.0f;
                    const float d = g + 1 < gridBins ? v[g + 1] : 0.0f;
                    const float e = g + 2 < gridBins ? v[g + 2] : 0.0f;
                    row[static_cast<std::size_t>(g)] = (a + 2.0f * b + 3.0f * v[g] + 2.0f * d + e) * (1.0f / 9.0f);
                }
            };

            // The clearest stretch: the highest whitened energy, averaged over +-0.1 s.
            std::vector<float> energy(static_cast<std::size_t>(steps), 0.0f);
            int audible = 0;

            for (int j = 0; j < steps; ++j)
            {
                const int f = j * c_patternHop;

                if (j % c_progressFrames == 0 && isCancelled(cancel))
                    return false;

                if (silent[static_cast<std::size_t>(f)] != 0)
                    continue;

                const float* v = grid.row(f, scratch.data());
                double sum = 0.0;

                for (int g = 0; g < gridBins; ++g)
                    sum += static_cast<double>(v[g]) * v[g];

                energy[static_cast<std::size_t>(j)] = static_cast<float>(sum);
                ++audible;
            }

            if (audible < 4)
                return true;

            int reference = 0;
            {
                const int half = std::max(1, static_cast<int>(std::lround(0.1 / (s.hopSeconds * c_patternHop))));
                double best = -1.0;

                for (int j = 0; j < steps; ++j)
                {
                    if (energy[static_cast<std::size_t>(j)] <= 0.0f)
                        continue;

                    double sum = 0.0;

                    for (int k = std::max(0, j - half); k <= std::min(steps - 1, j + half); ++k)
                        sum += energy[static_cast<std::size_t>(k)];

                    if (sum > best)
                    {
                        best = sum;
                        reference = j;
                    }
                }
            }

            std::vector<float> pattern(static_cast<std::size_t>(patternLength), 0.0f);
            std::vector<float> cover(static_cast<std::size_t>(patternLength), 0.0f);
            smoothRow(reference * c_patternHop);

            for (int g = 0; g < gridBins; ++g)
            {
                pattern[static_cast<std::size_t>(g + reachBins)] = row[static_cast<std::size_t>(g)];
                cover[static_cast<std::size_t>(g + reachBins)] = 1.0f;
            }

            // What step 3 expects each step to move (in states), and how far to trust it.
            std::vector<float> expected(static_cast<std::size_t>(steps), 0.0f);
            std::vector<float> trust(static_cast<std::size_t>(steps), 0.0f);

            for (int j = 1; j < steps; ++j)
            {
                const int f = j * c_patternHop;
                expected[static_cast<std::size_t>(j)] = static_cast<float>((shape[static_cast<std::size_t>(f)] - shape[static_cast<std::size_t>(f - c_patternHop)]) / c_patternBins);
                float q = 1.0f;

                for (int k = f - c_patternHop + 1; k <= f; ++k)
                    q = std::min(q, steadiness[static_cast<std::size_t>(k)]);

                trust[static_cast<std::size_t>(j)] = static_cast<float>(std::clamp((q - c_steadyFloor) / (c_steadyTop - c_steadyFloor), 0.0, 1.0));
            }

            std::vector<PatternPoint> points;
            std::vector<float> scale(static_cast<std::size_t>(states));
            std::vector<float> acc(static_cast<std::size_t>(states));
            std::vector<float> previous(static_cast<std::size_t>(states));
            std::vector<float> current(static_cast<std::size_t>(states));
            std::vector<std::uint8_t> back((static_cast<std::size_t>(steps) * static_cast<std::size_t>(states) + 1) / 2);
            std::vector<std::int16_t> leader(static_cast<std::size_t>(steps), 0);
            std::vector<int> path(static_cast<std::size_t>(steps), centre);
            std::vector<float> matched(static_cast<std::size_t>(steps), 0.0f);
            std::vector<std::uint8_t> broke(static_cast<std::size_t>(steps), 0);
            std::vector<float> fine(static_cast<std::size_t>(steps), 0.0f);

            // The pattern as a few points: its tallest peaks, and the lowest point between neighbours.
            auto pickPoints = [&]
            {
                points.clear();
                double mean = 0.0;
                int covered = 0;

                for (int k = 0; k < patternLength; ++k)
                {
                    if (cover[static_cast<std::size_t>(k)] > 0.0f)
                    {
                        mean += pattern[static_cast<std::size_t>(k)];
                        ++covered;
                    }
                }

                if (covered == 0)
                    return;

                mean /= covered;
                std::vector<int> peaks;

                for (int k = 1; k + 1 < patternLength; ++k)
                {
                    const float v = pattern[static_cast<std::size_t>(k)];

                    if (cover[static_cast<std::size_t>(k)] > 0.0f && v > mean
                        && v > pattern[static_cast<std::size_t>(k) - 1] && v >= pattern[static_cast<std::size_t>(k) + 1])
                        peaks.push_back(k);
                }

                std::stable_sort(peaks.begin(), peaks.end(),
                                 [&](int a, int b) { return pattern[static_cast<std::size_t>(a)] > pattern[static_cast<std::size_t>(b)]; });

                std::vector<int> kept;

                for (const int k : peaks)
                {
                    if (static_cast<int>(kept.size()) >= c_patternPoints)
                        break;

                    if (std::none_of(kept.begin(), kept.end(), [&](int other) { return std::abs(other - k) < 6; }))
                        kept.push_back(k);
                }

                std::sort(kept.begin(), kept.end());

                for (std::size_t i = 0; i < kept.size(); ++i)
                {
                    const int k = kept[i];
                    points.push_back({ k - reachBins, static_cast<float>(pattern[static_cast<std::size_t>(k)] - mean) });

                    if (i + 1 < kept.size() && kept[i + 1] - k >= 8)
                    {
                        int lowest = k + 1;

                        for (int m = k + 1; m < kept[i + 1]; ++m)
                            if (pattern[static_cast<std::size_t>(m)] < pattern[static_cast<std::size_t>(lowest)])
                                lowest = m;

                        const double either = 0.5 * (pattern[static_cast<std::size_t>(k)] + pattern[static_cast<std::size_t>(kept[i + 1])]) - mean;
                        points.push_back({ lowest - reachBins, static_cast<float>(-c_valleyShare * either) });
                    }
                }

                // A state's match is divided by the pattern's own match, less for the share of
                // its peaks that state keeps on the grid; states that keep under a quarter are out.
                double self = 0.0;
                double total = 0.0;

                for (const auto& point : points)
                {
                    self += point.weight * pattern[static_cast<std::size_t>(point.offset + reachBins)];
                    total += std::max(0.0f, point.weight);
                }

                for (int state = 0; state < states; ++state)
                {
                    const int shift = (state - centre) * c_patternBins;
                    double inside = 0.0;

                    for (const auto& point : points)
                    {
                        const int g = point.offset + shift;

                        if (g >= 0 && g < gridBins)
                            inside += std::max(0.0f, point.weight);
                    }

                    const double share = total > 0.0 ? inside / total : 0.0;
                    scale[static_cast<std::size_t>(state)] = share >= 0.25 && self > 0.0
                        ? static_cast<float>(1.0 / (self * std::pow(share, c_bandPower))) : 0.0f;
                }
            };

            // Match of the current `row` against the pattern at every state.
            auto matchRow = [&]
            {
                std::fill(acc.begin(), acc.end(), 0.0f);

                for (const auto& point : points)
                {
                    // g = offset + (state - centre) * bins must lie in [0, gridBins)
                    const int base = point.offset - centre * c_patternBins;
                    const int first = std::max(0, (-base + c_patternBins - 1) / c_patternBins);
                    const int last = std::min(states - 1, (gridBins - 1 - base) / c_patternBins);
                    const float w = point.weight;
                    const float* r = row.data() + base;

                    for (int state = first; state <= last; ++state)
                        acc[static_cast<std::size_t>(state)] += w * r[state * c_patternBins];
                }

                for (int state = 0; state < states; ++state)
                    acc[static_cast<std::size_t>(state)] *= scale[static_cast<std::size_t>(state)];
            };

            auto setBack = [&](int j, int state, int code)
            {
                const std::size_t i = static_cast<std::size_t>(j) * static_cast<std::size_t>(states) + static_cast<std::size_t>(state);
                const int shiftBits = (i & 1) != 0 ? 4 : 0;
                back[i / 2] = static_cast<std::uint8_t>((back[i / 2] & ~(0xF << shiftBits)) | (code << shiftBits));
            };

            auto getBack = [&](int j, int state)
            {
                const std::size_t i = static_cast<std::size_t>(j) * static_cast<std::size_t>(states) + static_cast<std::size_t>(state);
                return (back[i / 2] >> ((i & 1) != 0 ? 4 : 0)) & 0xF;
            };

            for (int pass = 0; pass < c_patternPasses; ++pass)
            {
                pickPoints();

                if (points.empty())
                    return true;

                std::fill(previous.begin(), previous.end(), 0.0f);
                int quiet = 0;

                for (int j = 0; j < steps; ++j)
                {
                    if (j % 16 == 0 && isCancelled(cancel))
                        return false;

                    const int f = j * c_patternHop;
                    int lead = 0;

                    for (int state = 1; state < states; ++state)
                        if (previous[static_cast<std::size_t>(state)] > previous[static_cast<std::size_t>(lead)])
                            lead = state;

                    leader[static_cast<std::size_t>(j)] = static_cast<std::int16_t>(lead);
                    const float top = previous[static_cast<std::size_t>(lead)];

                    if (silent[static_cast<std::size_t>(f)] != 0)
                    {
                        for (int state = 0; state < states; ++state)
                        {
                            current[static_cast<std::size_t>(state)] = previous[static_cast<std::size_t>(state)] - top;
                            setBack(j, state, c_patternReach);
                        }

                        ++quiet;
                    }
                    else
                    {
                        smoothRow(f);
                        matchRow();
                        const float sure = trust[static_cast<std::size_t>(j)];
                        const float want = expected[static_cast<std::size_t>(j)];
                        const float jump = top - (quiet >= silenceSteps ? c_breakCost * c_silenceBreak : c_breakCost);
                        quiet = 0;

                        for (int state = 0; state < states; ++state)
                        {
                            float best = jump;
                            int code = c_patternJump;

                            for (int d = -c_patternReach; d <= c_patternReach; ++d)
                            {
                                const int from = state + d;

                                if (from < 0 || from >= states)
                                    continue;

                                // moving from `from` to `state` is a move of -d states
                                const float v = previous[static_cast<std::size_t>(from)]
                                              - c_moveCost * std::abs(static_cast<float>(-d) - sure * want);

                                if (v > best || (!(v < best) && code == c_patternJump)) // a tie goes to a move, not to a break
                                {
                                    best = v;
                                    code = d + c_patternReach;
                                }
                            }

                            current[static_cast<std::size_t>(state)] = best + acc[static_cast<std::size_t>(state)] - top;
                            setBack(j, state, code);
                        }
                    }

                    std::swap(previous, current);
                }

                int state = 0;

                for (int k = 1; k < states; ++k)
                    if (previous[static_cast<std::size_t>(k)] > previous[static_cast<std::size_t>(state)])
                        state = k;

                std::fill(broke.begin(), broke.end(), std::uint8_t { 0 });

                for (int j = steps - 1; j >= 0; --j)
                {
                    path[static_cast<std::size_t>(j)] = state;

                    if (j == 0)
                        break;

                    const int code = getBack(j, state);

                    if (code == c_patternJump)
                    {
                        const int from = leader[static_cast<std::size_t>(j)];
                        broke[static_cast<std::size_t>(j)] = from != state ? 1 : 0;
                        state = from;
                    }
                    else
                    {
                        state += code - c_patternReach;
                    }
                }

                // Along the path: the match (the weight of each frame in the next pattern)
                // and a parabola through the neighbouring states for the fraction of a state.
                const bool lastPass = pass + 1 == c_patternPasses;

                if (!lastPass)
                {
                    std::fill(pattern.begin(), pattern.end(), 0.0f);
                    std::fill(cover.begin(), cover.end(), 0.0f);
                }

                double weightSum = 0.0;

                for (int j = 0; j < steps; ++j)
                {
                    const int f = j * c_patternHop;
                    matched[static_cast<std::size_t>(j)] = 0.0f;
                    fine[static_cast<std::size_t>(j)] = 0.0f;

                    if (silent[static_cast<std::size_t>(f)] != 0)
                        continue;

                    if (j % 64 == 0 && isCancelled(cancel))
                        return false;

                    smoothRow(f);
                    const int at = path[static_cast<std::size_t>(j)];
                    float near[3] = { 0.0f, 0.0f, 0.0f };

                    for (int o = -1; o <= 1; ++o)
                    {
                        const int k = at + o;

                        if (k < 0 || k >= states)
                            continue;

                        const int shift = (k - centre) * c_patternBins;
                        float sum = 0.0f;

                        for (const auto& point : points)
                        {
                            const int g = point.offset + shift;

                            if (g >= 0 && g < gridBins)
                                sum += point.weight * row[static_cast<std::size_t>(g)];
                        }

                        near[o + 1] = sum * scale[static_cast<std::size_t>(k)];
                    }

                    matched[static_cast<std::size_t>(j)] = near[1];
                    const float curvature = near[0] - 2.0f * near[1] + near[2];

                    if (curvature < -1e-9f && near[1] >= near[0] && near[1] >= near[2])
                        fine[static_cast<std::size_t>(j)] = std::clamp(0.5f * (near[0] - near[2]) / curvature, -0.5f, 0.5f);

                    if (!lastPass && near[1] > 0.0f)
                    {
                        const int shift = (at - centre) * c_patternBins;

                        for (int g = 0; g < gridBins; ++g)
                        {
                            const int k = g - shift + reachBins;

                            if (k >= 0 && k < patternLength)
                            {
                                pattern[static_cast<std::size_t>(k)] += near[1] * row[static_cast<std::size_t>(g)];
                                cover[static_cast<std::size_t>(k)] += near[1];
                            }
                        }

                        weightSum += near[1];
                    }
                }

                if (!lastPass)
                {
                    if (!(weightSum > 0.0))
                        return true;

                    for (int k = 0; k < patternLength; ++k)
                    {
                        const auto i = static_cast<std::size_t>(k);

                        if (cover[i] >= 0.25 * weightSum)
                        {
                            pattern[i] /= cover[i];
                        }
                        else
                        {
                            pattern[i] = 0.0f;
                            cover[i] = 0.0f;
                        }
                    }
                }
            }

            // The contour in grid bins per frame: straight between steps, stepped at a break.
            breaks = 0;
            auto binsAt = [&](int j) { return (path[static_cast<std::size_t>(j)] - centre + static_cast<double>(fine[static_cast<std::size_t>(j)])) * c_patternBins; };
            const double origin = binsAt(0);

            for (int f = 0; f < numFrames; ++f)
            {
                const int j = f / c_patternHop;
                const int next = std::min(steps - 1, j + 1);
                const double u = static_cast<double>(f - j * c_patternHop) / c_patternHop;
                const double here = binsAt(j);
                const double there = binsAt(next);
                const bool step = next != j && broke[static_cast<std::size_t>(next)] != 0;
                shape[static_cast<std::size_t>(f)] = (step ? (u < 0.5 ? here : there) : here + (there - here) * u) - origin;
            }

            for (int j = 1; j < steps; ++j)
                breaks += broke[static_cast<std::size_t>(j)] != 0 ? 1 : 0;

            double sum = 0.0;

            for (int j = 0; j < steps; ++j)
                sum += std::clamp(matched[static_cast<std::size_t>(j)], 0.0f, 1.0f);

            match = static_cast<float>(sum / audible);
            return true;
        }

        // What Engine says about its own line, for Auto and for Analysis.
        struct EngineReport
        {
            bool trusted = true;          // Analysis::engineOnGrid
            bool fits = true;             // the contour fitted inside the search range
            float partialShare = -1.0f;   // share of audible frames where the line is on a partial
            float patternMatch = -1.0f;   // how well the partial pattern was found along the contour, 0..1
            int breaks = -1;              // steps the contour made (gear changes, lift-offs, edits)
        };

        bool runEngine(const Signal& x, const std::vector<std::uint8_t>& silent, const Settings& s,
                       const std::atomic<bool>* cancel, const Stage& stage, Line& chosen, std::vector<Line>& parallel,
                       EngineReport& report, const GridUse& useGrid = {})
        {
            const int n = x.length;
            const double rate = x.rate;
            const int numFrames = static_cast<int>(chosen.frames.size());

            // FFT frame: the smallest 2^k samples that cover 0.3 s, then zero-padded to double length.
            int window = 1;

            while (window < static_cast<int>(rate * 0.3))
                window <<= 1;

            const int fftSize = window * 2;
            const int numBins = fftSize / 2;
            const double binHz = rate / fftSize;

            // Grid limits: half the lowest search frequency (20 Hz minimum) up to double the
            // highest (2 kHz minimum), so neighbouring partials stay visible.
            const double gridLowHz = std::max(20.0, s.lowestHz * 0.5);
            const double gridHighHz = std::min(rate * 0.45, std::max(s.highestHz * 2.0, 2000.0));
            const int gridBins = static_cast<int>(std::ceil(std::log2(gridHighHz / gridLowHz) * c_binsPerOctave)) + 1;
            auto binToHz = [&](double g) { return gridLowHz * std::exp2(g / c_binsPerOctave); };

            std::vector<double> hann(static_cast<std::size_t>(window));
            std::vector<double> re(static_cast<std::size_t>(fftSize));
            std::vector<double> im(static_cast<std::size_t>(fftSize));
            std::vector<double> logMag(static_cast<std::size_t>(numBins));
            std::vector<double> whitened(static_cast<std::size_t>(numBins));
            std::vector<float> rowValues(static_cast<std::size_t>(gridBins));

            for (int i = 0; i < window; ++i)
                hann[static_cast<std::size_t>(i)] = 0.5 - 0.5 * std::cos(2.0 * c_pi * i / (window - 1));

            // Clearing the grid touches 15 MB per minute: not worth starting once cancelled.
            if (isCancelled(cancel))
                return false;

            LineGrid grid(numFrames, gridBins, s.compactGrid);
            const int whitenHalf = std::max(3, static_cast<int>(15.0 / binHz)); // about 15 Hz each side

            // 1. Whitened spectrogram: subtracting a +-15 Hz running mean from the log
            //    magnitude leaves sharp partials and flattens formant-like humps. Each
            //    frame is then interpolated onto the log-frequency grid. Silent frames
            //    keep an empty row.
            for (int f = 0; f < numFrames; ++f)
            {
                if (isCancelled(cancel))
                    return false;

                if (f % c_progressFrames == 0)
                    stage(0.7f * static_cast<float>(f) / static_cast<float>(numFrames));

                if (silent[static_cast<std::size_t>(f)] != 0)
                    continue;

                const int centre = static_cast<int>(std::lround(f * s.hopSeconds * rate));
                std::fill(re.begin(), re.end(), 0.0);
                std::fill(im.begin(), im.end(), 0.0);

                for (int i = 0; i < window; ++i)
                {
                    const int k = centre - window / 2 + i;
                    re[static_cast<std::size_t>(i)] = (k >= 0 && k < n ? x.data[k] : 0.0) * hann[static_cast<std::size_t>(i)];
                }

                fftRadix2(re.data(), im.data(), static_cast<std::size_t>(fftSize));

                for (int b = 0; b < numBins; ++b)
                {
                    const auto i = static_cast<std::size_t>(b);
                    logMag[i] = std::log(std::sqrt(re[i] * re[i] + im[i] * im[i]) + 1e-9);
                }

                double sum = 0.0;
                int count = 0;
                int lo = 0;
                int hi = -1;

                for (int b = 0; b < numBins; ++b)
                {
                    while (hi < std::min(numBins - 1, b + whitenHalf))
                    {
                        ++hi;
                        sum += logMag[static_cast<std::size_t>(hi)];
                        ++count;
                    }

                    while (lo < b - whitenHalf)
                    {
                        sum -= logMag[static_cast<std::size_t>(lo)];
                        ++lo;
                        --count;
                    }

                    whitened[static_cast<std::size_t>(b)] = std::max(0.0, logMag[static_cast<std::size_t>(b)] - sum / count);
                }

                std::fill(rowValues.begin(), rowValues.end(), 0.0f);

                for (int g = 0; g < gridBins; ++g)
                {
                    const double pos = binToHz(g) / binHz;
                    const int i = static_cast<int>(pos);

                    if (i + 1 >= numBins)
                        break;

                    const double frac = pos - i;
                    rowValues[static_cast<std::size_t>(g)] = static_cast<float>(whitened[static_cast<std::size_t>(i)] * (1.0 - frac)
                                                                               + whitened[static_cast<std::size_t>(i) + 1] * frac);
                }

                grid.setRow(f, rowValues.data());
            }

            // 2. Shift between frames `span` apart (80 ms), by cross-correlation along the
            //    grid. Before correlating, every bin loses its own +-0.3 s mean, so
            //    stationary components (mains hum, body resonances) do not bias the shift
            //    towards zero. Those mean-removed rows come from two running column sums,
            //    and a ring of span + 1 rows is all that is stored.
            const int meanHalf = std::max(2, static_cast<int>(std::lround(0.3 / s.hopSeconds)));
            const int span = std::max(2, static_cast<int>(std::lround(0.08 / s.hopSeconds)));
            const int maxSlide = c_binsPerOctave / 4; // search +-48 bins
            std::vector<double> slope(static_cast<std::size_t>(numFrames), 0.0);
            std::vector<float> steadiness(static_cast<std::size_t>(numFrames), 0.0f); // 0..1: how well the two frames matched
            std::vector<double> match(static_cast<std::size_t>(2 * maxSlide + 1));
            std::vector<float> moving(static_cast<std::size_t>(span + 1) * static_cast<std::size_t>(gridBins));
            std::vector<double> sumBefore(static_cast<std::size_t>(gridBins), 0.0);
            std::vector<double> sumUpTo(static_cast<std::size_t>(gridBins), 0.0);
            std::vector<float> scratch(static_cast<std::size_t>(gridBins));
            int before = 0;
            int upTo = 0;

            for (int f = 0; f < numFrames; ++f)
            {
                if (isCancelled(cancel))
                    return false;

                if (f % c_progressFrames == 0)
                    stage(0.7f + 0.2f * static_cast<float>(f) / static_cast<float>(numFrames));

                const int a = std::max(0, f - meanHalf);
                const int b = std::min(numFrames, f + meanHalf + 1);

                for (; upTo < b; ++upTo)
                {
                    const float* values = grid.row(upTo, scratch.data());

                    for (int g = 0; g < gridBins; ++g)
                        sumUpTo[static_cast<std::size_t>(g)] += values[g];
                }

                for (; before < a; ++before)
                {
                    const float* values = grid.row(before, scratch.data());

                    for (int g = 0; g < gridBins; ++g)
                        sumBefore[static_cast<std::size_t>(g)] += values[g];
                }

                float* now = &moving[static_cast<std::size_t>(f % (span + 1)) * static_cast<std::size_t>(gridBins)];
                const float* level = grid.row(f, scratch.data());

                for (int g = 0; g < gridBins; ++g)
                {
                    const double mean = (sumUpTo[static_cast<std::size_t>(g)] - sumBefore[static_cast<std::size_t>(g)]) / (b - a);
                    now[g] = static_cast<float>(std::max(0.0, level[g] - mean));
                }

                if (f < span || silent[static_cast<std::size_t>(f)] != 0 || silent[static_cast<std::size_t>(f - span)] != 0)
                    continue;

                const float* then = &moving[static_cast<std::size_t>((f - span) % (span + 1)) * static_cast<std::size_t>(gridBins)];
                int bestSlide = 0;
                double best = -1.0;

                for (int k = -maxSlide; k <= maxSlide; ++k)
                {
                    double c = 0.0;

                    for (int g = std::max(0, k); g < std::min(gridBins, gridBins + k); ++g)
                        c += static_cast<double>(now[g]) * then[g - k];

                    match[static_cast<std::size_t>(k + maxSlide)] = c;

                    if (c > best)
                    {
                        best = c;
                        bestSlide = k;
                    }
                }

                double slide = bestSlide;

                if (bestSlide > -maxSlide && bestSlide < maxSlide)
                {
                    const double p = match[static_cast<std::size_t>(bestSlide + maxSlide - 1)];
                    const double q = match[static_cast<std::size_t>(bestSlide + maxSlide)];
                    const double r = match[static_cast<std::size_t>(bestSlide + maxSlide + 1)];
                    const double curvature = p - 2.0 * q + r;

                    if (std::abs(curvature) > 1e-12)
                        slide += std::clamp(0.5 * (p - r) / curvature, -0.5, 0.5);
                }

                slope[static_cast<std::size_t>(f - span / 2)] = slide / span; // grid bins per hop, centred

                if (bestSlide > -maxSlide && bestSlide < maxSlide)
                {
                    double nowPower = 0.0;
                    double thenPower = 0.0;

                    for (int g = 0; g < gridBins; ++g)
                    {
                        nowPower += static_cast<double>(now[g]) * now[g];
                        thenPower += static_cast<double>(then[g]) * then[g];
                    }

                    if (nowPower > 0.0 && thenPower > 0.0)
                        steadiness[static_cast<std::size_t>(f - span / 2)] = static_cast<float>(std::clamp(best / std::sqrt(nowPower * thenPower), 0.0, 1.0));
                }
            }

            // 3. Moving-average the per-frame shift (+-span) and accumulate it into the contour (grid bins).
            std::vector<double> shape(static_cast<std::size_t>(numFrames), 0.0);
            {
                double total = 0.0;

                for (int f = 0; f < numFrames; ++f)
                {
                    double sum = 0.0;
                    int m = 0;

                    for (int k = std::max(0, f - span); k <= std::min(numFrames - 1, f + span); ++k)
                    {
                        sum += slope[static_cast<std::size_t>(k)];
                        ++m;
                    }

                    if (f > 0)
                        total += sum / m;

                    shape[static_cast<std::size_t>(f)] = total;
                }
            }

            // 3b. Re-anchor the contour on the partial pattern itself (see patternContour).
            if (!patternContour(grid, gridBins, silent, steadiness, s, cancel, shape, report.breaks, report.patternMatch))
                return false;

            stage(0.9f);

            // 4. Vertical placement. The contour may be slid up and down only as far as the
            //    whole of it stays inside the search range: the anchor offsets tried are those
            //    for which the lowest audible frame is at or above lowestHz and the highest
            //    at or below highestHz. Each is scored by the mean whitened level found along
            //    the offset contour over audible frames; local maxima of that score are the
            //    partials.
            const double offsetLow = std::log2(s.lowestHz / gridLowHz) * c_binsPerOctave;
            const double offsetHigh = std::log2(s.highestHz / gridLowHz) * c_binsPerOctave;
            double shapeLow = 0.0;
            double shapeHigh = 0.0;
            {
                bool any = false;

                for (int f = 0; f < numFrames; ++f)
                {
                    if (silent[static_cast<std::size_t>(f)] != 0)
                        continue;

                    const double v = shape[static_cast<std::size_t>(f)];
                    shapeLow = any ? std::min(shapeLow, v) : v;
                    shapeHigh = any ? std::max(shapeHigh, v) : v;
                    any = true;
                }
            }

            double anchorLow = offsetLow - shapeLow;
            double anchorHigh = offsetHigh - shapeHigh;
            const bool fits = anchorLow <= anchorHigh;

            if (!fits)
            {
                // The contour spans more than the search range, so it cannot be a single
                // partial inside it: centre it, clamp it (step 5), and say so (onGrid).
                anchorLow = anchorHigh = 0.5 * (anchorLow + anchorHigh);
            }

            auto scoreOffset = [&](double offset)
            {
                double sum = 0.0;
                int m = 0;

                for (int f = 0; f < numFrames; f += 2)
                {
                    if (silent[static_cast<std::size_t>(f)] == 0)
                    {
                        sum += grid.at(f, offset + shape[static_cast<std::size_t>(f)]);
                        ++m;
                    }
                }

                return m > 0 ? sum / m : 0.0;
            };

            std::vector<double> offsets;
            std::vector<double> scores;
            double bestOffset = anchorLow;
            double topScore = -1.0;

            for (double c = anchorLow; c <= anchorHigh; c += 1.0)
            {
                if (isCancelled(cancel))
                    return false;

                const double score = scoreOffset(c);
                offsets.push_back(c);
                scores.push_back(score);

                if (score > topScore)
                {
                    topScore = score;
                    bestOffset = c;
                }
            }

            // In exhausts the top-scoring partial is frequently a half order, and the
            // perceived pitch sits an octave or two above it. So step up an octave at a
            // time (at most three) while the partial up there still reaches climbShare of
            // the STRONGEST partial's score and the whole line stays inside the range. The
            // comparison is always with the strongest partial, never with the previous
            // step, so a chain of ever weaker octaves cannot carry the line away.
            double chosenScore = topScore;
            {
                const double strongestOffset = bestOffset;

                for (int i = 1; i <= 3; ++i)
                {
                    const double up = strongestOffset + i * c_binsPerOctave;
                    double higherOffset = up;
                    double higherScore = -1.0;

                    for (double c = up - 4; c <= up + 4; c += 0.5)
                    {
                        if (c <= anchorHigh)
                        {
                            const double score = scoreOffset(c);

                            if (score > higherScore)
                            {
                                higherScore = score;
                                higherOffset = c;
                            }
                        }
                    }

                    if (higherScore < s.climbShare * topScore)
                        break;

                    bestOffset = higherOffset;
                    chosenScore = higherScore;
                }
            }

            // 5. Render a contour at an offset, then correct it towards the ridge: in each
            //    frame find the strongest level no more than 12 bins away (weighted towards
            //    the centre) and add the +-0.15 s average of those corrections.
            const int pull = c_binsPerOctave / 16;
            const int pullHalf = std::max(1, static_cast<int>(std::lround(0.15 / s.hopSeconds)));
            std::vector<double> nudge(static_cast<std::size_t>(numFrames));
            std::vector<std::uint8_t> hasNudge(static_cast<std::size_t>(numFrames));

            auto place = [&](double offset, std::vector<Frame>& out)
            {
                std::fill(nudge.begin(), nudge.end(), 0.0);
                std::fill(hasNudge.begin(), hasNudge.end(), std::uint8_t { 0 });

                for (int f = 0; f < numFrames; ++f)
                {
                    if (silent[static_cast<std::size_t>(f)] != 0)
                        continue;

                    const double centre = offset + shape[static_cast<std::size_t>(f)];
                    double bestLevel = 0.0;
                    double bestStep = 0.0;

                    for (int k = -pull; k <= pull; ++k)
                    {
                        const double v = grid.at(f, centre + k) * (1.0 - 0.5 * std::abs(k) / pull);

                        if (v > bestLevel)
                        {
                            bestLevel = v;
                            bestStep = k;
                        }
                    }

                    if (bestLevel > 0.0)
                    {
                        nudge[static_cast<std::size_t>(f)] = bestStep;
                        hasNudge[static_cast<std::size_t>(f)] = 1;
                    }
                }

                for (int f = 0; f < numFrames; ++f)
                {
                    Frame& frame = out[static_cast<std::size_t>(f)];

                    if (silent[static_cast<std::size_t>(f)] != 0)
                    {
                        frame.hz = 0.0f;
                        frame.confidence = 0.0f;
                        continue;
                    }

                    double sum = 0.0;
                    int m = 0;

                    for (int k = std::max(0, f - pullHalf); k <= std::min(numFrames - 1, f + pullHalf); ++k)
                    {
                        if (hasNudge[static_cast<std::size_t>(k)] != 0)
                        {
                            sum += nudge[static_cast<std::size_t>(k)];
                            ++m;
                        }
                    }

                    const double g = std::clamp(offset + shape[static_cast<std::size_t>(f)] + (m > 0 ? sum / m : 0.0), offsetLow, offsetHigh);
                    frame.hz = static_cast<float>(binToHz(g));
                    frame.confidence = static_cast<float>(std::clamp(grid.at(f, g) / 3.0, 0.0, 1.0));
                }
            };

            place(bestOffset, chosen.frames);
            chosen.score = static_cast<float>(chosenScore);
            chosen.ratio = 1.0f;

            // How much of the line is on a partial: the share of audible frames whose
            // whitened level at the line reaches c_onPartialLevel. A contour that had to be
            // clamped into the range, or a line that is on a partial too rarely, is not to
            // be trusted (see the header).
            {
                int audible = 0;
                int onPartial = 0;

                for (int f = 0; f < numFrames; ++f)
                {
                    if (silent[static_cast<std::size_t>(f)] != 0)
                        continue;

                    ++audible;
                    const double g = std::log2(static_cast<double>(chosen.frames[static_cast<std::size_t>(f)].hz) / gridLowHz) * c_binsPerOctave;
                    onPartial += grid.at(f, g) >= c_onPartialLevel ? 1 : 0;
                }

                report.partialShare = audible > 0 ? static_cast<float>(onPartial) / static_cast<float>(audible) : 0.0f;
                report.fits = fits;
                report.trusted = fits && report.partialShare >= c_trustedShare;
            }

            // 6. Candidate lines for the multi-curve view: the other partials that move with
            //    the chosen one, each as an exact small fraction of it. Every other peak of
            //    the placement score (at least 50 cents from its neighbours) is read as the
            //    simplest fraction p/q, q <= 8, within 25 cents of it, and scored where that
            //    fraction says it is; peaks that fit none are left out. The octave below and
            //    the octave above are always offered when they fit in the range. The lines
            //    are the chosen line times the fraction, so they stay inside the range too.
            if (s.parallelLines > 0 && scores.size() > 2)
            {
                struct Candidate
                {
                    int p;
                    int q;
                    double score;
                    bool octave;
                };

                std::vector<Candidate> candidates;

                auto add = [&](int p, int q, bool octave)
                {
                    for (const auto& c : candidates)
                        if (c.p == p && c.q == q)
                            return;

                    const double offset = bestOffset + std::log2(static_cast<double>(p) / q) * c_binsPerOctave;

                    if (offset < anchorLow || offset > anchorHigh)
                        return;

                    double score = 0.0;

                    for (int k = -2; k <= 2; ++k)
                        score = std::max(score, scoreOffset(offset + k));

                    candidates.push_back({ p, q, score, octave });
                };

                add(1, 2, true);
                add(2, 1, true);

                const int apart = c_binsPerOctave / 24;
                const int last = static_cast<int>(scores.size()) - 1;

                for (int k = 1; k < last; ++k)
                {
                    if (isCancelled(cancel))
                        return false;

                    if (scores[static_cast<std::size_t>(k)] <= 0.0)
                        continue;

                    bool isPeak = true;

                    for (int j = std::max(0, k - apart); j <= std::min(last, k + apart) && isPeak; ++j)
                    {
                        if (j < k ? scores[static_cast<std::size_t>(j)] >= scores[static_cast<std::size_t>(k)]
                                  : scores[static_cast<std::size_t>(j)] > scores[static_cast<std::size_t>(k)])
                            isPeak = false;
                    }

                    if (!isPeak)
                        continue;

                    const double octaves = (offsets[static_cast<std::size_t>(k)] - bestOffset) / c_binsPerOctave;
                    const double ratio = std::exp2(octaves);

                    for (int q = 1; q <= 8; ++q)
                    {
                        const int p = static_cast<int>(std::lround(ratio * q));

                        if (p >= 1 && std::abs(octaves - std::log2(static_cast<double>(p) / q)) * 1200.0 <= 25.0)
                        {
                            if (p != q)
                                add(p, q, false);

                            break;
                        }
                    }
                }

                // Strongest first; equal scores keep the order they were found in.
                std::stable_sort(candidates.begin(), candidates.end(),
                                 [](const Candidate& a, const Candidate& b) { return a.score > b.score; });

                // Too many: the weakest go, the two octaves last of all.
                for (const bool octaves : { false, true })
                {
                    for (int i = static_cast<int>(candidates.size()) - 1; i >= 0 && static_cast<int>(candidates.size()) > s.parallelLines; --i)
                        if (candidates[static_cast<std::size_t>(i)].octave == octaves)
                            candidates.erase(candidates.begin() + i);
                }

                for (const auto& c : candidates)
                {
                    if (isCancelled(cancel))
                        return false;

                    const double ratio = static_cast<double>(c.p) / c.q;
                    const double shift = std::log2(ratio) * c_binsPerOctave;
                    Line line;
                    line.frames.resize(static_cast<std::size_t>(numFrames));

                    for (int f = 0; f < numFrames; ++f)
                    {
                        const Frame& from = chosen.frames[static_cast<std::size_t>(f)];

                        if (from.hz > 0.0f)
                        {
                            const double g = std::clamp(std::log2(static_cast<double>(from.hz) / gridLowHz) * c_binsPerOctave + shift, offsetLow, offsetHigh);
                            line.frames[static_cast<std::size_t>(f)].hz = static_cast<float>(binToHz(g));
                            line.frames[static_cast<std::size_t>(f)].confidence = static_cast<float>(std::clamp(grid.at(f, g) / 3.0, 0.0, 1.0));
                        }
                    }

                    line.score = static_cast<float>(c.score);
                    line.ratio = static_cast<float>(ratio);
                    parallel.push_back(std::move(line));
                }
            }

            if (useGrid && !useGrid(GridView { &grid, gridLowHz, gridBins }))
                return false;

            return true;
        }


        //==============================================================================
        // Finishing helpers
        //==============================================================================

        void finishLine(Line& line, const std::vector<std::uint8_t>& silent, const Settings& s)
        {
            for (std::size_t f = 0; f < line.frames.size(); ++f)
            {
                if (silent[f] != 0)
                {
                    line.frames[f] = {};
                    line.frames[f].silent = true;
                }
            }

            bridgeGaps(line.frames, s.hopSeconds, s.bridgeSeconds, s.maxSemitonesPerSecond);
            limitSlew(line.frames, s.hopSeconds, s.maxSemitonesPerSecond);
        }

        bool isMeasured(const Frame& frame) noexcept
        {
            return frame.hz > 0.0f && !frame.bridged;
        }

        // Least-squares slope (octaves per frame) of the measured frames next to an end of
        // a gap: `from` is the frame at the edge, `direction` -1 looks back, +1 ahead.
        // Returns false when fewer than two measured frames are there in a row.
        bool edgeSlope(const std::vector<Frame>& frames, int from, int direction, int reach, double& slope)
        {
            const int numFrames = static_cast<int>(frames.size());
            double sumT = 0.0;
            double sumY = 0.0;
            double sumTT = 0.0;
            double sumTY = 0.0;
            int m = 0;

            for (int k = 0; k <= reach; ++k)
            {
                const int f = from + direction * k;

                if (f < 0 || f >= numFrames || !isMeasured(frames[static_cast<std::size_t>(f)]))
                    break;

                const double t = static_cast<double>(direction * k);
                const double y = std::log2(static_cast<double>(frames[static_cast<std::size_t>(f)].hz));
                sumT += t;
                sumY += y;
                sumTT += t * t;
                sumTY += t * y;
                ++m;
            }

            if (m < 2)
                return false;

            const double spread = sumTT - sumT * sumT / m;

            if (!(spread > 0.0))
                return false;

            slope = (sumTY - sumT * sumY / m) / spread;
            return true;
        }

        // Fills frames [f, g) between the pitched frames f - 1 and g, in log-frequency.
        void fillGap(std::vector<Frame>& frames, int f, int g, bool curved, int slopeReach)
        {
            const double from = std::log2(static_cast<double>(frames[static_cast<std::size_t>(f) - 1].hz));
            const double to = std::log2(static_cast<double>(frames[static_cast<std::size_t>(g)].hz));
            const int steps = g - f + 1; // frames from one end to the other
            const double chord = (to - from) / steps;
            double startSlope = chord;
            double endSlope = chord;

            if (curved)
            {
                // A long gap inside a ramp (a run-up, a slide) keeps going the way the sound
                // was going on either side, instead of cutting the corner with a straight line.
                if (!edgeSlope(frames, f - 1, -1, slopeReach, startSlope))
                    startSlope = chord;

                if (!edgeSlope(frames, g, 1, slopeReach, endSlope))
                    endSlope = chord;
            }

            const double lowest = std::min(from, to) - c_bridgeOvershoot;
            const double highest = std::max(from, to) + c_bridgeOvershoot;

            for (int k = f; k < g; ++k)
            {
                const double u = static_cast<double>(k - f + 1) / steps;
                double y = from + (to - from) * u;

                if (curved)
                {
                    // Cubic Hermite: the ends' values and slopes (slopes per frame, times the span).
                    const double u2 = u * u;
                    const double u3 = u2 * u;
                    y = (2.0 * u3 - 3.0 * u2 + 1.0) * from
                      + (u3 - 2.0 * u2 + u) * steps * startSlope
                      + (-2.0 * u3 + 3.0 * u2) * to
                      + (u3 - u2) * steps * endSlope;
                    y = std::clamp(y, lowest, highest);
                }

                Frame& frame = frames[static_cast<std::size_t>(k)];
                frame.hz = static_cast<float>(std::exp2(y));
                frame.confidence = 0.0f;
                frame.bridged = true;
            }
        }

        int countLeaps(const std::vector<Frame>& frames)
        {
            int leaps = 0;

            for (std::size_t i = 1; i < frames.size(); ++i)
            {
                const float a = frames[i - 1].hz;
                const float b = frames[i].hz;

                if (a > 0.0f && b > 0.0f && std::abs(std::log2(b / a)) > 0.25f)
                    ++leaps;
            }

            return leaps;
        }

        //==============================================================================
        // Auto: which line to trust
        //==============================================================================
        //
        // The evidence is in the audio, read off Engine's whitened spectrogram:
        //
        //   Where the Tonal line steps by more than a semitone, either the sound changed
        //   pitch (a note, a gear shift) or the line hopped to another partial of a sound
        //   that carried on. Seen from the line, the partials around it keep their places
        //   in the first case and all move by the size of the step in the second.
        //
        //   Undoing the hops leaves a steadied Tonal line that stays on one partial. Where
        //   it has holes, Engine's contour is borrowed between two of its stretches when
        //   it gets from one to the other within a semitone.
        //
        //   YIN measures a period in every frame and Engine matches a pattern of partials,
        //   so the two do not make the same mistakes: an Engine line that keeps one
        //   frequency ratio to the steadied line's own frames is on the same contour.
        //
        // What Auto does with that is in analyse().

        constexpr int    c_spreadReach = 2 * c_binsPerOctave;  // a pattern spans two octaves either side of the line
        constexpr int    c_spreadBins = 2 * c_spreadReach + 1;
        constexpr int    c_spreadSlack = 3;                    // bins (19 cents) either way when looking for an unmoved pattern
        constexpr int    c_hopSlack = 12;                       // bins (75 cents) around the step when looking for a moved one
        constexpr int    c_ridgeReach = 8;                      // bins (50 cents) a moved stretch may be nudged onto its ridge
        constexpr double c_anchorSeconds = 0.300;               // a stretch of Tonal line this long without a step is kept
        constexpr double c_stepGuardSeconds = 0.100;            // frames this close to a step still hear both sides of it
        constexpr double c_stepPatternSeconds = 0.300;          // length of line a pattern is averaged over
        constexpr double c_linkSeconds = 0.150;                 // stretches this close are compared along their own slope ...
        constexpr double c_engineLinkSeconds = 3.0;             // ... further apart, up to this, along Engine's contour
        constexpr double c_extendSeconds = 2.5;                 // how far Engine's contour may carry the line on towards silence
        constexpr double c_hopMargin = 0.05;                    // a hop must match this much better than a real step, and the reverse
        constexpr double c_realSmallest = 0.25;                 // octaves: Engine can follow smaller steps, so they say nothing against it
        constexpr double c_realMatch = 0.5;                     // a real step leaves the pattern around the line this well in place
        constexpr double c_agreeCents = 35.0;                   // Engine may wander this far from its ratio to the steadied line
        constexpr double c_rangeSlack = 1.0 / 24.0;             // octaves: a quarter tone outside the search range still counts as inside
        constexpr double c_pieceInRange = 0.995;                // a piece of the steadied line is placed where this share of it is inside the search range
        constexpr double c_engineJudged = 0.30;                 // share of the audible frames the steadied line must cover to judge Engine
        constexpr double c_steadyCovers = 0.80;                 // share of the audible frames a whole steadied line has a pitch on
        constexpr double c_steadyKeeps = 0.80;                  // the steadied line must keep this share of the Tonal line's pitched frames
        constexpr double c_steadyJudges = 0.50;                 // share of the Tonal line that must lie in steady stretches before its steps say anything
        constexpr double c_tonalHoles = 0.80;                   // a Tonal line pitched on less than this share of the audible frames has holes
        constexpr float  c_holesOnPartial = 0.9f;               // ... which Engine may fill when its line is on a partial for this share of them

        // Mean whitened level at fixed musical distances from a line, over some of its frames.
        struct Spread
        {
            std::array<float, c_spreadBins> sum {};
            std::array<int, c_spreadBins> count {};
            int frames = 0;
        };

        void addToSpread(const GridView& view, const std::vector<Frame>& frames, int from, int to, Spread& pattern)
        {
            for (int f = from; f <= to; ++f)
            {
                const float hz = frames[static_cast<std::size_t>(f)].hz;

                if (hz <= 0.0f)
                    continue;

                const int centre = static_cast<int>(std::lround(view.binOf(hz)));

                for (int d = std::max(-c_spreadReach, -centre); d <= std::min(c_spreadReach, view.bins - 1 - centre); ++d)
                {
                    pattern.sum[static_cast<std::size_t>(d + c_spreadReach)] += view.grid->cell(f, centre + d);
                    ++pattern.count[static_cast<std::size_t>(d + c_spreadReach)];
                }

                ++pattern.frames;
            }
        }

        // Correlation of `after` with `before` read `shift` bins higher, over the distances
        // both measured in at least half of their frames.
        double spreadMatch(const Spread& before, const Spread& after, int shift)
        {
            double n = 0.0;
            double sx = 0.0;
            double sy = 0.0;
            double sxx = 0.0;
            double syy = 0.0;
            double sxy = 0.0;

            for (int i = 0; i < c_spreadBins; ++i)
            {
                const int j = i + shift;

                if (j < 0 || j >= c_spreadBins)
                    continue;

                const auto a = static_cast<std::size_t>(j);
                const auto b = static_cast<std::size_t>(i);

                if (2 * before.count[a] < before.frames || 2 * after.count[b] < after.frames
                    || before.count[a] == 0 || after.count[b] == 0)
                    continue;

                const double x = static_cast<double>(before.sum[a]) / before.count[a];
                const double y = static_cast<double>(after.sum[b]) / after.count[b];
                n += 1.0;
                sx += x;
                sy += y;
                sxx += x * x;
                syy += y * y;
                sxy += x * y;
            }

            if (n < c_binsPerOctave)
                return -1.0; // less than an octave in common: nothing to go on

            const double vx = sxx - sx * sx / n;
            const double vy = syy - sy * sy / n;
            return vx > 1e-9 && vy > 1e-9 ? (sxy - sx * sy / n) / std::sqrt(vx * vy) : -1.0;
        }

        // The best spreadMatch within `slack` bins of `shift`, and where it was (the nearest to `shift` on a tie).
        double bestSpreadMatch(const Spread& before, const Spread& after, int shift, int slack, int& at)
        {
            double best = -2.0;
            at = shift;

            for (int k = 0; k <= slack; ++k)
            {
                for (const int sign : { 1, -1 })
                {
                    if (k == 0 && sign < 0)
                        continue;

                    const double match = spreadMatch(before, after, shift + sign * k);

                    if (match > best)
                    {
                        best = match;
                        at = shift + sign * k;
                    }
                }
            }

            return best;
        }

        struct Steadied
        {
            std::vector<Frame> frames;                // the Tonal line with its hops undone, gaps not yet bridged
            std::vector<Frame> own;                   // the same before anything was borrowed from Engine
            int hops = 0;                             // steps where the line changed partial
            int real = 0;                             // steps where the sound changed pitch
        };

        // Rebuilds the Tonal line from its stretches of at least c_anchorSeconds without a
        // step. From one stretch to the next the line is expected to carry on: along its
        // own slope across a short gap, along Engine's contour across a longer one. A step
        // away from that which the partials did not make is undone, so the later stretch
        // continues the earlier one. Fragments between stretches are left out; the gaps
        // that Engine's contour spans from one end to the other within a semitone are
        // filled along it, the rest is left for the bridging.
        bool steadyTonal(const GridView& view, const std::vector<Frame>& tonal, const std::vector<Frame>& engine,
                         const Settings& s, const std::atomic<bool>* cancel, Steadied& out)
        {
            const int numFrames = static_cast<int>(tonal.size());
            std::vector<Frame> raw = tonal;
            clearBridges(raw);

            const int minAnchor = std::max(2, static_cast<int>(std::lround(c_anchorSeconds / s.hopSeconds)));
            const int guard = std::max(0, static_cast<int>(std::lround(c_stepGuardSeconds / s.hopSeconds)));
            const int reach = std::max(4, static_cast<int>(std::lround(c_stepPatternSeconds / s.hopSeconds)));
            const int nearLink = std::max(1, static_cast<int>(std::lround(c_linkSeconds / s.hopSeconds)));
            const int farLink = std::max(nearLink, static_cast<int>(std::lround(c_engineLinkSeconds / s.hopSeconds)));
            const int slopeReach = std::max(2, static_cast<int>(std::lround(c_bridgeSlopeSeconds / s.hopSeconds)));
            const int extend = std::max(0, static_cast<int>(std::lround(c_extendSeconds / s.hopSeconds)));
            const double lowest = std::log2(s.lowestHz) - c_rangeSlack;
            const double highest = std::log2(s.highestHz) + c_rangeSlack;

            auto log2Of = [](const std::vector<Frame>& frames, int f)
            {
                return std::log2(static_cast<double>(frames[static_cast<std::size_t>(f)].hz));
            };

            struct Anchor
            {
                int first = 0;
                int last = 0;
                double lift = 0.0;    // octaves added to the Tonal line over this stretch
                int piece = 0;
                bool joined = false;  // carries on from the stretch before it
            };

            std::vector<Anchor> anchors;

            for (int f = 0; f < numFrames;)
            {
                if (raw[static_cast<std::size_t>(f)].hz <= 0.0f)
                {
                    ++f;
                    continue;
                }

                int g = f;

                while (g + 1 < numFrames && raw[static_cast<std::size_t>(g) + 1].hz > 0.0f
                       && std::abs(log2Of(raw, g + 1) - log2Of(raw, g)) <= c_jumpOctaves)
                    ++g;

                if (g - f + 1 >= minAnchor)
                    anchors.push_back({ f, g, 0.0, 0, false });

                f = g + 1;
            }

            out.frames = raw;

            for (auto& frame : out.frames)
            {
                frame.hz = 0.0f;
                frame.confidence = 0.0f;
            }

            int piece = 0;

            for (std::size_t k = 1; k < anchors.size(); ++k)
            {
                if (isCancelled(cancel))
                    return false;

                const Anchor& a = anchors[k - 1];
                Anchor& b = anchors[k];
                const int apart = b.first - a.last;
                bool linked = apart - 1 <= farLink;

                for (int f = a.last + 1; f < b.first && linked; ++f)
                    linked = !raw[static_cast<std::size_t>(f)].silent;

                const bool near = apart - 1 <= nearLink;
                const bool engineSpans = engine[static_cast<std::size_t>(a.last)].hz > 0.0f && engine[static_cast<std::size_t>(b.first)].hz > 0.0f;

                if (!linked || (!near && !engineSpans))
                {
                    b.piece = ++piece;
                    continue;
                }

                // Where the later stretch should start if the line simply carried on.
                double carried = 0.0;

                if (near)
                {
                    double slopeA = 0.0;
                    double slopeB = 0.0;
                    const bool hasA = edgeSlope(raw, a.last, -1, slopeReach, slopeA);
                    const bool hasB = edgeSlope(raw, b.first, 1, slopeReach, slopeB);
                    carried = (hasA && hasB ? 0.5 * (slopeA + slopeB) : hasA ? slopeA : hasB ? slopeB : 0.0) * apart;
                }
                else
                {
                    carried = log2Of(engine, b.first) - log2Of(engine, a.last);
                }

                const double step = log2Of(raw, b.first) - log2Of(raw, a.last) - carried;
                bool hop = false;
                bool real = false;
                int found = 0;

                if (std::abs(step) > c_jumpOctaves)
                {
                    Spread before;
                    Spread after;
                    int to = a.last - guard;

                    if (to - a.first + 1 < minAnchor / 2)
                        to = a.last;

                    addToSpread(view, raw, std::max(a.first, to - reach + 1), to, before);

                    int from = b.first + guard;

                    if (b.last - from + 1 < minAnchor / 2)
                        from = b.first;

                    addToSpread(view, raw, from, std::min(b.last, from + reach - 1), after);

                    // If the line hopped, the partials are found moved by the step, and where
                    // exactly they are found is the size of the hop to undo.
                    int same = 0;
                    const int shift = static_cast<int>(std::lround(step * c_binsPerOctave));
                    const int slack = std::clamp(std::abs(shift) - c_spreadSlack - 1, 0, c_hopSlack);
                    const double asHop = bestSpreadMatch(before, after, shift, slack, found);
                    const double asReal = bestSpreadMatch(before, after, 0, c_spreadSlack, same);
                    hop = asHop > asReal + c_hopMargin;
                    real = std::abs(step) >= c_realSmallest && asReal > asHop + c_hopMargin && asReal >= c_realMatch;
                }

                const bool carriedOn = std::abs(step) <= c_jumpOctaves;

                if (!near && !hop && !carriedOn)
                {
                    // Too far apart to tell a change of pitch from a contour that lost its way,
                    // unless the patterns show the sound itself made the step.
                    out.real += real ? 1 : 0;
                    b.piece = ++piece;
                    continue;
                }

                b.piece = piece;
                b.lift = hop ? a.lift - static_cast<double>(found) / c_binsPerOctave : a.lift;

                if (!carriedOn && !real)
                {
                    // The line keeps coming back to the few partials it knows: a lift already
                    // used in this piece that lets the later stretch carry on is taken, also
                    // where the patterns were too blurred to show a hop.
                    const double wanted = a.lift - step;
                    double nearest = c_jumpOctaves;

                    for (std::size_t j = k; j-- > 0 && anchors[j].piece == piece;)
                    {
                        if (std::abs(anchors[j].lift - wanted) < nearest)
                        {
                            nearest = std::abs(anchors[j].lift - wanted);
                            b.lift = anchors[j].lift;
                            hop = true;
                        }
                    }
                }

                b.joined = hop || carriedOn;
                out.hops += hop ? 1 : 0;
                out.real += real ? 1 : 0;
            }

            // Each piece keeps the Tonal line where it spent the longest and moves the rest
            // to it, unless that takes the line out of the search range: then the place it
            // visited that keeps the most of the piece in range, moved by octaves if need be.
            for (std::size_t k = 0; k < anchors.size();)
            {
                if (isCancelled(cancel))
                    return false;

                std::size_t end = k;

                while (end < anchors.size() && anchors[end].piece == anchors[k].piece)
                    ++end;

                int total = 0;

                for (std::size_t i = k; i < end; ++i)
                    total += anchors[i].last - anchors[i].first + 1;

                auto insideAt = [&](double lift)
                {
                    int inside = 0;

                    for (std::size_t i = k; i < end; ++i)
                    {
                        for (int f = anchors[i].first; f <= anchors[i].last; ++f)
                        {
                            const double y = log2Of(raw, f) + anchors[i].lift - lift;
                            inside += y >= lowest && y <= highest ? 1 : 0;
                        }
                    }

                    return inside;
                };

                double bestLift = 0.0;
                int bestFrames = -1;
                int bestInside = -1;
                const int enough = static_cast<int>(std::ceil(c_pieceInRange * total));

                for (std::size_t i = k; i < end; ++i)
                {
                    bool seen = false;

                    for (std::size_t j = k; j < i && !seen; ++j)
                        seen = std::abs(anchors[j].lift - anchors[i].lift) <= 0.5 * c_jumpOctaves;

                    if (seen)
                        continue;

                    int frames = 0;

                    for (std::size_t j = k; j < end; ++j)
                        if (std::abs(anchors[j].lift - anchors[i].lift) <= 0.5 * c_jumpOctaves)
                            frames += anchors[j].last - anchors[j].first + 1;

                    const int inside = std::min(enough, insideAt(anchors[i].lift));

                    if (inside > bestInside || (inside == bestInside && frames > bestFrames))
                    {
                        bestInside = inside;
                        bestFrames = frames;
                        bestLift = anchors[i].lift;
                    }
                }

                if (bestInside < enough)
                {
                    const double placed = bestLift;

                    for (const int octaves : { 1, -1, 2, -2 })
                    {
                        const int inside = std::min(enough, insideAt(placed - octaves));

                        if (inside > bestInside)
                        {
                            bestInside = inside;
                            bestLift = placed - octaves;
                        }
                    }
                }

                for (std::size_t i = k; i < end; ++i)
                {
                    // Stretches that come back to the kept place are the Tonal line itself there.
                    const bool atKeptPlace = std::abs(anchors[i].lift - bestLift) <= 0.5 * c_jumpOctaves;

                    if (atKeptPlace)
                        anchors[i].lift = bestLift;

                    // A moved stretch should lie along a partial: settle it on the ridge next to it.
                    if (!atKeptPlace)
                    {
                        double bestLevel = -1.0;
                        int bestNudge = 0;

                        for (int nudge = -c_ridgeReach; nudge <= c_ridgeReach; ++nudge)
                        {
                            double level = 0.0;

                            for (int f = anchors[i].first; f <= anchors[i].last; ++f)
                                level += view.grid->at(f, view.binOf(raw[static_cast<std::size_t>(f)].hz)
                                                              + (anchors[i].lift - bestLift) * c_binsPerOctave + nudge);

                            level *= 1.0 - 0.25 * std::abs(nudge) / c_ridgeReach;

                            if (level > bestLevel)
                            {
                                bestLevel = level;
                                bestNudge = nudge;
                            }
                        }

                        anchors[i].lift += static_cast<double>(bestNudge) / c_binsPerOctave;
                    }

                    for (int f = anchors[i].first; f <= anchors[i].last; ++f)
                    {
                        // What still ends up outside the search range stays unpitched.
                        const double y = log2Of(raw, f) + anchors[i].lift - bestLift;

                        if (y < lowest || y > highest)
                            continue;

                        Frame& frame = out.frames[static_cast<std::size_t>(f)];
                        frame = raw[static_cast<std::size_t>(f)];
                        frame.hz = static_cast<float>(std::exp2(y));
                    }
                }

                k = end;
            }

            out.own = out.frames;

            // Frames of a gap, along Engine's contour from `from` (a steadied frame), bent
            // evenly by `bend` octaves over the way to `to`.
            auto borrow = [&](int from, int to, double bend)
            {
                const int direction = to > from ? 1 : -1;
                const double start = log2Of(out.frames, from) - log2Of(engine, from);

                for (int f = from + direction; f != to; f += direction)
                {
                    const Frame& source = engine[static_cast<std::size_t>(f)];
                    Frame& frame = out.frames[static_cast<std::size_t>(f)];

                    if (source.hz <= 0.0f || frame.silent)
                        continue;

                    const double y = start + log2Of(engine, f) + bend * (f - from) / (to - from);

                    if (y < lowest || y > highest)
                        continue;

                    frame.hz = static_cast<float>(std::exp2(y));
                    frame.confidence = source.confidence;
                    frame.bridged = false;
                }
            };

            for (std::size_t k = 0; k < anchors.size(); ++k)
            {
                const Anchor& b = anchors[k];
                const bool firstOfPiece = k == 0 || anchors[k - 1].piece != b.piece;
                const bool lastOfPiece = k + 1 == anchors.size() || anchors[k + 1].piece != b.piece;

                if (!firstOfPiece && b.joined && b.first - anchors[k - 1].last > 1)
                {
                    const int from = anchors[k - 1].last;

                    if (out.frames[static_cast<std::size_t>(from)].hz > 0.0f && out.frames[static_cast<std::size_t>(b.first)].hz > 0.0f
                        && engine[static_cast<std::size_t>(from)].hz > 0.0f && engine[static_cast<std::size_t>(b.first)].hz > 0.0f)
                    {
                        const double bend = log2Of(out.frames, b.first) - log2Of(out.frames, from)
                                          - (log2Of(engine, b.first) - log2Of(engine, from));

                        if (std::abs(bend) <= c_jumpOctaves)
                            borrow(from, b.first, bend);
                    }
                }

                // Towards silence, or the ends of the buffer, there is nothing to meet: carry on a little way.
                auto reachOut = [&](int from, int direction)
                {
                    if (out.frames[static_cast<std::size_t>(from)].hz <= 0.0f || engine[static_cast<std::size_t>(from)].hz <= 0.0f)
                        return;

                    int to = from;

                    while (std::abs(to + direction - from) <= extend && to + direction >= 0 && to + direction < numFrames
                           && !raw[static_cast<std::size_t>(to + direction)].silent
                           && engine[static_cast<std::size_t>(to + direction)].hz > 0.0f
                           && out.frames[static_cast<std::size_t>(to + direction)].hz <= 0.0f)
                        to += direction;

                    // Only when it gets there: the walk stops at any frame that already has a pitch, so
                    // a stretch of another piece in the way is left alone.
                    const int beyond = to + direction;
                    const bool open = beyond < 0 || beyond >= numFrames || raw[static_cast<std::size_t>(beyond)].silent;

                    if (open && to != from)
                        borrow(from, beyond, 0.0);
                };

                if (firstOfPiece)
                    reachOut(b.first, -1);

                if (lastOfPiece)
                    reachOut(b.last, 1);
            }

            return true;
        }

        // Share of the frames pitched in both lines where Engine keeps one frequency ratio
        // (within c_agreeCents) to the steadied line: the most that any one ratio covers.
        // `judged` is the number of frames that could be compared. Only the steadied
        // line's own frames are passed in: the ones borrowed from Engine would agree
        // with it whatever it did.
        float ratioAgreement(const std::vector<Frame>& engine, const std::vector<Frame>& steady, int& judged)
        {
            std::vector<float> ratios;

            for (std::size_t f = 0; f < engine.size() && f < steady.size(); ++f)
                if (engine[f].hz > 0.0f && steady[f].hz > 0.0f)
                    ratios.push_back(std::log2(engine[f].hz / steady[f].hz));

            std::sort(ratios.begin(), ratios.end());
            const float width = static_cast<float>(2.0 * c_agreeCents / 1200.0);
            std::size_t most = 0;

            for (std::size_t i = 0, j = 0; i < ratios.size(); ++i)
            {
                while (j < ratios.size() && ratios[j] - ratios[i] <= width)
                    ++j;

                most = std::max(most, j - i);
            }

            judged = static_cast<int>(ratios.size());
            return judged > 0 ? static_cast<float>(most) / static_cast<float>(judged) : 0.0f;
        }
    } // namespace

    //==============================================================================
    // Finishing
    //==============================================================================

    void clearBridges(std::vector<Frame>& frames)
    {
        for (auto& frame : frames)
        {
            if (frame.bridged)
            {
                frame.hz = 0.0f;
                frame.confidence = 0.0f;
                frame.bridged = false;
            }
        }
    }

    void bridgeGaps(std::vector<Frame>& frames, double hopSeconds, double bridgeSeconds, double maxSemitonesPerSecond)
    {
        if (!(hopSeconds > 0.0))
            return;

        const int numFrames = static_cast<int>(frames.size());
        const int gapLimit = bridgeSeconds > 0.0 ? static_cast<int>(std::lround(std::min(bridgeSeconds, 3600.0) / hopSeconds)) : 0;
        const int straightLimit = static_cast<int>(std::lround(c_straightBridgeSeconds / hopSeconds));
        const int slopeReach = std::max(1, static_cast<int>(std::lround(c_bridgeSlopeSeconds / hopSeconds)));
        int f = 0;

        while (f < numFrames)
        {
            if (frames[static_cast<std::size_t>(f)].hz > 0.0f)
            {
                ++f;
                continue;
            }

            int g = f;
            bool crossesSilence = false;

            while (g < numFrames && !(frames[static_cast<std::size_t>(g)].hz > 0.0f))
            {
                crossesSilence = crossesSilence || frames[static_cast<std::size_t>(g)].silent;
                ++g;
            }

            // Only audible gaps with pitch on both sides are filled: a silent frame is a
            // boundary between notes, however short. A gap is filled when it is short, or
            // (with a slew limit) whenever the straight line across it stays within the limit.
            if (f > 0 && g < numFrames && !crossesSilence)
            {
                const double octaves = std::log2(static_cast<double>(frames[static_cast<std::size_t>(g)].hz)
                                                 / static_cast<double>(frames[static_cast<std::size_t>(f) - 1].hz));
                const bool withinSlew = maxSemitonesPerSecond > 0.0
                    && std::abs(12.0 * octaves) / ((g - f + 1) * hopSeconds) <= maxSemitonesPerSecond;

                if (g - f <= gapLimit)
                    fillGap(frames, f, g, g - f > straightLimit, slopeReach);
                else if (withinSlew)
                    fillGap(frames, f, g, false, slopeReach);
            }

            f = g;
        }
    }

    void limitSlew(std::vector<Frame>& frames, double hopSeconds, double maxSemitonesPerSecond)
    {
        if (maxSemitonesPerSecond <= 0.0)
            return;

        // Clamp the per-frame change in log-frequency in a forward pass and a backward
        // pass and take the mean of both, which keeps the clamp from delaying the
        // contour on either side.
        const double step = maxSemitonesPerSecond / 12.0 * hopSeconds;
        const std::size_t n = frames.size();
        std::vector<double> forwards(n, 0.0);
        std::vector<double> backwards(n, 0.0);

        auto pass = [&](std::vector<double>& y, bool ahead)
        {
            double last = 0.0;
            bool have = false;

            for (std::size_t k = 0; k < n; ++k)
            {
                const std::size_t i = ahead ? k : n - 1 - k;
                const float hz = frames[i].hz;

                if (!(hz > 0.0f))
                {
                    have = false;
                    y[i] = 0.0;
                    continue;
                }

                double v = std::log2(hz);

                if (have)
                    v = std::clamp(v, last - step, last + step);

                y[i] = v;
                last = v;
                have = true;
            }
        };

        pass(forwards, true);
        pass(backwards, false);

        for (std::size_t i = 0; i < n; ++i)
            if (frames[i].hz > 0.0f)
                frames[i].hz = static_cast<float>(std::exp2(0.5 * (forwards[i] + backwards[i])));
    }

    Summary summarise(const std::vector<Frame>& frames)
    {
        Summary result;
        double sumLog2 = 0.0;
        double sumConfidence = 0.0;
        int pitched = 0;
        int measured = 0;
        int silent = 0;
        int bridged = 0;
        result.lowestHz = std::numeric_limits<float>::max();

        for (std::size_t i = 0; i < frames.size(); ++i)
        {
            const Frame& frame = frames[i];
            silent += frame.silent ? 1 : 0;

            if (!(frame.hz > 0.0f))
                continue;

            result.lowestHz = std::min(result.lowestHz, frame.hz);
            result.highestHz = std::max(result.highestHz, frame.hz);
            sumLog2 += std::log2(frame.hz);
            ++pitched;

            if (frame.bridged)
            {
                ++bridged;
            }
            else
            {
                sumConfidence += frame.confidence;
                ++measured;
            }

            if (i > 0 && frames[i - 1].hz > 0.0f
                && std::abs(std::log2(static_cast<double>(frame.hz) / static_cast<double>(frames[i - 1].hz))) > c_jumpOctaves)
                ++result.jumps;
        }

        if (pitched == 0)
            result.lowestHz = 0.0f;

        const float total = frames.empty() ? 1.0f : static_cast<float>(frames.size());
        result.logMeanHz = pitched > 0 ? static_cast<float>(std::exp2(sumLog2 / pitched)) : 0.0f;
        result.pitchedShare = static_cast<float>(pitched) / total;
        result.silentShare = static_cast<float>(silent) / total;
        result.bridgedShare = static_cast<float>(bridged) / total;
        result.meanConfidence = measured > 0 ? static_cast<float>(sumConfidence / measured) : 0.0f;
        return result;
    }

    float hzAt(const std::vector<Frame>& frames, double hopSeconds, double seconds) noexcept
    {
        if (frames.empty() || !(hopSeconds > 0.0))
            return 0.0f;

        const double pos = seconds / hopSeconds;

        if (!(pos >= 0.0) || pos > static_cast<double>(frames.size() - 1))
            return 0.0f;

        const auto i = static_cast<std::size_t>(pos);
        const auto j = std::min(i + 1, frames.size() - 1);
        const float a = frames[i].hz > 0.0f ? frames[i].hz : 0.0f;
        const float b = frames[j].hz > 0.0f ? frames[j].hz : 0.0f;

        if (a <= 0.0f || b <= 0.0f)
            return (pos - static_cast<double>(i) < 0.5) ? a : b;

        const double frac = pos - static_cast<double>(i);
        return static_cast<float>(std::exp2((1.0 - frac) * std::log2(a) + frac * std::log2(b)));
    }

    //==============================================================================
    // Entry point
    //==============================================================================

    Analysis analyse(const float* mono, int numSamples, double sampleRate, const Settings& settings,
                     const std::atomic<bool>* cancel, const Progress& progress)
    {
        Analysis result;
        result.usedMethod = settings.method == Method::Engine ? Method::Engine : Method::Tonal;
        result.hopSeconds = settings.hopSeconds;
        result.durationSeconds = sampleRate > 0.0 && std::isfinite(sampleRate) && numSamples > 0 ? numSamples / sampleRate : 0.0;

        if (mono == nullptr || numSamples <= 0 || !(sampleRate > 0.0) || !std::isfinite(sampleRate)
            || !(settings.hopSeconds > 0.0) || !std::isfinite(settings.hopSeconds))
            return result;

        const Settings s = sanitised(settings);
        result.hopSeconds = s.hopSeconds;

        auto stopped = [&]
        {
            Analysis empty;
            empty.usedMethod = result.usedMethod;
            empty.hopSeconds = result.hopSeconds;
            empty.durationSeconds = result.durationSeconds;
            empty.cancelled = true;
            return empty;
        };

        // Shares of the progress bar: decimation (when needed), then the methods, then finishing.
        const bool both = s.method == Method::Auto;
        const Stage prepareStage { &progress, 0.0f, 0.05f };
        const Stage tonalStage { &progress, 0.05f, both ? 0.45f : 0.9f };
        const Stage secondStage { &progress, both ? 0.45f : 0.05f, 0.9f };

        Signal x;

        if (!prepare(mono, numSamples, sampleRate, s.workingRate, x, cancel, prepareStage))
            return stopped();

        // Silence: absolute (the caller's threshold), and optionally relative to the peak.
        float peak = 0.0f;

        for (int i = 0; i < x.length; ++i)
            peak = std::max(peak, std::abs(x.data[i]));

        const double thresholdRms = std::max(gainFromDb(s.silenceDbfs), peak * gainFromDb(s.gateDb));
        const int numFrames = std::max(1, static_cast<int>(std::ceil(result.durationSeconds / s.hopSeconds)));
        std::vector<std::uint8_t> silent;

        if (!findSilence(x, numFrames, s.hopSeconds, thresholdRms, cancel, silent))
            return stopped();

        auto tonalLine = [&](bool voting, const Stage& stage, Line& line)
        {
            line.frames.assign(static_cast<std::size_t>(numFrames), Frame {});

            if (!runTonal(x, silent, s, voting, cancel, stage, line.frames))
                return false;

            finishLine(line, silent, s);
            stage(1.0f);
            return true;
        };

        auto engineLine = [&](const Stage& stage, Line& line, std::vector<Line>& parallel, EngineReport& report, const GridUse& useGrid = {})
        {
            line.frames.assign(static_cast<std::size_t>(numFrames), Frame {});

            if (!runEngine(x, silent, s, cancel, stage, line, parallel, report, useGrid))
                return false;

            finishLine(line, silent, s);

            for (auto& other : parallel)
                finishLine(other, silent, s);

            result.engineOnGrid = report.trusted;
            result.engineConfidence = report.partialShare;
            result.enginePatternMatch = report.patternMatch;
            result.engineBreaks = report.breaks;
            stage(1.0f);
            return true;
        };

        switch (s.method)
        {
            case Method::Tonal:
            {
                if (!tonalLine(s.thresholdVoting, tonalStage, result.line))
                    return stopped();

                break;
            }

            case Method::Engine:
            {
                EngineReport report;

                if (!engineLine(secondStage, result.line, result.parallel, report))
                    return stopped();

                break;
            }

            case Method::Auto:
            {
                // Decide with plain dip depth: the partial-to-partial hops it produces on an
                // engine are the evidence, and threshold voting tends to hide them.
                Line tonal;

                if (!tonalLine(false, tonalStage, tonal))
                    return stopped();

                const int leaps = countLeaps(tonal.frames);
                const Summary heard = summarise(tonal.frames);
                result.tonalLeaps = leaps;

                // A Tonal line without a step in it and without a hole leaves nothing to decide.
                if (leaps == 0 && heard.jumps == 0 && heard.pitchedShare + heard.silentShare >= 0.999f)
                {
                    if (s.thresholdVoting && !tonalLine(true, secondStage, tonal))
                        return stopped();

                    result.usedMethod = Method::Tonal;
                    result.line = std::move(tonal);
                    break;
                }

                Line engine;
                Line steadied;
                std::vector<Line> parallel;
                EngineReport report;
                int hops = 0;
                int realSteps = 0;
                int judged = 0;
                int ownFrames = 0;
                float agreement = 0.0f;

                // The spectrogram only lives while Engine runs, so the evidence is gathered then.
                const GridUse weigh = [&](const GridView& view)
                {
                    Steadied steady;

                    if (!steadyTonal(view, tonal.frames, engine.frames, s, cancel, steady))
                        return false;

                    agreement = ratioAgreement(engine.frames, steady.own, judged);
                    hops = steady.hops;
                    realSteps = steady.real;

                    for (const auto& frame : steady.own)
                        ownFrames += frame.hz > 0.0f ? 1 : 0;

                    steadied.frames = std::move(steady.frames);
                    return true;
                };

                if (!engineLine(secondStage, engine, parallel, report, weigh))
                    return stopped();

                finishLine(steadied, silent, s);

                int audible = 0;
                int steadyPitched = 0;
                int tonalPitched = 0;

                for (std::size_t f = 0; f < silent.size(); ++f)
                {
                    audible += silent[f] == 0 ? 1 : 0;
                    steadyPitched += steadied.frames[f].hz > 0.0f ? 1 : 0;
                    tonalPitched += tonal.frames[f].hz > 0.0f ? 1 : 0;
                }

                // What the steps of the Tonal line say about the sound:
                //   only real steps    notes (a bass line, a melody): the Tonal pass as it stands;
                //   no step judged     with no leap, a held or gliding sound: the Tonal pass as it
                //                      stands, unless it has holes that a well-locked Engine line
                //                      fills. With leaps but too little steady line to judge them
                //                      (fast notes), the Tonal pass as it stands too;
                //   hops               a set of partials that carries on while the Tonal line
                //                      changes partial (an engine, a drone, a growl). The steadied
                //                      line is used when it is whole, except that Engine is
                //                      preferred where the two agree, because it comes with the
                //                      other partials to choose from; Engine when the steadied
                //                      line is not whole and Engine's own line can be trusted
                //                      (on a partial for c_trustedShare of the sound); failing
                //                      both, the steadied line if it kept most of the Tonal
                //                      pass, else the Tonal pass.
                const bool notes = realSteps > 0 && hops == 0;
                const bool unjudged = realSteps == 0 && hops == 0;
                const bool steadyWhole = summarise(steadied.frames).jumps <= realSteps && steadyPitched >= c_steadyCovers * audible;
                const bool vouched = judged >= c_engineJudged * audible && agreement >= s.autoCrossCheckShare;
                enum class Use { tonal, steadied, engine };
                Use use = Use::tonal;

                if (!s.autoCrossCheck)
                    use = leaps > 0 ? Use::engine : Use::tonal;
                else if (notes)
                    use = Use::tonal;
                else if (unjudged && leaps == 0)
                    use = tonalPitched < c_tonalHoles * audible && report.trusted && report.partialShare >= c_holesOnPartial ? Use::engine : Use::tonal;
                else if (unjudged && ownFrames < c_steadyJudges * tonalPitched)
                    use = Use::tonal;
                else if (steadyWhole)
                    use = report.trusted && vouched ? Use::engine : Use::steadied;
                else if (report.trusted)
                    use = Use::engine;
                else if (steadyPitched >= c_steadyKeeps * tonalPitched)
                    use = Use::steadied;

                // Reported only where it was measured and, when Engine is used, where the choice rested on it.
                const bool restedOnAgreement = s.autoCrossCheck && !notes && !(unjudged && leaps == 0)
                                               && !(unjudged && ownFrames < c_steadyJudges * tonalPitched) && steadyWhole;
                result.octaveAgreement = judged > 0 && (use != Use::engine || restedOnAgreement) ? agreement : -1.0f;

                if (use == Use::engine)
                {
                    result.usedMethod = Method::Engine;
                    result.line = std::move(engine);
                    result.parallel = std::move(parallel);
                    result.other = std::move(tonal);
                }
                else
                {
                    result.usedMethod = Method::Tonal;
                    result.line = use == Use::steadied ? std::move(steadied) : std::move(tonal);
                    result.other = std::move(engine);
                }

                break;
            }
        }

        result.summary = summarise(result.line.frames);

        // A cancel that arrived while the lines were being finished still wins: the caller
        // asked for nothing, so it gets nothing.
        if (isCancelled(cancel))
            return stopped();

        if (progress)
            progress(1.0f);

        return result;
    }
} // namespace subbertone::offline
