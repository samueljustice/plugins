#include "CreatureChain.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <type_traits>

namespace roar
{
    namespace
    {
        constexpr int c_numModules = Params::numModules;
        constexpr double c_outputLevelReleaseSeconds = 0.300;

        int index (Params::ModuleId id) { return static_cast<int> (id); }

        // 64-bit FNV-1a over the bytes of each field (never a whole struct: no padding bytes).
        struct KeyHash
        {
            std::uint64_t h = 1469598103934665603ull;

            void bytes (const void* p, size_t n)
            {
                const auto* b = static_cast<const unsigned char*> (p);
                for (size_t i = 0; i < n; ++i)
                {
                    h ^= b[i];
                    h *= 1099511628211ull;
                }
            }
            KeyHash& operator<< (bool v) { const unsigned char c = v ? 1 : 0; bytes (&c, 1); return *this; }
            KeyHash& operator<< (float v) { std::uint32_t u; std::memcpy (&u, &v, sizeof u); bytes (&u, sizeof u); return *this; }
            KeyHash& operator<< (int v) { const auto u = static_cast<std::int32_t> (v); bytes (&u, sizeof u); return *this; }
            template <typename E, typename = std::enable_if_t<std::is_enum_v<E>>>
            KeyHash& operator<< (E v) { return *this << static_cast<int> (v); }
        };
    }

    bool ChainSettings::isOn (int module) const
    {
        switch (static_cast<Params::ModuleId> (module))
        {
            case Params::ModuleId::pitch:    return pitch.on;
            case Params::ModuleId::clean:    return clean.on;
            case Params::ModuleId::deess:    return deess.on;
            case Params::ModuleId::detail:   return detail.on;
            case Params::ModuleId::vocoder:  return vocoder.on;
            case Params::ModuleId::ring:     return ring.on;
            case Params::ModuleId::shimmer:  return shimmer.on;
            case Params::ModuleId::growl:    return growl.on;
            case Params::ModuleId::swirl:    return swirl.on;
            case Params::ModuleId::roar:     return roar.on;
            case Params::ModuleId::ensemble: return ensemble.on;
            case Params::ModuleId::tame:     return tame.on;
            case Params::ModuleId::sub:      return sub.on;
            case Params::ModuleId::overtone: return overtone.on;
            case Params::ModuleId::chamber:  return chamber.on;
        }
        return false;
    }

    // =============================================================================================
    // ModuleSet

    CreatureChain::ModuleSet::ModuleSet()
    {
        m_modules[static_cast<size_t> (index (Params::ModuleId::pitch))] = &m_pitch;
        m_modules[static_cast<size_t> (index (Params::ModuleId::clean))] = &m_clean;
        m_modules[static_cast<size_t> (index (Params::ModuleId::deess))] = &m_deess;
        m_modules[static_cast<size_t> (index (Params::ModuleId::detail))] = &m_detail;
        m_modules[static_cast<size_t> (index (Params::ModuleId::vocoder))] = &m_vocoder;
        m_modules[static_cast<size_t> (index (Params::ModuleId::ring))] = &m_ring;
        m_modules[static_cast<size_t> (index (Params::ModuleId::shimmer))] = &m_shimmer;
        m_modules[static_cast<size_t> (index (Params::ModuleId::growl))] = &m_growl;
        m_modules[static_cast<size_t> (index (Params::ModuleId::swirl))] = &m_swirl;
        m_modules[static_cast<size_t> (index (Params::ModuleId::roar))] = &m_roar;
        m_modules[static_cast<size_t> (index (Params::ModuleId::ensemble))] = &m_ensemble;
        m_modules[static_cast<size_t> (index (Params::ModuleId::tame))] = &m_tame;
        m_modules[static_cast<size_t> (index (Params::ModuleId::sub))] = &m_sub;
        m_modules[static_cast<size_t> (index (Params::ModuleId::overtone))] = &m_overtone;
        m_modules[static_cast<size_t> (index (Params::ModuleId::chamber))] = &m_chamber;

        for (int k = 0; k < c_numModules; ++k)
            m_topology.order[static_cast<size_t> (k)] = k;
    }

