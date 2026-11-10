#include "SpectrogramVisualizer.h"
#include "PluginProcessor.h"
#include "dsp/PitchMeter.h"
#include "dsp/SourcePresets.h"

namespace
{
    constexpr float c_log2MinHz = 4.321928f;   // log2(20)
    constexpr float c_log2MaxHz = 13.287712f;  // log2(10000)
    constexpr int c_readoutWidth = 214;
    constexpr int c_audioChunk = 4096;

    float log2Hz (float hz) noexcept { return std::log2 (std::max (hz, 1.0f)); }
}

//==============================================================================
// Spectrogram
//==============================================================================
Spectrogram::Spectrogram()
    : m_fifo ((size_t) fftSize, 0.0f)
    , m_fftData ((size_t) fftSize * 2, 0.0f)
    , m_dbRow ((size_t) fftSize / 2 + 1, -120.0f)
    , m_binLo ((size_t) rows, -1)
    , m_binHi ((size_t) rows, -1)
    , m_binFrac ((size_t) rows, 0.0f)
{
    using namespace Theme::Colours;

    // Dark, low-contrast ramp: it sits behind the traces and must not compete with them.
    juce::ColourGradient ramp (scope, 0.0f, 0.0f, detect.interpolatedWith (text, 0.3f), 1.0f, 0.0f, false);
    ramp.addColour (0.4, juce::Colour (0xff10222a));
    ramp.addColour (0.75, detect.withMultipliedBrightness (0.5f));

    for (size_t i = 0; i < m_lut.size(); ++i)
    {
        const auto c = ramp.getColourAtPosition ((double) i / (double) (m_lut.size() - 1));
        m_lut[i] = c.getPixelARGB();
    }
}

float Spectrogram::rowToLog2Hz (float row) noexcept
{
    return c_log2MinHz + row * (c_log2MaxHz - c_log2MinHz) / (float) (rows - 1);
}

void Spectrogram::reset (double analysisRate)
{
    m_rate = analysisRate;
    std::fill (m_fifo.begin(), m_fifo.end(), 0.0f);
    m_fifoPos = 0;
    m_samplesUntilFrame = fftSize;
    m_samplesPushed = 0;
    m_columnsWritten = 0;
    m_image.clear (m_image.getBounds(), Theme::Colours::scope);

    const float binsPerHz = analysisRate > 0.0 ? (float) (fftSize / analysisRate) : 0.0f;
    const int maxBin = fftSize / 2;
    const float rowStep = (c_log2MaxHz - c_log2MinHz) / (float) (rows - 1);

    for (size_t r = 0; r < (size_t) rows; ++r)
    {
        const float centre = rowToLog2Hz ((float) r);
        const float lo = std::exp2 (centre - rowStep * 0.5f) * binsPerHz;
        const float hi = std::exp2 (centre + rowStep * 0.5f) * binsPerHz;
        const float mid = std::exp2 (centre) * binsPerHz;

        if (binsPerHz <= 0.0f || mid >= (float) maxBin)
        {
            m_binLo[r] = m_binHi[r] = -1;
            continue;
        }

        if (hi - lo > 1.0f)
        {
            // Row spans several bins: take the strongest.
            m_binLo[r] = juce::jlimit (0, maxBin, (int) std::ceil (lo));
            m_binHi[r] = juce::jlimit (m_binLo[r], maxBin, (int) std::floor (hi));
            m_binFrac[r] = 0.0f;
        }
        else
        {
            // Row is narrower than a bin: interpolate.
            m_binLo[r] = m_binHi[r] = juce::jlimit (0, maxBin - 1, (int) std::floor (mid));
            m_binFrac[r] = mid - (float) m_binLo[r];
        }
    }
}

void Spectrogram::push (const float* samples, int numSamples)
{
    if (m_rate <= 0.0)
        return;

    for (int i = 0; i < numSamples; ++i)
    {
        m_fifo[(size_t) m_fifoPos] = samples[i];
        m_fifoPos = (m_fifoPos + 1) & (fftSize - 1);

        if (--m_samplesUntilFrame == 0)
        {
            processFrame();
            m_samplesUntilFrame = hop;
        }
    }

    m_samplesPushed += numSamples;
}

void Spectrogram::processFrame()
{
    // Unroll the circular FIFO (oldest sample first) into the FFT buffer.
    const size_t head = (size_t) m_fifoPos;
    std::copy (m_fifo.begin() + (std::ptrdiff_t) head, m_fifo.end(), m_fftData.begin());
    std::copy (m_fifo.begin(), m_fifo.begin() + (std::ptrdiff_t) head, m_fftData.begin() + (std::ptrdiff_t) (fftSize - (int) head));
    std::fill (m_fftData.begin() + fftSize, m_fftData.end(), 0.0f);

    m_window.multiplyWithWindowingTable (m_fftData.data(), (size_t) fftSize);
    m_fft.performFrequencyOnlyForwardTransform (m_fftData.data(), true);

    // Full-scale sine with a Hann window peaks at fftSize / 4.
    const float norm = 4.0f / (float) fftSize;
    for (size_t b = 0; b < m_dbRow.size(); ++b)
        m_dbRow[b] = juce::Decibels::gainToDecibels (m_fftData[b] * norm, -120.0f);

    constexpr float floorDb = -100.0f, ceilDb = -24.0f;
    const int x = (int) (m_columnsWritten % columns);

    juce::Image::BitmapData data (m_image, x, 0, 1, rows, juce::Image::BitmapData::writeOnly);

    for (int r = 0; r < rows; ++r)
    {
        const auto ri = (size_t) r;
        float db = -120.0f;

        if (m_binLo[ri] >= 0)
        {
            if (m_binHi[ri] > m_binLo[ri])
            {
                for (int b = m_binLo[ri]; b <= m_binHi[ri]; ++b)
                    db = std::max (db, m_dbRow[(size_t) b]);
            }
            else
            {
                const auto b = (size_t) m_binLo[ri];
                db = m_dbRow[b] + (m_dbRow[b + 1] - m_dbRow[b]) * m_binFrac[ri];
            }
        }

        const float t = juce::jlimit (0.0f, 1.0f, (db - floorDb) / (ceilDb - floorDb));
        const auto idx = (size_t) juce::roundToInt (std::pow (t, 1.8f) * 255.0f); // gamma: keep the noise floor dark
        auto* pixel = reinterpret_cast<juce::PixelARGB*> (data.getPixelPointer (0, rows - 1 - r));
        pixel->set (m_lut[idx]);
    }

    ++m_columnsWritten;
}

