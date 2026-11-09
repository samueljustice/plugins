#include "SourceAdvisor.h"
#include "FlattenerCore.h"
#include "PitchMeter.h"
#include "SourcePresets.h"
#include <algorithm>
#include <cmath>

namespace pf
{
    ProcessingSettings withSourceType (ProcessingSettings s, SourceType type)
    {
        if (type == SourceType::Custom)
            return s;

        const auto& p = getSourcePreset (type);
        s.detector = p.detector;
        s.minHz = p.minHz;
        s.maxHz = p.maxHz;
        s.quality = p.quality;
        s.highpassHz = p.highpassHz;
        s.lowpassHz = p.lowpassHz;
        s.live.sensitivity = p.sensitivity;
        s.live.trackingSpeedOctPerSec = p.trackingSpeed;
        s.live.smoothingMs = p.smoothingMs;
        s.live.gapFillMs = p.gapFillMs;
        s.live.formantPreserve = p.formant;
        s.live.maxShiftSemitones = p.maxShiftSemitones;
        return s;
    }

    namespace
    {
        bool cancelled (const std::atomic<bool>* cancel) { return cancel != nullptr && cancel->load(); }

        /** Integer-factor decimation (8th-order Butterworth anti-alias) so trials on 96-192 kHz audio stay quick. */
        std::vector<float> bringDown (const float* x, int n, double fs, double& outRate)
        {
            const int factor = fs > 50000.0 ? std::max (1, (int) std::lround (fs / 48000.0)) : 1;
            outRate = fs / factor;
            if (factor == 1)
                return std::vector<float> (x, x + n);

            MeterLowPass lp1, lp2;
            lp1.set (fs, 0.42 * outRate);
            lp2.set (fs, 0.42 * outRate);
            std::vector<float> out;
            out.reserve ((size_t) (n / factor + 1));
            for (int i = 0; i < n; ++i)
            {
                const float y = lp2.process (lp1.process (x[i]));
                if (i % factor == 0)
                    out.push_back (y);
            }
            return out;
        }
    }

    TrialResult runTrial (const float* monoIn, int numSamplesIn, double sampleRateIn, const ProcessingSettings& s,
                          const std::atomic<bool>* cancel)
    {
        TrialResult result;
        if (monoIn == nullptr || numSamplesIn <= 0 || sampleRateIn <= 0.0)
            return result;

        double fs = sampleRateIn;
        auto audio = bringDown (monoIn, numSamplesIn, sampleRateIn, fs);
        const int len = (int) audio.size();

        constexpr int block = 1024;
        FlattenerCore core;
        DetectorConfig dc;
        dc.type = s.detector;
        dc.minHz = s.minHz;
        dc.maxHz = s.maxHz;
        core.prepare (fs, block, 1, dc, s.quality);
        auto live = s.live;
        live.mix = 1.0f;
        core.setLiveSettings (live);
        core.setFilters (s.highpassHz, s.lowpassHz);

        const int lat = core.getLatencySamples();
        audio.resize ((size_t) (len + lat), 0.0f);   // play out the latency so the whole sound comes back
        std::vector<TelemetryFrame> frames;
        frames.reserve ((size_t) (len / 100 + 64));
        for (int pos = 0; pos < (int) audio.size(); pos += block)
        {
            if (cancelled (cancel))
                return result;
            float* ch[1] = { audio.data() + pos };
            core.process (ch, 1, std::min (block, (int) audio.size() - pos));
            TelemetryFrame tf;
            while (core.popTelemetry (tf))
                frames.push_back (tf);
        }

        // The output, lined up with the input, low-passed for the meter.
        float highest = 0.0f, loudest = -120.0f;
        for (const auto& f : frames)
        {
            highest = std::max (highest, f.targetHz);
            loudest = std::max (loudest, f.rmsDb);
        }
        std::vector<float> out (audio.begin() + lat, audio.end());
        {
            MeterLowPass lp;
            lp.set (fs, 8.0 * std::max (60.0, (double) highest));
            for (auto& v : out)
                v = lp.process (v);
        }

        const float audibleDb = std::max (-60.0f, loudest - 35.0f);
        const double samplesPerFrame = core.getFrameInputSamples();
        const int win = std::max (64, (int) (0.04 * fs));
        std::vector<float> scratch, offs;
        int audible = 0, tracked = 0, eligible = 0, metered = 0;
        for (size_t k = 0; k < frames.size(); ++k)
        {
            const auto& f = frames[k];
            if (f.rmsDb <= audibleDb)
                continue;
            ++audible;
            if (f.targetHz <= 0.0f)
                continue;
            ++tracked;
            if (k % 4 != 0)
                continue;   // meter every 20 ms
            const int at = (int) std::lround ((double) f.frame * samplesPerFrame) - win / 2;
            if (at < 0 || at + win > len)
                continue;
            ++eligible;
            const float hz = measurePitchNear (out.data() + at, win, fs, f.targetHz, scratch);
            if (hz > 0.0f)
            {
                ++metered;
                offs.push_back (std::abs (1200.0f * std::log2 (hz / f.targetHz)));
            }
            if (cancelled (cancel))
                return result;
        }

        result.valid = true;
        result.coverage = audible > 0 ? (float) tracked / (float) audible : 0.0f;
        result.metered = eligible > 0 ? (float) metered / (float) eligible : 0.0f;
        if (! offs.empty())
        {
            std::nth_element (offs.begin(), offs.begin() + (long) offs.size() / 2, offs.end());
            result.medianOffCents = offs[offs.size() / 2];
        }
        result.flatness = result.coverage * result.metered * std::exp (-result.medianOffCents / 40.0f);
        return result;
    }

    SourceAdvice adviseSourceType (const float* mono, int numSamples, double sampleRate, const ProcessingSettings& current,
                                   int currentSource, const std::atomic<bool>* cancel)
    {
        SourceAdvice advice;
        advice.current = runTrial (mono, numSamples, sampleRate, current, cancel);
        if (! advice.current.valid)
            return advice;

        int best = -1;
        float bestFlatness = -1.0f;
        for (int i = 0; i < (int) SourceType::Custom; ++i)
        {
            const auto type = (SourceType) i;
            advice.bySource[(size_t) i] = runTrial (mono, numSamples, sampleRate, withSourceType (current, type), cancel);
            if (cancelled (cancel))
                return advice;
            if (i == currentSource)
                continue;   // its result is what "current" already measured, give or take the user's own tweaks
            if (advice.bySource[(size_t) i].flatness > bestFlatness)
            {
                bestFlatness = advice.bySource[(size_t) i].flatness;
                best = i;
            }
        }

        const float now = advice.current.flatness;
        if (best >= 0 && bestFlatness >= 0.35f && bestFlatness >= now + 0.15f && bestFlatness >= 1.5f * now)
            advice.recommended = best;
        advice.valid = true;
        return advice;
    }
}