    void CreatureChain::ModuleSet::prepare (double fs, int maxBlock)
    {
        for (auto* taps : { &m_tapL, &m_tapR })
            for (auto& t : *taps)
                t.assign (static_cast<size_t> (std::max (1, maxBlock)), 0.0f);
        m_tapStereo.fill (false);
        for (int m = 0; m < c_numModules; ++m)
        {
            module (m).prepare (fs, maxBlock);
            m_passes[static_cast<size_t> (m)].prepare (module (m).getLatencySamples(), maxBlock);
        }
    }

    void CreatureChain::ModuleSet::reset()
    {
        m_blendPos = m_blendLength;
        for (int m = 0; m < c_numModules; ++m)
        {
            module (m).reset();
            m_passes[static_cast<size_t> (m)].reset();
        }
    }

    int CreatureChain::ModuleSet::getLatencySamples() const
    {
        int total = 0;
        for (int m = 0; m < c_numModules; ++m)
            total += module (m).getLatencySamples();
        return total;
    }

    void CreatureChain::ModuleSet::setParams (const ChainSettings& s)
    {
        m_pitch.setParams (s.pitch);
        m_clean.setParams (s.clean);
        m_deess.setParams (s.deess);

        m_detail.setParams (s.detail);
        m_vocoderSettings = s.vocoder;
        m_pitchSizeShift = s.pitch.on ? -4.0f * std::clamp (s.pitch.size, 0.0f, 1.0f) : 0.0f;
        applyOrderDependent();
        m_ring.setParams (s.ring);
        m_shimmer.setParams (s.shimmer);
        m_growl.setParams (s.growl);
        m_swirl.setParams (s.swirl);
        m_roar.setParams (s.roar);
        m_ensemble.setParams (s.ensemble);
        m_tame.setParams (s.tame);
        m_sub.setParams (s.sub);
        m_overtone.setParams (s.overtone);
        m_chamber.setParams (s.chamber);
    }

    int CreatureChain::ModuleSet::slotOf (int moduleIndex) const
    {
        for (int k = 0; k < c_numModules; ++k)
            if (m_topology.order[static_cast<size_t> (k)] == moduleIndex)
                return k;
        return c_numModules;
    }

    void CreatureChain::ModuleSet::applyOrderDependent()
    {
        VocoderSettings v = m_vocoderSettings;
        const bool pitchFirst = slotOf (index (Params::ModuleId::pitch)) < slotOf (index (Params::ModuleId::vocoder));
        v.formantShiftBands = pitchFirst ? m_pitchSizeShift : 0.0f;
        m_vocoder.setParams (v);
    }

    void CreatureChain::ModuleSet::setTopology (const Topology& t)
    {
        m_topology = t;
        applyOrderDependent();
    }

    void CreatureChain::ModuleSet::enterTopology (const Topology& next)
    {
        for (int m = 0; m < c_numModules; ++m)
        {
            const bool wasStereo = slotOf (m) >= m_topology.split;
            int nextSlot = c_numModules;
            for (int k = 0; k < c_numModules; ++k)
                if (next.order[static_cast<size_t> (k)] == m)
                    nextSlot = k;
            if (! wasStereo && nextSlot >= next.split)
                module (m).copyLeftStateToRight();
        }
        setTopology (next);
    }

    int CreatureChain::ModuleSet::beginInputBlend (const Topology& from, const Topology& to, int length)
    {
        // Every module from the first slot whose upstream differs (another module before it, or
        // a different stereo state) gets its input blended; the ones before it see exactly what
        // they saw in the outgoing chain.
        int first = c_numModules;
        for (int k = 0; k < c_numModules; ++k)
            if (from.order[static_cast<size_t> (k)] != to.order[static_cast<size_t> (k)])
            {
                first = k;
                break;
            }
        if (from.split != to.split)
            first = std::min (first, std::min (from.split, to.split) + 1);
        m_blendFromSlot = first;
        m_blendLength = std::max (1, length);
        m_blendPos = first < c_numModules ? 0 : m_blendLength;
        if (first >= c_numModules)
            return 0;

        // Until the blended stretch has passed every module from the first blended one to the end,
        // the incoming chain's output still carries the outgoing order's timing (a module whose
        // upstream latency changed is fed the outgoing chain's tap). The crossfade waits for it,
        // or a sine would cancel against its own time-shifted copy.
        int downstream = 0;
        for (int k = first; k < c_numModules; ++k)
            downstream += module (to.order[static_cast<size_t> (k)]).getLatencySamples();
        return downstream + m_blendLength;
    }