//==============================================================================
// PitchDisplay
//==============================================================================
PitchDisplay::PitchDisplay (PitchFlattenerAudioProcessor& processor)
    : m_processor (processor)
    , m_history ((size_t) c_historyCapacity)
    , m_audioScratch ((size_t) c_audioChunk, 0.0f)
{
    using namespace Theme::Colours;

    auto& params = m_processor.parameters;
    m_minFreq = params.getRawParameterValue (ParamIDs::minFreq);
    m_maxFreq = params.getRawParameterValue (ParamIDs::maxFreq);
    m_gate = params.getRawParameterValue (ParamIDs::volumeThreshold);
    m_targetMode = params.getRawParameterValue (ParamIDs::targetMode);
    m_manualOverride = params.getRawParameterValue (ParamIDs::manualOverride);
    m_maxShift = params.getRawParameterValue (ParamIDs::maxShift);
    m_mix = params.getRawParameterValue (ParamIDs::mix);
    m_amount = params.getRawParameterValue (ParamIDs::flattenAmount);

    m_chips = { Chip { "SPECTRUM", textDim,                      "Spectrogram of the analysis signal (after the detection filters), drawn behind the traces. Click to toggle.", {} },
                Chip { "RAW",      detect.withAlpha (0.55f),     "Raw detector best guess for every analysis frame, before tracking. Click to toggle.", {} },
                Chip { "TRACK",    detect,                       "Detected pitch: the final cleaned pitch track the flattener follows. Click to toggle.", {} },
                Chip { "TARGET",   target,                       "Target: the pitch the sound is flattened to. Click to toggle.", {} },
                Chip { "OUTPUT",   correction,                   "Output: the detected pitch after the correction is applied. Click to toggle.", {} } };

    // Wobble warning buttons (shown with the warning)
    for (auto* b : { &m_tryButton, &m_dismissButton })
    {
        b->setColour (juce::TextButton::buttonColourId, juce::Colours::white.withAlpha (0.16f));
        b->setColour (juce::TextButton::buttonOnColourId, juce::Colours::white.withAlpha (0.28f));
        b->setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        b->setColour (juce::TextButton::textColourOnId, juce::Colours::white);
        b->setColour (juce::ComboBox::outlineColourId, juce::Colours::white.withAlpha (0.5f));
        addChildComponent (*b);
    }
    m_tryButton.onClick = [this]
    {
        if (m_wobble.recommended >= 0 && onTrySourceType)
        {
            const int type = m_wobble.recommended;
            resetWobble();
            onTrySourceType (type);
        }
    };
    m_dismissButton.setTooltip ("Hide this warning until the output settles or a setting changes");
    m_dismissButton.onClick = [this]
    {
        m_wobble.dismissed = true;
        m_tryButton.setVisible (false);
        m_dismissButton.setVisible (false);
        repaint();
    };

    setOpaque (false);
    startTimerHz (60);
}

//==============================================================================
/** Replays a copy of the last few seconds through every Source Type, off the message thread. */
class PitchDisplay::AdvisorThread final : public juce::Thread
{
public:
    AdvisorThread (std::vector<float> audio, double sampleRate, pf::ProcessingSettings settings, int sourceType)
        : juce::Thread ("Pitch Flattener advisor"),
          m_audio (std::move (audio)), m_rate (sampleRate), m_settings (settings), m_source (sourceType)
    {
        startThread (juce::Thread::Priority::low);
    }

    ~AdvisorThread() override
    {
        m_cancel.store (true);
        stopThread (10000);
    }

    void run() override
    {
        m_advice = pf::adviseSourceType (m_audio.data(), (int) m_audio.size(), m_rate, m_settings, m_source, &m_cancel);
        m_done.store (true);
    }

    bool isDone() const { return m_done.load(); }
    const pf::SourceAdvice& getAdvice() const { return m_advice; }   // only once isDone()

private:
    std::vector<float> m_audio;
    double m_rate;
    pf::ProcessingSettings m_settings;
    int m_source;
    std::atomic<bool> m_cancel { false }, m_done { false };
    pf::SourceAdvice m_advice;
};

PitchDisplay::~PitchDisplay()
{
    stopTimer();
    m_advisor.reset();   // cancels and waits for a trial in progress
}

double PitchDisplay::analysisRate() const noexcept
{
    const double rate = m_spectrogram.getRate();
    return rate > 0.0 ? rate : 24000.0;
}

//==============================================================================
void PitchDisplay::timerCallback()
{
    if (! m_primed)
    {
        // Whatever queued up while no editor was open is stale (and possibly
        // discontinuous): drop it once so both streams start in step.
        pf::TelemetryFrame discard;
        while (m_processor.popTelemetry (discard)) {}
        while (m_processor.readAnalysisAudio (m_audioScratch.data(), c_audioChunk) == c_audioChunk) {}
        m_primed = true;
        return;
    }

    const auto countBefore = m_historyCount;
    const auto samplesBefore = m_spectrogram.getSamplesPushed();

    drainTelemetry();
    drainAudio();

    const auto newFrames = m_historyCount - countBefore;
    const auto newSamples = m_spectrogram.getSamplesPushed() - samplesBefore;

    // Estimate the telemetry frame period from the two streams (both advance
    // in lockstep with the processed audio), so the time axis never assumes a hop.
    if (newFrames > 0 && newFrames < 256 && newSamples > 0)
    {
        m_hopAudioAccum += newSamples;
        m_hopFrameAccum += newFrames;

        if (m_hopFrameAccum >= 1000)
        {
            const int estimate = (int) std::llround ((double) m_hopAudioAccum / (double) m_hopFrameAccum);
            if (estimate >= 16 && estimate <= 4096)
                m_hopEstimate = estimate;

            m_hopAudioAccum = m_hopFrameAccum = 0;
        }
    }

    m_inputDb = m_processor.getInputLevelDb();

    if (--m_readoutCountdown <= 0)
    {
        m_shown = m_latest;
        m_readoutCountdown = 6;
    }

    updateViewRange();

    if (--m_wobbleCheckCountdown <= 0)
    {
        m_wobbleCheckCountdown = 15;   // 4 times a second
        checkForWobble();
    }
    pollAdvisor();

    if (isShowing())
        repaint();
}

//==============================================================================
// Wobble warning
//==============================================================================
namespace
{
    constexpr double c_checkSeconds = 3.0;      // output metered over this much of the newest decided sound
    constexpr float c_wobbleCents = 35.0f;      // median distance from the target that counts as wobbling...
    constexpr float c_settledCents = 25.0f;     // ...and below which it counts as settled (in between: no change)
    constexpr int c_minWindows = 15;            // meter readings needed before judging
    constexpr int c_showAfterChecks = 4;        // ~1 s of wobbling before the warning shows
    constexpr int c_hideAfterChecks = 8;        // ~2 s of flat output before it goes
    constexpr double c_advisorSeconds = 6.0;    // audio the advisor replays
    constexpr double c_advisorCooldownMs = 10000.0;
}

