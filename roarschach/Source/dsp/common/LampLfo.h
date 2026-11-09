#pragma once

#include "DspMath.h"

namespace roar
{
    /**
     * Lamp / light-dependent-resistor LFO (tremolo-phaser §1.5 item 4, §2.4): the lamp only
     * strikes above a threshold, and the cell responds fast when lit and slowly when it goes dark.
     *
     *   lamp = max (0, sin (2 pi phase) - threshold) / (1 - threshold)
     *   light += (lamp > light ? aOn : aOff) (lamp - light)
     *   aX = 1 - exp (-1 / (tauX k fs)),  k = min (1, referenceHz / rateHz)
     *
     * Scaling both times by k keeps the shape at fast rates. The caller owns the phase (so a
     * phase-locked or free-running LFO can drive it) and maps light to its gain law:
     *   GROWL Opto:  tauOn 3 ms, tauOff 25 ms, reference 8 Hz, s = 1 - light^0.8
     *   SWIRL Lamp:  tauOn 30 ms, tauOff 180 ms, reference 1 Hz, s = light^0.7
     *
     * setRate() computes two exps: call it per block or per control frame, not per sample.
     * Plain copyable value type, no allocation.
     */
    class LampLfo
    {
    public:
        void prepare (double sampleRate)
        {
            m_fs = dsp::safeSampleRate (sampleRate);
            setRate (m_rate);
            reset();
        }

        void setTimes (double tauOnSeconds, double tauOffSeconds, double referenceHz, float threshold = 0.2f)
        {
            m_tauOn = tauOnSeconds;
            m_tauOff = tauOffSeconds;
            m_referenceHz = referenceHz;
            m_threshold = std::clamp (threshold, 0.0f, 0.95f);
            setRate (m_rate);
        }

        void setRate (double rateHz)
        {
            m_rate = std::max (rateHz, 1.0e-3);
            const double k = std::min (1.0, m_referenceHz / m_rate);
            m_aOn = static_cast<float> (dsp::onePoleCoeff (m_tauOn * k, m_fs));
            m_aOff = static_cast<float> (dsp::onePoleCoeff (m_tauOff * k, m_fs));
        }

        void reset (float light = 0.0f) { m_light = light; }

        /** The raw lamp drive for a phase (0..1), before the cell's lag. */
        float lampFor (double phase01) const
        {
            const float s = static_cast<float> (std::sin (dsp::c_twoPi * phase01));
            return std::max (0.0f, s - m_threshold) / (1.0f - m_threshold);
        }

        /** Advance one sample at the given phase; returns the light, 0..1. */
        float process (double phase01)
        {
            const float lamp = lampFor (phase01);
            m_light += (lamp > m_light ? m_aOn : m_aOff) * (lamp - m_light);
            m_light = dsp::flushTiny (m_light);
            return m_light;
        }

        float getLight() const { return m_light; }

        void copyStateFrom (const LampLfo& other) { m_light = other.m_light; }

    private:
        double m_fs = 48000.0;
        double m_rate = 1.0;
        double m_tauOn = 0.003, m_tauOff = 0.025, m_referenceHz = 8.0;
        float m_threshold = 0.2f;
        float m_aOn = 1.0f, m_aOff = 1.0f;
        float m_light = 0.0f;
    };
}
