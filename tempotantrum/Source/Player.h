#pragma once

#include <JuceHeader.h>

#include <atomic>
#include <memory>
#include <vector>

namespace tt
{

// Plays the music with the click mixed in, on the default output. The music and the click are
// read from the same sample index, so the click sits exactly where the map puts it; the mix is
// then resampled when the output runs at a different rate from the file, so the pitch is right.
//
// All calls are made on the message thread.
class Player
{
public:
    Player();
    ~Player();

    // Opens the default output. Returns an error message, or an empty string.
    juce::String openDevice();
    bool isDeviceOpen() const                           { return deviceOpen; }

    // The buffer is owned by the caller and must stay valid until it is replaced or cleared here.
    // nullptr stops playback and plays nothing.
    void setMusic (const juce::AudioBuffer<float>* music, double sampleRate);

    // The click at the music's sample rate. Can be swapped while playing.
    void setClick (std::shared_ptr<const std::vector<float>> click);

    void setMusicGain (float gain)                      { state.musicGain.store (juce::jlimit (0.0f, 1.0f, gain)); }
    void setClickGain (float gain)                      { state.clickGain.store (juce::jlimit (0.0f, 1.0f, gain)); }

    void play();
    void stop();
    bool isPlaying() const                              { return state.playing.load(); }

    // Moves the read position; playback carries on from there if it is running.
    void setPosition (double seconds);

    // Where the music is being heard, allowing for the output's latency.
    double getPlayheadSeconds() const;

    // Plays [start, end) round and round until clearLoop().
    void setLoop (double startSeconds, double endSeconds);
    void clearLoop();
    bool isLooping() const;

    // True once after playback has run off the end of the music.
    bool takeReachedEnd()                               { return state.reachedEnd.exchange (false); }

private:
    // Shared with the audio thread. The lock guards the music and click pointers; the audio thread
    // holds it while it renders one block and the message thread only while it swaps pointers.
    struct State
    {
        juce::SpinLock lock;
        const juce::AudioBuffer<float>* music = nullptr;
        const float* clickData = nullptr;
        juce::int64 clickLength = 0;

        std::atomic<double> fileRate { 0.0 };
        std::atomic<bool> playing { false }, reachedEnd { false };
        std::atomic<juce::int64> position { 0 }, pendingSeek { -1 };
        std::atomic<juce::int64> loopStart { -1 }, loopEnd { -1 };

        // Where the stretch of audio now being read began, and whether it began by the loop
        // going round. Written by the audio thread (and by play() while stopped).
        std::atomic<juce::int64> runStart { 0 };
        std::atomic<bool> runFromWrap { false };
        std::atomic<float> musicGain { 1.0f }, clickGain { 0.8f };
    };

    class MixSource;
    class OutputSource;

    State state;
    std::unique_ptr<MixSource> mixSource;
    std::unique_ptr<OutputSource> outputSource;
    std::shared_ptr<const std::vector<float>> clickOwner;

    juce::AudioDeviceManager deviceManager;
    juce::AudioSourcePlayer sourcePlayer;
    bool deviceOpen = false;

    void updateRatio();
    juce::int64 toSamples (double seconds) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Player)
};

} // namespace tt
