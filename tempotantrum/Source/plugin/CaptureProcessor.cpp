#include "CaptureProcessor.h"
#include "CaptureEditor.h"

namespace tt::audiosuite
{

CaptureProcessor::CaptureProcessor (juce::File folder, CaptureRecorder::Timing timing)
    : juce::AudioProcessor (BusesProperties()
                                .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      recorder (folder == juce::File() ? capturesFolder() : folder, 1 << 19, timing)
{
}

CaptureProcessor::~CaptureProcessor() = default;

void CaptureProcessor::refreshWhereItRuns()
{
    aaxInsert.store (wrapperType == wrapperType_AAX && ! juce::PluginHostType::isInAAXAudioSuite (*this));
}

void CaptureProcessor::prepareToPlay (double, int)
{
    refreshWhereItRuns();
}

void CaptureProcessor::releaseResources()
{
    recorder.requestEnd();
}

void CaptureProcessor::reset()
{
    recorder.requestEnd();
}

void CaptureProcessor::setNonRealtime (bool nonRealtime) noexcept
{
    juce::AudioProcessor::setNonRealtime (nonRealtime);

    if (nonRealtime)
        refreshWhereItRuns();
    else
        recorder.requestEnd();
}

bool CaptureProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();

    return in == out && (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());
}

void CaptureProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    // The output is the input: the host gives the block in place, so it is left as it is. Any
    // output channel without an input is silent.
    const int ins = getTotalNumInputChannels();

    for (int ch = ins; ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    if (! isNonRealtime() || aaxInsert.load (std::memory_order_relaxed))
    {
        recorder.notRecording();
        return;
    }

    PlayHeadFacts facts;

    if (auto* head = getPlayHead())
    {
        if (const auto position = head->getPosition())
        {
            if (const auto bpm = position->getBpm())
            {
                facts.hasBpm = true;
                facts.bpm = *bpm;
            }

            if (const auto sig = position->getTimeSignature())
            {
                facts.hasTimeSignature = true;
                facts.numerator = sig->numerator;
                facts.denominator = sig->denominator;
            }

            if (const auto ppq = position->getPpqPosition())
            {
                facts.hasPpq = true;
                facts.ppqPosition = *ppq;
            }

            if (const auto samples = position->getTimeInSamples())
            {
                facts.hasTimeInSamples = true;
                facts.timeInSamples = *samples;
            }
        }
    }

    recorder.record (buffer, ins, buffer.getNumSamples(), getSampleRate(), facts);
}

juce::AudioProcessorEditor* CaptureProcessor::createEditor()
{
    return new CaptureEditor (*this);
}

} // namespace tt::audiosuite

// The plug-in's entry point.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new tt::audiosuite::CaptureProcessor();
}