    void CreatureChain::ModuleSet::process (float* l, float* r, int n, const VoiceFrameView& voice, bool recordTaps,
                                            const ModuleSet* blendSource)
    {
        const bool blending = blendSource != nullptr && m_blendPos < m_blendLength;
        const float invBlend = 1.0f / static_cast<float> (m_blendLength);

        VoiceFrameView v = voice;
        for (int k = 0; k < c_numModules; ++k)
        {
            if (k == m_topology.split)
                std::copy (l, l + n, r);   // R starts as L at the split (L == R up to here)

            const int m = m_topology.order[static_cast<size_t> (k)];
            const bool stereo = k >= m_topology.split;

            if (blending && k >= m_blendFromSlot)
            {
                const auto mi = static_cast<size_t> (m);
                const float* srcL = blendSource->m_tapL[mi].data();
                const float* srcR = blendSource->m_tapStereo[mi] ? blendSource->m_tapR[mi].data() : srcL;
                for (int i = 0; i < n; ++i)
                {
                    const float w = dsp::smoothstep01 (std::min (1.0f, static_cast<float> (m_blendPos + i + 1) * invBlend));
                    l[i] = srcL[i] + w * (l[i] - srcL[i]);
                    if (stereo)
                        r[i] = srcR[i] + w * (r[i] - srcR[i]);
                }
            }

            if (recordTaps)
            {
                const auto mi = static_cast<size_t> (m);
                std::copy (l, l + n, m_tapL[mi].begin());
                if (stereo)
                    std::copy (r, r + n, m_tapR[mi].begin());
                m_tapStereo[mi] = stereo;
            }

            // DETAIL follows CLEAN's gate when CLEAN runs before it (it never lifts gated room tone).
            if (m == index (Params::ModuleId::detail))
                m_detail.setGateTrace (slotOf (index (Params::ModuleId::clean)) < k ? m_clean.getGateTrace() : nullptr);

            Module& mod = module (m);
            mod.process (l, r, n, stereo, v);
            v = m_passes[static_cast<size_t> (m)].process (v, mod.getVoiceRewriter());
        }

        if (m_topology.split >= c_numModules)
            std::copy (l, l + n, r);       // no module made stereo: R = L before OUTPUT

        // The post-chain hook, on both channels (CHAMBER's Boom; a bit-exact pass while inactive).
        for (int m = 0; m < c_numModules; ++m)
            module (m).processReturn (l, r, n);

        if (blending)
            m_blendPos = std::min (m_blendLength, m_blendPos + n);
    }

    void CreatureChain::ModuleSet::copyStateFrom (const ModuleSet& other)
    {
        for (int m = 0; m < c_numModules; ++m)
        {
            module (m).copyStateFrom (other.module (m));
            m_passes[static_cast<size_t> (m)].copyStateFrom (other.m_passes[static_cast<size_t> (m)]);
        }
        m_topology = other.m_topology;
        m_vocoderSettings = other.m_vocoderSettings;
        m_pitchSizeShift = other.m_pitchSizeShift;
    }

    void CreatureChain::ModuleSet::publishFeatures (const ChainSettings& s, MonsterFeatures& f) const
    {
        for (int m = 0; m < c_numModules; ++m)
            module (m).publishFeatures (f);

        // The modules without a publishFeatures() of their own.
        f.pitchSt.store (s.pitch.on ? m_pitch.getAppliedSemitones() : 0.0f, std::memory_order_relaxed);

        f.overtoneLevel.store (m_overtone.getLevelNormalised(), std::memory_order_relaxed);
        for (int k = 0; k < MonsterFeatures::numOvertoneHarmonics; ++k)
            f.overtoneHarm[static_cast<size_t> (k)].store (m_overtone.getHarmonicDrive (k), std::memory_order_relaxed);
        f.overtoneReplace.store (s.overtone.on && m_overtone.isReplace() ? 1.0f : 0.0f, std::memory_order_relaxed);
        for (int b = 0; b < MonsterFeatures::numOvertoneBands; ++b)
        {
            f.overtoneBandHz[static_cast<size_t> (b)].store (m_overtone.getBandHz (b), std::memory_order_relaxed);
            f.overtoneBandLevel[static_cast<size_t> (b)].store (m_overtone.getBandLevelNormalised (b), std::memory_order_relaxed);
        }

        f.ringAmount.store (m_ring.getAmountFeature(), std::memory_order_relaxed);
        f.ringHz.store (m_ring.getOnValue() > 0.0f ? m_ring.getCarrierHz() : 0.0f, std::memory_order_relaxed);
        f.ringBody.store (m_ring.getBodyIndex(), std::memory_order_relaxed);
        f.ringBodyMix.store (m_ring.getBodyFeature(), std::memory_order_relaxed);
    }

