#include "LearnAudio.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <complex>
#include <limits>
#include <thread>

namespace subbertone::learn
{
    namespace
    {
        using Guide = SubharmonicEngine::PitchGuide;

        constexpr double c_pi = 3.14159265358979323846;
        constexpr double c_captureTargetHz = 12000.0;  // the decimated rate aimed for
        constexpr float c_maxInput = 1.0e4f;           // as in the engine: louder, NaN and inf count as silence
        constexpr double c_warmSeconds = 0.020;        // after a jump the band filters settle this long
        constexpr std::int64_t c_continuitySlack = 2;  // samples a continuing block may be off by (it resyncs)
        constexpr std::int64_t c_loopEdgeSlack = 2;    // samples a loop's end may be off by
        constexpr std::int64_t c_splitSlack = 16;      // a loop end this close to a block's end counts as its end
        // A pass whose first half second of compared hops differs this often (10 %) is worth an alignment job. A
        // shift of 2 ms already makes about a sixth of an engine's hops differ, scattered, which a stricter test would
        // never notice; a pass that matches differs in none.
        constexpr int c_alignFirstHops = 50;
        constexpr int c_alignDifferHops = 5;
        constexpr double c_alignMinSeconds = 0.5;      // the least audio an alignment job gets
        constexpr double c_maxGapSeconds = 0.050;      // gaps in a loop capture that are filled in
        constexpr double c_maxGapShare = 0.02;         // ... and at most this share of the loop together
        // Hosts that stop calling a plug-in whose input is silent (Pro Tools' Dynamic Plug-In Processing, Cubase's and
        // Nuendo's option to suspend VST3 processing) do so only after its tail (0.15 s here) has rung out: a jump
        // forward after less silence than this is a locate, which skips audio.
        constexpr double c_skipQuietSeconds = 0.100;
        constexpr float c_quietPeak = 3.1622777e-4f;   // -70 dBFS: silent, for that test
        constexpr double c_logSemitone = 0.057762265046662105;  // ln 2 / 12
        constexpr std::int64_t c_farAway = std::numeric_limits<std::int64_t>::max() / 4;

        // The pitch test (pitchDiffers()). Measured on notes of 62-330 Hz moved by 1-3 semitones (sines, sines with a
        // second harmonic, saws): every one caught, 45-75 ms into the note; on unedited bass lines (alone, under a
        // chord, with vibrato), a line of 330-590 Hz, drums and an engine learned as notes, played at the rate they
        // were learned at and at others (44.1-192 kHz), no hop falls back.
        constexpr double c_pitchSearchCents = 350.0;      // lags this far either side of the learned period
        constexpr double c_pitchToleranceCents = 70.0;    // a repeat further from it than this is another pitch ...
        constexpr double c_pitchClearRepeat = 0.10;       // ... if it repeats this closely (normalised difference) ...
        constexpr double c_pitchLearnedMiss = 0.05;       // ... while the learned period repeats at least this badly ...
        constexpr double c_pitchMargin = 0.02;            // ... and this much worse than it
        constexpr double c_pitchCurveLeadSeconds = 0.020; // the window is sized from the curve this long before its end
        constexpr double c_quietMeanSquare = 1.0e-7;      // -70 dBFS: too quiet to say

        // The sum of a[i] b[i], in eight lanes the compiler can keep in vector registers.
        float dot(const float* a, const float* b, int n) noexcept
        {
            std::array<float, 8> lane {};
            int i = 0;

            for (; i + 8 <= n; i += 8)
                for (int k = 0; k < 8; ++k)
                    lane[static_cast<std::size_t>(k)] += a[i + k] * b[i + k];

            float sum = 0.0f;

            for (; i < n; ++i)
                sum += a[i] * b[i];

            for (const float value : lane)
                sum += value;

            return sum;
        }

        int decimationFor(double hostRate) noexcept
        {
            return std::max(1, static_cast<int>(std::lround(hostRate / c_captureTargetHz)));
        }

        std::int64_t floorDiv(std::int64_t a, std::int64_t b) noexcept
        {
            const std::int64_t q = a / b;
            return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
        }

        int floorMod(std::int64_t a, std::int64_t b) noexcept
        {
            return static_cast<int>(a - floorDiv(a, b) * b);
        }

        int roundedDb(double value) noexcept
        {
            return static_cast<int>(std::lround(10.0 * std::log10(value)));
        }

        // Rates and alignments are compared exactly: they are copies of one another, not results of arithmetic.
        bool same(double a, double b) noexcept
        {
            return ! (a < b) && ! (b < a);
        }

        // The host reports a loop in quarter notes, and where the block lies in them at its tempo: enough to tell
        // whether a jump back went from the loop's end to its start.
        bool reportsLoop(const Transport& transport) noexcept
        {
            return transport.hasLoopPoints && transport.hasPpq && transport.hasBpm && transport.bpm > 0.0
                && std::isfinite(transport.bpm) && std::isfinite(transport.ppq) && std::isfinite(transport.loopStartPpq)
                && std::isfinite(transport.loopEndPpq) && transport.loopEndPpq > transport.loopStartPpq;
        }
    } // namespace

    //==============================================================================
    void PrintFilters::prepare(double hostRate)
    {
        m_decimation = decimationFor(hostRate);
        m_captureRate = hostRate / m_decimation;

        // The anti-alias low-pass: an 8th-order Butterworth as four biquads, stage k with Q = 1 / (2 cos((2k + 1) pi / 16)).
        const double w = 2.0 * c_pi * c_antiAliasHz / hostRate;
        const double cosW = std::cos(w);

        for (std::size_t k = 0; k < m_antiAlias.size(); ++k)
        {
            const double q = 1.0 / (2.0 * std::cos((2.0 * static_cast<double>(k) + 1.0) * c_pi / 16.0));
            const double alpha = std::sin(w) / (2.0 * q);
            const double a0 = 1.0 + alpha;
            auto& stage = m_antiAlias[k];
            stage.b0 = static_cast<float>((1.0 - cosW) * 0.5 / a0);
            stage.b1 = static_cast<float>((1.0 - cosW) / a0);
            stage.b2 = stage.b0;
            stage.a1 = static_cast<float>(-2.0 * cosW / a0);
            stage.a2 = static_cast<float>((1.0 - alpha) / a0);
        }

        // Each octave band: the second-order Butterworth low-pass prototype turned into a band-pass between the
        // band's edges (pre-warped), then the bilinear transform. That gives four poles, paired into two biquads, and
        // zeros at DC and Nyquist; the gain is 1 at the band's centre.
        using Complex = std::complex<double>;
        const double k2 = 2.0 * m_captureRate;

        for (std::size_t b = 0; b < m_bands.size(); ++b)
        {
            const double low = k2 * std::tan(c_pi * c_bandEdgesHz[b] / m_captureRate);
            const double high = k2 * std::tan(c_pi * c_bandEdgesHz[b + 1] / m_captureRate);
            const double centre = std::sqrt(low * high);
            const double width = high - low;
            const Complex prototype = std::polar(1.0, 0.75 * c_pi) * width;
            const Complex root = std::sqrt(prototype * prototype - 4.0 * centre * centre);
            const Complex poles[2] { 0.5 * (prototype + root), 0.5 * (prototype - root) };

            const Complex z1 = std::polar(1.0, -2.0 * std::atan(centre / k2));   // e^-jw at the digital centre
            const Complex z2 = z1 * z1;
            double gain = 1.0;

            for (std::size_t s = 0; s < 2; ++s)
            {
                const Complex pole = (k2 + poles[s]) / (k2 - poles[s]);
                auto& section = m_bands[b][s];
                section.a1 = -2.0 * pole.real();
                section.a2 = std::norm(pole);
                gain *= std::abs((1.0 - z2) / (1.0 + section.a1 * z1 + section.a2 * z2));
            }

            const double g = 1.0 / std::sqrt(gain);

            for (auto& section : m_bands[b])
            {
                section.b0 = g;
                section.b2 = -g;
            }
        }

        reset();
    }

    void PrintFilters::reset() noexcept
    {
        for (auto& stage : m_antiAlias)
            stage.z1 = stage.z2 = 0.0f;

        for (auto& band : m_bands)
            for (auto& section : band)
                section.z1 = section.z2 = 0.0;
    }

    void PrintFilters::bandsOf(const float* y, int count, std::vector<Bands>& out)
    {
        reset();
        out.resize(static_cast<std::size_t>(std::max(0, count)));

        for (int i = 0; i < count; ++i)
            bands(y[i], out[static_cast<std::size_t>(i)]);
    }

    //==============================================================================
    Fingerprint makeFingerprint(const HopEnergy& energy) noexcept
    {
        Fingerprint print;
        const double count = std::max(1, energy.count);
        const double mean = energy.total / count + 1.0e-12;   // -120 dBFS at the least
        print.level = static_cast<std::uint8_t>(std::clamp(roundedDb(mean) + 120, 0, 255));

        for (std::size_t b = 0; b < energy.band.size(); ++b)
        {
            const double share = energy.band[b] / count / mean + 1.0e-20;
            print.band[b] = static_cast<std::uint8_t>(std::clamp(-roundedDb(share), 0, 60));
        }

        print.flags = static_cast<std::uint8_t>(Fingerprint::complete | (energy.clipped ? Fingerprint::clipped : 0));
        return print;
    }

    bool printsDiffer(const Fingerprint& heard, const Fingerprint& learned, int previousHeardLevel,
                      int previousLearnedLevel) noexcept
    {
        // Levels in dBFS, band shares in dB relative to their hop.
        const int heardDb = heard.level - 120;
        const int learnedDb = learned.level - 120;
        const bool heardQuiet = heardDb < -70;
        const bool learnedQuiet = learnedDb < -70;

        if (heardQuiet && learnedQuiet)
            return false;

        if (heardQuiet != learnedQuiet)
            return true;

        if (previousHeardLevel >= 0 && previousLearnedLevel >= 0
            && std::abs((heard.level - previousHeardLevel) - (learned.level - previousLearnedLevel)) > 3)
            return true;

        for (std::size_t b = 0; b < heard.band.size(); ++b)
        {
            const int heardShare = -static_cast<int>(heard.band[b]);
            const int learnedShare = -static_cast<int>(learned.band[b]);
            const bool matters = (heardShare > -30 || learnedShare > -30)
                              && heardDb + heardShare > -85 && learnedDb + learnedShare > -85;

            if (matters && std::abs(heardShare - learnedShare) > 2)
                return true;
        }

        return false;
    }