void PitchDisplay::resetWobble()
{
    m_advisor.reset();
    m_wobble = WobbleState {};
    std::fill (m_readingFrame.begin(), m_readingFrame.end(), (int64_t) -1);

    // Judge only sound that entered after this point: older history was processed the old way (or, after a reset,
    // belongs to another stretch of audio altogether).
    m_firstHistoryToJudge = m_historyCount;
    const double perFrame = m_processor.getFrameInputSamples();
    m_firstFrameToJudge = perFrame > 0.0 ? (int64_t) std::ceil ((double) m_processor.getMonitorWritten (false) / perFrame) : 0;

    m_tryButton.setVisible (false);
    m_dismissButton.setVisible (false);
    repaint();
}

namespace
{
    bool sameProcessing (const pf::ProcessingSettings& a, const pf::ProcessingSettings& b)
    {
        const auto& x = a.live;
        const auto& y = b.live;
        return a.detector == b.detector && a.minHz == b.minHz && a.maxHz == b.maxHz && a.quality == b.quality
            && a.highpassHz == b.highpassHz && a.lowpassHz == b.lowpassHz
            && x.sensitivity == y.sensitivity && x.trackingSpeedOctPerSec == y.trackingSpeedOctPerSec
            && x.smoothingMs == y.smoothingMs && x.gapFillMs == y.gapFillMs && x.targetMode == y.targetMode
            && x.targetHz == y.targetHz && x.autoWindowMs == y.autoWindowMs && x.amount == y.amount
            && x.maxShiftSemitones == y.maxShiftSemitones && x.gateDb == y.gateDb && x.manualOverride == y.manualOverride
            && x.overrideHz == y.overrideHz && x.formantPreserve == y.formantPreserve && x.shiftBelowHz == y.shiftBelowHz;
    }
}

void PitchDisplay::checkForWobble()
{
    // A reset (transport jump, new session) or any change to the processing makes what came before stale, including a
    // suggestion the advisor made for the old settings.
    const int resets = m_processor.getResetCount();
    const auto settings = m_processor.getProcessingSettings();
    if (resets != m_lastResetCount || ! m_haveLastSettings || ! sameProcessing (settings, m_lastSettings))
    {
        m_lastResetCount = resets;
        m_lastSettings = settings;
        m_haveLastSettings = true;
        resetWobble();
        return;
    }

    // A deliberately partial result (Amount or Mix turned down) isn't meant to be flat: no warning then.
    if ((m_amount != nullptr && m_amount->load() < 95.0f) || (m_mix != nullptr && m_mix->load() < 0.95f))
    {
        if (m_wobble.showing)
            resetWobble();
        return;
    }

    const double fs = m_processor.getSampleRate();
    const double perFrame = m_processor.getFrameInputSamples();
    if (m_historyCount <= m_firstHistoryToJudge || fs <= 0.0 || perFrame <= 0.0)
        return;

    // Meter at ~24 kHz whatever the host rate: plenty for pitch, and cheap enough for the message thread.
    const int down = std::max (1, (int) std::lround (fs / 24000.0));
    const double meterRate = fs / down;
    const int win = (int) std::lround (0.04 * meterRate);   // 40 ms per reading
    const int settle = (int) std::lround (0.03 * meterRate); // low-pass settling before it
    const int span = (win + settle) * down;

    const int64_t newest = m_latest.frame;
    const int64_t oldestFrame = std::max (m_firstFrameToJudge, newest - (int64_t) (c_checkSeconds * fs / perFrame));
    const int64_t oldestIndex = std::max<int64_t> ({ (int64_t) 0, m_historyCount - c_historyCapacity, m_firstHistoryToJudge });

    float loudest = -120.0f;
    for (int64_t i = m_historyCount - 1; i >= oldestIndex; --i)
    {
        const auto& f = m_history[(size_t) (i % c_historyCapacity)];
        if (f.frame < oldestFrame) break;
        loudest = std::max (loudest, f.rmsDb);
    }
    const float audibleDb = std::max (m_gate != nullptr ? m_gate->load() : -60.0f, loudest - 35.0f);

    m_meterBuf.resize ((size_t) span);
    m_meterDecimated.resize ((size_t) (win + settle));
    std::vector<float> offs;
    int eligible = 0;

    for (int64_t i = m_historyCount - 1; i >= oldestIndex; --i)
    {
        const auto& f = m_history[(size_t) (i % c_historyCapacity)];
        if (f.frame < oldestFrame || f.frame > newest)
            break;
        if (f.frame % 4 != 0 || f.targetHz <= 0.0f || f.rmsDb <= audibleDb)
            continue;   // a reading every 20 ms of audible, tracked sound

        const size_t slot = (size_t) ((f.frame / 4) % c_readingSlots);
        float off = -1.0f;
        if (m_readingFrame[slot] == f.frame)
        {
            off = m_readingOff[slot];   // measured on an earlier check
        }
        else
        {
            const int64_t centre = (int64_t) std::llround ((double) f.frame * perFrame);
            const int64_t from = centre - (int64_t) (win / 2 + settle) * down;
            if (! m_processor.copyMonitor (true, from, span, m_meterBuf.data()))
                continue;   // not output yet (or gone): no evidence either way

            pf::MeterLowPass lp;
            lp.set (fs, std::min (8.0 * f.targetHz, 0.4 * meterRate));
            for (int k = 0, d = 0; k < span; ++k)
            {
                const float y = lp.process (m_meterBuf[(size_t) k]);
                if (k % down == 0 && d < win + settle)
                    m_meterDecimated[(size_t) d++] = y;
            }

            const float hz = pf::measurePitchNear (m_meterDecimated.data() + settle, win, meterRate, f.targetHz, m_meterScratch);
            off = hz > 0.0f ? std::abs (1200.0f * std::log2 (hz / f.targetHz)) : -1.0f;
            m_readingFrame[slot] = f.frame;
            m_readingOff[slot] = off;
        }

        ++eligible;
        if (off >= 0.0f)
            offs.push_back (off);
    }

    // Judge only on clear evidence: enough readings, most of them measurable. Short or noisy sounds the meter can't
    // read (ricochets, cracks) never raise the warning.
    const bool haveEvidence = (int) offs.size() >= c_minWindows && offs.size() * 2 >= (size_t) eligible;
    float median = 0.0f;
    if (haveEvidence)
    {
        std::nth_element (offs.begin(), offs.begin() + (long) offs.size() / 2, offs.end());
        median = offs[offs.size() / 2];
    }
    if (haveEvidence && median >= c_settledCents && median <= c_wobbleCents)
        return;   // in between: hold whatever the warning is doing
    updateWobbleState (haveEvidence, haveEvidence && median > c_wobbleCents, median);
}

