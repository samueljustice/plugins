#include "FlattenerCore.h"

namespace pf
{
    namespace
    {
        int nextPow2 (int v)
        {
            int p = 1;
            while (p < v)
                p <<= 1;
            return p;
        }
    }

    FlattenerCore::FlattenerCore() = default;

    FlattenerCore::~FlattenerCore()
    {
        releaseResources();
    }

    void FlattenerCore::designRbj (Biquad& f, bool highpass, double hz, double fs, double q)
    {
        hz = std::clamp (hz, 1.0, 0.49 * fs);
        const double w0 = 2.0 * juce::MathConstants<double>::pi * hz / fs;
        const double cw = std::cos (w0), alpha = std::sin (w0) / (2.0 * q);
        const double a0 = 1.0 + alpha;
        if (highpass)
        {
            f.b0 = (1.0 + cw) * 0.5 / a0;
            f.b1 = -(1.0 + cw) / a0;
            f.b2 = f.b0;
        }
        else
        {
            f.b0 = (1.0 - cw) * 0.5 / a0;
            f.b1 = (1.0 - cw) / a0;
            f.b2 = f.b0;
        }
        f.a1 = -2.0 * cw / a0;
        f.a2 = (1.0 - alpha) / a0;
    }

    DetectorConfig FlattenerCore::makeConfig (DetectorType type, float minHz, float maxHz) const
    {
        DetectorConfig c;
        c.type = type;
        c.minHz = std::min (minHz, maxHz * 0.66f);
        c.maxHz = std::max (maxHz, minHz * 1.5f);
        c.analysisRate = analysisRate;
        c.hop = hop;
        c.maxFrameLength = frameWindow;
        return c;
    }

    //==========================================================================
    void FlattenerCore::prepare (double fs, int maxBlockSize, int numChannels,
                                 const DetectorConfig& det, int quality)
    {
        releaseResources();
        const std::lock_guard<std::mutex> lock (structMutex);

        sampleRate = fs;
        maxBlock = std::max (32, maxBlockSize);
        numCh = std::clamp (numChannels, 1, 2);

        // Analysis runs at ~22-24 kHz whatever the host rate is.
        decimation = std::max (1, (int) std::ceil (fs / 24000.0 - 1.0e-9));   // analysis rate <= 24 kHz
        analysisRate = fs / decimation;
        hop = std::max (16, (int) std::lround (0.005 * analysisRate));
        frameWindow = nextPow2 ((int) std::lround (8192.0 * analysisRate / 24000.0));

        // 8th-order Butterworth anti-alias filter (4 biquads) when decimating
        aaStages = 0;
        if (decimation > 1)
        {
            static constexpr double qs[4] = { 0.50979558, 0.60134489, 0.89997622, 2.56291545 };
            for (int i = 0; i < 4; ++i)
            {
                designRbj (aaFilters[(size_t) i], false, 0.45 * analysisRate, fs, qs[i]);
                aaFilters[(size_t) i].clear();
            }
            aaStages = 4;
        }
        currentHp = currentLp = -1.0f;
        hpFilter = {};
        lpFilter = {};
        setFilters (40.0f, 10000.0f);

        const int anaSize = nextPow2 (frameWindow + 8 * hop + 4096);
        anaRing.assign ((size_t) anaSize, 0.0f);
        anaLevelRing.assign ((size_t) anaSize, 0.0f);
        anaMask = anaSize - 1;
        frameBuf.assign ((size_t) frameWindow, 0.0f);
        frameLevelBuf.assign ((size_t) frameWindow, 0.0f);

        currentConfig = makeConfig (det.type, det.minHz, det.maxHz);
        wantedConfig = currentConfig;
        detector = createDetector (currentConfig);
        activeDetectorType.store ((int) currentConfig.type);

        tracker.prepare ((double) hop / analysisRate);

        // Shifter: chunk of ~5 ms, power of two
        shifterChunk = nextPow2 ((int) std::lround (128.0 * fs / 48000.0));
        currentQuality = wantedQuality = std::clamp (quality, 0, 1);
        shifter = std::make_unique<RubberBandDriver> (fs, numCh, shifterChunk, currentQuality, formantMirror.load());

        // Latency budget (input samples)
        const int C = shifter->getChunkSize();
        analysisDelay = decimation * (frameWindow / 2 + (PitchTracker::totalLagFrames() + 3) * hop) + decimation + C;
        shifterHeadroom = shifter->getHeadroom();
        alignOffset = shifter->getRatioAlignment();
        totalLatency = analysisDelay + C + shifterHeadroom;

        const int ringSize = nextPow2 (totalLatency + 4 * C + 2 * maxChunk + 16);
        ringMask = ringSize - 1;
        inRing.assign ((size_t) numCh, std::vector<float> ((size_t) ringSize, 0.0f));
        wetRing.assign ((size_t) numCh, std::vector<float> ((size_t) ringSize, 0.0f));
        feedScratch.assign ((size_t) numCh, std::vector<float> ((size_t) C, 0.0f));
        feedPtrs.resize ((size_t) numCh);
        for (int c = 0; c < numCh; ++c)
            feedPtrs[(size_t) c] = feedScratch[(size_t) c].data();

        const int monitorSize = nextPow2 ((int) std::ceil (7.0 * fs));
        monitorIn.assign ((size_t) monitorSize, 0.0f);
        monitorOut.assign ((size_t) monitorSize, 0.0f);
        monitorMask = monitorSize - 1;

        telemetryBuf.assign ((size_t) telemetryFifo.getTotalSize(), TelemetryFrame {});
        uiAudioBuf.assign ((size_t) uiAudioFifo.getTotalSize(), 0.0f);

        reset();
        prepared.store (true);
    }

