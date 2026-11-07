#pragma once

#include <JuceHeader.h>
#include "ParamIDs.h"
#include "dsp/FlattenerCore.h"
#include "dsp/SourcePresets.h"

class PitchFlattenerAudioProcessor : public juce::AudioProcessor,
                                     private juce::AsyncUpdater,
                                     private juce::Timer
{
public:
    PitchFlattenerAudioProcessor();
    ~PitchFlattenerAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void reset() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;
    using AudioProcessor::processBlockBypassed;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override;

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==========================================================================
    // Editor-facing API (message thread)

    juce::AudioProcessorValueTreeState parameters;

    /** Writes the defaults of a Source Type into the user-facing parameters (as a host gesture). */
    void applySourceTypeDefaults (pf::SourceType type);

    /** Forget the latched pitch so the next stable pitch is latched again. */
    void relatch() { core.requestRelatch(); }

    /** Pops per-frame analysis telemetry (single consumer: the editor). */
    bool popTelemetry (pf::TelemetryFrame& f) { return core.popTelemetry (f); }

    /** Reads mono analysis-rate audio for the spectrogram (single consumer). */
    int readAnalysisAudio (float* dest, int maxSamples) { return core.readAnalysisAudio (dest, maxSamples); }
    double getAnalysisRate() const { return core.getAnalysisRate(); }

    float getLatchedHz() const          { return core.getLatchedHz(); }
    float getInputLevelDb() const       { return core.getInputLevelDb(); }
    int   getReportedLatency() const    { return getLatencySamples(); }
    int   getUnderrunCount() const      { return core.getUnderrunCount(); }
    pf::DetectorType getActiveDetector() const { return core.getActiveDetector(); }

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

private:
    void handleAsyncUpdate() override;
    void timerCallback() override;

    pf::LiveSettings readLiveSettings() const;
    pf::DetectorConfig wantedDetectorConfig() const;
    void migrateLegacyState (juce::ValueTree& state);

    pf::FlattenerCore core;
    double currentSampleRate = 0.0;

    struct Raw
    {
        std::atomic<float>* sourceType = nullptr;
        std::atomic<float>* detector = nullptr;
        std::atomic<float>* minFreq = nullptr;
        std::atomic<float>* maxFreq = nullptr;
        std::atomic<float>* sensitivity = nullptr;
        std::atomic<float>* trackingSpeed = nullptr;
        std::atomic<float>* trackSmoothing = nullptr;
        std::atomic<float>* gapFill = nullptr;
        std::atomic<float>* volumeThreshold = nullptr;
        std::atomic<float>* detectionHighpass = nullptr;
        std::atomic<float>* detectionLowpass = nullptr;
        std::atomic<float>* targetMode = nullptr;
        std::atomic<float>* targetPitch = nullptr;
        std::atomic<float>* autoWindow = nullptr;
        std::atomic<float>* flattenAmount = nullptr;
        std::atomic<float>* maxShift = nullptr;
        std::atomic<float>* manualOverride = nullptr;
        std::atomic<float>* overrideFreq = nullptr;
        std::atomic<float>* rbFormantPreserve = nullptr;
        std::atomic<float>* quality = nullptr;
        std::atomic<float>* mix = nullptr;
    } raw;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchFlattenerAudioProcessor)
};