    bool pitchWindowFor(double period, int& window, int& span) noexcept
    {
        const double search = std::exp2(c_pitchSearchCents / 1200.0);

        if (! (period >= 4.0 && period * search + 2.0 < static_cast<double>(c_pitchMaxSpan)))
            return false;

        const int lowest = std::max(2, static_cast<int>(std::floor(period / search)) - 1);
        const int longest = static_cast<int>(std::ceil(period * search)) + 1;
        window = std::max(c_pitchMinWindow, static_cast<int>(std::ceil(2.0 * period)));
        span = window + longest;
        return span <= c_pitchMaxSpan && longest - lowest + 1 <= c_pitchMaxLags;
    }

    bool pitchDiffers(const float* x, int count, int window, double period, const Candidate* candidates,
                      int numCandidates) noexcept
    {
        const double search = std::exp2(c_pitchSearchCents / 1200.0);

        if (x == nullptr || window < 1 || ! (period >= 4.0))
            return false;

        const int lo = std::max(2, static_cast<int>(std::floor(period / search)) - 1);
        const int hi = std::min(count - window, static_cast<int>(std::ceil(period * search)) + 1);
        const int lags = hi - lo + 1;

        if (lags < 3 || lags > c_pitchMaxLags || ! (period > lo && period < hi))
            return false;

        // The window's energy, and that of what it is compared with (which slides along with the lag)
        double energy = 0.0;
        double shifted = 0.0;

        for (int i = 0; i < window; ++i)
        {
            energy += static_cast<double>(x[i]) * static_cast<double>(x[i]);
            shifted += static_cast<double>(x[lo + i]) * static_cast<double>(x[lo + i]);
        }

        if (energy < c_quietMeanSquare * window)
            return false;   // too quiet to say

        // The normalised difference at each lag, sum (a - b)^2 / sum (a^2 + b^2): 0 where the audio repeats exactly,
        // about 1 where it has nothing to do with itself.
        std::array<double, c_pitchMaxLags> difference {};

        for (int lag = lo; lag <= hi; ++lag)
        {
            if (lag > lo)
            {
                const auto entering = static_cast<double>(x[lag + window - 1]);
                const auto leaving = static_cast<double>(x[lag - 1]);
                shifted += entering * entering - leaving * leaving;
            }

            const double both = energy + shifted;
            difference[static_cast<std::size_t>(lag - lo)] = both > 0.0 ? 1.0 - 2.0 * static_cast<double>(dot(x, x + lag, window)) / both : 2.0;
        }

        // The deepest dip inside the range, placed between lags by a parabola through its neighbours
        int best = -1;

        for (int k = 1; k + 1 < lags; ++k)
        {
            const double here = difference[static_cast<std::size_t>(k)];

            if (here <= difference[static_cast<std::size_t>(k - 1)] && here <= difference[static_cast<std::size_t>(k + 1)]
                && (best < 0 || here < difference[static_cast<std::size_t>(best)]))
                best = k;
        }

        if (best < 0 || ! (difference[static_cast<std::size_t>(best)] < c_pitchClearRepeat))
            return false;   // nothing near the learned period repeats clearly: nothing to say

        const double before = difference[static_cast<std::size_t>(best - 1)];
        const double at = difference[static_cast<std::size_t>(best)];
        const double after = difference[static_cast<std::size_t>(best + 1)];
        const double bend = 2.0 * (2.0 * at - after - before);
        double lag = static_cast<double>(lo + best);

        if (std::abs(bend) > 1.0e-12)
            lag += std::clamp((after - before) / bend, -1.0, 1.0);

        // How the learned period itself repeats (between the lags either side of it)
        const double position = period - static_cast<double>(lo);
        const int below = std::clamp(static_cast<int>(std::floor(position)), 0, lags - 1);
        const int above = std::min(below + 1, lags - 1);
        const double fraction = position - static_cast<double>(below);
        const double learned = difference[static_cast<std::size_t>(below)] * (1.0 - fraction)
                             + difference[static_cast<std::size_t>(above)] * fraction;
        const double cents = 1200.0 * std::log2(period / lag);

        if (! (std::abs(cents) > c_pitchToleranceCents && learned > c_pitchLearnedMiss && learned - at > c_pitchMargin))
            return false;

        // A line the analysis heard here besides the curve (an engine's half order, a fifth) is no edit.
        for (int c = 0; c < numCandidates; ++c)
        {
            const double off = 1200.0 * std::log2(period / (lag * static_cast<double>(candidates[c].ratio)));

            if (std::abs(off - 1200.0 * std::round(off / 1200.0)) <= c_pitchToleranceCents)
                return false;
        }

        return true;
    }

    bool MatchWindow::push(bool differs) noexcept
    {
        m_bits = ((m_bits << 1) | (differs ? 1u : 0u)) & 0x3ffu;
        m_count = std::min(m_count + 1, 10);
        const bool wasValid = m_valid;

        if (m_valid)
            m_valid = std::popcount(m_bits & 0x1fu) < 4;
        else
            m_valid = m_count >= 10 && std::popcount(m_bits) <= 2;

        return m_valid != wasValid;
    }

    //==============================================================================
    CaptureBuffer::CaptureBuffer(double hostRate)
        : sampleRate(hostRate),
          decimation(decimationFor(hostRate)),
          hopSamples(hopSamplesFor(hostRate)),
          captureRate(hostRate / decimationFor(hostRate))
    {
        // The longest capture, plus half a second for a block that may still be waiting to be committed when the
        // limit is reached, and one hop more per segment for prints.
        samples.assign(static_cast<std::size_t>(std::ceil((c_maxRegionSeconds + 0.5) * captureRate)), 0.0f);
        prints.assign(static_cast<std::size_t>(std::ceil((c_maxRegionSeconds + 0.5) / c_hopSeconds)) + c_maxSegments,
                      Fingerprint {});
    }

    //==============================================================================
    LearnAudio::Resources::Resources(double captureRate)
        : alignment(std::make_shared<AlignmentRequest>())
    {
        const auto length = static_cast<std::size_t>(std::ceil(captureRate)) + 1;
        ring.assign(length, 0.0f);
        alignment->audio.assign(length, 0.0f);
        trail.assign(static_cast<std::size_t>(c_trailLength), TrailPoint {});
    }

    void LearnAudio::RegionView::set(const Region* r, double rate) noexcept
    {
        region = r;
        sameRate = same(r->learnedRate, rate);
        alignment = static_cast<std::int64_t>(std::llround(r->alignmentSeconds * rate));

        if (sameRate)
        {
            start = r->start() + alignment;
            end = r->end() + alignment;
        }
        else
        {
            start = static_cast<std::int64_t>(std::llround(r->followStartSeconds() * rate));
            end = static_cast<std::int64_t>(std::llround(r->followEndSeconds() * rate));
        }

        framesOrigin = (r->firstFrameSeconds + r->alignmentSeconds) * rate;
        framesPerSample = 1.0 / (c_frameSeconds * rate);
        hopsOrigin = (r->startSeconds() + r->alignmentSeconds) * rate;
        hopsPerSample = r->learnedRate / (static_cast<double>(r->hopSamples) * rate);
    }

    std::int64_t LearnAudio::RegionView::learnedHop(std::int64_t t, int sessionHop) const noexcept
    {
        if (sameRate)
            return floorDiv(t - alignment, sessionHop) - region->firstHop;

        return static_cast<std::int64_t>(std::floor((static_cast<double>(t) - hopsOrigin) * hopsPerSample));
    }

    void LearnAudio::HopMeter::start(std::int64_t position, std::int64_t gridOffset, int hopLength) noexcept
    {
        hop = floorDiv(position - gridOffset, hopLength);
        phase = static_cast<int>(position - gridOffset - hop * hopLength);
        whole = phase == 0;
        energy = {};
    }

    //==============================================================================
    LearnAudio::LearnAudio(LearnStore& store)
        : m_store(store)
    {
        m_guide.state = m_guideState.data();
        m_guide.logLineHz = m_guideLine.data();
        m_guide.logTargetHz = m_guideTarget.data();
    }

    LearnAudio::~LearnAudio() = default;

    void LearnAudio::prepare(double sampleRate)
    {
        const bool usable = sampleRate >= c_minSampleRate && sampleRate <= c_maxSampleRate;
        const double rate = usable ? sampleRate : 0.0;
        const bool changed = ! same(rate, m_rate);

        m_rate = rate;

        if (usable)
        {
            m_filters.prepare(rate);
            m_hop = hopSamplesFor(rate);
            m_decimation = m_filters.getDecimation();
            m_warmSamples = static_cast<std::int64_t>(std::llround(c_warmSeconds * rate));
            m_skipQuietSamples = static_cast<std::int64_t>(std::llround(c_skipQuietSeconds * rate));
        }

        // A capture cannot carry on at another rate (its positions would mean other times); the buffer goes back,
        // and the request has to be made again with a buffer for the new rate.
        if (changed && m_capture != nullptr)
        {
            m_capture->state.store(CaptureBuffer::free, std::memory_order_release);
            m_capture = nullptr;
        }

        m_segment = -1;
        m_pendingAny = false;
        m_phase = m_capture != nullptr ? CapturePhase::waiting : CapturePhase::idle;

        if (m_capture != nullptr)
            restartCapture();

        {
            const std::lock_guard<std::mutex> lock(m_resourcesLock);

            if (m_resourcesOwner != nullptr && changed)
                m_resourcesOwner = usable ? std::make_shared<Resources>(rate / decimationFor(rate)) : nullptr;

            m_resources.store(m_resourcesOwner.get(), std::memory_order_release);
            m_preparedRate.store(rate, std::memory_order_release);
        }

        // The ring's indices start again with it: it may just have been made anew at another length (or will be, by
        // ensureListening(), after a rate Learn cannot use), and its write index must lie inside it. Nothing listens
        // until the next block, which starts a new pass anyway.
        m_ringWrite = 0;
        m_ringCount = 0;
        m_ringNewest = 0;

        resetTimeline();
        publishCaptureStatus();
    }

    bool LearnAudio::ensureListening()
    {
        const std::lock_guard<std::mutex> lock(m_resourcesLock);
        const double rate = m_preparedRate.load(std::memory_order_acquire);

        if (rate <= 0.0)
            return false;

        if (m_resourcesOwner == nullptr)
        {
            m_resourcesOwner = std::make_shared<Resources>(rate / decimationFor(rate));
            m_resources.store(m_resourcesOwner.get(), std::memory_order_release);
        }

        return true;
    }

    std::uint32_t LearnAudio::requestCapture(CaptureBuffer& buffer)
    {
        m_offered.store(&buffer, std::memory_order_release);
        const auto generation = static_cast<std::uint32_t>(m_request.load(std::memory_order_relaxed) >> 1) + 1;
        m_request.store((static_cast<std::uint64_t>(generation) << 1) | 1u, std::memory_order_release);
        return generation;
    }

