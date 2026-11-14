#pragma once

#include "VoiceStream.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace roar
{
    /**
     * Decides which frames of the voice stream may move a held carrier pitch (VOCODER's pulse
     * stack, RING's Follow).
     *
     * The detector reports a pitch whenever it finds periodicity, also on the noise floor
     * between phrases and on the tail of a word 30 dB down, where the estimate is usually an
     * octave or more off. A carrier that follows those frames starts the next phrase from the
     * wrong pitch and swoops into place. So a frame is accepted only when
     *   - it is voiced, with confidence >= c_minConfidence and f0 inside the caller's range, and
     *   - the audio at the module is loud enough: its level (the peak of the last
     *     c_levelWindowSeconds, long enough to span a period of the lowest voice) is within
     *     c_rangeDb of the recent level (that peak with a release of c_referenceDbPerSecond) and
     *     above c_floorDb. The release is quick enough that a murmur after a roar is followed
     *     within a second (a phrase 40 dB under the last one: 10 dB to fall), and slow enough
     *     that the tail of a word, which falls far faster, stays out.
     * The first accepted frame after c_phraseSeconds without one is flagged as a phrase start:
     * the caller takes that pitch at once, without its glide or octave check.
     *
     * A phrase usually ends well below the pitch the next one starts on, and the next pitch is
     * only known some tens of milliseconds into the phrase. So once the audio has been quiet for
     * c_restSeconds the gate hands out a resting pitch, the voice's usual one (the median of the
     * last c_usualCount accepted pitches, one taken every c_usualStepSeconds of accepted time, so
     * the odd octave error does not move it): the caller moves its held pitch there during the
     * pause, and the next phrase starts from it.
     *
     * tick() runs once per control interval with the peak |x| of the interval. Plain copyable
     * value type; reset() clears the levels (a transport restart starts from the floor alone).
     */
    class CarrierPitchGate
    {
    public:
        static constexpr double c_minConfidence = 0.4;
        static constexpr double c_rangeDb = 30.0;
        static constexpr double c_floorDb = -50.0;
        static constexpr double c_levelWindowSeconds = 0.025;
        static constexpr double c_referenceDbPerSecond = 12.0;
        static constexpr double c_phraseSeconds = 0.080;
        static constexpr double c_restSeconds = 0.150;
        static constexpr double c_usualStepSeconds = 0.050;
        static constexpr int c_usualCount = 32;

        struct Decision
        {
            bool accepted = false;      // this frame's f0 may be followed
            bool phraseStart = false;   // ... and it is the first after a gap
            float restHz = 0.0f;        // > 0, once per pause: move the held pitch here
        };

        void prepare (double fs, int tickSamples)
        {
            const double rate = (fs > 0.0 ? fs : 48000.0) / static_cast<double> (std::max (1, tickSamples));
            m_bucketTicks = std::max (1, static_cast<int> (std::lround (0.5 * c_levelWindowSeconds * rate)));
            m_referenceDecay = static_cast<float> (std::pow (10.0, -c_referenceDbPerSecond / (20.0 * rate)));
            m_phraseTicks = std::max (1, static_cast<int> (std::lround (c_phraseSeconds * rate)));
            m_restTicks = std::max (1, static_cast<int> (std::lround (c_restSeconds * rate)));
            m_usualStepTicks = std::max (1, static_cast<int> (std::lround (c_usualStepSeconds * rate)));
            m_range = static_cast<float> (std::pow (10.0, -c_rangeDb / 20.0));
            m_floor = static_cast<float> (std::pow (10.0, c_floorDb / 20.0));
            reset();
        }

        void reset()
        {
            m_bucket = m_lastBucket = 0.0f;
            m_bucketCount = 0;
            m_reference = 0.0f;
            m_gapTicks = m_phraseTicks;
            m_quietTicks = 0;
            m_usualFilled = 0;
            m_usualWrite = 0;
            m_usualCountdown = 0;
        }

        Decision tick (float peak, const VoiceFrame& frame, float minHz, float maxHz)
        {
            if (! (peak < 1.0e4f))   // also NaN
                peak = 0.0f;
            // The peak of the last half to whole window: two buckets of half a window each.
            m_bucket = std::max (m_bucket, peak);
            const float level = std::max (m_bucket, m_lastBucket);
            if (++m_bucketCount >= m_bucketTicks)
            {
                m_lastBucket = m_bucket;
                m_bucket = 0.0f;
                m_bucketCount = 0;
            }
            m_reference = std::max (level, m_reference * m_referenceDecay);
            if (m_reference < 1.0e-12f)
                m_reference = 0.0f;

            Decision d;
            const bool loud = level > std::max (m_reference * m_range, m_floor);
            d.accepted = loud && frame.voiced && frame.confidence >= static_cast<float> (c_minConfidence)
                         && frame.f0Hz >= minHz && frame.f0Hz <= maxHz;
            if (d.accepted)
            {
                d.phraseStart = m_gapTicks >= m_phraseTicks;
                m_gapTicks = 0;
                if (--m_usualCountdown <= 0)
                {
                    m_usualCountdown = m_usualStepTicks;
                    m_usual[static_cast<size_t> (m_usualWrite)] = frame.f0Hz;
                    m_usualWrite = (m_usualWrite + 1) % c_usualCount;
                    m_usualFilled = std::min (m_usualFilled + 1, c_usualCount);
                }
            }
            else if (m_gapTicks < m_phraseTicks)
            {
                ++m_gapTicks;
            }

            if (loud)
                m_quietTicks = 0;
            else if (m_quietTicks < m_restTicks && ++m_quietTicks == m_restTicks && m_usualFilled > 0)
                d.restHz = usualPitch();
            return d;
        }

    private:
        float usualPitch() const
        {
            std::array<float, c_usualCount> sorted = m_usual;
            const auto middle = sorted.begin() + m_usualFilled / 2;
            std::nth_element (sorted.begin(), middle, sorted.begin() + m_usualFilled);
            return *middle;
        }

        float m_referenceDecay = 1.0f;
        float m_range = 0.0316f, m_floor = 0.00316f;
        int m_phraseTicks = 1, m_bucketTicks = 1, m_restTicks = 1, m_usualStepTicks = 1;

        float m_bucket = 0.0f, m_lastBucket = 0.0f;   // peak of the running / the last half window
        int m_bucketCount = 0;
        float m_reference = 0.0f;
        int m_gapTicks = 1;
        int m_quietTicks = 0;
        std::array<float, c_usualCount> m_usual {};   // recent accepted pitches (a ring)
        int m_usualFilled = 0, m_usualWrite = 0, m_usualCountdown = 0;
    };
}