void PitchDisplay::updateWobbleState (bool haveEvidence, bool wobbling, float offCents)
{
    auto& w = m_wobble;
    if (! haveEvidence)
        return;

    if (wobbling)
    {
        w.goodChecks = 0;
        ++w.badChecks;
        w.offCents = w.showing ? 0.7f * w.offCents + 0.3f * offCents : offCents;
        if (! w.showing && w.badChecks >= c_showAfterChecks)
            w.showing = true;
        if (w.showing && w.advice == Advice::none && ! w.dismissed)
            startAdvisor();
    }
    else
    {
        w.badChecks = std::max (0, w.badChecks - 1);   // one good reading doesn't undo a run of bad ones
        if (w.showing && ++w.goodChecks >= c_hideAfterChecks)
        {
            m_advisor.reset();
            w = WobbleState {};
        }
    }

    layoutWarning();
    repaint();
}

void PitchDisplay::startAdvisor()
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (m_advisor != nullptr || now - m_lastAdvisorMs < c_advisorCooldownMs)
        return;

    const double fs = m_processor.getSampleRate();
    const int64_t written = m_processor.getMonitorWritten (false);
    const int64_t clear = (int64_t) std::lround (0.05 * fs);   // stay clear of the newest samples
    const int n = (int) std::min<int64_t> ((int64_t) (c_advisorSeconds * fs), written - clear);
    if (fs <= 0.0 || n < (int) (1.5 * fs))
        return;

    std::vector<float> audio ((size_t) n);
    if (! m_processor.copyMonitor (false, written - clear - n, n, audio.data()))
        return;

    m_lastAdvisorMs = now;
    m_wobble.advice = Advice::checking;
    m_advisor = std::make_unique<AdvisorThread> (std::move (audio), fs, m_processor.getProcessingSettings(), m_processor.getSourceType());
}

void PitchDisplay::pollAdvisor()
{
    if (m_advisor == nullptr || ! m_advisor->isDone())
        return;

    const auto advice = m_advisor->getAdvice();
    m_advisor.reset();

    auto& w = m_wobble;
    if (! w.showing)
        return;

    if (advice.valid && advice.recommended >= 0)
    {
        w.advice = Advice::recommend;
        w.recommended = advice.recommended;
        w.recommendedGain = advice.bySource[(size_t) advice.recommended].flatness / std::max (0.02f, advice.current.flatness);
    }
    else
    {
        w.advice = advice.valid ? Advice::noneBetter : Advice::none;
    }
    layoutWarning();
    repaint();
}

void PitchDisplay::layoutWarning()
{
    const bool show = m_wobble.showing && ! m_wobble.dismissed;
    m_warningArea = m_graphArea.reduced (12.0f, 0.0f).withTrimmedTop (34.0f).withHeight (52.0f);

    auto row = m_warningArea.reduced (10.0f, 0.0f);
    m_dismissButton.setBounds (row.removeFromRight (22.0f).withSizeKeepingCentre (22.0f, 22.0f).toNearestInt());
    row.removeFromRight (8.0f);

    const bool recommend = show && m_wobble.advice == Advice::recommend && m_wobble.recommended >= 0;
    if (recommend)
    {
        const auto name = "TRY " + juce::String (pf::getSourcePreset ((pf::SourceType) m_wobble.recommended).name).toUpperCase();
        m_tryButton.setButtonText (name);
        const float w = juce::GlyphArrangement::getStringWidth (m_fonts->label (11.0f), name) + 34.0f;
        m_tryButton.setBounds (row.removeFromRight (w).withSizeKeepingCentre (w, 26.0f).toNearestInt());
    }

    m_tryButton.setVisible (recommend);
    m_dismissButton.setVisible (show);
}

void PitchDisplay::paintWobbleWarning (juce::Graphics& g)
{
    const auto& w = m_wobble;
    if (! w.showing || w.dismissed)
        return;

    using namespace Theme::Colours;
    const auto area = m_warningArea;
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillRoundedRectangle (area.translated (0.0f, 2.0f), 7.0f);
    g.setColour (bad);
    g.fillRoundedRectangle (area, 7.0f);

    auto text = area.reduced (14.0f, 8.0f);
    text.setRight (std::min (text.getRight(), (float) (m_tryButton.isVisible() ? m_tryButton.getX() : m_dismissButton.getX()) - 10.0f));

    g.setColour (juce::Colours::white);
    g.setFont (m_fonts->heading (13.0f));
    g.drawText ("WOBBLING: OUTPUT " + juce::String (juce::roundToInt (w.offCents)) + " CT OFF TARGET",
                text.removeFromTop (18.0f), juce::Justification::centredLeft, false);

    juce::String detail;
    switch (w.advice)
    {
        case Advice::checking:   detail = "Trying the other Source Types on the last few seconds..."; break;
        case Advice::recommend:
            detail = juce::String (pf::getSourcePreset ((pf::SourceType) w.recommended).name)
                   + " comes out about " + juce::String (std::min (9.9f, w.recommendedGain), 1) + "x flatter here.";
            break;
        case Advice::noneBetter: detail = "No Source Type does clearly better. Narrow Min / Max Freq around the sound, or set a Fixed target."; break;
        case Advice::none:
        default:                 detail = "The detector is losing the pitch. Try another Source Type, or narrow Min / Max Freq."; break;
    }
    g.setFont (m_fonts->label (10.5f));
    g.setColour (juce::Colours::white.withAlpha (0.92f));
    g.drawFittedText (detail, text.toNearestInt(), juce::Justification::centredLeft, 2, 0.9f);
}

void PitchDisplay::drainTelemetry()
{
    pf::TelemetryFrame frame;

    while (m_processor.popTelemetry (frame))
    {
        m_history[(size_t) (m_historyCount % c_historyCapacity)] = frame;
        ++m_historyCount;
        m_latest = frame;
    }
}

void PitchDisplay::drainAudio()
{
    const double rate = m_processor.getAnalysisRate();

    if (rate > 0.0 && ! juce::approximatelyEqual (rate, m_spectrogram.getRate()))
        m_spectrogram.reset (rate);

    for (;;)
    {
        const int n = m_processor.readAnalysisAudio (m_audioScratch.data(), c_audioChunk);

        if (n <= 0)
            break;

        m_spectrogram.push (m_audioScratch.data(), n);

        if (n < c_audioChunk)
            break;
    }
}