    // =============================================================================================
    // CreatureChain

    CreatureChain::CreatureChain() = default;

    void CreatureChain::prepare (double sampleRate, int maxBlock)
    {
        m_fs = dsp::safeSampleRate (sampleRate);
        m_maxBlock = std::max (1, maxBlock);

        m_in.prepare (m_fs, m_maxBlock);
        for (auto& set : m_sets)
            set.prepare (m_fs, m_maxBlock);
        m_output.prepare (m_fs, m_maxBlock);

        m_moduleLatency = m_sets[0].getLatencySamples() + m_in.getLatencySamples();
        m_latency = m_moduleLatency + m_output.getLatencySamples();
        m_fadeLength = std::max (1, dsp::samplesFor (c_reorderSeconds, m_fs));
        m_inputBlendLength = std::max (1, dsp::samplesFor (c_inputBlendSeconds, m_fs));
        m_corrPole = dsp::poleFromTau (c_fadeCorrelationSeconds, m_fs);
        m_settleSamples = std::max (1, dsp::samplesFor (c_reorderSettleSeconds, m_fs));

        for (auto* d : { &m_dryDelay[0], &m_dryDelay[1], &m_gainDelay, &m_activeDelay })
        {
            d->prepare (m_moduleLatency);
            d->setFixedDelay (m_moduleLatency);
        }
        m_dryMatch.prepare (m_fs, m_output.getLowPassHz());

        const auto size = static_cast<size_t> (m_maxBlock);
        for (auto* v : { &m_dryL, &m_dryR, &m_monoIn, &m_aL, &m_aR, &m_bL, &m_bR, &m_scratchR, &m_gainD, &m_activeD })
            v->assign (size, 0.0f);

        m_levelRelease = static_cast<float> (dsp::poleFromTau (c_outputLevelReleaseSeconds, m_fs));
        m_hits.prepare (m_fs);
        m_reorderCount = 0;
        reset();
    }

    void CreatureChain::reset()
    {
        m_in.reset();
        for (auto& set : m_sets)
            set.reset();
        m_output.reset();
        for (auto& d : m_dryDelay)
            d.reset();
        refillLevelDelays();
        m_dryMatch.reset (DryPhaseMatch::Settings {});
        m_active = 0;
        m_fadePos = -1;
        m_waitLeft = 0;
        m_snap = true;
        m_outputLevel = 0.0f;
        m_hits.reset();
    }

    void CreatureChain::refillLevelDelays()
    {
        // The gain delay holds IN's current gain (not zeros: the comp must not jump after a
        // reset), the flag delay "not matched".
        const float g = m_in.getCurrentTotalGain();
        m_gainDelay.reset();
        m_activeDelay.reset();
        for (int i = 0; i <= m_gainDelay.getMaxDelay(); ++i)
            m_gainDelay.push (g);
    }

    void CreatureChain::beginPass()
    {
        m_in.beginPass();
    }

    void CreatureChain::restoreLevels (const LevelState& levels, const ChainSettings& settings)
    {
        m_in.setParams (settings.in);
        m_output.setParams (settings.output);
        m_in.restoreLearning (levels.inLearnt, levels.inDb, levels.inEvidence, levels.inPitched);
        switch (levels.outKind)
        {
            case LevelState::OutKind::learnt:
                m_output.restoreLearning (levels.out);
                break;
            case LevelState::OutKind::hint:
                m_output.applyHint (levels.out.matchDb, hintSettleSamples());
                m_output.setSettingsKey (settingsKey (settings), true);
                m_adoptKeyNext = true;
                break;
            case LevelState::OutKind::keep:
                break;
        }
        refillLevelDelays();
    }