    void LearnAudio::requestStop()
    {
        const auto generation = static_cast<std::uint32_t>(m_request.load(std::memory_order_relaxed) >> 1) + 1;
        m_request.store(static_cast<std::uint64_t>(generation) << 1, std::memory_order_release);
    }

    void LearnAudio::withdrawOffer(CaptureBuffer& buffer) noexcept
    {
        // handleRequest() checks the offer again after it announces the buffer it will look at: once the offer is
        // withdrawn, it never takes this buffer up again. Only an announcement made before can still be under way.
        CaptureBuffer* expected = &buffer;
        m_offered.compare_exchange_strong(expected, nullptr, std::memory_order_seq_cst);

        while (m_offeredInUse.load(std::memory_order_seq_cst) == &buffer)
            std::this_thread::yield();
    }

    std::uint32_t LearnAudio::getRequestGeneration() const noexcept
    {
        return static_cast<std::uint32_t>(m_request.load(std::memory_order_acquire) >> 1);
    }

    bool LearnAudio::isCaptureRequested() const noexcept
    {
        return (m_request.load(std::memory_order_acquire) & 1u) != 0;
    }

    std::shared_ptr<AlignmentRequest> LearnAudio::getAlignmentRequest() const
    {
        const std::lock_guard<std::mutex> lock(m_resourcesLock);
        return m_resourcesOwner != nullptr ? m_resourcesOwner->alignment : nullptr;
    }

    LearnAudio::CaptureStatus LearnAudio::getCaptureStatus() const noexcept
    {
        CaptureStatus status;
        status.generation = m_statusGeneration.load(std::memory_order_acquire);
        status.phase = static_cast<CapturePhase>(m_statusPhase.load(std::memory_order_relaxed));
        status.noBuffer = m_statusNoBuffer.load(std::memory_order_relaxed);
        status.completedGeneration = m_statusCompleted.load(std::memory_order_acquire);
        status.coverage = m_statusCoverage.load(std::memory_order_relaxed);
        status.heardSeconds = m_statusHeard.load(std::memory_order_relaxed);
        status.loopSeconds = m_statusLoop.load(std::memory_order_relaxed);
        status.loopTooShort = m_statusLoopTooShort.load(std::memory_order_relaxed);
        return status;
    }

    std::optional<double> LearnAudio::getPlayheadSeconds() const noexcept
    {
        const auto position = m_statusPosition.load(std::memory_order_relaxed);
        const double rate = m_preparedRate.load(std::memory_order_relaxed);

        if (position == c_noPosition || ! (rate > 0.0))
            return std::nullopt;

        return static_cast<double>(position) / rate;
    }

    //==============================================================================
    void LearnAudio::resetTimeline() noexcept
    {
        m_haveLast = false;
        m_lastValid = false;
        m_lastPlaying = false;
        m_lastJumped = false;
        m_lastHasPpq = false;
        m_advances = 0;
        m_needAdvances = 0;
        m_lastCommitSafe = true;
        m_haveWrap = false;
        m_haveJump = false;
        m_haveEstimate = false;
        m_splitPending = false;
        m_quietRun = 0;
        m_verifyInRegion = false;
        m_verify.clear();
        m_verifyGapStart = m_verifyGapEnd = 0;
        m_fillInRegion = false;
        m_fillView.clear();
        m_fillGapStart = m_fillGapEnd = 0;
        m_transitionCount = 0;
        m_transitionRead = 0;
        m_flagPending = false;
        m_alignWanted = false;
        m_stretchFirst = -1;
    }

    void LearnAudio::beginBlock(const Transport& transport, int numSamples, bool nonRealtime, float glideMs,
                                float thresholdDb) noexcept
    {
        m_active = false;
        m_blockValid = false;
        m_numRuns = 0;
        m_followedThisBlock = false;
        m_snapshot = nullptr;
        m_res = m_resources.load(std::memory_order_acquire);

        // Learn has never been used: nothing to do, nothing to cost.
        if (m_res == nullptr || m_rate <= 0.0 || numSamples <= 0)
            return;

        m_snapshot = m_store.beginAudioRead();

        if (m_snapshot->revision != m_snapshotRevision)
        {
            refreshViews();
            m_snapshotRevision = m_snapshot->revision;
        }

        m_follow = m_snapshot->follow;
        m_thresholdDb = thresholdDb;
        m_lookAhead = static_cast<int>(std::lround(std::clamp(static_cast<double>(glideMs), 0.0, 2000.0) * 0.001 * m_rate));

        handleRequest();

        m_active = ! m_snapshot->regions.empty() || m_capture != nullptr;

        if (! m_active)
        {
            // Nothing to listen for: the next block that needs listening starts afresh.
            m_haveLast = false;
            m_lastValid = false;
            m_lastPlaying = false;
            m_lastJumped = false;
            m_lastHasPpq = false;
            m_splitPending = false;
            m_statusTimeline.store(static_cast<int>(transport.hasTime ? TimelineState::stopped : TimelineState::none),
                                   std::memory_order_relaxed);
            publishCaptureStatus();
            return;
        }

        classifyBlock(transport, numSamples, nonRealtime);
        publishCaptureStatus();
    }

    void LearnAudio::refreshViews() noexcept
    {
        // The views point into the snapshot pinned for an earlier block. This one may hold changed copies of their
        // regions (an edit) or not hold them at all (forgotten, cleared, a state loaded), and the old copies are freed
        // as soon as no block reads them: find the regions again by id, never touching the old copies. A region that
        // is gone or has moved (another alignment) ends its pass; the next sample finds where it is.
        const auto refresh = [this](RegionView& view, std::uint32_t id)
        {
            const Region* region = id != 0 ? m_snapshot->findId(id) : nullptr;

            if (region == nullptr || static_cast<std::int64_t>(std::llround(region->alignmentSeconds * m_rate)) != view.alignment)
                return false;

            view.set(region, m_rate);
            return true;
        };

        if (m_verifyInRegion && ! refresh(m_verify, m_verifyRegionId))
        {
            m_verifyInRegion = false;
            m_verify.clear();
            m_alignWanted = false;
            m_stretchFirst = -1;
        }

        if (! m_verifyInRegion)
            m_verifyGapStart = m_verifyGapEnd = 0;   // a region may have appeared there: look again

        if (m_fillInRegion && ! refresh(m_fillView, m_fillRegionId))
        {
            m_fillInRegion = false;
            m_fillView.clear();
        }

        if (! m_fillInRegion)
            m_fillGapStart = m_fillGapEnd = 0;

        m_wrapView.clear();   // found again for every block
    }

    void LearnAudio::classifyBlock(const Transport& transport, int numSamples, bool nonRealtime) noexcept
    {
        const bool hasTime = transport.hasTime;
        const std::int64_t t = transport.timeInSamples;
        const bool continues = m_haveLast && hasTime && std::llabs(t - m_expected) <= c_continuitySlack;
        const bool jumps = m_lastValid && hasTime && ! continues;   // from where the last valid block ended

        // Rule 2: a time that does not move while playing is the VST3 pre-roll (negative times reported as 0): not
        // trusted until two blocks have advanced normally. Only between two playing blocks: while the transport is
        // stopped, hosts keep calling with the position the playhead rests at, and play then starts from that same
        // position, a new pass like any other. The pre-roll's first block cannot be told from a real start at 0, so
        // it is trusted; it holds what lies before 0, silence, so following it changes nothing, and what it captured
        // is dropped once the next block repeats its time.
        const bool stalled = m_haveLast && m_lastPlaying && hasTime && transport.isPlaying && ! continues && t == m_lastTime;
        m_lastPlaying = transport.isPlaying;

        m_advances = hasTime && continues ? std::min(m_advances + 1, 1 << 20) : 0;

        if (stalled)
            m_needAdvances = 2;

        // Rule 2b: playback at another speed (half speed, varispeed, a play rate other than 1) moves the position by
        // more or less than the block on every block. A wrap or a locate breaks continuity once; two valid blocks in a
        // row that both break it are a timeline the learned curve can be neither followed on nor checked against:
        // not trusted until two blocks have advanced normally, as after a stall.
        if (jumps && transport.isPlaying && ! stalled && m_lastJumped)
            m_needAdvances = 2;

        // Rule 1: a playing position, or (hosts that leave "playing" off in offline renders) one that has advanced by
        // exactly the block length twice.
        bool valid = false;

        if (hasTime && ! stalled)
            valid = (transport.isPlaying || (nonRealtime && m_advances >= 2)) && m_advances >= m_needAdvances;

        if (valid)
            m_needAdvances = 0;

        m_hostLooping = transport.isLooping;
        m_largestBlock = std::max(m_largestBlock, numSamples);

        // Rule 3: the loop, from what the timeline did between two playing blocks.
        if (m_splitPending)
        {
            // The last block was split at the loop end the host reported, the part after it placed where the loop was
            // taken to start. This block's time says where that part really played: there (it continues), or that
            // part's length before this block, when the loop's quarter notes last longer or shorter at the tempo where
            // it starts than at the tempo where it ends. Anywhere else, the host went straight on, or was located.
            if (valid && m_lastValid && continues)
                noteWrap(m_splitStart, m_splitEnd);
            else if (valid && jumps && t - m_splitAfter < m_splitEnd && hostConfirmsWrap(transport))
                noteWrap(t - m_splitAfter, m_splitEnd);

            m_splitPending = false;
        }
        else if (valid && jumps && t < m_expected)
        {
            noteJumpBack(transport, t, m_expected);
        }
        else if (valid && jumps && t > m_expected)
        {
            noteSilentSkip(m_expected, t);
        }
        else if (m_haveWrap && valid && m_lastValid && continues
                 && (t >= m_wrapEnd + c_loopEdgeSlack || t + numSamples <= m_wrapStart - c_loopEdgeSlack))
        {
            // A loop seen earlier that the playhead now plays on outside of, past its end without wrapping or before
            // its start, no longer holds (the loop moved, or looping stopped): kept, it would hide the host's loop
            // points for the loop played now.
            m_haveWrap = false;
        }

        updateLoopEstimate(transport, numSamples);

        // The capture: what the last block wrote is kept once this block continues it, or if it cannot have
        // straddled a loop end the host did not report (rule 4: one-block commit lag). Not if this block repeats its
        // time: then both were the pre-roll.
        if (m_capture != nullptr && m_pendingAny)
        {
            if ((continues && m_lastValid) || (m_lastCommitSafe && ! stalled))
                commitPending();
            else
                discardPending();
        }

        m_blockValid = valid;
        m_blockLength = numSamples;
        m_statusTimeline.store(static_cast<int>(! hasTime ? TimelineState::none
                                                          : (valid ? TimelineState::playing : TimelineState::stopped)),
                               std::memory_order_relaxed);

        if (! valid)
        {
            // Nothing is captured or followed here; the next valid block starts a new pass.
            if (m_capture != nullptr)
            {
                closeSegment();
                checkCaptureComplete(! hasTime || ! transport.isPlaying);
            }

            m_haveLast = hasTime;
            m_lastTime = t;
            m_expected = t + numSamples;
            m_lastValid = false;
            m_lastJumped = false;
            m_lastHasPpq = false;
            m_lastCommitSafe = true;
            m_statusPosition.store(hasTime ? t : c_noPosition, std::memory_order_relaxed);
            return;
        }

        if (m_capture != nullptr)
            checkCaptureComplete(false);

        // The runs of this block: one, or two when it crosses a loop end the host reported but did not split at.
        const bool newPass = ! (continues && m_lastValid);
        const bool resync = ! newPass && t != m_expected;
        m_numRuns = 1;
        m_runs[0] = { 0, numSamples, t, newPass, resync, newPass ? ++m_passCounter : m_passCounter };
        m_expected = t + numSamples;
        m_lastCommitSafe = ! transport.isLooping || m_haveEstimate;

        // A loop end a few samples before the block's end is taken for one the host split at (quarter-note positions
        // are rounded): a block wrongly left whole there misplaces a few samples, a wrong split a whole pass start.
        if (m_haveEstimate && m_estimateEnd > t && m_estimateEnd < t + numSamples - c_splitSlack)
        {
            // The part after the loop end plays from the loop start: where the wrap heard at this end already says it
            // is, else (until the timeline has wrapped here) where the host's loop points put it, which holds at a
            // constant tempo only. The next block's time confirms that or says where it is, and only then is the wrap
            // noted.
            const int split = static_cast<int>(m_estimateEnd - t);
            const bool heardHere = m_haveWrap && std::llabs(m_wrapEnd - m_estimateEnd) <= c_splitSlack;
            const std::int64_t start = heardHere ? m_wrapStart + (m_estimateEnd - m_wrapEnd) : m_estimateStart;
            m_runs[0].length = split;
            m_runs[1] = { split, numSamples - split, start, true, false, ++m_passCounter };
            m_numRuns = 2;
            m_expected = start + (numSamples - split);
            m_lastCommitSafe = false;   // what follows the split is only right if the host really wrapped there
            m_splitPending = true;
            m_splitStart = start;
            m_splitEnd = m_estimateEnd;
            m_splitAfter = numSamples - split;
        }

        m_haveLast = true;
        m_lastTime = t;
        m_lastValid = true;
        m_lastJumped = jumps;
        m_statusPosition.store(t, std::memory_order_relaxed);

        // Where this block ends in quarter notes, for the next block's test of a jump back against the host's loop.
        m_lastHasPpq = transport.hasPpq && transport.hasBpm && transport.bpm > 0.0 && std::isfinite(transport.bpm)
                    && std::isfinite(transport.ppq);
        m_lastBlockSamples = numSamples;

        if (m_lastHasPpq)
        {
            m_lastPerSample = transport.bpm / (60.0 * m_rate);
            m_lastEndPpq = transport.ppq + static_cast<double>(numSamples) * m_lastPerSample;
        }

        // The loop the glide look-ahead wraps across (only while the host says it loops: a bounce that plays straight
        // on must never wrap), and whether the line carries on over its wrap.
        std::int64_t loopStart = 0;
        std::int64_t loopEnd = 0;
        m_loopForTarget = m_hostLooping && m_lookAhead > 0 && loopKnown(loopStart, loopEnd);
        m_wrapContinues = false;
        m_wrapView.clear();

        if (m_loopForTarget)
        {
            m_targetLoopStart = loopStart;
            m_targetLoopEnd = loopEnd;
            RegionView endView;
            std::int64_t gapStart = 0;
            std::int64_t gapEnd = 0;
            findRegion(loopEnd - 1, endView, gapStart, gapEnd);
            findRegion(loopStart, m_wrapView, gapStart, gapEnd);
            float endLine = 0.0f;
            float startLine = 0.0f;
            float confidence = 0.0f;

            if (endView.region != nullptr && m_wrapView.region != nullptr
                && lineAt(endView, loopEnd - 1, endLine, confidence) && lineAt(m_wrapView, loopStart, startLine, confidence))
                m_wrapContinues = std::abs(static_cast<double>(endLine) - static_cast<double>(startLine)) <= c_logSemitone;
        }
    }

