#include "ChamberModule.h"

#include "../KnobLaws.h"
#include "../MonsterFeatures.h"

#include <cmath>

namespace roar
{
    namespace
    {
        constexpr double c_beta = 0.15;              // the breakup curve's bias (H2 next to H3)
        constexpr double c_clipKnee = 2.0;           // the amp's soft clip, 6 dB above the speaker's knee
        constexpr double c_referenceRms = 0.1;       // -20 dBFS: IN's working level
        constexpr double c_maxCalDb = 12.0;
        constexpr double c_ringFloorDb = -40.0, c_ringRangeDb = 30.0;
        constexpr int c_boomTick = 32;

        double logCosh (double z)
        {
            const double a = std::abs (z);
            return a + std::log1p (std::exp (-2.0 * a)) - 0.69314718055994531;
        }

        const double c_tanhBeta = std::tanh (c_beta);
        const double c_sech2Beta = 1.0 - c_tanhBeta * c_tanhBeta;

        // The speaker's breakup: f(v) = (tanh (v + b) - tanh b) / sech^2 b (slope 1 at 0), and its antiderivative.
        double breakup (double v) { return (std::tanh (v + c_beta) - c_tanhBeta) / c_sech2Beta; }
        double breakupF (double v) { return (logCosh (v + c_beta) - v * c_tanhBeta) / c_sech2Beta; }
        double clip (double v) { return c_clipKnee * std::tanh (v / c_clipKnee); }
        double clipF (double v) { return c_clipKnee * c_clipKnee * logCosh (v / c_clipKnee); }

        template <typename F, typename Fn>
        double adaa (double x, double& x1, double& f1, F antiderivative, Fn curve)
        {
            const double fx = antiderivative (x);
            const double dx = x - x1;
            const double y = std::abs (dx) > 1.0e-6 ? (fx - f1) / dx : curve (0.5 * (x + x1));
            x1 = x;
            f1 = fx;
            return y;
        }

        /** The edge stage on xg = x G with excursion d: the breakup, the excursion's grip on the highs, the amp clip. */
        double edgeCore (double xg, double d, double u, double& nlX1, double& nlF1, double& clipX1, double& clipF1)
        {
            const double e = adaa (d, nlX1, nlF1, breakupF, breakup) - d;
            const double h = (xg - d) * (1.0 - 0.25 * u * std::min (d * d, 1.0));
            return adaa (d + e + h, clipX1, clipF1, clipF, clip);
        }

        /** Loudness weighting per log frequency for a voice: the long-term average speech spectrum
            (1/3-octave levels, 63 Hz - 16 kHz, interpolated in log frequency) times the BS.1770 K
            weighting, so the level matches hold for what a loudness meter reads on speech. */
        double voiceWeight (double f)
        {
            static constexpr std::array<double, 25> ltass { 38.6, 43.5, 54.4, 57.7, 56.8, 60.2, 60.3, 59.0, 62.1, 62.1, 60.5, 56.8, 53.7,
                                                            53.0, 52.0, 48.7, 48.1, 46.8, 45.6, 44.5, 44.3, 43.7, 43.4, 41.3, 40.7 };
            const double pos = std::clamp (3.0 * std::log2 (std::max (f, 1.0) / 63.0), 0.0, static_cast<double> (ltass.size() - 1));
            const int i = std::min (static_cast<int> (pos), static_cast<int> (ltass.size()) - 2);
            const double db = ltass[static_cast<size_t> (i)] + (pos - i) * (ltass[static_cast<size_t> (i + 1)] - ltass[static_cast<size_t> (i)]);
            static const Biquad::Coeffs shelf = Biquad::highShelf (48000.0, 1681.974450955532, 0.7071752369554193, 3.99984385397);
            static const Biquad::Coeffs lowCut = Biquad::highpass (48000.0, 38.13547087613982, 0.5003270373253953);
            const double k = std::norm (shelf.response (std::min (f, 20000.0), 48000.0)) * std::norm (lowCut.response (std::min (f, 20000.0), 48000.0));
            return std::pow (10.0, (db - 60.0) / 10.0) * k;
        }
    }

    // =============================================================================================
    // Laws

