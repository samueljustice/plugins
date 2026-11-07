#include "ReverseEngine.h"

namespace
{
    // The original's float32 constants.
    constexpr float c_twoPiF = 6.2831855f;
    constexpr float c_piF = 3.1415927f;
    constexpr float c_invPiF = 0.31830987f;
    constexpr float c_degreesToRadians = 0.017453292f;

    // Initial phases, in degrees as stored in the original's presets:
    // voice 0 sweep / window, voice 1 sweep / window. With these, each window is
    // exactly zero where its sweep jumps back, and the two voices are half a period apart.
    constexpr std::array<float, 4> c_startDegrees { 270.0f, 0.0f, 90.0f, 180.0f };

    constexpr float c_vibratoHz = 5.0f;              // Reverse Repeat's second pass
    constexpr float c_vibratoDepth = 0.001f;         // relative delay modulation
    constexpr float c_vibratoStartDegrees = 270.0f;

    constexpr double c_timeCrossfadeSeconds = 0.020;
    constexpr double c_modeCrossfadeSeconds = 0.030;
    constexpr double c_minimumDelay = 2.0;           // samples; Lagrange needs one sample after the read point

    float wrapPhase(float phase)
    {
        while (phase < 0.0f)      phase += c_twoPiF;
        while (phase >= c_twoPiF) phase -= c_twoPiF;
        return phase;
    }

    /** The original's slow sawtooth: rises from 0 to +1 over [0, pi), jumps to -1 at pi, back to 0 at 2pi. */
    float sweep(float phase)
    {
        return phase < c_piF ? phase * c_invPiF : (c_twoPiF - phase) * (-c_invPiF);
    }

    float window(float phase)
    {
        return 0.5f + 0.5f * std::sin(phase);
    }

    /** Equal-gain cosine fade, 0 -> 1. */
    float fadeCurve(int position, int length)
    {
        return static_cast<float>(0.5 - 0.5 * std::cos(juce::MathConstants<double>::pi * (position + 0.5) / length));
    }
}

//==============================================================================
void ReverseEngine::DelayLine::allocate(int minimumLength)
{
    const int length = juce::nextPowerOfTwo(std::max(16, minimumLength));
    m_data.assign(static_cast<size_t>(length), 0.0f);
    m_mask = length - 1;
    m_write = 0;
}

void ReverseEngine::DelayLine::clear()
{
    std::fill(m_data.begin(), m_data.end(), 0.0f);
    m_write = 0;
}

float ReverseEngine::DelayLine::read(double delay) const
{
    // 4-point Lagrange interpolation between the samples around (write - delay).
    const double position = static_cast<double>(m_write) - std::max(delay, c_minimumDelay);
    const double whole = std::floor(position);
    const float d = static_cast<float>(position - whole);
    const int i = static_cast<int>(whole);

    const float x0 = m_data[static_cast<size_t>((i - 1) & m_mask)];
    const float x1 = m_data[static_cast<size_t>(i & m_mask)];
    const float x2 = m_data[static_cast<size_t>((i + 1) & m_mask)];
    const float x3 = m_data[static_cast<size_t>((i + 2) & m_mask)];

    const float dm1 = d - 1.0f, dm2 = d - 2.0f, dp1 = d + 1.0f;
    return -x0 * d * dm1 * dm2 * (1.0f / 6.0f)
         +  x1 * dp1 * dm1 * dm2 * 0.5f
         -  x2 * dp1 * d * dm2 * 0.5f
         +  x3 * dp1 * d * dm1 * (1.0f / 6.0f);
}

//==============================================================================
void ReverseEngine::Clock::start(float timeSeconds, float radiansPerSampleHz)
{
    time = timeSeconds;
    increment = radiansPerSampleHz * (1.0f / timeSeconds);
}

void ReverseEngine::Clock::advance()
{
    // Each oscillator accumulates its own float32 phase, as in the original.
    for (auto& p : phase)
        p = wrapPhase(increment + p);
}

void ReverseEngine::Clock::resync()
{
    // Float32 accumulation makes the four phases wander apart slightly; with a fixed TIME the
    // error comes back every period, but each TIME change would freeze some of it in. So a new
    // clock re-derives the others from voice 0's sweep, keeping every window at zero on its
    // jump and the voices half a period apart.
    phase[1] = wrapPhase(phase[0] + 0.5f * c_piF);
    phase[2] = wrapPhase(phase[0] + c_piF);
    phase[3] = wrapPhase(phase[0] - 0.5f * c_piF);
}

ReverseEngine::Taps ReverseEngine::tapsOf(const Clock& clock) const
{
    Taps taps;
    const float base = static_cast<float>(static_cast<double>(clock.time) * m_sampleRate);   // D0 = T * SR
    taps.delay[0] = base + base * sweep(clock.phase[0]);
    taps.delay[1] = base + base * sweep(clock.phase[2]);
    taps.window = clock.windowGain;
    return taps;
}