    bool CreatureChain::applyMatchHint (double matchDb, std::uint64_t hintKey, const ChainSettings& settings)
    {
        const std::uint64_t key = settingsKey (settings);
        const auto m = m_output.getMatchState();
        if (m.dataSeconds > 0.0 && m.haveKey && m.key == key)
            return false;
        m_output.setParams (settings.output);
        m_output.applyHint (matchDb, hintSettleSamples());
        // The hint's own settings are taken as the key in force; this chunk's key is compared with
        // it (an edit since the preset was loaded drops the hint's weight, nothing heard yet).
        m_output.setSettingsKey (hintKey, true);
        return true;
    }

    int CreatureChain::hintSettleSamples() const
    {
        return m_moduleLatency + dsp::samplesFor (c_hintSettleSeconds, m_fs);
    }

    LevelState CreatureChain::getLevels() const
    {
        LevelState l;
        l.inLearnt = m_in.isLearnt();
        l.inPitched = m_in.hasHeardPitched();
        l.inDb = m_in.getLearnedLevelDb();
        l.inEvidence = m_in.getEvidenceSeconds();
        l.out = m_output.getMatchState();
        return l;
    }

    std::uint64_t CreatureChain::settingsKey (const ChainSettings& s)
    {
        // What can change the sound: IN, then each module that is On, in the order the audio takes
        // it, with its own settings. A module that is Off passes its input untouched wherever it
        // sits, so its slot and its knobs are left out: moving or editing it is not a new sound
        // and must not start OUTPUT's probe (or drop a preset's hint), and the same On modules in
        // the same order give the same key whatever slots the Off ones hold.
        KeyHash k;
        k << s.in.trimDb << s.in.autoLevel;
        const auto order = Params::resolveOrder (s.positions);
        for (const int module : order)
        {
            if (! s.isOn (module))
                continue;
            k << module;
            switch (static_cast<Params::ModuleId> (module))
            {
                case Params::ModuleId::pitch:    k << s.pitch.pitchSemitones << s.pitch.size << s.pitch.drive << s.pitch.mode << s.pitch.formant; break;
                case Params::ModuleId::clean:    k << s.clean.contain << s.clean.gate << s.clean.tone; break;
                case Params::ModuleId::deess:    k << s.deess.amount << s.deess.mode; break;
                case Params::ModuleId::detail:   k << s.detail.low << s.detail.high << s.detail.speed; break;
                case Params::ModuleId::vocoder:  k << s.vocoder.gravel << s.vocoder.octave << s.vocoder.formantShiftBands; break;
                case Params::ModuleId::ring:     k << s.ring.amount << s.ring.freqHz << s.ring.ratio << s.ring.carrier << s.ring.track << s.ring.mode << s.ring.body; break;
                case Params::ModuleId::shimmer:  k << s.shimmer.amount << s.shimmer.rateHz << s.shimmer.mode; break;
                case Params::ModuleId::growl:    k << s.growl.depth << s.growl.rateHz << s.growl.shape << s.growl.character; break;
                case Params::ModuleId::swirl:    k << s.swirl.amount << s.swirl.rateHz << s.swirl.circuit << s.swirl.motion; break;
                case Params::ModuleId::roar:     k << s.roar.drive << s.roar.tone << s.roar.voicing; break;
                case Params::ModuleId::ensemble: k << s.ensemble.amount << s.ensemble.width << s.ensemble.voices << s.ensemble.motion; break;
                case Params::ModuleId::tame:     k << s.tame.amount; break;
                case Params::ModuleId::sub:      k << s.sub.level << s.sub.drive << s.sub.reg << s.sub.character; break;
                case Params::ModuleId::overtone: k << s.overtone.amount << s.overtone.freqOct << s.overtone.pattern << s.overtone.mode; break;
                case Params::ModuleId::chamber:  k << s.chamber.amount << s.chamber.resonance << s.chamber.edge << s.chamber.width
                                                   << s.chamber.room << s.chamber.speaker << s.chamber.boom; break;
            }
        }
        return k.h;
    }