    void ChamberModule::blendGains (double u, double& dry, double& wet)
    {
        const double b = laws::chamberBalanceDb (u);
        const double gd = b > 0.0 ? dsp::dbToGain (-b) : 1.0;
        const double gw = (b > 0.0 ? 1.0 : dsp::dbToGain (b)) * laws::chamberWetFade (u);
        // The wet and the dry are nearly uncorrelated (measured: the sum's loudness follows the
        // power sum within 0.5 LU), so the makeup is the power sum's.
        const double k = 1.0 / std::sqrt (gd * gd + gw * gw + 2.0 * c_blendCorrelation * gd * gw);
        dry = gd * k;
        wet = gw * k;
    }

    double ChamberModule::lateLevelDb (double u)
    {
        return laws::chamberLateLevelDb (u);
    }

    double ChamberModule::decayScale (double u, double maxScale)
    {
        return laws::chamberDecayScale (u, maxScale);
    }

    // =============================================================================================
    // Speakers

    void ChamberModule::Speaker::reset()
    {
        excursion.reset();
        for (auto& f : filters)
            f.reset();
        nlX1 = nlF1 = clipX1 = clipF1 = 0.0;
        nlPrimed = false;
    }

    void ChamberModule::Speaker::copyStateFrom (const Speaker& o)
    {
        type = o.type;
        excursion.copyStateFrom (o.excursion);
        excursion.setCoeffs (o.excursion.getCoeffs());
        for (size_t i = 0; i < filters.size(); ++i)
        {
            filters[i].copyStateFrom (o.filters[i]);
            filters[i].setCoeffs (o.filters[i].getCoeffs());
        }
        nlX1 = o.nlX1;
        nlF1 = o.nlF1;
        clipX1 = o.clipX1;
        clipF1 = o.clipF1;
        nlPrimed = o.nlPrimed;
    }

