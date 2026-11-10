/*
    Offline test harness for the Pitch Flattener DSP core.

    Renders synthetic test signals (sirens, a vibrato voice, a Doppler engine
    pass-by, an instrument, a whistle) through pf::FlattenerCore exactly the way
    the plugin does, block by block, then measures the pitch of the output with
    an independent NSDF estimator and reports how flat it is.

    Usage: PitchFlattenerTests [--wav <dir>] [--rate <hz>] [--block <n>] [--only <name>]
           PitchFlattenerTests --file <wav> [--source <n>] [--realtime] ...   (one recording; --realtime paces it
           like a live host, so the hindsight guide runs on its worker thread instead of in step)
    Exit code is non-zero if any case fails its thresholds.
*/

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "dsp/FlattenerCore.h"
#include "dsp/SourceAdvisor.h"
#include "dsp/PitchMeter.h"
#include "dsp/SourcePresets.h"
#include <random>
#include <climits>
#include <limits>

using namespace pf;

namespace
{
    constexpr double twoPi = 6.283185307179586;

    struct Signal
    {
        std::vector<float> l, r;
        std::vector<float> truthHz;   // instantaneous f0 of the input (0 = silent)
    };

    struct Case
    {
        std::string name;
        SourceType source;
        TargetMode mode;
        float targetHz;              // for Fixed
        std::function<Signal (double fs)> make;
        float maxMedianCents;        // pass thresholds on the output
        float maxP90Cents;
        float minWithin25;           // fraction of pitched frames within 25 cents of target
    };

