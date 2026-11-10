#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "StateMigration.h"

namespace
{
    juce::ParameterID pid (const char* id) { return { id, 1 }; }

    juce::NormalisableRange<float> hzRange (float lo, float hi, float centre)
    {
        juce::NormalisableRange<float> r (lo, hi, 0.1f);
        r.setSkewForCentre (centre);
        return r;
    }

    juce::String hzText (float v, int)
    {
        return v >= 1000.0f ? juce::String (v / 1000.0f, 2) + " kHz" : juce::String (v, v < 100.0f ? 1 : 0) + " Hz";
    }

    juce::StringArray sourceTypeNames()
    {
        juce::StringArray s;
        for (int i = 0; i < (int) pf::SourceType::NumSourceTypes; ++i)
            s.add (pf::getSourcePreset ((pf::SourceType) i).name);
        return s;
    }

    juce::StringArray detectorNames()
    {
        juce::StringArray s;
        for (int i = 0; i < (int) pf::DetectorType::NumDetectors; ++i)
            s.add (pf::getDetectorName ((pf::DetectorType) i));
        return s;
    }

    constexpr int defaultSource = (int) pf::SourceType::Siren;
}

//==============================================================================
PitchFlattenerAudioProcessor::PitchFlattenerAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, "Parameters", createParameterLayout())
{
    auto get = [this] (const char* id) { return parameters.getRawParameterValue (id); };
    raw.sourceType        = get (ParamIDs::sourceType);
    raw.detector          = get (ParamIDs::detector);
    raw.minFreq           = get (ParamIDs::minFreq);
    raw.maxFreq           = get (ParamIDs::maxFreq);
    raw.sensitivity       = get (ParamIDs::sensitivity);
    raw.trackingSpeed     = get (ParamIDs::trackingSpeed);
    raw.trackSmoothing    = get (ParamIDs::trackSmoothing);
    raw.gapFill           = get (ParamIDs::gapFill);
    raw.volumeThreshold   = get (ParamIDs::volumeThreshold);
    raw.detectionHighpass = get (ParamIDs::detectionHighpass);
    raw.detectionLowpass  = get (ParamIDs::detectionLowpass);
    raw.targetMode        = get (ParamIDs::targetMode);
    raw.targetPitch       = get (ParamIDs::targetPitch);
    raw.autoWindow        = get (ParamIDs::autoWindow);
    raw.flattenAmount     = get (ParamIDs::flattenAmount);
    raw.maxShift          = get (ParamIDs::maxShift);
    raw.manualOverride    = get (ParamIDs::manualOverride);
    raw.overrideFreq      = get (ParamIDs::overrideFreq);
    raw.rbFormantPreserve = get (ParamIDs::rbFormantPreserve);
    raw.quality           = get (ParamIDs::quality);
    raw.mix               = get (ParamIDs::mix);
    raw.shiftBelow        = get (ParamIDs::shiftBelow);

    // Structural changes (detector, range, quality) are picked up by the timer on the
    // message thread, or synchronously in processBlock when rendering offline.
    startTimerHz (10);
}

PitchFlattenerAudioProcessor::~PitchFlattenerAudioProcessor()
{
    stopTimer();
    cancelPendingUpdate();
}