void PitchDisplay::updateViewRange()
{
    const float lo = log2Hz (m_minFreq != nullptr ? m_minFreq->load() : 50.0f);
    const float hi = log2Hz (m_maxFreq != nullptr ? m_maxFreq->load() : 2000.0f);

    float rangeLo = juce::jlimit (c_log2MinHz, c_log2MaxHz - 1.0f, std::min (lo, hi) - 0.15f);
    float rangeHi = juce::jlimit (rangeLo + 1.0f, c_log2MaxHz, std::max (lo, hi) + 0.15f);

    float wantLo = rangeLo, wantHi = rangeHi;

    if (m_autoZoom && m_historyCount > 0)
    {
        // Fit everything visible in the last historySeconds, in log space.
        float minSeen = 100.0f, maxSeen = -100.0f;
        const int64_t newest = m_latest.frame;
        const double framesInWindow = historySeconds * analysisRate() / samplesPerFrame();
        const int64_t oldestIndex = std::max<int64_t> (0, m_historyCount - c_historyCapacity);

        for (int64_t i = m_historyCount - 1; i >= oldestIndex; --i)
        {
            const auto& f = m_history[(size_t) (i % c_historyCapacity)];
            const double age = (double) (newest - f.frame);
            // age < 0: frames from before the processor was re-prepared (counter restarted).
            if (age > framesInWindow || age < 0.0)
                break;

            for (const float hz : { f.trackHz, f.targetHz })
            {
                if (hz > 0.0f)
                {
                    const float l = log2Hz (hz);
                    minSeen = std::min (minSeen, l);
                    maxSeen = std::max (maxSeen, l);
                }
            }
        }

        if (maxSeen >= minSeen)
        {
            constexpr float minSpan = 1.5f;
            const float centre = 0.5f * (minSeen + maxSeen);
            const float half = std::max (minSpan * 0.5f, 0.5f * (maxSeen - minSeen) + 0.35f);
            wantLo = std::max (rangeLo, centre - half);
            wantHi = std::min (rangeHi, centre + half);

            // Keep the span when the fit hits the range edge.
            if (wantHi - wantLo < minSpan)
            {
                if (wantLo <= rangeLo) wantHi = std::min (rangeHi, wantLo + minSpan);
                else                   wantLo = std::max (rangeLo, wantHi - minSpan);
            }
        }
    }

    if (m_viewHi <= m_viewLo)
    {
        m_viewLo = wantLo;
        m_viewHi = wantHi;
        return;
    }

    constexpr float glide = 0.08f;
    m_viewLo += (wantLo - m_viewLo) * glide;
    m_viewHi += (wantHi - m_viewHi) * glide;
    m_viewHi = std::max (m_viewHi, m_viewLo + 0.5f);
}

//==============================================================================
void PitchDisplay::resized()
{
    auto bounds = getLocalBounds().toFloat();
    m_readoutArea = bounds.removeFromRight ((float) c_readoutWidth);
    m_graphArea = bounds;

    // Legend chips along the bottom of the graph.
    const auto font = m_fonts->label (10.0f);
    float x = m_graphArea.getX() + 12.0f;
    const float y = m_graphArea.getBottom() - 28.0f;

    for (auto& chip : m_chips)
    {
        const float w = juce::GlyphArrangement::getStringWidth (font, chip.name) + 30.0f;
        chip.bounds = { x, y, w, 18.0f };
        x += w + 6.0f;
    }

    m_zoomChip = { m_graphArea.getRight() - 112.0f, m_graphArea.getY() + 10.0f, 100.0f, 18.0f };
    layoutWarning();
}

float PitchDisplay::yForLog2 (float log2Value, juce::Rectangle<float> area) const noexcept
{
    const float t = (log2Value - m_viewLo) / std::max (0.01f, m_viewHi - m_viewLo);
    return area.getBottom() - t * area.getHeight();
}

void PitchDisplay::paint (juce::Graphics& g)
{
    using namespace Theme::Colours;

    const auto bounds = getLocalBounds().toFloat();
    g.setColour (scope);
    g.fillRoundedRectangle (bounds, 8.0f);

    {
        juce::Graphics::ScopedSaveState save (g);
        juce::Path clip;
        clip.addRoundedRectangle (m_graphArea.getX(), m_graphArea.getY(), m_graphArea.getWidth(), m_graphArea.getHeight(),
                                  8.0f, 8.0f, true, false, true, false);
        g.reduceClipRegion (clip);
        paintGraph (g, m_graphArea);
    }

    paintChips (g);
    paintWobbleWarning (g);
    paintReadouts (g, m_readoutArea);

    g.setColour (outlineSoft);
    g.drawVerticalLine ((int) m_readoutArea.getX(), bounds.getY() + 1.0f, bounds.getBottom() - 1.0f);
    g.drawRoundedRectangle (bounds.reduced (0.5f), 8.0f, 1.0f);
}