    void FlattenerCore::releaseResources()
    {
        const std::lock_guard<std::mutex> lock (structMutex);
        prepared.store (false);
        if (auto* p = pendingDetector.exchange (nullptr)) delete p;
        if (auto* p = retiredDetector.exchange (nullptr)) delete p;
        if (auto* p = pendingShifter.exchange (nullptr)) delete p;
        if (auto* p = retiredShifter.exchange (nullptr)) delete p;
    }

    void FlattenerCore::reset()
    {
        for (auto& f : aaFilters) f.clear();
        hpFilter.clear();
        lpFilter.clear();
        decimPhase = 0;
        decimAccum = 0.0f;
        std::fill (anaRing.begin(), anaRing.end(), 0.0f);
        std::fill (anaLevelRing.begin(), anaLevelRing.end(), 0.0f);
        // Start half a frame "into" silence so frame u is centred on analysis sample u*hop:
        // the very first input samples then get a correction too.
        anaWritten = frameWindow / 2;
        framesDone = 0;
        tracker.reset();

        for (auto& ch : inRing)  std::fill (ch.begin(), ch.end(), 0.0f);
        for (auto& ch : wetRing) std::fill (ch.begin(), ch.end(), 0.0f);
        inWritten = 0;
        monitorInWritten.store (0, std::memory_order_release);
        monitorOutWritten.store (0, std::memory_order_release);
        resetCount.fetch_add (1, std::memory_order_acq_rel);
        fed = 0;
        wetWritten = 0;
        xfadeStart = 0;
        if (shifter)
            shifter->reset();

        telemetryFifo.reset();
        uiAudioFifo.reset();
        latchedHz.store (0.0f);
        underruns.store (0);
    }

    //==========================================================================
    void FlattenerCore::setDetectorConfig (DetectorType type, float minHz, float maxHz)
    {
        const std::lock_guard<std::mutex> lock (structMutex);
        if (! prepared.load())
            return;
        wantedConfig = makeConfig (type, minHz, maxHz);
        housekeepingLocked();
    }

    void FlattenerCore::setQuality (int quality)
    {
        const std::lock_guard<std::mutex> lock (structMutex);
        if (! prepared.load())
            return;
        wantedQuality = std::clamp (quality, 0, 1);
        housekeepingLocked();
    }

