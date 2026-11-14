#include "RoomMaster.h"

#include <algorithm>

void RoomMaster::prepare(double sampleRate, room::Width width)
{
    m_width = pipeWidth(width);
    const auto& layout = room::layout(m_width);
    m_numChannels = layout.numChannels;
    m_lfe = layout.lfe;
    m_mains.prepare(sampleRate, m_numChannels - (m_lfe >= 0 ? 1 : 0));
    m_lfeStage.prepare(sampleRate, 1); // prepared at every width, so the latency never depends on it
    m_mainPtrs.fill(nullptr);
}

void RoomMaster::reset()
{
    m_mains.reset();
    m_lfeStage.reset();
}

void RoomMaster::setParameters(const MasterProcessor::Parameters& p)
{
    m_mains.setParameters(p);
    m_lfeStage.setParameters(p);
}

void RoomMaster::process(float* const* channels, int numChannels, int numSamples)
{
    numChannels = std::min(numChannels, m_numChannels);
    int mains = 0;
    float* lfe = nullptr;
    for (int ch = 0; ch < numChannels; ++ch)
    {
        if (ch == m_lfe)
            lfe = channels[ch];
        else
            m_mainPtrs[static_cast<size_t>(mains++)] = channels[ch];
    }
    if (mains > 0)
        m_mains.process(m_mainPtrs.data(), mains, numSamples);
    if (lfe != nullptr)
        m_lfeStage.process(&lfe, 1, numSamples);
}
