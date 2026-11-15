#pragma once

#include <JuceHeader.h>
#include "MasterProcessor.h"
#include "Room.h"

class SwooshbucklerAudioProcessor;

/**
 * The app's speakers: what stands between the rig and the audio device in the standalone app.
 *
 * JUCE's standalone player matches the processor it plays to the audio device by the device's output count, so
 * a rig played straight from it is Stereo on a stereo device and becomes 5.1 on any device with six outputs,
 * whether the listener wants that or not, and can never be wider than the outputs that happen to be switched on.
 * The player plays this instead. Its own bus is whatever the device has; the rig behind it plays the room the
 * Listening setting chooses, always at that width, so the tape and Render Variations get the whole room whatever
 * the device is:
 *
 *   rig (Listening's width, its own output stage) --+--> the device's outputs, speaker for speaker      (Direct)
 *                                                   +--> fold to Stereo --> limiter at the Ceiling --> L R (Stereo)
 *                                                   +--> fold to Mono   --> limiter at the Ceiling --> 1   (Mono)
 *
 * Direct when the device has as many outputs switched on as the room has speakers: the i-th output switched on
 * plays the i-th speaker in the order a room is wired (room::deviceChannels: the ear-level speakers and the LFE,
 * then the tops, so 7.1.4 is L R C LFE Lss Rss Lsr Rsr Ltf Rtf Ltr Rtr); outputs past the room are silent, and a
 * Mono room plays its one channel on the first two. With fewer outputs the room is folded down (room::fold) and
 * held under the rig's Ceiling, because a fold adds speakers together (a 7.1.4 fold can reach +11.5 dB).
 *
 * A change of width fades the sound out over 10 ms (fadeOut, isFadedOut); the app then prepares this again, which
 * prepares the rig at the new width, and the sound fades in over 10 ms.
 *
 * Real-time safe: every buffer is allocated in prepareToPlay, for twelve channels.
 */
class AppOutput final : public juce::AudioProcessor
{
public:
    enum class Monitor { Direct, Stereo, Mono };
    /** How a room is played on a device with this many outputs switched on. */
    static Monitor monitorFor(room::Width width, int deviceOutputs);
    static constexpr double c_fadeSeconds = 0.01;

    explicit AppOutput(SwooshbucklerAudioProcessor& rig);

    /** The sound fades to silence over 10 ms and stays silent until prepared again. Any thread. */
    void fadeOut() { m_fadeOut.store(true, std::memory_order_relaxed); }
    /** It has: nothing is heard now. Any thread. */
    bool isFadedOut() const { return m_fadedOut.load(std::memory_order_relaxed); }
    /** How many times it has been prepared (the app checks a device restart did prepare it). */
    int timesPrepared() const { return m_prepared.load(std::memory_order_relaxed); }
    /** As prepared: the room played and how. */
    room::Width getRoom() const { return m_room; }
    Monitor getMonitor() const { return m_monitor; }
    /** The fold's limiter: its deepest reduction in the last block (dB, <= 0; 0 when nothing is folded). */
    float getFoldGainReductionDb() const { return m_monitor == Monitor::Direct ? 0.0f : m_limiter.getGainReductionDb(); }
    /** The whole room the rig played in the last block, before it went to the device (its first getRoom() width's
        channels, the last block's samples). For tests; audio thread or after it. */
    const juce::AudioBuffer<float>& lastRoom() const { return m_roomBuffer; }

    // AudioProcessor --------------------------------------------------------------
    const juce::String getName() const override { return "Swooshbuckler Speakers"; }
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    using juce::AudioProcessor::processBlock;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

private:
    void play(juce::AudioBuffer<float>& out, int start, int numSamples, juce::MidiBuffer& midi);