    void FlattenerCore::housekeeping()
    {
        std::unique_lock<std::mutex> lock (structMutex, std::try_to_lock);
        if (lock.owns_lock())
            housekeepingLocked();
    }

    void FlattenerCore::housekeepingLocked()
    {
        // Retired objects were handed back by the audio thread; delete them here.
        if (auto* p = retiredDetector.exchange (nullptr)) delete p;
        if (auto* p = retiredShifter.exchange (nullptr)) delete p;

        if (! prepared.load())
            return;

        // Post a new object only when both hand-over slots are free. The audio
        // thread fills "retired" before it clears "pending", so a free pending
        // slot with a free retired slot means the last hand-over is complete.
        if (wantedConfig != currentConfig && pendingDetector.load() == nullptr && retiredDetector.load() == nullptr)
        {
            auto fresh = createDetector (wantedConfig);
            currentConfig = wantedConfig;
            pendingDetector.store (fresh.release());
        }

        if (wantedQuality != currentQuality && pendingShifter.load() == nullptr && retiredShifter.load() == nullptr)
        {
            auto fresh = std::make_unique<RubberBandDriver> (sampleRate, numCh, shifterChunk, wantedQuality, formantMirror.load());
            currentQuality = wantedQuality;
            pendingShifter.store (fresh.release());
        }
    }

    void FlattenerCore::applyStructuralNow (DetectorType type, float minHz, float maxHz, int quality)
    {
        const std::lock_guard<std::mutex> lock (structMutex);
        if (! prepared.load())
            return;

        installPending();   // anything the message thread already built

        wantedConfig = makeConfig (type, minHz, maxHz);
        wantedQuality = std::clamp (quality, 0, 1);

        if (wantedConfig != currentConfig)
        {
            currentConfig = wantedConfig;
            auto old = std::move (detector);
            detector = createDetector (currentConfig);
            activeDetectorType.store ((int) currentConfig.type);
        }

        if (wantedQuality != currentQuality)
        {
            currentQuality = wantedQuality;
            auto* s = new RubberBandDriver (sampleRate, numCh, shifterChunk, currentQuality, formantMirror.load());
            installShifter (s);   // puts the old one in the retired slot (may be occupied: delete directly)
        }

        if (auto* p = retiredDetector.exchange (nullptr)) delete p;
        if (auto* p = retiredShifter.exchange (nullptr)) delete p;
    }

    void FlattenerCore::installDetector (IPitchDetector* d)
    {
        if (auto* old = retiredDetector.exchange (detector.release()))
            delete old;   // only reachable from applyStructuralNow (non-realtime)
        detector.reset (d);
        activeDetectorType.store ((int) d->getConfig().type);
    }

    void FlattenerCore::installShifter (RubberBandDriver* s)
    {
        if (auto* old = retiredShifter.exchange (shifter.release()))
            delete old;   // only reachable from applyStructuralNow (non-realtime)
        shifter.reset (s);
        alignOffset = shifter->getRatioAlignment();

        // The new stretcher's aligned output starts at the next sample we feed it.
        // Fill what the old one never delivered with the (aligned) dry signal and
        // crossfade dry -> new wet, so a quality change is a smooth blend, not a dropout.
        const int64_t notYetOutput = inWritten - totalLatency;
        const int64_t fadeOutStart = std::max<int64_t> (notYetOutput, wetWritten - 256);
        for (int64_t p = fadeOutStart; p < wetWritten; ++p)
        {
            const float g = (float) (p - fadeOutStart) / 256.0f;   // old wet -> dry
            const int r = (int) (p & ringMask);
            for (size_t c = 0; c < wetRing.size(); ++c)
                wetRing[c][(size_t) r] = wetRing[c][(size_t) r] * (1.0f - g) + inRing[c][(size_t) r] * g;
        }
        for (; wetWritten < fed; ++wetWritten)
        {
            const int r = (int) (wetWritten & ringMask);
            for (size_t c = 0; c < wetRing.size(); ++c)
                wetRing[c][(size_t) r] = inRing[c][(size_t) r];
        }
        wetWritten = fed;
        xfadeFrom = fed;
        xfadeStart = xfadeLength;
    }

