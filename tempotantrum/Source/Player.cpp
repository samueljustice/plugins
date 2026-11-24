#include "Player.h"

#include <algorithm>
#include <cmath>

namespace tt
{

//==============================================================================
// The music and the click, mixed sample by sample at the file's rate.
class Player::MixSource : public juce::PositionableAudioSource
{
public:
    explicit MixSource (State& s) : state (s) {}

    void prepareToPlay (int, double) override {}
    void releaseResources() override {}

    void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override
    {
        info.clearActiveBufferRegion();

        const juce::SpinLock::ScopedLockType sl (state.lock);

        const auto seek = state.pendingSeek.exchange (-1);
        auto pos = seek >= 0 ? seek : state.position.load();

        if (seek >= 0)
        {
            state.runStart.store (seek);
            state.runFromWrap.store (false);
        }

        const auto* music = state.music;

        if (music == nullptr || music->getNumSamples() == 0 || info.buffer->getNumChannels() == 0)
            return;

        const juce::int64 length = music->getNumSamples();
        const auto loopStart = state.loopStart.load();
        const auto loopEnd = state.loopEnd.load();
        const bool looping = loopStart >= 0 && loopEnd > loopStart;
        const float musicGain = state.musicGain.load();
        const float clickGain = state.clickGain.load();
        const float* click = state.clickData;
        const auto clickLength = state.clickLength;

        const float* inL = music->getReadPointer (0);
        const float* inR = music->getNumChannels() > 1 ? music->getReadPointer (1) : inL;
        float* outL = info.buffer->getWritePointer (0, info.startSample);
        float* outR = info.buffer->getNumChannels() > 1 ? info.buffer->getWritePointer (1, info.startSample) : nullptr;

        for (int i = 0; i < info.numSamples; ++i)
        {
            if (looping && pos >= loopEnd)
            {
                pos = loopStart;
                state.runStart.store (loopStart);
                state.runFromWrap.store (true);
            }

            if (pos >= length)
            {
                state.playing.store (false);
                state.reachedEnd.store (true);
                break;
            }

            if (pos >= 0)
            {
                const auto index = (size_t) pos;
                const float c = (click != nullptr && pos < clickLength) ? click[index] * clickGain : 0.0f;

                if (outR != nullptr)
                {
                    outL[i] = inL[index] * musicGain + c;
                    outR[i] = inR[index] * musicGain + c;
                }
                else
                {
                    outL[i] = 0.5f * (inL[index] + inR[index]) * musicGain + c;
                }
            }

            ++pos;
        }

        state.position.store (pos);
    }

    void setNextReadPosition (juce::int64 newPosition) override { state.pendingSeek.store (newPosition); }
    juce::int64 getNextReadPosition() const override             { return state.position.load(); }

    juce::int64 getTotalLength() const override
    {
        const juce::SpinLock::ScopedLockType sl (state.lock);
        return state.music != nullptr ? state.music->getNumSamples() : 0;
    }

    bool isLooping() const override { return state.loopStart.load() >= 0 && state.loopEnd.load() > state.loopStart.load(); }

private:
    State& state;
};

//==============================================================================
// What the output plays: silence when stopped, otherwise the mix, resampled to the output's rate
// when that differs from the file's.
class Player::OutputSource : public juce::AudioSource
{
public:
    OutputSource (State& s, MixSource& m)
        : state (s), mix (m), resampler (&m, false, 2) {}

    void prepareToPlay (int samplesPerBlock, double sampleRate) override
    {
        deviceRate.store (sampleRate);
        updateRatio();
        mix.prepareToPlay (samplesPerBlock, state.fileRate.load());
        resampler.prepareToPlay (samplesPerBlock, sampleRate);
    }

    void releaseResources() override
    {
        resampler.releaseResources();
    }

    void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override
    {
        if (! state.playing.load())
        {
            info.clearActiveBufferRegion();
            return;
        }

        if (ratio.load() == 1.0)
        {
            mix.getNextAudioBlock (info);
            return;
        }

        // A jump in position: drop what the resampler holds from the old position.
        if (state.pendingSeek.load() >= 0)
            resampler.flushBuffers();

        resampler.getNextAudioBlock (info);
    }

    // Safe to call from the message thread while playing.
    void updateRatio()
    {
        const double file = state.fileRate.load();
        const double device = deviceRate.load();
        const double r = (file > 0.0 && device > 0.0) ? file / device : 1.0;
        ratio.store (r);
        resampler.setResamplingRatio (r);
    }