juce::AudioProcessorValueTreeState::ParameterLayout PitchFlattenerAudioProcessor::createParameterLayout()
{
    using namespace juce;
    const auto& d = pf::getSourcePreset ((pf::SourceType) defaultSource);
    std::vector<std::unique_ptr<RangedAudioParameter>> p;

    auto pct = [] (float v, int) { return String (juce::roundToInt (v)) + " %"; };
    auto ms  = [] (float v, int) { return String (juce::roundToInt (v)) + " ms"; };

    // Source / detection
    // Source Type is a label for the editor (it writes its defaults into the other parameters), not automatable.
    p.push_back (std::make_unique<AudioParameterChoice> (pid (ParamIDs::sourceType), "Source Type", sourceTypeNames(), defaultSource,
                                                         AudioParameterChoiceAttributes().withAutomatable (false)));
    // Detector, Speed, Smoothing and Gap Fill are set by the Source Type (Detector is adjustable on Custom): not for automation.
    p.push_back (std::make_unique<AudioParameterChoice> (pid (ParamIDs::detector), "Detector", detectorNames(), (int) d.detector,
                                                         AudioParameterChoiceAttributes().withAutomatable (false)));
    p.push_back (std::make_unique<AudioParameterFloat> (pid (ParamIDs::minFreq), "Min Frequency", hzRange (20.0f, 2000.0f, 200.0f), d.minHz,
                                                        AudioParameterFloatAttributes().withLabel ("Hz").withStringFromValueFunction (hzText)));
    p.push_back (std::make_unique<AudioParameterFloat> (pid (ParamIDs::maxFreq), "Max Frequency", hzRange (100.0f, 8000.0f, 1500.0f), d.maxHz,
                                                        AudioParameterFloatAttributes().withLabel ("Hz").withStringFromValueFunction (hzText)));
    p.push_back (std::make_unique<AudioParameterFloat> (pid (ParamIDs::sensitivity), "Sensitivity", NormalisableRange<float> (0.0f, 100.0f, 1.0f), d.sensitivity * 100.0f,
                                                        AudioParameterFloatAttributes().withLabel ("%").withStringFromValueFunction (pct)));
    {
        NormalisableRange<float> r (0.5f, 40.0f, 0.1f);
        r.setSkewForCentre (6.0f);
        p.push_back (std::make_unique<AudioParameterFloat> (pid (ParamIDs::trackingSpeed), "Tracking Speed", r, d.trackingSpeed,
                                                            AudioParameterFloatAttributes().withLabel ("oct/s").withAutomatable (false)
                                                                .withStringFromValueFunction ([] (float v, int) { return String (v, v < 10.0f ? 1 : 0) + " oct/s"; })));
    }
    p.push_back (std::make_unique<AudioParameterFloat> (pid (ParamIDs::trackSmoothing), "Smoothing", NormalisableRange<float> (0.0f, 50.0f, 0.5f), d.smoothingMs,
                                                        AudioParameterFloatAttributes().withLabel ("ms").withStringFromValueFunction (ms).withAutomatable (false)));
    p.push_back (std::make_unique<AudioParameterFloat> (pid (ParamIDs::gapFill), "Gap Fill", NormalisableRange<float> (0.0f, 400.0f, 1.0f), d.gapFillMs,
                                                        AudioParameterFloatAttributes().withLabel ("ms").withStringFromValueFunction (ms).withAutomatable (false)));
    p.push_back (std::make_unique<AudioParameterFloat> (pid (ParamIDs::volumeThreshold), "Gate", NormalisableRange<float> (-80.0f, -40.0f, 0.1f), -60.0f,
                                                        AudioParameterFloatAttributes().withLabel ("dB")
                                                            .withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " dB"; })));
    p.push_back (std::make_unique<AudioParameterFloat> (pid (ParamIDs::detectionHighpass), "Detection High-pass", hzRange (20.0f, 2000.0f, 200.0f), d.highpassHz,
                                                        AudioParameterFloatAttributes().withLabel ("Hz").withStringFromValueFunction (hzText)));
    p.push_back (std::make_unique<AudioParameterFloat> (pid (ParamIDs::detectionLowpass), "Detection Low-pass", hzRange (500.0f, 12000.0f, 4000.0f), d.lowpassHz,
                                                        AudioParameterFloatAttributes().withLabel ("Hz").withStringFromValueFunction (hzText)));

    // Target / correction
    p.push_back (std::make_unique<AudioParameterChoice> (pid (ParamIDs::targetMode), "Target Mode", StringArray { "Latch", "Fixed", "Auto" }, 0));
    p.push_back (std::make_unique<AudioParameterFloat> (pid (ParamIDs::targetPitch), "Target Pitch", hzRange (20.0f, 4000.0f, 440.0f), 440.0f,
                                                        AudioParameterFloatAttributes().withLabel ("Hz").withStringFromValueFunction (hzText)));
    {
        // More of the knob for the 200-600 ms vibrato and warble zone.
        NormalisableRange<float> r (200.0f, 2000.0f, 10.0f);
        r.setSkewForCentre (600.0f);
        p.push_back (std::make_unique<AudioParameterFloat> (pid (ParamIDs::autoWindow), "Auto Window", r, 1000.0f,
                                                            AudioParameterFloatAttributes().withLabel ("ms").withStringFromValueFunction (ms)));
    }
    p.push_back (std::make_unique<AudioParameterFloat> (pid (ParamIDs::flattenAmount), "Flatten Amount", NormalisableRange<float> (0.0f, 100.0f, 1.0f), 100.0f,
                                                        AudioParameterFloatAttributes().withLabel ("%").withStringFromValueFunction (pct)));
    p.push_back (std::make_unique<AudioParameterFloat> (pid (ParamIDs::maxShift), "Max Shift", NormalisableRange<float> (6.0f, 36.0f, 0.5f), d.maxShiftSemitones,
                                                        AudioParameterFloatAttributes().withLabel ("st")
                                                            .withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " st"; })));
    // Retired: Manual Override did what Fixed + Target Pitch do. Kept (hidden, not automatable) so old sessions load; a
    // state with it on is converted to Fixed at the Override pitch (StateMigration::overrideToFixed).
    p.push_back (std::make_unique<AudioParameterBool> (pid (ParamIDs::manualOverride), "Manual Override (retired)", false,
                                                       AudioParameterBoolAttributes().withAutomatable (false)));
    p.push_back (std::make_unique<AudioParameterFloat> (pid (ParamIDs::overrideFreq), "Override Frequency (retired)", hzRange (20.0f, 4000.0f, 440.0f), 440.0f,
                                                        AudioParameterFloatAttributes().withLabel ("Hz").withStringFromValueFunction (hzText).withAutomatable (false)));

    // Shifter / output
    p.push_back (std::make_unique<AudioParameterBool> (pid (ParamIDs::rbFormantPreserve), "Formant Preserve", d.formant));
    p.push_back (std::make_unique<AudioParameterChoice> (pid (ParamIDs::quality), "Quality", StringArray { "Smooth", "Tight" }, d.quality));
    p.push_back (std::make_unique<AudioParameterFloat> (pid (ParamIDs::mix), "Mix", NormalisableRange<float> (0.0f, 1.0f, 0.01f), 1.0f,
                                                        AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v * 100.0f)) + " %"; })));

    // Shift Below: the top of the range means Off (the whole signal is shifted), which is also what sessions saved before it existed get.
    p.push_back (std::make_unique<AudioParameterFloat> (pid (ParamIDs::shiftBelow), "Shift Below", hzRange (200.0f, pf::shiftBelowOffHz, 1500.0f),
                                                        d.shiftBelowHz > 0.0f ? d.shiftBelowHz : pf::shiftBelowOffHz,
                                                        AudioParameterFloatAttributes().withLabel ("Hz")
                                                            .withStringFromValueFunction ([] (float v, int n) { return v >= pf::shiftBelowOffHz - 1.0f ? String ("Off") : hzText (v, n); })));

    return { p.begin(), p.end() };
}

