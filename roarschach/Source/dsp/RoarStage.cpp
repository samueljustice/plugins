#include "RoarStage.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace roar
{
    namespace
    {
        constexpr double c_pi = 3.141592653589793;
        constexpr double c_ln2 = 0.6931471805599453;

        constexpr double c_lowShelfHz = 180.0, c_highShelfHz = 3000.0;
        constexpr float c_maxEmphasisDb = 9.0f;
        constexpr float c_minDriveDb = 4.0f, c_maxDriveDb = 30.0f;
        constexpr float c_trimPerDriveDb = 0.72f;  // first guess of the wet trim before the level match has learnt
        constexpr double c_trimSeconds = 0.200;
        constexpr float c_trimGatePower = 1.0e-7f; // -70 dBFS: below this the trim holds
        constexpr float c_minTrim = 0.02f, c_maxTrim = 2.0f;
        constexpr float c_minBias = 0.15f, c_maxBias = 0.35f;

        constexpr float c_postMaxHz = 18000.0f, c_postMinHz = 9000.0f;

        constexpr double c_splitHz = 3500.0;
        constexpr float c_tameRatio = 4.0f;
        constexpr float c_tameKneeDb = 4.0f;
        constexpr float c_tameMaxDb = 6.0f;
        constexpr float c_tameAboveAverageDb = 3.0f;  // threshold sits this far above the band's average
        constexpr float c_tameMinThresholdDb = -48.0f, c_tameMaxThresholdDb = -6.0f;
        constexpr float c_tameGateDb = -60.0f;        // the average only learns from real signal
        constexpr double c_tameAttackSeconds = 0.003, c_tameReleaseSeconds = 0.080;
        constexpr int c_tameInterval = 16;            // shelf redesign interval (samples)
        constexpr double c_tameAverageSeconds = 1.5;     // falling
        constexpr double c_tameAverageRiseSeconds = 0.25;
        constexpr float c_tameStartDb = -30.0f;          // the average's starting point after reset

        constexpr double c_paramSeconds = 0.030;
        constexpr double c_dcHz = 5.0;

        float smoothstep (float x, float lo, float hi)
        {
            const float t = std::clamp ((x - lo) / (hi - lo), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }

        float dbToGain (float db)
        {
            return std::exp (db * 0.115129255f);
        }

        enum class Shape { lowPass, highPass, lowShelf, highShelf };

        template <typename Filter>
        void design (Filter& f, Shape shape, double hz, double gainDb, double sampleRate)
        {
            const double w = 2.0 * c_pi * std::min (hz, 0.45 * sampleRate) / sampleRate;
            const double cw = std::cos (w), sw = std::sin (w);
            const double alpha = sw / (2.0 * 0.7071067811865476);
            const double A = std::pow (10.0, gainDb / 40.0);
            const double sq = 2.0 * std::sqrt (A) * alpha;
            double b0 = 1.0, b1 = 0.0, b2 = 0.0, a0 = 1.0, a1 = 0.0, a2 = 0.0;

            switch (shape)
            {
                case Shape::lowPass:
                    b0 = (1.0 - cw) * 0.5; b1 = 1.0 - cw; b2 = b0;
                    a0 = 1.0 + alpha; a1 = -2.0 * cw; a2 = 1.0 - alpha;
                    break;
                case Shape::highPass:
                    b0 = (1.0 + cw) * 0.5; b1 = -(1.0 + cw); b2 = b0;
                    a0 = 1.0 + alpha; a1 = -2.0 * cw; a2 = 1.0 - alpha;
                    break;
                case Shape::lowShelf:
                    b0 = A * ((A + 1.0) - (A - 1.0) * cw + sq);
                    b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cw);
                    b2 = A * ((A + 1.0) - (A - 1.0) * cw - sq);
                    a0 = (A + 1.0) + (A - 1.0) * cw + sq;
                    a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cw);
                    a2 = (A + 1.0) + (A - 1.0) * cw - sq;
                    break;
                case Shape::highShelf:
                    b0 = A * ((A + 1.0) + (A - 1.0) * cw + sq);
                    b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cw);
                    b2 = A * ((A + 1.0) + (A - 1.0) * cw - sq);
                    a0 = (A + 1.0) - (A - 1.0) * cw + sq;
                    a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cw);
                    a2 = (A + 1.0) - (A - 1.0) * cw - sq;
                    break;
            }

            f.b0 = b0 / a0;
            f.b1 = b1 / a0;
            f.b2 = b2 / a0;
            f.a1 = a1 / a0;
            f.a2 = a2 / a0;
        }

        // Tube curve f(v) = (tanh(v + b) - tanh(b)) / sech^2(b): f(0) = 0, f'(0) = 1, the positive
        // side saturates first (even harmonics). F is its antiderivative (for ADAA).
        double logCosh (double z)
        {
            const double az = std::abs (z);
            return az + std::log1p (std::exp (-2.0 * az)) - c_ln2;
        }

        struct Curve
        {
            double bias = 0.0, tb = 0.0, invSlope = 1.0;

            void set (double b)
            {
                bias = b;
                tb = std::tanh (b);
                invSlope = 1.0 / (1.0 - tb * tb);
            }

            double f (double v) const { return (std::tanh (v + bias) - tb) * invSlope; }
            double F (double v) const { return (logCosh (v + bias) - tb * v) * invSlope; }
        };
    }

    RoarStage::RoarStage() = default;
    RoarStage::~RoarStage() = default;

    void RoarStage::prepare (double sampleRate, int maxBlockSize)
    {
        m_sampleRate = sampleRate > 1000.0 ? sampleRate : 48000.0;
        m_maxBlock = std::max (1, maxBlockSize);

        m_oversampling = std::make_unique<juce::dsp::Oversampling<float>> (
            2, 1, juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, true);
        m_oversampling->initProcessing (static_cast<size_t> (m_maxBlock));
        m_latency = static_cast<int> (std::lround (m_oversampling->getLatencyInSamples()));

        for (auto& p : m_pre)
            p.assign (static_cast<size_t> (m_maxBlock), 0.0f);

        m_monoRight.assign (static_cast<size_t> (m_maxBlock), 0.0f);
        m_segments.assign (static_cast<size_t> (m_maxBlock / c_subBlock + 3), Segment {});

        for (auto& c : m_channels)
        {
            design (c.detectA, Shape::highPass, c_splitHz, 0.0, m_sampleRate);
            design (c.detectB, Shape::highPass, c_splitHz, 0.0, m_sampleRate);
        }

        m_subBlockCoeff = static_cast<float> (1.0 - std::exp (-static_cast<double> (c_subBlock) / (c_paramSeconds * m_sampleRate)));
        m_dcR = static_cast<float> (std::exp (-2.0 * c_pi * c_dcHz / (2.0 * m_sampleRate)));  // runs at 2x
        m_tameAttack = static_cast<float> (1.0 - std::exp (-1.0 / (c_tameAttackSeconds * m_sampleRate)));
        m_tameRelease = static_cast<float> (1.0 - std::exp (-1.0 / (c_tameReleaseSeconds * m_sampleRate)));
        m_tameAverageCoeffBlock = static_cast<float> (1.0 - std::exp (-static_cast<double> (c_tameInterval) / (c_tameAverageSeconds * m_sampleRate)));
        m_tameAverageRiseBlock = static_cast<float> (1.0 - std::exp (-static_cast<double> (c_tameInterval) / (c_tameAverageRiseSeconds * m_sampleRate)));
        m_trimCoeff = static_cast<float> (1.0 - std::exp (-1.0 / (c_trimSeconds * 2.0 * m_sampleRate)));

        reset();
    }

    void RoarStage::reset()
    {
        if (m_oversampling != nullptr)
            m_oversampling->reset();

        for (auto& c : m_channels)
        {
            for (auto* b : { &c.preLow, &c.preHigh, &c.deLow, &c.deHigh, &c.post, &c.detectA, &c.detectB, &c.tame })
                b->clear();

            c.prevV = 0.0;
            c.prevU = 0.0f;
            c.dcX = c.dcY = 0.0f;
            c.designedEmphasisDb = -1.0f;
            c.designedPostHz = -1.0f;
        }

        m_tameEnv = 0.0f;
        m_tameAverageDb = c_tameStartDb;
        m_tameDb = 0.0f;
        m_tameCounter = 0;
        m_tameDisplayDb = 0.0f;
        m_dryMs = m_shapedMs = 0.0f;
        m_trim = -1.0f;  // unknown: set from the static estimate on the first chunk
        m_driveDisplay = 0.0f;
        m_ctlPos = 0;
        m_ctlValid = false;
        m_snap = true;
    }

    void RoarStage::setParams (float amount01)
    {
        m_target = std::isfinite (amount01) ? std::clamp (amount01, 0.0f, 1.0f) : 0.0f;

        if (m_snap)
        {
            m_amount = m_target;
            m_snap = false;
        }
    }

    RoarStage::Control RoarStage::controlFor (float a) const
    {
        Control c;
        c.amount = a;
        const float driveDb = c_minDriveDb + (c_maxDriveDb - c_minDriveDb) * a;
        c.drive = dbToGain (driveDb);
        c.trim = dbToGain (-c_trimPerDriveDb * driveDb);
        c.bias = c_minBias + (c_maxBias - c_minBias) * a;
        c.emphasisDb = c_maxEmphasisDb * a;
        c.postHz = c_postMaxHz * std::pow (c_postMinHz / c_postMaxHz, smoothstep (a, 0.4f, 0.85f));
        c.postBlend = smoothstep (a, 0.3f, 0.5f);
        return c;
    }

    void RoarStage::designEmphasis (Channel& c, float emphasisDb) const
    {
        if (std::equal_to<float>() (c.designedEmphasisDb, emphasisDb))
            return;

        c.designedEmphasisDb = emphasisDb;
        design (c.preLow, Shape::lowShelf, c_lowShelfHz, emphasisDb, m_sampleRate);
        design (c.preHigh, Shape::highShelf, c_highShelfHz, emphasisDb, m_sampleRate);
        // An RBJ shelf with the negated gain is the exact inverse (poles and zeros swap).
        design (c.deLow, Shape::lowShelf, c_lowShelfHz, -emphasisDb, m_sampleRate);
        design (c.deHigh, Shape::highShelf, c_highShelfHz, -emphasisDb, m_sampleRate);
    }

    bool RoarStage::stateIsFinite() const
    {
        for (const auto& c : m_channels)
        {
            for (const auto* b : { &c.preLow, &c.preHigh, &c.deLow, &c.deHigh, &c.post, &c.detectA, &c.detectB, &c.tame })
                if (! b->isFinite())
                    return false;

            if (! std::isfinite (c.prevV) || ! std::isfinite (c.prevU) || ! std::isfinite (c.dcY))
                return false;
        }

        return std::isfinite (m_tameEnv) && std::isfinite (m_tameAverageDb) && std::isfinite (m_amount)
            && std::isfinite (m_dryMs) && std::isfinite (m_shapedMs) && std::isfinite (m_trim);
    }

    void RoarStage::process (float* left, float* right, int numSamples, bool stereo, const VoiceInfo&)
    {
        if (m_oversampling == nullptr)
            return;

        const bool twoChannels = stereo && right != nullptr;

        for (int start = 0; start < numSamples; start += m_maxBlock)
        {
            const int n = std::min (m_maxBlock, numSamples - start);
            float* r = twoChannels ? right + start : m_monoRight.data();

            if (! twoChannels)
                std::copy (left + start, left + start + n, r);

            processChunk (left + start, r, n);
        }

        if (! stateIsFinite())
            reset();
    }

    void RoarStage::processChunk (float* left, float* right, int numSamples)
    {
        std::array<float*, 2> io { left, right };

        // ---- control: a new target every c_subBlock samples, on a phase kept across calls
        if (! m_ctlValid)
        {
            m_ctlTo = controlFor (m_amount);
            m_ctlValid = true;
        }

        int numSegments = 0;

        for (int i = 0; i < numSamples;)
        {
            if (m_ctlPos == 0)
            {
                m_ctlFrom = m_ctlTo;
                m_amount += m_subBlockCoeff * (m_target - m_amount);
                if (std::abs (m_target - m_amount) < 1.0e-5f)
                    m_amount = m_target;
                m_ctlTo = controlFor (m_amount);
            }

            const int len = std::min (numSamples - i, c_subBlock - m_ctlPos);
            auto& seg = m_segments[static_cast<size_t> (numSegments++)];
            seg.start = i;
            seg.end = i + len;
            seg.phase = m_ctlPos;
            seg.from = m_ctlFrom;
            seg.to = m_ctlTo;
            i += len;
            m_ctlPos = (m_ctlPos + len) % c_subBlock;
        }

        if (m_trim < 0.0f)
            m_trim = m_segments[0].to.trim;

        // ---- pre-emphasis (sanitising the input on the way)
        for (int ch = 0; ch < 2; ++ch)
        {
            auto& c = m_channels[static_cast<size_t> (ch)];
            float* pre = m_pre[static_cast<size_t> (ch)].data();

            for (int k = 0; k < numSegments; ++k)
            {
                const auto& seg = m_segments[static_cast<size_t> (k)];
                designEmphasis (c, seg.to.emphasisDb);

                for (int i = seg.start; i < seg.end; ++i)
                {
                    const float x = io[static_cast<size_t> (ch)][i];
                    pre[i] = c.preHigh.process (c.preLow.process (std::isfinite (x) ? x : 0.0f));
                }
            }
        }

        // ---- 2x: blend of dry and the ADAA tube curve
        const float* preChannels[2] = { m_pre[0].data(), m_pre[1].data() };
        juce::dsp::AudioBlock<const float> inBlock (preChannels, 2, static_cast<size_t> (numSamples));
        auto up = m_oversampling->processSamplesUp (inBlock);
        float peakV = 0.0f;
        float* os[2] = { up.getChannelPointer (0), up.getChannelPointer (1) };
        Curve curve;
        const float span = static_cast<float> (2 * c_subBlock);

        for (int k = 0; k < numSegments; ++k)
        {
            const auto& seg = m_segments[static_cast<size_t> (k)];
            const Control& from = seg.from;
            const Control& to = seg.to;
            curve.set (to.bias);
            const int j0 = 2 * seg.start, j1 = 2 * seg.end;
            const int phase2 = 2 * seg.phase;

            for (int j = j0; j < j1; ++j)
            {
                const float t = static_cast<float> (phase2 + j - j0 + 1) / span;
                const float drive = from.drive + (to.drive - from.drive) * t;
                const float wet = from.amount + (to.amount - from.amount) * t;
                float dryPower = 0.0f, shapedPower = 0.0f;
                float shapedOut[2], dryOut[2];

                for (int ch = 0; ch < 2; ++ch)
                {
                    auto& c = m_channels[static_cast<size_t> (ch)];
                    const float u = os[ch][j];
                    const double v = static_cast<double> (drive) * u;
                    const double dv = v - c.prevV;
                    // First-order ADAA: the average of the curve over the segment from the last sample.
                    const double shaped = std::abs (dv) > 1.0e-6 ? (curve.F (v) - curve.F (c.prevV)) / dv
                                                                 : curve.f (0.5 * (v + c.prevV));
                    // ADAA delays by half a sample: the dry is averaged the same way to stay aligned.
                    dryOut[ch] = 0.5f * (u + c.prevU);
                    // The bias puts DC on the shaped signal only: block it there, so the dry path
                    // keeps its exact phase (a DC blocker on everything would lead the lows).
                    const float sh = static_cast<float> (shaped);
                    shapedOut[ch] = sh - c.dcX + m_dcR * c.dcY;
                    c.dcX = sh;
                    c.dcY = shapedOut[ch];
                    c.prevV = v;
                    c.prevU = u;

                    dryPower += dryOut[ch] * dryOut[ch];
                    shapedPower += shapedOut[ch] * shapedOut[ch];
                    peakV = std::max (peakV, static_cast<float> (std::abs (v)));
                }

                // Level match of the shaped signal to the dry (stereo-linked, ~200 ms), so the
                // loudness stays put whatever the drive and the input level. Held in pauses.
                m_dryMs += m_trimCoeff * (dryPower - m_dryMs);
                m_shapedMs += m_trimCoeff * (shapedPower - m_shapedMs);

                if (m_dryMs > c_trimGatePower && m_shapedMs > 1.0e-20f)
                    m_trim = std::clamp (std::sqrt (m_dryMs / m_shapedMs), c_minTrim, c_maxTrim);

                for (int ch = 0; ch < 2; ++ch)
                    os[ch][j] = dryOut[ch] + wet * (shapedOut[ch] * m_trim - dryOut[ch]);
            }
        }

        float* outChannels[2] = { left, right };
        juce::dsp::AudioBlock<float> outBlock (outChannels, 2, static_cast<size_t> (numSamples));
        m_oversampling->processSamplesDown (outBlock);

        // ---- de-emphasis, post LPF
        for (int ch = 0; ch < 2; ++ch)
        {
            auto& c = m_channels[static_cast<size_t> (ch)];
            float* x = io[static_cast<size_t> (ch)];

            for (int k = 0; k < numSegments; ++k)
            {
                const auto& seg = m_segments[static_cast<size_t> (k)];
                const Control& ctl = seg.to;
                designEmphasis (c, ctl.emphasisDb);
                if (! std::equal_to<float>() (c.designedPostHz, ctl.postHz))
                {
                    c.designedPostHz = ctl.postHz;
                    design (c.post, Shape::lowPass, ctl.postHz, 0.0, m_sampleRate);
                }

                for (int i = seg.start; i < seg.end; ++i)
                {
                    float y = c.deHigh.process (c.deLow.process (x[i]));
                    const float lp = c.post.process (y);
                    y += ctl.postBlend * (lp - y);

                    x[i] = y;
                }
            }
        }

        // ---- tame: a dynamic high shelf at 3.5 kHz driven by the top band's level (stereo-linked).
        // A shelf at 0 dB is exactly flat, so with no reduction nothing at all changes, and the
        // lows are never touched (no crossover allpass in the audio path).
        float maxTame = 0.0f;
        const float slope = 1.0f - 1.0f / c_tameRatio;

        int segment = 0;

        for (int i = 0; i < numSamples; ++i)
        {
            float highPower = 0.0f;

            while (i >= m_segments[static_cast<size_t> (segment)].end)
                ++segment;

            for (int ch = 0; ch < 2; ++ch)
            {
                auto& c = m_channels[static_cast<size_t> (ch)];
                const float h = c.detectB.process (c.detectA.process (io[static_cast<size_t> (ch)][i]));
                highPower = std::max (highPower, h * h);
            }

            m_tameEnv += (highPower > m_tameEnv ? m_tameAttack : m_tameRelease) * (highPower - m_tameEnv);

            const bool redesign = m_tameCounter == 0;
            m_tameCounter = (m_tameCounter + 1) % c_tameInterval;

            if (redesign)
            {
                const float envDb = 10.0f * std::log10 (std::max (m_tameEnv, 1.0e-14f));

                if (envDb > c_tameGateDb)
                {
                    // Rises fairly quickly (a louder passage) and falls slowly (a quieter one). It starts
                    // from a typical presence level, so a first word is never squashed while it learns.
                    m_tameAverageDb += (envDb > m_tameAverageDb ? m_tameAverageRiseBlock : m_tameAverageCoeffBlock)
                                       * (envDb - m_tameAverageDb);
                }

                const float threshold = std::clamp (m_tameAverageDb + c_tameAboveAverageDb, c_tameMinThresholdDb, c_tameMaxThresholdDb);
                const float over = envDb - threshold;
                float reduction = 0.0f;

                if (over >= 0.5f * c_tameKneeDb)
                    reduction = slope * over;
                else if (over > -0.5f * c_tameKneeDb)
                {
                    const float z = over + 0.5f * c_tameKneeDb;
                    reduction = slope * z * z / (2.0f * c_tameKneeDb);
                }

                const float a = m_segments[static_cast<size_t> (segment)].to.amount;
                reduction = std::min (reduction, c_tameMaxDb) * smoothstep (a, 0.0f, 0.25f);
                // Ease towards the new value so the shelf steps are tiny (the detector sets the timing).
                m_tameDb += 0.5f * (reduction - m_tameDb);

                for (auto& c : m_channels)
                    design (c.tame, Shape::highShelf, c_splitHz, -m_tameDb, m_sampleRate);

                maxTame = std::max (maxTame, m_tameDb);
            }

            io[0][i] = m_channels[0].tame.process (io[0][i]);
            io[1][i] = m_channels[1].tame.process (io[1][i]);
        }

        m_tameDisplayDb = maxTame;

        const float a = m_amount;
        const float hit = std::clamp (peakV / 3.0f, 0.0f, 1.0f);
        const float target = a * hit;
        m_driveDisplay = target > m_driveDisplay ? target : m_driveDisplay * 0.85f + target * 0.15f;
    }
}