    void LearnAudio::noteJumpBack(const Transport& transport, std::int64_t start, std::int64_t end) noexcept
    {
        // A jump back between two playing blocks, from `end` to `start`, is the loop only where the host agrees. A
        // host that reports its loop says so with its loop points (hostConfirmsWrap()). One that does not is believed
        // when the same jump comes again, or matches the loop already heard (a host that does not end a block at the
        // loop end jumps from up to a block past each end, a little differently on each pass). Anything else is a
        // locate (a click on the ruler, "go to the left locator"): a new pass, not the loop, and a capture listens on.
        bool isLoop = false;

        if (reportsLoop(transport) && m_lastHasPpq)
        {
            isLoop = hostConfirmsWrap(transport);
        }
        else
        {
            const std::int64_t tolerance = static_cast<std::int64_t>(m_largestBlock) + c_loopEdgeSlack;
            const auto near = [tolerance](std::int64_t a, std::int64_t b) { return std::llabs(a - b) <= tolerance; };
            isLoop = (m_haveWrap && near(start, m_wrapStart) && near(end, m_wrapEnd))
                  || (m_haveJump && near(start, m_jumpStart) && near(end, m_jumpEnd));
        }

        if (isLoop)
        {
            noteWrap(start, end);
            return;
        }

        m_haveJump = true;
        m_jumpStart = start;
        m_jumpEnd = end;
    }

    bool LearnAudio::hostConfirmsWrap(const Transport& transport) const noexcept
    {
        // The last block ended at the loop end the host reports and this one starts at its loop start, each within a
        // block (a host that does not end a block at the loop end plays up to a block past it, and lands as far past
        // the start), and within 50 ms whatever the block (a host may keep its quarter-note position a little apart
        // from its position in samples; a jump back from that close to the loop end to that close to its start is a
        // pass of the loop either way). Quarter notes are compared at each end's own tempo, so a tempo change inside
        // the loop does not matter.
        if (! (m_lastHasPpq && reportsLoop(transport)))
            return false;

        const double samples = std::max(static_cast<double>(m_lastBlockSamples), c_maxGapSeconds * m_rate)
                             + static_cast<double>(c_loopEdgeSlack);
        const double endTolerance = samples * m_lastPerSample;
        const double startTolerance = samples * transport.bpm / (60.0 * m_rate);

        return std::abs(m_lastEndPpq - transport.loopEndPpq) <= endTolerance
            && std::abs(transport.ppq - transport.loopStartPpq) <= startTolerance;
    }

    void LearnAudio::noteWrap(std::int64_t start, std::int64_t end) noexcept
    {
        m_haveWrap = true;
        m_wrapStart = start;
        m_wrapEnd = end;
        m_haveJump = false;

        if (m_capture != nullptr && m_phase == CapturePhase::capturing)
            m_captureWrapped = true;
    }

    void LearnAudio::noteSilentSkip(std::int64_t start, std::int64_t end) noexcept
    {
        // A jump forward right after the input fell silent: a host that stopped calling the plug-in while its input
        // was silent called it again when audio returned. What it skipped was silence, and a capture counts it as
        // heard, so a loop with a rest in it is complete after one pass. (A locate made in a quiet stretch counts too:
        // audio played there later differs from the silence learned, and falls back to live tracking.)
        if (m_capture == nullptr || m_phase != CapturePhase::capturing || m_quietRun < m_skipQuietSamples || end <= start
            || m_capture->numSilentSpans >= CaptureBuffer::c_maxSegments)
            return;

        auto& span = m_capture->silentSpans[static_cast<std::size_t>(m_capture->numSilentSpans++)];
        span.start = start;
        span.end = end;
    }

    void LearnAudio::updateLoopEstimate(const Transport& transport, int numSamples) noexcept
    {
        m_haveEstimate = false;

        if (! (transport.hasTime && transport.isLooping && transport.hasLoopPoints && transport.hasPpq && transport.hasBpm))
            return;

        const double bpm = transport.bpm;
        const double length = transport.loopEndPpq - transport.loopStartPpq;

        if (! (bpm > 0.0 && std::isfinite(bpm) && length > 0.0 && std::isfinite(length) && std::isfinite(transport.ppq))
            || transport.ppq >= transport.loopEndPpq)
            return;

        // Positions from quarter notes hold at a constant tempo only; positions in samples always win, so an estimate
        // more than a block away from a loop end actually seen is ignored.
        const double samplesPerQuarter = 60.0 / bpm * m_rate;
        const double toEnd = (transport.loopEndPpq - transport.ppq) * samplesPerQuarter;
        const double samples = length * samplesPerQuarter;

        if (! (toEnd < 1.0e12 && samples < 1.0e12))
            return;

        const std::int64_t end = transport.timeInSamples + static_cast<std::int64_t>(std::llround(toEnd));
        const std::int64_t start = end - static_cast<std::int64_t>(std::llround(samples));

        if (m_haveWrap && std::llabs(end - m_wrapEnd) > numSamples)
            return;

        m_haveEstimate = true;
        m_estimateStart = start;
        m_estimateEnd = end;
    }

    bool LearnAudio::loopKnown(std::int64_t& start, std::int64_t& end) const noexcept
    {
        // The last wrap heard: positions in samples. The host's loop points give the loop's length at a constant tempo
        // only, so they refine a wrap heard only where they describe the same loop (its end a few samples away, the
        // same length), and stand in for one, for the progress shown and the look-ahead, until there is one.
        if (m_haveWrap)
        {
            const std::int64_t lengths = (m_estimateEnd - m_estimateStart) - (m_wrapEnd - m_wrapStart);
            const bool sameLoop = m_haveEstimate && std::llabs(m_estimateEnd - m_wrapEnd) <= c_splitSlack
                               && std::llabs(lengths) <= c_loopEdgeSlack;
            start = sameLoop ? m_estimateStart : m_wrapStart;
            end = sameLoop ? m_estimateEnd : m_wrapEnd;
            return true;
        }

        if (m_haveEstimate)
        {
            start = m_estimateStart;
            end = m_estimateEnd;
            return true;
        }

        return false;
    }