//==============================================================================
pf::DetectorConfig PitchFlattenerAudioProcessor::wantedDetectorConfig() const
{
    pf::DetectorConfig c;
    c.type = (pf::DetectorType) juce::jlimit (0, (int) pf::DetectorType::NumDetectors - 1, (int) raw.detector->load());
    c.minHz = raw.minFreq->load();
    c.maxHz = raw.maxFreq->load();
    return c;
}

pf::LiveSettings PitchFlattenerAudioProcessor::readLiveSettings() const
{
    pf::LiveSettings s;
    s.sensitivity = raw.sensitivity->load() * 0.01f;
    s.trackingSpeedOctPerSec = raw.trackingSpeed->load();
    s.smoothingMs = raw.trackSmoothing->load();
    s.gapFillMs = raw.gapFill->load();
    s.targetMode = (pf::TargetMode) juce::jlimit (0, (int) pf::TargetMode::NumModes - 1, (int) raw.targetMode->load());
    s.targetHz = raw.targetPitch->load();
    s.autoWindowMs = raw.autoWindow->load();
    s.amount = raw.flattenAmount->load() * 0.01f;
    s.maxShiftSemitones = raw.maxShift->load();
    s.gateDb = raw.volumeThreshold->load();
    // Manual Override is retired (stored states are converted to Fixed on load): a host or stray preset setting the
    // hidden parameter must not take over the target where the panel can't show it.
    s.manualOverride = false;
    s.mix = raw.mix->load();
    s.formantPreserve = raw.rbFormantPreserve->load() > 0.5f;
    s.shiftBelowHz = pf::shiftBelowFromParam (raw.shiftBelow->load());
    return s;
}

