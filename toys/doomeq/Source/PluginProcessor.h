#pragma once

#include <JuceHeader.h>
#include "EqDsp.h"

class DoomEQProcessor : public juce::AudioProcessor
{
public:
    static constexpr int numBands = 6;
    static constexpr int maxChannels = 8;

    DoomEQProcessor();
    ~DoomEQProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    static juce::String pid (int band, const char* name) { return "b" + juce::String (band + 1) + "_" + name; }

    /** Reads post-EQ mono samples for the analyser (message thread). */
    int pullAnalyserSamples (float* dest, int maxSamples);

    double getCurrentSampleRate() const { return currentSampleRate.load(); }

    /** Output peak (linear), decaying; read by the editor for the HUD face. */
    float getOutputPeak() const { return outputPeak.load(); }
    bool ownsDoomAudio() const;

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    struct Band
    {
        std::atomic<float>* on = nullptr;
        std::atomic<float>* type = nullptr;
        std::atomic<float>* freq = nullptr;
        std::atomic<float>* gain = nullptr;
        std::atomic<float>* q = nullptr;

        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> f, qs;
        juce::SmoothedValue<float> g, mix;

        int currentType = 0;
        bool dirty = true;
        bool wasSilent = false;
        eq::BandCoeffs coeffs;
        std::array<std::array<eq::StageState, eq::maxStages>, maxChannels> state {};

        void resetState() { for (auto& ch : state) ch.fill ({}); }
    };

    std::array<Band, numBands> bands;
    std::atomic<float>* outputParam = nullptr;
    std::atomic<float>* doomSoundParam = nullptr;
    std::atomic<float>* doomLevelParam = nullptr;
    juce::SmoothedValue<float> outputGain, doomGain;
    juce::AudioBuffer<float> doomBuffer;
    std::atomic<float> outputPeak { 0.0f };

    std::atomic<double> currentSampleRate { 44100.0 };

    juce::AbstractFifo analyserFifo { 1 << 15 };
    std::vector<float> analyserBuffer = std::vector<float> (1 << 15);
    std::vector<float> monoScratch;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DoomEQProcessor)
};