    void ChamberModule::designSpeakers()
    {
        const double fs = m_fs;
        using B = Biquad;
        // Tiny (1.5-2"), Small (3-4" sealed), Horn (a small re-entrant paging horn).
        struct Spec
        {
            double excursionHz, excursionQ;
            std::vector<B::Coeffs> filters;
        };
        const std::array<Spec, 3> specs { {
            { 380.0, 1.2, { B::highpass (fs, 380.0, 1.2), B::highpass (fs, 300.0, B::c_butterworthQ),
                            B::peaking (fs, 1100.0, 2.5, 6.0), B::peaking (fs, 2200.0, 3.0, -4.0), B::peaking (fs, 3200.0, 3.0, 5.0),
                            B::lowpass (fs, 6500.0, 0.9), B::lowpass (fs, 9000.0, B::c_bw4Q1), B::lowpass (fs, 9000.0, B::c_bw4Q2) } },
            { 180.0, 1.1, { B::highpass (fs, 180.0, 1.1),
                            B::peaking (fs, 900.0, 1.5, 3.0), B::peaking (fs, 2600.0, 2.0, 4.0), B::peaking (fs, 4500.0, 2.5, -3.0),
                            B::lowpass (fs, 8000.0, 0.8), B::lowpass (fs, 11000.0, B::c_bw4Q1), B::lowpass (fs, 11000.0, B::c_bw4Q2) } },
            { 450.0, 0.9, { B::highpass (fs, 450.0, B::c_bw4Q1), B::highpass (fs, 450.0, B::c_bw4Q2),
                            B::peaking (fs, 800.0, 4.0, 3.0), B::peaking (fs, 1600.0, 2.0, 6.0), B::peaking (fs, 3000.0, 3.0, 4.0),
                            B::lowpass (fs, 5500.0, B::c_bw4Q1), B::lowpass (fs, 5500.0, B::c_bw4Q2) } },
        } };

        for (size_t t = 0; t < specs.size(); ++t)
        {
            auto& d = m_designs[t];
            const auto& s = specs[t];
            d.excursion = B::lowpass (fs, s.excursionHz, s.excursionQ);
            d.numFilters = static_cast<int> (std::min (s.filters.size(), d.filters.size()));
            for (int i = 0; i < d.numFilters; ++i)
                d.filters[static_cast<size_t> (i)] = s.filters[static_cast<size_t> (i)];

            // Linear makeup: the chain's voice-weighted power gain (1/24 octave, 80 Hz - 12 kHz).
            double num = 0.0, den = 0.0;
            for (double f = 80.0; f < std::min (12000.0, 0.45 * fs); f *= std::pow (2.0, 1.0 / 24.0))
            {
                double mag2 = 1.0;
                for (int i = 0; i < d.numFilters; ++i)
                    mag2 *= std::norm (d.filters[static_cast<size_t> (i)].response (f, fs));
                num += voiceWeight (f) * mag2;
                den += voiceWeight (f);
            }
            // (The horn's narrow band reads about 0.9 dB louder on real voices than the average
            // speech spectrum predicts: measured on three takes and eight rooms.)
            d.makeup = std::min (4.0, 1.0 / std::sqrt (std::max (num / den, 1.0e-6))) * (t == 2 ? 0.9 : 1.0);

            // Edge makeup: a -20 dBFS voice-like tone (110 Hz, 30 harmonics at 1/k) through the
            // edge stage at every 10 % of the knob.
            const int length = static_cast<int> (0.4 * fs);
            std::vector<double> tone (static_cast<size_t> (length));
            double power = 0.0;
            for (int i = 0; i < length; ++i)
            {
                double v = 0.0;
                for (int k = 1; k <= 30; ++k)
                    v += std::sin (dsp::c_twoPi * 110.0 * k * i / fs + 0.7 * k * k) / k;
                tone[static_cast<size_t> (i)] = v;
                power += v * v;
            }
            const double norm = c_referenceRms / std::sqrt (power / length);
            for (auto& v : tone)
                v *= norm;
            const int skip = length / 4;
            for (int step = 0; step <= 10; ++step)
            {
                const double u = step / 10.0;
                const double g = dsp::dbToGain (laws::chamberEdgeDriveDb (u));
                Biquad excursion;
                excursion.setCoeffs (d.excursion);
                double x1 = 0.0, f1 = breakupF (0.0), c1 = 0.0, cf1 = clipF (0.0);
                double pin = 0.0, pout = 0.0;
                for (int i = 0; i < length; ++i)
                {
                    const double x = tone[static_cast<size_t> (i)];
                    const double xg = x * g;
                    const double ex = excursion.process (xg);
                    const double y = edgeCore (xg, ex, u, x1, f1, c1, cf1) / g;
                    if (i >= skip)
                    {
                        pin += x * x;
                        pout += y * y;
                    }
                }
                d.edgeMakeup[static_cast<size_t> (step)] = std::clamp (std::sqrt (pin / std::max (pout, 1.0e-20)), 0.25, 4.0);
            }
        }

        // The rooms' level with the speakers: the voice-weighted power of speaker x room re the
        // speaker alone, per 1/6 octave (a pink spectrum has the same power in every such band),
        // averaged in dB over the three speakers.
        for (int r = 0; r < ChamberBank::numRooms; ++r)
        {
            const auto& room = m_bank->room (r);
            double sumDb = 0.0;
            for (const auto& d : m_designs)
            {
                double withRoom = 0.0, alone = 0.0;
                for (int band = 0; band < ChamberBank::numBands && band < static_cast<int> (room.bandPower.size()); ++band)
                {
                    const double f = ChamberBank::bandCentre (band);
                    if (f > 0.45 * fs)
                        break;
                    double mag2 = voiceWeight (f);
                    for (int i = 0; i < d.numFilters; ++i)
                        mag2 *= std::norm (d.filters[static_cast<size_t> (i)].response (f, fs));
                    withRoom += mag2 * room.bandPower[static_cast<size_t> (band)];
                    alone += mag2;
                }
                sumDb += withRoom > 0.0 && alone > 0.0 ? 10.0 * std::log10 (withRoom / alone) : 0.0;
            }
            m_roomTrim[static_cast<size_t> (r)] = std::clamp (dsp::dbToGain (-sumDb / static_cast<double> (m_designs.size())), 0.25, 4.0);
        }
    }

    void ChamberModule::setSpeakerType (Speaker& s, int type) const
    {
        s.type = std::clamp (type, 0, 2);
        const auto& d = m_designs[static_cast<size_t> (s.type)];
        s.excursion.setCoeffs (d.excursion);
        for (size_t i = 0; i < s.filters.size(); ++i)
            s.filters[i].setCoeffs (static_cast<int> (i) < d.numFilters ? d.filters[i] : Biquad::Coeffs {});
    }

