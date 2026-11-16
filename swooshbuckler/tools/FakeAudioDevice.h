#pragma once

// A pretend audio device for the tests and the UI shots: no hardware, no audio thread. Its devices are named after
// how many outputs they have ("Fake 2", "Fake 6", "Fake 16"); the caller pulls blocks through it with pull(), on
// its own thread, as a real device would call back on its audio thread. Add the type to an AudioDeviceManager
// before initialise() and it is the only type the manager knows.

#include <JuceHeader.h>

namespace FakeAudio
{
    constexpr double c_rate = 48000.0;
    constexpr int c_buffer = 256;

    class Device final : public juce::AudioIODevice
    {
    public:
        Device(const juce::String& name, int outputs) : juce::AudioIODevice(name, "Fake"), m_outputs(outputs) {}
        ~Device() override { close(); }

        juce::StringArray getOutputChannelNames() override
        {
            juce::StringArray names;
            for (int i = 0; i < m_outputs; ++i)
                names.add("Output " + juce::String(i + 1));
            return names;
        }
        juce::StringArray getInputChannelNames() override { return {}; }
        juce::Array<double> getAvailableSampleRates() override { return { c_rate }; }
        juce::Array<int> getAvailableBufferSizes() override { return { c_buffer }; }
        int getDefaultBufferSize() override { return c_buffer; }

        juce::String open(const juce::BigInteger&, const juce::BigInteger& outputs, double, int) override
        {
            m_active = outputs;
            m_active.setRange(m_outputs, juce::jmax(0, m_active.getHighestBit() + 1 - m_outputs), false);
            m_open = true;
            ++opened;
            return {};
        }
        void close() override
        {
            stop();
            m_open = false;
        }
        bool isOpen() override { return m_open; }
        void start(juce::AudioIODeviceCallback* callback) override
        {
            if (callback == nullptr || m_callback != nullptr)
                return;
            callback->audioDeviceAboutToStart(this);
            m_callback = callback;
        }
        void stop() override
        {
            if (auto* callback = std::exchange(m_callback, nullptr))
                callback->audioDeviceStopped();
        }
        bool isPlaying() override { return m_callback != nullptr; }
        juce::String getLastError() override { return {}; }
        int getCurrentBufferSizeSamples() override { return c_buffer; }
        double getCurrentSampleRate() override { return c_rate; }
        int getCurrentBitDepth() override { return 32; }
        juce::BigInteger getActiveOutputChannels() const override { return m_active; }
        juce::BigInteger getActiveInputChannels() const override { return {}; }
        int getOutputLatencyInSamples() override { return 0; }
        int getInputLatencyInSamples() override { return 0; }

        /** One block: what the outputs switched on play, into out (as many channels as are switched on). */
        void pull(juce::AudioBuffer<float>& out)
        {
            const int active = m_active.countNumberOfSetBits();
            out.setSize(juce::jmax(1, active), c_buffer, false, false, true);
            out.clear();
            if (m_callback != nullptr)
                m_callback->audioDeviceIOCallbackWithContext(nullptr, 0, out.getArrayOfWritePointers(), active, c_buffer, {});
        }

        int opened = 0;

    private:
        const int m_outputs;
        juce::BigInteger m_active;
        bool m_open = false;
        juce::AudioIODeviceCallback* m_callback = nullptr;
    };

    class Type final : public juce::AudioIODeviceType
    {
    public:
        Type() : juce::AudioIODeviceType("Fake") {}
        void scanForDevices() override {}
        juce::StringArray getDeviceNames(bool) const override { return { "Fake 2", "Fake 6", "Fake 16" }; }
        int getDefaultDeviceIndex(bool) const override { return 0; }
        int getIndexOfDevice(juce::AudioIODevice* device, bool) const override
        {
            return device != nullptr ? getDeviceNames(false).indexOf(device->getName()) : -1;
        }
        bool hasSeparateInputsAndOutputs() const override { return false; }
        juce::AudioIODevice* createDevice(const juce::String& outputName, const juce::String&) override
        {
            const auto name = outputName.isNotEmpty() ? outputName : juce::String("Fake 2");
            return new Device(name, name.fromLastOccurrenceOf(" ", false, false).getIntValue());
        }
    };

    /** The device the manager has open, as a fake one (nullptr if none). */
    inline Device* current(juce::AudioDeviceManager& devices) { return dynamic_cast<Device*>(devices.getCurrentAudioDevice()); }

    /** Opens the named fake device with the first `outputs` outputs switched on. */
    inline juce::String choose(juce::AudioDeviceManager& devices, const juce::String& name, int outputs)
    {
        auto setup = devices.getAudioDeviceSetup();
        setup.outputDeviceName = name;
        setup.inputDeviceName = {};
        setup.useDefaultOutputChannels = false;
        setup.outputChannels.clear();
        setup.outputChannels.setRange(0, outputs, true);
        setup.sampleRate = c_rate;
        setup.bufferSize = c_buffer;
        return devices.setAudioDeviceSetup(setup, true);
    }
}
