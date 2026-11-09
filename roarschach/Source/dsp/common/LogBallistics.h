#pragma once

#include "DspMath.h"

namespace roar
{
    /**
     * Smooth decoupled peak ballistics in the log domain (channel-strip §5.1), applied to a gain
     * change g in dB (g <= 0 is reduction):
     *
     *   g1[n] = min (g[n], aR g1[n-1] + (1 - aR) g[n])     release smoothing, instant drop
     *   y[n]  = aA y[n-1] + (1 - aA) g1[n]                 attack smoothing
     *
     * with aX = exp (-1 / (tauX fs)). A time of 0 makes that stage instant. For an upward (boost)
     * signal use processBoost(), the mirror image (instant rise of the release stage, attack
     * smoothing on the way up).
     *
     * State in double; denormal-safe; plain copyable value type.
     */
    class LogBallistics
    {
    public:
        void prepare (double sampleRate) { m_fs = dsp::safeSampleRate (sampleRate); setTimes (m_attackSeconds, m_releaseSeconds); }

        void setTimes (double attackSeconds, double releaseSeconds)
        {
            m_attackSeconds = attackSeconds;
            m_releaseSeconds = releaseSeconds;
            m_aA = dsp::poleFromTau (attackSeconds, m_fs);
            m_aR = dsp::poleFromTau (releaseSeconds, m_fs);
        }

        void reset (double valueDb = 0.0) { m_g1 = m_y = valueDb; }

        /** Reduction (g <= 0): instant drop into the release stage, attack smoothing after it. */
        double process (double gDb)
        {
            gDb = dsp::finiteOrZero (gDb);
            m_g1 = std::min (gDb, m_aR * m_g1 + (1.0 - m_aR) * gDb);
            m_y = m_aA * m_y + (1.0 - m_aA) * m_g1;
            m_g1 = dsp::flushTiny (m_g1);
            m_y = dsp::flushTiny (m_y);
            return m_y;
        }

        /** Boost (g >= 0): the mirror image of process(). */
        double processBoost (double gDb)
        {
            gDb = dsp::finiteOrZero (gDb);
            m_g1 = std::max (gDb, m_aR * m_g1 + (1.0 - m_aR) * gDb);
            m_y = m_aA * m_y + (1.0 - m_aA) * m_g1;
            m_g1 = dsp::flushTiny (m_g1);
            m_y = dsp::flushTiny (m_y);
            return m_y;
        }

        double getValue() const { return m_y; }

        void copyStateFrom (const LogBallistics& other) { m_g1 = other.m_g1; m_y = other.m_y; }

    private:
        double m_fs = 48000.0;
        double m_attackSeconds = 0.0, m_releaseSeconds = 0.0;
        double m_aA = 0.0, m_aR = 0.0;
        double m_g1 = 0.0, m_y = 0.0;
    };
}