    float ChamberModule::runSpeaker (Speaker& s, float x, double drive, double edgeU, double edgeMix, double& residual)
    {
        const auto& d = m_designs[static_cast<size_t> (std::clamp (s.type, 0, 2))];
        const double xg = static_cast<double> (x) * drive;
        const double ex = s.excursion.process (xg);   // always: the excursion stays warm
        double y = x;
        if (edgeMix > 0.0)
        {
            if (! s.nlPrimed)
            {
                // Entering from bypass: start the ADAA states on the current values (no step).
                s.nlX1 = ex;
                s.nlF1 = breakupF (ex);
                const double v = xg;
                s.clipX1 = v;
                s.clipF1 = clipF (v);
                s.nlPrimed = true;
            }
            const double pos = std::clamp (edgeU, 0.0, 1.0) * 10.0;
            const int i0 = std::min (9, static_cast<int> (pos));
            const double makeup = d.edgeMakeup[static_cast<size_t> (i0)]
                                + (pos - i0) * (d.edgeMakeup[static_cast<size_t> (i0 + 1)] - d.edgeMakeup[static_cast<size_t> (i0)]);
            const double nl = edgeCore (xg, ex, edgeU, s.nlX1, s.nlF1, s.clipX1, s.clipF1) / drive * makeup;
            residual = (nl - x) * edgeMix;
            y = x + edgeMix * (nl - x);
        }
        else
        {
            s.nlPrimed = false;
            residual = 0.0;
        }
        for (int i = 0; i < d.numFilters; ++i)
            y = s.filters[static_cast<size_t> (i)].process (y);
        return static_cast<float> (y * d.makeup);
    }

    // =============================================================================================
    // Module

    ChamberModule::ChamberModule() = default;

    void ChamberModule::setBankForTest (std::shared_ptr<const ChamberBank> bank)
    {
        m_testBank = std::move (bank);
        if (m_testBank != nullptr && m_bank != nullptr)
        {
            m_bank = m_testBank;
            m_conv.prepare (m_bank->getLayout());
            designSpeakers();
            reset();
        }
    }

    void ChamberModule::prepare (double fs, int maxBlock)
    {
        m_fs = dsp::safeSampleRate (fs);
        m_maxBlock = std::max (1, maxBlock);
        m_bank = m_testBank != nullptr ? m_testBank : ChamberBank::get (m_fs);
        m_conv.prepare (m_bank->getLayout());

        const int grid = m_bank->getLayout().head;
        m_pre.assign (static_cast<size_t> (grid), 0.0f);
        for (auto& binding : m_wet)
            for (auto& v : binding)
                v.assign (static_cast<size_t> (grid), 0.0f);

        designSpeakers();
        for (size_t t = 0; t < m_speakers.size(); ++t)
            m_speakers[t].reset();

        m_changeLength = std::max (1, dsp::samplesFor (c_changeSeconds, m_fs));
        m_inputRampLength = std::max (1, dsp::samplesFor (c_smoothSeconds, m_fs));
        m_tickCoeff = 1.0 - std::exp (-static_cast<double> (grid) / (c_smoothSeconds * m_fs));
        m_boomCoeff = 1.0 - std::exp (-static_cast<double> (c_boomTick) / (c_boomSeconds * m_fs));
        m_ringRelease = dsp::poleFromTau (0.150, m_fs);
        m_edgeRelease = dsp::poleFromTau (0.080, m_fs);

        m_fade.prepare (m_fs, m_maxBlock, 0);
        reset();
    }

    void ChamberModule::resetWet()
    {
        m_conv.reset();
        const int room = std::clamp (m_settings.room, 0, ChamberBank::numRooms - 1);
        m_conv.bind (0, &m_bank->room (room).ir, 1);
        m_conv.bind (1, nullptr, 0);
        m_roomOf = { { room, room } };
        m_roomSlot = 0;
        m_roomFade = -1;

        for (auto& s : m_speakers)
            s.reset();
        setSpeakerType (m_speakers[0], m_settings.speaker);
        setSpeakerType (m_speakers[1], m_settings.speaker);
        m_speakerSlot = 0;
        m_speakerFade = -1;

        m_snapControls = true;
        m_inputRamp = 0;
        m_ringEnv = m_edgeEnv = m_edgeInEnv = 0.0;
    }