pf::ProcessingSettings PitchFlattenerAudioProcessor::getProcessingSettings() const
{
    pf::ProcessingSettings s;
    s.live = readLiveSettings();
    const auto c = wantedDetectorConfig();
    s.detector = c.type;
    s.minHz = c.minHz;
    s.maxHz = c.maxHz;
    s.quality = (int) raw.quality->load();
    s.highpassHz = raw.detectionHighpass->load();
    s.lowpassHz = raw.detectionLowpass->load();
    return s;
}

int PitchFlattenerAudioProcessor::getSourceType() const
{
    return juce::jlimit (0, (int) pf::SourceType::NumSourceTypes - 1, juce::roundToInt (raw.sourceType->load()));
}

void PitchFlattenerAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    const int channels = juce::jmax (1, getTotalNumOutputChannels());
    core.setLiveSettings (readLiveSettings());
    core.prepare (sampleRate, samplesPerBlock, channels, wantedDetectorConfig(), (int) raw.quality->load());
    setLatencySamples (core.getLatencySamples());
}

void PitchFlattenerAudioProcessor::releaseResources()
{
    core.releaseResources();
}

void PitchFlattenerAudioProcessor::reset()
{
    // Flush ~2.6 s of in-flight audio and tracker state (done on the audio thread).
    core.requestReset();
}

bool PitchFlattenerAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return out == layouts.getMainInputChannelSet();
}

double PitchFlattenerAudioProcessor::getTailLengthSeconds() const
{
    return currentSampleRate > 0.0 ? (double) getLatencySamples() / currentSampleRate : 0.0;
}

void PitchFlattenerAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    for (auto i = getTotalNumInputChannels(); i < getTotalNumOutputChannels(); ++i)
        buffer.clear (i, 0, buffer.getNumSamples());

    if (! core.isPrepared() || buffer.getNumChannels() == 0)
        return;

    if (isNonRealtime())
    {
        // Offline render: the message thread may not run between blocks, so apply
        // detector/quality changes right here (allocation is fine when not realtime).
        const auto c = wantedDetectorConfig();
        core.applyStructuralNow (c.type, c.minHz, c.maxHz, (int) raw.quality->load());
    }

    core.setNonRealtime (isNonRealtime());
    core.setLiveSettings (readLiveSettings());
    core.setFilters (raw.detectionHighpass->load(), raw.detectionLowpass->load());
    core.process (buffer.getArrayOfWritePointers(), juce::jmin (2, buffer.getNumChannels()), buffer.getNumSamples());
}

void PitchFlattenerAudioProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    // Keep the dry signal delayed by the reported latency so bypassing doesn't shift audio in time.
    juce::ScopedNoDenormals noDenormals;
    if (! core.isPrepared() || buffer.getNumChannels() == 0)
        return;
    auto s = readLiveSettings();
    s.mix = 0.0f;
    core.setLiveSettings (s);
    core.setFilters (raw.detectionHighpass->load(), raw.detectionLowpass->load());
    core.process (buffer.getArrayOfWritePointers(), juce::jmin (2, buffer.getNumChannels()), buffer.getNumSamples());
}

//==============================================================================
void PitchFlattenerAudioProcessor::handleAsyncUpdate()
{
    if (! core.isPrepared())
        return;
    const auto c = wantedDetectorConfig();
    core.setDetectorConfig (c.type, c.minHz, c.maxHz);
    core.setQuality ((int) raw.quality->load());
}

void PitchFlattenerAudioProcessor::timerCallback()
{
    // Deletes retired objects and retries any rebuild that had to wait for a free slot.
    if (core.isPrepared())
        handleAsyncUpdate();
    core.housekeeping();
}

void PitchFlattenerAudioProcessor::applySourceTypeDefaults (pf::SourceType type)
{
    const auto& p = pf::getSourcePreset (type);

    auto set = [this] (const char* id, float value)
    {
        if (auto* param = parameters.getParameter (id))
        {
            param->beginChangeGesture();
            param->setValueNotifyingHost (param->convertTo0to1 (value));
            param->endChangeGesture();
        }
    };

    set (ParamIDs::sourceType, (float) (int) type);
    if (type == pf::SourceType::Custom)
        return;   // Custom keeps whatever is currently set

    set (ParamIDs::detector, (float) (int) p.detector);
    set (ParamIDs::minFreq, p.minHz);
    set (ParamIDs::maxFreq, p.maxHz);
    set (ParamIDs::sensitivity, p.sensitivity * 100.0f);
    set (ParamIDs::trackingSpeed, p.trackingSpeed);
    set (ParamIDs::trackSmoothing, p.smoothingMs);
    set (ParamIDs::gapFill, p.gapFillMs);
    set (ParamIDs::detectionHighpass, p.highpassHz);
    set (ParamIDs::detectionLowpass, p.lowpassHz);
    set (ParamIDs::rbFormantPreserve, p.formant ? 1.0f : 0.0f);
    set (ParamIDs::quality, (float) p.quality);
    set (ParamIDs::maxShift, p.maxShiftSemitones);
    set (ParamIDs::shiftBelow, p.shiftBelowHz > 0.0f ? p.shiftBelowHz : pf::shiftBelowOffHz);
}

