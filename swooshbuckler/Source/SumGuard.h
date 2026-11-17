#pragma once

#include <JuceHeader.h>

#include <algorithm>
#include <cmath>

/**
 * A guard over what you hear when a take is auditioned while the rig is playing.
 *
 * The rig's output is held under the Ceiling by its limiter, and so is an auditioned take, each on its own. Added
 * together they can reach 6 dB over it. The guard watches the sum and, only when a sample of it would go over the
 * Ceiling, turns every channel down together by just enough, holds that for 30 ms and lets go over 150 ms (it is
 * out of the way altogether within half a second): the
 * take ducks the rig (and the rig the take) for as long as the two together are too loud.
 *
 * It adds no delay, so the rig is heard exactly when it was before. That means it cannot see an over coming: the
 * first sample of one is turned down in one step (the limiter proper has 1.5 ms to do it in). It reads the
 * samples, not the waveform between them. While the sum stays under the Ceiling, which it always does with no
 * take playing, every sample passes untouched, bit for bit.
 *
 * No allocation: safe for the audio thread.
 */
class SumGuard
{
public:
    void prepare(double sampleRate)
    {
        const double rate = sampleRate > 0.0 ? sampleRate : 48000.0;
        m_holdSamples = static_cast<int>(0.03 * rate);
        m_release = static_cast<float>(1.0 - std::exp(-1.0 / (0.15 * rate)));
        m_step = static_cast<float>(1.0 / (0.5 * rate)); // (so it is back at exactly 1, untouched, within half a second)
        reset();
    }

    void reset()
    {
        m_gain = 1.0f;
        m_hold = 0;
    }

    /** Turning down now, or still letting go. */
    bool isWorking() const { return m_gain < 1.0f; }

    /** In place. ceiling: the Ceiling as a gain. */
    void process(juce::AudioBuffer<float>& audio, int start, int numSamples, float ceiling)
    {
        const int channels = audio.getNumChannels();
        for (int i = start; i < start + numSamples; ++i)
        {
            float peak = 0.0f;
            for (int ch = 0; ch < channels; ++ch)
                peak = std::max(peak, std::abs(audio.getSample(ch, i)));

            // (A millionth under, so rounding in the multiply cannot carry a sample over. False for NaN: left alone.)
            const float needed = peak > ceiling ? 0.999999f * ceiling / peak : 1.0f;
            if (needed < m_gain)
            {
                m_gain = needed;
                m_hold = m_holdSamples;
            }
            else if (m_gain < 1.0f)
            {
                if (m_hold > 0)
                    --m_hold;
                else
                    m_gain = std::min(needed, m_gain + m_release * (1.0f - m_gain) + m_step);
                if (m_gain >= 1.0f)
                    m_gain = 1.0f;
            }

            if (m_gain < 1.0f)
                for (int ch = 0; ch < channels; ++ch)
                    audio.setSample(ch, i, audio.getSample(ch, i) * m_gain);
        }
    }

private:
    float m_gain = 1.0f, m_release = 0.0f, m_step = 0.0f;
    int m_hold = 0, m_holdSamples = 1;
};