    // --- signal generators ----------------------------------------------------
    Signal harmonicTone (double fs, double seconds, std::function<double (double)> f0,
                         int harmonics, double rolloff, double noiseDb, bool missingFundamental = false,
                         std::function<double (double)> amp = nullptr)
    {
        const int n = (int) (fs * seconds);
        Signal s;
        s.l.resize ((size_t) n);
        s.truthHz.resize ((size_t) n);
        std::vector<double> phase ((size_t) harmonics + 1, 0.0);
        std::mt19937 rng (1234);
        std::normal_distribution<float> noise (0.0f, 1.0f);
        const float noiseAmp = (float) juce::Decibels::decibelsToGain (noiseDb);

        double ph = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double t = i / fs;
            const double f = f0 (t);
            ph += twoPi * f / fs;
            double y = 0.0, norm = 0.0;
            for (int h = missingFundamental ? 2 : 1; h <= harmonics; ++h)
            {
                if (f * h > 0.45 * fs) break;
                const double a = std::pow ((double) h, -rolloff);
                y += a * std::sin (ph * h);
                norm += a;
            }
            const double g = amp ? amp (t) : 1.0;
            s.l[(size_t) i] = (float) (0.5 * g * y / std::max (norm, 1e-9)) + noiseAmp * noise (rng);
            s.truthHz[(size_t) i] = g > 0.01 ? (float) f : 0.0f;
        }
        s.r = s.l;
        return s;
    }

    /** Glottal-ish pulse train through three vowel formants. */
    Signal voice (double fs, double seconds, std::function<double (double)> f0)
    {
        auto s = harmonicTone (fs, seconds, f0, 60, 1.0, -70.0, false,
                               [] (double t) { return (t > 0.3 && std::fmod (t, 2.5) < 2.2) ? 1.0 : 0.0; });
        // formant filter bank (/a/: 700, 1220, 2600 Hz)
        juce::IIRFilter f1, f2, f3;
        f1.setCoefficients (juce::IIRCoefficients::makeBandPass (fs, 700.0, 6.0));
        f2.setCoefficients (juce::IIRCoefficients::makeBandPass (fs, 1220.0, 8.0));
        f3.setCoefficients (juce::IIRCoefficients::makeBandPass (fs, 2600.0, 10.0));
        for (auto& x : s.l)
        {
            const float v = x;
            x = 1.5f * (f1.processSingleSampleRaw (v) + 0.6f * f2.processSingleSampleRaw (v) + 0.3f * f3.processSingleSampleRaw (v)) + 0.2f * v;
        }
        s.r = s.l;
        return s;
    }

    // --- reference pitch estimator (independent of the plugin's detectors) ------
    /** NSDF + parabolic peak near expectedHz (searches +-7 semitones). Returns 0 if unclear. */
    float measurePitch (const float* x, int len, double fs, float expectedHz)
    {
        const int tauLo = std::max (2, (int) (fs / (expectedHz * 1.5)));
        const int tauHi = std::min (len / 2, (int) (fs / (expectedHz / 1.5)) + 2);
        if (tauHi <= tauLo + 2) return 0.0f;

        std::vector<float> nsdf ((size_t) tauHi + 2, 0.0f);
        for (int t = tauLo - 1; t <= tauHi + 1; ++t)
        {
            double r = 0, m = 0;
            for (int j = 0; j + t < len; ++j)
            {
                r += (double) x[j] * x[j + t];
                m += (double) x[j] * x[j] + (double) x[j + t] * x[j + t];
            }
            nsdf[(size_t) t] = m > 0 ? (float) (2 * r / m) : 0.0f;
        }
        int best = -1;
        for (int t = tauLo; t <= tauHi; ++t)
            if (nsdf[(size_t) t] >= nsdf[(size_t) t - 1] && nsdf[(size_t) t] >= nsdf[(size_t) t + 1]
                && (best < 0 || nsdf[(size_t) t] > nsdf[(size_t) best]))
                best = t;
        if (best < 0 || nsdf[(size_t) best] < 0.45f) return 0.0f;
        const float a = nsdf[(size_t) best - 1], b = nsdf[(size_t) best], c = nsdf[(size_t) best + 1];
        const float den = a - 2 * b + c;
        const float off = std::abs (den) > 1e-9f ? 0.5f * (a - c) / den : 0.0f;
        return (float) (fs / (best + off));
    }

    float percentile (std::vector<float> v, float p)
    {
        if (v.empty()) return 0.0f;
        std::sort (v.begin(), v.end());
        return v[(size_t) std::clamp ((int) (p * (float) (v.size() - 1)), 0, (int) v.size() - 1)];
    }

    void writeWav (const juce::File& f, const std::vector<float>& l, const std::vector<float>& r, double fs)
    {
        f.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::FileOutputStream> os (f.createOutputStream());
        if (os == nullptr) return;
        std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os.get(), fs, 2, 24, {}, 0));
        if (w == nullptr) return;
        os.release();
        const float* chans[2] = { l.data(), r.data() };
        w->writeFromFloatArrays (chans, 2, (int) l.size());
    }

    LiveSettings settingsFor (const Case& c)
    {
        const auto& p = getSourcePreset (c.source);
        LiveSettings s;
        s.sensitivity = p.sensitivity;
        s.trackingSpeedOctPerSec = p.trackingSpeed;
        s.smoothingMs = p.smoothingMs;
        s.gapFillMs = p.gapFillMs;
        s.targetMode = c.mode;
        s.targetHz = c.targetHz;
        s.autoWindowMs = 1000.0f;
        s.amount = 1.0f;
        s.maxShiftSemitones = p.maxShiftSemitones;   // as a newly chosen Source Type sets it
        s.gateDb = -60.0f;
        s.mix = 1.0f;
        s.formantPreserve = p.formant;
        s.shiftBelowHz = p.shiftBelowHz;
        return s;
    }

    struct Result { bool pass; float median, p90, within, inputSpread; int underruns; double rtFactor; };

    int gAlign = INT32_MIN;
    bool gDump = false;
    bool gRealtime = false;
    bool gGuide = true;       // --guide 0: file mode without the hindsight guide
    float gSplit = -1.0f;     // --split Hz: file mode overrides Shift Below (0 = off)
    int gFormant = -1;        // --formant 0|1: file mode overrides the Source Type's formant preservation   // --realtime: file mode runs at the pace of real time, the hindsight guide on its worker

    Result runCase (const Case& c, double fs, int block, const juce::File& wavDir)
    {
        const auto sig = c.make (fs);
        const auto& p = getSourcePreset (c.source);

        FlattenerCore core;
        DetectorConfig dc;
        dc.type = p.detector; dc.minHz = p.minHz; dc.maxHz = p.maxHz;
        core.prepare (fs, block > 0 ? block : 2048, 2, dc, p.quality);
        core.setNonRealtime (true);   // offline render: the hindsight guide analyses in step
        core.setLiveSettings (settingsFor (c));
        core.setFilters (p.highpassHz, p.lowpassHz);
        if (gAlign != INT32_MIN) core.setAlignmentOffset (gAlign);

        const int lat = core.getLatencySamples();
        const int n = (int) sig.l.size();
        std::vector<float> outL ((size_t) (n + lat), 0.0f), outR ((size_t) (n + lat), 0.0f);
        std::copy (sig.l.begin(), sig.l.end(), outL.begin());
        std::copy (sig.r.begin(), sig.r.end(), outR.begin());

        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        std::mt19937 blockRng (99);
        for (int pos = 0, len = 0; pos < (int) outL.size(); pos += len)
        {
            len = block > 0 ? block : std::uniform_int_distribution<int> (1, 2048) (blockRng);
            len = std::min (len, (int) outL.size() - pos);
            float* ch[2] = { outL.data() + pos, outR.data() + pos };
            core.process (ch, 2, len);
            if (gDump)
            {
                TelemetryFrame tf;
                while (core.popTelemetry (tf))
                    if (tf.frame % 20 == 0)
                    {
                        const double fa = core.getAnalysisRate();
                        const double hopA = std::round (0.005 * fa);
                        const double fw = std::pow (2.0, std::ceil (std::log2 (8192.0 * fa / 24000.0)));
                        const double tsec = (tf.frame * hopA + fw / 2) / fa;
                        const int si = (int) (tsec * fs);
                        std::printf ("    t=%5.2f truth=%7.1f raw=%7.1f track=%7.1f target=%7.1f ratio=%7.1f vp=%.2f rms=%.0f\n",
                                     tsec, si < (int) sig.truthHz.size() ? sig.truthHz[(size_t) si] : 0.0f,
                                     tf.rawHz, tf.trackHz, tf.targetHz, tf.ratioCents, tf.voicedProb, tf.rmsDb);
                    }
            }
        }
        const double ms = juce::Time::getMillisecondCounterHiRes() - t0;

        // align output with input
        std::vector<float> aligned (outL.begin() + lat, outL.end());
        std::vector<float> metered = aligned;
        {
            // the reference meter looks at a low-passed copy (cutoff ~8x the lowest pitch) to cope with broadband noise
            float lowest = 1.0e9f;
            for (auto h : sig.truthHz) if (h > 0.0f) lowest = std::min (lowest, h);
            juce::IIRFilter lp1, lp2;
            const double fc = std::min (0.4 * fs, 8.0 * std::max (60.0, (double) lowest));
            lp1.setCoefficients (juce::IIRCoefficients::makeLowPass (fs, fc));
            lp2.setCoefficients (juce::IIRCoefficients::makeLowPass (fs, fc));
            for (auto& v : metered) v = lp2.processSingleSampleRaw (lp1.processSingleSampleRaw (v));
        }

        if (wavDir.isDirectory())
        {
            std::vector<float> alignedR (outR.begin() + lat, outR.end());
            writeWav (wavDir.getChildFile (c.name + "_in.wav"), sig.l, sig.r, fs);
            writeWav (wavDir.getChildFile (c.name + "_out.wav"), aligned, alignedR, fs);
        }

        // measure: 40 ms windows every 10 ms, skip the first second
        // window: 40 ms, or 5 periods of the lowest pitch involved (low engines need longer)
        float lowest = 1.0e9f;
        for (auto h : sig.truthHz) if (h > 0.0f) lowest = std::min (lowest, h);
        if (c.mode == TargetMode::Fixed) lowest = std::min (lowest, c.targetHz);
        const int win = std::max ((int) (0.04 * fs), (int) (5.0 * fs / std::max (lowest, 20.0f))), step = (int) (0.01 * fs);
        std::vector<float> outCents, inCents, outHz;
        for (int pos = (int) fs; pos + win < n; pos += step)
        {
            const float truth = sig.truthHz[(size_t) (pos + win / 2)];
            if (truth <= 0.0f) continue;
            bool steady = true;   // skip windows that touch an on/off edge
            for (int k = 0; k < win; k += win / 4)
                if (sig.truthHz[(size_t) (pos + k)] <= 0.0f) steady = false;
            if (! steady) continue;

            inCents.push_back (hzToCents (truth));
            const float expected = c.mode == TargetMode::Fixed ? c.targetHz : truth;   // rough search centre
            float hz = measurePitch (metered.data() + pos, win, fs, c.mode == TargetMode::Fixed ? expected : 0.0f + expected);
            outHz.push_back (hz);
        }

        // target reference: Fixed -> the target; otherwise the median output pitch (Latch) per sound,
        // approximated by the median over the whole file.
        std::vector<float> valid;
        for (auto h : outHz) if (h > 0) valid.push_back (hzToCents (h));
        float refCents = c.mode == TargetMode::Fixed ? hzToCents (c.targetHz) : percentile (valid, 0.5f);

        std::vector<float> err;
        int within = 0, pitched = 0;
        for (auto h : outHz)
        {
            ++pitched;
            if (h <= 0) { err.push_back (1200.0f); continue; }
            const float e = std::abs (hzToCents (h) - refCents);
            err.push_back (e);
            if (e <= 25.0f) ++within;
        }

        Result res;
        res.median = percentile (err, 0.5f);
        res.p90 = percentile (err, 0.9f);
        res.within = pitched > 0 ? (float) within / (float) pitched : 0.0f;
        res.inputSpread = percentile (inCents, 0.95f) - percentile (inCents, 0.05f);
        res.underruns = core.getUnderrunCount();
        res.rtFactor = (outL.size() / fs * 1000.0) / std::max (ms, 1e-3);
        res.pass = res.median <= c.maxMedianCents && res.p90 <= c.maxP90Cents && res.within >= c.minWithin25 && res.underruns == 0;
        return res;
    }

    bool testLatencyAlignment (double fs, int block)
    {
        if (block <= 0) block = 333;
        // mix = 0 must reproduce the input exactly, delayed by the reported latency
        FlattenerCore core;
        DetectorConfig dc;
        core.prepare (fs, block, 2, dc, 0);
        core.setNonRealtime (true);   // offline render: the hindsight guide analyses in step
        auto s = LiveSettings {};
        s.mix = 0.0f;
        core.setLiveSettings (s);
        const int lat = core.getLatencySamples();
        const int n = lat + (int) fs;
        std::vector<float> l ((size_t) n, 0.0f), r ((size_t) n, 0.0f), ref ((size_t) n, 0.0f);
        std::mt19937 rng (7);
        std::uniform_real_distribution<float> u (-0.5f, 0.5f);
        for (int i = 0; i < (int) fs; ++i) { l[(size_t) i] = r[(size_t) i] = ref[(size_t) i] = u (rng); }
        for (int pos = 0; pos < n; pos += block)
        {
            float* ch[2] = { l.data() + pos, r.data() + pos };
            core.process (ch, 2, std::min (block, n - pos));
        }
        double maxErr = 0;
        for (int i = 0; i < (int) fs; ++i)
            maxErr = std::max (maxErr, (double) std::abs (l[(size_t) (i + lat)] - ref[(size_t) i]));
        std::printf ("  latency %d samples (%.3f s), dry alignment max error %.2g -> %s\n",
                     lat, lat / fs, maxErr, maxErr < 1e-6 ? "OK" : "FAIL");
        return maxErr < 1e-6;
    }

    bool testRebuildsMidStream (double fs)
    {
        // Swap detector and shifter quality while audio runs; must not crash and must keep running.
        FlattenerCore core;
        DetectorConfig dc;
        core.prepare (fs, 512, 2, dc, 0);
        core.setNonRealtime (true);   // offline render: the hindsight guide analyses in step
        core.setLiveSettings (LiveSettings {});
        const int lat = core.getLatencySamples();
        std::vector<float> l (512), r (512);
        int64_t t = 0;
        for (int b = 0; b < (int) (8 * fs / 512); ++b)
        {
            for (int i = 0; i < 512; ++i, ++t)
                l[(size_t) i] = r[(size_t) i] = (float) (0.3 * std::sin (twoPi * 300.0 * (double) t / fs));
            if (b % 50 == 10)
                core.setDetectorConfig ((DetectorType) ((b / 50) % 4), 50.0f + (float) (b % 7) * 30.0f, 2000.0f);
            if (b % 120 == 60)
                core.setQuality ((b / 120) % 2);
            core.housekeeping();
            float* ch[2] = { l.data(), r.data() };
            core.process (ch, 2, 512);
        }
        core.housekeeping();
        const bool ok = core.getLatencySamples() == lat && std::isfinite (l[0]);
        std::printf ("  detector/quality swaps mid-stream: latency constant, %d underrun samples -> %s\n",
                     core.getUnderrunCount(), ok ? "OK" : "FAIL");
        return ok;
    }

    bool testMono (double fs)
    {
        FlattenerCore core;
        DetectorConfig dc;
        core.prepare (fs, 256, 1, dc, 1);
        core.setNonRealtime (true);   // offline render: the hindsight guide analyses in step
        auto s = LiveSettings {};
        s.targetMode = TargetMode::Fixed; s.targetHz = 1000.0f;
        core.setLiveSettings (s);
        const int lat = core.getLatencySamples();
        const int n = lat + (int) (3 * fs);
        std::vector<float> x ((size_t) n, 0.0f);
        for (int i = 0; i < (int) (3 * fs); ++i)
            x[(size_t) i] = (float) (0.3 * std::sin (twoPi * 900.0 * i / fs));
        for (int pos = 0; pos < n; pos += 256)
        {
            float* ch[1] = { x.data() + pos };
            core.process (ch, 1, std::min (256, n - pos));
        }
        const float hz = measurePitch (x.data() + lat + (int) (1.5 * fs), (int) (0.05 * fs), fs, 1000.0f);
        const bool ok = std::abs (hzToCents (hz) - hzToCents (1000.0f)) < 10.0f && core.getUnderrunCount() == 0;
        std::printf ("  mono 900 Hz -> fixed 1000 Hz: measured %.1f Hz -> %s\n", hz, ok ? "OK" : "FAIL");
        return ok;
    }

    /** A siren that stops, leaving traffic-like noise: the noise must not be treated as pitch (and shifted). */
    bool testNoiseTail (double fs)
    {
        const auto& p = getSourcePreset (SourceType::Siren);
        FlattenerCore core;
        DetectorConfig dc;
        dc.type = p.detector; dc.minHz = p.minHz; dc.maxHz = p.maxHz;
        core.prepare (fs, 512, 2, dc, p.quality);
        core.setNonRealtime (true);   // offline render: the hindsight guide analyses in step
        Case c { "noise_tail", SourceType::Siren, TargetMode::Latch, 0.0f, nullptr, 0, 0, 0 };
        auto ls = settingsFor (c);
        ls.gateDb = -60.0f;
        core.setLiveSettings (ls);
        core.setFilters (p.highpassHz, p.lowpassHz);

        // 3 s of wail, then 5 s of broadband noise at -20 dBFS (well above the gate).
        auto sig = harmonicTone (fs, 8.0, [] (double t) { return 950.0 + 400.0 * std::sin (twoPi * 0.2 * t); }, 4, 1.5, -20.0,
                                 false, [] (double t) { return t < 3.0 ? 1.0 : 0.0; });
        const int lat = core.getLatencySamples(), n = lat + (int) sig.l.size();
        std::vector<float> l ((size_t) n, 0.0f);
        std::copy (sig.l.begin(), sig.l.end(), l.begin());
        auto r = l;
        int tailFrames = 0, tailPitched = 0;
        const double hopSec = 0.005;
        for (int pos = 0; pos < n; pos += 512)
        {
            float* ch[2] = { l.data() + pos, r.data() + pos };
            core.process (ch, 2, std::min (512, n - pos));
            TelemetryFrame tf;
            while (core.popTelemetry (tf))
            {
                const double t = (double) tf.frame * hopSec;
                if (t > 4.0 && t < 7.5)   // well clear of the siren's end and the gap fill
                {
                    ++tailFrames;
                    if (tf.trackHz > 0.0f) ++tailPitched;
                }
            }
        }
        const float frac = tailFrames > 0 ? (float) tailPitched / (float) tailFrames : 1.0f;
        const bool ok = tailFrames > 100 && frac < 0.05f;
        std::printf ("  noise after a siren: %.1f%% of %d noise frames treated as pitch -> %s\n", 100.0f * frac, tailFrames, ok ? "OK" : "FAIL");
        return ok;
    }

    /** Latch locks before a sound plays: two calls with soft, breathy attacks (Latch, Vocal / Speech), and every
        tracked frame of each call must already carry that call's latched target, including its first ones. */
    bool testLatchFromFirstFrame (double fs)
    {
        const auto& p = getSourcePreset (SourceType::Vocal);
        FlattenerCore core;
        DetectorConfig dc;
        dc.type = p.detector; dc.minHz = p.minHz; dc.maxHz = p.maxHz;
        core.prepare (fs, 512, 2, dc, p.quality);
        core.setNonRealtime (true);   // offline render: the hindsight guide analyses in step
        Case c { "latch_first_frame", SourceType::Vocal, TargetMode::Latch, 0.0f, nullptr, 0, 0, 0 };
        auto ls = settingsFor (c);
        ls.gateDb = -60.0f;
        core.setLiveSettings (ls);
        core.setFilters (p.highpassHz, p.lowpassHz);

        // Two calls 3 s apart, each a 250 ms breathy swell into a falling 600 -> 450 Hz call.
        const auto inCall = [] (double t) { return std::fmod (t, 3.0) - 0.5; };   // time since the call began
        auto sig = harmonicTone (fs, 6.0, [&inCall] (double t) { return 600.0 * std::pow (0.75, std::clamp (inCall (t), 0.0, 1.2) / 1.2); },
                                 12, 1.0, -55.0, false,
                                 [&inCall] (double t) { const double u = inCall (t);
                                                        return u < 0.0 || u > 1.2 ? 0.0 : std::min (1.0, std::pow (u / 0.25, 2.0)); });
        const int lat = core.getLatencySamples(), n = lat + (int) sig.l.size();
        std::vector<float> l ((size_t) n, 0.0f);
        std::copy (sig.l.begin(), sig.l.end(), l.begin());
        auto r = l;
        std::vector<TelemetryFrame> frames;
        for (int pos = 0; pos < n; pos += 512)
        {
            float* ch[2] = { l.data() + pos, r.data() + pos };
            core.process (ch, 2, std::min (512, n - pos));
            TelemetryFrame tf;
            while (core.popTelemetry (tf))
                frames.push_back (tf);
        }

        // Split into calls (runs of tracked frames) and compare every frame's target with the call's middle.
        int calls = 0, offFrames = 0, tracked = 0;
        for (size_t i = 0; i < frames.size();)
        {
            if (frames[i].targetHz <= 0.0f) { ++i; continue; }
            size_t j = i;
            while (j < frames.size() && frames[j].targetHz > 0.0f) ++j;
            if (j - i > 40)
            {
                ++calls;
                const float latchedHz = frames[(i + j) / 2].targetHz;
                for (size_t k = i; k < j; ++k, ++tracked)
                    if (frames[k].rmsDb > -50.0f && std::abs (1200.0f * std::log2 (frames[k].targetHz / latchedHz)) > 1.0f)
                        ++offFrames;
            }
            i = j;
        }
        const bool ok = calls == 2 && offFrames == 0;
        std::printf ("  latch from a sound's first frame: %d calls, %d of %d audible tracked frames off the call's latch -> %s\n",
                     calls, offFrames, tracked, ok ? "OK" : "FAIL");
        return ok;
    }

    /** Manual Override flattens the detected pitch to Override Hz in every target mode. */
    bool testOverride (double fs)
    {
        bool ok = true;
        for (auto mode : { TargetMode::Latch, TargetMode::Fixed, TargetMode::Auto })
        {
            FlattenerCore core;
            DetectorConfig dc;
            core.prepare (fs, 512, 2, dc, 1);
            core.setNonRealtime (true);   // offline render: the hindsight guide analyses in step
            auto s = LiveSettings {};
            s.targetMode = mode;
            s.targetHz = 700.0f;        // must be ignored while Override is on
            s.manualOverride = true;
            s.overrideHz = 1000.0f;
            core.setLiveSettings (s);
            const int lat = core.getLatencySamples();
            const int len = (int) (4 * fs), n = lat + len;
            std::vector<float> l ((size_t) n, 0.0f), r;
            double ph = 0.0;
            for (int i = 0; i < len; ++i)
            {
                // 900 Hz with a +-100-cent wobble at 2 Hz
                ph += twoPi * 900.0 * std::pow (2.0, std::sin (twoPi * 2.0 * i / fs) / 12.0) / fs;
                l[(size_t) i] = (float) (0.3 * std::sin (ph));
            }
            r = l;
            for (int pos = 0; pos < n; pos += 512)
            {
                float* ch[2] = { l.data() + pos, r.data() + pos };
                core.process (ch, 2, std::min (512, n - pos));
            }
            float worst = 0.0f;
            for (double t = 1.0; t < 3.6; t += 0.1)
            {
                const float hz = measurePitch (l.data() + lat + (int) (t * fs), (int) (0.04 * fs), fs, 1000.0f);
                worst = std::max (worst, hz > 0.0f ? std::abs (hzToCents (hz) - hzToCents (1000.0f)) : 1200.0f);
            }
            const bool pass = worst < 25.0f;
            std::printf ("  override in %s mode: worst %.1f cents from Override Hz -> %s\n",
                         mode == TargetMode::Latch ? "Latch" : mode == TargetMode::Fixed ? "Fixed" : "Auto", worst, pass ? "OK" : "FAIL");
            ok &= pass;
        }
        return ok;
    }

    /**
     * Diagnostic for a real recording (--file): plays it through a Source Type's settings in Latch mode and prints,
     * every 100 ms (--step), the tracked pitch, the target, the correction and the output pitch measured near the target,
     * then the median and 90th percentile of how far the output is from the target.
     */
    int analyseFile (const juce::File& file, SourceType source, int detectorOverride, float maxShiftOverride, double step, float maxFreqOverride, float minFreqOverride, const juce::File& outFile)
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
        if (reader == nullptr)
        {
            std::printf ("can't read %s\n", file.getFullPathName().toRawUTF8());
            return 1;
        }
        const double fs = reader->sampleRate;
        const int len = (int) reader->lengthInSamples;
        juce::AudioBuffer<float> in (1, len);
        reader->read (&in, 0, len, 0, true, false);   // left (or mono) channel

        const auto& p = getSourcePreset (source);
        FlattenerCore core;
        DetectorConfig dc;
        dc.type = detectorOverride >= 0 ? (DetectorType) detectorOverride : p.detector;
        dc.minHz = minFreqOverride > 0.0f ? minFreqOverride : p.minHz;
        dc.maxHz = maxFreqOverride > 0.0f ? maxFreqOverride : p.maxHz;
        core.prepare (fs, 512, 2, dc, p.quality);
        core.setNonRealtime (! gRealtime);
        core.setHindsightAllowed (gGuide);
        Case c { file.getFileName().toStdString(), source, TargetMode::Latch, 0.0f, nullptr, 0, 0, 0 };
        auto ls = settingsFor (c);
        ls.gateDb = -60.0f;   // the plugin's default
        if (maxShiftOverride > 0.0f)
            ls.maxShiftSemitones = maxShiftOverride;
        if (gFormant >= 0)
            ls.formantPreserve = gFormant > 0;
        if (gSplit >= 0.0f)
            ls.shiftBelowHz = gSplit;
        core.setLiveSettings (ls);
        core.setFilters (p.highpassHz, p.lowpassHz);

        const int lat = core.getLatencySamples(), n = lat + len;
        std::vector<float> l ((size_t) n, 0.0f);
        std::copy (in.getReadPointer (0), in.getReadPointer (0) + len, l.begin());
        auto r = l;
        std::vector<TelemetryFrame> frames;
        const double started = juce::Time::getMillisecondCounterHiRes();
        for (int pos = 0; pos < n; pos += 512)
        {
            if (gRealtime)
            {
                const double due = started + 1000.0 * pos / fs, now = juce::Time::getMillisecondCounterHiRes();
                if (due > now)
                    juce::Thread::sleep ((int) (due - now));
            }
            float* ch[2] = { l.data() + pos, r.data() + pos };
            core.process (ch, 2, std::min (512, n - pos));
            TelemetryFrame tf;
            while (core.popTelemetry (tf))
                frames.push_back (tf);
        }

        std::printf ("%s: %s, detector %s, %.0f-%.0f Hz, Max Shift %.0f st, latency %.2f s\n", file.getFileName().toRawUTF8(), p.name,
                     getDetectorName (dc.type), dc.minHz, dc.maxHz, ls.maxShiftSemitones, lat / fs);
        std::printf ("hindsight windows: %d analysed, %d guided\n", core.getHindsightWindows (false), core.getHindsightWindows (true));
        std::printf ("   t(s)   track  target  corr(ct)  out(Hz)  off(ct)  conf  level(dB)\n");

        // Telemetry frame -> time of the audio it describes (frame k is centred on input sample k * hop).
        const double fa = core.getAnalysisRate();
        const double hopA = std::round (0.005 * fa);
        const int win = (int) (0.04 * fs);

        // Meter a low-passed copy of the output, as runCase does, so broadband noise doesn't hide the pitch.
        float highestTarget = 0.0f;
        for (const auto& tf : frames) highestTarget = std::max (highestTarget, tf.targetHz);
        std::vector<float> metered (l.begin() + lat, l.end());
        {
            juce::IIRFilter lp1, lp2;
            const double fc = std::min (0.4 * fs, 8.0 * std::max (60.0, (double) highestTarget));
            lp1.setCoefficients (juce::IIRCoefficients::makeLowPass (fs, fc));
            lp2.setCoefficients (juce::IIRCoefficients::makeLowPass (fs, fc));
            for (auto& v : metered) v = lp2.processSingleSampleRaw (lp1.processSingleSampleRaw (v));
        }

        std::vector<float> off;
        int rows = 0, unpitched = 0;
        double next = 0.0;
        for (const auto& tf : frames)
        {
            const double t = tf.frame * hopA / fa;
            const int at = (int) (t * fs);
            if (t < next || at + win >= len)
                continue;
            next = std::floor (t / step + 1.0e-6) * step + step;   // one row per step
            ++rows;
            if (tf.targetHz <= 0) ++unpitched;
            const float out = tf.targetHz > 0 ? measurePitch (metered.data() + at, win, fs, tf.targetHz) : 0.0f;
            const float cents = out > 0 ? 1200.0f * std::log2 (out / tf.targetHz) : 0.0f;
            if (tf.targetHz > 0)
                off.push_back (out > 0 ? std::abs (cents) : 1200.0f);
            std::printf ("  %5.2f %7.1f %7.1f %8.1f %8.1f %8.1f  %4.2f  %6.1f\n", t, tf.trackHz, tf.targetHz, tf.ratioCents, out, cents,
                         tf.voicedProb, tf.rmsDb);
        }
        if (outFile != juce::File())
        {
            // The processed sound, latency removed, so it lines up with the input.
            std::vector<float> outL (l.begin() + lat, l.end()), outR (r.begin() + lat, r.end());
            writeWav (outFile, outL, outR, fs);
        }

        std::printf ("rows with no pitch tracked: %d of %d\n", unpitched, rows);
        if (off.empty())
            std::printf ("output off target: no pitch was tracked, so there is nothing to measure\n");
        else
            std::printf ("output off target (rows with a pitch), median / 90th pct: %.0f / %.0f cents (1200 = no pitch measured in the output)\n",
                         percentile (off, 0.5f), percentile (off, 0.9f));
        return 0;
    }

    /** --advise: what the plugin's Source Type advisor makes of a recording, starting from the given Source Type. */
    int adviseFile (const juce::File& file, int fromSource)
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
        if (reader == nullptr) { std::printf ("can't read %s\n", file.getFullPathName().toRawUTF8()); return 1; }
        const double fs = reader->sampleRate;
        const int len = (int) reader->lengthInSamples;
        juce::AudioBuffer<float> in (1, len);
        reader->read (&in, 0, len, 0, true, false);

        ProcessingSettings now;
        now.live.targetMode = TargetMode::Latch;
        now.live.gateDb = -60.0f;
        now = withSourceType (now, (SourceType) fromSource);
        const auto advice = adviseSourceType (in.getReadPointer (0), len, fs, now, fromSource);

        std::printf ("%s (from %s)\n", file.getFileName().toRawUTF8(), getSourcePreset ((SourceType) fromSource).name);
        const auto row = [] (const char* name, const TrialResult& r)
        {
            std::printf ("  %-17s coverage %3.0f%%  metered %3.0f%%  off %4.0f ct  flatness %.3f\n", name, 100.0f * r.coverage,
                         100.0f * r.metered, r.medianOffCents, r.flatness);
        };
        row ("(current)", advice.current);
        for (int i = 0; i < (int) SourceType::NumSourceTypes; ++i)
            if (i != (int) SourceType::Custom) row (getSourcePreset ((SourceType) i).name, advice.bySource[(size_t) i]);
        std::printf ("  recommend: %s\n", advice.recommended >= 0 ? getSourcePreset ((SourceType) advice.recommended).name : "nothing");
        return 0;
    }

    /**
     * --sweep: plays a recording through its Source Type's settings (Latch) with one parameter set to each value in
     * turn, and prints one CSV line per value: param,value,file,flatness,coverage,metered,medianOffCents.
     * minfreq / maxfreq values are multiples of the Source Type's own range.
     */
    int sweepFile (const juce::File& file, int source, const juce::String& param, const juce::StringArray& values)
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
        if (reader == nullptr) { std::printf ("can't read %s\n", file.getFullPathName().toRawUTF8()); return 1; }
        const double fs = reader->sampleRate;
        const int len = (int) std::min<juce::int64> (reader->lengthInSamples, (juce::int64) (15.0 * fs));   // first 15 s
        juce::AudioBuffer<float> in (1, len);
        reader->read (&in, 0, len, 0, true, false);

        ProcessingSettings base;
        base.live.targetMode = TargetMode::Latch;
        base.live.gateDb = -60.0f;
        base.live.targetHz = 440.0f;
        base = withSourceType (base, (SourceType) source);

        for (const auto& vs : values)
        {
            auto st = base;
            const float v = vs.getFloatValue();
            if      (param == "none")        {}
            else if (param == "sensitivity") st.live.sensitivity = v / 100.0f;
            else if (param == "speed")       st.live.trackingSpeedOctPerSec = v;
            else if (param == "smoothing")   st.live.smoothingMs = v;
            else if (param == "gapfill")     st.live.gapFillMs = v;
            else if (param == "gate")        st.live.gateDb = v;
            else if (param == "highpass")    st.highpassHz = v;
            else if (param == "lowpass")     st.lowpassHz = v;
            else if (param == "minfreq")     st.minHz = base.minHz * v;
            else if (param == "maxfreq")     st.maxHz = base.maxHz * v;
            else if (param == "maxshift")    st.live.maxShiftSemitones = v;
            else if (param == "quality")     st.quality = (int) v;
            else if (param == "formant")     st.live.formantPreserve = v > 0.5f;
            else if (param == "detector")    st.detector = (DetectorType) (int) v;
            else if (param == "autowindow")  { st.live.targetMode = TargetMode::Auto; st.live.autoWindowMs = v; }
            else if (param == "mode")        st.live.targetMode = (TargetMode) (int) v;
            else { std::printf ("unknown parameter %s\n", param.toRawUTF8()); return 1; }

            if (st.maxHz < st.minHz * 1.5f) st.maxHz = st.minHz * 1.5f;
            const auto r = runTrial (in.getReadPointer (0), len, fs, st);
            std::printf ("%s,%s,%s,%.4f,%.3f,%.3f,%.1f\n", param.toRawUTF8(), vs.toRawUTF8(), file.getFileName().toRawUTF8(),
                         r.flatness, r.coverage, r.metered, r.medianOffCents);
            std::fflush (stdout);
        }
        return 0;
    }

    /** The advisor suggests the right Source Type when the current one is wrong, and nothing when it is right. */
    bool testSourceAdvisor (double fs)
    {
        const auto siren = harmonicTone (fs, 6.0, [] (double t) { return 950.0 + 400.0 * std::sin (twoPi * 0.3 * t); }, 4, 1.5, -40.0);
        ProcessingSettings base;
        base.live.targetMode = TargetMode::Latch;
        base.live.gateDb = -60.0f;

        const auto wrong = adviseSourceType (siren.l.data(), (int) siren.l.size(), fs,
                                             withSourceType (base, SourceType::Drone), (int) SourceType::Drone);
        const auto right = adviseSourceType (siren.l.data(), (int) siren.l.size(), fs,
                                             withSourceType (base, SourceType::Siren), (int) SourceType::Siren);
        const bool ok = wrong.valid && wrong.recommended == (int) SourceType::Siren && right.valid && right.recommended < 0;
        std::printf ("  advisor on a siren: from Drone / Hum (flatness %.2f) suggests %s; from Siren (%.2f) suggests %s -> %s\n",
                     wrong.current.flatness, wrong.recommended >= 0 ? getSourcePreset ((SourceType) wrong.recommended).name : "nothing",
                     right.current.flatness, right.recommended >= 0 ? getSourcePreset ((SourceType) right.recommended).name : "nothing",
                     ok ? "OK" : "FAIL");
        return ok;
    }

    /** The output monitor ring is lined up with the input: monitored output around input sample k is k, flattened. */
    /** Engine Run-up: a run-up is flattened, but a fixed resonance above Shift Below stays where it is. */
    bool testEngineResonanceStaysPut (double fs)
    {
        // Harmonics 100 -> 200 Hz over 3 s (up to ~1 kHz), plus a steady 2.5 kHz tone, like an exhaust's fixed whine.
        auto sig = harmonicTone (fs, 4.0, [] (double t) { return 100.0 * std::exp2 (std::min (t, 3.0) / 3.0); }, 5, 1.0, -70.0);
        for (size_t i = 0; i < sig.l.size(); ++i)
            sig.l[i] += (float) (0.1 * std::sin (twoPi * 2500.0 * (double) i / fs));
        sig.r = sig.l;

        const auto& p = getSourcePreset (SourceType::EngineRunup);
        FlattenerCore core;
        DetectorConfig dc;
        dc.type = p.detector; dc.minHz = p.minHz; dc.maxHz = p.maxHz;
        core.prepare (fs, 512, 2, dc, p.quality);
        core.setNonRealtime (true);
        Case c { "engine_resonance", SourceType::EngineRunup, TargetMode::Latch, 0.0f, nullptr, 0, 0, 0 };
        core.setLiveSettings (settingsFor (c));
        core.setFilters (p.highpassHz, p.lowpassHz);
        const int lat = core.getLatencySamples(), n = (int) sig.l.size() + lat;
        std::vector<float> l ((size_t) n, 0.0f), r ((size_t) n, 0.0f);
        std::copy (sig.l.begin(), sig.l.end(), l.begin());
        std::copy (sig.r.begin(), sig.r.end(), r.begin());
        for (int pos = 0; pos < n; pos += 512)
        {
            float* ch[2] = { l.data() + pos, r.data() + pos };
            core.process (ch, 2, std::min (512, n - pos));
        }

        // Over 3-4 s (correction about -1200 cents): energy at 2.5 kHz vs where an octave-down shift would put it.
        auto level = [&] (double hz)
        {
            const int a = lat + (int) (3.0 * fs), b = lat + (int) (4.0 * fs);
            double re = 0.0, im = 0.0;
            for (int i = a; i < b; ++i)
            {
                re += l[(size_t) i] * std::cos (twoPi * hz * i / fs);
                im += l[(size_t) i] * std::sin (twoPi * hz * i / fs);
            }
            return 20.0 * std::log10 (std::sqrt (re * re + im * im) / (b - a) + 1.0e-12);
        };
        const double kept = level (2500.0), dragged = level (1250.0);
        const bool ok = kept - dragged > 20.0;
        std::printf ("  engine resonance above Shift Below: 2.5 kHz %.1f dB, 1.25 kHz %.1f dB -> %s\n", kept, dragged, ok ? "OK" : "FAIL");
        return ok;
    }

    /** Shift Below keeps the level of the unshifted band: at partial Mix, and when it is switched on and off mid-stream. */
    bool testShiftBelowLevel (double fs)
    {
        // mode 0: Shift Below off; 1: on; 2: on for 1-2 s and from 3 s of input, off otherwise. Returns 20 ms RMS levels (dB).
        auto run = [fs] (double hz, float mix, int mode)
        {
            FlattenerCore core;
            DetectorConfig dc;
            core.prepare (fs, 512, 1, dc, 0);
            core.setNonRealtime (true);
            LiveSettings ls;
            ls.amount = 0.0f;            // no correction: Shift Below should change nothing but the phase
            ls.mix = mix;
            const int lat = core.getLatencySamples(), n = lat + (int) (4 * fs);
            std::vector<float> x ((size_t) n);
            for (int i = 0; i < n; ++i)
                x[(size_t) i] = (float) (0.3 * std::sin (twoPi * hz * i / fs));
            for (int pos = 0; pos < n; pos += 512)
            {
                const double t = pos / fs;
                const bool on = mode == 1 || (mode == 2 && ((t >= 1.0 && t < 2.0) || t >= 3.0));
                ls.shiftBelowHz = on ? 1200.0f : 0.0f;
                core.setLiveSettings (ls);
                float* ch[1] = { x.data() + pos };
                core.process (ch, 1, std::min (512, n - pos));
            }
            std::vector<double> levels;
            const int w = (int) (0.02 * fs);
            for (int a = lat + (int) (0.5 * fs); a + w < n; a += w / 2)
            {
                double e = 0.0;
                for (int i = a; i < a + w; ++i) e += (double) x[(size_t) i] * x[(size_t) i];
                levels.push_back (10.0 * std::log10 (e / w + 1.0e-20));
            }
            return levels;
        };
        auto worstDiff = [] (const std::vector<double>& a, const std::vector<double>& b)
        {
            double worst = 0.0;
            for (size_t i = 0; i < std::min (a.size(), b.size()); ++i)
                worst = std::max (worst, std::abs (a[i] - b[i]));
            return worst;
        };
        double atMix = 0.0, toggled = 0.0;
        for (double hz : { 900.0, 1200.0, 1700.0 })    // 50 % Mix, on vs off, around the crossover
            atMix = std::max (atMix, worstDiff (run (hz, 0.5f, 1), run (hz, 0.5f, 0)));
        for (double hz : { 500.0, 3000.0 })            // switching on and off mid-stream vs never on
            toggled = std::max (toggled, worstDiff (run (hz, 1.0f, 2), run (hz, 1.0f, 0)));
        const bool ok = atMix < 0.5 && toggled < 1.5;
        std::printf ("  Shift Below level: worst %.2f dB at 50%% Mix around the crossover, %.2f dB while switching on/off -> %s\n",
                     atMix, toggled, ok ? "OK" : "FAIL");
        return ok;
    }

    bool testMonitorAlignment (double fs)
    {
        FlattenerCore core;
        DetectorConfig dc;
        core.prepare (fs, 512, 1, dc, 1);
        core.setNonRealtime (true);   // offline render: the hindsight guide analyses in step
        auto s = LiveSettings {};
        s.targetMode = TargetMode::Fixed; s.targetHz = 1000.0f;
        core.setLiveSettings (s);
        const int lat = core.getLatencySamples();
        const int n = lat + (int) (4 * fs);
        std::vector<float> x ((size_t) n, 0.0f);
        for (int i = 0; i < n; ++i)
            x[(size_t) i] = i < (int) (3 * fs) && i > (int) fs ? (float) (0.3 * std::sin (twoPi * 900.0 * i / fs)) : 0.0f;
        for (int pos = 0; pos < n; pos += 512)
        {
            float* ch[1] = { x.data() + pos };
            core.process (ch, 1, std::min (512, n - pos));
        }
        // the tone runs from 1 s to 3 s of input: the monitored output there is at 1000 Hz, before 1 s it is silent
        std::vector<float> win ((size_t) (0.04 * fs)), scratch;
        const bool gotTone = core.copyMonitor (true, (int64_t) (2.0 * fs), (int) win.size(), win.data());
        const float hz = gotTone ? measurePitchNear (win.data(), (int) win.size(), fs, 1000.0f, scratch) : 0.0f;
        std::vector<float> quiet ((size_t) (0.04 * fs));
        const bool gotQuiet = core.copyMonitor (true, (int64_t) (0.5 * fs), (int) quiet.size(), quiet.data());
        float peak = 0.0f;
        for (auto v : quiet) peak = std::max (peak, std::abs (v));
        const bool ok = gotTone && gotQuiet && std::abs (1200.0f * std::log2 (std::max (hz, 1.0f) / 1000.0f)) < 10.0f && peak < 1.0e-3f;
        std::printf ("  output monitor lined up with the input: tone section at %.1f Hz, silent section peak %.4f -> %s\n", hz, peak, ok ? "OK" : "FAIL");
        return ok;
    }

    /** Runs a 900 Hz tone flattened to 1000 Hz; optionally injects a NaN at t=1 s. */
    bool testToneAndNaN (double fs, bool injectNaN, const char* label)
    {
        FlattenerCore core;
        DetectorConfig dc;
        core.prepare (fs, 512, 2, dc, 1);
        core.setNonRealtime (true);   // offline render: the hindsight guide analyses in step
        auto s = LiveSettings {};
        s.targetMode = TargetMode::Fixed; s.targetHz = 1000.0f;
        core.setLiveSettings (s);
        const int lat = core.getLatencySamples();
        const int n = lat + (int) (4 * fs);
        std::vector<float> l ((size_t) n, 0.0f), r;
        for (int i = 0; i < (int) (4 * fs); ++i)
            l[(size_t) i] = (float) (0.3 * std::sin (twoPi * 900.0 * i / fs));
        if (injectNaN) l[(size_t) fs] = std::numeric_limits<float>::quiet_NaN();
        r = l;
        for (int pos = 0; pos < n; pos += 512)
        {
            float* ch[2] = { l.data() + pos, r.data() + pos };
            core.process (ch, 2, std::min (512, n - pos));
        }
        int bad = 0;
        for (auto v : l) if (! std::isfinite (v)) ++bad;
        // pitch right at the start (first 60 ms after onset) and long after the NaN
        const float early = measurePitch (l.data() + lat + (int) (0.02 * fs), (int) (0.04 * fs), fs, 1000.0f);
        const float late  = measurePitch (l.data() + lat + (int) (3.0 * fs), (int) (0.04 * fs), fs, 1000.0f);
        const bool ok = bad == 0 && std::abs (hzToCents (early) - hzToCents (1000.0f)) < 25.0f
                     && std::abs (hzToCents (late) - hzToCents (1000.0f)) < 10.0f;
        std::printf ("  %s @ %.0f Hz: non-finite out %d, onset %.1f Hz, later %.1f Hz -> %s\n",
                     label, fs, bad, early, late, ok ? "OK" : "FAIL");
        return ok;
    }

    bool testWetAlignment (double fs, int block)
    {
        // With no pitch to correct (amount 0), the wet path must line up with the dry path.
        FlattenerCore core;
        DetectorConfig dc;
        core.prepare (fs, block, 1, dc, 0);
        core.setNonRealtime (true);   // offline render: the hindsight guide analyses in step
        auto s = LiveSettings {};
        s.amount = 0.0f;
        core.setLiveSettings (s);
        const int lat = core.getLatencySamples();
        const int n = lat + (int) (2 * fs);
        std::vector<float> x ((size_t) n, 0.0f), ref;
        for (int i = 0; i < (int) (2 * fs); ++i)
            x[(size_t) i] = (float) (0.3 * std::sin (twoPi * 440.0 * i / fs) * (1.0 + 0.5 * std::sin (twoPi * 3.0 * i / fs)));
        ref = x;
        for (int pos = 0; pos < n; pos += block)
        {
            float* ch[1] = { x.data() + pos };
            core.process (ch, 1, std::min (block, n - pos));
        }
        // best lag by cross-correlation around 0
        double bestC = -1; int bestLag = 0;
        for (int lag = -400; lag <= 400; ++lag)
        {
            double c = 0, e1 = 0, e2 = 0;
            for (int i = (int) (0.5 * fs); i < (int) (1.5 * fs); ++i)
            {
                const double a = ref[(size_t) i], b = x[(size_t) (i + lat + lag)];
                c += a * b; e1 += a * a; e2 += b * b;
            }
            c /= std::sqrt (e1 * e2 + 1e-20);
            if (c > bestC) { bestC = c; bestLag = lag; }
        }
        const bool ok = std::abs (bestLag) <= 2 && bestC > 0.98 && core.getUnderrunCount() == 0;
        std::printf ("  wet path offset %d samples, correlation %.4f, underruns %d -> %s\n",
                     bestLag, bestC, core.getUnderrunCount(), ok ? "OK" : "FAIL");
        return ok;
    }
}