    Topology CreatureChain::topologyFor (const ChainSettings& s)
    {
        Topology t;
        t.order = Params::resolveOrder (s.positions);
        t.split = c_numModules;
        for (int k = 0; k < c_numModules; ++k)
        {
            const auto m = static_cast<Params::ModuleId> (t.order[static_cast<size_t> (k)]);
            const bool makesStereo = (m == Params::ModuleId::ensemble && s.ensemble.on)
                                  || (m == Params::ModuleId::shimmer && s.shimmer.on && s.shimmer.mode == Params::ShimmerMode::stereo)
                                  || (m == Params::ModuleId::chamber && s.chamber.on && s.chamber.width > 0.0f);
            if (makesStereo)
            {
                t.split = k;
                break;
            }
        }
        return t;
    }

    DryPhaseMatch::Settings CreatureChain::dryMatchSettings (const ChainSettings& s)
    {
        DryPhaseMatch::Settings d;
        d.cleanOn = s.clean.on;
        d.tone = s.clean.tone;
        d.overtoneSplit = s.overtone.on && s.overtone.mode != OvertoneMode::add;
        const auto& spec = OvertoneModule::patternSpec (s.overtone.pattern);
        const double oct = std::clamp (static_cast<double> (dsp::finiteOrZero (s.overtone.freqOct)), -1.0, 1.0);
        d.overtoneFxHz = std::clamp (spec.crossoverHz * std::exp2 (oct), 30.0, 1000.0);
        return d;
    }

    void CreatureChain::startReorder (const Topology& next)
    {
        auto& from = m_sets[static_cast<size_t> (m_active)];
        auto& to = m_sets[static_cast<size_t> (1 - m_active)];
        to.copyStateFrom (from);
        m_waitLeft = to.beginInputBlend (from.topology(), next, m_inputBlendLength);
        to.enterTopology (next);
        m_fadePos = 0;
        m_corrAA = m_corrBB = m_corrAB = 0.0;
        ++m_reorderCount;
    }

