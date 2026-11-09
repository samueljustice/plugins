#include "VoiceStream.h"

#include "DspMath.h"

#include <algorithm>
#include <cassert>

namespace roar
{
    namespace
    {
        const VoiceFrame c_defaultFrame {};

        int framesFor (int startOffset, int numSamples)
        {
            return numSamples > 0 ? ((startOffset + numSamples - 1) >> c_voiceFrameShift) + 1 : 1;
        }

        std::int64_t floorDiv (std::int64_t a, std::int64_t b)
        {
            const std::int64_t q = a / b;
            return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
        }
    }

    // ---------------------------------------------------------------- VoiceFrameView

    VoiceFrameView::VoiceFrameView() : m_frames (&c_defaultFrame) {}

    VoiceFrameView::VoiceFrameView (const VoiceFrame* frames, int numFrames, int startOffset, std::int64_t firstFrame, int skewSamples)
        : m_frames (frames != nullptr && numFrames > 0 ? frames : &c_defaultFrame),
          m_numFrames (frames != nullptr && numFrames > 0 ? numFrames : 1),
          m_startOffset (startOffset),
          m_firstFrame (firstFrame),
          m_skew (skewSamples)
    {
    }

    // ---------------------------------------------------------------- VoiceStream

    void VoiceStream::prepare (int maxBlock)
    {
        m_frames.assign (static_cast<size_t> (maxFramesFor (std::max (1, maxBlock))), VoiceFrame {});
        reset();
    }

    void VoiceStream::reset()
    {
        std::fill (m_frames.begin(), m_frames.end(), VoiceFrame {});
        m_numFrames = 1;
        m_startOffset = 0;
        m_firstFrame = 0;
        m_clock = 0;
        m_skew = 0;
    }

    void VoiceStream::beginChunk (int numSamples)
    {
        assert (! m_frames.empty());
        const VoiceFrame held = m_frames.empty() ? VoiceFrame {} : m_frames[static_cast<size_t> (m_numFrames - 1)];

        m_firstFrame = m_clock >> c_voiceFrameShift;
        m_startOffset = static_cast<int> (m_clock & (c_voiceFrameSize - 1));
        m_numFrames = std::min (framesFor (m_startOffset, numSamples), static_cast<int> (m_frames.size()));
        m_skew = 0;
        m_clock += std::max (0, numSamples);

        std::fill (m_frames.begin(), m_frames.begin() + m_numFrames, held);
    }

    void VoiceStream::assign (const VoiceFrameView& source)
    {
        m_numFrames = std::min (source.numFrames(), static_cast<int> (m_frames.size()));
        m_startOffset = source.startOffset();
        m_firstFrame = source.firstFrameIndex();
        m_skew = source.skewSamples();
        std::copy (source.data(), source.data() + m_numFrames, m_frames.begin());
    }

    void VoiceStream::fill (const VoiceFrame& f)
    {
        std::fill (m_frames.begin(), m_frames.begin() + m_numFrames, f);
    }

    VoiceFrameView VoiceStream::view() const
    {
        return { m_frames.data(), m_numFrames, m_startOffset, m_firstFrame, m_skew };
    }

    // ---------------------------------------------------------------- VoiceDelay

    void VoiceDelay::prepare (int latencySamples, int maxBlock)
    {
        m_latency = std::max (0, latencySamples);
        m_maxFrameDelay = (m_latency + 2 * c_voiceFrameSize) / c_voiceFrameSize;
        const int maxFrames = VoiceStream::maxFramesFor (std::max (1, maxBlock));
        const int capacity = dsp::nextPowerOfTwo (m_maxFrameDelay + maxFrames + 2);
        m_ring.assign (static_cast<size_t> (capacity), VoiceFrame {});
        m_mask = capacity - 1;
        m_out.assign (static_cast<size_t> (maxFrames), VoiceFrame {});
        reset();
    }

    void VoiceDelay::reset()
    {
        std::fill (m_ring.begin(), m_ring.end(), VoiceFrame {});
        m_any = false;
        m_oldestValid = 0;
        m_newest = -1;
    }

    VoiceFrameView VoiceDelay::process (const VoiceFrameView& in)
    {
        if (m_ring.empty() || m_out.empty())
            return in;

        const int numFrames = std::min (in.numFrames(), static_cast<int> (m_out.size()));
        const std::int64_t first = in.firstFrameIndex();

        // Chunks arrive in order: the first frame is the last one seen (a partly covered frame)
        // or the next. Anything else is a restart of the producer's clock: older frames are stale.
        if (! m_any || first < m_newest || first > m_newest + 1)
        {
            m_any = true;
            m_oldestValid = first;
        }

        for (int k = 0; k < numFrames; ++k)
            m_ring[static_cast<size_t> ((first + k) & m_mask)] = in.frame (k);
        m_newest = first + numFrames - 1;

        // Whole-frame delay that best matches L given the incoming skew.
        const std::int64_t d = floorDiv (static_cast<std::int64_t> (m_latency) - in.skewSamples() + c_voiceFrameSize / 2,
                                         c_voiceFrameSize);
        const int frameDelay = static_cast<int> (std::clamp<std::int64_t> (d, 0, m_maxFrameDelay));
        const int skew = in.skewSamples() + frameDelay * c_voiceFrameSize - m_latency;

        for (int k = 0; k < numFrames; ++k)
        {
            const std::int64_t s = first + k - frameDelay;
            m_out[static_cast<size_t> (k)] = s < m_oldestValid ? VoiceFrame {} : m_ring[static_cast<size_t> (s & m_mask)];
        }

        return { m_out.data(), numFrames, in.startOffset(), first, skew };
    }

    void VoiceDelay::copyStateFrom (const VoiceDelay& other)
    {
        assert (other.m_ring.size() == m_ring.size());
        const size_t n = std::min (m_ring.size(), other.m_ring.size());
        std::copy (other.m_ring.begin(), other.m_ring.begin() + static_cast<std::ptrdiff_t> (n), m_ring.begin());
        m_oldestValid = other.m_oldestValid;
        m_newest = other.m_newest;
        m_any = other.m_any;
    }

    // ---------------------------------------------------------------- VoicePass

    void VoicePass::prepare (int latencySamples, int maxBlock)
    {
        m_delay.prepare (latencySamples, maxBlock);
        m_scratch.prepare (maxBlock);
    }

    void VoicePass::reset()
    {
        m_delay.reset();
        m_scratch.reset();
    }

    VoiceFrameView VoicePass::process (const VoiceFrameView& in, VoiceRewriter* rewriter)
    {
        const VoiceFrameView delayed = m_delay.getLatencySamples() > 0 ? m_delay.process (in) : in;
        if (rewriter == nullptr)
            return delayed;

        m_scratch.assign (delayed);
        rewriter->rewriteVoice (m_scratch.frames(), m_scratch.numFrames());
        return m_scratch.view();
    }

    void VoicePass::copyStateFrom (const VoicePass& other)
    {
        m_delay.copyStateFrom (other.m_delay);
    }
}
