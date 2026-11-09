#pragma once

#include "DelayLine.h"
#include "DspMath.h"
#include "VoiceStream.h"

#include <algorithm>
#include <cassert>
#include <vector>

namespace roar
{
    struct MonsterFeatures;

    /**
     * The interface every movable module implements (spec §6). The chain owns two instances of
     * every module (twin chains, §3.5) and calls, per chunk of at most maxBlock samples:
     *
     *   module.setParams (settings);                      // concrete type: setParams (const XSettings&)
     *   module.process (l, r, n, stereo, voiceIn);
     *   voiceOut = pass.process (voiceIn, module.getVoiceRewriter());   // VoicePass, one per module
     *   module.publishFeatures (features);               // active chain only
     *
     * Rules:
     *   - prepare() is the only place that allocates. reset() only clears state (no allocation).
     *   - getLatencySamples() is constant after prepare(): it never depends on settings, On or
     *     position. An Off module still delays by it.
     *   - process(): !stereo means the mono part of the chain: use l only, r is untouched and may
     *     be a scratch buffer. stereo means l and r both carry signal, processed linked (§3.3).
     *   - copyStateFrom() copies every piece of state from another instance of the same concrete
     *     type, prepared with the same sample rate and block size: std::copy into the buffers
     *     prepare() allocated, plus plain members. No allocation.
     *   - copyLeftStateToRight() makes every right-channel state an exact copy of the left one
     *     (the module is entering the stereo part, and L == R up to that point).
     */
    class Module
    {
    public:
        virtual ~Module() = default;

        virtual void prepare (double fs, int maxBlock) = 0;   // allocate here only
        virtual void reset() = 0;
        virtual int  getLatencySamples() const = 0;           // constant after prepare
        virtual bool makesStereo() const { return false; }    // from current settings (ENSEMBLE, SHIMMER Stereo)
        virtual void process (float* l, float* r, int n, bool stereo, const VoiceFrameView& voice) = 0;
        virtual void copyStateFrom (const Module& other) = 0; // same concrete type, no allocation
        virtual void copyLeftStateToRight() = 0;

        /** PITCH returns itself here (it rewrites f0 of the stream it passes on); others nullptr. */
        virtual VoiceRewriter* getVoiceRewriter() { return nullptr; }

        /**
         * Write this module's readouts into the shared features once per chunk (audio thread,
         * relaxed atomics). Scale them by the On fade (OnFade::getValue()) so an Off module reads 0.
         */
        virtual void publishFeatures (MonsterFeatures& /*features*/) const {}

    protected:
        /** For copyStateFrom(): the other module as this concrete type. */
        template <typename T>
        static const T& sameType (const Module& other)
        {
            assert (dynamic_cast<const T*> (&other) != nullptr);
            return static_cast<const T&> (other);
        }
    };

    /**
     * The standard On/Off fade every module uses (spec §3.5): the module's output crossfades
     * between its wet and its latency-aligned dry with a smoothstep over 20 ms, and the costly DSP
     * stops once the fade is fully off. The dry is the module's own input delayed by the module's
     * latency, so On and Off line up sample for sample.
     *
     *   prepare:   m_onFade.prepare (fs, maxBlock, getLatencySamples());
     *   setParams: m_onFade.setOn (s.on);
     *   reset:     m_onFade.reset();                     // snaps to the requested state
     *   process:
     *       m_onFade.captureDry (l, r, n, stereo);        // always: keeps the dry delay running
     *       if (! m_onFade.isActive())
     *       {
     *           runLatencyOnly (l, r, n, stereo);         // optional: keep pure-delay state warm
     *           m_onFade.outputDry (l, r, n, stereo);     // output = aligned dry
     *           return;
     *       }
     *       if (m_onFade.consumeRestart())
     *           resetWetState();                          // optional: start the wet from clean state
     *       ... wet DSP in place on l (and r) ...
     *       m_onFade.apply (l, r, n, stereo);             // l = dry + w (wet - dry), w = smoothstep
     *
     * dryLeft() / dryRight() give the aligned dry of the current chunk for internal mixes.
     * Process at most maxBlock samples per call. No allocation after prepare(); copyable.
     */
    class OnFade
    {
    public:
        static constexpr double c_fadeSeconds = 0.020;

