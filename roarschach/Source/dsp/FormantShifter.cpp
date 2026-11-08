#include "FormantShifter.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace roar
{
    namespace
    {
        constexpr double c_pi = 3.14159265358979323846;
        constexpr double c_latencySeconds = 0.030;
        constexpr double c_unvoicedPeriodSeconds = 0.010;  // pseudo-period: grain half-width and spacing
        constexpr double c_smoothSeconds = 0.020;
        constexpr double c_wetFadeSeconds = 0.020;
        constexpr double c_sizeFormantDepth = 0.6;          // r = 1 / (1 + 0.6 * size)
        constexpr double c_markTolerance = 0.25;            // search +-25 % of a period around the prediction
        constexpr float c_markPenalty = 0.5f;               // how strongly the search prefers the predicted spot
        constexpr float c_minMarkLevel = 1.0e-5f;
        constexpr double c_minMarkCorrelation = 0.5;        // adjacent periods must look alike to count as voiced
        constexpr double c_minHz = 50.0, c_maxHz = 1000.0;
        constexpr int c_bodyUpdateInterval = 32;
        constexpr std::array<double, 3> c_bodyHz { 120.0, 250.0, 500.0 };
        constexpr std::array<double, 3> c_bodyDb { 5.0, 4.0, 3.0 };
        constexpr std::array<double, 3> c_bodyQ { 0.9, 1.1, 1.3 };

        inline float sanitize (float x)
        {
            return std::abs (x) < 1.0e4f ? x : 0.0f;  // NaN fails the comparison too
        }
    }

    //==============================================================================
    void FormantShifter::Biquad::setPeaking (double fs, double fc, double q, double gainDb)
    {
        const double a = std::pow (10.0, gainDb / 40.0);
        const double w = 2.0 * c_pi * fc / fs;
        const double cw = std::cos (w);
        const double alpha = std::sin (w) / (2.0 * q);
        const double a0 = 1.0 + alpha / a;
        b0 = static_cast<float> ((1.0 + alpha * a) / a0);
        b1 = static_cast<float> (-2.0 * cw / a0);
        b2 = static_cast<float> ((1.0 - alpha * a) / a0);
        a1 = b1;
        a2 = static_cast<float> ((1.0 - alpha / a) / a0);
    }

    void FormantShifter::Biquad::setLowpass (double fs, double fc, double q)
    {
        const double w = 2.0 * c_pi * fc / fs;
        const double cw = std::cos (w);
        const double alpha = std::sin (w) / (2.0 * q);
        const double a0 = 1.0 + alpha;
        b0 = static_cast<float> ((1.0 - cw) * 0.5 / a0);
        b1 = static_cast<float> ((1.0 - cw) / a0);
        b2 = b0;
        a1 = static_cast<float> (-2.0 * cw / a0);
        a2 = static_cast<float> ((1.0 - alpha) / a0);
    }

    //==============================================================================
    void FormantShifter::prepare (double sampleRate, int /*maxBlockSize*/)
    {
        m_sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
        m_latency = static_cast<int> (std::ceil (c_latencySeconds * m_sampleRate));
        m_minPeriod = std::max (2, static_cast<int> (std::floor (m_sampleRate / c_maxHz)));
        m_maxPeriod = static_cast<int> (std::ceil (m_sampleRate / c_minHz));
        m_unvoicedHalfWidth = c_unvoicedPeriodSeconds * m_sampleRate;

        // History: the latency, plus grains that borrow a mark up to three periods back and read a
        // period either side of it, plus room for the interpolator.
        const int needed = m_latency + 6 * m_maxPeriod + 1024;
        int size = 1;
        while (size < needed)
            size <<= 1;
        m_input.assign (static_cast<size_t> (size), 0.0f);
        m_band.assign (static_cast<size_t> (size), 0.0f);
        m_mask = size - 1;

        m_detector.prepare (m_sampleRate);
        m_bandHpCoeff = static_cast<float> (std::exp (-2.0 * c_pi * 60.0 / m_sampleRate));
        m_bandLp.setLowpass (m_sampleRate, 900.0, 0.7071);

        m_smoothCoeff = 1.0 - std::exp (-1.0 / (c_smoothSeconds * m_sampleRate));
        m_wetStep = static_cast<float> (1.0 / (c_wetFadeSeconds * m_sampleRate));

        m_throat.assign (static_cast<size_t> (std::ceil (0.0045 * m_sampleRate)) + 4, 0.0f);

        for (int i = 0; i <= c_windowSize + 1; ++i)
            m_cosTable[static_cast<size_t> (i)] = static_cast<float> (std::cos (0.5 * c_pi * std::min (i, c_windowSize) / c_windowSize));

        reset();
    }

    void FormantShifter::reset()
    {
        std::fill (m_input.begin(), m_input.end(), 0.0f);
        std::fill (m_band.begin(), m_band.end(), 0.0f);
        m_writePos = 0;

        m_detector.reset();
        m_bandHpX1 = m_bandHpY1 = 0.0f;
        m_bandLp.clear();

        m_markHead = 0;
        m_markCount = 0;
        m_lastMark = -1;
        m_haveLastMark = false;
        m_startRetry = 0;
        m_positivePeaks = m_negativePeaks = 0.0f;
        m_polarity = 1.0f;

        m_grainCount = 0;
        m_prevSyn = -static_cast<double> (m_latency);
        m_nextSyn = m_prevSyn + m_unvoicedHalfWidth;
        m_prevHalfWidth = m_unvoicedHalfWidth;
        m_prevVoiced = false;
        m_unvoicedOffset = 0.0;

        // Jump straight to the targets: a reset is a new start, nothing to glide from.
        m_logRatio = std::log (2.0) * m_pitchTarget / 12.0;
        m_ratio = std::exp (m_logRatio);
        m_size = m_sizeTarget;
        m_rate = 1.0 / (1.0 + c_sizeFormantDepth * m_size);
        m_wet = (std::abs (m_pitchTarget) > 0.005f || m_sizeTarget > 0.001f) ? 1.0f : 0.0f;

        for (auto& b : m_body)
            b.clear();
        m_bodyCounter = 0;
        m_bodyActive = false;
        updateBody();
        std::fill (m_throat.begin(), m_throat.end(), 0.0f);
        m_throatWrite = 0;

        m_random = 0x12345678u;
        m_voicedGrains = m_unvoicedGrains = 0;
    }

    void FormantShifter::setParams (float pitchSemitones, float size)
    {
        // Written so that NaN lands on a safe value (std::clamp would pass it through).
        m_pitchTarget = pitchSemitones >= -12.0f ? std::min (pitchSemitones, 12.0f) : (pitchSemitones < -12.0f ? -12.0f : 0.0f);
        m_sizeTarget = size >= 0.0f ? std::min (size, 1.0f) : 0.0f;
    }

    VoiceInfo FormantShifter::getOutputVoice() const
    {
        VoiceInfo v = m_detector.getVoiceInfo();
        v.f0Hz *= static_cast<float> (m_ratio);
        return v;
    }

    //==============================================================================
    void FormantShifter::process (float* left, float* right, int numSamples, bool stereo, const VoiceInfo& /*voice*/)
    {
        if (stereo && right != nullptr)
        {
            for (int i = 0; i < numSamples; ++i)
            {
                const float y = processSample (0.5f * (left[i] + right[i]));
                left[i] = y;
                right[i] = y;
            }
        }
        else
        {
            for (int i = 0; i < numSamples; ++i)
                left[i] = processSample (left[i]);
        }
    }

    float FormantShifter::processSample (float x)
    {
        x = sanitize (x);
        const std::int64_t n = m_writePos;
        m_input[static_cast<size_t> (n & m_mask)] = x;

        // Band-limited copy for the peak search: the glottal pulse shows as one clear peak per
        // period once the upper formants are gone.
        const float hp = m_bandHpCoeff * (m_bandHpY1 + x - m_bandHpX1);
        m_bandHpX1 = x;
        m_bandHpY1 = std::abs (hp) > 1.0e-20f ? hp : 0.0f;
        float e = m_bandLp.process (hp);
        if (! std::isfinite (e))
        {
            m_bandLp.clear();
            e = 0.0f;
        }
        m_band[static_cast<size_t> (n & m_mask)] = e;

        m_detector.pushSample (x);
        ++m_writePos;
        findMarks();

        // Parameter smoothing (per sample, so pitch sweeps stay smooth).
        const double logTarget = std::log (2.0) * m_pitchTarget / 12.0;
        m_logRatio += m_smoothCoeff * (logTarget - m_logRatio);
        m_ratio = std::exp (m_logRatio);
        m_size += m_smoothCoeff * (m_sizeTarget - m_size);
        if (std::abs (m_size - m_sizeTarget) < 1.0e-7)
            m_size = m_sizeTarget;
        m_rate = 1.0 / (1.0 + c_sizeFormantDepth * m_size);

        const bool active = std::abs (m_pitchTarget) > 0.005f || m_sizeTarget > 0.001f;
        m_wet = active ? std::min (1.0f, m_wet + m_wetStep) : std::max (0.0f, m_wet - m_wetStep);

        const std::int64_t outPos = n - m_latency;
        scheduleGrains (outPos);
        const float shifted = renderGrains (outPos);
        const float dry = m_input[static_cast<size_t> (outPos & m_mask)];
        float y = dry + m_wet * (shifted - dry);

        // Body resonators and throat comb, only while Size is above zero (exact delay at 0).
        if (++m_bodyCounter >= c_bodyUpdateInterval)
        {
            m_bodyCounter = 0;
            updateBody();
        }

        if (m_bodyActive)
        {
            for (auto& b : m_body)
                y = b.process (y);

            const double delay = (0.001 + 0.003 * m_size) * m_sampleRate;
            const int throatSize = static_cast<int> (m_throat.size());
            const double readPos = m_throatWrite - delay;
            const int i0 = static_cast<int> (std::floor (readPos));
            const float frac = static_cast<float> (readPos - i0);
            const int a = ((i0 % throatSize) + throatSize) % throatSize;
            const int b = (a + 1) % throatSize;
            const float delayed = m_throat[static_cast<size_t> (a)] + frac * (m_throat[static_cast<size_t> (b)] - m_throat[static_cast<size_t> (a)]);
            const float g = static_cast<float> (0.25 * m_size);
            const float combed = y + g * delayed;
            m_throat[static_cast<size_t> (m_throatWrite)] = std::abs (combed) > 1.0e-20f ? combed : 0.0f;
            m_throatWrite = (m_throatWrite + 1) % throatSize;
            // sqrt (1 - g^2) keeps the comb's power gain at one; the body peaks add about 2.5 dB at
            // full Size on a voice, taken back here so the knob changes colour rather than level.
            y = combed * std::sqrt (1.0f - g * g) * m_bodyTrim;
        }

        if (! std::isfinite (y))
        {
            for (auto& b : m_body)
                b.clear();
            std::fill (m_throat.begin(), m_throat.end(), 0.0f);
            m_grainCount = 0;
            y = 0.0f;
        }

        return y;
    }

    //==============================================================================
    float FormantShifter::nextRandom()
    {
        // xorshift32, uniform in [0, 1).
        m_random ^= m_random << 13;
        m_random ^= m_random >> 17;
        m_random ^= m_random << 5;
        return static_cast<float> (m_random >> 8) * (1.0f / 16777216.0f);
    }

    bool FormantShifter::searchPeak (std::int64_t from, std::int64_t to, double predicted, double tolerance,
                                     std::int64_t& best, float& bestValue)
    {
        if (to <= from)
            return false;

        float hi = -1.0e30f, lo = 1.0e30f;
        for (std::int64_t t = from; t <= to; ++t)
        {
            const float v = m_band[static_cast<size_t> (t & m_mask)];
            hi = std::max (hi, v);
            lo = std::min (lo, v);
        }

        // Slow polarity choice: the glottal pulse may point either way; follow whichever side
        // carries the larger peaks, with hysteresis so marks do not hop half a period.
        m_positivePeaks += 0.2f * (std::max (0.0f, hi) - m_positivePeaks);
        m_negativePeaks += 0.2f * (std::max (0.0f, -lo) - m_negativePeaks);
        if (m_polarity > 0.0f && m_negativePeaks > 1.4f * m_positivePeaks)
            m_polarity = -1.0f;
        else if (m_polarity < 0.0f && m_positivePeaks > 1.4f * m_negativePeaks)
            m_polarity = 1.0f;

        const float range = hi - lo;
        if (range < c_minMarkLevel)
            return false;

        const bool penalise = tolerance > 0.0;
        const float penalty = c_markPenalty * range;
        float bestScore = -1.0e30f;

        for (std::int64_t t = from; t <= to; ++t)
        {
            const float v = m_polarity * m_band[static_cast<size_t> (t & m_mask)];
            float score = v;
            if (penalise)
            {
                const float d = static_cast<float> ((static_cast<double> (t) - predicted) / tolerance);
                score -= penalty * d * d;
            }

            if (score > bestScore)
            {
                bestScore = score;
                best = t;
                bestValue = v;
            }
        }

        return true;
    }

    double FormantShifter::markCorrelation (std::int64_t mark, std::int64_t previous) const
    {
        // Normalised correlation of the period ending at `mark` with the one ending at `previous`
        // (band-limited signal): near 1 on a voice, small on noise.
        const std::int64_t length = mark - previous;
        if (length < 2 || length > 2 * static_cast<std::int64_t> (m_maxPeriod))
            return 0.0;

        double xy = 0.0, xx = 0.0, yy = 0.0;
        for (std::int64_t k = 1; k <= length; ++k)
        {
            const double a = m_band[static_cast<size_t> ((previous + k) & m_mask)];
            const double b = m_band[static_cast<size_t> ((previous - length + k) & m_mask)];
            xy += a * b;
            xx += a * a;
            yy += b * b;
        }

        const double norm = std::sqrt (xx * yy);
        return norm > 1.0e-20 ? xy / norm : 0.0;
    }

    void FormantShifter::pushMark (std::int64_t pos, float period)
    {
        if (m_markCount == c_maxMarks)
        {
            m_markHead = (m_markHead + 1) % c_maxMarks;
            --m_markCount;
        }

        m_marks[static_cast<size_t> ((m_markHead + m_markCount) % c_maxMarks)] = { pos, period };
        ++m_markCount;
    }

    void FormantShifter::findMarks()
    {
        const VoiceInfo& v = m_detector.getVoiceInfo();
        if (! v.voiced || v.f0Hz <= 0.0f)
        {
            m_haveLastMark = false;
            return;
        }

        const std::int64_t newest = m_writePos - 1;
        const double period = std::clamp (m_sampleRate / v.f0Hz, static_cast<double> (m_minPeriod), static_cast<double> (m_maxPeriod));
        const double tolerance = c_markTolerance * period;
        const std::int64_t newestMark = m_markCount > 0
                                          ? m_marks[static_cast<size_t> ((m_markHead + m_markCount - 1) % c_maxMarks)].pos
                                          : std::numeric_limits<std::int64_t>::min() / 2;

        if (m_haveLastMark && static_cast<double> (newest - m_lastMark) <= 3.0 * period)
        {
            const double predicted = static_cast<double> (m_lastMark) + period;
            if (static_cast<double> (newest) < predicted + tolerance + 2.0)
                return;

            std::int64_t best = 0;
            float value = 0.0f;
            const auto from = static_cast<std::int64_t> (std::ceil (predicted - tolerance));
            const auto to = static_cast<std::int64_t> (std::floor (predicted + tolerance));

            // The detector judges a window ~20 ms old and needs a few weak hops to let go, so it still
            // says voiced well into a following fricative. A mark only counts if the period it closes
            // resembles the one before; otherwise the run ends here and that noise gets noise grains
            // instead of a buzz at the old pitch.
            if (! searchPeak (from, to, predicted, tolerance, best, value)
                || markCorrelation (best, m_lastMark) < c_minMarkCorrelation)
            {
                m_haveLastMark = false;
                return;
            }

            // The detector's period goes with the mark as a fallback and for the plausibility checks;
            // the grain spacing itself comes from the marks around the grain (localPeriod).
            pushMark (best, static_cast<float> (period));
            m_lastMark = best;
            return;
        }

        // Start of a voiced run: the strongest peak in the last-but-one half period..1.5 periods,
        // then walk back towards the onset so the first grains of the vowel are voiced too.
        // A failed start is retried half a period later, not on every sample.
        if (newest < m_startRetry)
            return;
        m_startRetry = newest + static_cast<std::int64_t> (0.5 * period);

        auto from = newest - static_cast<std::int64_t> (std::lround (1.5 * period));
        const auto to = newest - static_cast<std::int64_t> (std::lround (0.5 * period));
        from = std::max (from, newestMark + static_cast<std::int64_t> (0.5 * period));

        // Fresh polarity decision for a new run.
        {
            float hi = 0.0f, lo = 0.0f;
            for (std::int64_t t = from; t <= to; ++t)
            {
                const float s = m_band[static_cast<size_t> (t & m_mask)];
                hi = std::max (hi, s);
                lo = std::min (lo, s);
            }
            m_polarity = -lo > hi ? -1.0f : 1.0f;
            m_positivePeaks = hi;
            m_negativePeaks = -lo;
        }

        std::int64_t first = 0;
        float firstValue = 0.0f;
        if (! searchPeak (from, to, 0.0, 0.0, first, firstValue) || firstValue < c_minMarkLevel)
            return;

        // The run only starts where the signal really repeats: the period before the first mark must
        // look like the one before that (the detector's verdict belongs to older sound).
        {
            const double predicted = static_cast<double> (first) - period;
            std::int64_t previous = 0;
            float value = 0.0f;
            const float savedPolarity = m_polarity, savedPositive = m_positivePeaks, savedNegative = m_negativePeaks;
            const bool ok = searchPeak (static_cast<std::int64_t> (std::ceil (predicted - tolerance)),
                                        static_cast<std::int64_t> (std::floor (predicted + tolerance)),
                                        predicted, tolerance, previous, value);
            m_polarity = savedPolarity;
            m_positivePeaks = savedPositive;
            m_negativePeaks = savedNegative;
            if (! ok || markCorrelation (first, previous) < c_minMarkCorrelation)
                return;
        }

        std::array<std::int64_t, 8> back {};
        int numBack = 0;
        const std::int64_t outPos = newest - m_latency;
        const std::int64_t limit = std::max (newestMark + static_cast<std::int64_t> (0.5 * period),
                                             outPos + static_cast<std::int64_t> (period));
        std::int64_t mark = first;

        while (numBack < static_cast<int> (back.size()))
        {
            const double predicted = static_cast<double> (mark) - period;
            const auto bFrom = static_cast<std::int64_t> (std::ceil (predicted - tolerance));
            const auto bTo = static_cast<std::int64_t> (std::floor (predicted + tolerance));
            if (bFrom <= limit)
                break;

            std::int64_t found = 0;
            float value = 0.0f;
            const float savedPolarity = m_polarity;
            const bool ok = searchPeak (bFrom, bTo, predicted, tolerance, found, value);
            m_polarity = savedPolarity;  // the onset decides polarity, not the quiet lead-in
            if (! ok || value < 0.2f * firstValue || markCorrelation (mark, found) < c_minMarkCorrelation)
                break;

            back[static_cast<size_t> (numBack++)] = found;
            mark = found;
        }

        for (int i = numBack - 1; i >= 0; --i)
            pushMark (back[static_cast<size_t> (i)], static_cast<float> (period));

        pushMark (first, static_cast<float> (period));
        m_lastMark = first;
        m_haveLastMark = true;
    }

    //==============================================================================
    void FormantShifter::scheduleGrains (std::int64_t outPos)
    {
        // Forget marks no grain can use any more.
        const std::int64_t oldest = outPos - 5 * static_cast<std::int64_t> (m_maxPeriod);
        while (m_markCount > 0 && m_marks[static_cast<size_t> (m_markHead)].pos < oldest)
        {
            m_markHead = (m_markHead + 1) % c_maxMarks;
            --m_markCount;
        }

        const VoiceInfo& v = m_detector.getVoiceInfo();
        double lead = m_unvoicedHalfWidth;
        if (v.voiced && v.f0Hz > 0.0f)
            lead = std::max (lead, std::min (static_cast<double> (m_maxPeriod), m_sampleRate / v.f0Hz) / m_rate);

        // Create each grain a little before its window starts.
        int guard = 0;
        while (m_nextSyn - lead - 1.0 <= static_cast<double> (outPos) && guard++ < 8)
            createGrain (outPos);
    }

    void FormantShifter::createGrain (std::int64_t outPos)
    {
        const double now = static_cast<double> (outPos);
        double syn = m_nextSyn;

        // Nearest known analysis mark to the synthesis point.
        int nearest = -1;
        for (int i = m_markCount - 1; i >= 0; --i)
        {
            const int idx = (m_markHead + i) % c_maxMarks;
            if (static_cast<double> (m_marks[static_cast<size_t> (idx)].pos) <= syn)
            {
                nearest = i;
                if (i + 1 < m_markCount)
                {
                    const int next = (m_markHead + i + 1) % c_maxMarks;
                    if (static_cast<double> (m_marks[static_cast<size_t> (next)].pos) - syn
                        < syn - static_cast<double> (m_marks[static_cast<size_t> (idx)].pos))
                        nearest = i + 1;
                }
                break;
            }
        }
        if (nearest < 0 && m_markCount > 0)
            nearest = 0;

        bool voiced = false;
        Mark mark {};
        if (nearest >= 0)
        {
            mark = m_marks[static_cast<size_t> ((m_markHead + nearest) % c_maxMarks)];
            const double distance = syn - static_cast<double> (mark.pos);
            const bool newest = nearest == m_markCount - 1;
            if (std::abs (distance) <= 1.5 * mark.period)
                voiced = true;
            else if (newest && distance > 0.0 && distance <= 3.0 * mark.period && m_detector.getVoiceInfo().voiced)
                voiced = true;  // marks lag behind at low pitch: borrow the latest one
        }

        // Shape and place the voiced grain. It may never read input that has not arrived yet: with
        // rate < 1 its read position leads real time the most at its start, so check there and step
        // back a period (to the previous mark) if needed; at the lowest pitches this can happen.
        double period = 0.0, halfWidth = 0.0, voicedSyn = syn;
        while (voiced)
        {
            period = std::clamp (localPeriod (nearest), static_cast<double> (m_minPeriod), static_cast<double> (m_maxPeriod));
            halfWidth = period / m_rate;
            voicedSyn = m_prevVoiced ? syn : m_prevSyn + 0.5 * (m_prevHalfWidth + halfWidth);
            voicedSyn = std::max (voicedSyn, now + halfWidth);

            const double lead = static_cast<double> (mark.pos) - voicedSyn;
            if (lead <= m_latency - halfWidth * (1.0 - m_rate) - 4.0)
                break;

            if (nearest == 0)
            {
                voiced = false;
                break;
            }

            --nearest;
            mark = m_marks[static_cast<size_t> ((m_markHead + nearest) % c_maxMarks)];
            if (voicedSyn - static_cast<double> (mark.pos) > 3.0 * mark.period)
                voiced = false;
        }

        if (m_grainCount >= c_maxGrains)
        {
            // Should not happen (overlap is at most ~4); keep time moving.
            m_nextSyn += voiced ? mark.period / m_ratio : m_unvoicedHalfWidth;
            return;
        }

        Grain g;
        g.rate = m_rate;

        if (voiced)
        {
            g.halfWidth = halfWidth;
            syn = voicedSyn;
            g.anaCentre = static_cast<double> (mark.pos);
            g.gain = voicedGain (m_ratio, m_rate);
            g.voiced = true;
            m_nextSyn = syn + period / m_ratio;
            ++m_voicedGrains;
        }
        else
        {
            g.halfWidth = m_unvoicedHalfWidth;
            if (m_prevVoiced)
                syn = m_prevSyn + 0.5 * (m_prevHalfWidth + g.halfWidth);
            syn = std::max (syn, now + g.halfWidth);

            // At Size 0 the noise grains read exactly where they play (Hann at 50 % overlap sums to
            // one: unvoiced sound passes untouched). With Size the stretched grains no longer line up,
            // overlaps add incoherently and a Hann sum would dip at the grain rate (a 100 Hz buzz), so
            // the window moves towards a power-complementary sine and the read positions are
            // randomised within +-1/4 pseudo-period, each at least a tenth of one away from the last,
            // so no grain-rate pattern survives.
            const double spread = std::clamp ((1.0 - m_rate) / 0.1, 0.0, 1.0);
            const double range = 0.25 * m_unvoicedHalfWidth;
            const double step = (0.1 + 0.3 * nextRandom()) * m_unvoicedHalfWidth;
            double offset = m_unvoicedOffset + (nextRandom() < 0.5f ? -step : step);
            if (offset > range)
                offset = 2.0 * range - offset;
            if (offset < -range)
                offset = -2.0 * range - offset;
            m_unvoicedOffset = std::clamp (offset, -range, range);

            g.anaCentre = syn + spread * m_unvoicedOffset;
            g.gain = 1.0f;
            g.hann = static_cast<float> (1.0 - spread);
            g.voiced = false;
            m_nextSyn = syn + m_unvoicedHalfWidth;
            ++m_unvoicedGrains;
        }

        g.synCentre = syn;
        g.invHalfWidth = 1.0 / g.halfWidth;
        m_grains[static_cast<size_t> (m_grainCount++)] = g;

        m_prevSyn = syn;
        m_prevHalfWidth = g.halfWidth;
        m_prevVoiced = voiced;
    }

    double FormantShifter::localPeriod (int index) const
    {
        // Mean spacing of up to two marks either side: exactly the period at this grain (the
        // detector's estimate belongs to a window ~20 ms away, which on gliding speech is tens of
        // cents off), while averaging over up to four cycles smooths the few-sample wander of the
        // peak positions that would otherwise become period jitter in the output.
        const Mark& centre = m_marks[static_cast<size_t> ((m_markHead + index) % c_maxMarks)];
        const double nominal = centre.period;
        int lo = index, hi = index;

        auto spacingOk = [&] (int a, int b)
        {
            const double d = static_cast<double> (m_marks[static_cast<size_t> ((m_markHead + b) % c_maxMarks)].pos
                                                  - m_marks[static_cast<size_t> ((m_markHead + a) % c_maxMarks)].pos);
            return d > 0.6 * nominal && d < 1.4 * nominal;
        };

        for (int k = 0; k < 2; ++k)
        {
            if (lo - 1 >= 0 && spacingOk (lo - 1, lo))
                --lo;
            if (hi + 1 < m_markCount && spacingOk (hi, hi + 1))
                ++hi;
        }

        if (hi == lo)
            return nominal;

        const double span = static_cast<double> (m_marks[static_cast<size_t> ((m_markHead + hi) % c_maxMarks)].pos
                                                 - m_marks[static_cast<size_t> ((m_markHead + lo) % c_maxMarks)].pos);
        return span / (hi - lo);
    }

    float FormantShifter::renderGrains (std::int64_t outPos)
    {
        const double now = static_cast<double> (outPos);
        float y = 0.0f;

        for (int i = 0; i < m_grainCount;)
        {
            const Grain& g = m_grains[static_cast<size_t> (i)];
            const double d = now - g.synCentre;

            if (d >= g.halfWidth)
            {
                m_grains[static_cast<size_t> (i)] = m_grains[static_cast<size_t> (m_grainCount - 1)];
                --m_grainCount;
                continue;
            }

            if (d > -g.halfWidth)
            {
                const float w = window (d * g.invHalfWidth, g.hann);
                y += g.gain * w * readInput (g.anaCentre + d * g.rate);
            }

            ++i;
        }

        return y;
    }

    float FormantShifter::readInput (double pos) const
    {
        // 4-point cubic Hermite.
        const double fl = std::floor (pos);
        const auto i = static_cast<std::int64_t> (fl);
        const float t = static_cast<float> (pos - fl);
        const float xm1 = m_input[static_cast<size_t> ((i - 1) & m_mask)];
        const float x0 = m_input[static_cast<size_t> (i & m_mask)];
        const float x1 = m_input[static_cast<size_t> ((i + 1) & m_mask)];
        const float x2 = m_input[static_cast<size_t> ((i + 2) & m_mask)];
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * t + c2) * t + c1) * t + x0;
    }

    float FormantShifter::window (double u, float hann) const
    {
        const double a = std::min (1.0, std::abs (u)) * c_windowSize;
        const int i = static_cast<int> (a);
        const float f = static_cast<float> (a - i);
        const float c = m_cosTable[static_cast<size_t> (i)] + f * (m_cosTable[static_cast<size_t> (i + 1)] - m_cosTable[static_cast<size_t> (i)]);
        // hann 1: cos^2 (Hann, sums to one at 50 % overlap of coherent content);
        // hann 0: cos (sine window, power-complementary for incoherent content).
        return c * ((1.0f - hann) + hann * c);
    }

    float FormantShifter::voicedGain (double ratio, double rate) const
    {
        // Grains placed ratio times as often, each 1 / rate times longer, add mostly incoherently:
        // output power goes as ratio / rate. Measured on vowels this law holds within ~1 dB.
        return static_cast<float> (std::sqrt (rate / ratio));
    }

    void FormantShifter::updateBody()
    {
        const bool active = m_size > 1.0e-4;
        if (active && ! m_bodyActive)
        {
            for (auto& b : m_body)
                b.clear();
            std::fill (m_throat.begin(), m_throat.end(), 0.0f);
        }
        m_bodyActive = active;

        if (! active)
            return;

        m_bodyTrim = static_cast<float> (std::pow (10.0, -2.5 * m_size / 20.0));
        const double scale = 1.0 - 0.4 * m_size;
        for (size_t i = 0; i < m_body.size(); ++i)
            m_body[i].setPeaking (m_sampleRate, c_bodyHz[i] * scale, c_bodyQ[i], c_bodyDb[i] * m_size);
    }
}