    void CreatureChain::process (const float* inL, const float* inR, bool inStereo, float* outL, float* outR, int n,
                                 const ChainSettings& settings, MonsterFeatures& features)
    {
        n = std::clamp (n, 0, m_maxBlock);
        if (n == 0)
            return;
        if (inR == nullptr)
        {
            inR = inL;
            inStereo = false;
        }

        // The host input: IN's working copy, and the dry for OUTPUT's Mix.
        for (int i = 0; i < n; ++i)
        {
            const float xl = dsp::finiteOrZero (inL[i]);
            const float xr = inStereo ? dsp::finiteOrZero (inR[i]) : xl;
            m_monoIn[static_cast<size_t> (i)] = xl;
            m_scratchR[static_cast<size_t> (i)] = xr;
            m_dryL[static_cast<size_t> (i)] = m_dryDelay[0].processFixed (xl);
            m_dryR[static_cast<size_t> (i)] = m_dryDelay[1].processFixed (xr);
        }

        // Settings (once per chunk).
        const Topology wanted = topologyFor (settings);
        m_in.setParams (settings.in);
        m_output.setParams (settings.output);
        for (auto& set : m_sets)
            set.setParams (settings);

        if (m_snap)
        {
            // After prepare() or reset(): every state is empty, so take the requested topology and
            // settings at once (no reorder fade, no On fades, no parameter glides).
            m_snap = false;
            m_fadePos = -1;
            for (auto& set : m_sets)
            {
                set.setTopology (wanted);
                set.reset();
                set.setParams (settings);
            }
            m_in.reset();
            m_in.setParams (settings.in);
            m_output.reset();
            m_output.setParams (settings.output);
            refillLevelDelays();
            m_dryMatch.reset (dryMatchSettings (settings));
            m_pendingTopology = wanted;
            m_pendingSamples = m_settleSamples;
        }
        else
        {
            m_dryMatch.setSettings (dryMatchSettings (settings));
            // The positions are separate parameters: a writer (or a host's automation lanes) may
            // be part way through a new order. A fade starts only once the requested topology has
            // stood for c_reorderSettleSeconds, counted from the chunk after it was first seen,
            // so an order in between is never faded into.
            if (wanted != m_pendingTopology)
            {
                m_pendingTopology = wanted;
                m_pendingSamples = 0;
            }
            if (m_fadePos < 0 && m_pendingSamples >= m_settleSamples
                && wanted != m_sets[static_cast<size_t> (m_active)].topology())
                startReorder (wanted);
            m_pendingSamples = std::min (m_pendingSamples + n, m_settleSamples);
        }

        // The settings key (a change starts OUTPUT's probe; not in the chunk a hint was applied).
        m_output.setSettingsKey (settingsKey (settings), m_adoptKeyNext);
        m_adoptKeyNext = false;

        // IN: mono sum, trim, auto level and the voice stream.
        m_in.process (m_monoIn.data(), m_scratchR.data(), n, inStereo, VoiceFrameView {});
        const VoiceFrameView voice = m_in.getVoiceView();
        {
            // IN's gain and flag, delayed to line up with OUTPUT's input.
            const float* g = m_in.getGainTrace();
            const float* a = m_in.getMatchActiveTrace();
            for (int i = 0; i < n; ++i)
            {
                m_gainD[static_cast<size_t> (i)] = m_gainDelay.processFixed (g[i]);
                m_activeD[static_cast<size_t> (i)] = m_activeDelay.processFixed (a[i]);
            }
        }

        // The modules: the active chain, and during a reorder fade the incoming one too.
        auto& a = m_sets[static_cast<size_t> (m_active)];
        std::copy (m_monoIn.begin(), m_monoIn.begin() + n, m_aL.begin());
        a.process (m_aL.data(), m_aR.data(), n, voice, m_fadePos >= 0, nullptr);

        if (m_fadePos >= 0)
        {
            // The incoming chain's module inputs glide from what the outgoing chain feeds the
            // same module (continuous with the cloned state) to their new upstream signal.
            auto& b = m_sets[static_cast<size_t> (1 - m_active)];
            std::copy (m_monoIn.begin(), m_monoIn.begin() + n, m_bL.begin());
            b.process (m_bL.data(), m_bR.data(), n, voice, false, &a);

            // Power-preserving crossfade. PITCH grains, SUB's divider and ENSEMBLE's delays can
            // leave the two renders out of phase, and a linear (1-w)A + wB of two incoherent
            // signals cancels in the middle. So the gains are cos / sin of the fade, scaled by k so
            // that the mix has the power c^2 Pa + s^2 Pb, with the running correlation rho of A and
            // B (a few ms): k^2 (c^2 Pa + s^2 Pb + 2 rho c s sqrt(Pa Pb)) = c^2 Pa + s^2 Pb. Two
            // identical renders give rho = 1 and k (c A + s B) = A.
            const float invLength = 1.0f / static_cast<float> (m_fadeLength);
            for (int i = 0; i < n; ++i)
            {
                const auto s = static_cast<size_t> (i);
                if (m_waitLeft > 0)
                {
                    --m_waitLeft;   // the incoming chain runs, unheard, until its timing is the new order's
                    continue;
                }
                const double xa = m_aL[s], xb = m_bL[s], ya = m_aR[s], yb = m_bR[s];
                const double r = m_corrPole;
                m_corrAA = r * m_corrAA + (1.0 - r) * (xa * xa + ya * ya);
                m_corrBB = r * m_corrBB + (1.0 - r) * (xb * xb + yb * yb);
                m_corrAB = r * m_corrAB + (1.0 - r) * (xa * xb + ya * yb);

                m_fadePos = std::min (m_fadePos + 1, m_fadeLength);
                const float w = dsp::smoothstep01 (static_cast<float> (m_fadePos) * invLength);
                if (w >= 1.0f)
                {
                    m_aL[s] = m_bL[s];
                    m_aR[s] = m_bR[s];
                    continue;
                }
                const double phi = 0.5 * 3.14159265358979323846 * static_cast<double> (w);
                const double c = std::cos (phi), sn = std::sin (phi);
                const double cross = std::sqrt (m_corrAA * m_corrBB);
                double rho = cross > 1e-24 ? m_corrAB / cross : 1.0;
                rho = std::clamp (rho, c_minFadeCorrelation, 1.0);
                const double target = c * c * m_corrAA + sn * sn * m_corrBB;
                const double mixed = target + 2.0 * rho * c * sn * cross;
                double k = mixed > 1e-24 ? std::sqrt (target / mixed) : 1.0 / (c + sn);
                k = std::min (k, c_maxFadeGain);
                const auto ga = static_cast<float> (k * c), gb = static_cast<float> (k * sn);
                m_aL[s] = ga * m_aL[s] + gb * m_bL[s];
                m_aR[s] = ga * m_aR[s] + gb * m_bR[s];
            }

            if (m_fadePos >= m_fadeLength)
            {
                m_active = 1 - m_active;   // the incoming chain runs alone from the next chunk
                m_fadePos = -1;
            }
        }

        // OUTPUT, with the phase-matched dry.
        m_dryMatch.process (m_dryL.data(), m_dryR.data(), n, true);
        std::copy (m_aL.begin(), m_aL.begin() + n, outL);
        std::copy (m_aR.begin(), m_aR.begin() + n, outR);
        m_output.processWithDry (outL, outR, m_dryL.data(), m_dryR.data(), m_gainD.data(), m_activeD.data(), n, true);

        publish (settings, outL, outR, n, features);
    }