    //==============================================================================
    void LearnAudio::handleRequest() noexcept
    {
        const auto request = m_request.load(std::memory_order_acquire);
        const auto generation = static_cast<std::uint32_t>(request >> 1);

        if (generation == m_generation)
            return;

        m_generation = generation;
        m_noBuffer = false;
        m_loopTooShort = false;

        if ((request & 1u) == 0)
        {
            releaseCapture();
            return;
        }

        // The buffer is announced before it is touched, then checked to be the one still offered: one withdrawn before
        // the announcement is never touched, and one withdrawn after it is not freed until the announcement ends
        // (withdrawOffer()). LEARN may be stopped and armed again meanwhile, and right after a rate change that frees
        // the buffer offered at the old rate.
        CaptureBuffer* offered = m_offered.load(std::memory_order_acquire);

        for (;;)
        {
            m_offeredInUse.store(offered, std::memory_order_seq_cst);
            CaptureBuffer* const current = m_offered.load(std::memory_order_seq_cst);

            if (current == offered)
                break;

            offered = current;
        }

        if (offered != nullptr && offered == m_capture)
        {
            // Asked again with the buffer already held: start over in it.
            m_capture->generation = generation;
            restartCapture();
        }
        else
        {
            releaseCapture();
            int expected = CaptureBuffer::offered;

            if (offered != nullptr && ! same(offered->sampleRate, m_rate))
            {
                // Made for a rate the host left before this request was seen: it can never be taken at this one. Hand it
                // back, so that it no longer counts as held and the worker can reuse or remake it, and ask for another.
                offered->state.compare_exchange_strong(expected, CaptureBuffer::free, std::memory_order_acq_rel);
                m_noBuffer = true;
            }
            else if (offered != nullptr
                     && offered->state.compare_exchange_strong(expected, CaptureBuffer::capturing, std::memory_order_acq_rel))
            {
                m_capture = offered;
                m_capture->generation = generation;
                restartCapture();
            }
            else
            {
                m_noBuffer = true;
            }
        }

        m_offeredInUse.store(nullptr, std::memory_order_release);
    }

    void LearnAudio::releaseCapture() noexcept
    {
        if (m_capture != nullptr)
            m_capture->state.store(CaptureBuffer::free, std::memory_order_release);

        m_capture = nullptr;
        m_segment = -1;
        m_pendingAny = false;
        m_phase = CapturePhase::idle;
        m_loopTooShort = false;
    }

    void LearnAudio::restartCapture() noexcept
    {
        m_capture->numSegments = 0;
        m_capture->numSilentSpans = 0;
        m_capture->onLoop = false;
        m_capture->segmentToLearn = -1;
        m_segment = -1;
        m_sampleWrite = 0;
        m_printWrite = 0;
        m_pendingAny = false;
        m_bufferFull = false;
        m_captureWrapped = false;
        m_largestBlock = 0;
        m_phase = CapturePhase::waiting;
    }

    void LearnAudio::openSegment(std::int64_t position) noexcept
    {
        if (m_capture == nullptr || m_bufferFull)
            return;

        if (m_capture->numSegments >= CaptureBuffer::c_maxSegments)
        {
            m_bufferFull = true;
            return;
        }

        auto& segment = m_capture->segments[static_cast<std::size_t>(m_capture->numSegments)];
        segment = {};
        segment.start = position;
        segment.end = position;
        segment.firstSample = position;
        segment.sampleOffset = m_sampleWrite;
        segment.printOffset = m_printWrite;
        m_segment = m_capture->numSegments++;
        m_pendingEnd = position;
        m_pendingAny = false;
        m_phase = CapturePhase::capturing;
        m_captureMeter.start(position, 0, m_hop);
    }

    void LearnAudio::closeSegment() noexcept
    {
        if (m_capture == nullptr || m_segment < 0)
            return;

        discardPending();
        const auto& segment = m_capture->segments[static_cast<std::size_t>(m_segment)];

        if (segment.end <= segment.start)
        {
            // Nothing of it was kept.
            m_sampleWrite = segment.sampleOffset;
            m_printWrite = segment.printOffset;
            --m_capture->numSegments;
        }

        m_segment = -1;

        if (m_capture->numSegments == 0)
            m_phase = CapturePhase::waiting;
    }

    void LearnAudio::commitPending() noexcept
    {
        if (m_capture == nullptr || m_segment < 0)
            return;

        auto& segment = m_capture->segments[static_cast<std::size_t>(m_segment)];
        segment.end = m_pendingEnd;
        segment.numSamples = m_sampleWrite - segment.sampleOffset;
        segment.numPrints = m_printWrite - segment.printOffset;
        m_pendingAny = false;
    }

    void LearnAudio::discardPending() noexcept
    {
        if (m_capture == nullptr || m_segment < 0)
            return;

        const auto& segment = m_capture->segments[static_cast<std::size_t>(m_segment)];
        m_sampleWrite = segment.sampleOffset + segment.numSamples;
        m_printWrite = segment.printOffset + segment.numPrints;
        m_pendingEnd = segment.end;
        m_pendingAny = false;
    }

    double LearnAudio::coverageOf(std::int64_t start, std::int64_t end, std::int64_t& largestGap) const noexcept
    {
        largestGap = end - start;

        if (m_capture == nullptr || end <= start)
            return 0.0;

        // The committed segments, and the stretches the host skipped as silence, clipped to [start, end), in order
        // (insertion sort: at most 128).
        std::array<std::pair<std::int64_t, std::int64_t>, 2 * CaptureBuffer::c_maxSegments> spans {};
        int count = 0;

        const auto add = [&spans, &count, start, end](std::int64_t a, std::int64_t b)
        {
            const std::int64_t from = std::max(a, start);
            const std::int64_t to = std::min(b, end);

            if (to <= from)
                return;

            int k = count++;

            while (k > 0 && spans[static_cast<std::size_t>(k - 1)].first > from)
            {
                spans[static_cast<std::size_t>(k)] = spans[static_cast<std::size_t>(k - 1)];
                --k;
            }

            spans[static_cast<std::size_t>(k)] = { from, to };
        };

        for (int i = 0; i < m_capture->numSegments; ++i)
        {
            const auto& segment = m_capture->segments[static_cast<std::size_t>(i)];
            add(segment.start, segment.end);
        }

        for (int i = 0; i < m_capture->numSilentSpans; ++i)
        {
            const auto& span = m_capture->silentSpans[static_cast<std::size_t>(i)];
            add(span.start, span.end);
        }

        std::int64_t covered = 0;
        std::int64_t reached = start;
        largestGap = 0;

        for (int i = 0; i < count; ++i)
        {
            const auto [from, to] = spans[static_cast<std::size_t>(i)];

            if (from > reached)
                largestGap = std::max(largestGap, from - reached);

            if (to > reached)
            {
                covered += to - std::max(from, reached);
                reached = to;
            }
        }

        largestGap = std::max(largestGap, end - reached);
        return static_cast<double>(covered) / static_cast<double>(end - start);
    }

    void LearnAudio::checkCaptureComplete(bool stopped) noexcept
    {
        if (m_capture == nullptr)
            return;

        int longest = -1;
        std::int64_t longestLength = 0;

        for (int i = 0; i < m_capture->numSegments; ++i)
        {
            const auto& segment = m_capture->segments[static_cast<std::size_t>(i)];

            if (segment.end - segment.start > longestLength)
            {
                longestLength = segment.end - segment.start;
                longest = i;
            }
        }

        const auto minCapture = static_cast<std::int64_t>(std::llround(c_minCaptureSeconds * m_rate));
        const auto maxRegion = static_cast<std::int64_t>(std::llround(c_maxRegionSeconds * m_rate));

        // A loop: complete once a wrap has been heard and the loop is covered, but for gaps of up to 50 ms (or one
        // block, for hosts with larger blocks: the block before a wrap may have been dropped) that are filled in.
        // The loop is the one heard wrapping, never one taken from the host's loop points alone (they say where it
        // starts at a constant tempo only). Its start itself must have been heard (it is, a block after the wrap):
        // the region starts there, so every wrap lands inside it.
        std::int64_t loopStart = 0;
        std::int64_t loopEnd = 0;
        const auto heard = [this](std::int64_t position)
        {
            for (int i = 0; i < m_capture->numSegments; ++i)
            {
                const auto& segment = m_capture->segments[static_cast<std::size_t>(i)];

                if (position >= segment.start && position < segment.end)
                    return true;
            }

            return false;
        };

        if (m_loopTooShort)
            return;

        const bool onLoop = m_captureWrapped && m_haveWrap && loopKnown(loopStart, loopEnd) && heard(loopStart);

        // A loop shorter than a capture can be would only ever be heard again (a wrap is heard only where the host's
        // loop agrees, or the same one twice: it certainly is a loop, not a locate back): this request ends there, and
        // the worker tells the user why. Nothing is learned from it, not even when the transport stops.
        if (onLoop && loopEnd - loopStart < minCapture)
        {
            m_loopTooShort = true;
            return;
        }

        if (onLoop && loopEnd - loopStart <= maxRegion)
        {
            const std::int64_t length = loopEnd - loopStart;
            const std::int64_t maxGap = std::max(static_cast<std::int64_t>(std::llround(c_maxGapSeconds * m_rate)),
                                                 static_cast<std::int64_t>(m_largestBlock) + c_loopEdgeSlack);
            std::int64_t largestGap = 0;
            const double covered = coverageOf(loopStart, loopEnd, largestGap);
            const double allowed = std::max(c_maxGapShare * static_cast<double>(length), static_cast<double>(maxGap));

            if (largestGap <= maxGap && (1.0 - covered) * static_cast<double>(length) <= allowed)
            {
                m_capture->maxGapSamples = maxGap;
                completeCapture(true, loopStart, loopEnd, -1);
                return;
            }
        }

        // The longest a region can be, the buffer full, or the transport stopped: the longest stretch heard.
        if (longestLength >= maxRegion || ((m_bufferFull || stopped) && longestLength >= minCapture))
        {
            completeCapture(false, 0, 0, longest);
            return;
        }

        if (m_bufferFull || stopped)
            restartCapture();   // too little to learn from: wait for playback again
    }

    void LearnAudio::completeCapture(bool onLoop, std::int64_t loopStart, std::int64_t loopEnd, int segment) noexcept
    {
        closeSegment();

        auto& capture = *m_capture;
        capture.onLoop = onLoop;
        capture.loopStart = loopStart;
        capture.loopEnd = loopEnd;
        capture.segmentToLearn = segment;
        capture.thresholdDb = m_thresholdDb;
        capture.generation = m_generation;
        capture.state.store(CaptureBuffer::captured, std::memory_order_release);   // the worker's from here on

        m_capture = nullptr;
        m_segment = -1;
        m_pendingAny = false;
        m_phase = CapturePhase::idle;
        m_completedGeneration = m_generation;
    }

