#pragma once

#include "../common/Biquad.h"
#include "../common/Module.h"
#include "../common/ZeroLatencyConvolver.h"
#include "ChamberRooms.h"

#include <array>
#include <memory>
#include <vector>

namespace roar
{
    struct ChamberSettings
    {
        bool on = false;
        float amount = 0.50f;      // Chamber: the blend against the dry, 0..1
        float resonance = 0.40f;   // how much the room's late part rings, 0..1
        float edge = 0.30f;        // speaker drive, 0..1
        float width = 0.35f;       // the room's own side signal, 0..1
        int room = 2;              // Params::ChamberRoom
        int speaker = 1;           // Params::ChamberSpeaker
        int boom = 1;              // Params::ChamberBoom
    };

    /**
     * CHAMBER: a voice played through a small speaker into a tight, resonant room, blended back
     * under the untreated voice.
     *
     *   mid (l, or (l + r) / 2) -> EDGE: speaker drive -> SPEAKER: the small speaker's response
     *       -> ROOM: stereo convolution (the room's mid and side) -> wet
     *   l = g_d dry_l + g_w (wet_mid + w wet_side),  r = g_d dry_r + g_w (wet_mid - w wet_side)
     *   post-chain (processReturn): BOOM low shelf at 110 Hz on both channels
     *
     * The knob laws live in KnobLaws.h (laws::chamber*).
     *
     * - Edge (Hammerstein, zero latency): x = m dB(24 u); the speaker's excursion d = LP2 (x) at
     *   its box resonance breaks up through f(v) = (tanh (v + 0.15) - tanh 0.15) / sech^2 0.15
     *   (even and odd harmonics, ADAA-1); the highs are modulated by the excursion; a soft amp clip
     *   (ADAA-1 tanh, knee 6 dB higher) follows; a static makeup (measured at prepare on a
     *   -20 dBFS voice-like tone) keeps the level. Below 5 % it fades out; at 0 % it is bypassed.
     * - Speaker: Tiny / Small / Horn, minimum-phase biquads (box high-pass, breakup peaks and dips,
     *   top roll-off), with a static makeup weighted by speech's long-term spectrum and K weighting
     *   (so the three read the same on a loudness meter with a voice; the rooms are matched the same way).
     * - Room: one of ChamberBank's ten rooms through the ZeroLatencyConvolver. Resonance shapes
     *   the late part (from ~10.7 ms): its level (-9 dB at 0 %, nominal at 40 %, +9 dB at 100 %)
     *   and its decay (x0.55 .. x the room's longest, at most 1.6, of the room's own; the decay
     *   change starts where the room's late energy peaks), with the wet's energy held (k_cal), so the
     *   knob changes the ring, not the loudness. Width: the room's side at w = Width^0.75 (0 % mono,
     *   100 % the room's full stereo); the mono fold-down is the same at every Width.
     * - Chamber: the balance b = -24 + 30 u dB (wet re dry): below 0 dB the dry stays at 0 dB and
     *   the wet is at b, above it the wet stays and the dry falls; equal at 80 %; the wet fades out
     *   below 2 %. A static makeup 1 / sqrt (g_d^2 + g_w^2) keeps the sum's level (wet and dry are
     *   nearly uncorrelated).
     * - Boom: Off / Light / Medium / Heavy = 0 / +3 / +6 / +9 dB at 110 Hz (RBJ shelf, Q 0.7) after
     *   the whole chain, so CLEAN's high-pass cannot undo it; it glides over 50 ms (also with On).
     *
     * Room and speaker changes crossfade over 50 ms (the new room is primed from the convolver's
     * history, so it starts as if it had always run). Settings are read on the convolver's grid
     * (64 samples at 48 kHz, counted from reset), so the output is bit-identical for any block
     * sizes. Latency 0 at every setting. makesStereo() while On with Width > 0. Off: the dry, bit
     * for bit; the convolver stops. No allocation after prepare().
     */
    class ChamberModule final : public Module
    {
    public:
        static constexpr double c_changeSeconds = 0.050;
        static constexpr double c_smoothSeconds = 0.020;
        static constexpr double c_boomHz = 110.0;
        static constexpr double c_boomQ = 0.7;
        static constexpr double c_boomSeconds = 0.050;
        static constexpr std::array<double, 4> c_boomDb { 0.0, 3.0, 6.0, 9.0 };
        static constexpr double c_edgeFadeIn = 0.05;
        static constexpr double c_blendCorrelation = 0.0;
        /** The most Resonance's level hold may correct the wet by, either way (dB). */
        static constexpr double c_maxLevelHoldDb = 12.0;

        ChamberModule();

        void prepare (double fs, int maxBlock) override;
        void reset() override;
        int getLatencySamples() const override { return 0; }
        bool makesStereo() const override { return m_settings.on && m_settings.width > 0.0f; }
        void process (float* l, float* r, int n, bool stereo, const VoiceFrameView& voice) override;
        void processReturn (float* l, float* r, int n) override;
        void copyStateFrom (const Module& other) override;
        void copyLeftStateToRight() override;
        void publishFeatures (MonsterFeatures& features) const override;