//==============================================================================
juce::AudioProcessorEditor* PitchFlattenerAudioProcessor::createEditor()
{
    return new PitchFlattenerAudioProcessorEditor (*this);
}

void PitchFlattenerAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = parameters.copyState();
    state.setProperty ("pluginVersion", PLUGIN_VERSION, nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void PitchFlattenerAudioProcessor::migrateLegacyState (juce::ValueTree& state)
{
    // 1.x sessions have no sourceType. Map what still makes sense and drop the rest.
    auto find = [&state] (const juce::String& id) -> juce::ValueTree
    {
        for (auto child : state)
            if (child.hasType ("PARAM") && child.getProperty ("id").toString() == id)
                return child;
        return {};
    };

    if (find (ParamIDs::sourceType).isValid())
        return;

    auto valueOf = [&find] (const juce::String& id, float fallback)
    {
        auto c = find (id);
        return c.isValid() ? (float) c.getProperty ("value") : fallback;
    };

    const bool wasYin = valueOf ("pitchAlgorithm", 1.0f) < 0.5f;
    const bool wasLatch = valueOf ("basePitchLatch", 1.0f) > 0.5f;

    auto addParam = [&state] (const juce::String& id, float value)
    {
        juce::ValueTree p ("PARAM");
        p.setProperty ("id", id, nullptr);
        p.setProperty ("value", value, nullptr);
        state.appendChild (p, nullptr);
    };

    addParam (ParamIDs::sourceType, (float) (int) pf::SourceType::Custom);
    addParam (ParamIDs::detector, (float) (int) (wasYin ? pf::DetectorType::PYin : pf::DetectorType::SpectralPeak));
    addParam (ParamIDs::targetMode, wasLatch ? 0.0f : 1.0f);
    addParam (ParamIDs::quality, 0.0f);

    // remove parameters that no longer exist
    const juce::StringArray removed { "smoothingTimeMs", "detectionRate", "pitchThreshold", "pitchHoldTime", "pitchJumpThreshold",
                                      "minConfidence", "pitchSmoothing", "basePitchLatch", "resetBasePitch", "flattenSensitivity",
                                      "hardFlattenMode", "lookahead", "pitchAlgorithm", "dioSpeed", "dioFramePeriod",
                                      "dioAllowedRange", "dioChannelsInOctave", "dioBufferTime", "rbPitchMode", "rbTransients",
                                      "rbPhase", "rbWindow" };
    for (int i = state.getNumChildren(); --i >= 0;)
        if (removed.contains (state.getChild (i).getProperty ("id").toString()))
            state.removeChild (i, nullptr);
}

void PitchFlattenerAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));
    if (xml == nullptr || ! xml->hasTagName (parameters.state.getType()))
        return;

    auto tree = juce::ValueTree::fromXml (*xml);
    migrateLegacyState (tree);
    StateMigration::overrideToFixed (tree);

    // Parameters missing from the stored state get their defaults, not this instance's current values.
    for (auto* p : getParameters())
    {
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
        {
            bool found = false;
            for (auto child : tree)
                if (child.hasType ("PARAM") && child.getProperty ("id").toString() == rp->paramID)
                    found = true;
            if (! found)
            {
                juce::ValueTree c ("PARAM");
                c.setProperty ("id", rp->paramID, nullptr);
                c.setProperty ("value", rp->convertFrom0to1 (rp->getDefaultValue()), nullptr);
                tree.appendChild (c, nullptr);
            }
        }
    }

    parameters.replaceState (tree);

    if (juce::MessageManager::existsAndIsCurrentThread())
    {
        cancelPendingUpdate();
        handleAsyncUpdate();   // post the rebuild now so the next block already uses it
    }
    else
    {
        triggerAsyncUpdate();
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PitchFlattenerAudioProcessor();
}