    void LearnAudio::publishCaptureStatus() noexcept
    {
        float coverage = -1.0f;
        float heard = 0.0f;
        float loop = 0.0f;

        if (m_capture != nullptr)
        {
            std::int64_t longest = 0;

            for (int i = 0; i < m_capture->numSegments; ++i)
            {
                const auto& segment = m_capture->segments[static_cast<std::size_t>(i)];
                longest = std::max(longest, segment.end - segment.start);
            }

            heard = static_cast<float>(static_cast<double>(longest) / m_rate);
            std::int64_t loopStart = 0;
            std::int64_t loopEnd = 0;

            if (loopKnown(loopStart, loopEnd) && loopEnd > loopStart)
            {
                loop = static_cast<float>(static_cast<double>(loopEnd - loopStart) / m_rate);

                // A loop too short to learn never completes, however much of it is heard: no share to show.
                if (loopEnd - loopStart >= static_cast<std::int64_t>(std::llround(c_minCaptureSeconds * m_rate)))
                {
                    std::int64_t largestGap = 0;
                    coverage = static_cast<float>(coverageOf(loopStart, loopEnd, largestGap));
                }
            }
        }

        m_statusPhase.store(static_cast<int>(m_phase), std::memory_order_relaxed);
        m_statusNoBuffer.store(m_noBuffer, std::memory_order_relaxed);
        m_statusCoverage.store(coverage, std::memory_order_relaxed);
        m_statusHeard.store(heard, std::memory_order_relaxed);
        m_statusLoop.store(loop, std::memory_order_relaxed);
        m_statusLoopTooShort.store(m_loopTooShort, std::memory_order_relaxed);
        m_statusCompleted.store(m_completedGeneration, std::memory_order_release);
        m_statusGeneration.store(m_generation, std::memory_order_release);
    }

    //==============================================================================
    const SubharmonicEngine::PitchGuide* LearnAudio::processChunk(const float* mono, int offset, int numSamples) noexcept
    {
        if (! m_active || ! m_blockValid || numSamples <= 0 || mono == nullptr)
            return nullptr;

        numSamples = std::min(numSamples, c_maxChunkSamples);
        const int chunkEnd = offset + numSamples;

        // Listen to the whole chunk first: where it decides that a region stops (or starts again) matching, the
        // guide below changes from that sample on.
        for (int r = 0; r < m_numRuns; ++r)
        {
            const Run& run = m_runs[static_cast<std::size_t>(r)];
            const int from = std::max(offset, run.offset);
            const int to = std::min(chunkEnd, run.offset + run.length);

            if (to <= from)
                continue;

            if (from == run.offset)
                beginRun(run);

            listen(mono + (from - offset), to - from, run.position + (from - run.offset));
        }

        bool any = false;

        for (int r = 0; r < m_numRuns; ++r)
        {
            const Run& run = m_runs[static_cast<std::size_t>(r)];
            const int from = std::max(offset, run.offset);
            const int to = std::min(chunkEnd, run.offset + run.length);

            if (to <= from)
                continue;

            const bool first = from == run.offset;
            any = fill(from - offset, to - from, run.position + (from - run.offset), first && run.newPass, run.pass) || any;
        }

        // where this chunk ends on the timeline (the last run it reached), for the trail
        for (int r = m_numRuns - 1; r >= 0; --r)
        {
            const Run& run = m_runs[static_cast<std::size_t>(r)];

            if (run.offset < chunkEnd)
            {
                m_chunkEnd = run.position + (std::min(chunkEnd, run.offset + run.length) - run.offset);
                break;
            }
        }

        if (! any)
            return nullptr;

        m_followedThisBlock = true;
        return &m_guide;
    }

    void LearnAudio::addTrailPoint(float detectorHz, float subHz, bool learned) noexcept
    {
        if (! isListening() || m_res == nullptr)
            return;

        auto& trail = m_res->trail;
        const int written = m_res->trailWritten.load(std::memory_order_relaxed);
        auto& point = trail[static_cast<std::size_t>(written % c_trailLength)];
        point.seconds = static_cast<double>(m_chunkEnd) / m_rate;
        point.detectorHz = detectorHz;
        point.subHz = subHz;
        point.learned = learned;
        m_res->trailWritten.store(written == std::numeric_limits<int>::max() ? c_trailLength : written + 1,
                                  std::memory_order_release);
    }

    void LearnAudio::copyTrail(std::vector<TrailPoint>& dest) const
    {
        std::shared_ptr<Resources> resources;

        {
            const std::lock_guard<std::mutex> lock(m_resourcesLock);
            resources = m_resourcesOwner;
        }

        dest.clear();

        if (resources == nullptr)
            return;

        const int written = resources->trailWritten.load(std::memory_order_acquire);
        const int count = std::min(written, c_trailLength);

        for (int i = written - count; i < written; ++i)
            dest.push_back(resources->trail[static_cast<std::size_t>(i % c_trailLength)]);
    }

    void LearnAudio::endBlock(bool engineGuided) noexcept
    {
        if (m_snapshot == nullptr)
            return;

        if (m_active)
        {
            m_statusRegion.store(m_fillInRegion && m_fillView.region != nullptr ? m_fillView.region->id : 0u,
                                 std::memory_order_relaxed);
            m_statusFollowing.store(m_followedThisBlock && engineGuided, std::memory_order_relaxed);

            if (m_followedThisBlock && engineGuided)
                m_statusConfidence.store(m_lastConfidence, std::memory_order_relaxed);

            if (m_blockValid)
                m_statusPosition.store(m_expected, std::memory_order_relaxed);
        }
        else
        {
            m_statusRegion.store(0, std::memory_order_relaxed);
            m_statusFollowing.store(false, std::memory_order_relaxed);
        }

        m_snapshot = nullptr;
        m_store.endAudioRead();
    }

    //==============================================================================
    void LearnAudio::beginRun(const Run& run) noexcept
    {
        const std::int64_t position = run.position;

        if (run.newPass)
        {
            // The pass that ends here first (it may still hand its audio to an alignment job). Then the filters start
            // from silence, so what they measure from here on depends only on what plays from here (a bounce that
            // starts here measures what this pass does).
            endVerifyPass();
            m_filters.reset();
            m_warmUntil = position + m_warmSamples;
            m_ringCount = 0;
            m_listenPass = run.pass;
            m_quietRun = 0;

            // The verifier and the fill find their region again at the first sample.
            m_verifyInRegion = false;
            m_verify.clear();
            m_verifyGapStart = m_verifyGapEnd = position;
        }
        else if (run.resync && m_verifyInRegion)
        {
            // The same pass, its position reported a sample or two off where it was expected (hosts round fractional
            // positions). That is far below what a hop can tell, so the pass goes on: its region, its verdict so far
            // and its figures stay. Only the hop being measured restarts on the grid at the new position; heard in
            // part, it is not compared.
            m_verifyMeter.start(position, m_verify.alignment, m_hop);
            m_previousHeardLevel = -1;
        }

        // Decimated samples are taken on the timeline's grid. A resync keeps the stream running as it was, so a
        // capture's segment goes on holding samples a decimation apart.
        if (! run.resync)
            m_decPhase = floorMod(position, m_decimation);

        if (m_capture != nullptr && (run.newPass || m_segment < 0))
        {
            // A segment holds consecutive positions only (a resync goes on in the same one). What the previous run of
            // this block wrote ended at the loop end the host reported, where it split the block: keep it.
            if (m_pendingAny)
                commitPending();

            closeSegment();
            openSegment(position);
        }
    }

    void LearnAudio::listen(const float* mono, int count, std::int64_t position) noexcept
    {
        Resources& resources = *m_res;
        const int ringSize = static_cast<int>(resources.ring.size());
        CaptureBuffer* capture = m_segment >= 0 ? m_capture : nullptr;
        const int sampleCapacity = capture != nullptr ? static_cast<int>(capture->samples.size()) : 0;
        PrintFilters::Bands bands {};

        // prepare() starts the ring's indices again whenever the ring may change; whatever happens, never write
        // outside it.
        if (m_ringWrite < 0 || m_ringWrite >= ringSize)
        {
            m_ringWrite = 0;
            m_ringCount = 0;
        }

        for (int i = 0; i < count; ++i, ++position)
        {
            if (m_verifyInRegion ? (position < m_verify.start || position >= m_verify.end)
                                 : (position < m_verifyGapStart || position >= m_verifyGapEnd))
                relocateVerifier(position);

            float x = mono[i];

            if (! (std::abs(x) < c_maxInput))
                x = 0.0f;

            m_quietRun = std::abs(x) <= c_quietPeak ? std::min(m_quietRun + 1, c_farAway) : 0;
            const float y = m_filters.antiAlias(x);

            if (m_decPhase == 0)
            {
                m_filters.bands(y, bands);

                if (m_verifyInRegion)
                    m_verifyMeter.energy.add(y, bands);

                resources.ring[static_cast<std::size_t>(m_ringWrite)] = y;
                m_ringWrite = m_ringWrite + 1 >= ringSize ? 0 : m_ringWrite + 1;
                m_ringCount = std::min(m_ringCount + 1, ringSize);
                m_ringNewest = position;

                if (capture != nullptr)
                {
                    m_captureMeter.energy.add(y, bands);

                    if (m_sampleWrite < sampleCapacity)
                    {
                        auto& segment = capture->segments[static_cast<std::size_t>(m_segment)];

                        if (m_sampleWrite == segment.sampleOffset)
                            segment.firstSample = position;

                        capture->samples[static_cast<std::size_t>(m_sampleWrite++)] = y;
                    }
                    else
                    {
                        m_bufferFull = true;
                    }
                }
            }

            // What the open segment covers: up to here, unless the buffer is full.
            if (capture != nullptr && ! m_bufferFull)
            {
                m_pendingEnd = position + 1;
                m_pendingAny = true;
            }

            if (std::abs(x) >= 1.0f)
            {
                m_captureMeter.energy.clipped = true;
                m_verifyMeter.energy.clipped = true;
            }

            m_decPhase = m_decPhase + 1 == m_decimation ? 0 : m_decPhase + 1;

            if (capture != nullptr && ++m_captureMeter.phase == m_hop)
                finishCaptureHop();

            if (m_verifyInRegion && ++m_verifyMeter.phase == m_hop)
                finishVerifyHop();
        }
    }

    void LearnAudio::finishCaptureHop() noexcept
    {
        auto& meter = m_captureMeter;
        const std::int64_t hopStart = meter.hop * m_hop;
        auto& segment = m_capture->segments[static_cast<std::size_t>(m_segment)];

        // Only hops heard whole, once the filters have settled, are kept: the verifier compares no others.
        if (meter.whole && hopStart >= m_warmUntil && hopStart >= segment.start)
        {
            if (m_printWrite < static_cast<int>(m_capture->prints.size()))
            {
                if (m_printWrite == segment.printOffset)
                    segment.firstHop = meter.hop;

                m_capture->prints[static_cast<std::size_t>(m_printWrite++)] = makeFingerprint(meter.energy);
            }
            else
            {
                m_bufferFull = true;
            }
        }

        ++meter.hop;
        meter.phase = 0;
        meter.whole = true;
        meter.energy = {};
    }