        void setParams (const ChamberSettings& s);

        // ---- Laws (pure)
        /** Dry and wet gains for the Chamber knob u (makeup included). */
        static void blendGains (double u, double& dry, double& wet);
        /** The late part's level change, dB, for Resonance u. */
        static double lateLevelDb (double u);
        /** The late part's decay scale (x the room's own) for Resonance u, up to the room's maxScale at 100 %. */
        static double decayScale (double u, double maxScale);

        // ---- Readouts and test hooks
        /** The decay in effect (the room's RT60 x the Resonance scale), seconds. */
        double getDecaySeconds() const;
        int getRoom() const { return m_roomOf[static_cast<size_t> (m_roomSlot)]; }
        const ChamberBank* getBank() const { return m_bank.get(); }
        /** Uses this bank instead of the shared one (tests); call before prepare's reset or after prepare. */
        void setBankForTest (std::shared_ptr<const ChamberBank> bank);
        const ZeroLatencyConvolver& getConvolver() const { return m_conv; }
        double getBoomDb() const { return m_boomDb; }
        /** The correction Resonance's level hold asks for in the room in use, dB, before it is
            limited to +-c_maxLevelHoldDb (tests: a room must stay clear of that limit). */
        double getLevelHoldDb() const { return m_levelHoldDb[static_cast<size_t> (m_roomSlot)]; }

    private:
        struct SpeakerDesign
        {
            Biquad::Coeffs excursion;
            std::array<Biquad::Coeffs, 8> filters {};
            int numFilters = 0;
            double makeup = 1.0;                       // linear chain, voice-weighted
            std::array<double, 11> edgeMakeup {};      // edge stage, per 10 % of the knob
        };

        struct Speaker
        {
            int type = 1;
            Biquad excursion;
            std::array<Biquad, 8> filters;
            double nlX1 = 0.0, nlF1 = 0.0, clipX1 = 0.0, clipF1 = 0.0;
            bool nlPrimed = false;

            void reset();
            void copyStateFrom (const Speaker& o);
        };

        void designSpeakers();
        /** Sets the speaker's type and its filters' coefficients (its states are left as they are). */
        void setSpeakerType (Speaker& s, int type) const;
        void controlTick (bool stereo);
        void startRoom (int room, bool stereo);
        void applyResonance (int slot);
        float runSpeaker (Speaker& s, float x, double drive, double edgeU, double edgeMix, double& residual);
        void resetWet();

        double m_fs = 48000.0;
        int m_maxBlock = 512;
        OnFade m_fade;
        ChamberSettings m_settings;

        std::shared_ptr<const ChamberBank> m_bank, m_testBank;
        ZeroLatencyConvolver m_conv;
        std::array<SpeakerDesign, 3> m_designs;
        // Per room: the gain that puts the speaker and the room together at the speaker's own
        // voice-weighted level (the average over the three speakers), so switching rooms keeps the level.
        std::array<double, ChamberBank::numRooms> m_roomTrim {};

        // Speakers: the one in use and, during a change, the one fading out.
        std::array<Speaker, 2> m_speakers;
        int m_speakerSlot = 0;
        int m_speakerFade = -1;                  // samples into a speaker change, -1 none
        // Rooms: the binding in use and, during a change, the one fading out.
        std::array<int, 2> m_roomOf { { 2, 2 } };
        int m_roomSlot = 0;
        int m_roomFade = -1;
        int m_changeLength = 2400;
        // The wet path's input fades in after a restart (20 ms), so the speaker and the room hear
        // the voice start smoothly: their reflections of an abrupt start would click after the On fade.
        int m_inputRamp = 0, m_inputRampLength = 960;

        // Smoothed controls (per grid tick) and the per-sample ramps between ticks.
        double m_amount = 0.5, m_resonance = 0.4, m_edge = 0.3, m_width = 0.35;
        double m_tickCoeff = 0.0;
        float m_dryGain = 1.0f, m_wetGain = 0.0f, m_widthGain = 0.0f;
        float m_dryFrom = 1.0f, m_wetFrom = 0.0f, m_widthFrom = 0.0f;
        std::array<float, 2> m_kcal { { 1.0f, 1.0f } }, m_kcalFrom { { 1.0f, 1.0f } };
        std::array<double, 2> m_levelHoldDb { { 0.0, 0.0 } };
        bool m_snapControls = true;

        // Boom (post chain).
        std::array<Biquad, 2> m_boom;
        double m_boomDb = 0.0, m_boomTarget = 0.0, m_boomCoeff = 0.0;
        int m_boomPos = 0;   // samples into the shelf's 32-sample update grid

        // Readouts.
        double m_ringEnv = 0.0, m_ringRelease = 0.999, m_edgeEnv = 0.0, m_edgeInEnv = 0.0, m_edgeRelease = 0.999;

        // Scratch.
        std::vector<float> m_pre;
        std::array<std::array<std::vector<float>, 2>, 2> m_wet;
    };
}