//==============================================================================
void ReverseEngine::prepare(double sampleRate, int, int numChannels)
{
    m_sampleRate = sampleRate;
    m_radiansPerSampleHz = (1.0f / static_cast<float>(sampleRate)) * c_twoPiF;
    m_numChannels = std::max(1, numChannels);

    // A voice reads up to 2T back; Reverse Repeat's second pass adds up to T/2 (+ vibrato).
    const int sweepLength = static_cast<int>(std::ceil(2.0 * maxTimeSeconds * sampleRate)) + 8;
    const int repeatLength = static_cast<int>(std::ceil(0.5 * maxTimeSeconds * sampleRate * (1.0 + c_vibratoDepth))) + 8;

    m_channels.resize(static_cast<size_t>(m_numChannels));
    for (auto& c : m_channels)
    {
        c.history.allocate(sweepLength);
        for (auto& f : c.feedback)
            f.allocate(sweepLength);
        c.repeat.allocate(repeatLength);
    }

    m_crossfadeLength = std::max(1, static_cast<int>(c_timeCrossfadeSeconds * sampleRate));
    m_modeFadeLength = std::max(1, static_cast<int>(c_modeCrossfadeSeconds * sampleRate));
    m_feedback.reset(sampleRate, 0.02);

    reset();
}

void ReverseEngine::reset()
{
    for (auto& c : m_channels)
    {
        c.history.clear();
        for (auto& f : c.feedback)
            f.clear();
        c.repeat.clear();
        c.lastVoice = {};
    }

    for (size_t k = 0; k < m_clock.phase.size(); ++k)
        m_clock.phase[k] = c_startDegrees[k] * c_degreesToRadians;
    m_clock.windowGain = { 0.5f, 0.5f };   // no window value has arrived yet
    m_clock.start(m_targetTime, m_radiansPerSampleHz);
    m_crossfadePos = m_crossfadeLength;

    m_vibratoPhase = c_vibratoStartDegrees * c_degreesToRadians;
    m_vibratoIncrement = m_radiansPerSampleHz * c_vibratoHz;
    m_vibratoValue = 0.0f;

    m_mode = m_targetMode;
    m_previousMode = m_targetMode;
    m_modeFadePos = m_modeFadeLength;
    m_feedback.setCurrentAndTargetValue(m_feedback.getTargetValue());
}

void ReverseEngine::setMode(Mode mode, bool immediate)
{
    m_targetMode = mode;
    if (immediate)
    {
        m_mode = m_previousMode = mode;
        m_modeFadePos = m_modeFadeLength;
    }
}

void ReverseEngine::setTime(float seconds, bool immediate)
{
    m_targetTime = juce::jlimit(minTimeSeconds, maxTimeSeconds, seconds);
    if (immediate)
    {
        m_clock.start(m_targetTime, m_radiansPerSampleHz);
        m_crossfadePos = m_crossfadeLength;
    }
}

void ReverseEngine::setFeedback(float amount, bool immediate)
{
    const float f = juce::jlimit(0.0f, maxFeedback, amount);
    if (immediate) m_feedback.setCurrentAndTargetValue(f);
    else           m_feedback.setTargetValue(f);
}