        void prepare (double fs, int maxBlock, int latencySamples)
        {
            const double rate = dsp::safeSampleRate (fs);
            m_fadeLength = std::max (1, dsp::samplesFor (c_fadeSeconds, rate));
            m_latency = std::max (0, latencySamples);
            m_dryLineL.prepare (m_latency);
            m_dryLineR.prepare (m_latency);
            m_dryLineL.setFixedDelay (m_latency);
            m_dryLineR.setFixedDelay (m_latency);
            m_dryL.assign (static_cast<size_t> (std::max (1, maxBlock)), 0.0f);
            m_dryR.assign (static_cast<size_t> (std::max (1, maxBlock)), 0.0f);
            reset();
        }

        void setOn (bool on)
        {
            if (on && ! m_on && m_pos == 0)
                m_restart = true;
            m_on = on;
        }

        bool isOn() const { return m_on; }

        /** Snap to the requested state and clear the dry delay. */
        void reset()
        {
            m_pos = m_on ? m_fadeLength : 0;
            m_restart = false;
            m_dryLineL.reset();
            m_dryLineR.reset();
        }

        /** The wet DSP must run this chunk (On, or still fading out). */
        bool isActive() const { return m_on || m_pos > 0; }

        bool isFullyOn() const { return m_on && m_pos >= m_fadeLength; }

        /** True once after the fade starts rising from fully off. */
        bool consumeRestart()
        {
            const bool r = m_restart;
            m_restart = false;
            return r;
        }

        /** Current wet weight, 0..1 (smoothstep of the fade position). */
        float getValue() const
        {
            return dsp::smoothstep01 (static_cast<float> (m_pos) / static_cast<float> (m_fadeLength));
        }

        int getLatencySamples() const { return m_latency; }

        /** Push the module's input into the dry delays and keep the aligned dry for this chunk. */
        void captureDry (const float* l, const float* r, int n, bool stereo)
        {
            assert (n <= static_cast<int> (m_dryL.size()));
            n = std::min (n, static_cast<int> (m_dryL.size()));
            for (int i = 0; i < n; ++i)
                m_dryL[static_cast<size_t> (i)] = m_dryLineL.processFixed (l[i]);
            if (stereo)
                for (int i = 0; i < n; ++i)
                    m_dryR[static_cast<size_t> (i)] = m_dryLineR.processFixed (r[i]);
            m_chunk = n;
        }

        const float* dryLeft() const { return m_dryL.data(); }
        const float* dryRight() const { return m_dryR.data(); }

        /** Output the aligned dry (the module is fully off). Advances the fade (it stays at 0). */
        void outputDry (float* l, float* r, int n, bool stereo)
        {
            n = std::min (n, m_chunk);
            std::copy (m_dryL.begin(), m_dryL.begin() + n, l);
            if (stereo)
                std::copy (m_dryR.begin(), m_dryR.begin() + n, r);
            advance (n);
        }

        /** Blend the wet in l / r against the aligned dry and advance the fade. */
        void apply (float* l, float* r, int n, bool stereo)
        {
            n = std::min (n, m_chunk);
            if (isFullyOn())
                return;

            const float invLength = 1.0f / static_cast<float> (m_fadeLength);
            const int step = m_on ? 1 : -1;
            int pos = m_pos;
            for (int i = 0; i < n; ++i)
            {
                pos = std::clamp (pos + step, 0, m_fadeLength);
                const float w = dsp::smoothstep01 (static_cast<float> (pos) * invLength);
                if (w >= 1.0f)
                    continue;   // fully wet: dry + 1 (wet - dry) is not bit-equal to wet
                const auto s = static_cast<size_t> (i);
                l[i] = m_dryL[s] + w * (l[i] - m_dryL[s]);
                if (stereo)
                    r[i] = m_dryR[s] + w * (r[i] - m_dryR[s]);
            }
            m_pos = pos;
        }

        void copyStateFrom (const OnFade& other)
        {
            m_dryLineL.copyStateFrom (other.m_dryLineL);
            m_dryLineR.copyStateFrom (other.m_dryLineR);
            m_on = other.m_on;
            m_pos = std::min (other.m_pos, m_fadeLength);
            m_restart = other.m_restart;
        }

        void copyLeftStateToRight() { m_dryLineR.copyStateFrom (m_dryLineL); }

    private:
        void advance (int n)
        {
            m_pos = std::clamp (m_pos + (m_on ? n : -n), 0, m_fadeLength);
        }

        DelayLine m_dryLineL, m_dryLineR;
        std::vector<float> m_dryL, m_dryR;
        int m_chunk = 0;
        int m_latency = 0;
        int m_fadeLength = 960;
        int m_pos = 0;
        bool m_on = true;
        bool m_restart = false;
    };
}