    double getDeviceRate() const { return deviceRate.load(); }

private:
    State& state;
    MixSource& mix;
    juce::ResamplingAudioSource resampler;
    std::atomic<double> deviceRate { 0.0 }, ratio { 1.0 };
};

//==============================================================================
Player::Player()
    : mixSource (std::make_unique<MixSource> (state)),
      outputSource (std::make_unique<OutputSource> (state, *mixSource))
{
}

Player::~Player()
{
    if (deviceOpen)
    {
        deviceManager.removeAudioCallback (&sourcePlayer);
        deviceManager.closeAudioDevice();
    }

    sourcePlayer.setSource (nullptr);
}

juce::String Player::openDevice()
{
    if (deviceOpen)
        return {};

    const auto error = deviceManager.initialiseWithDefaultDevices (0, 2);

    if (error.isNotEmpty())
        return error;

    if (deviceManager.getCurrentAudioDevice() == nullptr)
        return "No audio output is available";

    sourcePlayer.setSource (outputSource.get());
    deviceManager.addAudioCallback (&sourcePlayer);
    deviceOpen = true;
    return {};
}

void Player::setMusic (const juce::AudioBuffer<float>* music, double sampleRate)
{
    stop();

    {
        const juce::SpinLock::ScopedLockType sl (state.lock);
        state.music = music;
        state.fileRate.store (music != nullptr ? sampleRate : 0.0);
        state.position.store (0);
        state.pendingSeek.store (0);
        state.loopStart.store (-1);
        state.loopEnd.store (-1);
    }

    updateRatio();

    // The resampler sizes its buffer for the ratio when it is prepared, so it is prepared again
    // here, while nothing is playing, and not left to grow its buffer on the audio thread.
    if (deviceOpen && music != nullptr)
    {
        sourcePlayer.setSource (nullptr);
        sourcePlayer.setSource (outputSource.get());
    }
}

void Player::setClick (std::shared_ptr<const std::vector<float>> click)
{
    auto old = clickOwner; // released after the swap, here on the message thread

    {
        const juce::SpinLock::ScopedLockType sl (state.lock);
        state.clickData = click != nullptr && ! click->empty() ? click->data() : nullptr;
        state.clickLength = click != nullptr ? (juce::int64) click->size() : 0;
    }

    clickOwner = std::move (click);
}

void Player::play()
{
    if (! deviceOpen || state.fileRate.load() <= 0.0)
        return;

    const juce::SpinLock::ScopedLockType sl (state.lock);

    if (state.music == nullptr)
        return;

    // Played to the end last time: start again from the top.
    const auto pos = state.pendingSeek.load() >= 0 ? state.pendingSeek.load() : state.position.load();
    if (pos >= state.music->getNumSamples() && state.loopEnd.load() < 0)
        state.pendingSeek.store (0);

    if (! state.playing.load())
    {
        state.runStart.store (state.pendingSeek.load() >= 0 ? state.pendingSeek.load() : state.position.load());
        state.runFromWrap.store (false);
    }

    state.reachedEnd.store (false);
    state.playing.store (true);
}

void Player::stop()
{
    state.playing.store (false);
}

juce::int64 Player::toSamples (double seconds) const
{
    return (juce::int64) std::llround (std::max (0.0, seconds) * state.fileRate.load());
}

void Player::setPosition (double seconds)
{
    const auto pos = toSamples (seconds);
    state.pendingSeek.store (pos);

    if (! state.playing.load())
        state.position.store (pos);
}

double Player::getPlayheadSeconds() const
{
    const double rate = state.fileRate.load();

    if (rate <= 0.0)
        return 0.0;

    const auto pending = state.pendingSeek.load();
    double seconds = (double) (pending >= 0 ? pending : state.position.load()) / rate;

    if (! state.playing.load() || pending >= 0)
        return seconds;

    // What is heard is behind what has been read by the output's buffer and latency.
    if (auto* device = const_cast<juce::AudioDeviceManager&> (deviceManager).getCurrentAudioDevice())
    {
        const double deviceRate = device->getCurrentSampleRate();

        if (deviceRate > 0.0)
            seconds -= (double) (device->getOutputLatencyInSamples() + device->getCurrentBufferSizeSamples()) / deviceRate;
    }

    // The audio now being read started at runStart: at a seek, at the start of playback, or at
    // the loop's start after going round. What is heard has not got there yet if it is before
    // that point. After going round it is still near the loop's end; otherwise nothing from this
    // run has come out yet, and the playhead waits at its start.
    const double runStart = (double) state.runStart.load() / rate;

    if (seconds < runStart)
    {
        const auto loopStart = state.loopStart.load();
        const auto loopEnd = state.loopEnd.load();

        if (state.runFromWrap.load() && loopStart >= 0 && loopEnd > loopStart)
            seconds = std::max (runStart, seconds + (double) (loopEnd - loopStart) / rate);
        else
            seconds = runStart;
    }

    return std::max (0.0, seconds);
}

void Player::setLoop (double startSeconds, double endSeconds)
{
    const auto start = toSamples (startSeconds);
    const auto end = toSamples (endSeconds);

    if (end <= start)
        return clearLoop();

    state.loopEnd.store (-1);
    state.loopStart.store (start);
    state.loopEnd.store (end);
}

void Player::clearLoop()
{
    state.loopEnd.store (-1);
    state.loopStart.store (-1);
}

bool Player::isLooping() const
{
    const auto start = state.loopStart.load();
    return start >= 0 && state.loopEnd.load() > start;
}

void Player::updateRatio()
{
    outputSource->updateRatio();
}

} // namespace tt