    void FlattenerCore::installPending()
    {
        // Fill "retired" before clearing "pending" (see housekeepingLocked).
        if (auto* d = pendingDetector.load())
        {
            if (retiredDetector.load() == nullptr)
            {
                retiredDetector.store (detector.release());
                detector.reset (d);
                pendingDetector.store (nullptr);
                activeDetectorType.store ((int) d->getConfig().type);
            }
        }

        if (auto* s = pendingShifter.load())
        {
            if (retiredShifter.load() == nullptr)
            {
                auto* old = shifter.release();
                shifter.reset();
                installShifter (s);     // retired slot is empty, so this just stores nullptr there
                retiredShifter.store (old);
                pendingShifter.store (nullptr);
            }
        }
    }

    void FlattenerCore::setFilters (float highpassHz, float lowpassHz)
    {
        if (! juce::exactlyEqual (highpassHz, currentHp))
        {
            designRbj (hpFilter, true, highpassHz, analysisRate, 0.7071);
            currentHp = highpassHz;
        }
        const float lp = std::min (lowpassHz, (float) (0.45 * analysisRate));
        if (! juce::exactlyEqual (lp, currentLp))
        {
            designRbj (lpFilter, false, lp, analysisRate, 0.7071);
            currentLp = lp;
        }
    }

    //==========================================================================
    void FlattenerCore::process (float* const* channels, int numChannels, int numSamples)
    {
        if (! prepared.load() || numSamples <= 0)
            return;

        juce::ScopedNoDenormals noDenormals;

        if (resetRequested.exchange (false))
            reset();

        installPending();
        shifter->setFormantPreserve (live.formantPreserve);
        tracker.setSettings (live, detector->getConfig().type);
        if (relatchRequested.exchange (false))
            tracker.requestRelatch();

        // block level for the UI
        {
            double sum = 0.0;
            for (int c = 0; c < numChannels; ++c)
                for (int i = 0; i < numSamples; ++i)
                    sum += (double) channels[c][i] * channels[c][i];
            const double rms = std::sqrt (sum / (double) (numSamples * std::max (1, numChannels)));
            inputLevelDb.store ((float) juce::Decibels::gainToDecibels (rms, -120.0));
        }

        for (int done = 0; done < numSamples;)
        {
            const int n = std::min (maxChunk, numSamples - done);
            float* ptrs[2] = { channels[0] + done, channels[std::min (1, numChannels - 1)] + done };
            processChunk (ptrs, numChannels, n);
            done += n;
        }

        latchedHz.store (tracker.getLatchedHz());
    }