    void ChamberModule::reset()
    {
        m_fade.reset();
        if (m_bank != nullptr)
            resetWet();
        m_boomDb = m_boomTarget;
        m_boomPos = 0;
        for (auto& b : m_boom)
        {
            b.reset();
            b.setCoeffs (Biquad::lowShelf (m_fs, c_boomHz, c_boomQ, m_boomDb));
        }
    }

    void ChamberModule::setParams (const ChamberSettings& s)
    {
        m_settings = s;
        m_settings.amount = std::clamp (dsp::finiteOrZero (s.amount), 0.0f, 1.0f);
        m_settings.resonance = std::clamp (dsp::finiteOrZero (s.resonance), 0.0f, 1.0f);
        m_settings.edge = std::clamp (dsp::finiteOrZero (s.edge), 0.0f, 1.0f);
        m_settings.width = std::clamp (dsp::finiteOrZero (s.width), 0.0f, 1.0f);
        m_settings.room = std::clamp (s.room, 0, ChamberBank::numRooms - 1);
        m_settings.speaker = std::clamp (s.speaker, 0, 2);
        m_settings.boom = std::clamp (s.boom, 0, 3);
        m_fade.setOn (s.on);
        m_boomTarget = s.on ? c_boomDb[static_cast<size_t> (m_settings.boom)] : 0.0;
    }

    double ChamberModule::getDecaySeconds() const
    {
        if (m_bank == nullptr)
            return 0.0;
        const auto& room = m_bank->room (getRoom());
        return room.rt60 * decayScale (m_resonance, room.maxDecayScale);
    }

    void ChamberModule::applyResonance (int slot)
    {
        const auto& room = m_bank->room (m_roomOf[static_cast<size_t> (slot)]);
        const auto& layout = m_bank->getLayout();
        const auto& level = layout.levels[1];
        const double lateDb = lateLevelDb (m_resonance);
        const double s = decayScale (m_resonance, room.maxDecayScale);
        const double length = static_cast<double> (room.ir.getLength()) / m_fs;
        const double taperWeight = std::clamp ((s - 1.0) / 0.2, 0.0, 1.0);

        float* gains = m_conv.gains (slot, 1);
        const int used = room.ir.getNumPartitions (1);
        double nominal = room.earlyEnergy, shaped = room.earlyEnergy;
        for (int k = 0; k < level.count; ++k)
        {
            const double t = (static_cast<double> (level.start) + (k + 0.5) * level.partition) / m_fs;
            const double ramp = std::min (1.0, (k + 1) / 3.0);
            double g = dsp::dbToGain (ChamberBank::lateGainDb (room, t, ramp * lateDb, s));
            // A longer decay ends with the IR: the room's scale keeps the tail 40 dB down by then,
            // and the last 15 % fade out.
            const double x = std::clamp ((t - 0.85 * length) / (0.15 * length), 0.0, 1.0);
            const double taper = 0.5 * (1.0 + std::cos (dsp::c_pi * x));
            g *= 1.0 + taperWeight * (taper - 1.0);
            gains[k] = static_cast<float> (g);
            if (k < used)
            {
                const double e = room.lateEnergy[static_cast<size_t> (k)];
                nominal += e;
                shaped += g * g * e;
            }
        }
        const double cal = shaped > 0.0 ? std::sqrt (nominal / shaped) : 1.0;
        m_kcal[static_cast<size_t> (slot)] = static_cast<float> (std::clamp (cal, dsp::dbToGain (-c_maxCalDb), dsp::dbToGain (c_maxCalDb))
                                                                 * m_roomTrim[static_cast<size_t> (m_roomOf[static_cast<size_t> (slot)])]);
    }

    void ChamberModule::startRoom (int room, bool stereo)
    {
        const int next = 1 - m_roomSlot;
        m_conv.bind (next, &m_bank->room (room).ir, stereo ? 2 : 1);
        m_roomOf[static_cast<size_t> (next)] = room;
        applyResonance (next);
        m_kcalFrom[static_cast<size_t> (next)] = m_kcal[static_cast<size_t> (next)];
        m_roomSlot = next;
        m_roomFade = 0;
    }

