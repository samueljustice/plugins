#include "HindsightTracker.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <utility>

namespace pf::hindsight
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
        // 50 cents of an exhaust run-up's order-4 line). The headroom lets buffers
        // already at 10.7-12.6 kHz through as they are: no second filter, and no copy.
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
        // of 61 MB at the default range). Whitened levels measured under 3 on an exhaust
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

        bool runEngine(const Signal& x, const std::vector<std::uint8_t>& silent, const Settings& s,
                       const std::atomic<bool>* cancel, const Stage& stage, Line& chosen, std::vector<Line>& parallel,
                       bool& onGrid)
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

            stage(0.9f);

            // 4. Vertical placement: every offset in the search range is tried, scoring the
            //    mean whitened level found along the offset contour over audible frames.
            //    Local maxima of that score are the alternative partials (step 6).
            const double offsetLow = std::log2(s.lowestHz / gridLowHz) * c_binsPerOctave;
            const double offsetHigh = std::log2(s.highestHz / gridLowHz) * c_binsPerOctave;

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
            double bestOffset = offsetLow;
            double topScore = -1.0;

            for (double c = offsetLow; c <= offsetHigh; c += 1.0)
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
            // perceived pitch sits one octave above it. So raise the offset by 192 bins (at
            // most three times) while the best score near there still reaches climbShare of
            // the current one.
            for (int i = 0; i < 3; ++i)
            {
                const double up = bestOffset + c_binsPerOctave;
                double higherOffset = up;
                double higherScore = -1.0;

                for (double c = up - 4; c <= up + 4; c += 0.5)
                {
                    if (c <= offsetHigh)
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
                topScore = higherScore;
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

                    const double g = offset + shape[static_cast<std::size_t>(f)] + (m > 0 ? sum / m : 0.0);
                    frame.hz = static_cast<float>(binToHz(g));
                    frame.confidence = static_cast<float>(std::clamp(grid.at(f, g) / 3.0, 0.0, 1.0));
                }
            };

            place(bestOffset, chosen.frames);
            chosen.score = static_cast<float>(topScore);
            chosen.ratio = 1.0f;

            // A line that leaves the grid has drifted (see the header): nothing there was measured.
            onGrid = std::all_of(chosen.frames.begin(), chosen.frames.end(), [&](const Frame& frame)
            {
                return frame.hz <= 0.0f || (frame.hz >= gridLowHz && frame.hz <= gridHighHz);
            });

            // 6. Parallel lines for the multi-curve view: the other peaks of the score,
            //    at least 50 cents from each other and from the chosen line.
            if (s.parallelLines > 0 && scores.size() > 2)
            {
                const int apart = c_binsPerOctave / 24;
                const int last = static_cast<int>(scores.size()) - 1;
                std::vector<int> peaks;

                for (int k = 1; k < last; ++k)
                {
                    if (scores[static_cast<std::size_t>(k)] <= 0.0)
                        continue;

                    bool isPeak = true;

                    for (int j = std::max(0, k - apart); j <= std::min(last, k + apart) && isPeak; ++j)
                    {
                        if (j < k ? scores[static_cast<std::size_t>(j)] >= scores[static_cast<std::size_t>(k)]
                                  : scores[static_cast<std::size_t>(j)] > scores[static_cast<std::size_t>(k)])
                            isPeak = false;
                    }

                    if (isPeak && std::abs(offsets[static_cast<std::size_t>(k)] - bestOffset) > apart)
                        peaks.push_back(k);
                }

                std::stable_sort(peaks.begin(), peaks.end(),
                                 [&](int p, int q) { return scores[static_cast<std::size_t>(p)] > scores[static_cast<std::size_t>(q)]; });

                if (static_cast<int>(peaks.size()) > s.parallelLines)
                    peaks.resize(static_cast<std::size_t>(s.parallelLines));

                for (const int k : peaks)
                {
                    if (isCancelled(cancel))
                        return false;

                    Line line;
                    line.frames.resize(static_cast<std::size_t>(numFrames));
                    place(offsets[static_cast<std::size_t>(k)], line.frames);
                    line.score = static_cast<float>(scores[static_cast<std::size_t>(k)]);
                    line.ratio = static_cast<float>(std::exp2((offsets[static_cast<std::size_t>(k)] - bestOffset) / c_binsPerOctave));
                    parallel.push_back(std::move(line));
                }
            }

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

        // Share of the frames pitched in both lines where `tonal` lies within 50 cents of
        // a whole number of octaves from `engine`.
        float octaveAgreement(const std::vector<Frame>& engine, const std::vector<Frame>& tonal)
        {
            int joint = 0;
            int onOctaves = 0;

            for (std::size_t i = 0; i < engine.size() && i < tonal.size(); ++i)
            {
                const float e = engine[i].hz;
                const float t = tonal[i].hz;

                if (e > 0.0f && t > 0.0f)
                {
                    const double octaves = std::log2(static_cast<double>(t) / static_cast<double>(e));
                    ++joint;
                    onOctaves += std::abs(octaves - std::round(octaves)) * 1200.0 <= 50.0 ? 1 : 0;
                }
            }

            return joint > 0 ? static_cast<float>(onOctaves) / static_cast<float>(joint) : 0.0f;
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

        auto engineLine = [&](const Stage& stage, Line& line, std::vector<Line>& parallel, bool& onGrid)
        {
            line.frames.assign(static_cast<std::size_t>(numFrames), Frame {});

            if (!runEngine(x, silent, s, cancel, stage, line, parallel, onGrid))
                return false;

            finishLine(line, silent, s);

            for (auto& other : parallel)
                finishLine(other, silent, s);

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
                if (!engineLine(secondStage, result.line, result.parallel, result.engineOnGrid))
                    return stopped();

                break;
            }

            case Method::Auto:
            {
                // Decide with plain dip depth: the order-to-order jumps it produces on an
                // engine are the evidence, and threshold voting tends to hide them.
                Line tonal;

                if (!tonalLine(false, tonalStage, tonal))
                    return stopped();

                const int leaps = countLeaps(tonal.frames);
                result.tonalLeaps = leaps;

                if (leaps == 0)
                {
                    if (s.thresholdVoting && !tonalLine(true, secondStage, tonal))
                        return stopped();

                    result.usedMethod = Method::Tonal;
                    result.line = std::move(tonal);
                    break;
                }

                Line engine;
                std::vector<Line> parallel;
                bool onGrid = true;

                if (!engineLine(secondStage, engine, parallel, onGrid))
                    return stopped();

                result.octaveAgreement = octaveAgreement(engine.frames, tonal.frames);
                result.engineOnGrid = onGrid;

                if (s.autoCrossCheck && (!onGrid || result.octaveAgreement < s.autoCrossCheckShare))
                {
                    // Engine does not explain the Tonal pass (a melody, or a drifted line): keep Tonal.
                    result.usedMethod = Method::Tonal;
                    result.line = std::move(tonal);
                    result.other = std::move(engine);
                }
                else
                {
                    result.usedMethod = Method::Engine;
                    result.line = std::move(engine);
                    result.parallel = std::move(parallel);
                    result.other = std::move(tonal);
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
} // namespace pf::hindsight
