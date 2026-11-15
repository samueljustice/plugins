#include "VocoderModule.h"

#include "../KnobLaws.h"

#include "../MonsterFeatures.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace roar
{
    namespace
    {
        constexpr double c_pi = 3.14159265358979323846;

        constexpr double c_lowestCentreHz = 80.0;
        constexpr double c_highestCentreHz = 12000.0;

        // The analysis bands are never narrower than this many ERBs (24.7 (4.37 f / 1000 + 1) Hz):
        // a constant-Q band at 80 Hz is 13 Hz wide and rings for tens of milliseconds, so the
        // weight of a syllable would trail its consonant. The synthesis bands keep their width.
        // No analysis band reaches below c_analysisLowEdgeHz (the lowest one widens upwards
        // instead), which keeps mains hum out of the envelopes.
        constexpr double c_analysisMinErbs = 1.45;
        constexpr double c_analysisLowEdgeHz = 70.0;

        constexpr int c_subBlock = 16;              // control-rate updates (pitch, U/V, mapping)
        constexpr float c_maxWet = 0.85f;           // dry is kept so the words stay readable
        constexpr float c_fadeSeconds = 0.02f;
        constexpr float c_characterSeconds = 0.02f;
        constexpr float c_stereoPan = 0.3f;         // bands pan alternately +-30 % when stereo

        // Follower times at gravel 0 (smooth) and at the speed cap. Faster lets more glottal
        // ripple through; the cap stops short of the clipped, stuttering extreme.
        constexpr float c_attackSlowMs = 3.0f, c_releaseSlowMs = 40.0f;
        constexpr float c_speedCap = 1.0f;          // decades the times shrink by at full gravel
        constexpr float c_rippleLowpassHz = 30.0f;  // ripple above this is boosted
        constexpr float c_whitenSeconds = 0.015f;
        constexpr float c_whitenFloor = 0.03f;      // limits the boost of bands between harmonics

        constexpr float c_glideSeconds = 0.025f;
        constexpr float c_vibratoHz = 5.0f;
        constexpr float c_vibratoCents = 6.0f;
        constexpr float c_driftCents = 3.0f;
        constexpr float c_minCarrierHz = 12.0f, c_maxCarrierHz = 1200.0f;
        constexpr float c_minF0Hz = 20.0f, c_maxF0Hz = 2000.0f;   // pitches the carrier follows

        // U/V: band-energy tilt above 4 kHz vs below 1 kHz, with hysteresis.
        constexpr float c_uvOn = 0.55f, c_uvOff = 0.3f;
        constexpr float c_uvOnWhenTracked = 1.1f;   // stricter while the detector hears a pitch
        constexpr float c_uvLevelFloor = 1.0e-4f;   // ~ -80 dB: below this, keep the last state
        constexpr float c_uvSeconds = 0.004f;

        // Equivalent noise bandwidth of a 4th-order Butterworth prototype, relative to its band.
        constexpr double c_enbwFactor = 1.026;

        // Output trim so the vocoded signal sits near the modulator's level (measured).
        constexpr float c_makeup = 0.67f;

        constexpr uint32_t c_noiseSeed = 0x9e3779b9u;

        inline float polyBlep (float t, float dt)
        {
            if (t < dt)
            {
                const float x = t / dt;
                return x + x - x * x - 1.0f;
            }
            if (t > 1.0f - dt)
            {
                const float x = (t - 1.0f) / dt;
                return x * x + x + x + 1.0f;
            }
            return 0.0f;
        }

        inline float coeffForSeconds (float seconds, double sampleRate)
        {
            return 1.0f - (float) std::exp (-1.0 / (std::max (1.0e-6, (double) seconds) * sampleRate));
        }

        inline float sanitize (float x)
        {
            return std::abs (x) < 1.0e4f ? x : 0.0f;   // also false for NaN
        }
    }

    // ============================================================================== module

    float VocoderModule::gravelFor (float u)
    {
        return std::isfinite (u) ? static_cast<float> (laws::gravelAmount (static_cast<double> (u))) : 0.0f;
    }

    void VocoderModule::prepare (double fs, int maxBlock)
    {
        m_engine.setParams (gravelFor (m_settings.gravel), static_cast<int> (m_settings.octave), m_settings.formantShiftBands);
        m_engine.prepare (fs);
        m_fade.prepare (fs, maxBlock, getLatencySamples());
        m_fade.setOn (m_settings.on);
        reset();
    }

    void VocoderModule::reset()
    {
        m_fade.reset();
        m_engine.clearState();
        m_engine.resetPitchGate();
    }

    void VocoderModule::setParams (const VocoderSettings& s)
    {
        m_settings = s;
        m_engine.setParams (gravelFor (s.gravel), static_cast<int> (s.octave), s.formantShiftBands);
        m_fade.setOn (s.on);
    }

    void VocoderModule::process (float* l, float* r, int n, bool stereo, const VoiceFrameView& voice)
    {
        if (l == nullptr || n <= 0)
            return;
        stereo = stereo && r != nullptr;

        m_fade.captureDry (l, r, n, stereo);
        if (! m_fade.isActive())
        {
            m_engine.holdPitch (l, stereo ? r : nullptr, n, voice);
            m_fade.outputDry (l, r, n, stereo);
            return;
        }
        if (m_fade.consumeRestart())
            m_engine.restart (voice.atSample (0));

        m_engine.process (l, r, n, stereo, voice);
        m_fade.apply (l, r, n, stereo);
    }

    void VocoderModule::copyStateFrom (const Module& other)
    {
        const auto& o = sameType<VocoderModule> (other);
        m_engine = o.m_engine;
        m_fade.copyStateFrom (o.m_fade);
        m_settings = o.m_settings;
    }

    void VocoderModule::copyLeftStateToRight()
    {
        // The engine analyses the mid and has one synthesis bank, so only the dry delay of the
        // On fade is per channel.
        m_fade.copyLeftStateToRight();
    }

    float VocoderModule::getBandLevel (int band) const
    {
        const auto& levels = m_engine.getBandLevels();
        return levels[static_cast<size_t> (std::clamp (band, 0, c_numBands - 1))] * m_fade.getValue();
    }

    float VocoderModule::getActivity() const
    {
        return m_engine.isActive() ? m_fade.getValue() : 0.0f;
    }

    void VocoderModule::publishFeatures (MonsterFeatures& features) const
    {
        const float fade = m_fade.getValue();
        const auto& levels = m_engine.getBandLevels();
        for (int b = 0; b < c_numBands && b < MonsterFeatures::numBands; ++b)
            features.vocoderBands[static_cast<size_t> (b)].store (levels[static_cast<size_t> (b)] * fade, std::memory_order_relaxed);
        features.gravelActive.store (getActivity(), std::memory_order_relaxed);
    }

    // ============================================================================== engine

    void VocoderModule::Engine::Bank::clear()
    {
        for (int s = 0; s < c_numSections; ++s)
            for (int b = 0; b < c_numBands; ++b)
            {
                ic1[s][b] = 0.0f;
                ic2[s][b] = 0.0f;
            }
    }

    void VocoderModule::Engine::prepare (double sampleRate)
    {
        m_sampleRate = dsp::safeSampleRate (sampleRate);
        const double fs = m_sampleRate;
        const double ratio = std::pow (c_highestCentreHz / c_lowestCentreHz, 1.0 / (c_numBands - 1));
        const double edge = std::sqrt (ratio);

        // 4th-order Butterworth low-pass prototype poles in the upper half plane.
        std::complex<double> lpPoles[2];
        for (int k = 0; k < 2; ++k)
            lpPoles[k] = std::polar (1.0, c_pi * (2.0 * k + 5.0) / 8.0);

        for (int b = 0; b < c_numBands; ++b)
        {
            const double fc = c_lowestCentreHz * std::pow (ratio, (double) b);
            const double fh = std::min (fc * edge, 0.45 * fs);
            const double fl = std::min (fc / edge, 0.9 * fh);
            m_centreHz[(size_t) b] = (float) fc;

            // Pre-warped edges; the sections are then realised by TPT SVFs (bilinear), so
            // g = w / (2 fs) with no further tan().
            const double wl = 2.0 * fs * std::tan (c_pi * fl / fs);
            const double wh = 2.0 * fs * std::tan (c_pi * fh / fs);
            const double w0 = std::sqrt (wl * wh);
            const double bw = wh - wl;

            // Low-pass to band-pass: each prototype pole p gives s^2 - p B s + w0^2 = 0.
            int section = 0;
            double gain = 1.0;
            for (const auto& p : lpPoles)
            {
                const auto pb = p * bw;
                const auto disc = std::sqrt (pb * pb - 4.0 * w0 * w0);
                for (const auto& root : { (pb + disc) * 0.5, (pb - disc) * 0.5 })
                {
                    // Keep the upper-half-plane member of each conjugate pair.
                    const auto s = root.imag() >= 0.0 ? root : std::conj (root);
                    const double w = std::abs (s);
                    const double k = -2.0 * s.real() / w;   // 1 / Q
                    const double g = w / (2.0 * fs);
                    const double a1 = 1.0 / (1.0 + g * (g + k));
                    m_a1[section][b] = (float) a1;
                    m_a2[section][b] = (float) (g * a1);
                    m_a3[section][b] = (float) (g * g * a1);
                    gain *= bw / w;   // section = (B / w) * (w s / (s^2 + k w s + w^2))
                    ++section;
                }
            }

            // The analysis band: on the same centre (so the band mapping and the read-out stay
            // put), at least c_analysisMinErbs wide. A wider band passes more of a broadband voice, so
            // its envelope is scaled by sqrt (synthesis width / analysis width) to hold the level.
            {
                const double erbHz = 24.7 * (4.37 * fc / 1000.0 + 1.0);
                const double widthHz = std::max (fh - fl, c_analysisMinErbs * erbHz);
                const double lowEdge = std::max (c_analysisLowEdgeHz, std::sqrt (fc * fc + 0.25 * widthHz * widthHz) - 0.5 * widthHz);
                const double fhA = std::min (lowEdge + widthHz, 0.45 * fs);
                const double flA = std::min (lowEdge, 0.9 * fhA);
                const double wlA = 2.0 * fs * std::tan (c_pi * flA / fs);
                const double whA = 2.0 * fs * std::tan (c_pi * fhA / fs);
                const double w0A = std::sqrt (wlA * whA);
                const double bwA = whA - wlA;
                int sectionA = 0;
                double gainA = 1.0;
                for (const auto& p : lpPoles)
                {
                    const auto pb = p * bwA;
                    const auto disc = std::sqrt (pb * pb - 4.0 * w0A * w0A);
                    for (const auto& root : { (pb + disc) * 0.5, (pb - disc) * 0.5 })
                    {
                        const auto s = root.imag() >= 0.0 ? root : std::conj (root);
                        const double w = std::abs (s);
                        const double k = -2.0 * s.real() / w;
                        const double g = w / (2.0 * fs);
                        const double a1 = 1.0 / (1.0 + g * (g + k));
                        m_an1[sectionA][b] = (float) a1;
                        m_an2[sectionA][b] = (float) (g * a1);
                        m_an3[sectionA][b] = (float) (g * g * a1);
                        gainA *= bwA / w;
                        ++sectionA;
                    }
                }
                m_analysisGain[(size_t) b] = (float) (gainA * std::sqrt ((fh - fl) / (fhA - flA)));
            }

            // Expected band RMS of the pulse stack: a pulse harmonic n has mean power
            // ~ 0.5 * (2 / (pi n))^2 * 0.5 and the band holds bw / f harmonics, so
            // P = bw * f / (pi^2 * fc^2). Gain = 1 / sqrt (P) = pi fc / sqrt (bw f).
            const double bwHz = fh - fl;
            m_pulseGainBase[(size_t) b] = (float) (gain * c_pi * fc / std::sqrt (bwHz));

            // Uniform noise in [-1, 1] has variance 1/3; after the band its power is
            // (1/3) * 2 * ENBW / fs.
            const double noisePower = (1.0 / 3.0) * 2.0 * c_enbwFactor * bwHz / fs;
            m_noiseGainBase[(size_t) b] = (float) (gain / std::sqrt (noisePower));

            // At low sample rates (22.05 / 24 kHz) the top bands would be clamped onto the same
            // edges below Nyquist and duplicate each other. Such a band stays silent: no analysis
            // envelope (so it adds nothing to the U/V tilt or the band display) and no carrier.
            if (fc * edge > 0.45 * fs)
            {
                m_analysisGain[(size_t) b] = 0.0f;
                m_pulseGainBase[(size_t) b] = 0.0f;
                m_noiseGainBase[(size_t) b] = 0.0f;
            }

            // Breath: noise above ~5 kHz rides on noise even in voiced frames (a sub-octave buzz
            // up there just sounds like fizz). Unvoiced frames move everything from 2.5 kHz up.
            m_breathNoise[(size_t) b] = (float) std::clamp ((fc - 5000.0) / 4000.0, 0.0, 1.0) * 0.6f;
            m_uvRamp[(size_t) b] = (float) std::clamp ((fc - 2500.0) / 1000.0, 0.0, 1.0);

            const float pan = (b & 1) == 0 ? c_stereoPan : -c_stereoPan;
            m_panLeft[(size_t) b] = 1.0f + pan;
            m_panRight[(size_t) b] = 1.0f - pan;
        }

        // Fixed oscillator spread: duty 20..30 % fills each other's sinc nulls, detune +-8 cents.
        uint32_t seed = 0x2545f491u;
        auto rnd = [&seed]()
        {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            return (float) (seed >> 8) * (1.0f / 16777216.0f);
        };
        for (int i = 0; i < c_numOscillators; ++i)
        {
            const float spread = ((float) i + 0.5f) / (float) c_numOscillators * 2.0f - 1.0f;
            m_detuneCents[(size_t) i] = 8.0f * spread * (0.6f + 0.4f * rnd());
            m_duty[(size_t) i] = 0.2f + 0.1f * rnd();
            m_driftRate[(size_t) i] = 0.07f + 0.25f * rnd();
            m_driftPhase[(size_t) i] = rnd();
        }
        m_driftPhaseStart = m_driftPhase;

        m_wetStep = 1.0f / (c_fadeSeconds * (float) fs);
        m_envLowCoeff = coeffForSeconds (1.0f / (2.0f * (float) c_pi * c_rippleLowpassHz), fs);
        m_whitenCoeff = coeffForSeconds (c_whitenSeconds, fs);
        m_glideCoeff = 1.0f - (float) std::exp (-(double) c_subBlock / (static_cast<double> (c_glideSeconds) * fs));
        m_characterCoeff = 1.0f - (float) std::exp (-(double) c_subBlock / (static_cast<double> (c_characterSeconds) * fs));
        m_makeup = c_makeup;
        m_pitchGate.prepare (fs, c_subBlock);
        m_gatePeak = 0.0f;
        m_prepared = true;

        m_active = false;
        m_wet = 0.0f;
        m_characterTarget = m_targetGravel;
        m_gravel = m_targetGravel;
        updateCharacter();
        m_shift = m_targetShift;
        clearState();
    }

    void VocoderModule::Engine::clearState()
    {
        // Flushes the filters and followers; the wet fade and the active state carry on, so a
        // host reset mid-stream does not jump the dry/wet balance.
        m_analysis.clear();
        m_synth.clear();
        m_env.fill (0.0f);
        m_envLow.fill (0.0f);
        // Start the whitening at the expected unit level so the first frames are not boosted.
        m_whiten.fill (1.0f);
        m_noiseMix = m_breathNoise;
        m_envPad.fill (0.0f);
        m_bandLevels.fill (0.0f);
        m_unvoiced = false;
        m_uv = 0.0f;
        m_subCounter = 0;

        uint32_t seed = 0x51ed2701u;
        for (auto& p : m_phase)
        {
            seed = seed * 1664525u + 1013904223u;
            p = (float) (seed >> 8) * (1.0f / 16777216.0f);   // spread start phases: no onset click
        }
        m_carrierHz = m_carrierTargetHz;
    }

    void VocoderModule::Engine::setParams (float gravel01, int octaveIndex, float formantShiftBands)
    {
        m_targetGravel = std::isfinite (gravel01) ? std::clamp (gravel01, 0.0f, 1.0f) : 0.0f;
        m_octaveIndex = octaveIndex > 0 ? 1 : 0;
        m_targetShift = std::isfinite (formantShiftBands) ? std::clamp (-formantShiftBands, 0.0f, 4.0f) : 0.0f;
    }

    bool VocoderModule::Engine::followPitch (const VoiceFrame& voice)
    {
        // Hold the last pitch through unvoiced passages, silence, the noise floor and the quiet
        // tail of a word (CarrierPitchGate): the next phrase then starts from a real pitch.
        const auto decision = m_pitchGate.tick (m_gatePeak, voice, c_minF0Hz, c_maxF0Hz);
        m_gatePeak = 0.0f;
        if (decision.accepted)
            m_heldF0 = voice.f0Hz;
        else if (decision.restHz > 0.0f)
            m_heldF0 = decision.restHz;   // in a pause: wait at the voice's usual pitch
        return decision.phraseStart;
    }

    void VocoderModule::Engine::updateVoice (const VoiceFrame& voice)
    {
        const bool phraseStart = followPitch (voice);
        m_detectorVoiced = voice.voiced;
        m_detectorConfidence = voice.confidence;

        const float octave = m_octaveIndex == 0 ? 0.5f : 0.25f;
        m_carrierTargetHz = std::clamp (m_heldF0 * octave, c_minCarrierHz, c_maxCarrierHz);
        if (phraseStart)
            m_carrierHz = m_carrierTargetHz;   // a new phrase starts on its own pitch: no glide from the last one
    }

    void VocoderModule::Engine::activate (const VoiceFrame& voice)
    {
        m_shift = m_targetShift;
        updateVoice (voice);
        if (m_targetGravel > 0.0f)
            m_characterTarget = m_targetGravel;
        m_gravel = m_characterTarget;   // starts from silence, so no glide is needed
        updateCharacter();
        m_carrierHz = m_carrierTargetHz;
        clearState();
        m_vibratoPhase = 0.0f;
        m_driftPhase = m_driftPhaseStart;
        m_noiseState = c_noiseSeed;
        m_wet = 0.0f;
        m_active = true;
    }

    void VocoderModule::Engine::holdPitch (const float* left, const float* right, int numSamples, const VoiceFrameView& voice)
    {
        // The same gate on the same control grid as the active path, so the pitch the carrier
        // starts from does not depend on when (or in which blocks) the engine was idle.
        if (! m_prepared || left == nullptr)
            return;
        for (int i = 0; i < numSamples; ++i)
        {
            if (m_subCounter == 0)
                followPitch (voice.atSample (i));
            m_subCounter = (m_subCounter + 1) % c_subBlock;
            const float x = right != nullptr ? 0.5f * (sanitize (left[i]) + sanitize (right[i])) : sanitize (left[i]);
            m_gatePeak = std::max (m_gatePeak, std::abs (x));
        }
    }

    void VocoderModule::Engine::restart (const VoiceFrame& voice)
    {
        const float wetTarget = m_targetGravel * c_maxWet;
        if (wetTarget <= 0.0f)
        {
            m_active = false;
            m_wet = 0.0f;
            m_bandLevels.fill (0.0f);
            return;
        }
        activate (voice);
        m_wet = wetTarget;
    }

    void VocoderModule::Engine::updateCharacter()
    {
        const float g = m_gravel;
        const float speed = std::min (g, 1.0f) * c_speedCap;
        const float scale = std::pow (10.0f, -speed);
        m_attackCoeff = coeffForSeconds (c_attackSlowMs * 0.001f * scale, m_sampleRate);
        m_releaseCoeff = coeffForSeconds (c_releaseSlowMs * 0.001f * scale, m_sampleRate);
        // Ripple boost tuned by measurement on a synthetic vowel (envelope sd/mean in bands
        // 1-9 kHz): ~0.2 at 50 % (gritty, words intact), ~0.9 at 100 % (rough, not chopped).
        m_rippleBoost = 2.0f * std::pow (g, 1.3f);
        m_satAmount = 1.0f + 3.0f * g * g * g;   // a single band saturates at 1 / sat
    }

    void VocoderModule::Engine::updateSubBlock()
    {
        const float fs = (float) m_sampleRate;

        // Glide the character too: a jump in ripple boost or saturation would step every band's
        // envelope at once and click.
        if (m_gravel != m_characterTarget)
        {
            m_gravel += (m_characterTarget - m_gravel) * m_characterCoeff;
            if (std::abs (m_characterTarget - m_gravel) < 1.0e-4f)
                m_gravel = m_characterTarget;
            updateCharacter();
        }

        // Carrier glide in the log domain.
        m_carrierHz *= std::exp (m_glideCoeff * std::log (m_carrierTargetHz / m_carrierHz));
        if (! std::isfinite (m_carrierHz))
            m_carrierHz = m_carrierTargetHz;

        const float dtBlock = (float) c_subBlock / fs;
        m_vibratoPhase += c_vibratoHz * dtBlock;
        m_vibratoPhase -= std::floor (m_vibratoPhase);
        const float vib = c_vibratoCents * std::sin (2.0f * (float) c_pi * m_vibratoPhase);
        for (int i = 0; i < c_numOscillators; ++i)
        {
            float& dp = m_driftPhase[(size_t) i];
            dp += m_driftRate[(size_t) i] * dtBlock;
            dp -= std::floor (dp);
            const float cents = m_detuneCents[(size_t) i] + vib + c_driftCents * std::sin (2.0f * (float) c_pi * dp);
            const float hz = m_carrierHz * std::exp2 (cents * (1.0f / 1200.0f));
            m_inc[(size_t) i] = std::min (hz / fs, 0.45f);
        }

        // Formant shift: smooth, then split for the band-mapping interpolation.
        m_shift += (m_targetShift - m_shift) * std::min (1.0f, dtBlock / 0.03f);
        m_shiftInt = std::clamp ((int) std::floor (m_shift), 0, 4);
        m_shiftFrac = std::clamp (m_shift - (float) m_shiftInt, 0.0f, 1.0f);

        // U/V from the band energies, with hysteresis.
        float lo = 0.0f, hi = 0.0f, total = 0.0f;
        for (int b = 0; b < c_numBands; ++b)
        {
            const float e2 = m_env[(size_t) b] * m_env[(size_t) b];
            total += e2;
            if (m_centreHz[(size_t) b] < 1000.0f)
                lo += e2;
            else if (m_centreHz[(size_t) b] > 4000.0f)
                hi += e2;
        }
        if (total > c_uvLevelFloor * c_uvLevelFloor)
        {
            const float tilt = std::sqrt (hi / (lo + 1.0e-12f));
            const bool tracked = m_detectorVoiced && m_detectorConfidence > 0.6f;
            const float on = tracked ? c_uvOnWhenTracked : c_uvOn;
            m_unvoiced = m_unvoiced ? tilt > c_uvOff : tilt > on;
        }
        const float uvCoeff = std::min (1.0f, dtBlock / c_uvSeconds);
        m_uv += ((m_unvoiced ? 1.0f : 0.0f) - m_uv) * uvCoeff;

        // Per-band carrier: equal-power mix of pulse and noise, both pre-scaled to ~unit band RMS.
        const float pulseScale = 1.0f / std::sqrt (m_carrierHz);
        for (int b = 0; b < c_numBands; ++b)
        {
            const float target = std::max (m_breathNoise[(size_t) b], m_uv * m_uvRamp[(size_t) b]);
            float& n = m_noiseMix[(size_t) b];
            n += (target - n) * uvCoeff;
            m_pulseGain[(size_t) b] = m_pulseGainBase[(size_t) b] * pulseScale * std::sqrt (std::max (0.0f, 1.0f - n));
            m_noiseGain[(size_t) b] = m_noiseGainBase[(size_t) b] * std::sqrt (std::max (0.0f, n));
        }
    }

    void VocoderModule::Engine::runBank (Bank& bank, float* lanes, bool analysis) const
    {
        for (int s = 0; s < c_numSections; ++s)
        {
            float* ic1 = bank.ic1[s];
            float* ic2 = bank.ic2[s];
            const float* a1 = analysis ? m_an1[s] : m_a1[s];
            const float* a2 = analysis ? m_an2[s] : m_a2[s];
            const float* a3 = analysis ? m_an3[s] : m_a3[s];
            for (int b = 0; b < c_numBands; ++b)
            {
                const float v3 = lanes[b] - ic2[b];
                const float v1 = a1[b] * ic1[b] + a2[b] * v3;
                const float v2 = ic2[b] + a2[b] * ic1[b] + a3[b] * v3;
                ic1[b] = 2.0f * v1 - ic1[b];
                ic2[b] = 2.0f * v2 - ic2[b];
                lanes[b] = v1;
            }
        }
    }

    void VocoderModule::Engine::synthesise (const float* carrierIn)
    {
        float* lanes = m_lanesSynth.data();
        for (int b = 0; b < c_numBands; ++b)
            lanes[b] = carrierIn[b];
        runBank (m_synth, lanes, false);

        const float* pad = m_envPad.data() + m_shiftInt;
        const float frac = m_shiftFrac;
        const float wc = m_whitenCoeff;
        const float sat = m_satAmount;
        float* ms = m_whiten.data();
        for (int b = 0; b < c_numBands; ++b)
        {
            const float c = lanes[b];
            ms[b] += wc * (c * c - ms[b]);
            const float env = pad[b] + frac * (pad[b + 1] - pad[b]);
            const float y = c * env / std::sqrt (ms[b] + c_whitenFloor);
            // Soft VCA saturation: transparent at low band levels, rounds loud bands at high gravel.
            lanes[b] = y / std::sqrt (1.0f + sat * sat * y * y);
        }
    }

    float VocoderModule::Engine::renderMono (const float* carrierIn)
    {
        synthesise (carrierIn);
        const float* lanes = m_lanesSynth.data();
        float sum = 0.0f;
        for (int b = 0; b < c_numBands; ++b)
            sum += lanes[b];
        return sum * m_makeup;
    }

    void VocoderModule::Engine::renderStereo (const float* carrierIn, float& outL, float& outR)
    {
        synthesise (carrierIn);
        const float* lanes = m_lanesSynth.data();
        const float* pl = m_panLeft.data();
        const float* pr = m_panRight.data();
        float sumL = 0.0f, sumR = 0.0f;
        for (int b = 0; b < c_numBands; ++b)
        {
            sumL += lanes[b] * pl[b];
            sumR += lanes[b] * pr[b];
        }
        outL = sumL * m_makeup;
        outR = sumR * m_makeup;
    }

    float VocoderModule::Engine::nextNoise()
    {
        m_noiseState ^= m_noiseState << 13;
        m_noiseState ^= m_noiseState >> 17;
        m_noiseState ^= m_noiseState << 5;
        return (float) (int32_t) m_noiseState * (1.0f / 2147483648.0f);
    }

    bool VocoderModule::Engine::stateIsFinite() const
    {
        float acc = 0.0f;
        for (int b = 0; b < c_numBands; ++b)
            acc += m_env[(size_t) b] + m_whiten[(size_t) b]
                 + m_analysis.ic1[c_numSections - 1][b] + m_synth.ic1[c_numSections - 1][b];
        return std::isfinite (acc) && std::isfinite (m_carrierHz);
    }

    void VocoderModule::Engine::process (float* left, float* right, int numSamples, bool stereo, const VoiceFrameView& voice)
    {
        if (! m_prepared || left == nullptr || numSamples <= 0)
            return;

        const bool useStereo = stereo && right != nullptr;
        const float wetTarget = m_targetGravel * c_maxWet;

        if (! m_active)
        {
            if (wetTarget <= 0.0f)
            {
                holdPitch (left, useStereo ? right : nullptr, numSamples, voice);
                return;   // Gravel 0: no CPU, signal untouched
            }
            activate (voice.atSample (0));
        }
        else if (m_targetGravel > 0.0f)
        {
            // Character follows the knob while it is up; during a fade-out it keeps its last value.
            m_characterTarget = m_targetGravel;
        }

        alignas (16) float carrier[c_numBands];
        float* lanes = m_lanes.data();

        for (int i = 0; i < numSamples; ++i)
        {
            if (m_subCounter == 0)
            {
                updateVoice (voice.atSample (i));
                updateSubBlock();
            }
            m_subCounter = (m_subCounter + 1) % c_subBlock;

            const float dryL = sanitize (left[i]);
            const float dryR = useStereo ? sanitize (right[i]) : dryL;
            const float x = useStereo ? 0.5f * (dryL + dryR) : dryL;
            m_gatePeak = std::max (m_gatePeak, std::abs (x));

            // Analysis bank and followers.
            for (int b = 0; b < c_numBands; ++b)
                lanes[b] = x;
            runBank (m_analysis, lanes, true);

            const float att = m_attackCoeff, rel = m_releaseCoeff, lowC = m_envLowCoeff, boost = m_rippleBoost;
            float* env = m_env.data();
            float* envLow = m_envLow.data();
            float* pad = m_envPad.data();
            const float* gain = m_analysisGain.data();
            for (int b = 0; b < c_numBands; ++b)
            {
                const float r = std::abs (lanes[b]) * gain[b];
                env[b] += (r > env[b] ? att : rel) * (r - env[b]);
                envLow[b] += lowC * (env[b] - envLow[b]);
                // Exaggerate the ripple above ~30 Hz (glottal-rate AM) without changing the mean.
                pad[b] = std::max (0.0f, env[b] + boost * (env[b] - envLow[b]));
            }

            // Pulse stack.
            float pe = 0.0f, po = 0.0f;
            for (int k = 0; k < c_numOscillators; ++k)
            {
                float t = m_phase[(size_t) k];
                const float dt = m_inc[(size_t) k];
                const float duty = m_duty[(size_t) k];
                float y = (t < duty ? 1.0f : 0.0f) - duty;
                y += 0.5f * polyBlep (t, dt);
                float t2 = t - duty;
                if (t2 < 0.0f)
                    t2 += 1.0f;
                y -= 0.5f * polyBlep (t2, dt);
                t += dt;
                if (t >= 1.0f)
                    t -= 1.0f;
                m_phase[(size_t) k] = t;
                if ((k & 1) == 0)
                    pe += y;
                else
                    po += y;
            }

            const float noise = nextNoise();
            const float* pg = m_pulseGain.data();
            const float* ng = m_noiseGain.data();
            const float p = (pe + po) * 0.25f;   // 1 / sqrt (16)
            for (int b = 0; b < c_numBands; ++b)
                carrier[b] = p * pg[b] + noise * ng[b];

            float vocL, vocR;
            if (useStereo)
                renderStereo (carrier, vocL, vocR);
            else
            {
                vocL = renderMono (carrier);
                vocR = vocL;
            }

            // Dry and vocoded are largely incoherent, so a plain crossfade dips ~2 dB mid-way;
            // normalising by the summed power keeps the loudness steady as gravel moves.
            m_wet += std::clamp (wetTarget - m_wet, -m_wetStep, m_wetStep);
            const float norm = 1.0f / std::sqrt ((1.0f - m_wet) * (1.0f - m_wet) + m_wet * m_wet);
            const float dryGain = (1.0f - m_wet) * norm;
            const float wetGain = m_wet * norm;
            left[i] = dryL * dryGain + vocL * wetGain;
            if (useStereo)
                right[i] = dryR * dryGain + vocR * wetGain;
        }

        if (! stateIsFinite())
        {
            clearState();
            m_carrierHz = m_carrierTargetHz;
        }

        for (int b = 0; b < c_numBands; ++b)
        {
            const float e = m_env[(size_t) b];
            const float db = 20.0f * std::log10 (e + 1.0e-9f);
            m_bandLevels[(size_t) b] = std::clamp ((db + 60.0f) / 60.0f, 0.0f, 1.0f) * std::min (1.0f, m_wet / c_maxWet * 4.0f);
        }

        if (wetTarget <= 0.0f && m_wet <= 0.0f)
        {
            m_active = false;
            m_bandLevels.fill (0.0f);
        }
    }
}