    SwooshbucklerAudioProcessor& m_rig;
    std::atomic<float>* m_ceiling = nullptr;
    room::Width m_room = room::Width::Stereo;
    Monitor m_monitor = Monitor::Direct;
    int m_roomChannels = 2;
    std::array<int, room::c_maxChannels> m_order {}; // the processing channel each output plays (room::deviceChannels)
    const room::FoldMatrix* m_fold = nullptr;
    juce::AudioBuffer<float> m_roomBuffer, m_foldBuffer;
    juce::MidiBuffer m_chunkMidi;
    MasterProcessor m_limiter;
    float m_gain = 0.0f, m_step = 0.0f;
    int m_holdIn = 0; // samples still to wait after a prepare before fading in (the rig's look-ahead)
    std::atomic<bool> m_fadeOut { false }, m_fadedOut { false };
    std::atomic<int> m_prepared { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AppOutput)
};

/**
 * The app's audio: plays the rig through AppOutput with JUCE's standalone player and device manager, opens the
 * outputs the Listening room needs, and changes room without a click. Message thread.
 *
 * When Listening changes: the sound fades out (AppOutput::fadeOut), then the outputs the new room needs are
 * switched on if the device has them and fewer are on (outputsFor), and AppOutput (so the rig) is prepared again
 * at the new width. A device chosen in the dialog gets the room's outputs switched on too; outputs the listener
 * switches off by hand stay off (the room is then folded down, and the status line says so).
 */
class AppAudio final : private juce::Timer, private juce::ChangeListener
{
public:
    AppAudio(SwooshbucklerAudioProcessor& rig, juce::AudioDeviceManager& devices, juce::AudioProcessorPlayer& player);
    ~AppAudio() override;

    /** Plays the rig through AppOutput, with the room's outputs switched on. */
    void start();
    /** The audio device dialog: the device, its rate and buffer, and which outputs are used. */
    void showDialog();
    AppOutput& output() { return m_output; }
    /** After a change of Listening: once the sound has faded out (or the device has stopped), opens the room's
        outputs and prepares the rig at the new width. True when done. The timer calls it; so can a test. */
    bool finishChange();
    /** The device changed (another one chosen, outputs switched on or off): a new device gets the room's outputs,
        and the rig is told what the device has. The device manager's change message calls it. */
    void deviceChanged();

    /** The outputs to switch on for a room of `needed` channels, on a device with `available` outputs of which
        `active` are on: `active` itself when it has enough or the device has no more; otherwise `active` with the
        outputs from its lowest one up (or from the first) added until the room fits. */
    static juce::BigInteger outputsFor(const juce::BigInteger& active, int available, int needed);

    /** What the dialog says above the device's settings: the room and how many outputs it needs; with enough
        outputs switched on (`active`, of the device's `available`), each speaker beside the number of the output it
        plays on; with too few, the order the speakers go out in, and on a second line the fold and what to do. */
    static juce::String dialogText(room::Width width, const juce::BigInteger& active, int available);

    /** The dialog's content (public so the UI shots can draw it). */
    class DevicePanel;

private:
    void listeningChanged();
    void apply();
    void openRoomOutputs();
    void publish();
    void timerCallback() override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;

    SwooshbucklerAudioProcessor& m_rig;
    juce::AudioDeviceManager& m_devices;
    juce::AudioProcessorPlayer& m_player;
    AppOutput m_output;
    double m_fadeStarted = 0.0;
    juce::String m_deviceName;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AppAudio)
};

class AppAudio::DevicePanel final : public juce::Component, private juce::ChangeListener, private juce::Timer
{
public:
    DevicePanel(juce::AudioDeviceManager& devices, SwooshbucklerAudioProcessor& rig);
    ~DevicePanel() override;
    void paint(juce::Graphics&) override;
    void resized() override;
    void childBoundsChanged(juce::Component* child) override;
    static constexpr int c_width = 560;

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override { refresh(); }
    void timerCallback() override { refresh(); }
    void refresh();
    /** JUCE's captions in the app's own words and capitals. */
    void relabel();
    void fit();
    int textHeight() const;
    static constexpr int c_margin = 18, c_title = 44, c_orderHeight = 34, c_warningHeight = 34, c_gap = 8, c_foot = 24;
    juce::AudioDeviceManager& m_devices;
    SwooshbucklerAudioProcessor& m_rig;
    juce::AudioDeviceSelectorComponent m_selector;
    juce::String m_text;
    juce::Rectangle<int> m_textArea;
};