void PitchDisplay::paintGraph (juce::Graphics& g, juce::Rectangle<float> full)
{
    using namespace Theme::Colours;

    // Plot area: leave room for the note labels on the left and the time axis below.
    const auto area = full.withTrimmedLeft (34.0f).withTrimmedTop (34.0f).withTrimmedBottom (40.0f).withTrimmedRight (12.0f);
    const double rate = analysisRate();
    const double pxPerSample = (double) area.getWidth() / (historySeconds * rate);

    // --- Spectrogram -------------------------------------------------------
    if (m_show[spectrumTrace] && m_spectrogram.getColumnsWritten() > 0)
    {
        // The telemetry is decided after the lookahead; delay the spectrogram by
        // the same amount so the newest column lines up with the newest frame.
        const double sr = m_processor.getSampleRate();
        const double lag = sr > 0.0 ? (double) m_processor.getReportedLatency() * rate / sr : 0.0;
        const double rightSample = (double) m_spectrogram.getSamplesPushed() - lag;

        const int64_t written = m_spectrogram.getColumnsWritten();
        const int64_t firstAvailable = std::max<int64_t> (0, written - Spectrogram::columns);
        const double colW = Spectrogram::hop * pxPerSample;

        // Visible column range
        const double leftSample = rightSample - historySeconds * rate;
        int64_t kA = (int64_t) std::floor ((leftSample - Spectrogram::fftSize * 0.5) / Spectrogram::hop);
        int64_t kB = (int64_t) std::ceil ((rightSample - Spectrogram::fftSize * 0.5) / Spectrogram::hop);
        kA = std::max (kA, firstAvailable);
        kB = std::min (kB, written - 1);

        if (kB >= kA)
        {
            // Image y (pixel-centre coordinate c) -> screen y; rows are linear in log2(Hz).
            const float rowStep = (Spectrogram::rowToLog2Hz (1.0f) - Spectrogram::rowToLog2Hz (0.0f));
            const float k = area.getHeight() / std::max (0.01f, m_viewHi - m_viewLo);
            const float sy = rowStep * k;
            const float ty = area.getBottom() - (Spectrogram::rowToLog2Hz ((float) Spectrogram::rows - 0.5f) - m_viewLo) * k;

            g.setOpacity (0.6f);
            g.setImageResamplingQuality (juce::Graphics::mediumResamplingQuality);

            // At most two contiguous slices of the ring.
            int64_t k0 = kA;
            while (k0 <= kB)
            {
                const int i0 = (int) (k0 % Spectrogram::columns);
                const int64_t k1 = std::min (kB, k0 + (Spectrogram::columns - 1 - i0));

                const double x0 = area.getRight() - (rightSample - Spectrogram::columnCentre (k0)) * pxPerSample;
                const double x1 = area.getRight() - (rightSample - Spectrogram::columnCentre (k1)) * pxPerSample;

                juce::Graphics::ScopedSaveState save (g);
                g.reduceClipRegion (juce::Rectangle<float>::leftTopRightBottom ((float) (x0 - colW * 0.5), area.getY(),
                                                                                (float) (x1 + colW * 0.5), area.getBottom())
                                        .getIntersection (area)
                                        .getSmallestIntegerContainer());

                const auto sx = (float) colW;
                const auto tx = (float) (x0 - ((double) i0 + 0.5) * colW);
                g.drawImageTransformed (m_spectrogram.getImage(), juce::AffineTransform (sx, 0.0f, tx, 0.0f, sy, ty));

                k0 = k1 + 1;
            }

            g.setOpacity (1.0f);
        }
    }

    // --- Note grid -----------------------------------------------------------
    const float span = m_viewHi - m_viewLo;
    const int firstMidi = (int) std::ceil (69.0f + 12.0f * (m_viewLo - log2Hz (440.0f)));
    const int lastMidi  = (int) std::floor (69.0f + 12.0f * (m_viewHi - log2Hz (440.0f)));
    const bool fine = span < 2.6f;

    g.setFont (m_fonts->label (9.0f));

    for (int midi = std::max (0, firstMidi); midi <= std::min (135, lastMidi); ++midi)
    {
        const int pc = midi % 12;
        const bool isC = pc == 0;
        const bool natural = pc == 0 || pc == 2 || pc == 4 || pc == 5 || pc == 7 || pc == 9 || pc == 11;

        if (! isC && ! (fine && natural))
            continue;

        const float hz = 440.0f * std::exp2 ((float) (midi - 69) / 12.0f);
        const float y = yForLog2 (log2Hz (hz), area);

        g.setColour (isC ? grid.brighter (0.25f) : grid.withAlpha (0.7f));
        g.drawHorizontalLine (juce::roundToInt (y), area.getX(), area.getRight());

        if (isC || span < 1.6f)
        {
            g.setColour (isC ? textFaint : textFaint.withAlpha (0.6f));
            g.drawText (Theme::noteName (hz), juce::Rectangle<float> (full.getX() + 4.0f, y - 7.0f, 28.0f, 14.0f),
                        juce::Justification::centredLeft, false);
        }
    }

    // --- Time grid -------------------------------------------------------------
    for (int s = 0; s <= (int) historySeconds; ++s)
    {
        const float x = area.getRight() - (float) s * area.getWidth() / (float) historySeconds;
        g.setColour (grid.withAlpha (s == 0 ? 0.9f : 0.45f));
        g.drawVerticalLine (juce::roundToInt (x), area.getY(), area.getBottom());

        if (s % 2 == 0)
        {
            g.setColour (textFaint.withAlpha (0.8f));
            g.drawText (s == 0 ? juce::String ("0 s") : "-" + juce::String (s) + " s",
                        juce::Rectangle<float> (x - 20.0f, area.getBottom() + 2.0f, 40.0f, 12.0f),
                        juce::Justification::centred, false);
        }
    }

    // --- Traces ------------------------------------------------------------------
    m_rawPath.clear();
    m_trackPath.clear();
    m_targetPath.clear();
    m_outputPath.clear();

    bool anyVisible = false;

    if (m_historyCount > 0)
    {
        const int64_t newest = m_latest.frame;
        const double pxPerFrame = samplesPerFrame() * pxPerSample;
        const int64_t oldestIndex = std::max<int64_t> (0, m_historyCount - c_historyCapacity);

        bool trackOn = false, targetOn = false, outputOn = false;
        int64_t prevFrame = 0;

        const auto addPoint = [] (juce::Path& path, bool& on, bool contiguous, float x, float y)
        {
            if (on && contiguous) path.lineTo (x, y);
            else                  path.startNewSubPath (x, y);
            on = true;
        };

        for (int64_t i = m_historyCount - 1; i >= oldestIndex; --i)
        {
            const auto& f = m_history[(size_t) (i % c_historyCapacity)];
            const double ageFrames = (double) (newest - f.frame);
            const float x = (float) ((double) area.getRight() - ageFrames * pxPerFrame);

            if (x < area.getX() - 4.0f || ageFrames < 0.0)
                break;

            const bool contiguous = i < m_historyCount - 1 && (prevFrame - f.frame) <= 2;
            prevFrame = f.frame;

            if (f.rawHz > 0.0f)
                m_rawPath.addRectangle (x - 1.0f, yForLog2 (log2Hz (f.rawHz), area) - 1.0f, 2.0f, 2.0f);

            if (f.trackHz > 0.0f)
            {
                anyVisible = true;
                addPoint (m_trackPath, trackOn, contiguous, x, yForLog2 (log2Hz (f.trackHz), area));

                const float outHz = f.trackHz * std::exp2 (f.ratioCents / 1200.0f);
                addPoint (m_outputPath, outputOn, contiguous, x, yForLog2 (log2Hz (outHz), area));
            }
            else
            {
                trackOn = outputOn = false;
            }

            if (f.targetHz > 0.0f)
                addPoint (m_targetPath, targetOn, contiguous, x, yForLog2 (log2Hz (f.targetHz), area));
            else
                targetOn = false;
        }

        const juce::PathStrokeType thin (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
        const juce::PathStrokeType thick (2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
        const juce::PathStrokeType glow (7.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (area.withTrimmedTop (-6.0f).getSmallestIntegerContainer());

        if (m_show[rawTrace])
        {
            g.setColour (detect.withAlpha (0.35f));
            g.fillPath (m_rawPath);
        }

        if (m_show[targetTrace])
        {
            g.setColour (target.withAlpha (0.14f));
            g.strokePath (m_targetPath, glow);
            g.setColour (target.withAlpha (0.95f));
            g.strokePath (m_targetPath, thin);
        }

        if (m_show[outputTrace])
        {
            g.setColour (correction.withAlpha (0.9f));
            g.strokePath (m_outputPath, thin);
        }

        if (m_show[trackTrace])
        {
            g.setColour (detect.withAlpha (0.14f));
            g.strokePath (m_trackPath, glow);
            g.setColour (detect);
            g.strokePath (m_trackPath, thick);
        }
    }

    // --- Labels ------------------------------------------------------------------
    {
        const double sr = m_processor.getSampleRate();
        const double latencySeconds = sr > 0.0 ? (double) m_processor.getReportedLatency() / sr : 0.0;

        g.setFont (m_fonts->heading (10.5f));
        g.setColour (text.withAlpha (0.85f));
        auto titleRow = juce::Rectangle<float> (full.getX() + 14.0f, full.getY() + 10.0f, full.getWidth() - 140.0f, 18.0f);
        const juce::String title = "DETECTED PITCH";
        const float w = juce::GlyphArrangement::getStringWidth (m_fonts->heading (10.5f), title);
        g.drawText (title, titleRow.removeFromLeft (w + 4.0f), juce::Justification::centredLeft, false);

        g.setFont (m_fonts->label (9.5f));
        g.setColour (textFaint);
        g.drawText (latencySeconds > 0.0 ? "  /  SHOWN AFTER " + juce::String (latencySeconds, 2) + " s LOOKAHEAD"
                                         : juce::String ("  /  SHOWN AFTER LOOKAHEAD"),
                    titleRow, juce::Justification::centredLeft, false);
    }

    if (! anyVisible)
    {
        g.setFont (m_fonts->label (10.5f));
        g.setColour (textFaint);
        g.drawText (m_historyCount == 0 ? "WAITING FOR AUDIO" : "NO PITCH", area, juce::Justification::centred, false);
    }
}

void PitchDisplay::paintChips (juce::Graphics& g)
{
    using namespace Theme::Colours;

    const auto font = m_fonts->label (10.0f);

    for (size_t i = 0; i < m_chips.size(); ++i)
    {
        const auto& chip = m_chips[i];
        const bool on = m_show[i];
        const bool hover = m_hoverChip == (int) i;

        g.setColour (scope.withAlpha (0.75f));
        g.fillRoundedRectangle (chip.bounds, 9.0f);
        g.setColour (hover ? outline.brighter (0.3f) : outlineSoft);
        g.drawRoundedRectangle (chip.bounds.reduced (0.5f), 9.0f, 1.0f);

        g.setColour (chip.colour.withMultipliedAlpha (on ? 1.0f : 0.3f));
        g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ chip.bounds.getX() + 11.0f, chip.bounds.getCentreY() }));

        g.setFont (font);
        g.setColour (text.withAlpha (on ? 0.9f : 0.35f));
        g.drawText (chip.name, chip.bounds.withTrimmedLeft (21.0f), juce::Justification::centredLeft, false);
    }

    const bool hover = m_hoverChip == numTraces;
    g.setColour (scope.withAlpha (0.75f));
    g.fillRoundedRectangle (m_zoomChip, 9.0f);
    g.setColour (hover ? outline.brighter (0.3f) : outlineSoft);
    g.drawRoundedRectangle (m_zoomChip.reduced (0.5f), 9.0f, 1.0f);
    g.setFont (font);
    g.setColour (m_autoZoom ? text.withAlpha (0.9f) : textDim);
    g.drawText (m_autoZoom ? "ZOOM: AUTO" : "ZOOM: RANGE", m_zoomChip, juce::Justification::centred, false);
}