//==============================================================================
void ReverseEngine::process(const float* const* input, float* const* output, int numChannels, int numSamples)
{
    numChannels = std::min(numChannels, m_numChannels);

    for (int i = 0; i < numSamples; ++i)
    {
        // TIME: when the requested time differs, keep the current read heads running on
        // their old clock and cross-fade to heads on the new one. (The original froze the
        // old heads for one host block, which flips their direction and leaves a kink, and
        // depends on the block size.) Further moves wait for the fade to finish, so a
        // dragged slider becomes a chain of short cross-fades.
        if (m_crossfadePos >= m_crossfadeLength && ! juce::exactlyEqual(m_targetTime, m_clock.time))
        {
            m_oldClock = m_clock;
            m_clock.start(m_targetTime, m_radiansPerSampleHz);
            m_clock.resync();   // its heads start silent, so this is inaudible
            m_crossfadePos = 0;
        }

        // Mode: cross-fade between the outgoing and incoming effect.
        if (m_modeFadePos >= m_modeFadeLength && m_targetMode != m_mode)
        {
            m_previousMode = m_mode;
            m_mode = m_targetMode;
            m_modeFadePos = 0;
        }

        const bool timeFading = m_crossfadePos < m_crossfadeLength;
        const float g = timeFading ? fadeCurve(m_crossfadePos, m_crossfadeLength) : 1.0f;

        // The sweeps advance before they are read; the windows applied this sample are
        // the ones computed on the previous sample.
        m_clock.advance();
        const auto taps = tapsOf(m_clock);
        Taps oldTaps;
        if (timeFading)
        {
            m_oldClock.advance();
            oldTaps = tapsOf(m_oldClock);
        }

        m_vibratoPhase = wrapPhase(m_vibratoIncrement + m_vibratoPhase);
        const float repeatBase = static_cast<float>(0.5 * m_clock.time * m_sampleRate);
        const double repeatDelay = repeatBase + repeatBase * (c_vibratoDepth * m_vibratoValue);
        const double oldRepeatDelay = timeFading ? 0.5 * m_oldClock.time * m_sampleRate * (1.0 + c_vibratoDepth * m_vibratoValue) : 0.0;

        const bool modeFading = m_modeFadePos < m_modeFadeLength;
        const float modeWeight = modeFading ? fadeCurve(m_modeFadePos, m_modeFadeLength) : 1.0f;
        const auto weightOf = [&](Mode m) { return (m == m_mode ? modeWeight : 0.0f) + (modeFading && m == m_previousMode ? 1.0f - modeWeight : 0.0f); };
        const float rpWeight = weightOf(Mode::ReversePlayback);
        const float fbWeight = weightOf(Mode::ForwardBackwards);
        const float rrWeight = weightOf(Mode::ReverseRepeat);

        const float forwardWindow = timeFading ? g * taps.window[1] + (1.0f - g) * oldTaps.window[1] : taps.window[1];
        const float feedback = m_feedback.getNextValue();

        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto& c = m_channels[static_cast<size_t>(ch)];
            float x = input[ch][i];
            if (! std::isfinite(x))
                x = 0.0f;   // one bad sample must not lodge in the delay and feedback lines

            // Every delay line is written each sample, whichever effect is active, so a
            // mode change lands on a full history. The input is halved here and the output
            // doubled below (as in the original), which makes the feedback depth exact.
            c.history.push(0.5f * x);
            c.feedback[0].push(feedback * c.lastVoice[0]);
            c.feedback[1].push(feedback * c.lastVoice[1]);

            const auto voice = [&](size_t v)
            {
                const auto readAt = [&](double d) { return c.history.read(d) + c.feedback[v].read(d); };
                float y = readAt(taps.delay[v]) * taps.window[v];
                if (timeFading)
                    y = g * y + (1.0f - g) * readAt(oldTaps.delay[v]) * oldTaps.window[v];
                return y;
            };

            // Voice 0 is common to all three effects.
            const float y0 = voice(0);
            float out = 0.0f;
            float y1 = 0.0f;

            if (rpWeight > 0.0f)
            {
                y1 = voice(1);
                out += rpWeight * 2.0f * (y0 + y1);
            }

            if (fbWeight > 0.0f)
            {
                // The forward path also receives the feedback send.
                const float forward = (0.5f * x + feedback * y0) * forwardWindow;
                out += fbWeight * 2.0f * (y0 + forward);
            }

            // Reverse Repeat's second pass: voice 0 again, T/2 later, with vibrato.
            c.repeat.push(y0);
            if (rrWeight > 0.0f)
            {
                float again = c.repeat.read(repeatDelay);
                if (timeFading)
                    again = g * again + (1.0f - g) * c.repeat.read(oldRepeatDelay);
                out += rrWeight * 2.0f * (y0 + again);
            }

            c.lastVoice = { y0, rpWeight * y1 };
            output[ch][i] = out;
        }

        // Window values for the next sample (one sample late, as in the original).
        m_clock.windowGain = { window(m_clock.phase[1]), window(m_clock.phase[3]) };
        if (timeFading)
            m_oldClock.windowGain = { window(m_oldClock.phase[1]), window(m_oldClock.phase[3]) };
        m_vibratoValue = std::sin(m_vibratoPhase);

        if (timeFading)
            ++m_crossfadePos;
        if (modeFading)
            ++m_modeFadePos;
    }

    // Read heads for the display (voice 0 and the mode's second voice).
    const auto taps = tapsOf(m_clock);
    m_views[0] = { static_cast<float>(taps.delay[0] / m_sampleRate), taps.window[0] };

    switch (m_mode)
    {
        case Mode::ReversePlayback:
            m_views[1] = { static_cast<float>(taps.delay[1] / m_sampleRate), taps.window[1] };
            break;
        case Mode::ForwardBackwards:
            m_views[1] = { 0.0f, taps.window[1] };
            break;
        case Mode::ReverseRepeat:
        {
            // Plays what voice 0 played half a period ago.
            const float earlier = wrapPhase(m_clock.phase[0] - c_piF);
            const double base = m_clock.time * m_sampleRate;
            const double earlierDelay = base * (1.0 + sweep(earlier));
            m_views[1] = { static_cast<float>((0.5 * base + earlierDelay) / m_sampleRate), taps.window[1] };
            break;
        }
    }
}

std::array<ReverseEngine::VoiceView, ReverseEngine::maxVoices> ReverseEngine::getVoiceViews(int& count) const
{
    count = maxVoices;
    return m_views;
}

double ReverseEngine::tailSeconds(float timeSeconds, float feedback)
{
    // A snippet is heard for up to 2.5 T; each trip round the feedback loop
    // takes about T and is scaled by the feedback depth. Stop at -60 dB.
    const double t = juce::jlimit(static_cast<double>(minTimeSeconds), static_cast<double>(maxTimeSeconds), static_cast<double>(timeSeconds));
    const double f = juce::jlimit(0.0, static_cast<double>(maxFeedback), static_cast<double>(feedback));
    const double trips = f > 0.001 ? std::log(0.001) / std::log(f) : 0.0;
    return std::min(120.0, 2.5 * t + trips * t);   // at most ~106 s (T = 4 s, F = 75 %)
}
