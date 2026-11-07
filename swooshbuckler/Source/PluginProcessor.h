#pragma once

#include <JuceHeader.h>
#include "Cargo.h"
#include "MasterProcessor.h"
#include "Parameters.h"
#include "TapeRecorder.h"
#include "RigEngine.h"
#include "Modulators.h"

/**
 * Swooshbuckler: a whoosh generator / movement processor with a built-in tape
 * machine. Runs as a standalone app, or as an AU/VST3 (MIDI notes trigger
 * whooshes; audio on the input can be used as a source).
 *
 *   engine (sources -> flights -> rigging) -> push -> inflate -> limiter -> out
 *                                                                        \-> Captain's Log (tape)
 */
class SwooshbucklerAudioProcessor : public juce::AudioProcessor, private juce::Timer
{
public:
    SwooshbucklerAudioProcessor();
    ~SwooshbucklerAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 8.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    //==============================================================================
    juce::AudioProcessorValueTreeState& getState() { return m_state; }
    CargoHold& getCargo() { return m_cargo; }
    const RigEngine& getEngine() const { return m_engine; }
    ModMatrix& getMods() { return m_mods; }
    /** The output (after the limiter), for the Output section's little scope. */
    const ScopeRing& getOutputScope() const { return m_outRing; }

    /** Fire a whoosh from the UI (any thread). */
    /** Transport. The rig opens stopped; stopping fades out over 60 ms and then the engine rests. Any thread. */
    void setPlaying(bool on) { if (on && ! m_playing.load()) m_restartPending.store(true); m_playing.store(on); }
    bool isPlaying() const { return m_playing.load(); }
    /** New session: stops, empties every track and puts every parameter back to its default. Message thread. */
    void newSession();

    /** Swell: while held, every track's fader rides up (button, space bar, MIDI notes). Any thread. */
    void setSwell(bool held) { m_uiSwell.store(held); }

    // Captain's Log -----------------------------------------------------------
    bool startRecording(juce::String& error);
    void stopRecording();
    bool isRecording() const { return m_recorder.isRecording(); }
    /** Arm: a take is open and writes whenever there is sound (silence is skipped). */
    bool arm(juce::String& error);
    bool isArmed() const { return m_recorder.isRecording() && m_recorder.getSignalGate(); }
    bool isCapturing() const { return m_recorder.isCapturing(); }
    double getRecordedSeconds() const { return m_recorder.getRecordedSeconds(); }
    juce::File getLastRecording() const { return m_recorder.getLastFile(); }
    juce::File getCurrentRecording() const { return m_recorder.getCurrentFile(); }
    bool saveRetro(juce::File& written, juce::String& error);

    // App settings (shared across sessions, not saved with presets).
    juce::File getRecordFolder() const;
    void setRecordFolder(const juce::File& folder);
    int getBitDepth() const;
    void setBitDepth(int bits);
    /** Recording sample rate in Hz, 0 = the session rate. Above the device rate the rig renders at this rate. */
    int getRecordRate() const;
    void setRecordRate(int rate);
    double getRenderRate() const { return m_renderRate; }
    double getDeviceRate() const { return m_deviceRate; }
    int getFileType() const;          // 0 WAV, 1 AIFF, 2 FLAC
    void setFileType(int type);
    bool getMono() const;
    void setMono(bool mono);
    bool getAutoRecord() const;
    void setAutoRecord(bool on);

    // Metering / visuals -------------------------------------------------------
    float getGainReductionDb() const { return m_master.getGainReductionDb(); }
    float getOutputPeak() const { return m_outputPeak.load(std::memory_order_relaxed); }

    static constexpr int c_scopeSize = 1 << 13;
    /** Copies the most recent count mono output samples (count <= c_scopeSize). */
    void copyScope(float* dest, int count) const;

    /** Set by the standalone app: opens the audio device settings (empty in a plugin host). */
    static std::function<void()> showAudioSettings;

    // Editor state (message thread only: a restore from another thread is applied on the message thread)
    int savedEditorWidth = 0;
    juce::String currentPresetName { "Default" };

    /** Bumped (message thread) each time a restored state has been applied, so an open editor re-reads
        savedEditorWidth / currentPresetName. */
    std::atomic<int> stateGeneration { 0 };