    void ChamberModule::controlTick (bool stereo)
    {
        const auto& s = m_settings;
        if (m_snapControls)
        {
            m_amount = s.amount;
            m_resonance = s.resonance;
            m_edge = s.edge;
            m_width = s.width;
        }
        else
        {
            const auto glide = [this] (double& v, double target)
            {
                v += m_tickCoeff * (target - v);
                if (std::abs (target - v) < 1.0e-5)
                    v = target;
            };
            glide (m_amount, s.amount);
            glide (m_resonance, s.resonance);
            glide (m_edge, s.edge);
            glide (m_width, s.width);
        }

        // A finished room change: the outgoing binding stops.
        const int old = 1 - m_roomSlot;
        if (m_roomFade < 0 && m_conv.getBinding (old) != nullptr)
            m_conv.bind (old, nullptr, 0);

        // Outputs: the side only while it is heard.
        const int outputs = stereo && (s.width > 0.0f || m_width > 0.0) ? 2 : 1;
        for (int b = 0; b < ZeroLatencyConvolver::maxBindings; ++b)
            if (m_conv.getBinding (b) != nullptr)
                m_conv.setActiveOutputs (b, outputs);

        // A new room or speaker (one change at a time; a later one waits for the fade).
        if (m_roomFade < 0 && s.room != m_roomOf[static_cast<size_t> (m_roomSlot)])
        {
            if (m_snapControls)
            {
                m_conv.bind (m_roomSlot, &m_bank->room (s.room).ir, outputs);
                m_roomOf[static_cast<size_t> (m_roomSlot)] = s.room;
            }
            else
            {
                startRoom (s.room, outputs == 2);
            }
        }
        if (m_speakerFade < 0 && s.speaker != m_speakers[static_cast<size_t> (m_speakerSlot)].type)
        {
            if (m_snapControls)
            {
                setSpeakerType (m_speakers[static_cast<size_t> (m_speakerSlot)], s.speaker);
            }
            else
            {
                const int next = 1 - m_speakerSlot;
                m_speakers[static_cast<size_t> (next)].reset();
                setSpeakerType (m_speakers[static_cast<size_t> (next)], s.speaker);
                m_speakerSlot = next;
                m_speakerFade = 0;
            }
        }

        // Resonance and the calibration of every bound room.
        for (int b = 0; b < ZeroLatencyConvolver::maxBindings; ++b)
        {
            m_kcalFrom[static_cast<size_t> (b)] = m_kcal[static_cast<size_t> (b)];
            if (m_conv.getBinding (b) != nullptr)
                applyResonance (b);
        }

        double dry = 1.0, wet = 0.0;
        blendGains (m_amount, dry, wet);
        m_dryFrom = m_dryGain;
        m_wetFrom = m_wetGain;
        m_widthFrom = m_widthGain;
        m_dryGain = static_cast<float> (dry);
        m_wetGain = static_cast<float> (wet);
        m_widthGain = static_cast<float> (stereo ? laws::chamberSideGain (m_width) : 0.0);

        if (m_snapControls)
        {
            m_dryFrom = m_dryGain;
            m_wetFrom = m_wetGain;
            m_widthFrom = m_widthGain;
            m_kcalFrom = m_kcal;
            m_snapControls = false;
        }
    }

