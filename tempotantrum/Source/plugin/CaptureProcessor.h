#pragma once

#include "CaptureRecorder.h"

#include <atomic>

// Tempo Tantrum Capture: an AudioSuite plug-in for Pro Tools that sends the selected audio to
// Tempo Tantrum. The audio passes through unchanged, sample for sample, with no latency. During an
// AudioSuite Render (offline, isNonRealtime()) the stream is recorded to a WAV in Tempo Tantrum's
// captures folder; when the Render ends, a JSON description is written beside it and Tempo Tantrum
// is told. Preview records nothing. There are no parameters and no settings.
namespace tt::audiosuite
{

class CaptureProcessor : public juce::AudioProcessor
{
public:
    // Captures go in `folder` (the captures folder when none is given). `timing` is for tests.
    explicit CaptureProcessor (juce::File folder = {}, CaptureRecorder::Timing timing = {});
    ~CaptureProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void reset() override;
    void setNonRealtime (bool isNonRealtime) noexcept override;

    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                             { return true; }

    const juce::String getName() const override                 { return JucePlugin_Name; }
    bool acceptsMidi() const override                           { return false; }
    bool producesMidi() const override                          { return false; }
    double getTailLengthSeconds() const override                { return 0.0; }

    int getNumPrograms() override                               { return 1; }
    int getCurrentProgram() override                            { return 0; }
    void setCurrentProgram (int) override                       {}
    const juce::String getProgramName (int) override            { return {}; }
    void changeProgramName (int, const juce::String&) override  {}

    void getStateInformation (juce::MemoryBlock&) override      {}
    void setStateInformation (const void*, int) override        {}

    CaptureRecorder& getRecorder()                              { return recorder; }

private:
    CaptureRecorder recorder;

    // An AAX instance on a track insert also runs offline during a Bounce; only AudioSuite records.
    std::atomic<bool> aaxInsert { false };
    void refreshWhereItRuns();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CaptureProcessor)
};

} // namespace tt::audiosuite
