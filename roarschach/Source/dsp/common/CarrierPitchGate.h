#pragma once

#include "VoiceStream.h"

#include <algorithm>
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
     *   - the audio at the module is loud enough: its level (a peak follower, 20 ms release) is
     *     within c_rangeDb of the recent level (the same follower with a release of
     *     c_referenceDbPerSecond) and above c_floorDb.
     * The first accepted frame after c_phraseSeconds without one is flagged as a phrase start:
     * the caller takes that pitch at once, without its glide or octave check.
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
        static constexpr double c_levelReleaseSeconds = 0.020;
        static constexpr double c_referenceDbPerSecond = 1.5;
        static constexpr double c_phraseSeconds = 0.080;

        struct Decision
        {
            bool accepted = false;      // this frame's f0 may be followed
            bool phraseStart = false;   // ... and it is the first after a gap
        };

        void prepare (double fs, int tickSamples)
        {
            const double rate = (fs > 0.0 ? fs : 48000.0) / static_cast<double> (std::max (1, tickSamples));
            m_levelDecay = static_cast<float> (std::exp (-1.0 / (c_levelReleaseSeconds * rate)));
            m_referenceDecay = static_cast<float> (std::pow (10.0, -c_referenceDbPerSecond / (20.0 * rate)));
            m_phraseTicks = std::max (1, static_cast<int> (std::lround (c_phraseSeconds * rate)));
            m_range = static_cast<float> (std::pow (10.0, -c_rangeDb / 20.0));
            m_floor = static_cast<float> (std::pow (10.0, c_floorDb / 20.0));
            reset();
        }

        void reset()
        {
            m_level = 0.0f;
            m_reference = 0.0f;
            m_gapTicks = m_phraseTicks;
        }

        Decision tick (float peak, const VoiceFrame& frame, float minHz, float maxHz)
        {
            if (! (peak < 1.0e4f))   // also NaN
                peak = 0.0f;
            m_level = std::max (peak, m_level * m_levelDecay);
            m_reference = std::max (m_level, m_reference * m_referenceDecay);
            if (m_reference < 1.0e-12f)
                m_reference = 0.0f;

            Decision d;
            const bool loud = m_level > std::max (m_reference * m_range, m_floor);
            d.accepted = loud && frame.voiced && frame.confidence >= static_cast<float> (c_minConfidence)
                         && frame.f0Hz >= minHz && frame.f0Hz <= maxHz;
            if (d.accepted)
            {
                d.phraseStart = m_gapTicks >= m_phraseTicks;
                m_gapTicks = 0;
            }
            else if (m_gapTicks < m_phraseTicks)
            {
                ++m_gapTicks;
            }
            return d;
        }

    private:
        float m_levelDecay = 0.0f, m_referenceDecay = 1.0f;
        float m_range = 0.0316f, m_floor = 0.00316f;
        int m_phraseTicks = 1;

        float m_level = 0.0f;
        float m_reference = 0.0f;
        int m_gapTicks = 1;
    };
}