    void CreatureChain::publish (const ChainSettings& s, const float* outL, const float* outR, int n, MonsterFeatures& f)
    {
        float level = m_outputLevel;
        for (int i = 0; i < n; ++i)
        {
            const float x = std::max (std::abs (outL[i]), std::abs (outR[i]));
            level = x > level ? x : level * m_levelRelease;
        }
        m_outputLevel = dsp::flushTiny (level);
        m_hits.process (outL, outR, n);

        m_in.publishFeatures (f);
        const int front = frontSet();
        m_sets[static_cast<size_t> (front)].publishFeatures (s, f);
        m_output.publishFeatures (f);
        f.clipDb.store (m_output.getClipDb(), std::memory_order_relaxed);
        {
            const auto m = m_output.getMatchState();
            f.levelInLearnt.store (m_in.isLearnt() ? 1 : 0, std::memory_order_relaxed);
            f.levelInPitched.store (m_in.hasHeardPitched() ? 1 : 0, std::memory_order_relaxed);
            f.levelInDb.store (m_in.getLearnedLevelDb(), std::memory_order_relaxed);
            f.levelInEvidence.store (m_in.getEvidenceSeconds(), std::memory_order_relaxed);
            f.levelOutDb.store (m.matchDb, std::memory_order_relaxed);
            f.levelOutSeconds.store (m.seconds, std::memory_order_relaxed);
            f.levelOutDryPower.store (m.dryPower, std::memory_order_relaxed);
            f.levelOutWetPower.store (m.wetPower, std::memory_order_relaxed);
            f.levelOutData.store (m.dataSeconds, std::memory_order_relaxed);
            f.levelOutHaveGainRef.store (m.haveGainRef ? 1 : 0, std::memory_order_relaxed);
            f.levelOutGainRefDb.store (m.gainRefDb, std::memory_order_relaxed);
            f.levelOutHaveKey.store (m.haveKey ? 1 : 0, std::memory_order_relaxed);
            f.levelOutKey.store (m.key, std::memory_order_relaxed);
        }
        f.outputLevel.store (std::min (m_outputLevel, 1.0f), std::memory_order_relaxed);
        f.rumble.store (m_hits.rumble(), std::memory_order_relaxed);
        {
            // every hit counted since the last chunk, oldest first, each with its own strength
            const int count = m_hits.hits();
            const int moved = std::min ((count - m_hitsPublished) & MonsterFeatures::hitCountMask, MonsterFeatures::numHitLog);
            for (int k = moved - 1; k >= 0; --k)
            {
                const int c = (count - k) & MonsterFeatures::hitCountMask;
                f.publishHit (c, m_hits.strengthOf (c));
            }
            m_hitsPublished = count;
        }

        int mask = 0;
        for (int m = 0; m < c_numModules; ++m)
            if (s.isOn (m))
                mask |= 1 << m;
        f.moduleOnMask.store (mask, std::memory_order_relaxed);

        const auto& t = m_sets[static_cast<size_t> (front)].topology();
        std::uint64_t packed = 0;
        for (int k = 0; k < c_numModules; ++k)
            packed |= static_cast<std::uint64_t> (t.order[static_cast<size_t> (k)] & 0xf) << (4 * k);
        f.orderInUse.store (packed, std::memory_order_relaxed);
        f.stereoSplit.store (t.split, std::memory_order_relaxed);
        m_generation = (m_generation + 1) & 0x3fffffff;
        f.generation.store (m_generation, std::memory_order_relaxed);
    }
}