void PitchDisplay::paintReadouts (juce::Graphics& g, juce::Rectangle<float> area)
{
    using namespace Theme::Colours;

    auto r = area.reduced (16.0f, 12.0f);
    const auto& f = m_shown;

    const auto caption = [&] (juce::Rectangle<float> row, const juce::String& name, juce::Colour dot)
    {
        g.setColour (dot);
        g.fillEllipse (row.removeFromLeft (6.0f).withSizeKeepingCentre (6.0f, 6.0f));
        row.removeFromLeft (7.0f);
        g.setFont (m_fonts->label (9.5f));
        g.setColour (textDim);
        g.drawText (name, row, juce::Justification::centredLeft, false);
    };

    const auto noteText = [] (float hz)
    {
        const auto note = Theme::noteName (hz);
        if (note.isEmpty())
            return juce::String();
        return note + "  " + Theme::signedText (Theme::centsFromNote (hz), " ct", 0);
    };

    // Detected
    caption (r.removeFromTop (12.0f), "DETECTED", detect);
    r.removeFromTop (2.0f);
    {
        auto valueRow = r.removeFromTop (24.0f);
        g.setFont (m_fonts->body (20.0f));
        g.setColour (f.trackHz > 0.0f ? detect : textFaint);
        g.drawText (f.trackHz > 0.0f ? Theme::formatHertz (f.trackHz) : juce::String ("--"), valueRow, juce::Justification::centredLeft, false);

        g.setFont (m_fonts->label (10.5f));
        g.setColour (textDim);
        g.drawText (f.trackHz > 0.0f ? noteText (f.trackHz) : juce::String ("NO PITCH"), r.removeFromTop (14.0f),
                    juce::Justification::centredLeft, false);
    }

    r.removeFromTop (9.0f);

    // Target
    const int mode = m_targetMode != nullptr ? juce::roundToInt (m_targetMode->load()) : 0;
    const bool overrideOn = m_manualOverride != nullptr && m_manualOverride->load() > 0.5f;
    {
        caption (r.removeFromTop (12.0f), "TARGET", target);
        r.removeFromTop (2.0f);

        auto valueRow = r.removeFromTop (18.0f);
        g.setFont (m_fonts->body (15.0f));
        g.setColour (f.targetHz > 0.0f ? target : textFaint);
        const auto valueText = f.targetHz > 0.0f ? Theme::formatHertz (f.targetHz) : juce::String ("--");
        g.drawText (valueText, valueRow, juce::Justification::centredLeft, false);

        if (f.targetHz > 0.0f)
        {
            const float w = juce::GlyphArrangement::getStringWidth (m_fonts->body (15.0f), valueText);
            g.setFont (m_fonts->label (10.5f));
            g.setColour (textDim);
            g.drawText (Theme::noteName (f.targetHz), valueRow.withTrimmedLeft (w + 8.0f), juce::Justification::centredLeft, false);
        }

        juce::String status;
        juce::Colour statusColour = textFaint;

        if (overrideOn)
        {
            status = "MANUAL OVERRIDE";
            statusColour = target.withAlpha (0.8f);
        }
        else if (mode == (int) pf::TargetMode::Fixed)
        {
            status = "FIXED TARGET";
        }
        else if (mode == (int) pf::TargetMode::Auto)
        {
            status = "AUTO: FOLLOWS CONTOUR";
        }
        else
        {
            const float latched = m_processor.getLatchedHz();
            status = latched > 0.0f ? "LATCHED AT " + Theme::formatHertz (latched).toUpperCase() : juce::String ("LISTENING TO LATCH");
            statusColour = latched > 0.0f ? good.withAlpha (0.85f) : textFaint;
        }

        g.setFont (m_fonts->label (9.5f));
        g.setColour (statusColour);
        g.drawText (status, r.removeFromTop (14.0f), juce::Justification::centredLeft, false);
    }

    r.removeFromTop (9.0f);

    // Correction
    {
        auto row = r.removeFromTop (13.0f);
        const bool voiced = f.trackHz > 0.0f;
        g.setFont (m_fonts->body (12.0f));
        g.setColour (voiced ? correction : textFaint);
        g.drawText (voiced ? Theme::signedText (f.ratioCents, " ct", 0) : juce::String ("--"), row, juce::Justification::centredRight, false);
        caption (row, "CORRECTION", correction);

        r.removeFromTop (5.0f);
        const auto bar = r.removeFromTop (5.0f);
        g.setColour (outline);
        g.fillRoundedRectangle (bar, 2.5f);

        // Bar spans +-maxShift; fills from the centre.
        const float maxCents = 100.0f * (m_maxShift != nullptr ? std::max (1.0f, m_maxShift->load()) : 12.0f);
        const float t = voiced ? juce::jlimit (-1.0f, 1.0f, f.ratioCents / maxCents) : 0.0f;
        const float cx = bar.getCentreX();
        const float x0 = std::min (cx, cx + t * bar.getWidth() * 0.5f);
        const float x1 = std::max (cx, cx + t * bar.getWidth() * 0.5f);
        g.setColour (std::abs (t) > 0.98f ? warning : correction);
        g.fillRoundedRectangle (juce::Rectangle<float>::leftTopRightBottom (x0, bar.getY(), std::max (x1, x0 + 2.0f), bar.getBottom()), 2.5f);
        g.setColour (textFaint);
        g.fillRect (juce::Rectangle<float> (1.0f, 9.0f).withCentre ({ cx, bar.getCentreY() }));
    }

    r.removeFromTop (9.0f);

    // Confidence
    {
        auto row = r.removeFromTop (13.0f);
        g.setFont (m_fonts->body (11.5f));
        g.setColour (textDim);
        g.drawText (juce::String (juce::roundToInt (f.voicedProb * 100.0f)) + "%", row, juce::Justification::centredRight, false);
        caption (row, "CONFIDENCE", detect);

        r.removeFromTop (5.0f);
        const auto bar = r.removeFromTop (5.0f);
        g.setColour (outline);
        g.fillRoundedRectangle (bar, 2.5f);
        g.setColour (detect.withAlpha (0.85f));
        g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * juce::jlimit (0.0f, 1.0f, m_latest.voicedProb)), 2.5f);
    }

    r.removeFromTop (9.0f);

    // Input level vs gate (live, not delayed)
    {
        const float gateDb = m_gate != nullptr ? m_gate->load() : -50.0f;
        const bool open = m_inputDb > gateDb;

        auto row = r.removeFromTop (13.0f);
        g.setFont (m_fonts->body (11.5f));
        g.setColour (open ? good : textFaint);
        g.drawText (m_inputDb <= -100.0f ? juce::String ("-inf dB") : juce::String (m_inputDb, 1) + " dB",
                    row, juce::Justification::centredRight, false);
        caption (row, "INPUT", open ? good : textFaint);

        r.removeFromTop (5.0f);
        const auto bar = r.removeFromTop (5.0f);
        const auto toX = [&bar] (float db) { return bar.getX() + bar.getWidth() * juce::jlimit (0.0f, 1.0f, (db + 80.0f) / 80.0f); };

        g.setColour (outline);
        g.fillRoundedRectangle (bar, 2.5f);
        g.setColour ((open ? good : textFaint).withAlpha (0.85f));
        g.fillRoundedRectangle (bar.withRight (toX (m_inputDb)), 2.5f);
        g.setColour (text);
        g.fillRect (juce::Rectangle<float> (1.5f, 9.0f).withCentre ({ toX (gateDb), bar.getCentreY() }));
    }

    // Footer: latency and underruns
    {
        auto row = area.reduced (16.0f, 10.0f).removeFromBottom (12.0f);
        const double sr = m_processor.getSampleRate();
        const double latency = sr > 0.0 ? (double) m_processor.getReportedLatency() / sr : 0.0;
        const int underruns = m_processor.getUnderrunCount();

        g.setFont (m_fonts->label (9.5f));
        g.setColour (textFaint);
        g.drawText ("LATENCY " + juce::String (latency, 2) + " s", row, juce::Justification::centredLeft, false);

        if (underruns > 0)
        {
            g.setColour (warning);
            g.drawText ("UNDERRUNS " + juce::String (underruns), row, juce::Justification::centredRight, false);
        }
    }
}