    void FlattenerCore::processChunk (float* const* channels, int numChannels, int numSamples)
    {
        const int64_t start = inWritten;
        const float monoScale = numChannels > 1 ? 0.5f : 1.0f;

        // 1. store input, run the analysis front end
        for (int i = 0; i < numSamples; ++i)
        {
            const int w = (int) ((start + i) & ringMask);
            float mono = 0.0f;
            for (int c = 0; c < numCh; ++c)
            {
                float x = channels[std::min (c, numChannels - 1)][i];
                if (! std::isfinite (x))
                    x = 0.0f;   // one bad sample must not poison the filters, analysis or shifter
                x = std::clamp (x, -64.0f, 64.0f);
                inRing[(size_t) c][(size_t) w] = x;
                mono += x;
            }
            mono *= (numCh > 1 && numChannels > 1) ? monoScale : 1.0f;
            monitorIn[(size_t) ((start + i) & monitorMask)] = mono;

            float y = mono;
            for (int s = 0; s < aaStages; ++s)
                y = aaFilters[(size_t) s].process (y);

            if (++decimPhase >= decimation)
            {
                decimPhase = 0;
                const int a = (int) (anaWritten & anaMask);
                anaLevelRing[(size_t) a] = y;
                anaRing[(size_t) a] = lpFilter.process (hpFilter.process (y));
                ++anaWritten;

                if (uiAudioFifo.getFreeSpace() > 0)
                {
                    const auto scope = uiAudioFifo.write (1);
                    if (scope.blockSize1 > 0) uiAudioBuf[(size_t) scope.startIndex1] = y;
                    else if (scope.blockSize2 > 0) uiAudioBuf[(size_t) scope.startIndex2] = y;
                }
            }
        }
        inWritten += numSamples;

        // 2. analyse every complete frame
        runAnalysis();

        // 3. feed the shifter with audio whose correction is now final
        feedShifter();

        // 4. output: dry and wet, both exactly totalLatency samples late
        const float mix = std::clamp (live.mix, 0.0f, 1.0f);
        for (int i = 0; i < numSamples; ++i)
        {
            const int64_t o = start + i - totalLatency;
            for (int c = 0; c < numChannels; ++c)
            {
                const int ch = std::min (c, numCh - 1);
                float dry = 0.0f, wet = 0.0f;
                if (o >= 0)
                {
                    const int r = (int) (o & ringMask);
                    dry = inRing[(size_t) ch][(size_t) r];
                    if (o < wetWritten)
                        wet = wetRing[(size_t) ch][(size_t) r];
                    else if (c == 0)
                        underruns.fetch_add (1);
                }
                const float y = dry * (1.0f - mix) + wet * mix;
                channels[c][i] = std::isfinite (y) ? y : 0.0f;
            }
            if (o >= 0)
            {
                // The flattened (wet) signal only: what the processing did, whatever Mix or bypass let through.
                float m = 0.0f;
                if (o < wetWritten)
                {
                    const int r = (int) (o & ringMask);
                    for (int c = 0; c < numCh; ++c)
                        m += wetRing[(size_t) c][(size_t) r];
                    m /= (float) numCh;
                }
                monitorOut[(size_t) (o & monitorMask)] = m;
            }
        }

        monitorInWritten.store (inWritten, std::memory_order_release);
        monitorOutWritten.store (std::max<int64_t> (0, inWritten - totalLatency), std::memory_order_release);
    }

    bool FlattenerCore::copyMonitor (bool output, int64_t from, int numSamples, float* dest)
    {
        // The rings are only reallocated by prepare(), which holds this lock.
        const std::lock_guard<std::mutex> lock (structMutex);
        if (! prepared.load() || numSamples <= 0 || from < 0)
            return false;

        const auto& ring = output ? monitorOut : monitorIn;
        const auto& written = output ? monitorOutWritten : monitorInWritten;
        const int64_t capacity = (int64_t) ring.size();
        const int before = resetCount.load (std::memory_order_acquire);
        const int64_t end = written.load (std::memory_order_acquire);

        // Keep well clear of where the audio thread is writing.
        const int64_t margin = (int64_t) (2 * maxChunk + maxBlock);
        if (from + numSamples > end || from < end - capacity + margin)
            return false;

        for (int i = 0; i < numSamples; ++i)
            dest[i] = ring[(size_t) ((from + i) & monitorMask)];

        return resetCount.load (std::memory_order_acquire) == before
            && from >= written.load (std::memory_order_acquire) - capacity + margin / 2;
    }

    void FlattenerCore::runAnalysis()
    {
        while (anaWritten >= framesDone * hop + frameWindow)
        {
            const int len = detector->frameLength();
            const int bias = std::clamp (detector->timeBias(), 0, (frameWindow - len) / 2);
            const int64_t frameStart = framesDone * hop + (frameWindow - len) / 2 + bias;

            double sum = 0.0;
            for (int i = 0; i < len; ++i)
            {
                const int a = (int) ((frameStart + i) & anaMask);
                frameBuf[(size_t) i] = anaRing[(size_t) a];
                const float l = anaLevelRing[(size_t) a];
                sum += (double) l * l;
            }

            FrameObservation obs;
            obs.clear();
            obs.rmsDb = (float) juce::Decibels::gainToDecibels (std::sqrt (sum / std::max (1, len)), -120.0);
            detector->analyse (frameBuf.data(), live.sensitivity, obs);
            if (obs.rmsDb < live.gateDb)
                obs.voicedProb = 0.0f;

            tracker.push (obs);
            ++framesDone;

            if (tracker.lastFinalFrame() >= 0)
            {
                const auto scope = telemetryFifo.write (1);
                if (scope.blockSize1 > 0) telemetryBuf[(size_t) scope.startIndex1] = tracker.lastTelemetry();
                else if (scope.blockSize2 > 0) telemetryBuf[(size_t) scope.startIndex2] = tracker.lastTelemetry();
            }
        }
    }

