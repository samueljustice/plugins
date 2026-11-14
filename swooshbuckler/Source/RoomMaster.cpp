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

    // A room (Quad and wider): the speakers of each side are shaped and limited as the one Stereo channel they
    // stand in for (MasterProcessor::setSides). Stereo, and Mono carried as Stereo, stay a channel each.
    m_sided = layout.numChannels > 2;
    if (m_sided)
    {
        std::array<float, room::c_maxChannels> sides {};
        int mains = 0;
        for (int ch = 0; ch < layout.numChannels; ++ch)
            if (ch != m_lfe)
                sides[static_cast<size_t>(mains++)] = room::info(layout.speakers[static_cast<size_t>(ch)]).x;
        m_mains.setSides(sides.data(), mains);
    }
}

void RoomMaster::reset()
{
    m_mains.reset();
    m_lfeStage.reset();
}

void RoomMaster::setParameters(const MasterProcessor::Parameters& p)
{
    m_mains.setParameters(p);
    // The LFE: no Inflate (its harmonics would put back above the LFE's low-pass what the low-pass took out).
    // Push stays, so the LFE keeps its balance with the mains as they are driven; its own limiter holds the Ceiling.
    auto lfe = p;
    lfe.inflateEffect = 0.0f;
    lfe.inflateCurve = 0.0f;
    m_lfeStage.setParameters(lfe);
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