int main (int argc, char** argv)
{
    double fs = 48000.0;
    int block = 512;
    juce::File wavDir;
    juce::String only;
    juce::File analyse;
    int analyseSource = (int) SourceType::Siren, analyseDetector = -1;
    float analyseMaxShift = 0.0f;
    double analyseStep = 0.1;
    float analyseMaxFreq = 0.0f, analyseMinFreq = 0.0f;
    juce::File analyseOut, advise;
    juce::String sweepParam;
    juce::StringArray sweepValues;

    for (int i = 1; i < argc; ++i)
    {
        const juce::String a (argv[i]);
        if (a == "--wav" && i + 1 < argc) { wavDir = juce::File (juce::String (argv[++i])); wavDir.createDirectory(); }
        else if (a == "--rate" && i + 1 < argc) fs = juce::String (argv[++i]).getDoubleValue();
        else if (a == "--block" && i + 1 < argc) block = juce::String (argv[++i]).getIntValue();
        else if (a == "--only" && i + 1 < argc) only = argv[++i];
        else if (a == "--align" && i + 1 < argc) gAlign = juce::String (argv[++i]).getIntValue();
        else if (a == "--dump") gDump = true;
        else if (a == "--realtime") gRealtime = true;
        else if (a == "--split" && i + 1 < argc) gSplit = juce::String (argv[++i]).getFloatValue();
        else if (a == "--guide" && i + 1 < argc) gGuide = juce::String (argv[++i]).getIntValue() != 0;
        else if (a == "--formant" && i + 1 < argc) gFormant = juce::String (argv[++i]).getIntValue();
        else if (a == "--file" && i + 1 < argc) analyse = juce::File (juce::String (argv[++i]));
        else if (a == "--source" && i + 1 < argc) analyseSource = juce::String (argv[++i]).getIntValue();
        else if (a == "--detector" && i + 1 < argc) analyseDetector = juce::String (argv[++i]).getIntValue();
        else if (a == "--maxshift" && i + 1 < argc) analyseMaxShift = juce::String (argv[++i]).getFloatValue();
        else if (a == "--sweep" && i + 2 < argc) { sweepParam = juce::String (argv[++i]); sweepValues = juce::StringArray::fromTokens (juce::String (argv[++i]), ",", ""); }
        else if (a == "--advise" && i + 1 < argc) advise = juce::File (juce::String (argv[++i]));
        else if (a == "--out" && i + 1 < argc) analyseOut = juce::File (juce::String (argv[++i]));
        else if (a == "--minfreq" && i + 1 < argc) analyseMinFreq = juce::String (argv[++i]).getFloatValue();
        else if (a == "--maxfreq" && i + 1 < argc) analyseMaxFreq = juce::String (argv[++i]).getFloatValue();
        else if (a == "--step" && i + 1 < argc) analyseStep = juce::jmax (0.005, juce::String (argv[++i]).getDoubleValue());
    }

    if (sweepParam.isNotEmpty() && analyse != juce::File())
        return sweepFile (analyse, juce::jlimit (0, (int) SourceType::NumSourceTypes - 1, analyseSource), sweepParam, sweepValues);

    if (advise != juce::File())
        return adviseFile (advise, juce::jlimit (0, (int) SourceType::NumSourceTypes - 1, analyseSource));

    if (analyse != juce::File())
        return analyseFile (analyse, (SourceType) juce::jlimit (0, (int) SourceType::NumSourceTypes - 1, analyseSource), analyseDetector, analyseMaxShift, analyseStep, analyseMaxFreq, analyseMinFreq, analyseOut);

    std::printf ("Pitch Flattener DSP tests @ %.0f Hz, block %d\n", fs, block);
    bool ok = true;
    ok &= testLatencyAlignment (fs, block);
    ok &= testWetAlignment (fs, block > 0 ? block : 512);
    ok &= testMono (fs);
    ok &= testToneAndNaN (fs, false, "onset correction");
    ok &= testToneAndNaN (fs, true, "NaN in input");
    for (double odd : { 32000.0, 88200.0, 176400.0 })
        ok &= testToneAndNaN (odd, false, "odd sample rate");
    ok &= testRebuildsMidStream (fs);
    ok &= testOverride (fs);
    ok &= testNoiseTail (fs);
    ok &= testLatchFromFirstFrame (fs);
    ok &= testMonitorAlignment (fs);
    ok &= testEngineResonanceStaysPut (fs);
    ok &= testShiftBelowLevel (fs);
    ok &= testSourceAdvisor (fs);

    const double c = 343.0;
    std::vector<Case> cases = {
        { "siren_wail", SourceType::Siren, TargetMode::Fixed, 1000.0f,
          [] (double r) { return harmonicTone (r, 8.0, [] (double t) { return 950.0 + 400.0 * std::sin (twoPi * 0.2 * t); }, 4, 1.5, -40.0); },
          10.0f, 30.0f, 0.85f },
        { "siren_yelp", SourceType::Siren, TargetMode::Fixed, 1000.0f,
          [] (double r) { return harmonicTone (r, 8.0, [] (double t) {
                const double ph = std::fmod (t * 3.0, 1.0);
                return 700.0 * std::pow (2.0, (ph < 0.5 ? ph * 2.0 : 2.0 - ph * 2.0) * 1.0); }, 4, 1.5, -40.0); },
          25.0f, 60.0f, 0.50f },   // ~6 oct/s triangle: close to what a phase-vocoder shifter can follow
        // A British-style siren cycle: fast yelp (1 octave at 4.5 Hz, ~9 oct/s), then a two-tone horn well below it,
        // then a slow wail. Needs MPM's sharpness on the yelp and the full 24-semitone Max Shift for the two-tone.
        { "siren_multi", SourceType::Siren, TargetMode::Fixed, 1450.0f,
          [] (double r) { return harmonicTone (r, 12.0, [] (double t) {
                if (t < 4.5)
                {
                    const double ph = std::fmod (t * 4.5, 1.0);
                    return 870.0 * std::pow (2.0, ph < 0.8 ? ph / 0.8 : (1.0 - ph) / 0.2);   // saw-like: slow up, fast down
                }
                if (t < 7.0)
                    return std::fmod (t, 0.7) < 0.35 ? 575.0 : 645.0;
                return 1300.0 + 450.0 * std::sin (twoPi * 0.18 * (t - 7.0)); }, 4, 1.5, -40.0); },
          40.0f, 120.0f, 0.40f },
        // A yelp in heavy broadband noise (about 0 dB SNR): where MPM alone loses the pitch.
        { "siren_yelp_noisy", SourceType::Siren, TargetMode::Fixed, 1000.0f,
          [] (double r) { return harmonicTone (r, 8.0, [] (double t) {
                const double ph = std::fmod (t * 3.0, 1.0);
                return 700.0 * std::pow (2.0, (ph < 0.5 ? ph * 2.0 : 2.0 - ph * 2.0) * 1.0); }, 4, 1.5, -12.0); },
          30.0f, 70.0f, 0.45f },
        { "voice_vibrato", SourceType::Vocal, TargetMode::Latch, 0.0f,
          [] (double r) { return voice (r, 10.0, [] (double t) {
                return 180.0 * std::pow (2.0, (50.0 * std::sin (twoPi * 5.5 * t) + 150.0 * std::sin (twoPi * 0.15 * t)) / 1200.0); }); },
          12.0f, 40.0f, 0.75f },
        { "engine_doppler", SourceType::Engine, TargetMode::Latch, 0.0f,
          [c] (double r) { return harmonicTone (r, 10.0, [c] (double t) {
                const double v = 25.0, d = 8.0, x = v * (t - 5.0);
                const double vr = -v * x / std::sqrt (x * x + d * d);   // radial velocity (negative = approaching)
                return 95.0 * c / (c + vr); }, 24, 0.8, -45.0, false,
                [] (double t) { return 0.3 + 0.7 * std::exp (-std::pow ((t - 5.0) / 3.0, 2.0)); }); },
          12.0f, 40.0f, 0.75f },
        { "engine_missing_f0", SourceType::Engine, TargetMode::Fixed, 120.0f,
          [c] (double r) { return harmonicTone (r, 10.0, [c] (double t) {
                const double v = 20.0, d = 10.0, x = v * (t - 5.0);
                return 110.0 * c / (c - v * x / std::sqrt (x * x + d * d)); }, 20, 0.7, -45.0, true); },
          15.0f, 50.0f, 0.70f },
        { "field_engine_noisy", SourceType::NoisyField, TargetMode::Latch, 0.0f,
          [c] (double r) { return harmonicTone (r, 10.0, [c] (double t) {
                const double v = 30.0, d = 12.0, x = v * (t - 5.0);
                return 70.0 * c / (c - v * x / std::sqrt (x * x + d * d)); }, 30, 0.6, -24.0, false,
                [] (double t) { return 0.25 + 0.75 * std::exp (-std::pow ((t - 5.0) / 2.5, 2.0)); }); },
          15.0f, 50.0f, 0.70f },
        { "instrument_auto", SourceType::Instrument, TargetMode::Fixed, 440.0f,
          [] (double r) { return harmonicTone (r, 8.0, [] (double t) {
                return 440.0 * std::pow (2.0, 35.0 * std::sin (twoPi * 6.0 * t) / 1200.0); }, 12, 1.0, -55.0); },
          10.0f, 25.0f, 0.85f },
        { "whistle", SourceType::Whistle, TargetMode::Fixed, 1500.0f,
          [] (double r) { return harmonicTone (r, 8.0, [] (double t) {
                return 1500.0 * std::pow (2.0, 100.0 * std::sin (twoPi * 0.5 * t) / 1200.0); }, 1, 1.0, -50.0); },
          8.0f, 25.0f, 0.85f },
    };

    std::printf ("\n%-18s %8s %8s %9s %10s %6s %8s  %s\n", "case", "med(c)", "p90(c)", "<=25c", "in-range(c)", "xruns", "speed", "result");
    for (const auto& tc : cases)
    {
        if (only.isNotEmpty() && ! juce::String (tc.name).contains (only))
            continue;
        const auto r = runCase (tc, fs, block, wavDir);
        std::printf ("%-18s %8.1f %8.1f %8.0f%% %10.0f %6d %7.1fx  %s\n", tc.name.c_str(), r.median, r.p90,
                     100.0f * r.within, r.inputSpread, r.underruns, r.rtFactor, r.pass ? "PASS" : "FAIL");
        ok &= r.pass;
    }

    std::printf ("\n%s\n", ok ? "ALL PASSED" : "SOME TESTS FAILED");
    return ok ? 0 : 1;
}