    float FlattenerCore::ratioCentsAtInputSample (int64_t m) const
    {
        // frame u is centred on analysis sample u*hop (the analysis ring starts half a frame early)
        const double uf = ((double) m / decimation) / hop;
        const int64_t last = tracker.lastFinalFrame();
        if (last < 0)
            return 0.0f;
        const int64_t u0 = (int64_t) std::floor (uf);
        if (u0 < 0)
            return tracker.ratioCents (0);
        if (u0 >= last)
            return tracker.ratioCents (last);
        const float fr = (float) (uf - (double) u0);
        return tracker.ratioCents (u0) * (1.0f - fr) + tracker.ratioCents (u0 + 1) * fr;
    }

    void FlattenerCore::feedShifter()
    {
        const int C = shifter->getChunkSize();
        while (fed + C <= inWritten - analysisDelay)
        {
            for (int c = 0; c < numCh; ++c)
            {
                auto& ring = inRing[(size_t) c];
                float* dst = feedScratch[(size_t) c].data();
                for (int i = 0; i < C; ++i)
                    dst[i] = ring[(size_t) ((fed + i) & ringMask)];
            }

            float cents = ratioCentsAtInputSample (fed + C / 2 - alignOffset);
            if (! std::isfinite (cents))
                cents = 0.0f;
            const double scale = std::exp2 ((double) cents / 1200.0);

            shifter->processChunk ((const float* const*) feedPtrs.data(), scale,
                [this] (const float* const* data, int n)
                {
                    for (int i = 0; i < n; ++i)
                    {
                        const int64_t pos = wetWritten + i;
                        const int w = (int) (pos & ringMask);
                        if (xfadeStart > 0 && pos < xfadeFrom + xfadeLength)
                        {
                            const float g = (float) (pos - xfadeFrom) / (float) xfadeLength;   // dry -> new wet
                            for (int c = 0; c < numCh; ++c)
                                wetRing[(size_t) c][(size_t) w] = inRing[(size_t) c][(size_t) w] * (1.0f - g) + data[c][i] * g;
                        }
                        else
                        {
                            xfadeStart = 0;
                            for (int c = 0; c < numCh; ++c)
                                wetRing[(size_t) c][(size_t) w] = data[c][i];
                        }
                    }
                    wetWritten += n;
                });

            fed += C;
        }
    }

    //==========================================================================
    bool FlattenerCore::popTelemetry (TelemetryFrame& f)
    {
        if (telemetryFifo.getNumReady() <= 0)
            return false;
        const auto scope = telemetryFifo.read (1);
        if (scope.blockSize1 > 0) f = telemetryBuf[(size_t) scope.startIndex1];
        else if (scope.blockSize2 > 0) f = telemetryBuf[(size_t) scope.startIndex2];
        else return false;
        return true;
    }

    int FlattenerCore::readAnalysisAudio (float* dest, int maxSamples)
    {
        const int n = std::min (maxSamples, uiAudioFifo.getNumReady());
        if (n <= 0)
            return 0;
        const auto scope = uiAudioFifo.read (n);
        if (scope.blockSize1 > 0)
            std::copy_n (uiAudioBuf.data() + scope.startIndex1, scope.blockSize1, dest);
        if (scope.blockSize2 > 0)
            std::copy_n (uiAudioBuf.data() + scope.startIndex2, scope.blockSize2, dest + scope.blockSize1);
        return scope.blockSize1 + scope.blockSize2;
    }
}