    void ChamberModule::process (float* l, float* r, int n, bool stereo, const VoiceFrameView&)
    {
        m_fade.captureDry (l, r, n, stereo);
        if (! m_fade.isActive())
        {
            m_fade.outputDry (l, r, n, stereo);
            return;
        }
        if (m_fade.consumeRestart())
            resetWet();

        const int grid = m_conv.getLayout().head;
        const float invGrid = 1.0f / static_cast<float> (grid);
        const float invChange = 1.0f / static_cast<float> (m_changeLength);
        int done = 0;
        while (done < n)
        {
            if (m_conv.gridPosition() == 0)
                controlTick (stereo);
            const int m = std::min (n - done, m_conv.samplesToGrid());
            const int tickPos = m_conv.gridPosition();
            float* L = l + done;
            float* R = r + done;

            // The speaker (and, during a change, the outgoing one).
            const double edgeMix = dsp::smoothstep01 (m_edge / c_edgeFadeIn);
            const double drive = dsp::dbToGain (laws::chamberEdgeDriveDb (m_edge));
            for (int i = 0; i < m; ++i)
            {
                float x = stereo ? 0.5f * (L[i] + R[i]) : L[i];
                if (m_inputRamp < m_inputRampLength)
                    x *= dsp::smoothstep01 (static_cast<float> (++m_inputRamp) / static_cast<float> (m_inputRampLength));
                double residual = 0.0;
                float y = runSpeaker (m_speakers[static_cast<size_t> (m_speakerSlot)], x, drive, m_edge, edgeMix, residual);
                if (m_speakerFade >= 0)
                {
                    double unused = 0.0;
                    const float w = dsp::smoothstep01 (static_cast<float> (m_speakerFade + 1) * invChange);
                    const float old = runSpeaker (m_speakers[static_cast<size_t> (1 - m_speakerSlot)], x, drive, m_edge, edgeMix, unused);
                    y = old + w * (y - old);
                    if (++m_speakerFade >= m_changeLength)
                        m_speakerFade = -1;
                }
                m_pre[static_cast<size_t> (i)] = y;
                const double ax = std::abs (static_cast<double> (x)), ar = std::abs (residual);
                m_edgeInEnv = ax > m_edgeInEnv ? ax : m_edgeInEnv * m_edgeRelease;
                m_edgeEnv = ar > m_edgeEnv ? ar : m_edgeEnv * m_edgeRelease;
            }

            // The rooms.
            float* const outs[2][2] {
                { m_wet[0][0].data(), m_wet[0][1].data() },
                { m_wet[1][0].data(), m_wet[1][1].data() },
            };
            m_conv.process (m_pre.data(), m, outs);

            const int cur = m_roomSlot, old = 1 - cur;
            const bool curSide = m_conv.getActiveOutputs (cur) > 1;
            const bool oldBound = m_conv.getBinding (old) != nullptr;
            const bool oldSide = oldBound && m_conv.getActiveOutputs (old) > 1;
            for (int i = 0; i < m; ++i)
            {
                const float t = static_cast<float> (tickPos + i + 1) * invGrid;
                const float gd = m_dryFrom + t * (m_dryGain - m_dryFrom);
                const float gw = m_wetFrom + t * (m_wetGain - m_wetFrom);
                const float wd = m_widthFrom + t * (m_widthGain - m_widthFrom);
                const float kc = m_kcalFrom[static_cast<size_t> (cur)] + t * (m_kcal[static_cast<size_t> (cur)] - m_kcalFrom[static_cast<size_t> (cur)]);

                float aNew = 1.0f, aOld = 0.0f;
                if (m_roomFade >= 0)
                {
                    const float w = static_cast<float> (m_roomFade + 1) * invChange;
                    const float phi = 1.57079633f * std::min (w, 1.0f);
                    aNew = std::sin (phi);
                    aOld = std::cos (phi);
                    if (++m_roomFade >= m_changeLength)
                        m_roomFade = -1;
                }

                const auto si = static_cast<size_t> (i);
                float wetM = aNew * kc * m_wet[static_cast<size_t> (cur)][0][si];
                float wetS = curSide ? aNew * kc * m_wet[static_cast<size_t> (cur)][1][si] : 0.0f;
                if (oldBound && aOld > 0.0f)
                {
                    const float ko = m_kcalFrom[static_cast<size_t> (old)] + t * (m_kcal[static_cast<size_t> (old)] - m_kcalFrom[static_cast<size_t> (old)]);
                    wetM += aOld * ko * m_wet[static_cast<size_t> (old)][0][si];
                    if (oldSide)
                        wetS += aOld * ko * m_wet[static_cast<size_t> (old)][1][si];
                }

                const double aw = std::abs (static_cast<double> (wetM));
                m_ringEnv = aw > m_ringEnv ? aw : m_ringEnv * m_ringRelease;

                if (stereo)
                {
                    const float side = wd * wetS;
                    L[i] = gd * L[i] + gw * (wetM + side);
                    R[i] = gd * R[i] + gw * (wetM - side);
                }
                else
                {
                    L[i] = gd * L[i] + gw * wetM;
                }
            }
            done += m;
        }

        m_ringEnv = dsp::flushTiny (m_ringEnv);
        m_edgeEnv = dsp::flushTiny (m_edgeEnv);
        m_edgeInEnv = dsp::flushTiny (m_edgeInEnv);
        m_fade.apply (l, r, n, stereo);
    }

