#include "Stamper.h"
#include "PeakLimiter.h"
#include "RubberBandDriver.h"

#include <algorithm>
#include <cmath>

namespace tonalrecall::core
{

namespace
{
    constexpr int kInterpolatorReach = 8;   // samples either side for the fractional read

    // The limits the stretcher is driven within; the latency is sized for them.
    constexpr double kMinRatio = 0.25, kMaxRatio = 4.0;

    // What is left after the stretcher's stated delay is taken out: shifting up, it still lets
    // hits out a little early (measured with noise bursts over a quiet tone, see the tests and
    // Tests/CoreTests.cpp's timing cases). In samples, by semitones up; nothing to correct going
    // down. The stretcher uses longer windows from 88.2 kHz, hence the second table.
    constexpr int kLeadPoints = 5;
    constexpr double kLeadSemitones[kLeadPoints] { 0.0, 3.0, 6.0, 9.0, 12.0 };
    constexpr double kLeadSamples[kLeadPoints] { 0.0, 19.0, 60.0, 111.0, 136.0 };
    constexpr double kLeadSamplesHighRate[kLeadPoints] { 0.0, 0.0, 0.0, 45.0, 102.0 };

    double residualLead(double semitones, double sampleRate) noexcept
    {
        const double* table = sampleRate >= 88200.0 ? kLeadSamplesHighRate : kLeadSamples;
        if (semitones <= 0.0)
            return 0.0;
        if (semitones >= kLeadSemitones[kLeadPoints - 1])
            return table[kLeadPoints - 1];
        int i = 0;
        while (semitones > kLeadSemitones[i + 1])
            ++i;
        const double frac = (semitones - kLeadSemitones[i]) / (kLeadSemitones[i + 1] - kLeadSemitones[i]);
        return table[i] + (table[i + 1] - table[i]) * frac;
    }
}

Stamper::Stamper(const Setup& setup)
    : m_setup(setup)
{
    const int channels = std::max(1, m_setup.channels);
    m_setup.channels = channels;

    // The standard window: the best tone. Timing is held by the stretcher's own transient
    // handling, and measured in the tests.
    m_driver = std::make_unique<RubberBandDriver>(m_setup.sampleRate, channels, m_chunk, 0, m_setup.preserveFormants);
    m_ratioAlignment = m_driver->getRatioAlignment();
    m_baseDelay = m_driver->getBaseDelay();

    // Latency: input waits for a full chunk, then for the stretcher (its headroom), and then the
    // read-back needs room for the stretcher's delay at the lowest ratio (base / 0.25), plus the
    // interpolator's reach.
    m_shiftLatency = m_chunk + m_driver->getHeadroom() + 3 * m_baseDelay + kInterpolatorReach;

    // The safety at the output. Its look-ahead is always part of the latency, on or off, so the
    // latency depends on the sample rate alone.
    m_limiter = std::make_unique<PeakLimiter>(m_setup.sampleRate, channels);
    m_limiter->setEnabled(m_setup.safety);
    m_limiter->setTrim(m_setup.trim);
    m_latency = m_shiftLatency + m_limiter->delaySamples();

    m_in.assign(static_cast<std::size_t>(channels), std::vector<float>(static_cast<std::size_t>(m_chunk), 0.0f));
    m_inPtrs.resize(static_cast<std::size_t>(channels));
    for (int c = 0; c < channels; ++c)
        m_inPtrs[static_cast<std::size_t>(c)] = m_in[static_cast<std::size_t>(c)].data();

    std::size_t ring = 1;
    while (ring < static_cast<std::size_t>(2 * m_latency + 16 * m_chunk + 16384))
        ring <<= 1;
    m_history.assign(static_cast<std::size_t>(channels), std::vector<float>(ring, 0.0f));

    // What to look for in the stream: a short stretch of the learned audio, taken where the
    // effect first has some level (a silent lead-in would match anywhere).
    if (m_setup.learned != nullptr && ! m_setup.curve.empty())
    {
        const auto& learned = *m_setup.learned;
        m_window = static_cast<int>(std::min<std::int64_t>(1024, static_cast<std::int64_t>(learned.size())));
        if (m_window >= 64)
        {
            float peak = 0.0f;
            for (float v : learned)
                peak = std::max(peak, std::abs(v));
            const float gate = peak * 0.05f;
            const auto lastStart = static_cast<std::int64_t>(learned.size()) - m_window;
            std::int64_t anchor = -1;
            for (std::int64_t i = 0; i <= lastStart && anchor < 0; ++i)
                if (std::abs(learned[static_cast<std::size_t>(i)]) > gate)
                    anchor = i;
            if (anchor >= 0 && peak > 0.0f)
            {
                m_anchor = std::min(anchor, lastStart);
                m_wanted.assign(learned.begin() + m_anchor, learned.begin() + m_anchor + m_window);
                m_tolerance = peak * 1.0e-4f;
                m_canAlign = true;
                m_stream.assign(static_cast<std::size_t>(m_window + 4 * m_chunk), 0.0f);
            }
        }
    }
    m_setup.learned.reset();   // only the window is needed from here on
}

Stamper::~Stamper() = default;

bool Stamper::updateShift(double staticSemitones, const std::vector<float>& curve) noexcept
{
    if (curve.size() != m_setup.curve.size())
        return false;
    std::copy(curve.begin(), curve.end(), m_setup.curve.begin());
    m_setup.staticSemitones = staticSemitones;
    return true;
}

double Stamper::semitonesAt(std::int64_t streamSample) const noexcept
{
    if (! m_aligned || m_setup.curve.empty())
        return m_setup.staticSemitones;
    auto sample = streamSample - m_offset;
    if (m_setup.loopLength > 0 && sample >= m_setup.loopLength)
        sample %= m_setup.loopLength;
    const double seconds = static_cast<double>(sample) / m_setup.sampleRate;
    const double at = std::clamp(seconds / m_setup.curveHopSeconds, 0.0, static_cast<double>(m_setup.curve.size() - 1));
    const auto lo = static_cast<std::size_t>(at);
    const auto hi = std::min(lo + 1, m_setup.curve.size() - 1);
    const double frac = at - static_cast<double>(lo);
    return m_setup.curve[lo] * (1.0 - frac) + m_setup.curve[hi] * frac;
}

void Stamper::search() noexcept
{
    // Looks for the learned window in the stretch of the stream that is held. The render reads
    // the same file the preview played, so the samples agree almost to the bit; the tolerance
    // only allows for the last bits of float rounding.
    const float* want = m_wanted.data();
    const auto lastStart = m_streamStart + m_streamCount - m_window;
    for (auto at = m_searchFrom; at <= lastStart; ++at)
    {
        const float* have = m_stream.data() + (at - m_streamStart);
        if (std::abs(have[0] - want[0]) > m_tolerance || std::abs(have[m_window - 1] - want[m_window - 1]) > m_tolerance)
            continue;
        bool same = true;
        for (int i = 1; i < m_window - 1 && same; ++i)
            same = std::abs(have[i] - want[i]) <= m_tolerance;
        if (same)
        {
            m_aligned = true;
            m_offset = at - m_anchor;
            return;
        }
    }
    m_searchFrom = std::max<std::int64_t>(m_searchFrom, lastStart + 1);

    // Drop what can no longer start a window.
    const auto drop = m_searchFrom - m_streamStart;
    if (drop > 0)
    {
        std::move(m_stream.begin() + drop, m_stream.begin() + m_streamCount, m_stream.begin());
        m_streamStart += drop;
        m_streamCount -= drop;
    }
}

void Stamper::process(float* const* channels, int numSamples) noexcept
{
    const int numChannels = m_setup.channels;
    const auto mask = static_cast<std::int64_t>(m_history[0].size()) - 1;

    for (int done = 0; done < numSamples;)
    {
        const int n = std::min(numSamples - done, m_chunk - m_inFill);

        for (int c = 0; c < numChannels; ++c)
            std::copy(channels[c] + done, channels[c] + done + n, m_in[static_cast<std::size_t>(c)].data() + m_inFill);

        if (m_canAlign && ! m_aligned)
        {
            // n is at most a chunk, and search() runs every chunk, so this always fits.
            for (int i = 0; i < n; ++i)
            {
                float sum = 0.0f;
                for (int c = 0; c < numChannels; ++c)
                    sum += channels[c][done + i];
                m_stream[static_cast<std::size_t>(m_streamCount + i)] = sum;
            }
            m_streamCount += n;
        }

        m_inFill += n;

        if (m_inFill == m_chunk)
        {
            m_fed += m_chunk;
            if (m_canAlign && ! m_aligned)
                search();

            // The ratio set with a chunk is heard a little behind the newest sample fed.
            const auto heardAt = m_fed - m_chunk / 2 - m_ratioAlignment;
            const double scale = std::clamp(std::exp2(semitonesAt(heardAt) / 12.0), kMinRatio, kMaxRatio);

            m_driver->processChunk(m_inPtrs.data(), scale, [&](const float* const* data, int got)
            {
                for (int i = 0; i < got; ++i)
                {
                    const auto at = static_cast<std::size_t>((m_produced + i) & mask);
                    for (int c = 0; c < numChannels; ++c)
                        m_history[static_cast<std::size_t>(c)][at] = data[c][i];
                }
                m_produced += got;
            });
            m_inFill = 0;
        }

        // One sample out for each sample in. Output sample k carries input time t = k - latency.
        // The stretcher put that moment at history index t + base * (1/r - 1), r being the ratio
        // at t; reading there undoes the ratio-dependent part of its delay.
        for (int i = 0; i < n; ++i)
        {
            const auto t = m_emitted - m_shiftLatency;
            ++m_emitted;
            if (t < 0)
            {
                for (int c = 0; c < numChannels; ++c)
                    channels[c][done + i] = 0.0f;
                continue;
            }

            const double semitones = semitonesAt(t);
            const double ratio = std::clamp(std::exp2(semitones / 12.0), kMinRatio, kMaxRatio);
            const double at = static_cast<double>(t) + static_cast<double>(m_baseDelay) * (1.0 / ratio - 1.0)
                              - residualLead(semitones, m_setup.sampleRate);
            const auto whole = static_cast<std::int64_t>(std::floor(at));
            const double frac = at - static_cast<double>(whole);

            if (whole + kInterpolatorReach >= m_produced)
            {
                // Should not happen: the latency covers the stretcher's headroom.
                for (int c = 0; c < numChannels; ++c)
                    channels[c][done + i] = 0.0f;
                continue;
            }

            if (frac < 1.0e-6)
            {
                for (int c = 0; c < numChannels; ++c)
                    channels[c][done + i] = whole >= 0 ? m_history[static_cast<std::size_t>(c)][static_cast<std::size_t>(whole & mask)] : 0.0f;
                continue;
            }

            // Windowed-sinc read between samples (a bend moves the read point smoothly).
            float taps[2 * kInterpolatorReach];
            for (int k = 0; k < 2 * kInterpolatorReach; ++k)
            {
                const double x = static_cast<double>(k - kInterpolatorReach + 1) - frac;
                const double sinc = std::abs(x) < 1.0e-9 ? 1.0 : std::sin(3.14159265358979323846 * x) / (3.14159265358979323846 * x);
                const double window = 0.5 + 0.5 * std::cos(3.14159265358979323846 * x / kInterpolatorReach);
                taps[k] = static_cast<float>(sinc * window);
            }
            for (int c = 0; c < numChannels; ++c)
            {
                const auto& history = m_history[static_cast<std::size_t>(c)];
                float sum = 0.0f;
                for (int k = 0; k < 2 * kInterpolatorReach; ++k)
                {
                    const auto index = whole + k - kInterpolatorReach + 1;
                    if (index >= 0)
                        sum += taps[k] * history[static_cast<std::size_t>(index & mask)];
                }
                channels[c][done + i] = sum;
            }
        }
        done += n;
    }

    m_limiter->process(channels, numSamples);
}

void Stamper::setTrim(float gain) noexcept { m_limiter->setTrim(gain); }
float Stamper::limiterReductionDb() const noexcept { return m_limiter->maxReductionDb(); }

//==============================================================================
float StampLevels::trim() const noexcept
{
    if (! complete || outputPeak <= PeakLimiter::kCeiling)
        return 1.0f;
    return PeakLimiter::kCeiling / outputPeak;
}

float StampLevels::trimDb() const noexcept
{
    return 20.0f * std::log10(std::max(trim(), 1.0e-6f));
}

StampLevels measureStamp(Stamper::Setup setup, const std::vector<std::vector<float>>& channels, const std::function<bool()>& cancelled)
{
    StampLevels levels;
    if (channels.empty() || channels[0].empty())
        return levels;
    const auto length = channels[0].size();
    for (const auto& channel : channels)
        if (channel.size() != length)
            return levels;

    setup.channels = static_cast<int>(channels.size());
    setup.safety = false;
    setup.loopLength = 0;
    Stamper stamper(setup);

    for (const auto& channel : channels)
        for (const float v : channel)
            levels.inputPeak = std::max(levels.inputPeak, std::abs(v));

    // The effect, then silence for as long as the latency, so its end comes out too.
    constexpr std::size_t kBlock = 4096;
    std::vector<std::vector<float>> block(channels.size(), std::vector<float>(kBlock, 0.0f));
    std::vector<float*> pointers(channels.size());
    for (std::size_t c = 0; c < channels.size(); ++c)
        pointers[c] = block[c].data();

    const auto total = length + static_cast<std::size_t>(stamper.latencySamples());
    for (std::size_t at = 0; at < total; at += kBlock)
    {
        if (cancelled && cancelled())
            return levels;
        const auto n = std::min(kBlock, total - at);
        for (std::size_t c = 0; c < channels.size(); ++c)
        {
            const auto have = at < length ? std::min(n, length - at) : std::size_t { 0 };
            std::copy_n(channels[c].begin() + static_cast<std::ptrdiff_t>(std::min(at, length)), have, block[c].begin());
            std::fill(block[c].begin() + static_cast<std::ptrdiff_t>(have), block[c].begin() + static_cast<std::ptrdiff_t>(n), 0.0f);
        }
        stamper.process(pointers.data(), static_cast<int>(n));
        for (std::size_t c = 0; c < channels.size(); ++c)
            for (std::size_t i = 0; i < n; ++i)
                levels.outputPeak = std::max(levels.outputPeak, std::abs(block[c][i]));
    }

    levels.complete = true;
    return levels;
}

} // namespace tonalrecall::core