    void LearnAudio::finishVerifyHop() noexcept
    {
        auto& meter = m_verifyMeter;
        const Region& region = *m_verify.region;
        const std::int64_t hopStart = meter.hop * m_hop + m_verify.alignment;
        bool compared = false;

        if (meter.whole && hopStart >= m_warmUntil)
        {
            // The learned hop this one lines up with.
            std::int64_t learned = -1;

            if (m_verify.sameRate)
            {
                learned = meter.hop - region.firstHop;
            }
            else
            {
                const double exact = (static_cast<double>(hopStart) - m_verify.hopsOrigin) * m_verify.hopsPerSample;
                const double nearest = std::round(exact);

                if (std::abs(exact - nearest) < 0.05)
                    learned = static_cast<std::int64_t>(nearest);
            }

            if (learned >= 0 && learned < region.numHops()
                && (region.prints[static_cast<std::size_t>(learned)].flags & Fingerprint::complete) != 0)
            {
                const Fingerprint& stored = region.prints[static_cast<std::size_t>(learned)];
                const Fingerprint heard = makeFingerprint(meter.energy);
                const bool chained = learned == m_previousLearnedHop + 1 && m_previousHeardLevel >= 0;
                const bool printDiffers = printsDiffer(heard, stored, chained ? m_previousHeardLevel : -1,
                                                       chained ? m_previousLearnedLevel : -1);

                // A note moved after learning (by a semitone or two, timing and level kept) leaves the prints nearly
                // as they were: where they match, the pitch the audio repeats at decides, once it has said so for two
                // tested hops in a row (a single hop can be a passing partial).
                if (! printDiffers)
                    m_pitchRun = pitchMoved(region) ? std::min(m_pitchRun + 1, 2) : 0;

                const bool differs = printDiffers || m_pitchRun >= 2;
                m_previousHeardLevel = heard.level;
                m_previousLearnedLevel = stored.level;
                m_previousLearnedHop = learned;
                compared = true;

                const bool changed = m_window.push(differs);
                RegionLive& live = *region.live;
                live.comparedHops.store(++m_passCompared, std::memory_order_relaxed);

                if (m_firstCompared < static_cast<std::uint32_t>(c_alignFirstHops))
                {
                    ++m_firstCompared;
                    m_firstDiffered += printDiffers ? 1u : 0u;   // a shift in time shows in the prints; a moved note is no shift

                    if (m_firstCompared == static_cast<std::uint32_t>(c_alignFirstHops)
                        && m_firstDiffered >= static_cast<std::uint32_t>(c_alignDifferHops))
                        m_alignWanted = true;
                }

                const auto next = static_cast<std::uint32_t>(learned + 1);

                if (changed)
                {
                    // The new state applies from the first sample after this hop.
                    pushTransition(hopStart + m_hop, m_window.valid());

                    if (! m_window.valid())
                    {
                        const int n = live.numStretches.load(std::memory_order_relaxed);

                        if (n < RegionLive::c_maxStretches)
                        {
                            live.stretchFirst[static_cast<std::size_t>(n)].store(next, std::memory_order_relaxed);
                            live.stretchEnd[static_cast<std::size_t>(n)].store(next, std::memory_order_relaxed);
                            live.numStretches.store(n + 1, std::memory_order_relaxed);
                            m_stretchFirst = n;
                        }
                    }
                    else
                    {
                        m_stretchFirst = -1;
                    }
                }
                else if (! m_window.valid() && m_stretchFirst >= 0)
                {
                    live.stretchEnd[static_cast<std::size_t>(m_stretchFirst)].store(next, std::memory_order_relaxed);
                }

                if (! m_window.valid())
                    live.invalidHops.store(++m_passInvalid, std::memory_order_relaxed);
            }
        }

        if (! compared)
        {
            m_previousHeardLevel = -1;   // the level-change test needs two compared hops in a row ...
            m_pitchRun = 0;              // ... and so does the pitch test
        }

        if (m_alignWanted && m_ringCount >= static_cast<int>(m_res->ring.size()))
            requestAlignment();

        ++meter.hop;
        meter.phase = 0;
        meter.whole = true;
        meter.energy = {};
    }

    bool LearnAudio::pitchMoved(const Region& region) noexcept
    {
        // Notes only. An engine's waveform repeats at its crankshaft's period, not at the firing line the curve
        // follows, and near that line it can repeat closely a semitone away (on an engine run-up, scattered over its
        // first second, 27 of 746 hops at 48 kHz); edits that move a note are made to notes anyway.
        if (m_res == nullptr || region.used != Method::Tonal)
            return false;

        const auto& ring = m_res->ring;
        const int ringSize = static_cast<int>(ring.size());
        const double captureRate = m_rate / static_cast<double>(m_decimation);
        double logCurve = 0.0;

        // The curve a little before the hop's end sizes what is compared (two of its periods and the lags around one) ...
        const auto lead = static_cast<std::int64_t>(std::llround(c_pitchCurveLeadSeconds * m_rate));

        if (! curveAt(m_verify, m_ringNewest - lead, logCurve))
            return false;

        const double sizedPeriod = captureRate / std::exp(logCurve);
        int window = 0;
        int span = 0;

        // ... all of it heard in this pass (its first samples could be anything), and only from inside the ring
        if (! pitchWindowFor(sizedPeriod, window, span) || span > m_ringCount || span > ringSize || m_ringWrite < 0
            || m_ringWrite >= ringSize)
            return false;

        // ... and the curve in its middle, between the samples compared a period apart, is what it should repeat at.
        const std::int64_t first = m_ringNewest - static_cast<std::int64_t>(span - 1) * m_decimation;
        const auto middle = first + static_cast<std::int64_t>(std::llround(0.5 * (static_cast<double>(window) + sizedPeriod) * m_decimation));

        if (! curveAt(m_verify, middle, logCurve))
            return false;

        int read = m_ringWrite - span;

        if (read < 0)
            read += ringSize;

        for (int i = 0; i < span; ++i)
        {
            m_pitchSpan[static_cast<std::size_t>(i)] = ring[static_cast<std::size_t>(read)];
            read = read + 1 == ringSize ? 0 : read + 1;
        }

        return pitchDiffers(m_pitchSpan.data(), span, window, captureRate / std::exp(logCurve), region.candidates.data(),
                            static_cast<int>(region.candidates.size()));
    }

    bool LearnAudio::curveAt(const RegionView& view, std::int64_t t, double& logCurve) const noexcept
    {
        // The analysis' own curve (the audio's pitch, not the line picked from it) where it was measured: a gap it
        // bridged says nothing about what the audio repeats at, and neither does a frame outside the region.
        const auto& frames = view.region->frames;
        const auto last = static_cast<std::int64_t>(frames.size()) - 1;
        const double position = (static_cast<double>(t) - view.framesOrigin) * view.framesPerSample;

        if (! (position >= 0.0 && position <= static_cast<double>(last)))
            return false;

        const auto i = static_cast<std::int64_t>(position);
        const double weight = position - static_cast<double>(i);
        const Frame& a = frames[static_cast<std::size_t>(i)];
        const Frame& b = frames[static_cast<std::size_t>(std::min(i + 1, last))];
        const auto measured = [](const Frame& frame) { return frame.isVoiced() && (frame.flags & Frame::bridged) == 0; };

        if (measured(a) && measured(b))
        {
            logCurve = static_cast<double>(a.logLineHz) + weight * (static_cast<double>(b.logLineHz) - static_cast<double>(a.logLineHz));
            return true;
        }

        const Frame& nearer = weight < 0.5 ? a : b;

        if (! measured(nearer))
            return false;

        logCurve = static_cast<double>(nearer.logLineHz);
        return true;
    }