//==============================================================================
void PitchDisplay::mouseDown (const juce::MouseEvent& e)
{
    const auto p = e.position;

    for (size_t i = 0; i < m_chips.size(); ++i)
    {
        if (m_chips[i].bounds.contains (p))
        {
            m_show[i] = ! m_show[i];
            repaint();
            return;
        }
    }

    if (m_zoomChip.contains (p))
    {
        m_autoZoom = ! m_autoZoom;
        repaint();
    }
}

void PitchDisplay::mouseMove (const juce::MouseEvent& e)
{
    int hover = -1;

    for (size_t i = 0; i < m_chips.size(); ++i)
        if (m_chips[i].bounds.contains (e.position))
            hover = (int) i;

    if (m_zoomChip.contains (e.position))
        hover = numTraces;

    if (hover != m_hoverChip)
    {
        m_hoverChip = hover;
        setMouseCursor (hover >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void PitchDisplay::mouseExit (const juce::MouseEvent&)
{
    if (m_hoverChip != -1)
    {
        m_hoverChip = -1;
        setMouseCursor (juce::MouseCursor::NormalCursor);
        repaint();
    }
}

juce::String PitchDisplay::getHelpTextAt (juce::Point<int> localPoint) const
{
    const auto p = localPoint.toFloat();

    if (m_wobble.showing && ! m_wobble.dismissed && m_warningArea.contains (p))
        return "Wobbling: the output pitch is measured against the target, and it is staying well off it. The plugin replays the last "
               "few seconds through every Source Type and suggests one only if it comes out clearly flatter.";

    for (const auto& chip : m_chips)
        if (chip.bounds.contains (p))
            return chip.help;

    if (m_zoomChip.contains (p))
        return "Zoom: AUTO fits the view to the recent pitch; RANGE shows the whole Min..Max Freq detection range. Click to switch.";

    if (m_readoutArea.contains (p))
        return "Readouts for the newest analysed frame: detected pitch, target, correction in cents (bar spans Max Shift), "
               "voicing confidence, live input level against the gate (white tick), and the plugin latency.";

    return "Pitch history over the last 8 s on a log-frequency axis with note lines. The display runs behind the input by the "
           "lookahead, because each frame is shown once the tracker has decided it.";
}