    /** Seconds the rolling take lost because the disk couldn't keep up (0 = none). Any thread. */
    double getRecordingDroppedSeconds() const;

private:
    void timerCallback() override;
    void updateEngineParams();
    void prepareInternal();
    void renderAtRenderRate(const float* inL, const float* inR, float* l, float* r, int n);
    void applyRestoredState(const juce::ValueTree& cargo, int editorWidth, const juce::String& presetName); //
    void applyPendingState();                                                                              //

    // Restore handling. A state set off the message thread is kept here until the message thread has
    // applied it, and getStateInformation returns it meanwhile (a save straight after a load keeps the samples).
    struct PendingState
    {
        bool valid = false;
        int id = 0;
        juce::ValueTree cargo;
        int editorWidth = 0;
        juce::String presetName;
    };
    mutable juce::CriticalSection m_stateLock;
    PendingState m_pending;            // guarded by m_stateLock
    int m_pendingCounter = 0;          // guarded by m_stateLock
    int m_mirrorWidth = 0;             // copies of the editor fields for saves off the message thread (m_stateLock)
    juce::String m_mirrorPreset { "Default" };

    juce::AudioProcessorValueTreeState m_state;
    CargoHold m_cargo;
    RigEngine m_engine;
    std::atomic<bool> m_uiSwell { false };
    std::atomic<bool> m_playing { false }, m_restartPending { false };
    float m_transportGain = 0.0f;
    int m_midiNotesHeld = 0;
    MasterProcessor m_master;
    TapeRecorder m_recorder;
    std::unique_ptr<juce::PropertiesFile> m_settings;

    juce::AudioBuffer<float> m_work;
    double m_deviceRate = 0.0, m_renderRate = 48000.0;
    int m_blockSize = 512;
    juce::AudioBuffer<float> m_fifo;   // high-rate render ahead of the downsampler
    int m_fifoCount = 0;
    std::array<juce::WindowedSincInterpolator, 2> m_down;

    struct Raw
    {
        std::array<std::atomic<float>*, ParamIDs::numCargoTracks> tracks {};
        std::atomic<float>* pitch = nullptr, *input = nullptr;
        std::atomic<float>* rideOn = nullptr, *energy = nullptr, *rate = nullptr, *fill = nullptr, *chaos = nullptr;
        std::atomic<float>* bus[ParamIDs::numBuses][ParamIDs::numBusControls] {};
        std::atomic<float>* hitSense = nullptr, *hitBoost = nullptr, *hitRelease = nullptr, *hitDuck = nullptr, *hitSync = nullptr;
        std::atomic<float>* gateMode = nullptr, *hitHold = nullptr, *hitFloor = nullptr, *duckMode = nullptr, *duckReturn = nullptr,
                          *takeTurns = nullptr;
        std::atomic<float>* space = nullptr, *inflate = nullptr, *push = nullptr, *ceiling = nullptr;
        std::atomic<float>* morphOn = nullptr, *morphMode = nullptr, *morphX = nullptr, *morphY = nullptr;
        std::atomic<float>* modType[ParamIDs::numMods] {}, *modShape[ParamIDs::numMods] {}, *modKnob[ParamIDs::numMods] {};
    } m_raw;

    // Modulation: the wires add offsets to the knobs' values on the way to the engine (parameters stay untouched).
    ModMatrix m_mods;
    ScopeRing m_outRing;
    struct ModTarget
    {
        juce::RangedAudioParameter* param = nullptr;
        std::atomic<float>* raw = nullptr;
    };
    std::vector<ModTarget> m_modTargets;   // aligned with ModMatrix::targets()
    float m_followerLevel = 0.0f;          // output RMS of the last block (the Follower's input)
    float m_pushDb = 0.0f;                 // Push after modulation
    /** A parameter's value with its wires applied (audio thread, no allocation). */
    float modValue(std::atomic<float>* raw);
    void runModulators(int numSamples);
    void publishModViews(); // the knobs' live dots while stopped

    juce::SmoothedValue<float> m_pushGain;
    std::array<float, c_scopeSize> m_scope {};
    std::atomic<int> m_scopeWrite { 0 };
    std::atomic<float> m_outputPeak { 0.0f };
    bool m_autoRecordDone = false;

    JUCE_DECLARE_WEAK_REFERENCEABLE(SwooshbucklerAudioProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SwooshbucklerAudioProcessor)
};
