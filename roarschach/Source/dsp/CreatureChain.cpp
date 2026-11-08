#include "CreatureChain.h"

#include <algorithm>
#include <cmath>

namespace roar
{
    namespace
    {
        constexpr double c_moveFadeSeconds = 0.020;
        constexpr float c_jawAttackSeconds = 0.005f;
        constexpr float c_jawReleaseSeconds = 0.120f;
        constexpr float c_levelSeconds = 0.100f;
        // The jaw opens over this input range (peak envelope, dBFS).
        constexpr float c_jawFloorDb = -48.0f, c_jawFullDb = -6.0f;

        inline float onePole (float seconds, double fs)
        {
            return static_cast<float> (1.0 - std::exp (-1.0 / (seconds * fs)));
        }

        inline float finiteOr (float v, float fallback)
        {
            return std::isfinite (v) ? v : fallback;
        }

        inline float clamp01 (float v)
        {
            return v >= 0.0f ? std::min (v, 1.0f) : 0.0f;  // NaN -> 0
        }

        inline int clampSlot (int s)
        {
            return std::clamp (s, 0, CreatureChain::numSlots - 1);
        }

        inline void store (std::atomic<float>& a, float v)
        {
            a.store (std::isfinite (v) ? v : 0.0f, std::memory_order_relaxed);
        }
    }

    void CreatureChain::prepare (double sampleRate, int maxBlockSize)
    {
        m_sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
        m_maxBlock = std::max (1, maxBlockSize);

        m_clean.prepare (m_sampleRate, m_maxBlock);
        m_cleanDetector.prepare (m_sampleRate);
        m_shifter.prepare (m_sampleRate, m_maxBlock);
        m_growl.prepare (m_sampleRate, m_maxBlock);
        m_liquid.prepare (m_sampleRate, m_maxBlock);
        m_roar.prepare (m_sampleRate, m_maxBlock);
        m_sub.prepare (m_sampleRate, m_maxBlock);
        m_finish.prepare (m_sampleRate, m_maxBlock);
        m_finish.setParams (true);
        m_detailStage.prepare (m_sampleRate, m_maxBlock);
        m_vocoderStage.prepare (m_sampleRate, m_maxBlock);

        // Only these three stages delay the signal; none of them changes its delay with its settings.
        m_latency = m_shifter.getLatencySamples() + m_growl.getLatencySamples() + m_roar.getLatencySamples()
                  + m_clean.getLatencySamples() + m_liquid.getLatencySamples() + m_sub.getLatencySamples()
                  + m_finish.getLatencySamples() + m_detailStage.getLatencySamples() + m_vocoderStage.getLatencySamples();

        m_input.assign (static_cast<size_t> (m_maxBlock), 0.0f);
        m_dryL.assign (static_cast<size_t> (m_maxBlock), 0.0f);
        m_dryR.assign (static_cast<size_t> (m_maxBlock), 0.0f);

        m_fadeStep = static_cast<float> (1.0 / std::max (1.0, c_moveFadeSeconds * m_sampleRate));
        m_jawAttack = onePole (c_jawAttackSeconds, m_sampleRate);
        m_jawRelease = onePole (c_jawReleaseSeconds, m_sampleRate);

        reset();
    }

    void CreatureChain::reset()
    {
        m_clean.reset();
        m_cleanDetector.reset();
        m_shifter.reset();
        m_growl.reset();
        m_liquid.reset();
        m_roar.reset();
        m_sub.reset();
        m_finish.reset();
        m_detailStage.reset();
        m_vocoderStage.reset();

        m_cleanVoice = {};
        m_jawEnv = 0.0f;
        m_inputLevel = 0.0f;
        m_outputLevel = 0.0f;
        m_snap = true;   // the next process() puts the movable modules straight into their slots
    }