    void LearnAudio::relocateVerifier(std::int64_t position) noexcept
    {
        endVerifyPass();
        findRegion(position, m_verify, m_verifyGapStart, m_verifyGapEnd);
        m_verifyInRegion = m_verify.region != nullptr;
        m_verifyRegionId = m_verifyInRegion ? m_verify.region->id : 0u;

        if (! m_verifyInRegion)
            return;

        // A new pass through this region: it starts trusting, and its figures start again. Those of the pass that ends
        // here are kept as the last pass's, unless it compared nothing (a locate that only touched the region).
        RegionLive& live = *m_verify.region->live;

        if (const auto compared = live.comparedHops.load(std::memory_order_relaxed); compared > 0)
        {
            const int count = std::clamp(live.numStretches.load(std::memory_order_relaxed), 0, RegionLive::c_maxStretches);

            for (std::size_t i = 0; i < static_cast<std::size_t>(count); ++i)
            {
                live.lastStretchFirst[i].store(live.stretchFirst[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
                live.lastStretchEnd[i].store(live.stretchEnd[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
            }

            live.lastNumStretches.store(count, std::memory_order_relaxed);
            live.lastComparedHops.store(compared, std::memory_order_relaxed);
            live.lastInvalidHops.store(live.invalidHops.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }

        live.passes.fetch_add(1, std::memory_order_relaxed);
        live.comparedHops.store(0, std::memory_order_relaxed);
        live.invalidHops.store(0, std::memory_order_relaxed);
        live.numStretches.store(0, std::memory_order_relaxed);

        m_window.reset();
        m_verifyMeter.start(position, m_verify.alignment, m_hop);
        m_previousHeardLevel = -1;
        m_previousLearnedLevel = -1;
        m_previousLearnedHop = -2;
        m_pitchRun = 0;
        m_passCompared = 0;
        m_passInvalid = 0;
        m_firstCompared = 0;
        m_firstDiffered = 0;
        m_alignWanted = false;
        m_stretchFirst = -1;
    }

    void LearnAudio::endVerifyPass() noexcept
    {
        if (! m_verifyInRegion)
            return;

        // An alignment the pass asked for, with at least half a second heard, goes now (the ring is still this
        // pass's audio).
        if (m_alignWanted && m_ringCount >= static_cast<int>(std::lround(c_alignMinSeconds * m_rate / m_decimation)))
            requestAlignment();

        m_alignWanted = false;
        m_stretchFirst = -1;
        m_verifyInRegion = false;
        m_verify.clear();
    }

    void LearnAudio::pushTransition(std::int64_t position, bool valid) noexcept
    {
        if (m_transitionCount >= c_maxTransitions)
        {
            // Never more than a few per chunk; if the fill fell this far behind, the oldest is the least useful.
            m_transitionRead = (m_transitionRead + 1) % c_maxTransitions;
            --m_transitionCount;
        }

        auto& transition = m_transitions[static_cast<std::size_t>((m_transitionRead + m_transitionCount) % c_maxTransitions)];
        transition.position = position;
        transition.pass = m_listenPass;
        transition.regionId = m_verify.region != nullptr ? m_verify.region->id : 0u;
        transition.valid = valid;
        ++m_transitionCount;
    }

    void LearnAudio::requestAlignment() noexcept
    {
        m_alignWanted = false;

        if (! m_verifyInRegion || m_verify.region == nullptr || m_res == nullptr)
            return;

        const Region& region = *m_verify.region;
        AlignmentRequest& request = *m_res->alignment;

        if (request.state.load(std::memory_order_acquire) != AlignmentRequest::idle)
            return;

        // What the worker did not get to the end of does not count as tried. (A job reports it before the next job
        // hands the slot back, long before its place among the attempts comes round again.)
        if (const auto notMade = request.notMade.exchange(0, std::memory_order_acquire); notMade != 0)
            for (int i = 0; i < c_maxAttempts; ++i)
                if ((notMade & (1u << i)) != 0)
                    m_attemptedIds[static_cast<std::size_t>(i)] = 0;   // no region has id 0

        // One attempt per region and alignment: if the audio there really changed, asking again will not help.
        for (int i = 0; i < c_maxAttempts; ++i)
            if (m_attemptedIds[static_cast<std::size_t>(i)] == region.id
                && same(m_attemptedAlignments[static_cast<std::size_t>(i)], region.alignmentSeconds))
                return;

        const int ringSize = static_cast<int>(m_res->ring.size());

        // Only from inside the ring (listen() keeps its indices there; this never reads past it regardless).
        if (m_ringWrite < 0 || m_ringWrite >= ringSize)
            return;

        const int count = std::clamp(std::min(m_ringCount, static_cast<int>(request.audio.size())), 0, ringSize);
        int read = m_ringWrite - count;

        if (read < 0)
            read += ringSize;

        for (int i = 0; i < count; ++i)
        {
            request.audio[static_cast<std::size_t>(i)] = m_res->ring[static_cast<std::size_t>(read)];
            read = read + 1 == ringSize ? 0 : read + 1;
        }

        request.attempt = m_attemptWrite;
        request.regionId = region.id;
        request.regionAlignmentSeconds = region.alignmentSeconds;
        request.firstPosition = m_ringNewest - static_cast<std::int64_t>(count - 1) * m_decimation;
        request.numSamples = count;
        request.decimation = m_decimation;
        request.sampleRate = m_rate;
        request.state.store(AlignmentRequest::filled, std::memory_order_release);

        m_attemptedIds[static_cast<std::size_t>(m_attemptWrite)] = region.id;
        m_attemptedAlignments[static_cast<std::size_t>(m_attemptWrite)] = region.alignmentSeconds;
        m_attemptWrite = (m_attemptWrite + 1) % c_maxAttempts;
    }

    //==============================================================================
    bool LearnAudio::fill(int index, int count, std::int64_t position, bool newPass, std::uint32_t pass) noexcept
    {
        bool any = false;

        // A new pass finds its region again, and trusts it again. A resync (the position a sample or two off) is the
        // same pass: it keeps the region and what the verifier decided about it.
        if (newPass)
        {
            m_fillPass = pass;
            m_flagPending = true;   // the first sample of a pass: the input jumped, so may the sub
            m_flagPassStart = position;
            m_fillInRegion = false;
            m_fillView.clear();
            m_fillGapStart = m_fillGapEnd = position;
        }

        for (int i = 0; i < count; ++i, ++position)
        {
            const auto k = static_cast<std::size_t>(index + i);

            if (m_fillInRegion ? (position < m_fillView.start || position >= m_fillView.end)
                               : (position < m_fillGapStart || position >= m_fillGapEnd))
                relocateFill(position);

            // Where the verifier decided the region stops (or starts again) matching.
            while (m_transitionCount > 0)
            {
                const Transition& transition = m_transitions[static_cast<std::size_t>(m_transitionRead)];

                if (transition.pass != m_fillPass)
                {
                    if (static_cast<std::int32_t>(transition.pass - m_fillPass) > 0)
                        break;   // a later pass of this chunk
                }
                else
                {
                    if (transition.position > position)
                        break;

                    if (m_fillInRegion && transition.regionId == m_fillRegionId)
                        m_fillValid = transition.valid;
                }

                m_transitionRead = (m_transitionRead + 1) % c_maxTransitions;
                --m_transitionCount;
            }

            std::uint8_t state = Guide::live;

            if (m_fillInRegion && m_follow && m_fillValid)
            {
                const Region& region = *m_fillView.region;
                const std::int64_t hop = m_fillView.learnedHop(position, m_hop);

                // A hop the capture did not hear (a gap it filled in) has no learned pitch worth following.
                if (hop >= 0 && hop < region.numHops() && region.prints[static_cast<std::size_t>(hop)].isHeard())
                {
                    float line = 0.0f;
                    float confidence = 0.0f;
                    bool silent = false;

                    if (lineAt(m_fillView, position, line, confidence, &silent))
                    {
                        state = Guide::voiced;
                        m_guideLine[k] = line;
                        m_guideTarget[k] = targetAt(position, line);
                        m_lastConfidence = confidence;
                    }
                    else if (! (silent && m_thresholdDb - SubharmonicEngine::c_gateHysteresisDb < region.silenceDbfs))
                    {
                        state = Guide::unvoiced;
                    }

                    // Otherwise the analysis called the audio here silent, but the Threshold has since been lowered so far
                    // that the level gate may be open here: the gate and the live tracker decide, as outside a learned
                    // passage (the curve would stop the sub there whatever the Threshold). Learning the passage again
                    // gives these parts learned pitch.
                }
            }

            // The input jumped where the pass started. A region aligned later (latency inserted before Subbertone, a
            // nudged clip) hears that jump where its audio now arrives, at its moved start: up to its alignment after
            // the pass start. So the flag waits through the samples before the first region of the pass and is decided
            // there: marked if the alignment explains the distance, dropped otherwise (a locate shortly before a region)
            // or where that sample is tracked live (USE LEARNED off, a fallback).
            if (m_flagPending && m_fillInRegion)
            {
                const std::int64_t reach = std::max<std::int64_t>(0, m_fillView.alignment) + c_loopEdgeSlack;

                if (state != Guide::live && position - m_flagPassStart <= reach)
                    state = static_cast<std::uint8_t>(state | Guide::discontinuity);

                m_flagPending = false;
            }

            m_guideState[k] = state;
            any = any || state != Guide::live;
        }

        return any;
    }

    void LearnAudio::relocateFill(std::int64_t position) noexcept
    {
        findRegion(position, m_fillView, m_fillGapStart, m_fillGapEnd);
        m_fillInRegion = m_fillView.region != nullptr;
        m_fillRegionId = m_fillInRegion ? m_fillView.region->id : 0u;
        m_fillValid = true;   // as the verifier: every pass through a region starts trusting
    }

    bool LearnAudio::lineAt(const RegionView& view, std::int64_t t, float& logLine, float& confidence, bool* silent) const noexcept
    {
        if (silent != nullptr)
            *silent = false;

        const auto& frames = view.region->frames;
        const auto last = static_cast<std::int64_t>(frames.size()) - 1;
        const double position = (static_cast<double>(t) - view.framesOrigin) * view.framesPerSample;
        std::int64_t i = 0;
        double weight = 0.0;

        if (position > 0.0 && position < static_cast<double>(last))
        {
            i = static_cast<std::int64_t>(position);
            weight = position - static_cast<double>(i);
        }
        else if (position >= static_cast<double>(last))
        {
            i = last;
        }

        const Frame& a = frames[static_cast<std::size_t>(i)];
        const Frame& b = frames[static_cast<std::size_t>(std::min(i + 1, last))];
        double line = 0.0;

        // Linear in ln Hz between two frames with a pitch; next to a frame without one, the nearer frame decides.
        if (a.isVoiced() && b.isVoiced())
        {
            line = static_cast<double>(a.logLineHz) + weight * (static_cast<double>(b.logLineHz) - static_cast<double>(a.logLineHz));
            confidence = static_cast<float>(static_cast<double>(a.confidence)
                                            + weight * (static_cast<double>(b.confidence) - static_cast<double>(a.confidence)));
        }
        else
        {
            const Frame& nearer = weight < 0.5 ? a : b;

            if (! nearer.isVoiced())
            {
                if (silent != nullptr)
                    *silent = (nearer.flags & Frame::silent) != 0;

                return false;
            }

            line = static_cast<double>(nearer.logLineHz);
            confidence = nearer.confidence;
        }

        logLine = static_cast<float>(line + static_cast<double>(view.region->lineLogRatio));
        return true;
    }

    float LearnAudio::targetAt(std::int64_t t, float lineNow) const noexcept
    {
        if (m_lookAhead <= 0)
            return lineNow;

        std::int64_t ahead = t + m_lookAhead;

        // Rule 5: across the loop end the look-ahead reads what plays after the wrap, where the line carries on
        // there. Where it does not, the sub jumps at the wrap (the guide marks it), so the look-ahead holds at the
        // loop end and the sub stays on the line until then.
        if (m_loopForTarget && t < m_targetLoopEnd && ahead >= m_targetLoopEnd)
        {
            if (m_wrapContinues)
            {
                const std::int64_t wrapped = m_targetLoopStart + (ahead - m_targetLoopEnd);
                float line = 0.0f;
                float confidence = 0.0f;

                if (m_wrapView.region != nullptr && wrapped >= m_wrapView.start && wrapped < m_wrapView.end
                    && lineAt(m_wrapView, wrapped, line, confidence))
                    return line;

                return lineNow;
            }

            ahead = m_targetLoopEnd - 1;
        }

        // Past the region's end: the line where it ends.
        if (ahead >= m_fillView.end)
            ahead = m_fillView.end - 1;

        float line = 0.0f;
        float confidence = 0.0f;

        if (ahead >= m_fillView.start && lineAt(m_fillView, ahead, line, confidence))
            return line;

        return lineNow;   // a note ends ahead: hold the pitch while it releases
    }

    void LearnAudio::findRegion(std::int64_t position, RegionView& view, std::int64_t& gapStart, std::int64_t& gapEnd) const noexcept
    {
        view.clear();
        gapStart = -c_farAway;
        gapEnd = c_farAway;

        if (m_snapshot == nullptr)
            return;

        for (const auto& region : m_snapshot->regions)
        {
            RegionView candidate;
            candidate.set(region.get(), m_rate);

            if (position >= candidate.start && position < candidate.end)
            {
                view = candidate;
                return;
            }

            if (candidate.end <= position)
                gapStart = std::max(gapStart, candidate.end);
            else
                gapEnd = std::min(gapEnd, candidate.start);
        }
    }
} // namespace subbertone::learn