    void ChamberModule::processReturn (float* l, float* r, int n)
    {
        if (m_boomTarget == 0.0 && m_boomDb == 0.0)
            return;   // bit-exact pass

        int done = 0;
        while (done < n)
        {
            if (m_boomPos == 0 && m_boomDb != m_boomTarget)
            {
                m_boomDb += m_boomCoeff * (m_boomTarget - m_boomDb);
                if (std::abs (m_boomTarget - m_boomDb) < 1.0e-3)
                    m_boomDb = m_boomTarget;
                const auto c = Biquad::lowShelf (m_fs, c_boomHz, c_boomQ, m_boomDb);
                for (auto& b : m_boom)
                    b.setCoeffs (c);
            }
            const int m = std::min (n - done, c_boomTick - m_boomPos);
            for (int i = done; i < done + m; ++i)
            {
                l[i] = m_boom[0].process (l[i]);
                r[i] = m_boom[1].process (r[i]);
            }
            done += m;
            m_boomPos = (m_boomPos + m) % c_boomTick;
        }

        if (m_boomTarget == 0.0 && m_boomDb == 0.0)
            for (auto& b : m_boom)
                b.reset();
    }

    void ChamberModule::copyStateFrom (const Module& other)
    {
        const auto& o = sameType<ChamberModule> (other);
        m_fade.copyStateFrom (o.m_fade);
        m_settings = o.m_settings;
        m_conv.copyStateFrom (o.m_conv);
        for (size_t i = 0; i < m_speakers.size(); ++i)
            m_speakers[i].copyStateFrom (o.m_speakers[i]);
        m_speakerSlot = o.m_speakerSlot;
        m_speakerFade = o.m_speakerFade;
        m_roomOf = o.m_roomOf;
        m_roomSlot = o.m_roomSlot;
        m_roomFade = o.m_roomFade;
        m_inputRamp = o.m_inputRamp;
        m_amount = o.m_amount;
        m_resonance = o.m_resonance;
        m_edge = o.m_edge;
        m_width = o.m_width;
        m_dryGain = o.m_dryGain;
        m_wetGain = o.m_wetGain;
        m_widthGain = o.m_widthGain;
        m_dryFrom = o.m_dryFrom;
        m_wetFrom = o.m_wetFrom;
        m_widthFrom = o.m_widthFrom;
        m_kcal = o.m_kcal;
        m_kcalFrom = o.m_kcalFrom;
        m_snapControls = o.m_snapControls;
        for (size_t i = 0; i < m_boom.size(); ++i)
        {
            m_boom[i].copyStateFrom (o.m_boom[i]);
            m_boom[i].setCoeffs (o.m_boom[i].getCoeffs());
        }
        m_boomDb = o.m_boomDb;
        m_boomTarget = o.m_boomTarget;
        m_boomPos = o.m_boomPos;
        m_ringEnv = o.m_ringEnv;
        m_edgeEnv = o.m_edgeEnv;
        m_edgeInEnv = o.m_edgeInEnv;
    }

    void ChamberModule::copyLeftStateToRight()
    {
        // The wet path runs on the mid (l in the mono part, and (l + r) / 2 = l where it turns
        // stereo); the Boom shelves always run on both channels. Only the dry delay is per side.
        m_fade.copyLeftStateToRight();
    }

    void ChamberModule::publishFeatures (MonsterFeatures& f) const
    {
        const float on = m_fade.getValue();
        const double ringDb = dsp::gainToDb (m_ringEnv / c_referenceRms);
        const float ring = static_cast<float> (std::clamp ((ringDb - c_ringFloorDb) / c_ringRangeDb, 0.0, 1.0));
        const float edge = m_edgeInEnv > 1.0e-6 ? static_cast<float> (std::clamp (2.0 * m_edgeEnv / m_edgeInEnv, 0.0, 1.0)) : 0.0f;
        f.chamberWet.store (on * m_wetGain, std::memory_order_relaxed);
        f.chamberRing.store (on * ring * std::min (1.0f, 2.0f * m_wetGain), std::memory_order_relaxed);
        f.chamberEdge.store (on * edge, std::memory_order_relaxed);
        f.chamberBoom.store (static_cast<float> (std::clamp (m_boomDb / 9.0, 0.0, 1.0)), std::memory_order_relaxed);
        f.chamberResonance.store (on * static_cast<float> (m_resonance), std::memory_order_relaxed);
        f.chamberDecay.store (on > 0.0f ? static_cast<float> (getDecaySeconds()) : 0.0f, std::memory_order_relaxed);
        f.chamberRoom.store (getRoom(), std::memory_order_relaxed);
    }
}