    void CreatureChain::process (float* l, float* r, int n, const Settings& settings, MonsterFeatures& features)
    {
        if (l == nullptr || r == nullptr || n <= 0 || m_input.empty())
            return;

        n = std::min (n, m_maxBlock);

        m_settings = settings;
        m_detail.target = clampSlot (settings.detailSlot);
        m_vocoder.target = clampSlot (settings.vocoderSlot);

        if (m_snap)
        {
            m_snap = false;
            m_detail.slot = m_detail.target;
            m_vocoder.slot = m_vocoder.target;
            m_detail.gain = m_vocoder.gain = 1.0f;
        }

        // Mono core: the input summed to mono (r may be a copy of l, which leaves l unchanged).
        float* input = m_input.data();
        for (int i = 0; i < n; ++i)
        {
            const float x = 0.5f * (finiteOr (l[i], 0.0f) + finiteOr (r[i], 0.0f));
            input[i] = x;
            l[i] = x;
        }

        // FINISH learns the loudness of the chain's own input (auto gain).
        m_finish.measureInput (input, n);

        const VoiceInfo none {};

        m_clean.process (l, r, n, false, none);

        // The vocoder's carrier follows the cleaned voice (the shifter's own detector sees the
        // signal after slot 0, which may already be processed).
        m_cleanDetector.process (l, n);
        m_cleanVoice = m_cleanDetector.getVoiceInfo();

        runSlot (0, l, r, n);

        m_shifter.setParams (std::clamp (finiteOr (settings.pitchSt, 0.0f), -12.0f, 12.0f), clamp01 (settings.size));
        m_shifter.process (l, r, n, false, none);
        const VoiceInfo shifted = m_shifter.getOutputVoice();

        runSlot (1, l, r, n);

        m_growl.setParams (clamp01 (settings.growl));
        m_growl.process (l, r, n, false, shifted);

        runSlot (2, l, r, n);

        // LIQUID reads the mono left and writes both channels: stereo from here on.
        m_liquid.setParams (clamp01 (settings.liquid));
        m_liquid.process (l, r, n, true, shifted);

        runSlot (3, l, r, n);

        m_roar.setParams (clamp01 (settings.roar));
        m_roar.process (l, r, n, true, shifted);

        runSlot (4, l, r, n);

        m_sub.setParams (clamp01 (settings.sub));
        m_sub.process (l, r, n, true, shifted);

        runSlot (5, l, r, n);

        m_finish.process (l, r, n, true, shifted);

        finishMoves();
        publish (input, l, r, n, features);
    }

    void CreatureChain::runSlot (int slot, float* l, float* r, int n)
    {
        const bool stereo = slot >= 3;

        if (m_detail.slot == slot)
            runMovable (m_detail, false, l, r, n, stereo);

        if (m_vocoder.slot == slot)
            runMovable (m_vocoder, true, l, r, n, stereo);
    }

    void CreatureChain::runMovable (Movable& m, bool isVocoder, float* l, float* r, int n, bool stereo)
    {
        const VoiceInfo none {};

        if (isVocoder)
        {
            // Once the vocoder sits after PITCH, its carrier must follow the shifted voice.
            const float ratio = m.slot >= 1 ? m_shifter.getPitchRatio() : 1.0f;
            m_vocoderStage.setParams (clamp01 (m_settings.gravel), m_settings.vocoderOctave,
                                      -4.0f * clamp01 (m_settings.size), ratio);
        }
        else
        {
            m_detailStage.setParams (clamp01 (m_settings.detailLow), clamp01 (m_settings.detailHigh));
        }

        const float targetGain = m.slot == m.target ? 1.0f : 0.0f;

        // Settled in place.
        if (m.gain >= 1.0f && targetGain >= 1.0f)
        {
            if (isVocoder)
                m_vocoderStage.process (l, r, n, stereo, m_cleanVoice);
            else
                m_detailStage.process (l, r, n, stereo, none);
            return;
        }

        // Faded out and waiting to move: a bypass.
        if (m.gain <= 0.0f && targetGain <= 0.0f)
            return;

        // Fading: blend the module's output with its own input.
        std::copy (l, l + n, m_dryL.data());
        if (stereo)
            std::copy (r, r + n, m_dryR.data());

        if (isVocoder)
            m_vocoderStage.process (l, r, n, stereo, m_cleanVoice);
        else
            m_detailStage.process (l, r, n, stereo, none);

        float g = m.gain;
        for (int i = 0; i < n; ++i)
        {
            g = targetGain > g ? std::min (targetGain, g + m_fadeStep) : std::max (targetGain, g - m_fadeStep);
            const float w = g * g * (3.0f - 2.0f * g);
            l[i] = m_dryL[static_cast<size_t> (i)] + w * (l[i] - m_dryL[static_cast<size_t> (i)]);
            if (stereo)
                r[i] = m_dryR[static_cast<size_t> (i)] + w * (r[i] - m_dryR[static_cast<size_t> (i)]);
        }
        m.gain = g;
    }

    void CreatureChain::finishMoves()
    {
        // A module that has faded out moves now, between chunks, so it never runs twice in one chunk.
        for (auto* m : { &m_detail, &m_vocoder })
            if (m->slot != m->target && m->gain <= 0.0f)
                m->slot = m->target;
    }

    void CreatureChain::publish (const float* input, const float* l, const float* r, int n, MonsterFeatures& f)
    {
        float inSq = 0.0f, outSq = 0.0f;
        float env = m_jawEnv;

        for (int i = 0; i < n; ++i)
        {
            const float x = std::abs (input[i]);
            env += (x > env ? m_jawAttack : m_jawRelease) * (x - env);
            inSq += input[i] * input[i];
            outSq += 0.5f * (l[i] * l[i] + r[i] * r[i]);
        }

        m_jawEnv = std::isfinite (env) ? env : 0.0f;

        const float levelCoeff = 1.0f - std::exp (-static_cast<float> (n) / (c_levelSeconds * static_cast<float> (m_sampleRate)));
        const float inRms = std::sqrt (inSq / static_cast<float> (n));
        const float outRms = std::sqrt (outSq / static_cast<float> (n));
        m_inputLevel = finiteOr (m_inputLevel + levelCoeff * (inRms - m_inputLevel), 0.0f);
        m_outputLevel = finiteOr (m_outputLevel + levelCoeff * (outRms - m_outputLevel), 0.0f);

        const float jawDb = 20.0f * std::log10 (std::max (m_jawEnv, 1.0e-6f));
        const float jaw = clamp01 ((jawDb - c_jawFloorDb) / (c_jawFullDb - c_jawFloorDb));

        store (f.inputLevel, std::min (m_inputLevel, 1.0f));
        store (f.outputLevel, std::min (m_outputLevel, 1.0f));
        store (f.jaw, jaw);
        store (f.f0Hz, m_cleanVoice.f0Hz);
        store (f.voiced, m_cleanVoice.voiced ? 1.0f : 0.0f);
        store (f.growlMod, std::clamp (m_growl.getModulation(), -1.0f, 1.0f));
        store (f.liquidPhase, clamp01 (m_liquid.getPhase()));
        store (f.roarDrive, clamp01 (m_roar.getDrive()));
        store (f.subLevel, clamp01 (m_sub.getLevel()));
        store (f.detailBoostDb, m_detailStage.getBoostDb() * m_detail.gain);
        store (f.detailCutDb, m_detailStage.getCutDb() * m_detail.gain);

        const bool gravel = m_vocoderStage.isActive();
        store (f.gravelActive, gravel ? 1.0f : 0.0f);

        const auto& bands = m_vocoderStage.getBandLevels();
        for (size_t b = 0; b < f.vocoderBands.size() && b < bands.size(); ++b)
            store (f.vocoderBands[b], gravel ? clamp01 (bands[b]) * m_vocoder.gain : 0.0f);

        f.generation.fetch_add (1, std::memory_order_relaxed);
    }
}
