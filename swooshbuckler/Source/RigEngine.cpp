#include "RigEngine.h"
#include "Parameters.h"

#include <algorithm>
#include <functional>

namespace
{
    constexpr float c_lineSeconds = 4.6f;
    constexpr size_t c_sideBus = 1;           // the bus that keeps a stereo bed wide (bus B)
    constexpr float c_sideFadeSeconds = 0.015f; // a track's side signal coming in or going out
    constexpr float c_maxPassDelay = 4.0f;
    constexpr float c_earOffset = 0.09f / 343.0f;
    constexpr float c_baseDelay = 0.002f;
    // Space: the ping-pong echo's two times and how far and how fast each drifts.
    constexpr float c_echoSecondsL = 0.337f, c_echoSecondsR = 0.523f;
    constexpr float c_echoDriftSeconds = 0.004f, c_echoDriftHzL = 0.11f, c_echoDriftHzR = 0.17f;
    // Space: the reverb's wet level at 100%, and how much of the dry is taken away there.
    constexpr float c_spaceWet = 0.9f, c_spaceDryTrim = 0.45f;
    // Space in a room: how much of the reverb follows where the room's sound has been (the rest is shared evenly),
    // and over how long that is smoothed.
    constexpr float c_spaceLean = 0.5f, c_spaceLeanSeconds = 0.3f;
    // Every track rides at its own speed (offset speeds keep the layers from moving together).
    constexpr std::array<float, RigEngine::c_tracks> c_rideOffsets { 1.0f, 1.37f, 0.71f, 1.62f, 0.87f, 1.19f, 0.79f, 1.48f, 0.93f, 1.27f };

    // Fill: every track has its own three echo times (first tap, then 1.9x and 3.7x it), so layered tracks don't echo
    // on one shared half-second grid. The longest is 2.15 s (the lines hold 2.2 s).
    constexpr std::array<float, RigEngine::c_tracks> c_fillTap { 0.500f, 0.557f, 0.431f, 0.581f, 0.467f, 0.523f, 0.449f, 0.571f, 0.483f, 0.541f };
    constexpr std::array<float, 3> c_fillRatios { 1.0f, 1.9f, 3.7f };
    constexpr float c_fillLineSeconds = 2.2f;
    // Smoothing times (set in seconds so they are the same at every sample rate and chunk size).
    constexpr float c_matchSeconds = 0.003f;  // the Swirl and Drive level-match gains
    constexpr float c_duckSeconds = 0.0019f;  // Ducked by A
    constexpr float c_peakPasses = 0.72f;     // the recent peak's fall-back time constant, in pass lengths (-12 dB per pass)
    constexpr float c_shapeSeconds = 0.06f;   // Pass Mode: how fast one window shape turns into the other
    constexpr float c_variUpDbPerOctave = 1.0f;    // VARI on: make-up per octave of a track's own upward pitch
    constexpr float c_restartFadeSeconds = 0.005f; // Play while the sources still sound: they fade this fast first
    constexpr float c_proxSeconds = 0.02f;    // how fast the pass's nearness reaches Low End, Swirl and Filter
    // Fly-By: the tail is over after this much of the way out, and the approach rises late (a higher power = later).
    constexpr float c_flyTail = 0.6f;
    constexpr float c_flyApproach = 1.3f;
    // A throw: its sources open this fast and let go this slowly, and stay open a little past the end of its tail.
    constexpr float c_throwOpenSeconds = 0.005f, c_throwCloseSeconds = 0.06f;
    // Velocity: how close the softest throw comes (the hardest comes right by), and how loud it is against the hardest.
    constexpr float c_throwSoftCloseness = 0.3f, c_throwSoftLevel = 0.35f;
    // ThrowFlight::nearer at 1 (or -1): how much closer (further off) than the bus draws the pass, and how much
    // louder (quieter) that makes it. More quieter than louder, so a near pass does not lean on the limiter.
    constexpr float c_throwNearerCloseness = 0.35f, c_throwNearerDb = 1.5f, c_throwFurtherDb = 4.0f;
    // A ridden throw (ThrowFlight::ridden): no track is taken further under its fader than this (-9 dB), however
    // deep the ride's dips are, so a whoosh never loses the sounds that carry it.
    constexpr float c_throwBalanceFloor = 0.355f;
    constexpr float c_reseedFadeSeconds = 0.005f;
    constexpr float c_flyMakeup = 1.23f; // +1.8 dB: Fly-By measured against Bed (integrated loudness, steady and gappy sources)
    // Head shadow: at full azimuth the far ear's air filter is this many octaves lower, but never under the floor (Hz).
    constexpr float c_shadowOctaves = 2.0f;
    constexpr float c_shadowFloor = 2500.0f;
    // Ride make-up: the ride depth the output stage is set up for (Energy at its default, 35%), how much of the
    // ride's average power the make-up takes back (the rest is the peaks, which the ride doesn't lower), and how
    // slowly a held Swell is followed (it lets go quickly).
    constexpr float c_makeupEnergy = 0.35f;
    constexpr float c_makeupSlope = 0.85f;
    constexpr float c_makeupSwellSeconds = 2.5f, c_makeupReleaseSeconds = 0.3f;
    // Follow Master: how fast the rides take up the Master's loudness (up, down), and how fast the option comes in.
    constexpr float c_masterUpSeconds = 0.015f, c_masterDownSeconds = 0.09f, c_followSeconds = 0.05f;
    constexpr float c_ropeFadeSeconds = 0.015f; // a change of morph rope: out, swap, back in
    constexpr float c_subTailSeconds = 0.5f;    // how long a sub's filters are left to ring out after its tracks stop
    constexpr float c_subKeySeconds = 0.02f;    // how fast a sub's Follow takes up what it goes by
    constexpr float c_subDriveFloor = 0.4f;     // a sub's Drive at 0%: its level (RMS) into the saturator
    constexpr float c_subDriveDb = 20.0f;       // and how much more gain goes in at 100%
    constexpr float c_subDriveMaxMatch = 8.0f;  // the most the level match after it may turn up
    constexpr float c_subPeakPasses = 2.16f;    // the peak a sub measures a bus's swell against falls back 12 dB per three passes
    constexpr float c_subBusSeconds = 0.1f;     // how fast it changes over between bus A, bus B and its own ride
    constexpr float c_stowFadeSeconds = 0.03f;  // a change of Stowaway's Mode: out, swap, back in
    constexpr float c_stowGateSeconds = 0.06f;  // Stowaway switched on or off: in, or out
    constexpr float c_busFadeSeconds = 0.01f;   // a track changing place: out, swap, back in
    // Wake. How far bus A has fallen dies away with this time constant, in pass lengths (so a trail lasts with the
    // passes), and the trail is fully open once A has fallen this far.
    constexpr float c_wakeTrailPasses = 0.25f;
    constexpr float c_wakeFallDb = 12.0f;
    constexpr float c_followMatchPasses = 3.0f;  // Follow's make-up (Sub and Wake): how slowly it learns what the knob costs, in pass lengths
    constexpr float c_wakeAmountSeconds = 0.03f; // how fast the trail opens and shuts
    constexpr float c_wakePanSeconds = 0.08f;    // how fast it moves to a new place
    constexpr float c_wakeLag = 0.5f;            // the trail travels this much of the way its pass has gone
    constexpr float c_stowShelfDb = 5.0f;        // Low End's lift at 100% (Wake)
    constexpr float c_tiltSeconds = 0.005f;      // a side of the Filter tilt coming in or going out
    constexpr float c_drySideTailSeconds = 0.05f; // how long Dry's side filter is left to ring out
    // Fly. Half a pass is never shorter or longer than this (seconds), whatever Follow asks for; under this level
    // nothing is taken to be coming in (-140 dBFS: a Fill echo dying away for ever must not keep it awake); and
    // its level against the other modes (measured: see StowawayTest).
    constexpr float c_flyHalfMin = 0.35f, c_flyHalfMax = 6.0f;
    constexpr float c_flyQuiet = 1.0e-7f;
    // Fly against the other modes, on top of its ride make-up (see renderFly): +1.3 dB. Measured: the same sound in
    // each mode over a minute and a half, on a drone, a bed and a gappy sound, with Energy at 0%, where it starts
    // and at 100% (see StowawayTest: "modes: all four").
    constexpr float c_flyLevel = 1.16f;
    constexpr std::uint32_t c_flySeed = 0x0F1Bu; // its draws start here, from the engine's seed
    // The room.
    constexpr float c_widthFadeSeconds = 0.005f; // between stereo and a room: out, swap, back in
    constexpr float c_pathSeconds = 0.25f;       // a change of Flight Path: the pass glides to its place on the new one
    constexpr float c_roomSeconds = 0.03f;       // Spread, Height and Centre Speaker glide this fast
    constexpr float c_hitsMoveSeconds = 0.004f;  // the hits moving to where they were aimed (they are there before they sound)
    constexpr float c_nearSpread = 0.5f;         // an object right at the listener bleeds this far into every speaker
    // Around: the closeness its level is worked out for (see orbitLoudness: every circle is as loud as a pass of
    // the middle closeness is on the other paths, whatever its own radius), and the Width at which a pass goes
    // exactly once round (the Width the buses open with).
    constexpr float c_orbitCloseness = 0.55f;
    constexpr float c_orbitFullWidth = 0.7f;
    constexpr float c_orbitBendDb = 9.8f; // Around: what the bend costs a crossing pass, in dB per speed squared (see orbitLoudness)
    // Height Varies: at 100% an Overhead pass may fly this much lower than its full arch (the other paths: from
    // ear level up to that arch).
    constexpr float c_archVaries = 0.25f;
    constexpr std::uint32_t c_placeSeed = 0x51DEu; // a room's own draws start here, from the engine's seed
    constexpr std::uint32_t c_hitSeed = 0x0417u;   // a Random hit's place: its draws start here, from the engine's seed
    constexpr float c_hitAlikeSeconds = 0.1f;      // how alike a hit's two sides are: measured over this long
    constexpr float c_hitCarrySeconds = 0.02f;     // a hit starting: what its files are as a whole counts as this much of it
    constexpr float c_bedLevelSeconds = 0.1f;      // a stereo bed's level against its mono sum: measured, and moved, over this long
    constexpr float c_hitRandomPan = 0.85f;        // Stereo: a Random hit goes this far to either side at most
    constexpr float c_hitSideTailSeconds = 0.05f;  // the hits' side lane is mixed this long after the last stereo hit track fed it
    // The part of a pass that is heard: within this much of its window's top (-10 dB). Its path is flown over that.
    constexpr float c_heardLevel = 0.3162f;
    constexpr float c_thirdTailSeconds = 0.25f;  // a bus's throw lane is left to ring out this long after the last throw
    constexpr float c_shareSeconds = 0.01f;      // how fast Low End's shares follow the lanes' levels
    constexpr float c_gainSeconds = 0.01f;       // a speaker's gain never goes from nothing to full faster than this
    constexpr float c_leavingSeconds = 0.25f;    // a narrower room: the speakers that have gone are left this long to fade out (their DC blockers settle slowly)
    constexpr float c_root2 = 1.41421356f;
    // Faders Together (design 3.1). A change of Ride Mode crossfades the rides over this long; Stagger at 100% fans
    // the tracks over this much of a swell either way; a bus's lead glides to the next pass's with this time
    // constant (settled in about a tenth of a second); the swell's size eases to each new draw as fast.
    constexpr float c_togetherSeconds = 0.5f;
    constexpr float c_staggerShare = 0.10f;
    constexpr float c_leadSeconds = 0.03f;
    // Faders Together: how long the swell's length takes to follow Rate (a one-pole on its logarithm). The passes on
    // the clock run faster or slower with it, and their doppler bend with them: over this long it is a glide, and a
    // modulator wired to Rate cannot wobble the pitch.
    constexpr float c_cycleSlewSeconds = 1.0f;
    constexpr std::uint32_t c_cycleSeed = 0xC1C7Eu; // the swell sizes: their draws start here, from the engine's seed
    // Auto Swell (design 3.7). On Peaks: the gate opens this long before a pass is at its closest, and stays open
    // at least until it is. (The Swell spring's step: 0.81 after 0.2 s, 0.9 after 0.233 s, all the way in by 0.3 s.)
    constexpr float c_autoPeakLead = 0.3f;
    constexpr std::uint32_t c_autoSeed = 0xA5E11u;  // Auto Swell's draws start here, from the engine's seed
    // Behind Swirl and Filter: each bus's Doubler and Filter rhythm draw from here (and the bus's number).
    constexpr std::uint32_t c_swirlSeed = 0x5F1A7u, c_sweepSeed = 0xF117Eu;
    constexpr float c_typeFadeSeconds = 0.03f; // a change of Swirl's Type: the bus out, and in again
    constexpr float c_swirlFeedbackMax = 0.95f; // the phaser's and the flanger's loops stay under 1 at either sign
    bool isRoom(room::Width w) { return w != room::Width::Mono && w != room::Width::Stereo; }

    float coef(float ms, float fs) { return 1.0f - std::exp(-1.0f / (0.001f * ms * fs)); }
    /** A one-pole smoothing coefficient for a step of `frames` samples: the same glide at every rate and chunk size. */
    float glide(int frames, float seconds, float fs) { return 1.0f - std::exp(-static_cast<float>(frames) / (seconds * fs)); }
}

float RigEngine::fillTapSeconds(int track, int tap)
{
    return c_fillTap[static_cast<size_t>(juce::jlimit(0, c_tracks - 1, track))] * c_fillRatios[static_cast<size_t>(juce::jlimit(0, 2, tap))];
}

float RigEngine::passWindow(float progress, float shape)
{
    const float u = sj::clamp(progress, 0.0f, 1.0f);
    const float bed = shape < 1.0f ? std::sin(sj::c_pi * u) : 0.0f;
    if (shape <= 0.0f)
        return bed;
    // Fly-By: raised cosines either side of the closest point (no step and no corner anywhere). The approach takes
    // the whole way in and rises late; the tail is over c_flyTail of the way out. The other pass of the bus is half
    // a pass away, so it is still far off (more than 20 dB down) while this one is at its loudest, and has taken
    // over by the time this one has gone: the bus dips between passes without going silent.
    float fly = 0.0f;
    if (u < 0.5f)
        fly = 0.5f - 0.5f * std::cos(sj::c_pi * std::pow(2.0f * u, c_flyApproach));
    else if (const float out = (2.0f * u - 1.0f) / c_flyTail; out < 1.0f)
        fly = 0.5f + 0.5f * std::cos(sj::c_pi * out);
    return bed + shape * (fly - bed);
}

float RigEngine::ridePower(float depth, float chaos, float exponent)
{
    // The same swell rideMakeup measures (see there), with `exponent` of its dips (in dB) kept.
    const float e = sj::clamp(depth, 0.0f, 1.0f);
    if (e <= 0.0f || exponent <= 0.0f)
        return 1.0f;
    float sum = 0.0f;
    const int points = 16;
    for (int j = 0; j < points; ++j)
    {
        const float lfo = 0.5f - 0.5f * std::cos(sj::c_twoPi * (static_cast<float>(j) + 0.5f) / static_cast<float>(points));
        const float r = std::pow(lfo, 1.0f + 1.85f * e) * (1.0f - 0.3f * chaos);
        sum += std::pow(std::max(1.0f - e * (1.0f - r), 1.0e-3f), 2.0f * exponent);
    }
    return sum / static_cast<float>(points);
}

float RigEngine::rideMakeup(float depth, float chaos)
{
    // The average power the ride leaves a fader at, over one swell (the same shape renderTracks rides with, at the
    // average swell size Chaos gives), against the same at the default depth. With the faders still, every track
    // sits at full level all the time, and the mix is that much hotter into the output stage.
    auto power = [chaos](float e)
    {
        float sum = 0.0f;
        const int points = 16;
        for (int j = 0; j < points; ++j)
        {
            const float lfo = 0.5f - 0.5f * std::cos(sj::c_twoPi * (static_cast<float>(j) + 0.5f) / static_cast<float>(points));
            const float r = std::pow(lfo, 1.0f + 1.85f * e) * (1.0f - 0.3f * chaos);
            const float g = 1.0f - e * (1.0f - r);
            sum += g * g;
        }
        return sum / static_cast<float>(points);
    };
    const float e = sj::clamp(depth, 0.0f, 1.0f);
    const float reference = ::Params::energyDepth(c_makeupEnergy);
    if (e >= reference)
        return 1.0f;
    return std::pow(power(reference) / power(e), 0.5f * c_makeupSlope);
}

//==============================================================================
float RigEngine::trackSpeed(int track) const
{
    const auto t = static_cast<size_t>(track);
    return m_variNow[t] ? std::exp2(m_trackPitchNow[t] / 12.0f) : 1.0f;
}

std::array<float, RigEngine::c_sections> RigEngine::takeSectionLevels()
{
    // Nothing rendered since the last call (the resampler can skip a block): keep the last levels.
    if (m_sectionSamples > 0)
        for (size_t k = 0; k < m_sectionLevels.size(); ++k)
            m_sectionLevels[k] = static_cast<float>(std::sqrt(m_sectionSquares[k] / m_sectionSamples));
    m_sectionSquares = {};
    m_sectionSamples = 0;
    return m_sectionLevels;
}

void RigEngine::prepare(double sampleRate, int maxBlockSize)
{
    juce::ignoreUnused(maxBlockSize);
    m_sampleRate = sampleRate;
    m_fs = static_cast<float>(sampleRate);
    juce::ignoreUnused(sj::SincKernel::get()); // the doppler read's table is built here, not on the audio thread
    for (auto& scope : m_scopes)
        scope.prepare(sampleRate);
    const float controlRate = m_fs / static_cast<float>(c_chunk);

    for (auto& s : m_levelSprings) s.prepare(controlRate, 2.5f, 0.85f);
    m_swellSpring.prepare(controlRate, 1.8f, 0.7f);
    m_energySpring.prepare(controlRate, 1.5f, 0.8f);
    m_pitchSpring.prepare(controlRate, 1.6f, 0.55f);
    m_morph.prepare(sampleRate, c_chunk);
    m_morphMix.prepare(controlRate, 3.0f, 1.0f);
    for (auto* s : { &m_fillSpring, &m_spaceSpring })
        s->prepare(controlRate, 2.0f, 0.8f);

    // Peak tremolo: a follower (5 ms up, 120 ms down) against a peak that falls back over ~3 s; the depth eases in
    // over ~15 ms and out over ~150 ms.
    m_tremAttack = 1.0f - std::exp(-1.0f / (0.005f * m_fs));
    m_tremRelease = 1.0f - std::exp(-1.0f / (0.12f * m_fs));
    m_tremPeakDecay = std::exp(-1.0f / (3.0f * m_fs));
    m_tremDepthUp = 1.0f - std::exp(-1.0f / (0.015f * m_fs));
    m_tremDepthDown = 1.0f - std::exp(-1.0f / (0.15f * m_fs));
    for (int t = 0; t < c_tracks; ++t)
    {
        m_shifters[static_cast<size_t>(t)].prepare(sampleRate);
        m_players[static_cast<size_t>(t)].prepare(sampleRate);
        m_fillLines[static_cast<size_t>(t)].prepare(static_cast<int>(c_fillLineSeconds * m_fs));
        for (int tap = 0; tap < 3; ++tap)
            m_fillTaps[static_cast<size_t>(t)][static_cast<size_t>(tap)] = std::min(fillTapSeconds(t, tap), c_fillLineSeconds - 0.01f) * m_fs;
        m_fillTone[static_cast<size_t>(t)].set(6000.0f, m_fs);
        m_rideDrift[static_cast<size_t>(t)].prepare(m_fs, 0.3f + 0.11f * static_cast<float>(t), rideSeed(t));
    }

    for (auto& b : m_buses)
    {
        auto* bus = &b;
        bus->line.prepare(static_cast<int>(c_lineSeconds * m_fs));
        bus->throwLine.prepare(static_cast<int>(c_lineSeconds * m_fs));
        if (bus == &m_buses[c_sideBus])
            bus->sideLine.prepare(static_cast<int>(c_lineSeconds * m_fs));
        bus->flL.prepare(static_cast<int>(0.02f * m_fs));
        bus->flR.prepare(static_cast<int>(0.02f * m_fs));
        bus->flT.prepare(static_cast<int>(0.02f * m_fs));
        bus->swirlFx.prepare(sampleRate);
        bus->sweep.prepare(sampleRate);
        bus->lowA.set(130.0f, 0.7071f, m_fs);
        bus->lowB.set(130.0f, 0.7071f, m_fs);
        bus->lowAdd.prepare(m_fs);
        bus->shelfL.set(110.0f, 0.7071f, m_fs);
        bus->shelfR.set(110.0f, 0.7071f, m_fs);
        bus->shelfT.set(110.0f, 0.7071f, m_fs);
        bus->follower.set(4.0f, 160.0f, m_fs);
        for (auto& s : bus->springs) s.prepare(controlRate, 2.2f, 0.6f);
        for (auto* d : { &bus->byBus, &bus->byHits })
            for (auto& band : d->band)
                band.set(1000.0f, 0.35f, m_fs);
    }

    for (auto& s : m_stow.springs) s.prepare(controlRate, 2.2f, 0.6f);
    for (auto& band : m_stow.byHits.band)
        band.set(1000.0f, 0.35f, m_fs);
    m_stow.follower.set(4.0f, 160.0f, m_fs);
    m_stow.sub.low.follower.set(5.0f, 90.0f, m_fs);
    m_stow.sub.lowHp.set(static_cast<float>(SubFilter::c_highpassHz), 0.7071f, m_fs);
    m_stow.wake.colour.prepare(m_fs);
    m_stow.fly.line.prepare(static_cast<int>(c_lineSeconds * m_fs));
    m_stow.fly.throwLine.prepare(static_cast<int>(c_lineSeconds * m_fs));
    m_stow.fly.colour.prepare(m_fs);
    m_stow.fly.throwColour.prepare(m_fs);

    m_detect.set(1.0f, 60.0f, m_fs);
    m_hitsFollower.set(2.0f, 120.0f, m_fs);
    m_mixFollower.set(10.0f, 300.0f, m_fs);

    m_echoL.prepare(static_cast<int>(1.2f * m_fs));
    m_echoR.prepare(static_cast<int>(1.2f * m_fs));
    m_echoLpL.set(4500.0f, m_fs); m_echoLpR.set(4500.0f, m_fs);
    m_echoHpL.set(220.0f, m_fs);  m_echoHpR.set(220.0f, m_fs);
    m_reverb.prepare(sampleRate);
    m_roomReverb.prepare(sampleRate);
    m_lfeLow.prepare(sampleRate);
    for (auto& f : m_lfePhase)
        f.prepare(sampleRate);
    for (auto& dc : m_dc)
        dc.set(8.0f, m_fs);
    m_subStem.assign(static_cast<size_t>(c_stemSamples), 0.0f);
    // The width asked for is the width from the start (a change while running fades: see stepWidth).
    m_width = m_widthWanted;
    m_room = isRoom(m_width);
    m_widthFade = 1.0f;
    m_liveChannels = room::layout(m_width).numChannels;
    m_liveHold = 0;

    m_rng.seed(m_seed);
    m_placeRng.seed(m_seed ^ c_placeSeed);
    m_stow.fly.rng.seed(m_seed ^ c_flySeed);
    m_hitRng.seed(m_seed ^ c_hitSeed);
    reset();
}

void RigEngine::reset()
{
    for (auto& p : m_players) p.reset();
    for (auto& t : m_trem) t = PeakTremolo {};
    for (size_t t = 0; t < m_shifters.size(); ++t)
    {
        m_trackPitchNow[t] = m_params.trackPitch[t];
        m_shifters[t].reset(std::exp2(m_trackPitchNow[t] / 12.0f), static_cast<float>(t) / static_cast<float>(c_tracks)); // staggered frames
        m_modeHold[t] = 0;
        m_shifterUsed[t] = false;
        m_variNow[t] = m_params.trackVari[t];
        m_modeGain[t] = 1.0f;
        m_trackSpeedView[t].store(trackSpeed(static_cast<int>(t)), std::memory_order_relaxed);
    }
    m_pendingHitRegion.fill(-1);
    m_beatViewWait = -1.0;
    m_sectionSquares = {};
    m_sectionSamples = 0;
    m_sectionLevels = {};
    for (auto& l : m_fillLines) l.reset();
    m_gain.fill(0.0f);
    m_throwGain.fill(0.0f);
    m_sideGain.fill(0.0f);
    m_throwOpen = 0.0f;
    m_throwBalance.fill(1.0f);
    m_throwBalanceWanted = false;
    for (auto& v : m_throws) v = ThrowVoice {};
    for (auto& q : m_throwQueue) q = QueuedThrow {};
    m_throwPeakView.store(-1.0f, std::memory_order_relaxed);
    m_restartPending = false;
    m_releaseSnap = true;
    m_coverageFor.fill(nullptr);
    m_ridePhase.fill(0.0);
    m_rideDepth.fill(0.0f);
    for (size_t t = 0; t < m_ridePhase.size(); ++t)
        m_ridePhase[t] = 0.17 * static_cast<double>(t); // start the rides apart
    // Faders Together's clock from where setCycleStart says (the first chunk places it by Peak At), its draws and
    // Auto Swell's from their own seeds. Auto Swell's first one comes a whole Every after the start.
    m_cycleRng.seed(m_seed ^ c_cycleSeed);
    m_autoRng.seed(m_seed ^ c_autoSeed);
    m_cycle = Cycle {};
    m_cycle.phase = m_cycleStart;
    m_cycle.size = 1.0f - 0.3f * sj::clamp(m_params.chaos, 0.0f, 1.0f) * m_cycleRng.unipolar();
    m_cycleSizeNow = m_cycle.size;
    m_cycleStartPending = true;
    m_cycleSeconds = nominalPassSeconds();
    m_cyclePeakAt = sj::clamp(m_params.together.peakAt, 0.2f, 0.8f);
    m_cycleX0 = cycleX();
    m_cycleDX = 0.0;
    m_cycleSwell = m_cycleStart;
    m_cycleAimed = -1;
    m_togetherMix = m_params.rideMode == RideTogether ? 1.0f : 0.0f;
    m_autoSwell = AutoSwellState {};
    m_autoSwell.fires = false;
    m_cyclePeakView.store(-1.0f, std::memory_order_relaxed);
    m_autoSwellView.store(false, std::memory_order_relaxed);
    for (auto& v : m_varispeedView)
        v.store(0.0f, std::memory_order_relaxed);

    for (size_t i = 0; i < m_buses.size(); ++i)
    {
        auto& bus = m_buses[i];
        bus.line.reset();
        bus.throwLine.reset();
        bus.sideLine.reset();
        bus.sideHold = 0;
        bus.sideAmount = 0.0f;
        bus.sidePre = bus.sidePost = 0.0f;
        bus.sideMatch = 1.0f;
        bus.bedMid = bus.bedSide = 0.0f;
        bus.bedGain = 1.0f;
        bus.flL.reset();
        bus.flR.reset();
        bus.resetThird();
        bus.lanes = 2;
        bus.lowShare = { 0.5f, 0.5f, 0.0f };
        bus.placed = false;
        bus.path.snap(juce::jlimit(0, room::c_numPaths - 1, m_params.buses[(m_params.linkBuses && i > 0) ? 0 : i].path));
        bus.env = 0.0f;
        bus.duck = 0.0f;
        bus.peakRef = 1.0e-3f;
        bus.swirlOn = false;
        // Behind Swirl and Filter: the type asked for is the type from the start (no fade), the Tape Flanger's
        // held offset back in the middle, the Doubler's and the Filter rhythm's draws from their own seeds.
        bus.swirlType = std::clamp(m_params.swirlDetail[i].type, 0, static_cast<int>(c_swirlTypes) - 1);
        bus.typeFade = 1.0f;
        bus.typeFadeIn = false;
        bus.swirlFx.reset();
        bus.swirlFx.setSeed(m_seed ^ c_swirlSeed ^ static_cast<std::uint32_t>(i + 1));
        bus.sweep.reset();
        bus.sweep.setSeed(m_seed ^ c_sweepSeed ^ static_cast<std::uint32_t>(i + 1));
        bus.lowAdd.armed = false;
        bus.passShape = m_params.buses[(m_params.linkBuses && i > 0) ? 0 : i].passMode == 1 ? 1.0f : 0.0f;
        bus.prox = 1.0f;
        // The second pass starts half a pass from the first (at its closest point), and stays there.
        startPass(bus.heads[0], m_params.buses[i]);
        startPass(bus.heads[1], m_params.buses[i], bus.heads[0].lenIn);
        bus.heads[1].time = static_cast<double>(bus.heads[1].lenIn);
        for (auto& h : bus.heads)
        {
            h.clockSeconds = 0.0f;
            h.pace = 1.0f;
        }
        // Faders Together: the passes start on the clock (the same draws, a swell each way).
        if (m_params.rideMode == RideTogether)
            placeOnClock(bus);
        // Linked: B starts on A's Doppler and Width and on A's passes (A was set up just before it).
        auto snapped = m_params.buses[i];
        if (m_params.linkBuses && i > 0)
        {
            snapped.doppler = m_params.buses[0].doppler;
            snapped.width = m_params.buses[0].width;
            for (size_t k = 0; k < bus.heads.size(); ++k)
                followPass(bus.heads[k], m_buses[0].heads[k], snapped.bend, true);
        }
        bus.leadTarget = passLead(bus.heads[0]);
        bus.lead = bus.leadTarget / nominalPassSeconds();
        snapBus(bus, snapped);
    }
    m_linkNow = m_params.linkBuses;
    m_linkFade = 1.0f;

    {
        auto& s = m_stow;
        const float v[c_stowKnobs] = { m_params.stow.follow, m_params.stow.low, m_params.stow.filter, m_params.stow.drive, m_params.stow.width };
        for (size_t k = 0; k < s.springs.size(); ++k)
            s.springs[k].snap(v[k]);
        s.now = m_params.stow;
        s.mode = s.now.mode = juce::jlimit(0, c_stowModes - 1, m_params.stow.mode);
        s.fade = 1.0f;
        s.gate = m_params.stow.on ? 1.0f : 0.0f;
        s.fed = s.sideFed = s.sounding = s.ducked = false;
        s.pan = s.panFrom = 0.0f;
        s.placed = false;
        s.path.snap(juce::jlimit(0, room::c_numPaths - 1, m_params.stow.path));
        s.throwKey = 0.0f;
        s.byHits.reset();
        s.env = s.follower.env = 0.0f;
        s.sub.reset();
        s.wake.reset();
        s.dry.reset();
        s.fly.reset();
    }
    m_busNow.fill(-1);
    m_busFade.fill(1.0f);

    m_echoL.reset(); m_echoR.reset();
    m_echoLpL.reset(); m_echoLpR.reset(); m_echoHpL.reset(); m_echoHpR.reset();
    m_reverb.reset();
    m_roomReverb.reset();
    m_spaceLean.fill(0.0f);
    m_roomTapsSnap = true;
    m_echoSend = 1.0f;
    m_spaceOn = false;
    m_lfeLow.reset();
    for (auto& f : m_lfePhase)
        f.reset();
    m_lfeSnap = true;
    m_lfeMatchSnap = true;
    m_lfeChannel = room::layout(m_width).lfe;
    m_stem.fill(0.0f);
    m_echoDriftL = 0.0f; m_echoDriftR = 0.5f; // the same start every time, so a bounce repeats
    m_spaceDry = 1.0f; m_spaceWet = 0.0f;
    for (auto& dc : m_dc)
        dc.reset();
    m_spreadNow = sj::clamp(m_params.roomSpread, 0.0f, 1.0f);
    m_heightNow = sj::clamp(m_params.roomHeight, 0.0f, 1.0f);
    m_variesNow = sj::clamp(m_params.roomHeightVaries, 0.0f, 1.0f);
    m_centreNow = m_params.roomCentre ? 1.0f : 0.0f;
    m_roomSnap = true;
    m_spaceFoldSet = false;
    m_hitsAt = m_hitsTo = room::Position {};
    m_hitsDistance = m_hitsDistanceTo = 1.0f;
    m_hitFollow = HitFollow {};
    m_hitStarted = m_hitWantsLoudest = false;
    m_hitTurn = m_hitHigh = 0.0f; // (a Random hit draws its place as it starts)
    m_hitSpreadNow = sj::clamp(m_params.hitSpread, 0.0f, 1.0f);
    m_hitMidPower = m_hitSidePower = 0.0f;
    m_hitMidShare = 1.0f;
    m_hitsFileMid = m_hitsFileAll = 0.0f;
    m_hitOnset = false;
    m_hitSince = 0;
    m_hitLevel = 1.0f;
    m_hitsPlaced = false;
    m_hitsSideFed = false;
    m_hitsSideHold = 0;
    m_objectLevel.fill(0.0f);
    m_gate = m_hold = 0.0f;
    m_peakRef = 1.0e-3f;
    m_slowEnv = 0.0f;
    m_talker = 0;
    m_talkerHold = 0.0f;
    for (auto& bus : m_buses)
        for (auto* d : { &bus.byBus, &bus.byHits })
            d->reset();
    m_energyModNow = m_params.energyMod;
    m_pitchModNow = m_params.pitchMod;
    m_fillModNow = m_params.fillMod;
    m_spaceModNow = m_params.spaceMod;
    m_busModNow = m_params.busMod;
    m_stowModNow = m_params.stowMod;

    for (size_t t = 0; t < m_levelSprings.size(); ++t)
        m_levelSprings[t].snap(m_params.levels[t]);
    m_swellSpring.snap(m_params.swell ? 1.0f : 0.0f);
    m_energySpring.snap(m_params.rideOn ? m_params.energy : 0.0f);
    {
        const float depth = ::Params::energyDepth(sj::clamp(m_energySpring.value + (m_params.rideOn ? m_params.energyMod : 0.0f), 0.0f, 1.0f));
        m_makeupSwell = m_params.swell ? 1.0f : 0.0f;
        m_proxBlend = (1.0f - depth * (1.0f - m_makeupSwell)) * (1.0f - depth * (1.0f - m_makeupSwell));
        m_makeupDepth = depth * (1.0f - m_makeupSwell);
        m_makeupChaos = m_params.chaos;
        m_makeupTogether = m_togetherMix;
        m_makeup = m_makeupTarget = rideMakeup(m_makeupDepth * (1.0f - m_makeupTogether), m_makeupChaos);
    }
    m_pitchSpring.snap(m_params.pitch);
    m_fillSpring.snap(m_params.fill);
    m_spaceSpring.snap(m_params.space);
    {
        SpectralMorph::Params mp;
        mp.x = m_params.morphX;
        mp.y = m_params.morphY;
        mp.mode = juce::jlimit(0, SpectralMorph::NumModes - 1, m_params.morphMode);
        m_morph.setParams(mp);
    }
    m_morph.reset(m_params.morphOn);
    m_morphMix.snap(m_params.morphOn ? 1.0f : 0.0f);
    for (int side = 0; side < 2; ++side)
    {
        m_ropeNow[static_cast<size_t>(side)] = wantedRope(side);
        m_ropeFade[static_cast<size_t>(side)] = 1.0f;
        m_ropeFading[static_cast<size_t>(side)] = 0;
        m_ropeView[static_cast<size_t>(side)].store(m_ropeNow[static_cast<size_t>(side)], std::memory_order_relaxed);
        m_morphTap[static_cast<size_t>(side)].fill(0.0f);
    }
    m_masterFollow = {};
    m_masterRide = 0.0f;
    {
        // Follow Master starts in only when there is a Master to follow.
        const int master = m_cargo != nullptr ? m_cargo->getMaster() : -1;
        m_followMix = m_params.rideMode == RideFollowMaster && master >= 0 && m_cargo->getLive(master) != nullptr ? 1.0f : 0.0f;
    }
    m_masterView.store(0.0f, std::memory_order_relaxed);
    letGoOfCargo();
}

void RigEngine::letGoOfCargo()
{
    // The players hold no sample data now. Say so, or the marks of the last block would keep replaced data alive
    // for as long as the engine isn't run again.
    for (auto& p : m_players)
        p.reset();
    m_gain.fill(0.0f);
    m_throwGain.fill(0.0f);
    m_sideGain.fill(0.0f);
    if (m_cargo != nullptr)
        for (int t = 0; t < c_tracks; ++t)
            m_cargo->markInUse(t, nullptr, nullptr);
}

void RigEngine::reseedNow()
{
    // The same start every time: the random sources from their seeds, every delay and tail empty, every glide at
    // its target, the passes and the rides from the top.
    m_rng.seed(m_seed);
    m_placeRng.seed(m_seed ^ c_placeSeed);
    m_stow.fly.rng.seed(m_seed ^ c_flySeed);
    m_hitRng.seed(m_seed ^ c_hitSeed);
    for (int t = 0; t < c_tracks; ++t)
        m_rideDrift[static_cast<size_t>(t)].prepare(m_fs, 0.3f + 0.11f * static_cast<float>(t), rideSeed(t));
    const auto waitingThrows = m_throwQueue; // a throw asked for with the restart belongs to the new run
    reset();
    m_throwQueue = waitingThrows;
    for (auto& bus : m_buses)
    {
        for (auto* f : { &bus.lowA, &bus.lowB, &bus.shelfL, &bus.shelfR, &bus.keyL, &bus.keyR })
            f->reset();
        bus.lowAdd.reset();
        for (auto& h : bus.heads) { h.airL.reset(); h.airR.reset(); h.airSL.reset(); h.airSR.reset(); }
        for (auto& a : bus.phL) a = {};
        for (auto& a : bus.phR) a = {};
        bus.phFbL = bus.phFbR = bus.lfo = bus.flFbL = bus.flFbR = 0.0f;
        bus.swirlPre = bus.swirlPost = bus.drivePre = bus.drivePost = 0.0f;
        bus.swirlGain = bus.driveGain = 1.0f;
        bus.toneL.reset(); bus.toneR.reset();
        bus.follower.env = 0.0f;
    }
    // A change of width in progress is over with the restart: the new run starts at the wanted width.
    m_width = m_widthWanted;
    m_room = isRoom(m_width);
    m_liveChannels = room::layout(m_width).numChannels;
    m_liveHold = 0;
    for (auto& f : m_fillTone) f.reset();
    m_detect.env = m_hitsFollower.env = m_mixFollower.env = 0.0f;
    m_average = 0.0f;
    m_hitClock = 0.0;
    m_aimClock = 0.0;
    m_lastAimTarget = -1.0e12;
    m_lastHitTiming = -1;
    m_hitMarks.fill(-1.0);
    m_releaseGain = 0.0f;
    m_releaseSnap = false;
    m_reseedPending = false;
}

void RigEngine::throwWhoosh(int delaySamples, float velocity)
{
    throwWhoosh(delaySamples, velocity, ThrowFlight {});
}

void RigEngine::throwWhoosh(int delaySamples, float velocity, const ThrowFlight& flight)
{
    for (auto& q : m_throwQueue)
        if (! q.pending)
        {
            q.pending = true;
            q.delay = std::max(0, delaySamples);
            q.velocity = velocity;
            q.flight = flight;
            return;
        }
}

RigEngine::Places RigEngine::getPlaces() const
{
    Places places;
    places.players = m_players;
    places.ridePhase = m_ridePhase;
    places.rideDepth = m_rideDepth;
    places.cycle = m_cycle;
    places.autoSwell = m_autoSwell;
    return places;
}

void RigEngine::setPlaces(const Places& places)
{
    m_players = places.players;
    for (auto& p : m_players)
        p.letGoOfShots();
    m_ridePhase = places.ridePhase;
    m_rideDepth = places.rideDepth;
    // The clock carries on from where it was (setCycleStart after this moves it within its swell), and the passes
    // that are on it are put where it stands: nothing has sounded yet.
    m_cycle = places.cycle;
    m_cycleSizeNow = m_cycle.size;
    m_cycleStartPending = false;
    m_cycleAimed = -1;
    m_cycleX0 = cycleX();
    m_cycleSeconds = nominalPassSeconds();
    for (auto& bus : m_buses)
        if (bus.heads[0].clockSeconds > 0.0f)
            placeOnClock(bus);
    m_autoSwell = places.autoSwell;
    if (m_cargo != nullptr) // the data the players read again is in use again
        for (int t = 0; t < c_tracks; ++t)
            m_cargo->markInUse(t, m_players[static_cast<size_t>(t)].getData(), m_players[static_cast<size_t>(t)].getPending());
}

void RigEngine::scatterRide(std::uint32_t seed)
{
    sj::Random draw;
    draw.seed(seed);
    for (size_t t = 0; t < m_ridePhase.size(); ++t)
    {
        m_ridePhase[t] = static_cast<double>(draw.unipolar());
        m_rideDepth[t] = draw.unipolar();
    }
    // Auto Swell on a clock: from a moment of its own in its period (the next one comes when the period is over).
    m_autoSwell = AutoSwellState {};
    m_autoSwell.fires = false;
    m_autoSwell.clock = static_cast<double>(draw.unipolar() * std::max(0.05f, m_params.autoSwell.every));
    // Behind Swirl and Filter: each bus's own sweeps (Tape Flanger On Its Own, the Doubler's waves, the Filter's
    // rhythm) from a moment of their own.
    for (auto& bus : m_buses)
    {
        bus.swirlFx.scatter(draw.unipolar());
        bus.sweep.scatter(draw.unipolar());
    }
}

bool RigEngine::isThrowing() const
{
    for (const auto& q : m_throwQueue)
        if (q.pending)
            return true;
    for (const auto& v : m_throws)
        if (v.active)
            return true;
    return m_throwOpen > 0.0f;
}

void RigEngine::startThrows(int frames)
{
    for (auto& q : m_throwQueue)
    {
        if (! q.pending)
            continue;
        if (q.delay >= frames)
        {
            q.delay -= frames;
            continue;
        }
        q.pending = false;
        size_t voice = m_throws.size();
        for (size_t v = 0; v < m_throws.size() && voice == m_throws.size(); ++v)
            if (! m_throws[v].active)
                voice = v;
        if (voice == m_throws.size())
            continue; // every voice is in flight

        // One pass, the same on every bus (bus A's Doppler; each bus bends it by its own Bend), as long as Throw
        // Length says: half of it is the approach (or the flight's share), so the peak is heard that far from now.
        auto knobNow = [this](size_t bus, size_t k) { return sj::clamp(m_buses[bus].springs[k].value + m_busModNow[bus][k], 0.0f, 1.0f); };
        BusParams lead;
        lead.doppler = knobNow(0, 0);
        lead.bend = knobNow(0, 2);
        const auto& a = m_params.buses[0]; // bus A's Direction and Side (and the modulators wired to them)
        lead.direction = a.direction;
        lead.directionWired = a.directionWired;
        lead.directionMod = a.directionMod;
        lead.side = a.side;
        lead.sideWired = a.sideWired;
        lead.sideMod = a.sideMod;
        const float seconds = ::Params::throwSeconds(m_params.throwSeconds, m_params.rate);
        const auto& flight = q.flight;
        auto& first = m_buses[0].throwHeads[voice];
        startPass(first, lead, sj::clamp(flight.approach, 0.2f, 0.8f) * seconds);
        first.lenOut = seconds - first.lenIn;
        first.length = seconds;
        // The flight's own way, side and height, where bus A leaves them to the pass.
        if (flight.direction != 0 && lead.direction == 0 && ! lead.directionWired)
            first.dir = flight.direction > 0 ? 1.0f : -1.0f;
        if (flight.side != 0 && lead.side == 0 && ! lead.sideWired)
            first.side = flight.side > 0 ? 1.0f : -1.0f;
        if (flight.lift >= 0.0f)
            first.lift = sj::clamp(flight.lift, 0.0f, 1.0f);
        auto& v = m_throws[voice];
        v = ThrowVoice {};
        if (q.velocity >= 0.0f)
        {
            const float vel = sj::clamp(q.velocity, 0.0f, 1.0f);
            first.closeness = sj::lerp(c_throwSoftCloseness, 1.0f, vel);
            first.closest = 0.004f * std::exp2(5.0f * (1.0f - first.closeness));
            v.level = sj::lerp(c_throwSoftLevel, 1.0f, vel);
        }
        else if (const float nearer = sj::clamp(flight.nearer, -1.0f, 1.0f); std::abs(nearer) > 0.0f)
        {
            // Nearer or further off than the bus drew it: closer is louder, brighter and bends more steeply.
            first.closeness = sj::clamp(first.closeness + c_throwNearerCloseness * nearer, 0.05f, 1.0f);
            first.closest = 0.004f * std::exp2(5.0f * (1.0f - first.closeness));
            v.level = sj::dbToGain(nearer * (nearer > 0.0f ? c_throwNearerDb : c_throwFurtherDb));
        }
        // Ridden: the tracks' balance is taken from the ride on the next chunk; otherwise every track at its fader.
        m_throwBalanceWanted = flight.ridden;
        if (! flight.ridden)
            m_throwBalance.fill(1.0f);
        const double wait = static_cast<double>(q.delay) / m_sampleRate;
        first.time = -wait;
        for (size_t b = 1; b < m_buses.size(); ++b)
            followPass(m_buses[b].throwHeads[voice], first, knobNow(b, 2), true);
        for (auto& bus : m_buses)
        {
            bus.throwHeads[voice].airL.reset();
            bus.throwHeads[voice].airR.reset();
        }
        // Stowaway's Fly flies the throw too (see renderFly: it takes the pass from bus A's copy as it goes).
        m_stow.fly.throwHeads[voice].airL.reset();
        m_stow.fly.throwHeads[voice].airR.reset();
        v.active = true;
        v.gate = wait + static_cast<double>(first.lenIn + c_flyTail * first.lenOut) + 0.02;
        m_throwCountView.fetch_add(1, std::memory_order_relaxed);
    }
}

void RigEngine::setParams(const Params& p)
{
    m_params = p;
    for (size_t t = 0; t < m_levelSprings.size(); ++t)
        m_levelSprings[t].target = p.levels[t];
    m_swellSpring.target = p.swell ? 1.0f : 0.0f;
    m_energySpring.target = p.rideOn ? p.energy : 0.0f;
    m_pitchSpring.target = p.pitch;
    m_fillSpring.target = p.fill;
    m_spaceSpring.target = p.space;
    m_morphMix.target = p.morphOn ? 1.0f : 0.0f;

    for (size_t i = 0; i < m_buses.size(); ++i)
    {
        // Linked: bus B takes bus A's Doppler and Width (it keeps its own settings for when they're unlinked).
        const auto& b = p.buses[i];
        const auto& lead = (p.linkBuses && i > 0) ? p.buses[0] : b;
        const float v[9] = { lead.doppler, lead.width, b.bend, b.low, b.swirl, b.filter, b.drive, b.duck, b.key };
        for (size_t k = 0; k < 9; ++k)
            m_buses[i].springs[k].target = v[k];
    }
    const float stow[c_stowKnobs] = { p.stow.follow, p.stow.low, p.stow.filter, p.stow.drive, p.stow.width };
    for (size_t k = 0; k < m_stow.springs.size(); ++k)
        m_stow.springs[k].target = stow[k];
}

void RigEngine::snapBus(BusState& bus, const BusParams& b)
{
    const float v[9] = { b.doppler, b.width, b.bend, b.low, b.swirl, b.filter, b.drive, b.duck, b.key };
    for (size_t k = 0; k < 9; ++k)
        bus.springs[k].snap(v[k]);
}

float RigEngine::getLatencySamples() const { return 0.0f; }

void RigEngine::glideMods(int frames)
{
    const float a = glide(frames, 0.015f, m_fs); // ~15 ms
    m_energyModNow += (m_params.energyMod - m_energyModNow) * a;
    m_pitchModNow += (m_params.pitchMod - m_pitchModNow) * a;
    m_fillModNow += (m_params.fillMod - m_fillModNow) * a;
    m_spaceModNow += (m_params.spaceMod - m_spaceModNow) * a;
    for (size_t b = 0; b < m_busModNow.size(); ++b)
        for (size_t k = 0; k < m_busModNow[b].size(); ++k)
            m_busModNow[b][k] += (m_params.busMod[b][k] - m_busModNow[b][k]) * a;
    for (size_t k = 0; k < m_stowModNow.size(); ++k)
        m_stowModNow[k] += (m_params.stowMod[k] - m_stowModNow[k]) * a;
}

void RigEngine::restart()
{
    // Play pressed: every track starts again from its start marker, the rides from the top. Done by renderTracks
    // once the sources are silent: straight away from rest, after a few milliseconds' fade if they are still
    // sounding (Play again within Stop's 60 ms let-go, or a restart while playing), so nothing is cut mid-wave.
    m_restartPending = true;
}

void RigEngine::restartNow()
{
    for (auto& p : m_players)
        p.reset();
    for (size_t t = 0; t < m_ridePhase.size(); ++t)
        m_ridePhase[t] = 0.17 * static_cast<double>(t);
    m_gain.fill(0.0f);
    m_releaseGain = 0.0f;
    m_restartPending = false;
}

RigEngine::FlightView RigEngine::getFlightView(int index) const
{
    FlightView v;
    const auto i = static_cast<size_t>(juce::jlimit(0, c_maxFlights - 1, index));
    v.env = m_viewData[i * 4 + 0].load(std::memory_order_relaxed);
    v.x = m_viewData[i * 4 + 1].load(std::memory_order_relaxed);
    v.closeness = m_viewData[i * 4 + 2].load(std::memory_order_relaxed);
    v.age = m_viewData[i * 4 + 3].load(std::memory_order_relaxed);
    v.active = v.env > 0.01f;
    return v;
}

RigEngine::FlightView RigEngine::getStowFlight(int head) const
{
    FlightView v;
    const auto i = static_cast<size_t>(juce::jlimit(0, 1, head));
    v.env = m_stowFlightView[i * 4 + 0].load(std::memory_order_relaxed);
    v.x = m_stowFlightView[i * 4 + 1].load(std::memory_order_relaxed);
    v.closeness = m_stowFlightView[i * 4 + 2].load(std::memory_order_relaxed);
    v.age = m_stowFlightView[i * 4 + 3].load(std::memory_order_relaxed);
    v.active = v.env > 0.01f;
    return v;
}

//==============================================================================
float RigEngine::nominalPassSeconds() const
{
    // Pass length follows Rate exponentially (6 s .. 1.6 s), like the ride speed.
    return ::Params::passSeconds(m_params.rate);
}

namespace
{
    /** The fastest a pass may fly: half the speed of sound (an octave up on the way in), and no faster than keeps
        the delay at the start of the approach inside the line. */
    float machLimit(float lenIn) { return std::min(0.5f, c_maxPassDelay / (lenIn + c_maxPassDelay)); }
}

void RigEngine::startPass(Head& h, const BusParams& p, float lenIn, sj::Random* rng)
{
    // One pass of the moving source. Speed, distance and direction are randomly
    // automated (doppler speed etc.).
    auto& draw = rng != nullptr ? *rng : m_rng;
    const float c = m_params.chaos;
    const float d = sj::clamp(p.doppler, 0.0f, 1.0f);
    h.time = 0.0;
    if (lenIn < 0.0f)
    {
        // The bus's first head draws the length of the pass (Chaos varies it).
        h.length = sj::clamp(nominalPassSeconds() * std::exp2(c * 0.5f * draw.gauss()), 0.8f, 8.0f);
        h.lenIn = h.lenOut = 0.5f * h.length;
    }
    else
    {
        // The second head approaches over the rest of the first head's pass; its way out is set when the first
        // head draws its next pass (until then, the same again).
        h.lenIn = h.lenOut = lenIn;
        h.length = 2.0f * lenIn;
    }
    // Bend scales the speed the delay moves at (= the doppler pitch bend) and nothing else: the pass swells, brightens
    // and pans by its own speed (machRef), as set by Doppler and Width.
    h.machBase = 0.42f * std::pow(d, 1.3f) * (1.0f + c * 0.6f * draw.bipolar());
    h.machRef = sj::clamp(h.machBase, 0.0f, machLimit(h.lenIn));
    h.mach = sj::clamp(h.machBase * sj::clamp(p.bend, 0.0f, 1.0f), 0.0f, machLimit(h.lenIn));
    h.closeness = sj::clamp(0.55f + 0.45f * d * draw.bipolar() * (0.4f + c), 0.0f, 1.0f);
    h.closest = 0.004f * std::exp2(5.0f * (1.0f - h.closeness));
    // Which way: as Direction says, or as the modulator wired to it stands right now; left to itself each pass
    // draws its own (the draw is made whatever decides, so every other draw stays what it is).
    const float drawn = draw.bipolar() >= 0.0f ? 1.0f : -1.0f;
    h.dir = p.directionWired ? (p.directionMod >= 0.0f ? 1.0f : -1.0f) : p.direction == 1 ? 1.0f : p.direction == 2 ? -1.0f : drawn;
    // A room: which side it takes and how high it flies, pass by pass, from draws of their own.
    const float sideDrawn = m_placeRng.bipolar() >= 0.0f ? 1.0f : -1.0f;
    h.side = p.sideWired ? (p.sideMod >= 0.0f ? -1.0f : 1.0f) : p.side == 1 ? -1.0f : p.side == 2 ? 1.0f : sideDrawn;
    h.lift = m_placeRng.unipolar();
    h.aimed = h.swelled = false;
}

//==============================================================================
// Faders Together: the cycle clock (design 3.1).
float RigEngine::tapeDelaySeconds(const BusState& bus) const
{
    return bus.swirlType == SwirlTape ? static_cast<float>(bus.swirlFx.dryDelaySamples()) / m_fs : 0.0f;
}

float RigEngine::passLead(const Head& h)
{
    const float v = sj::clamp(h.mach, 0.0f, 0.95f);
    return c_baseDelay + h.closest / std::sqrt(1.0f - v * v);
}

void RigEngine::placeOnClock(BusState& bus)
{
    const double x = cycleX();
    const float seconds = m_cycleSeconds;
    for (size_t k = 0; k < bus.heads.size(); ++k)
    {
        auto& h = bus.heads[k];
        h.clockPeak = std::floor(x) + (k == 0 ? 1.0 : 0.0);
        h.clockSeconds = seconds;
        h.pace = 1.0f;
        h.lenIn = h.lenOut = seconds;
        h.length = 2.0f * seconds;
        h.machRef = sj::clamp(h.machBase, 0.0f, machLimit(h.lenIn));
        h.mach = std::min(h.mach, machLimit(h.lenIn));
        h.time = static_cast<double>(h.lenIn) + (x - h.clockPeak) * static_cast<double>(h.clockSeconds);
    }
    bus.leadTarget = passLead(bus.heads[0]);
    bus.lead = bus.leadTarget / seconds;
}

void RigEngine::stepCycle(int frames)
{
    // The swell's length follows Rate over about a second (at once on a start): see c_cycleSlewSeconds.
    const float wanted = nominalPassSeconds();
    if (m_cycleStartPending || ! (m_cycleSeconds > 0.0f))
        m_cycleSeconds = wanted;
    else if (std::abs(m_cycleSeconds - wanted) < 1.0e-5f * wanted)
        m_cycleSeconds = wanted;
    else
        m_cycleSeconds *= std::exp(std::log(wanted / m_cycleSeconds) * glide(frames, c_cycleSlewSeconds, m_fs));
    const float seconds = m_cycleSeconds;
    const float peakAt = sj::clamp(m_params.together.peakAt, 0.2f, 0.8f);
    m_cyclePeakAt = peakAt;
    if (m_cycleStartPending)
    {
        // The clock's peak is at 0.5 and the swell's trough Peak At before it: start that far into the swell.
        m_cycleStartPending = false;
        const double phase = m_cycleStart + 0.5 - static_cast<double>(peakAt);
        m_cycle.phase = phase - std::floor(phase);
        m_cycleSwell = m_cycleStart;
        for (size_t b = 0; b < m_buses.size(); ++b)
        {
            auto& bus = m_buses[b];
            if (bus.heads[0].clockSeconds <= 0.0f)
                continue;
            if (b > 0 && m_linkNow)
            {
                for (size_t k = 0; k < bus.heads.size(); ++k)
                    followPass(bus.heads[k], m_buses[0].heads[k], sj::clamp(bus.springs[2].value + m_busModNow[b][2], 0.0f, 1.0f), true);
                bus.leadTarget = passLead(bus.heads[0]);
                bus.lead = bus.leadTarget / seconds;
            }
            else
                placeOnClock(bus);
        }
    }
    m_cycleX0 = cycleX();
    m_cycleDX = 1.0 / (m_sampleRate * static_cast<double>(seconds));
    const double before = m_cycle.phase;
    double phase = before + static_cast<double>(frames) * m_cycleDX;
    if (before < 0.5 && phase >= 0.5)
        ++m_cycle.peaks;
    if (phase >= 1.0)
        phase -= 1.0;
    m_cycle.phase = phase;

    // A new swell starts at the trough: its size (Chaos), and each bus's lead for the pass now on its way in. The
    // faders are at their lowest there, so neither is heard to change.
    double swell = phase - (0.5 - static_cast<double>(peakAt));
    swell -= std::floor(swell);
    const bool newSwell = swell < m_cycleSwell - 0.5;
    m_cycleSwell = swell;
    if (newSwell)
        m_cycle.size = 1.0f - 0.3f * sj::clamp(m_params.chaos, 0.0f, 1.0f) * m_cycleRng.unipolar();
    m_cycleSizeNow += (m_cycle.size - m_cycleSizeNow) * glide(frames, c_leadSeconds, m_fs);
    const double x = cycleX();
    const float morphLead = m_morphMixNow > 0.0f || m_morph.isRunning() ? static_cast<float>(m_morph.getLatencySamples()) / m_fs : 0.0f;
    for (auto& bus : m_buses)
    {
        if (newSwell)
            for (const auto& h : bus.heads)
                if (h.clockSeconds > 0.0f && h.clockPeak > x && h.clockPeak <= x + 1.0)
                    bus.leadTarget = passLead(h);
        // (Glided as a share of the swell, so a Rate that jumps does not step the faders.)
        // (A Tape Flanger's clean path is 8 ms long: the bus's faders are that much further ahead.)
        bus.lead += ((bus.leadTarget + morphLead + tapeDelaySeconds(bus)) / seconds - bus.lead) * glide(frames, c_leadSeconds, m_fs);
    }
    const bool together = m_params.rideMode == RideTogether;
    m_cyclePeakView.store(together ? static_cast<float>((std::floor(x) + 1.0 - x) * static_cast<double>(seconds)) : -1.0f, std::memory_order_relaxed);
    m_cycleSecondsView.store(seconds, std::memory_order_relaxed);
}

bool RigEngine::stepAutoSwell(float seconds, float energyKnob)
{
    auto& a = m_autoSwell;
    const auto& s = m_params.autoSwell;
    // It rests while Auto Ride is off or Energy is at nothing (there is no ride to swell over), and while the rig
    // is stopped. On Peaks rests in Faders Together: the faders are at the top on every peak there already.
    const bool together = m_params.rideMode == RideTogether;
    const bool runs = s.mode != AutoSwellOff && m_params.rideOn && energyKnob > 0.0f && ! m_released && ! m_restartPending
                      && ! (s.mode == AutoSwellPeaks && together);
    if (! runs)
    {
        a.gate = false;
        a.open = 0.0;
        return false;
    }
    bool fire = false;
    float hold = s.hold;
    const float chance = sj::clamp(s.chance, 0.0f, 1.0f);
    if (s.mode == AutoSwellClock)
    {
        // One to a period: it starts Loose x Every x a draw into the period, if its draw against Chance says so.
        // Both draws are made every period, whatever Chance and Loose are, so a change of either keeps the rest.
        const double every = static_cast<double>(std::max(0.05f, s.every));
        double before = a.clock;
        a.clock += static_cast<double>(seconds);
        if (a.clock >= every)
        {
            a.clock = std::fmod(a.clock, every);
            a.onset = static_cast<double>(sj::clamp(s.loose, 0.0f, 0.5f) * m_autoRng.unipolar()) * every;
            a.fires = m_autoRng.unipolar() < chance;
            before = -1.0;
        }
        fire = a.fires && before < a.onset && a.clock >= a.onset;
    }
    else
    {
        // Bus A's passes (a linked bus B flies the same ones): as a pass comes within the lead of its closest
        // point, one draw against Chance decides whether it gets a swell.
        hold = std::max(hold, c_autoPeakLead);
        for (auto& h : m_buses[0].heads)
        {
            const double toPeak = (static_cast<double>(h.lenIn) - h.time) / static_cast<double>(std::max(h.pace, 0.05f));
            if (h.swelled || toPeak <= 0.0 || toPeak > static_cast<double>(c_autoPeakLead))
                continue;
            h.swelled = true;
            fire = fire || m_autoRng.unipolar() < chance;
        }
    }
    if (fire)
    {
        a.gate = true;
        a.open = 0.0;
    }
    else if (a.gate)
    {
        a.open += static_cast<double>(seconds);
        if (a.open >= static_cast<double>(hold))
            a.gate = false;
    }
    return a.gate;
}

void RigEngine::followPass(Head& h, const Head& from, float bend, bool newPass)
{
    // The same pass as the lead bus (when, how long, how close, which way), at this bus's own Bend.
    h.time = from.time;
    h.length = from.length;
    h.lenIn = from.lenIn;
    h.lenOut = from.lenOut;
    h.machBase = from.machBase;
    h.machRef = from.machRef;
    h.closest = from.closest;
    h.closeness = from.closeness;
    h.dir = from.dir;
    h.side = from.side;
    h.lift = from.lift;
    h.clockPeak = from.clockPeak;
    h.clockSeconds = from.clockSeconds;
    h.pace = from.pace;
    if (newPass)
        h.aimed = h.swelled = false;
    if (newPass)
        h.mach = sj::clamp(h.machBase * sj::clamp(bend, 0.0f, 1.0f), 0.0f, machLimit(h.lenIn));
}

int RigEngine::wantedRope(int side) const
{
    if (m_cargo == nullptr)
        return -1;
    const int track = m_cargo->getMorphSource(side);
    if (track < 0 || track >= c_tracks || m_cargo->getLive(track) == nullptr)
        return -1; // no rope, or its track is empty: the side's whole bus
    if (side == 1 && track == m_cargo->getMorphSource(0))
        return -1; // one track can't be both sides
    return track;
}

void RigEngine::renderTracks(int frames)
{
    for (auto& bus : m_buses)
    {
        std::fill(bus.in.begin(), bus.in.begin() + frames, 0.0f);
        std::fill(bus.throwIn.begin(), bus.throwIn.begin() + frames, 0.0f);
        std::fill(bus.sideIn.begin(), bus.sideIn.begin() + frames, 0.0f);
    }
    std::fill(m_hitsIn.begin(), m_hitsIn.begin() + frames, 0.0f);
    std::fill(m_hitsSideIn.begin(), m_hitsSideIn.begin() + frames, 0.0f);
    m_hitsSideFed = false;
    m_hitsFileMid = m_hitsFileAll = 0.0f;
    m_hitStarted = false;
    std::fill(m_stow.in.begin(), m_stow.in.begin() + frames, 0.0f);
    std::fill(m_stow.throwIn.begin(), m_stow.throwIn.begin() + frames, 0.0f);
    // (The whole of it, not just this chunk's length: chunks differ in length, and Dry goes on reading it for a
    // moment after the last stereo track stops, so nothing of a longer chunk may be left behind a shorter one.)
    if (m_stow.sideFed)
        m_stow.sideIn.fill(0.0f);
    m_stow.fed = m_stow.sideFed = false;

    // Energy: how deep each track's fader dips, even in dB across the knob (-4 dB per 10%, then silence at the top).
    // Modulator offsets go on after the springs (see Params::energyMod).
    // Everything that glides is stepped by the time this chunk really covers (a block's last chunk can be short).
    const float seconds = static_cast<float>(frames) / m_fs;
    glideMods(frames);
    // The first chunk of a new engine: a rig at rest starts with its sources let go. (Left open, they played for the
    // 60 ms it takes to let go: a burst of the tracks at the head of the first throw, and never again after it.)
    if (m_releaseSnap)
    {
        m_releaseSnap = false;
        m_releaseGain = m_released ? 0.0f : 1.0f;
    }
    // Stop lets go of the sources (60 ms down); Play brings them back (10 ms up, under the transport fade).
    if (m_restartPending)
    {
        bool silent = true;
        for (const float g : m_gain)
            silent = silent && g <= 0.0f;
        if (silent)
            restartNow();
    }
    m_releaseGain = m_restartPending ? std::max(0.0f, m_releaseGain - seconds / c_restartFadeSeconds)
                  : m_released ? std::max(0.0f, m_releaseGain - seconds / 0.06f) : std::min(1.0f, m_releaseGain + seconds / 0.01f);
    const float energyKnob = sj::clamp(m_energySpring.step(seconds) + (m_params.rideOn ? m_energyModNow : 0.0f), 0.0f, 1.0f);
    const float energy = ::Params::energyDepth(energyKnob);
    // Faders Together: the clock (it runs in every mode, so a change of mode finds it going), and how far the mode
    // is in.
    stepCycle(frames);
    const bool together = m_params.rideMode == RideTogether;
    m_togetherMix = together ? std::min(1.0f, m_togetherMix + seconds / c_togetherSeconds)
                             : std::max(0.0f, m_togetherMix - seconds / c_togetherSeconds);
    // Swell: whatever holds it outside the engine (Params::swell), or Auto Swell.
    {
        const bool autoSwell = stepAutoSwell(seconds, energyKnob);
        m_swellSpring.target = m_params.swell || autoSwell ? 1.0f : 0.0f;
        m_autoSwellView.store(autoSwell, std::memory_order_relaxed);
    }
    const float swell = sj::clamp(m_swellSpring.step(seconds), 0.0f, 1.0f);
    {
        // How still the faders are, for the buses (see m_proxBlend), and the ride make-up for the mix. The make-up
        // follows Energy and Auto Ride as they glide, and a held Swell slowly (it lets go quickly).
        const float still = 1.0f - energy * (1.0f - swell);
        m_proxBlend = still * still;
        m_makeupSwell += (swell - m_makeupSwell) * glide(frames, swell > m_makeupSwell ? c_makeupSwellSeconds : c_makeupReleaseSeconds, m_fs);
        const float depth = energy * (1.0f - m_makeupSwell);
        // Faders Together: every fader is at its top at the same moment, as with the faders still, so the mix
        // takes the make-up it has with the faders still: its peaks are then as loud as with Auto Ride off.
        if (std::abs(depth - m_makeupDepth) > 1.0e-4f || std::abs(m_params.chaos - m_makeupChaos) > 1.0e-4f || std::abs(m_togetherMix - m_makeupTogether) > 0.0f)
        {
            m_makeupDepth = depth;
            m_makeupChaos = m_params.chaos;
            m_makeupTogether = m_togetherMix;
            m_makeupTarget = rideMakeup(depth * (1.0f - m_makeupTogether), m_makeupChaos);
        }
    }
    const float pitchRatio = std::exp2(sj::clamp(m_pitchSpring.step(seconds) + m_pitchModNow, -24.0f, 24.0f) / 12.0f);
    // Fill: echoes after the fader (so they hang into the ride's dips), up to about +6 dB of echo send.
    const float fillKnob = sj::clamp(m_fillSpring.step(seconds) + m_fillModNow, 0.0f, 1.0f);
    const float fill = 2.0f * std::pow(fillKnob, 1.2f);
    const float chaos = m_params.chaos;
    const float baseHz = ::Params::rideHz(m_params.rate);
    const float invFrames = 1.0f / static_cast<float>(frames);
    const float morphMixBefore = m_morphMixNow;
    const float morphMix = sj::clamp(m_morphMix.step(seconds), 0.0f, 1.0f);
    m_morphMixNow = morphMix;

    // Each track's own pitch, settled before the hit window below (which depends on every track's speed and mode).
    // The pitch glides to a new setting. A VARI switch fades the track out (~15 ms) and swaps the mode; switching to
    // the shifter then waits for its latency (its delay line refills) before fading back in, so neither a jump in
    // speed nor a step in the delayed sound is heard as a click.
    std::array<float, c_tracks> modeFrom {}, modeTo {};
    {
        const float modeStep = static_cast<float>(frames) / (0.015f * m_fs);
        const float pitchGlide = glide(frames, 0.04f, m_fs); // ~40 ms
        // Held keys: one key moves every track; several are dealt out over the loaded tracks in order.
        const int numKeys = std::min(m_params.numKeys, c_maxKeys);
        int dealt = 0;
        for (size_t ti = 0; ti < static_cast<size_t>(c_tracks); ++ti)
        {
            float key = 0.0f;
            if (numKeys > 0 && (m_cargo == nullptr || m_cargo->getLive(static_cast<int>(ti)) != nullptr))
                key = m_params.keys[static_cast<size_t>(dealt++ % numKeys)];
            m_keyPitchView[ti].store(key, std::memory_order_relaxed);
            auto& now = m_trackPitchNow[ti];
            const float want = sj::clamp(m_params.trackPitch[ti] + key, -24.0f, 24.0f);
            now += (want - now) * pitchGlide;
            if (std::abs(want - now) < 1.0e-4f)
                now = want;

            auto& gain = m_modeGain[ti];
            modeFrom[ti] = gain;
            if (m_params.trackVari[ti] != m_variNow[ti])
            {
                gain = std::max(0.0f, gain - modeStep);
                if (gain <= 0.0f)
                {
                    m_variNow[ti] = m_params.trackVari[ti];
                    m_shifters[ti].reset(std::exp2(now / 12.0f), static_cast<float>(ti) / static_cast<float>(c_tracks));
                    m_modeHold[ti] = m_variNow[ti] ? 0 : m_shifters[ti].getLatencySamples();
                }
            }
            else if (m_modeHold[ti] > 0)
                m_modeHold[ti] = std::max(0, m_modeHold[ti] - frames);
            else
                gain = std::min(1.0f, gain + modeStep);
            modeTo[ti] = gain;
            m_trackSpeedView[ti].store(trackSpeed(static_cast<int>(ti)), std::memory_order_relaxed);
        }
    }

    // Hit timing. AIM: the hits are fired so their transients land on the buses' pass peaks. SYNC HITS: on one
    // shared beat. Otherwise they loop through the hit gate. Either cued mode fires every hit track's next sound
    // together, started early by that sound's own lead-in. Switching between them restarts the hit tracks.
    const bool aim = m_params.hitAim;
    const bool hitSync = m_params.hitSync && ! aim;
    const int timing = aim ? 2 : (hitSync ? 1 : 0);
    if (timing != m_lastHitTiming)
    {
        m_lastHitTiming = timing;
        m_hitClock = 0.0;
        m_lastAimTarget = -1.0e12;
        for (int t = 0; t < c_tracks; ++t)
            if (m_cargo != nullptr && m_cargo->getBus(t) == CargoHold::c_hitsBus)
                m_players[static_cast<size_t>(t)].reset();
    }
    // The last cued hit reaches its transients: publish it for the track view.
    if (m_beatViewWait >= 0.0)
    {
        m_beatViewWait -= frames;
        if (m_beatViewWait < 0.0)
        {
            for (int t = 0; t < c_tracks; ++t)
                m_hitRegionView[static_cast<size_t>(t)].store(m_pendingHitRegion[static_cast<size_t>(t)], std::memory_order_relaxed);
            m_hitBeatView.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (timing == 0)
        m_beatViewWait = -1.0;

    // The lead-in and tail the cued hits need: from the regions exactly as each player will fire them (skip silence
    // off = the whole file is one hit, aligned on its first transient; start markers respected), each track at its own
    // speed (global Pitch times its own VARI pitch), in output seconds. A VARI-off track is heard later by its
    // shifter's latency, so it needs that much more lead-in.
    auto hitWindow = [&](double& maxPre, double& maxPost)
    {
        maxPre = maxPost = 0.0;
        bool any = false;
        for (int t = 0; t < c_tracks; ++t)
        {
            const auto* d = m_cargo->getLive(t);
            if (d == nullptr || m_cargo->getBus(t) != CargoHold::c_hitsBus)
                continue;
            HitWindow w;
            w.extend(*d, m_cargo->getSkipSilence(t), m_cargo->getStart(t));
            const double speed = juce::jmax(0.01, static_cast<double>(pitchRatio * trackSpeed(t)));
            const double lead = m_variNow[static_cast<size_t>(t)]
                                    ? 0.0 : static_cast<double>(m_shifters[static_cast<size_t>(t)].getLatencySamples()) / m_sampleRate;
            maxPre = std::max(maxPre, std::min(w.pre / speed, 1.0) + lead);
            maxPost = std::max(maxPost, w.post / speed);
            any = true;
        }
        if (! any)
        {
            const HitWindow none;
            maxPre = none.pre / juce::jmax(0.01, static_cast<double>(pitchRatio));
            maxPost = none.post / juce::jmax(0.01, static_cast<double>(pitchRatio));
        }
        return any;
    };
    auto publishCue = [&](CargoCue& cue, double toPeak, double maxPost)
    {
        const double period = std::min(toPeak + maxPost + 0.08, 4.0);
        cue.active = true;
        cue.secondsToPeak = toPeak;
        cue.flightSeconds = period;
        m_beatViewWait = toPeak * m_sampleRate; // the view moves when the transients land
        if (m_hasHitTracks)                     // and a recording gets a marker there
        {
            // (The marks waiting are counted down at the top of each chunk, and this chunk's count is done: the
            // mark is kept from the start of the next chunk, or made now if the transients land in this one.)
            const double landing = toPeak * m_sampleRate - static_cast<double>(frames);
            if (landing < 0.0)
                mark(MarkHit, std::max(0, static_cast<int>(toPeak * m_sampleRate)));
            else
                for (auto& at : m_hitMarks)
                    if (at < 0.0)
                    {
                        at = landing;
                        break;
                    }
        }
        const double ratio = juce::jmax(0.01, static_cast<double>(pitchRatio));
        m_hitPreView.store(toPeak * ratio, std::memory_order_relaxed);
        m_hitPostView.store(juce::jmax(0.0, period - toPeak - 0.08) * ratio, std::memory_order_relaxed);
        return period;
    };

    m_hasHitTracks = false;
    if (m_cargo != nullptr)
        for (int t = 0; t < c_tracks && ! m_hasHitTracks; ++t)
            m_hasHitTracks = m_cargo->getLive(t) != nullptr && m_cargo->getBus(t) == CargoHold::c_hitsBus;
    // Cued hits whose transients land in this chunk.
    for (auto& at : m_hitMarks)
        if (at >= 0.0)
        {
            if (at < static_cast<double>(frames))
            {
                mark(MarkHit, static_cast<int>(at));
                at = -1.0;
            }
            else
                at -= static_cast<double>(frames);
        }

    // A hit starts: the pass it goes with (see placeHits). Aimed: the pass or throw it was aimed at.
    auto hitGoesWith = [this](int kind, int busIndex, int index)
    {
        m_hitFollow.kind = kind;
        m_hitFollow.bus = busIndex;
        m_hitFollow.index = index;
        m_hitFollow.age = 0.0f;
        m_hitStarted = true;
        m_hitWantsLoudest = false;
    };

    CargoCue hitCue;
    if (hitSync && m_cargo != nullptr)
    {
        m_hitClock -= frames;
        if (m_hitClock <= 0.0)
        {
            double maxPre = 0.0, maxPost = 0.0;
            hitWindow(maxPre, maxPost);
            m_hitClock += publishCue(hitCue, maxPre, maxPost) * m_sampleRate;
            m_hitStarted = m_hitWantsLoudest = true; // the beat's hits go with the loudest pass in flight
        }
    }
    else if (aim && m_cargo != nullptr)
    {
        // Look ahead: each doppler pass is loudest at its middle (closest to the listener), which is known as soon as
        // the pass starts. Once a pass's peak is no further off than the longest lead-in any hit needs, decide whether
        // it earns a hit (closer passes and a louder bus score higher; Sensitivity sets the bar) and cue every hit
        // track so its transient lands on it. Peaks that land together (linked buses, overlapping passes) fire once.
        double maxPre = 0.0, maxPost = 0.0;
        if (hitWindow(maxPre, maxPost))
        {
            const double chunk = static_cast<double>(frames) / m_sampleRate;
            // Sensitivity: the score a pass needs (0.80 .. 0.15), eased so the knob spreads over sparse and busy rigs.
            const float bar = 0.80f - 0.65f * std::pow(sj::clamp(m_params.hitSense, 0.0f, 1.0f), 1.55f);
            // One cue per chunk: the nearest peak that earns a hit. A pass is settled (aimed) once it's cued, scored too
            // low, or found to be a peak already aimed at. Others in range wait for a later chunk, and a bus that's
            // silent right now is looked at again while its peak is still far enough off.
            double best = -1.0;
            Head* bestHead = nullptr;
            const BusState* bestBus = nullptr;
            // A throw always earns its hit, and goes first. Its sources stay open until the hits have rung out.
            size_t bestThrow = m_throws.size();
            for (size_t v = 0; v < m_throws.size(); ++v)
            {
                if (! m_throws[v].active || m_throws[v].aimed)
                    continue;
                const auto& h = m_buses[0].throwHeads[v];
                // (To where it is heard: a bus whose Swirl is a Tape Flanger is 8 ms late. A throw goes by bus A's.)
                const double toPeak = static_cast<double>(h.lenIn) - h.time + static_cast<double>(tapeDelaySeconds(m_buses[0]));
                if (toPeak <= chunk)
                    m_throws[v].aimed = true; // too late
                else if (toPeak <= maxPre + chunk && (best < 0.0 || toPeak < best))
                {
                    best = toPeak;
                    bestThrow = v;
                }
            }
            if (bestThrow < m_throws.size())
            {
                m_throws[bestThrow].aimed = true;
                m_throws[bestThrow].gate = std::max(m_throws[bestThrow].gate, best + maxPost + 0.1);
                m_lastAimTarget = m_aimClock + best * m_sampleRate;
                publishCue(hitCue, best, maxPost);
                hitGoesWith(HitFollow::Throw, 0, static_cast<int>(bestThrow));
            }
            // The rig's own passes: not while it is stopped (only a throw's hits play then).
            const bool ridePasses = bestThrow == m_throws.size() && m_releaseGain > 0.0f;
            if (together && ridePasses)
            {
                // Faders Together: the clock's peaks are the only targets, every Hit Every-th of them (the first
                // peak after a start counts as one). Sensitivity has no say. A peak is settled once it is cued,
                // passed over, or found to be one a throw's hit already lands on.
                const double next = std::floor(m_cycleX0) + 1.0; // the clock reads this at its next peak
                const size_t sounding = m_buses[0].env >= 0.002f ? 0 : 1; // the bus the hit goes with (bus A first)
                const double toPeak = (next - m_cycleX0) * static_cast<double>(m_cycleSeconds) + static_cast<double>(tapeDelaySeconds(m_buses[sounding]));
                const auto index = static_cast<std::int64_t>(next);
                if (index != m_cycleAimed && toPeak > chunk && toPeak <= maxPre + chunk)
                {
                    const double target = m_aimClock + toPeak * m_sampleRate;
                    if ((index - 1) % std::max(1, m_params.together.hitEvery) != 0 || std::abs(target - m_lastAimTarget) < 0.15 * m_sampleRate)
                        m_cycleAimed = index;
                    else if (std::max(m_buses[0].env, m_buses[1].env) >= 0.002f) // (nothing playing yet: look again next chunk)
                    {
                        m_cycleAimed = index;
                        m_lastAimTarget = target;
                        publishCue(hitCue, toPeak, maxPost);
                        // It goes with the pass that is closest on that peak, on the bus that sounds (bus A first).
                        const size_t b = sounding;
                        int with = -1;
                        for (size_t k = 0; k < m_buses[b].heads.size(); ++k)
                            if (const auto& h = m_buses[b].heads[k]; h.clockSeconds > 0.0f && std::abs(h.clockPeak - next) < 0.25)
                                with = static_cast<int>(k);
                        if (with >= 0)
                            hitGoesWith(HitFollow::Pass, static_cast<int>(b), with);
                        else
                            m_hitStarted = m_hitWantsLoudest = true; // (the passes have not joined the clock yet)
                    }
                }
            }
            for (auto& bus : m_buses)
                for (auto& h : bus.heads)
                {
                    if (! ridePasses || together)
                        break;
                    const double toPeak = static_cast<double>(h.lenIn) - h.time + static_cast<double>(tapeDelaySeconds(bus));
                    if (h.aimed || toPeak <= chunk || toPeak > maxPre + chunk)
                        continue;
                    if (bus.env < 0.002f)
                        continue; // nothing playing on this bus yet: check again next chunk
                    const float level = sj::clamp(bus.env / std::max(bus.peakRef, 1.0e-4f), 0.0f, 1.0f);
                    // Closeness counts most: with steady levels (ride off, Swell) the level part is the same for every pass.
                    const float score = 0.7f * h.closeness + 0.3f * level;
                    const double target = m_aimClock + toPeak * m_sampleRate;
                    if (score < bar || std::abs(target - m_lastAimTarget) < 0.15 * m_sampleRate)
                    {
                        h.aimed = true; // not big enough, or the same peak as one already aimed at
                        continue;
                    }
                    if (best < 0.0 || toPeak < best)
                    {
                        best = toPeak;
                        bestHead = &h;
                        bestBus = &bus;
                    }
                }
            if (bestHead != nullptr && bestThrow == m_throws.size())
            {
                bestHead->aimed = true;
                m_lastAimTarget = m_aimClock + best * m_sampleRate;
                publishCue(hitCue, best, maxPost);
                hitGoesWith(HitFollow::Pass, static_cast<int>(bestBus - m_buses.data()), static_cast<int>(bestHead - bestBus->heads.data()));
            }
        }
    }
    m_aimClock += frames;

    // Throws: their sources are open while any voice needs them (the tracks at their fader levels, not ridden).
    {
        bool open = false;
        for (auto& v : m_throws)
            if (v.active || v.gate > 0.0)
            {
                v.gate -= static_cast<double>(seconds);
                open = open || v.gate > 0.0;
            }
        m_throwOpen = open ? std::min(1.0f, m_throwOpen + seconds / c_throwOpenSeconds)
                           : std::max(0.0f, m_throwOpen - seconds / c_throwCloseSeconds);
    }

    // The Master track: its contours where it is playing now (the loop's two heads while they cross: the louder).
    const int master = m_cargo != nullptr ? m_cargo->getMaster() : -1;
    bool masterPlays = false;
    m_masterFollow[0] = 0.0f;
    if (master >= 0 && master < c_tracks)
    {
        const auto& player = m_players[static_cast<size_t>(master)];
        const CargoData* d = player.getData();
        const CargoContours* c = d != nullptr ? d->getContours() : nullptr;
        const float head = player.getPlayhead(0), other = player.getPlayhead(1);
        if (c != nullptr && c->size() > 0 && head >= 0.0f)
        {
            masterPlays = true;
            const double length = static_cast<double>(d->length());
            const double at = static_cast<double>(head) * length;
            float level = CargoContours::at(c->level, c->hop, at);
            if (other >= 0.0f)
                level = std::max(level, CargoContours::at(c->level, c->hop, static_cast<double>(other) * length));
            m_masterFollow = { level * m_releaseGain, CargoContours::at(c->pitch, c->hop, at), CargoContours::at(c->bright, c->hop, at) };
            m_masterRide += (level - m_masterRide) * glide(frames, level > m_masterRide ? c_masterUpSeconds : c_masterDownSeconds, m_fs);
        }
    }
    else
    {
        m_masterFollow = {};
    }
    if (! masterPlays)
        m_masterRide += (0.0f - m_masterRide) * glide(frames, c_masterDownSeconds, m_fs);
    m_masterView.store(m_masterRide, std::memory_order_relaxed);
    // Follow Master: every other track's ride is the Master's loudness (shaped by Energy like the LFO it replaces),
    // and the Master itself plays unridden: its own sound is the swell.
    m_followMix += ((m_params.rideMode == RideFollowMaster && masterPlays ? 1.0f : 0.0f) - m_followMix) * glide(frames, c_followSeconds, m_fs);
    if (m_followMix < 1.0e-4f) m_followMix = 0.0f;
    const float followRide = std::pow(sj::clamp(m_masterRide, 0.0f, 1.0f), 1.0f + 1.85f * energy);

    // Morph ropes: the track each side listens to this chunk. A change while the morph is in fades what it moves
    // out first (the old roped track and the new one, and the rest of the side's bus only when the bus itself goes
    // into or comes out of the morph: rope to rope leaves the bus playing as it is), swaps at silence, and fades
    // back in; with the morph out the roped track plays through its bus just the same, so the swap is made at once.
    std::array<int, 2> rope = m_ropeNow, ropeWanted { wantedRope(0), wantedRope(1) };
    std::array<float, 2> ropeFrom = m_ropeFade, ropeTo = m_ropeFade;
    {
        const bool morphHeard = morphMix > 1.0e-4f || m_morph.isRunning();
        const float step = seconds / c_ropeFadeSeconds;
        for (size_t side = 0; side < 2; ++side)
        {
            if (ropeFrom[side] >= 1.0f)
                m_ropeFading[side] = 0; // the last change is done with
            if (ropeWanted[side] != m_ropeNow[side])
            {
                if (! morphHeard)
                {
                    rope[side] = m_ropeNow[side] = ropeWanted[side];
                }
                else
                {
                    ropeTo[side] = std::max(0.0f, ropeFrom[side] - step);
                    // Everything this change moves stays in the fade until it is back up.
                    for (const int track : { m_ropeNow[side], ropeWanted[side] })
                        m_ropeFading[side] |= track >= 0 ? (1u << track) : c_ropeFadesBus;
                }
            }
            if (ropeWanted[side] == m_ropeNow[side])
                ropeTo[side] = std::min(1.0f, ropeFrom[side] + step);
            m_ropeFade[side] = ropeTo[side];
        }
        if (rope[0] >= 0 && rope[0] == rope[1])
            rope[1] = -1; // never the same track on both sides (the wanted ropes already rule it out)
        for (auto& tap : m_morphTap)
            std::fill(tap.begin(), tap.begin() + frames, 0.0f);
    }

    // A sub swells with bus A (or bus B, when A holds no track that can be heard), not with its own tracks' rides:
    // those are let go as far as it has a bus to go by. With neither bus to follow, its tracks are ridden like any
    // others, and Follow is how deep. So Follow is always how much the sub swells, whatever it swells with.
    // The other modes: Wake's tracks are not ridden either (the trail is what moves it); Dry's keep as much of
    // their ride as Follow says; Fly's are ridden like any others.
    const int stowMode = m_stow.mode; // the mode in use
    const bool stowShut = isStowShut();
    const bool subMode = stowMode == StowSub;
    float subRide = stowMode == StowWake ? 0.0f : stowMode == StowDry ? m_stow.now.follow : 1.0f; // how much of its ride (in dB) a track on Stowaway keeps
    if (subMode)
    {
        auto& sub = m_stow.sub;
        bool onA = false, onB = false;
        if (m_cargo != nullptr)
            for (int t = 0; t < c_tracks; ++t)
                if (m_cargo->getLive(t) != nullptr && m_levelSprings[static_cast<size_t>(t)].target * m_cargo->laneGain(CargoHold::c_numBuiltIns + t) > 1.0e-3f)
                {
                    const int bus = m_cargo->getBus(t);
                    onB = onB || bus == 1;
                    onA = onA || (bus != 1 && bus != CargoHold::c_hitsBus && bus != CargoHold::c_stowBus);
                }
        const float a = onA ? 1.0f : 0.0f, b = onB ? 1.0f : 0.0f;
        sub.busA = sub.on ? sub.busA + (a - sub.busA) * glide(frames, c_subBusSeconds, m_fs) : a;
        sub.busB = sub.on ? sub.busB + (b - sub.busB) * glide(frames, c_subBusSeconds, m_fs) : b;
        subRide = m_stow.now.follow * (1.0f - sub.busA) * (1.0f - sub.busB);
    }

    // Faders Together's Stagger: each loaded track's place in the fan, -1 (the first) .. 1 (the last).
    std::array<float, c_tracks> staggerPlace {};
    if (m_togetherMix > 0.0f && m_cargo != nullptr)
    {
        int loaded = 0;
        for (int t = 0; t < c_tracks; ++t)
            loaded += m_cargo->getLive(t) != nullptr ? 1 : 0;
        int place = 0;
        for (int t = 0; t < c_tracks; ++t)
            if (m_cargo->getLive(t) != nullptr)
                staggerPlace[static_cast<size_t>(t)] = loaded > 1 ? 2.0f * static_cast<float>(place++) / static_cast<float>(loaded - 1) - 1.0f : 0.0f;
    }

    for (int t = 0; t < c_tracks; ++t)
    {
        const auto ti = static_cast<size_t>(t);
        const float level = sj::clamp(m_levelSprings[ti].step(seconds), 0.0f, 1.2f);

        // Ride: an LFO on the fader at this track's own offset speed, plus a random hand on it. Chaos varies the speed,
        // and how big each swell is (a new size every cycle), without lifting the dips Energy sets.
        m_ridePhase[ti] += static_cast<double>(baseHz * c_rideOffsets[ti] * (1.0f + 0.7f * chaos * m_rideDrift[ti].current)
                                               * static_cast<float>(frames) / m_fs);
        if (m_ridePhase[ti] >= 1.0)
        {
            m_ridePhase[ti] -= 1.0;
            m_rideDepth[ti] = m_rng.unipolar();
        }
        const float lfo = 0.5f - 0.5f * std::cos(sj::c_twoPi * static_cast<float>(m_ridePhase[ti]));
        // Higher energy = deeper and more peaked swells (the fader spends longer down low).
        const float shaped = std::pow(lfo, 1.0f + 1.85f * energy);
        const float drift = m_rideDrift[ti].next(frames);
        float r = sj::clamp(shaped * (1.0f - 0.6f * chaos * m_rideDepth[ti]) * (1.0f + 0.2f * chaos * drift), 0.0f, 1.0f);
        const CargoData* data = m_cargo != nullptr ? m_cargo->getLive(t) : nullptr;
        // Where the track plays. A change of place (by hand, or a rumble as Stowaway becomes a sub or stops being
        // one) fades the track out of the old place, swaps, and fades it into the new one. A track that is not
        // sounding just moves.
        const int busWanted = m_cargo != nullptr ? m_cargo->getBus(t) : 0;
        float busFade0 = m_busFade[ti];
        if (m_busNow[ti] != busWanted)
        {
            const float busStep = seconds / c_busFadeSeconds;
            if (m_busNow[ti] < 0 || data == nullptr || (m_gain[ti] < 1.0e-5f && m_throwGain[ti] < 1.0e-5f))
            {
                m_busNow[ti] = busWanted;
                busFade0 = m_busFade[ti] = 1.0f;
            }
            else if (busFade0 <= 0.0f)
            {
                m_busNow[ti] = busWanted;
                m_busFade[ti] = std::min(1.0f, busStep);
            }
            else
                m_busFade[ti] = std::max(0.0f, busFade0 - busStep);
        }
        else if (busFade0 < 1.0f)
            m_busFade[ti] = std::min(1.0f, busFade0 + seconds / c_busFadeSeconds);
        const float busFade1 = m_busFade[ti];
        const int bus = m_busNow[ti];
        const bool onStow = bus == CargoHold::c_stowBus;
        // Stowaway switched off (and faded out): the tracks sent there wait. They are not played, here or
        // anywhere else, and start again like a sound just loaded when it is switched on.
        const bool waits = onStow && busWanted == bus && stowShut;
        if (waits)
            data = nullptr;
        // Faders Together: every track on the one swell of the clock, in place of its own wave (a change of mode
        // crossfades the two). The swell is at its top when the clock is at its peak, for the listener: a track
        // on bus A or bus B is that bus's lead ahead (its sound takes that long to come out of the pass), a track
        // on a morph rope its side's. Peak At is how much of the swell is the build (the trough moves, the top
        // stays on the peak); Stagger fans the loaded tracks either side, in track order; Chaos is one size for
        // the whole swell. The fader shown is the swell as it is heard, without the lead.
        float shown = r;
        if (m_togetherMix > 0.0f)
        {
            int leadBus = bus == 0 || bus == 1 ? bus : -1;
            for (int side = 0; side < 2; ++side)
                if (rope[static_cast<size_t>(side)] == t)
                    leadBus = side;
            const float lead = leadBus >= 0 ? m_buses[static_cast<size_t>(leadBus)].lead : 0.0f;
            const double fan = static_cast<double>(-c_staggerShare * sj::clamp(m_params.together.stagger, 0.0f, 1.0f) * staggerPlace[ti]); // (the first track first)
            auto swellAt = [&](double ahead)
            {
                double at = m_cycle.phase + ahead + fan - (0.5 - static_cast<double>(m_cyclePeakAt));
                at -= std::floor(at);
                const auto a = static_cast<float>(at);
                const float warped = a < m_cyclePeakAt ? 0.5f * a / m_cyclePeakAt : 0.5f + 0.5f * (a - m_cyclePeakAt) / (1.0f - m_cyclePeakAt);
                const float wave = 0.5f - 0.5f * std::cos(sj::c_twoPi * warped);
                return sj::clamp(std::pow(wave, 1.0f + 1.85f * energy) * m_cycleSizeNow, 0.0f, 1.0f);
            };
            // (All the way in, the swell itself: a blend of 100% still rounds by what it blends from.)
            const bool all = m_togetherMix >= 1.0f;
            shown = all ? swellAt(0.0) : sj::lerp(r, swellAt(0.0), m_togetherMix);
            r = all ? swellAt(static_cast<double>(lead)) : sj::lerp(r, swellAt(static_cast<double>(lead)), m_togetherMix);
        }
        const float ownSwell = r; // the track's own swell, whatever the Master says
        // A cued hit is not ridden in Faders Together (it lands on the peak, where its fader would be at the top).
        const bool cuedHit = timing != 0 && bus == CargoHold::c_hitsBus;
        auto rideOf = [&](float wave)
        {
            if (m_followMix > 0.0f)
                wave = sj::lerp(wave, t == master ? 1.0f : followRide, m_followMix);
            float fader = sj::lerp(1.0f, wave, energy);
            fader = sj::lerp(fader, 1.0f, swell);
            if (cuedHit && m_togetherMix > 0.0f)
                fader = sj::lerp(fader, 1.0f, m_togetherMix);
            if (onStow && data != nullptr && subRide < 1.0f)
                fader = subRide <= 0.0f ? 1.0f : std::pow(fader, subRide);
            return fader;
        };
        const float ride = rideOf(r);
        m_rideView[ti].store(m_togetherMix > 0.0f ? rideOf(shown) : ride, std::memory_order_relaxed);
        if (m_throwBalanceWanted)
        {
            // A ridden throw's balance: each track at a moment of its own swell. Not of Follow Master's, which says
            // nothing as a throw starts from rest: the Master has not played yet, and every other track would be
            // held down by the whole of Energy in every throw.
            float own = sj::lerp(sj::lerp(1.0f, ownSwell, energy), 1.0f, swell);
            if (onStow && subRide < 1.0f)
                own = subRide <= 0.0f ? 1.0f : std::pow(own, subRide);
            m_throwBalanceRide[ti] = data != nullptr ? own : 0.0f;
        }

        const float laneGain = m_cargo != nullptr ? m_cargo->laneGain(CargoHold::c_numBuiltIns + t) : 1.0f;
        const float target = data != nullptr ? level * ride * laneGain * m_releaseGain : 0.0f;
        const float throwTarget = data != nullptr ? level * laneGain * m_throwOpen * m_throwBalance[ti] : 0.0f;

        if (data == nullptr && (waits || (m_gain[ti] < 1.0e-5f && m_throwGain[ti] < 1.0e-5f)))
        {
            m_gain[ti] = 0.0f;
            m_throwGain[ti] = 0.0f;
            m_sideGain[ti] = 0.0f;
            if (m_shifterUsed[ti]) // emptied while VARI was off: drop the old sound still inside the shifter (once)
            {
                m_shifters[ti].reset(std::exp2(m_trackPitchNow[ti] / 12.0f), static_cast<float>(t) / static_cast<float>(c_tracks));
                m_shifterUsed[ti] = false;
            }
            m_players[ti].render(nullptr, CargoRole::Bed, true, 1.0f, chaos, m_rng, 0.6f, CargoCue {}, m_trackScratch.data(), frames);
            m_hitRegionView[ti].store(-1, std::memory_order_relaxed);
            m_pendingHitRegion[ti] = -1;
            continue;
        }

        // How gappy is this sound? Gappy sounds get more of the long fill delays.
        if (data != m_coverageFor[ti])
        {
            m_coverageFor[ti] = data;
            double covered = 0.0;
            if (data != nullptr && data->length() > 0)
            {
                for (const auto& reg : data->regions)
                    covered += reg.end - reg.start;
                covered /= static_cast<double>(data->length());
            }
            m_coverage[ti] = static_cast<float>(covered);
            m_fillAmount[ti] = sj::clamp(1.2f * (1.0f - m_coverage[ti]) + 0.15f, 0.0f, 1.0f);
        }

        const float modeGain0 = modeFrom[ti], modeGain1 = modeTo[ti];
        const bool vari = m_variNow[ti];
        const float ownRatio = std::exp2(m_trackPitchNow[ti] / 12.0f);
        // VARI on, pitched up: the faster read filters a little under the new top of the band (so nothing folds
        // back), which costs a hissy sound some level against the same pitch with VARI off. A small make-up.
        const float variGain = vari ? sj::dbToGain(c_variUpDbPerOctave * sj::clamp(m_trackPitchNow[ti] / 12.0f, 0.0f, 2.0f)) : 1.0f;

        const bool skip = m_cargo == nullptr || m_cargo->getSkipSilence(t);
        m_players[ti].setStart(m_cargo != nullptr ? m_cargo->getStart(t) : 0.0f);
        const float rate = pitchRatio * (vari ? ownRatio : 1.0f);
        const bool syncedHit = timing != 0 && m_cargo != nullptr && bus == CargoHold::c_hitsBus;
        // A stereo sound on bus B, or on Stowaway in Dry: its side signal is played along with it (see the bus and
        // renderDry, which decide how much of it is heard). Not through the pitch shifter: with VARI off the track
        // plays its mono sum.
        const bool dryStow = onStow && stowMode == StowDry;
        // A stereo hit keeps its two sides too: the hits are placed as a left and a right (see hitGains).
        const bool toHitGate = bus == CargoHold::c_hitsBus;
        const bool wantSide = data != nullptr && data->hasSide() && (bus == static_cast<int>(c_sideBus) || dryStow || toHitGate) && vari;
        // (A track that has just arrived somewhere that takes no sides brings none with it: they went out with the
        // fade it left by, and must not carry on into bus B.)
        const float sideGain0 = ! wantSide && busFade0 <= 0.0f ? 0.0f : m_sideGain[ti];
        const float sideGain1 = wantSide ? std::min(1.0f, sideGain0 + seconds / c_sideFadeSeconds)
                                         : std::max(0.0f, sideGain0 - seconds / c_sideFadeSeconds);
        m_sideGain[ti] = sideGain1;
        float* const sideOut = (sideGain0 > 0.0f || sideGain1 > 0.0f) ? m_sideScratch.data() : nullptr;
        if (syncedHit)
        {
            CargoCue cue = hitCue;
            if (cue.active && ! vari) // heard later by the shifter's latency: fire that much earlier
                cue.secondsToPeak = std::max(0.0, cue.secondsToPeak - static_cast<double>(m_shifters[ti].getLatencySamples()) / m_sampleRate);
            m_players[ti].render(data, CargoRole::Impact, skip, rate, chaos, m_rng, 0.15f, cue, m_trackScratch.data(), frames, sideOut);
        }
        else
            m_players[ti].render(data, CargoRole::Bed, skip, rate, chaos, m_rng, 0.6f, CargoCue {}, m_trackScratch.data(), frames, sideOut);
        if (! vari)
        {
            m_shifters[ti].process(m_trackScratch.data(), m_trackScratch.data(), frames, ownRatio);
            m_shifterUsed[ti] = true;
        }
        if (! syncedHit)
        {
            m_hitRegionView[ti].store(-1, std::memory_order_relaxed);
            m_pendingHitRegion[ti] = -1;
        }
        else if (hitCue.active)
            m_pendingHitRegion[ti] = m_players[ti].getShotRegion();

        // Skip Silence hops over the file's gaps, so every sound gets a fair share of the echoes then.
        // (Stowaway in Dry takes no echoes.)
        const float fillAmount = dryStow ? 0.0f : fill * (skip ? std::max(0.5f, m_fillAmount[ti]) : m_fillAmount[ti]);
        auto& line = m_fillLines[ti];
        // Routed by type, or to the place chosen by hand: the hit gate, bus A, bus B or Stowaway. A track on a
        // morph rope goes to its side's tap instead (see below); the fades are those of a rope being changed.
        float* dest = trackInput(bus);
        float rope0 = busFade0, rope1 = busFade1; // every fade on the way to the track's place: a change of place, then the ropes
        for (size_t side = 0; side < 2; ++side)
        {
            if (rope[side] == t)
                dest = m_morphTap[side].data();
            const std::uint32_t fading = m_ropeFading[side];
            if ((fading & (1u << t)) != 0 || (bus == static_cast<int>(side) && (fading & c_ropeFadesBus) != 0))
            {
                rope0 *= ropeFrom[side];
                rope1 *= ropeTo[side];
            }
        }
        const float g0 = m_gain[ti];
        const auto& taps = m_fillTaps[ti];
        // A throw in flight: the track also feeds its bus's throw line, at its fader level. A hit track has no
        // line of its own (the hits are one sound, heard once): the throw only lifts it to its fader level, so
        // a throw's hit is as loud with the rig playing as from rest (not the ridden hit and the thrown one summed).
        // Stowaway's throw feed is the same sound as its track, in step with it (no pass delays it), so it too
        // carries only what the ride leaves out: the throw lifts the track towards its fader level. (Not in Fly,
        // which flies the throw as a pass of its own, from a line of its own, like a bus.)
        const float throw0 = m_throwGain[ti];
        if (throw0 > 0.0f || throwTarget > 0.0f)
        {
            const bool toHits = bus == CargoHold::c_hitsBus;
            const bool lifts = toHits || (onStow && stowMode != StowFly);
            float* throwDest = toHits ? m_hitsIn.data() : throwInput(bus);
            for (int i = 0; i < frames; ++i)
            {
                const float frac = static_cast<float>(i + 1) * invFrames;
                float throwGain = throw0 + (throwTarget - throw0) * frac;
                if (lifts)
                    throwGain = std::max(0.0f, throwGain - (g0 + (target - g0) * frac));
                const float lift = (modeGain0 + (modeGain1 - modeGain0) * frac) * throwGain * variGain * (busFade0 + (busFade1 - busFade0) * frac);
                throwDest[i] += m_trackScratch[static_cast<size_t>(i)] * lift;
                if (toHits && sideOut != nullptr) // a stereo hit's sides are lifted with it
                    m_hitsSideIn[static_cast<size_t>(i)] += m_sideScratch[static_cast<size_t>(i)] * lift * (sideGain0 + (sideGain1 - sideGain0) * frac);
            }
        }
        m_throwGain[ti] = throwTarget;
        for (int i = 0; i < frames; ++i)
        {
            const float x = m_trackScratch[static_cast<size_t>(i)];
            // Long delays (about 0.5 / 1 / 2 s, this track's own times) that fill the gaps of unsteady sounds.
            const float delayed = m_fillTone[ti].lowpass(0.5f * line.readLinear(taps[0])
                                                       + 0.35f * line.readLinear(taps[1])
                                                       + 0.25f * line.readLinear(taps[2]));
            const float frac = static_cast<float>(i + 1) * invFrames;
            const float g = g0 + (target - g0) * frac;
            const float dry = x * (modeGain0 + (modeGain1 - modeGain0) * frac) * g * variGain;
            line.push(dry + 0.3f * delayed);
            dest[i] += (dry + fillAmount * delayed) * (rope0 + (rope1 - rope0) * frac);
        }
        if (sideOut != nullptr)
        {
            // The side signal takes the track's fader, ride and fades (not the Fill echoes), and goes as the morph
            // comes in over this track's sound (its own rope, or bus B as a whole): a morph has no sides.
            const bool morphed = rope[0] == t || rope[1] == t || (rope[1] < 0 && ! onStow && ! toHitGate);
            const float keep0 = morphed ? 1.0f - morphMixBefore : 1.0f, keep1 = morphed ? 1.0f - morphMix : 1.0f;
            float* sideDest = onStow ? m_stow.sideIn.data() : (toHitGate ? m_hitsSideIn.data() : m_buses[c_sideBus].sideIn.data());
            m_stow.sideFed = m_stow.sideFed || onStow;
            m_hitsSideFed = m_hitsSideFed || toHitGate;
            for (int i = 0; i < frames; ++i)
            {
                const float frac = static_cast<float>(i + 1) * invFrames;
                sideDest[i] += m_sideScratch[static_cast<size_t>(i)] * (modeGain0 + (modeGain1 - modeGain0) * frac)
                               * (g0 + (target - g0) * frac) * variGain * (rope0 + (rope1 - rope0) * frac)
                               * (sideGain0 + (sideGain1 - sideGain0) * frac) * (keep0 + (keep1 - keep0) * frac);
            }
        }
        // What the hit tracks' files are as a whole, by how loud each plays (see renderHits).
        if (toHitGate && data != nullptr)
        {
            const float heard = std::max(target, throwTarget);
            m_hitsFileAll += heard * heard;
            m_hitsFileMid += heard * heard * (wantSide ? data->midShare : 1.0f);
        }
        m_gain[ti] = target;
    }

    // A ridden throw: the balance of the tracks as the ride has them now, the loudest at its fader.
    if (m_throwBalanceWanted)
    {
        m_throwBalanceWanted = false;
        const float loudest = *std::max_element(m_throwBalanceRide.begin(), m_throwBalanceRide.end());
        for (size_t t = 0; t < m_throwBalance.size(); ++t)
            m_throwBalance[t] = loudest > 1.0e-4f ? std::max(c_throwBalanceFloor, m_throwBalanceRide[t] / loudest) : 1.0f;
    }

    // Tell the hold which sample data the players still read, so data replaced by an edit is never freed while a
    // paused or bypassed engine could come back to it.
    if (m_cargo != nullptr)
        for (int t = 0; t < c_tracks; ++t)
            m_cargo->markInUse(t, m_players[static_cast<size_t>(t)].getData(), m_players[static_cast<size_t>(t)].getPending());

    {
        // Morph between what feeds bus A and bus B, then hand the result back to the two buses in proportion to
        // X, so it still moves with A's or B's doppler and colour. The insert keeps the switch clean: the dry
        // sound it fades against is delayed to line up with the morph, and it clears the morph when it is out.
        SpectralMorph::Params mp;
        mp.x = m_params.morphX;
        mp.y = m_params.morphY;
        mp.mode = juce::jlimit(0, SpectralMorph::NumModes - 1, m_params.morphMode);
        m_morph.setParams(mp);
        // A roped side: the morph hears the tap (that one track), its share of the result goes to the side's bus,
        // and what is left of the track's dry sound goes back to the bus the track plays on. With the morph out
        // the tap comes back untouched, so the roped track is simply heard through its bus.
        const bool ropedA = rope[0] >= 0, ropedB = rope[1] >= 0;
        m_morph.process(ropedA ? m_morphTap[0].data() : m_buses[0].in.data(), ropedB ? m_morphTap[1].data() : m_buses[1].in.data(),
                        frames, morphMix, ropedA ? m_buses[0].in.data() : nullptr, ropedB ? m_buses[1].in.data() : nullptr);
        for (size_t side = 0; side < 2; ++side)
        {
            if (rope[side] >= 0)
            {
                float* home = trackInput(std::max(0, m_busNow[static_cast<size_t>(rope[side])]));
                const float* tap = m_morphTap[side].data();
                for (int i = 0; i < frames; ++i)
                    home[i] += tap[i];
            }
            // A rope that has faded out is swapped for the wanted one here, at silence.
            if (ropeWanted[side] != m_ropeNow[side] && m_ropeFade[side] <= 0.0f)
                m_ropeNow[side] = ropeWanted[side];
            m_ropeView[side].store(m_ropeNow[side], std::memory_order_relaxed);
        }
    }

}

float* RigEngine::trackInput(int bus)
{
    switch (bus)
    {
        case CargoHold::c_hitsBus: return m_hitsIn.data();
        case CargoHold::c_stowBus: m_stow.fed = true; return m_stow.in.data();
        case 1:                    return m_buses[1].in.data();
        default:                   return m_buses[0].in.data();
    }
}

float* RigEngine::throwInput(int bus)
{
    switch (bus)
    {
        case CargoHold::c_hitsBus: return nullptr;
        case CargoHold::c_stowBus: m_stow.fed = true; return m_stow.throwIn.data();
        case 1:                    return m_buses[1].throwIn.data();
        default:                   return m_buses[0].throwIn.data();
    }
}

void RigEngine::FollowMakeup::measure(float inSq, const float keeps[4], const float typicalDb[4], float c)
{
    if (sent <= 1.0e-12f)
    {
        // Nothing measured yet (or it has all died away): start from what a typical sound costs.
        if (inSq > 1.0e-10f)
        {
            sent = inSq;
            for (size_t k = 0; k < kept.size(); ++k)
                kept[k] = inSq * std::pow(10.0f, -0.1f * typicalDb[k]);
        }
        return;
    }
    sent += (inSq - sent) * c;
    for (size_t k = 0; k < kept.size(); ++k)
        kept[k] += (inSq * keeps[k] - kept[k]) * c;
}

float RigEngine::FollowMakeup::costDb(float follow) const
{
    if (sent <= 1.0e-12f || follow <= 0.0f)
        return 0.0f;
    const float at = 4.0f * std::min(follow, 1.0f);
    const int k = std::min(3, static_cast<int>(at));
    auto cost = [&](int index) { return index <= 0 ? 0.0f : -10.0f * std::log10(sj::clamp(kept[static_cast<size_t>(index - 1)] / sent, 1.0e-6f, 1.0f)); };
    return sj::lerp(cost(k), cost(k + 1), at - static_cast<float>(k));
}

float RigEngine::wakeBloom(float out)
{
    // Up over the first quarter of the way out, then down over the rest (raised cosines: no step, no corner).
    if (out <= 0.0f || out >= 1.0f)
        return 0.0f;
    return out < 0.25f ? 0.5f - 0.5f * std::cos(sj::c_pi * out / 0.25f) : 0.5f + 0.5f * std::cos(sj::c_pi * (out - 0.25f) / 0.75f);
}

void RigEngine::StowTilt::process(float* x, int frames, float knob, float fs, bool snap)
{
    const float step = static_cast<float>(frames) / (c_tiltSeconds * fs);
    const float invFrames = 1.0f / static_cast<float>(frames);
    auto side = [&](bool lowpass, float hz, sj::Svf& f, float& mix)
    {
        const bool want = hz > 0.0f;
        const float mix0 = snap ? (want ? 1.0f : 0.0f) : mix;
        const float mix1 = want ? std::min(1.0f, mix0 + step) : std::max(0.0f, mix0 - step);
        mix = mix1;
        if (mix0 <= 0.0f && mix1 <= 0.0f)
            return;
        if (mix0 <= 0.0f)
            f.reset(); // coming in from nothing
        if (want)
            f.set(hz, 0.7071f, fs); // (going out, it stays where it was)
        for (int i = 0; i < frames; ++i)
        {
            const float y = lowpass ? f.lowpass(x[i]) : f.highpass(x[i]);
            x[i] += (mix0 + (mix1 - mix0) * static_cast<float>(i + 1) * invFrames) * (y - x[i]);
        }
    };
    side(true, ::Params::stowLowpassHz(knob), lp, lpMix);
    side(false, ::Params::stowHighpassHz(knob), hp, hpMix);
}

void RigEngine::StowSaturator::process(float* x, int frames, float drive, float fs)
{
    if (drive <= 1.0e-4f)
    {
        reset();
        return;
    }
    const float invFrames = 1.0f / static_cast<float>(frames);
    float preSq = 0.0f;
    for (int i = 0; i < frames; ++i)
        preSq += x[i] * x[i];
    preSq /= static_cast<float>(frames);
    const bool starting = pre <= 0.0f;
    pre = starting ? preSq : pre + (preSq - pre) * glide(frames, preSq > pre ? 0.03f : 1.5f, fs);
    const float rms = std::max(std::sqrt(pre), 1.0e-4f);
    const float into = c_subDriveFloor * sj::dbToGain(c_subDriveDb * drive) / rms; // the gain into the saturator
    const float mix = sj::clamp(drive * 20.0f, 0.0f, 1.0f); // in over the first 5%: 0% is the sound untouched
    float postSq = 0.0f;
    for (int i = 0; i < frames; ++i)
    {
        x[i] += mix * (sj::fastTanh(into * x[i]) / into - x[i]);
        postSq += x[i] * x[i];
    }
    postSq /= static_cast<float>(frames);
    post = starting ? postSq : post + (postSq - post) * glide(frames, postSq > post ? 0.03f : 1.5f, fs);
    const float want = post > 1.0e-12f ? std::min(c_subDriveMaxMatch, std::sqrt(pre / post)) : 1.0f;
    const float g0 = starting ? want : gain;
    gain = starting ? want : gain + (want - gain) * glide(frames, c_matchSeconds, fs);
    for (int i = 0; i < frames; ++i)
        x[i] *= g0 + (gain - g0) * static_cast<float>(i + 1) * invFrames;
}

void RigEngine::StowColour::prepare(float fs)
{
    lowA.set(130.0f, 0.7071f, fs);
    lowB.set(130.0f, 0.7071f, fs);
    shelf.set(110.0f, 0.7071f, fs);
    lowAdd.prepare(fs);
}

void RigEngine::StowColour::reset()
{
    for (auto* f : { &lowA, &lowB, &shelf })
        f->reset();
    lowAdd.reset();
    lowOn = false;
    lowAmount = 0.0f;
    tilt.reset();
    drive.reset();
}

void RigEngine::StowColour::process(float* x, int frames, const StowParams& now, float fs, bool snap)
{
    // Low End, as a bus has it: a note an octave under the low band (under 130 Hz) and its overtones, and a lift.
    const float low1 = now.low, low0 = snap ? low1 : lowAmount;
    lowAmount = low1;
    if (low0 > 1.0e-4f || low1 > 1.0e-4f)
    {
        lowOn = true;
        const float lift0 = sj::dbToGain(c_stowShelfDb * low0) - 1.0f, lift1 = sj::dbToGain(c_stowShelfDb * low1) - 1.0f;
        const float invFrames = 1.0f / static_cast<float>(frames);
        for (int i = 0; i < frames; ++i)
        {
            const float t = static_cast<float>(i + 1) * invFrames;
            const float low = lowB.lowpass(lowA.lowpass(x[i]));
            x[i] += (low0 + (low1 - low0) * t) * lowAdd.process(low) + (lift0 + (lift1 - lift0) * t) * shelf.lowpass(x[i]);
        }
    }
    else if (lowOn)
    {
        for (auto* f : { &lowA, &lowB, &shelf })
            f->reset();
        lowAdd.reset();
        lowOn = false;
    }
    tilt.process(x, frames, now.filter, fs, snap);
    drive.process(x, frames, now.drive, fs);
}

void RigEngine::StowState::Wake::reset()
{
    colour.reset();
    on = false;
    fall = amount = gain = pan = 0.0f;
    envDb = -60.0f;
    makeup.reset();
}

void RigEngine::StowState::Dry::reset()
{
    filter.reset();
    sideFilter.reset();
    on = sideOn = false;
    sideTail = 0;
    gain = width = 1.0f;
    levelMid = levelSide = 0.0f;
    level = 1.0f;
}

void RigEngine::StowState::Fly::reset()
{
    line.reset();
    throwLine.reset();
    colour.reset();
    throwColour.reset();
    for (auto& h : heads)
        h = Head {};
    for (auto& h : throwHeads)
        h = Head {};
    on = throwOn = false;
    hold = throwHold = 0;
    inGain = 0.0f;
    level = 1.0f;
    segTime = 0.0;
    segLen = 1.5f;
    inbound = 0;
}

void RigEngine::StowState::leaveMode()
{
    placed = false; // a room: the next mode's lanes start at their own places
    if (sub.on)
        sub.reset();
    if (wake.on)
        wake.reset();
    if (dry.on)
        dry.reset();
    if (fly.on)
        fly.reset();
}

void RigEngine::renderStow(int frames)
{
    auto& s = m_stow;
    // The knobs (after their springs) plus any modulator offsets: what this chunk is rendered with.
    const float seconds = static_cast<float>(frames) / m_fs;
    float* const knobs[c_stowKnobs] = { &s.now.follow, &s.now.low, &s.now.filter, &s.now.drive, &s.now.width };
    for (size_t k = 0; k < s.springs.size(); ++k)
        *knobs[k] = sj::clamp(s.springs[k].step(seconds) + m_stowModNow[k], 0.0f, 1.0f);

    // The mode. A change (by hand or by automation) fades what is sounding out, swaps at silence (see the end),
    // and fades the new mode in: about 30 ms each way. With nothing sounding it just changes.
    const int wanted = juce::jlimit(0, c_stowModes - 1, m_params.stow.mode);
    // The switch. Off fades what is sounding out (60 ms) and then nothing here runs at all; on fades it back in.
    const float gate0 = s.gate;
    if (! m_params.stow.on && gate0 <= 0.0f)
    {
        s.leaveMode();
        s.mode = s.now.mode = wanted;
        s.fade = 1.0f;
        s.sounding = false;
        s.throwKey = 0.0f;
        s.panFrom = s.pan = 0.0f;
        s.placed = false;
        s.env = s.follower.env = 0.0f;
        return;
    }
    s.gate = m_params.stow.on ? std::min(1.0f, gate0 + seconds / c_stowGateSeconds) : std::max(0.0f, gate0 - seconds / c_stowGateSeconds);
    const float gate1 = s.gate;
    if (wanted != s.mode && ! s.fed && ! (s.mode == StowSub && s.sub.on) && ! (s.mode == StowFly && s.fly.on))
    {
        s.leaveMode();
        s.mode = wanted;
        s.fade = 1.0f;
    }
    const float fade0 = s.fade;
    s.fade = wanted != s.mode ? std::max(0.0f, fade0 - seconds / c_stowFadeSeconds) : std::min(1.0f, fade0 + seconds / c_stowFadeSeconds);
    const float fade1 = s.fade;
    const int mode = s.now.mode = s.mode;

    // A throw: what the ride holds back of each track arrives for as long as its sources are open, so its share is
    // shaped here by the throw's own pass (bus A's copy of it, already moved on by this chunk): it swells to the
    // throw's peak and is gone with its tail, the same from rest as with the rig playing. In Wake it is the
    // throw's trail instead: it blooms behind the throw's peak and is gone with its tail too.
    float key = 0.0f;
    for (size_t v = 0; v < m_throws.size(); ++v)
        if (m_throws[v].active)
        {
            const float u = m_buses[0].throwHeads[v].progress();
            key = std::max(key, (mode == StowWake ? wakeBloom((2.0f * u - 1.0f) / c_flyTail) : passWindow(u, 1.0f)) * m_throws[v].level);
        }
    const float key0 = s.throwKey;
    s.throwKey = key;

    // Planted in the middle (Wake moves it: see renderWake).
    s.panFrom = s.pan;
    s.pan = 0.0f;

    // Nothing sent here: no sound, and nothing for the mix to add (see process()). A sub's filters ring on for a
    // moment after its tracks stop, so it stays in until they are quiet.
    const bool isSub = mode == StowSub, isFly = mode == StowFly;
    if (isSub && s.fed)
        s.sub.tail = static_cast<int>(c_subTailSeconds * m_fs);
    s.sounding = s.fed || (isSub && s.sub.on && s.sub.tail > 0);
    if (isFly)
    {
        // Fly sleeps unless sound is coming in or is still on its way through its lines (a pass hears what was
        // sent up to four seconds ago): a loaded track that is silent, or a stopped rig, costs nothing.
        auto& f = s.fly;
        bool any = false, anyThrow = false;
        if (s.fed)
            for (int i = 0; i < frames; ++i)
            {
                any = any || std::abs(s.in[static_cast<size_t>(i)]) > c_flyQuiet;
                anyThrow = anyThrow || std::abs(s.throwIn[static_cast<size_t>(i)]) > c_flyQuiet;
            }
        f.hold = any ? f.line.maxDelay() : std::max(0, f.hold - frames);
        f.throwHold = anyThrow ? f.throwLine.maxDelay() : std::max(0, f.throwHold - frames);
        s.sounding = f.hold > 0 || f.throwHold > 0;
    }
    if (! s.sounding)
    {
        s.env = s.follower.env = 0.0f;
        s.placed = false;
        s.path.snap(juce::jlimit(0, room::c_numPaths - 1, m_params.stow.path));
        s.leaveMode();
        return;
    }
    const float invFrames = 1.0f / static_cast<float>(frames);
    s.at = {}; // a room: at the front, unless Wake or Fly says otherwise
    if (mode == StowWake)
        renderWake(frames, key0, key);
    else if (isFly)
        renderFly(frames);
    else
    {
        for (int i = 0; i < frames; ++i)
        {
            const auto ii = static_cast<size_t>(i);
            s.mono[ii] = s.in[ii] + s.throwIn[ii] * (key0 + (key - key0) * static_cast<float>(i + 1) * invFrames);
        }
        if (isSub)
            renderSub(frames);
        else if (mode == StowDry)
            renderDry(frames);
    }
    if (fade0 < 1.0f || fade1 < 1.0f || gate0 < 1.0f || gate1 < 1.0f)
    {
        // The Mode's fade and the switch's, as one gain.
        const bool sides = mode == StowDry && s.dry.sideOn;
        const float g0 = fade0 * gate0, g1 = fade1 * gate1;
        for (int i = 0; i < frames; ++i)
        {
            const auto ii = static_cast<size_t>(i);
            const float g = g0 + (g1 - g0) * static_cast<float>(i + 1) * invFrames;
            s.mono[ii] *= g;
            if (sides)
                s.side[ii] *= g;
            if (isFly)
            {
                s.l[ii] *= g;
                s.r[ii] *= g;
            }
        }
    }
    if (isFly && m_room) // three lanes (see renderFly), each as loud as a centred pass is in left and right
        for (int i = 0; i < frames; ++i)
            s.env = s.follower.process(s.l[static_cast<size_t>(i)] + s.r[static_cast<size_t>(i)] + s.mono[static_cast<size_t>(i)]);
    else
        for (int i = 0; i < frames; ++i)
            s.env = s.follower.process(isFly ? 0.5f * (s.l[static_cast<size_t>(i)] + s.r[static_cast<size_t>(i)]) : s.mono[static_cast<size_t>(i)]);
    // Faded right out: the wanted mode takes over from the next chunk, from clean state.
    if (wanted != s.mode && fade1 <= 0.0f)
    {
        s.leaveMode();
        s.mode = wanted;
    }
}

float RigEngine::flySegmentSeconds(float free, float toPeak, float toSecond, float halfA, float follow)
{
    const float own = sj::clamp(free, c_flyHalfMin, c_flyHalfMax);
    if (follow <= 0.0f)
        return own;
    // The peak to land on: of bus A's peaks that are known for certain (the next one, and the one after it while
    // A's first pass is in its first half), the nearest to where this half would have ended by itself. A peak too
    // close to reach is left to go by; with none in reach, the best guess at the ones after (half of A's pass in
    // flight apart: A draws each pass's length as it starts) stands in, and the next half puts it right.
    float best = 0.0f, bestMiss = 1.0e9f;
    auto offer = [&](float peak)
    {
        if (peak < c_flyHalfMin || peak > c_flyHalfMax)
            return;
        const float miss = std::abs(std::log(peak / own));
        if (miss < bestMiss)
        {
            bestMiss = miss;
            best = peak;
        }
    };
    offer(toPeak);
    offer(toSecond);
    if (best <= 0.0f)
    {
        const float step = std::max(halfA, 0.5f * c_flyHalfMin);
        for (float peak = std::max({ toPeak, toSecond, 0.0f }) + step; peak <= c_flyHalfMax; peak += step)
        {
            offer(peak);
            if (peak > own)
                break;
        }
    }
    if (best <= 0.0f)
        return own;
    return sj::clamp(own * std::pow(best / own, sj::clamp(follow, 0.0f, 1.0f)), c_flyHalfMin, c_flyHalfMax);
}

void RigEngine::renderFly(int frames)
{
    auto& s = m_stow;
    auto& f = s.fly;
    const float fs = m_fs;
    const double dt = 1.0 / m_sampleRate;
    const bool starting = ! f.on; // from sleep: nothing to carry on from
    f.on = true;

    // The stream: what was sent here, through Low End, Filter and Drive, into the line the passes read. (Coloured
    // before it flies, so it stays one stream with a place: a room can put each pass where it is.)
    std::copy(s.in.begin(), s.in.begin() + frames, s.mono.begin());
    f.colour.process(s.mono.data(), frames, s.now, fs, starting);
    {
        // Waking with sound already playing (a change of Mode): it fades into the line, or the passes would hear
        // the line start on a step a moment later.
        const float in0 = starting ? 0.0f : f.inGain;
        f.inGain = std::min(1.0f, in0 + static_cast<float>(frames) / (c_busFadeSeconds * fs));
        const float inStep = (f.inGain - in0) / static_cast<float>(frames);
        for (int i = 0; i < frames; ++i)
            f.line.push(s.mono[static_cast<size_t>(i)] * (in0 + inStep * static_cast<float>(i + 1)));
    }
    // A throw's feed (the tracks at their fader levels) has its own line, as on a bus, and the same colours.
    const bool throwing = f.throwHold > 0;
    if (throwing)
    {
        auto& feed = s.side; // (Fly has no side signal: the buffer is free)
        std::copy(s.throwIn.begin(), s.throwIn.begin() + frames, feed.begin());
        f.throwColour.process(feed.data(), frames, s.now, fs, ! f.throwOn);
        f.throwOn = true;
        for (int i = 0; i < frames; ++i)
            f.throwLine.push(feed[static_cast<size_t>(i)]);
    }
    else if (f.throwOn)
    {
        f.throwColour.reset();
        f.throwOn = false;
    }

    // The passes: always Fly-Bys, a fixed Doppler and Bend, how close and which way from Fly's own draws.
    BusParams pass;
    pass.doppler = c_flyDoppler;
    pass.bend = c_flyBend;
    pass.direction = m_params.stow.direction;
    pass.directionWired = m_params.stow.directionWired;
    pass.directionMod = m_params.stow.directionMod;
    pass.side = m_params.stow.side;
    pass.sideWired = m_params.stow.sideWired;
    pass.sideMod = m_params.stow.sideMod;
    const float law = passWidthLaw(s.now.width);
    // A room: each pass is one stream at a place on Fly's own Flight Path, read once (see the class comment).
    const bool room = m_room;
    const float orbit0 = starting ? 0.0f : s.path.orbit();
    if (starting)
        s.path.snap(juce::jlimit(0, room::c_numPaths - 1, m_params.stow.path));
    else
        s.path.step(juce::jlimit(0, room::c_numPaths - 1, m_params.stow.path), static_cast<float>(frames) / fs);
    const float orbit1 = s.path.orbit();
    const float orbitFrom = starting ? orbit1 : orbit0, orbitStep = (orbit1 - orbitFrom) / static_cast<float>(frames);
    const auto& kernel = sj::SincKernel::get();
    // How long the next half of a pass is, chosen `over` seconds into it at sample i: Fly's own draw (the length
    // Rate gives, varied by Chaos), pulled by Follow towards landing on bus A's next peaks. Bus A has rendered
    // this chunk already, so its first pass's clock stands at the chunk's end. The aim is two samples past A's
    // peak: a half that ends there is chosen after A has drawn its next pass, so the peak after it is known for
    // certain too, and a pass that has landed stays landed.
    auto nextHalf = [&](int i, double over)
    {
        const float own = 0.5f * sj::clamp(nominalPassSeconds() * std::exp2(m_params.chaos * 0.5f * f.rng.gauss()), 0.8f, 8.0f);
        const Head& a = m_buses[0].heads[0];
        const double halfA = 0.5 * static_cast<double>(a.length);
        const double left = a.time < halfA ? halfA - a.time : static_cast<double>(a.length) - a.time;
        const double toPeak = left + static_cast<double>(frames - i + 2) * dt + over;
        return flySegmentSeconds(own, static_cast<float>(toPeak), a.time < halfA ? static_cast<float>(toPeak + halfA) : -1.0f, static_cast<float>(halfA), s.now.follow);
    };
    if (starting)
    {
        // One pass at its closest (so it is heard at once), the other setting off.
        const float half = nextHalf(0, 0.0);
        startPass(f.heads[1], pass, half, &f.rng);
        startPass(f.heads[0], pass, half, &f.rng);
        f.heads[1].time = static_cast<double>(half);
        f.inbound = 0;
        f.segTime = 0.0;
        f.segLen = half;
        for (auto& h : f.heads)
        {
            h.airL.reset();
            h.airR.reset();
        }
    }
    std::fill(s.l.begin(), s.l.begin() + frames, 0.0f);
    std::fill(s.r.begin(), s.r.begin() + frames, 0.0f);
    if (room)
        std::fill(s.mono.begin(), s.mono.begin() + frames, 0.0f); // the throws' lane (the stream itself is in the line by now)
    for (auto& h : f.heads)
    {
        if (room)
            roomAir(h, c_flyDoppler, orbit1, fs, false, 1.0f);
        else
            passAir(h, c_flyDoppler, law, fs, false);
    }
    // How loud: on average as loud as the other modes are with the same sound. Its tracks are ridden like any
    // others, and the deeper they dip the quieter they are on average, so a make-up brings them back to their
    // fader level as Dry's does (never by more than 6 dB); c_flyLevel then makes up for the passes themselves.
    // (A throw is not ridden and is one pass, the same on every bus: it keeps its own level, below.)
    if (std::abs(m_makeupDepth - f.powerDepth) > 1.0e-4f || std::abs(m_makeupChaos - f.powerChaos) > 1.0e-4f)
    {
        f.powerDepth = m_makeupDepth;
        f.powerChaos = m_makeupChaos;
        f.power = ridePower(f.powerDepth, f.powerChaos, 1.0f);
    }
    const float level1 = c_flyLevel * std::min(2.0f, 1.0f / std::sqrt(std::max(f.power, 1.0e-3f)));
    const float level0 = starting ? level1 : f.level;
    f.level = starting ? level1 : f.level + (level1 - f.level) * glide(frames, 0.05f, fs);
    const float levelStep = (f.level - level0) / static_cast<float>(frames);
    const bool marks = m_releaseGain > 0.0f && s.env >= 0.002f;
    for (int i = 0; i < frames; ++i)
    {
        if (f.segTime >= static_cast<double>(f.segLen))
        {
            // The half is over: the head on its way in is at its closest and learns how long its way out is; the
            // other has gone, and sets off again on a new pass that long. (The part of a sample over is kept.)
            const double over = f.segTime - static_cast<double>(f.segLen);
            const float half = nextHalf(i, over);
            auto& arrived = f.heads[static_cast<size_t>(f.inbound)];
            arrived.lenOut = half;
            arrived.length = arrived.lenIn + half;
            startPass(f.heads[static_cast<size_t>(1 - f.inbound)], pass, half, &f.rng);
            f.inbound = 1 - f.inbound;
            f.segTime = over;
            f.segLen = half;
            if (marks)
                mark(MarkPassStow, i);
        }
        auto& in = f.heads[static_cast<size_t>(f.inbound)];
        auto& out = f.heads[static_cast<size_t>(1 - f.inbound)];
        in.time = f.segTime;
        out.time = static_cast<double>(out.lenIn) + f.segTime;
        const double back = static_cast<double>(frames - 1 - i);
        for (auto& h : f.heads)
        {
            const float u = h.progress();
            const float window = passWindow(u, 1.0f);
            if (window > 0.0f && room)
            {
                const PassPoint pt = roomPoint(h, u, law, orbitFrom + orbitStep * static_cast<float>(i + 1));
                const float gain = (level0 + levelStep * static_cast<float>(i + 1)) * window * c_flyMakeup * (1.0f + 0.8f * c_flyDoppler * (pt.loud - 0.35f));
                auto& lane = &h == &f.heads[0] ? s.l : s.r;
                lane[static_cast<size_t>(i)] += h.airL.lowpass(f.line.readSinc(pt.base * m_sampleRate + back, pt.stretch, kernel)) * gain;
            }
            else if (window > 0.0f)
            {
                const PassPoint pt = passPoint(h, u, law);
                const float gain = (level0 + levelStep * static_cast<float>(i + 1)) * window * c_flyMakeup * (1.0f + 0.8f * c_flyDoppler * (pt.nearness - 0.35f));
                hearPass(h, f.line, pt, back, gain, s.l[static_cast<size_t>(i)], s.r[static_cast<size_t>(i)]);
            }
        }
        f.segTime += dt;
    }
    // Where the passes are after this chunk, for the views; Stowaway's place is the louder one's.
    f.heads[static_cast<size_t>(f.inbound)].time = f.segTime;
    f.heads[static_cast<size_t>(1 - f.inbound)].time = static_cast<double>(f.heads[static_cast<size_t>(1 - f.inbound)].lenIn) + f.segTime;
    float panSum = 0.0f, weight = 1.0e-6f;
    for (auto& h : f.heads)
    {
        h.age = h.progress();
        h.view = passWindow(h.age, 1.0f);
        h.x = h.dir * std::tanh(5.0f * (h.age - 0.5f));
        panSum += h.view * passAzimuth(h.age, h.dir, law);
        weight += h.view;
    }
    s.pan = panSum / weight;
    if (starting)
        s.panFrom = s.pan;
    if (room)
        for (size_t k = 0; k < f.heads.size(); ++k)
        {
            const Place place = placeOf(f.heads[k], pathProgress(f.heads[k].age, 1.0f), law, s.path);
            s.at[k] = place.position;
            roomGains(place.position, place.distance, s.gainTo[k]);
        }

    // Throws: each one is flown here too, the same pass as on bus A (when, how long, how close, which way), at
    // Fly's own Bend and Width, reading the tracks at their fader levels.
    if (throwing)
        for (size_t v = 0; v < m_throws.size(); ++v)
        {
            if (! m_throws[v].active)
                continue;
            auto& h = f.throwHeads[v];
            const Head& a = m_buses[0].throwHeads[v]; // bus A has flown this chunk: its copy stands at the chunk's end
            followPass(h, a, c_flyBend, true);
            h.time = a.time - static_cast<double>(frames) * dt;
            if (room)
                roomAir(h, c_flyDoppler, m_throwOrbit1, fs, false, 1.0f);
            else
                passAir(h, c_flyDoppler, law, fs, false);
            const float level = m_throws[v].level;
            const float throwOrbitStep = (m_throwOrbit1 - m_throwOrbit0) / static_cast<float>(frames);
            for (int i = 0; i < frames; ++i)
            {
                const float u = h.progress();
                const float window = passWindow(u, 1.0f);
                if (window > 0.0f && room)
                {
                    // A throw flies bus A's Flight Path wherever it is heard.
                    const PassPoint pt = roomPoint(h, u, law, m_throwOrbit0 + throwOrbitStep * static_cast<float>(i + 1));
                    const float gain = window * c_flyMakeup * (1.0f + 0.8f * c_flyDoppler * (pt.loud - 0.35f)) * level;
                    s.mono[static_cast<size_t>(i)] += h.airL.lowpass(f.throwLine.readSinc(pt.base * m_sampleRate + static_cast<double>(frames - 1 - i), pt.stretch, kernel)) * gain;
                }
                else if (window > 0.0f)
                {
                    const PassPoint pt = passPoint(h, u, law);
                    const float gain = window * c_flyMakeup * (1.0f + 0.8f * c_flyDoppler * (pt.nearness - 0.35f)) * level;
                    hearPass(h, f.throwLine, pt, static_cast<double>(frames - 1 - i), gain, s.l[static_cast<size_t>(i)], s.r[static_cast<size_t>(i)]);
                }
                h.time += dt;
            }
        }
    if (room && ! throwGains(f.throwHeads, law, s.gainTo[2], nullptr))
        s.gainTo[2] = s.gain[2];
}

void RigEngine::renderWake(int frames, float key0, float key1)
{
    auto& s = m_stow;
    auto& w = s.wake;
    const float fs = m_fs;
    const bool starting = ! w.on; // from silence: nothing to glide from
    w.on = true;
    const auto& a = m_buses[0];

    // Bus A falling away, by its level: how far A has dropped (in dB) from what it was. Every drop adds to it,
    // every rise takes away from it, and it dies away by itself over about a quarter of a pass: so the trail opens
    // as A falls (fully once A is 12 dB down), shuts as A comes back, and after a fall that stays down (or A
    // stopping) it blooms and then goes. Levels under -60 dBFS are all the same silence, and with what A fell from
    // under -50 dBFS there is no bus A to trail: a bus idling on noise cannot hold it open.
    const float envDb = std::max(-60.0f, 20.0f * std::log10(std::max(a.env, 1.0e-6f)));
    w.fall = starting ? 0.0f : std::max(0.0f, w.fall + w.envDb - envDb) * std::exp(-static_cast<float>(frames) / (c_wakeTrailPasses * nominalPassSeconds() * fs));
    w.envDb = envDb;
    const float heard = sj::clamp((envDb + w.fall + 50.0f) / 20.0f, 0.0f, 1.0f);
    const float fallen = heard * sj::clamp(w.fall / c_wakeFallDb, 0.0f, 1.0f);

    // And by its passes: one of A's two is always on its way out, and the trail blooms just behind it (wakeBloom).
    // That counts as far as the passes are what makes A rise and fall: all the way in Fly-By, and in Bed the stiller
    // the faders are (a ridden bed swells with its rides: its level, above, says when it falls away). With no bus A
    // to hear, the passes are all there is to go by, so Wake is never stuck silent. A throw's trail always counts.
    // The trail sits behind its pass: it sets off the same way from the middle and gets half as far.
    float passBloom = 0.0f, throwBloom = 0.0f, panSum = 0.0f, weight = 0.0f;
    // A room: the trail is on bus A's Flight Path, as far behind its pass (Width: how much of that way it goes).
    room::Position placeSum { 0.0f, 0.0f, 0.0f };
    float distanceSum = 0.0f;
    const float trailLaw = m_throwWidthLaw * passWidthLaw(s.now.width);
    auto trail = [&](const Head& h, float out, float level, float& bloom, float shape)
    {
        const float b = wakeBloom(out) * level;
        bloom = std::max(bloom, b);
        panSum += (b + 1.0e-3f) * h.dir * std::pow(c_wakeLag * sj::clamp(out, 0.0f, 1.0f), 0.6f);
        weight += b + 1.0e-3f;
        if (m_room)
        {
            const Place place = placeOf(h, 0.5f + c_wakeLag * (pathProgress(h.progress(), shape) - 0.5f), trailLaw, a.path);
            placeSum.x += (b + 1.0e-3f) * place.position.x;
            placeSum.y += (b + 1.0e-3f) * place.position.y;
            placeSum.z += (b + 1.0e-3f) * place.position.z;
            distanceSum += (b + 1.0e-3f) * place.distance;
        }
    };
    for (const auto& h : a.heads)
        if (const float u = h.progress(); u > 0.5f)
            trail(h, 2.0f * u - 1.0f, 1.0f, passBloom, a.passShape);
    for (size_t v = 0; v < m_throws.size(); ++v)
        if (const float u = a.throwHeads[v].progress(); m_throws[v].active && u > 0.5f)
            trail(a.throwHeads[v], (2.0f * u - 1.0f) / c_flyTail, m_throws[v].level, throwBloom, 1.0f);
    const float byPass = std::max({ a.passShape, m_proxBlend, 1.0f - heard });
    const float open = 1.0f - (1.0f - fallen) * (1.0f - byPass * passBloom) * (1.0f - sj::clamp(throwBloom, 0.0f, 1.0f));
    w.amount = starting ? open : w.amount + (open - w.amount) * glide(frames, c_wakeAmountSeconds, fs);

    // Follow: how far the trail shuts between passes, in dB, as Sub's Follow: 0% is a steady layer, 100% is silent
    // until bus A falls away. How much of the time the trail is open depends on how bus A moves (a few seconds in
    // every pass of a bed, most of the time behind fly-bys or a gappy sound), so the make-up that keeps it as loud
    // on average wherever the knob is cannot be one figure: it is measured (see FollowMakeup), and never more than
    // 12 dB. It starts from what a typical sound on bus A needs.
    {
        float inSq = 0.0f;
        for (int i = 0; i < frames; ++i)
            inSq += s.in[static_cast<size_t>(i)] * s.in[static_cast<size_t>(i)];
        inSq /= static_cast<float>(frames);
        const float amt = sj::clamp(w.amount, 1.0e-3f, 1.0f), root = std::sqrt(amt);
        const float keeps[4] = { root, amt, amt * root, amt * amt }; // the power kept at Follow 25, 50, 75, 100%
        float typical[4];
        for (int k = 0; k < 4; ++k)
        {
            const float f = 0.25f * static_cast<float>(k + 1);
            typical[k] = sj::lerp(11.9f * std::pow(f, 0.82f), 4.0f * std::pow(f, 0.55f), byPass);
        }
        w.makeup.measure(inSq, keeps, typical, glide(frames, c_followMatchPasses * nominalPassSeconds(), fs));
    }
    const float makeupDb = std::min(c_wakeFollowMakeupDb, w.makeup.costDb(s.now.follow));
    const float gain1 = sj::dbToGain(makeupDb) * subFollowGain(s.now.follow, w.amount);
    const float gain0 = starting ? gain1 : w.gain;
    w.gain = gain1;

    // Its place, by Width (the law the buses' Width has).
    if (weight > 0.0f)
    {
        const float panTo = panSum / weight;
        w.pan = starting ? panTo : w.pan + (panTo - w.pan) * glide(frames, c_wakePanSeconds, fs);
    }
    s.pan = std::pow(s.now.width, 0.8f) * w.pan;
    if (starting)
        s.panFrom = s.pan;
    if (m_room)
    {
        if (weight > 0.0f)
        {
            const float c = (starting || ! s.placed) ? 1.0f : glide(frames, c_wakePanSeconds, fs);
            s.wakeAt.x += (placeSum.x / weight - s.wakeAt.x) * c;
            s.wakeAt.y += (placeSum.y / weight - s.wakeAt.y) * c;
            s.wakeAt.z += (placeSum.z / weight - s.wakeAt.z) * c;
            s.wakeDistance += (distanceSum / weight - s.wakeDistance) * c;
        }
        s.at[0] = s.wakeAt;
        roomGains(s.wakeAt, s.wakeDistance, s.gainTo[0]);
    }

    const float invFrames = 1.0f / static_cast<float>(frames);
    for (int i = 0; i < frames; ++i)
    {
        const auto ii = static_cast<size_t>(i);
        const float t = static_cast<float>(i + 1) * invFrames;
        s.mono[ii] = s.in[ii] * (gain0 + (gain1 - gain0) * t) + s.throwIn[ii] * (key0 + (key1 - key0) * t);
    }
    w.colour.process(s.mono.data(), frames, s.now, fs, starting);
}

void RigEngine::renderDry(int frames)
{
    auto& s = m_stow;
    auto& d = s.dry;
    const float fs = m_fs;
    const bool starting = ! d.on;
    d.on = true;

    // Follow is how much of their ride the tracks keep (see renderTracks). The deeper they dip, the quieter they are
    // on average, so a make-up brings the ridden sound's average back to its fader level (never by more than 6 dB;
    // nothing with the faders still), as Sub's and Wake's do: on average Stowaway is as loud in every mode. (The
    // mix's own ride make-up comes on top, as for everything else: see process().)
    if (std::abs(m_makeupDepth - d.powerDepth) > 1.0e-4f || std::abs(m_makeupChaos - d.powerChaos) > 1.0e-4f || std::abs(s.now.follow - d.powerFollow) > 1.0e-3f)
    {
        d.powerDepth = m_makeupDepth;
        d.powerChaos = m_makeupChaos;
        d.powerFollow = s.now.follow;
        d.power = ridePower(d.powerDepth, d.powerChaos, d.powerFollow);
    }
    const float gain1 = std::min(2.0f, 1.0f / std::sqrt(std::max(d.power, 1.0e-3f)));
    const float gain0 = starting ? gain1 : d.gain;
    d.gain = starting ? gain1 : d.gain + (gain1 - d.gain) * glide(frames, 0.05f, fs);
    const float width0 = starting ? s.now.width : d.width;
    d.width = s.now.width;

    // The sides of its stereo tracks: left is the sum plus them, right the sum minus them, by Width (100% = as wide
    // as the file). They go through the same Filter.
    if (s.sideFed)
        d.sideTail = static_cast<int>(c_drySideTailSeconds * fs);
    else
        d.sideTail = std::max(0, d.sideTail - frames);
    const bool sides = s.sideFed || (d.sideOn && d.sideTail > 0);
    if (sides)
    {
        std::copy(s.sideIn.begin(), s.sideIn.begin() + frames, s.side.begin());
        d.sideFilter.process(s.side.data(), frames, s.now.filter, fs, ! d.sideOn);
        d.sideOn = true;
    }
    else if (d.sideOn)
    {
        d.sideFilter.reset();
        d.sideOn = false;
    }
    d.filter.process(s.mono.data(), frames, s.now.filter, fs, starting);
    const float invFrames = 1.0f / static_cast<float>(frames);
    for (int i = 0; i < frames; ++i)
    {
        const auto ii = static_cast<size_t>(i);
        const float t = static_cast<float>(i + 1) * invFrames;
        const float g = gain0 + (d.gain - gain0) * t;
        s.mono[ii] *= g;
        if (sides)
            s.side[ii] *= g * (width0 + (d.width - width0) * t);
    }
    // As loud wide as narrow (as a stereo bed on bus B is: see renderBus): left and right together have the sum's
    // power plus the sides', and are brought to the sum's. Not at Mono, where these sides fold away exactly.
    if (sides || d.level < 0.9999f)
    {
        float midSq = 0.0f, sideSq = 0.0f;
        for (int i = 0; i < frames && sides; ++i)
        {
            midSq += s.mono[static_cast<size_t>(i)] * s.mono[static_cast<size_t>(i)];
            sideSq += s.side[static_cast<size_t>(i)] * s.side[static_cast<size_t>(i)];
        }
        const float c = starting ? 1.0f : glide(frames, c_bedLevelSeconds, fs);
        d.levelMid += (midSq * invFrames - d.levelMid) * c;
        d.levelSide += (sideSq * invFrames - d.levelSide) * c;
        const float want = m_width == room::Width::Mono || d.levelMid + d.levelSide < 1.0e-12f ? 1.0f : std::sqrt(d.levelMid / (d.levelMid + d.levelSide));
        const float level0 = starting ? want : d.level;
        d.level = starting ? want : d.level + (want - d.level) * c;
        if (! sides && d.level >= 0.9999f)
            d.level = 1.0f;
        for (int i = 0; i < frames; ++i)
        {
            const float g = level0 + (d.level - level0) * static_cast<float>(i + 1) * invFrames;
            s.mono[static_cast<size_t>(i)] *= g;
            if (sides)
                s.side[static_cast<size_t>(i)] *= g;
        }
    }
    else
        d.levelMid = d.levelSide = 0.0f;
}

void RigEngine::StowState::Sub::reset()
{
    filter.reset();
    low.reset();
    lowHp.reset();
    on = false;
    tail = 0;
    hz = 0.0f;
    lowAmount = 0.0f;
    gain = 0.0f;
    key = 1.0f;
    busA = busB = 0.0f;
    peak = { 1.0e-3f, 1.0e-3f };
    makeup.reset();
    drive.reset();
}

float RigEngine::subFollowGain(float follow, float key)
{
    // The swell it goes by, taken on in dB: at 100% the sub rises and falls exactly as far as bus A does, at 50%
    // half as far, at 0% not at all. (Nothing is ever further down than 60 dB: silence.)
    return follow <= 0.0f ? 1.0f : std::pow(sj::clamp(key, 1.0e-3f, 1.0f), std::min(follow, 1.0f));
}

void RigEngine::renderSub(int frames)
{
    auto& s = m_stow;
    auto& sub = s.sub;
    const float fs = m_fs;
    const bool starting = ! sub.on; // from silence: nothing to glide from
    sub.on = true;
    if (! s.fed)
        sub.tail -= frames;

    // Filter: the crossover. The filters are retuned as it glides (they are safe to move).
    const float hz = ::Params::stowCrossoverHz(s.now.filter);
    if (starting || std::abs(hz - sub.hz) > 1.0e-4f * sub.hz)
    {
        sub.hz = hz;
        sub.filter.set(static_cast<double>(hz), m_sampleRate);
        // Low End's sub octave stays under the crossover, and its overtones sit just above it (under the safety
        // low-pass): they are what lets a small speaker hear the note.
        sub.low.tune(static_cast<float>(SubFilter::c_highpassHz), std::min(90.0f, hz), 1.2f * hz, fs);
    }

    // Follow: bus A's swell: its level against its recent peak and, the stiller the faders are, how near its pass
    // is (what its own colours follow, but against a peak that falls back more slowly, so the whole rise and fall
    // of a pass is in it, not only its top). With no track on bus A it is bus B's; with none there either the sub's
    // own tracks are ridden instead (see renderTracks). The changeover glides, so a track arriving on a bus or
    // leaving it never steps the sub.
    const float peakFall = std::exp(-static_cast<float>(frames) / (c_subPeakPasses * nominalPassSeconds() * fs));
    float swell[c_maxBuses] {};
    for (size_t b = 0; b < m_buses.size(); ++b)
    {
        const auto& bus = m_buses[b];
        sub.peak[b] = std::max({ bus.env, sub.peak[b] * peakFall, 1.0e-3f });
        swell[b] = sj::clamp(bus.env / sub.peak[b], 0.0f, 1.0f) * (1.0f - m_proxBlend * (1.0f - bus.prox));
    }
    const float keyNow = sub.busA * swell[0] + (1.0f - sub.busA) * swell[1];
    sub.key = starting ? keyNow : sub.key + (keyNow - sub.key) * glide(frames, c_subKeySeconds, fs);
    const float byBus = 1.0f - (1.0f - sub.busA) * (1.0f - sub.busB); // the rest of Follow is its tracks' ride
    // The make-up that goes with it: on average the sub is as loud wherever the knob is. What Follow costs depends
    // on how the bus moves (little under a steady bed, a lot under fly-bys or a gappy sound), so it is measured
    // (see FollowMakeup; until it has been, a typical bed's figure), and never more than 6 dB: the sub's loudest
    // moments are never further up than that. The part of Follow that is its own tracks' ride takes the typical
    // figure.
    const float followed = s.now.follow * byBus;
    const float makeupDb = starting ? c_subFollowTypicalDb * s.now.follow
                                    : std::min(c_subFollowMakeupDb, sub.makeup.costDb(followed) + c_subFollowTypicalDb * (s.now.follow - followed));
    const float gain1 = sj::dbToGain(makeupDb) * subFollowGain(followed, sub.key);
    const float gain0 = starting ? gain1 : sub.gain;
    sub.gain = gain1;

    // The crossover, then Low End: a note an octave down and overtones, made from what the crossover let through.
    const float low1 = std::pow(s.now.low, 0.8f); // a little faster at the bottom of the knob: each 10% adds about as much
    const float low0 = starting ? low1 : sub.lowAmount;
    sub.lowAmount = low1;
    const bool lowOn = low0 > 1.0e-4f || low1 > 1.0e-4f;
    const float invFrames = 1.0f / static_cast<float>(frames);
    for (int i = 0; i < frames; ++i)
    {
        auto& x = s.mono[static_cast<size_t>(i)];
        x = static_cast<float>(sub.filter.band(static_cast<double>(x)));
        if (lowOn)
            x += (low0 + (low1 - low0) * static_cast<float>(i + 1) * invFrames) * sub.lowHp.highpass(sub.low.process(x));
    }

    // Drive (see StowSaturator): against the sub's own level, and level-matched.
    sub.drive.process(s.mono.data(), frames, s.now.drive, fs);

    // The safety low-pass (what Low End and Drive added stays within about an octave of the crossover), then Follow.
    float inSq = 0.0f;
    for (int i = 0; i < frames; ++i)
    {
        auto& x = s.mono[static_cast<size_t>(i)];
        x = static_cast<float>(sub.filter.safety(static_cast<double>(x)));
        inSq += x * x;
        x *= gain0 + (gain1 - gain0) * static_cast<float>(i + 1) * invFrames;
    }
    {
        // What Follow at 25, 50, 75 and 100% keeps of the sub's power, as it is being followed now.
        const float key = sj::clamp(sub.key, 1.0e-3f, 1.0f);
        const float quarter = std::pow(key, 0.5f * byBus), half = quarter * quarter;
        const float keeps[4] = { quarter, half, half * quarter, half * half };
        const float typical[4] = { 0.25f * c_subFollowTypicalDb * byBus, 0.5f * c_subFollowTypicalDb * byBus, 0.75f * c_subFollowTypicalDb * byBus, c_subFollowTypicalDb * byBus };
        sub.makeup.measure(inSq / static_cast<float>(frames), keeps, typical, glide(frames, c_followMatchPasses * nominalPassSeconds(), fs));
    }
}

void RigEngine::placeStow(int frames)
{
    // The stream at its place, as stereo: equal in both channels in the middle (as loud as the stream itself, like
    // the hits), turning to one side at constant power.
    auto& s = m_stow;
    if (s.now.mode == StowFly)
        return; // its passes are heard in stereo already (see renderFly)
    if (s.now.mode == StowDry && s.dry.sideOn)
    {
        // Dry with stereo tracks: the sum and their sides.
        for (int i = 0; i < frames; ++i)
        {
            const auto ii = static_cast<size_t>(i);
            s.l[ii] = s.mono[ii] + s.side[ii];
            s.r[ii] = s.mono[ii] - s.side[ii];
        }
        return;
    }
    auto gains = [](float pan, float& l, float& r)
    {
        const float a = 0.25f * sj::c_pi * (sj::clamp(pan, -1.0f, 1.0f) + 1.0f);
        l = 1.41421356f * std::cos(a);
        r = 1.41421356f * std::sin(a);
    };
    float l0 = 1.0f, r0 = 1.0f, l1 = 1.0f, r1 = 1.0f;
    if (s.panFrom != 0.0f || s.pan != 0.0f)
    {
        gains(s.panFrom, l0, r0);
        gains(s.pan, l1, r1);
    }
    const float invFrames = 1.0f / static_cast<float>(frames);
    for (int i = 0; i < frames; ++i)
    {
        const auto ii = static_cast<size_t>(i);
        const float t = static_cast<float>(i + 1) * invFrames;
        s.l[ii] = s.mono[ii] * (l0 + (l1 - l0) * t);
        s.r[ii] = s.mono[ii] * (r0 + (r1 - r0) * t);
    }
}

float RigEngine::keyLevel(const BusState& bus)
{
    const float rel = sj::clamp(bus.env / std::max(bus.peakRef, 1.0e-3f), 0.0f, 1.0f);
    const float db = 20.0f * std::log10(std::max(bus.env, 1.0e-6f));
    return rel * sj::clamp((db + 40.0f) / 20.0f, 0.0f, 1.0f); // nothing below -40 dBFS, full from -20 dBFS
}

//==============================================================================
// The doppler core (see PassPoint).
float RigEngine::passWidthLaw(float width) { return std::pow(width, 0.8f); }

float RigEngine::passAzimuth(float u, float dir, float widthLaw)
{
    // Where a pass is across the field (-1 left .. 1 right), scaled by Width: it pans the pass and sets the time
    // difference between the ears (so Width, not the amount of bend, sets how wide a pass is; Width 0 is centred).
    const float lateral = 2.0f * u - 1.0f;
    const float side = std::copysign(std::pow(std::abs(lateral), 0.6f), lateral);
    return widthLaw * sj::clamp(dir * side, -1.0f, 1.0f);
}

void RigEngine::passAir(Head& h, float doppler, float widthLaw, float fs, bool sides)
{
    // Air: the pass dulls with distance. By the pass's own speed, not the Bend-scaled one.
    const float s = static_cast<float>(h.time) - h.lenIn;
    const float x = h.machRef * s;
    const float dist = std::sqrt(x * x + h.closest * h.closest);
    const float u = h.progress();
    const float near = std::pow(sj::clamp(0.006f / dist, 0.0f, 1.0f), 0.6f);
    const float cutoff = sj::clamp(19000.0f * sj::lerp(1.0f, near * (0.3f + 0.7f * std::sin(sj::c_pi * u)), doppler), 600.0f, 20000.0f);
    // Head shadow: the ear turned away from the pass hears it duller, the further to the side it is. Only the
    // top goes (never under c_shadowFloor), so the two ears stay in step lower down and a mono sum is as
    // solid as without it.
    const float az = passAzimuth(u, h.dir, widthLaw);
    auto shadowed = [cutoff](float away) { return std::max(std::min(cutoff, c_shadowFloor), cutoff * std::exp2(-c_shadowOctaves * std::max(0.0f, away))); };
    h.airL.set(shadowed(az), fs);
    h.airR.set(shadowed(-az), fs);
    if (sides)
    {
        h.airSL.set(shadowed(az), fs);
        h.airSR.set(shadowed(-az), fs);
    }
}

RigEngine::PassPoint RigEngine::passPoint(const Head& h, float u, float widthLaw)
{
    PassPoint pt;
    pt.u = u;
    const double s = h.time - static_cast<double>(h.lenIn); // seconds from the closest point
    // The delay is the distance (in seconds of travel) the sound heard now was sent from: the source is
    // placed where it was when it made the sound, not where it is now, so the pitch rises by 1 / (1 - speed)
    // on the way in and falls by 1 / (1 + speed) on the way out, like a real pass-by. Solved in closed form:
    // d = sqrt((v (s - d))^2 + c^2). Kept in double all the way to the read.
    const double v = static_cast<double>(h.mach);
    const double c2 = static_cast<double>(h.closest) * static_cast<double>(h.closest);
    const double k2 = 1.0 - v * v;
    const double vs = v * s;
    const double root = std::sqrt(vs * vs + k2 * c2);
    const double delay = (root - v * vs) / k2;
    pt.az = passAzimuth(u, h.dir, widthLaw);
    pt.ear = static_cast<double>(c_earOffset * pt.az);
    pt.base = static_cast<double>(c_baseDelay) + delay;
    // Band-limited read: the delay shrinks on the way in, so the line is read faster than it was written
    // (by 1 / (1 - speed), up to twice as fast). The read's filter comes down with that speed, so the top
    // of the sound is filtered out instead of folding back as whistles that sweep the wrong way.
    // (A pass on Faders Together's clock runs at the clock's pace when Rate moves mid-pass: the delay moves that
    // much faster or slower.)
    const double readSpeed = 1.0 - static_cast<double>(h.pace) * ((v * vs / std::max(root, 1.0e-12) - v * v) / k2);
    pt.stretch = sj::SincKernel::stretchFor(readSpeed);
    // How near it is: by the pass's own speed, so Bend changes the pitch and leaves the swell alone.
    const float xRef = h.machRef * static_cast<float>(s);
    const float distRef = std::sqrt(xRef * xRef + h.closest * h.closest);
    pt.nearness = h.closest / std::max(distRef, 1.0e-6f);
    pt.loud = pt.nearness;
    const float theta = (0.95f * pt.az + 1.0f) * (sj::c_pi * 0.25f);
    pt.panL = std::cos(theta);
    pt.panR = std::sin(theta);
    return pt;
}

void RigEngine::hearPass(Head& h, const sj::DelayLine& line, const PassPoint& pt, double back, float gain, float& l, float& r) const
{
    const auto& kernel = sj::SincKernel::get();
    const float inL = line.readSinc((pt.base + pt.ear) * m_sampleRate + back, pt.stretch, kernel);
    const float inR = line.readSinc((pt.base - pt.ear) * m_sampleRate + back, pt.stretch, kernel);
    l += h.airL.lowpass(inL) * gain * pt.panL * 1.41421356f;
    r += h.airR.lowpass(inR) * gain * pt.panR * 1.41421356f;
}

void RigEngine::renderBus(BusState& bus, int frames, float key, bool isKeyBus,
                          const std::array<Head, 2>* lead, const std::array<Head, 2>* leadStart)
{
    // The knobs (after their springs) plus any modulator offsets.
    const size_t busIndex = static_cast<size_t>(&bus - m_buses.data());
    const auto& mod = m_busModNow[busIndex];
    // Linked: bus B flies A's passes with A's Doppler and Width, so it takes A's modulation of them too.
    const auto& leadMod = (m_linkNow && busIndex > 0) ? m_busModNow[0] : mod;
    const float seconds = static_cast<float>(frames) / m_fs; // what this chunk covers (the last of a block can be short)
    auto knob = [&](size_t k, float offset) { return sj::clamp(bus.springs[k].step(seconds) + offset, 0.0f, 1.0f); };
    BusParams p;
    p.doppler = knob(0, leadMod[0]);
    p.width = knob(1, leadMod[1]);
    p.bend = knob(2, mod[2]);
    p.low = knob(3, mod[3]);
    p.swirl = knob(4, mod[4]);
    p.filter = knob(5, mod[5]);
    p.drive = knob(6, mod[6]);
    p.duck = isKeyBus ? 0.0f : knob(7, mod[7]);
    p.key = isKeyBus ? 0.0f : knob(8, mod[8]);
    {
        const auto& own = m_params.buses[busIndex]; // (linked, bus B flies A's passes: A's way and side with them)
        p.direction = own.direction;
        p.directionWired = own.directionWired;
        p.directionMod = own.directionMod;
        p.side = own.side;
        p.sideWired = own.sideWired;
        p.sideMod = own.sideMod;
    }

    const float fs = m_fs;
    const double dt = 1.0 / m_sampleRate;

    // Pass Mode (linked: bus A's). The window glides from one shape to the other, ramped across the chunk.
    const int passMode = m_params.buses[(m_linkNow && busIndex > 0) ? 0 : busIndex].passMode;
    const float shape0 = bus.passShape;
    {
        const float want = passMode == 1 ? 1.0f : 0.0f;
        bus.passShape += (want - bus.passShape) * glide(frames, c_shapeSeconds, fs);
        if (std::abs(want - bus.passShape) < 1.0e-4f)
            bus.passShape = want;
    }
    const float shape1 = bus.passShape;
    const float shapeStep = (shape1 - shape0) / static_cast<float>(frames);
    const float widthLaw = passWidthLaw(p.width);
    bus.lawNow = widthLaw;
    float proximity = 0.0f; // of the nearer pass, at the end of this chunk
    // The same for each pass and for the throws, against what it is at that pass's closest point (0 far .. 1 there):
    // a Tape Flanger's copy moves With the Pass by it.
    std::array<float, 2> passNear {};
    float throwNear = 0.0f;
    // (At the closest point the window is 1 and so is the nearness, but for a pass round the listener, held at its
    // own level all the way.)
    auto nearAtClosest = [room = m_room](const Head& h, float orbit)
    {
        return room && orbit > 0.0f ? 0.1f + 0.9f * std::sqrt(sj::clamp(1.0f + orbit * (h.orbitLoud - 1.0f), 0.0f, 1.0f)) : 1.0f;
    };

    // A room (see the class comment): l and r are the two passes' lanes, t the throws'. The Flight Path (linked:
    // bus A's) glides; how far it is Around is ramped across the chunk, because it moves the passes' delay.
    const bool room = m_room;
    const float orbit0 = bus.path.orbit();
    bus.path.step(juce::jlimit(0, room::c_numPaths - 1, m_params.buses[(m_linkNow && busIndex > 0) ? 0 : busIndex].path), seconds);
    const float orbit1 = bus.path.orbit();
    const float orbitStep = (orbit1 - orbit0) / static_cast<float>(frames);
    if (busIndex == 0)
    {
        m_throwOrbit0 = orbit0;
        m_throwOrbit1 = orbit1;
    }
    // The throws' lane and its colour chain run while a throw is in flight and for a moment after (its tail).
    bool third = false, thirdNew = false;
    if (room)
    {
        bool throwing = false;
        for (const auto& v : m_throws)
            throwing = throwing || v.active;
        bus.thirdTail = throwing ? static_cast<int>(c_thirdTailSeconds * m_fs) : std::max(0, bus.thirdTail - frames);
        third = throwing || (bus.thirdOn && bus.thirdTail > 0);
    }
    if (third)
    {
        thirdNew = ! bus.thirdOn;
        if (thirdNew)
            bus.resetThird();
        bus.thirdOn = true;
        std::fill(bus.t.begin(), bus.t.begin() + frames, 0.0f);
    }
    else if (bus.thirdOn)
        bus.resetThird();

    for (int i = 0; i < frames; ++i)
        bus.line.push(bus.in[static_cast<size_t>(i)]);
    for (int i = 0; i < frames; ++i)
        bus.throwLine.push(bus.throwIn[static_cast<size_t>(i)]);
    // Stereo beds: the side signals of this bus's stereo tracks go down a line of their own, and the two passes
    // read it where they read the mono sum, ear by ear, so each pass is the stereo sound itself, moved: its left
    // is the mono sum plus the side signal, its right the mono sum minus it. Heard by Width x (1 - Doppler), in Bed
    // mode only (wide while the passes are gentle, folding into the moving point as Doppler rises), and less the
    // further a pass is to one side: the amount is the most that leaves left and right no more apart than they are
    // in the file, so a pass panned hard over is a point and nothing is ever wider than its source. Nothing here
    // runs unless the line holds sound.
    bool sideHeard = false;
    const float sideAmount0 = bus.sideAmount;
    float sideAmount1 = 0.0f;
    if (busIndex == c_sideBus)
    {
        bool any = false;
        for (int i = 0; i < frames; ++i)
        {
            const float x = bus.sideIn[static_cast<size_t>(i)];
            bus.sideLine.push(x);
            any = any || x != 0.0f;
        }
        bus.sideHold = any ? bus.sideLine.maxDelay() : std::max(0, bus.sideHold - frames);
        if (bus.sideHold > 0)
        {
            sideAmount1 = p.width * (1.0f - p.doppler);
            sideHeard = (sideAmount0 > 0.0f || sideAmount1 > 0.0f) && (shape0 < 1.0f || shape1 < 1.0f);
        }
        bus.sideAmount = sideAmount1;
        if (sideHeard)
        {
            std::fill(bus.sideL.begin(), bus.sideL.begin() + frames, 0.0f);
            std::fill(bus.sideR.begin(), bus.sideR.begin() + frames, 0.0f);
        }
    }
    const float sideAmountStep = (sideAmount1 - sideAmount0) / static_cast<float>(frames);
    if (busIndex == 0)
    {
        m_throwDoppler = p.doppler;
        m_throwWidthLaw = widthLaw;
    }

    std::fill(bus.l.begin(), bus.l.begin() + frames, 0.0f);
    std::fill(bus.r.begin(), bus.r.begin() + frames, 0.0f);

    // Linked: line up with bus A's passes as they were at the start of this chunk (A has already rendered it).
    if (leadStart != nullptr)
        for (size_t k = 0; k < bus.heads.size(); ++k)
            followPass(bus.heads[k], (*leadStart)[k], p.bend, false);

    // Doppler: two passes, always exactly half a pass apart (the second head has no clock of its own: it is placed
    // from the first head's). Bed: their sine windows overlap, and their power adds up to a steady level whatever
    // lengths Chaos draws. Fly-By: each is heard on its own, a long approach and a shorter tail (see passWindow).
    //
    // Faders Together puts the passes on the clock (design 3.1): a pass lasts two swells, one in and one out, and
    // is closest on a peak of the clock; the two passes are a swell apart, so there is one at its closest on every
    // peak. A pass on the clock takes its time from the clock (Head::clockPeak), so nothing drifts, and a Rate
    // that moves mid-pass moves the pass with it. Passes join the clock, and leave it again in the other modes,
    // where the first one runs out: the second is at its closest right then, in either world, so nothing steps.
    // The pass that joins flies in for as long as it is to the next peak but one half swell (0.5 to 1.5 swells).
    // A linked bus B does what bus A's passes do, on the clock or off it.
    const auto& kernel = sj::SincKernel::get();
    const double leadTime = bus.heads[0].time;                              // the first head before this chunk,
    const double leadLength = static_cast<double>(bus.heads[0].length);     // for placing the second
    const bool together = m_params.rideMode == RideTogether;
    const float swellSeconds = m_cycleSeconds;
    auto onClock = [swellSeconds](Head& h, double x)
    {
        h.pace = h.clockSeconds / swellSeconds;
        h.time = static_cast<double>(h.lenIn) + (x - h.clockPeak) * static_cast<double>(h.clockSeconds);
    };
    for (size_t k = 0; k < bus.heads.size(); ++k)
    {
        auto& h = bus.heads[k];
        if (room)
            roomAir(h, p.doppler, orbit1, fs, sideHeard, shape1);
        else
            passAir(h, p.doppler, widthLaw, fs, sideHeard);

        double t0 = leadTime, len0 = leadLength; // the first head's clock again, sample by sample (second head only)
        for (int i = 0; i < frames; ++i)
        {
            const double clockNow = m_cycleX0 + static_cast<double>(i) * m_cycleDX;
            bool continueOnClock = false; // this sample's time has been taken from the clock
            if (h.clockSeconds > 0.0f)
            {
                const double end = h.clockPeak + static_cast<double>(h.lenOut) / static_cast<double>(h.clockSeconds);
                if (k != 0 && bus.heads[0].clockSeconds <= 0.0f && clockNow >= h.clockPeak)
                {
                    // The first head has left the clock (it has been through this chunk already): this one, at
                    // its closest right now, goes back to being placed from the first head's time.
                    h.clockSeconds = 0.0f;
                    h.pace = 1.0f;
                    t0 = bus.heads[0].time - static_cast<double>(frames - i) * dt;
                    len0 = static_cast<double>(bus.heads[0].length);
                    h.lenOut = static_cast<float>(0.5 * len0);
                    h.length = h.lenIn + h.lenOut;
                }
                else if (clockNow >= end)
                {
                    const double over = (clockNow - end) * static_cast<double>(h.clockSeconds);
                    if (lead != nullptr)
                        followPass(h, (*lead)[k], p.bend, true);
                    else if (together || k != 0)
                    {
                        startPass(h, p, swellSeconds);
                        h.clockPeak = std::round(end) + 1.0;
                        h.clockSeconds = swellSeconds;
                    }
                    else
                    {
                        startPass(h, p); // out of Faders Together: a pass of its own length again
                        h.clockSeconds = 0.0f;
                    }
                    if (h.clockSeconds <= 0.0f)
                    {
                        h.pace = 1.0f;
                        h.time = over;
                    }
                }
                if (h.clockSeconds > 0.0f)
                {
                    onClock(h, clockNow);
                    continueOnClock = true;
                }
            }
            if (continueOnClock)
            {
                // (the clock has placed it)
            }
            else if (k == 0)
            {
                if (h.time >= static_cast<double>(h.length))
                {
                    // The next pass starts where this one ran out (the fraction of a sample over is kept, so the
                    // second head, at its closest point right now, carries on without a step).
                    const double over = h.time - static_cast<double>(h.length);
                    if (lead != nullptr)
                        followPass(h, (*lead)[k], p.bend, true); // the pass bus A started at this same moment
                    else if (together)
                    {
                        // It joins the clock: closest on the first peak that is half a swell or more away.
                        const double from = clockNow - over / static_cast<double>(swellSeconds);
                        const double peak = std::ceil(from + 0.5);
                        startPass(h, p, static_cast<float>((peak - from) * static_cast<double>(swellSeconds)));
                        h.lenOut = swellSeconds;
                        h.length = h.lenIn + h.lenOut;
                        h.clockPeak = peak;
                        h.clockSeconds = swellSeconds;
                    }
                    else
                        startPass(h, p);
                    h.time = over;
                    if (h.clockSeconds > 0.0f)
                        onClock(h, clockNow);
                }
            }
            else
            {
                if (t0 >= len0)
                {
                    // The first head started its next pass: this head is at its closest point, and now knows how
                    // long its way out is (half of that pass).
                    t0 -= len0;
                    len0 = static_cast<double>(bus.heads[0].length);
                    h.lenOut = static_cast<float>(0.5 * len0);
                    h.length = h.lenIn + h.lenOut;
                    if (const auto& first = bus.heads[0]; first.clockSeconds > 0.0f)
                    {
                        // ... and the first head joined the clock with it: this one goes on the clock too, on
                        // its way out for as long as the first is on its way in.
                        h.clockSeconds = first.clockSeconds;
                        h.clockPeak = first.clockPeak - static_cast<double>(first.lenIn) / static_cast<double>(first.clockSeconds);
                        h.lenOut = first.lenIn;
                        h.length = h.lenIn + h.lenOut;
                    }
                }
                const double half = 0.5 * len0;
                if (h.clockSeconds > 0.0f)
                    onClock(h, clockNow);
                else if (t0 >= half)
                {
                    if (h.time >= static_cast<double>(h.lenIn)) // was on its way out: the next pass starts here
                    {
                        if (lead != nullptr)
                            followPass(h, (*lead)[k], p.bend, true);
                        else
                            startPass(h, p, static_cast<float>(half));
                    }
                    h.time = t0 - half;
                }
                else
                    h.time = static_cast<double>(h.lenIn) + t0;
                t0 += dt;
            }

            const PassPoint pt = room ? roomPoint(h, h.progress(), widthLaw, orbit0 + orbitStep * static_cast<float>(i + 1))
                                      : passPoint(h, h.progress(), widthLaw);
            const double back = static_cast<double>(frames - 1 - i);
            // The swell: the window, and how near the pass is.
            const float shapeNow = shape0 + shapeStep * static_cast<float>(i + 1);
            const float window = passWindow(pt.u, shapeNow);
            const float nearness = pt.nearness;
            // Fly-By's passes don't overlap, so the bus is quieter between them: a little make-up keeps it as loud
            // as Bed on the same material.
            const float gain = window * (1.0f + (c_flyMakeup - 1.0f) * shapeNow) * (1.0f + 0.8f * p.doppler * (pt.loud - 0.35f));
            if (i == frames - 1)
            {
                const float near = window * (0.1f + 0.9f * std::sqrt(std::min(nearness, 1.0f)));
                proximity = std::max(proximity, near);
                passNear[k] = near / nearAtClosest(h, orbit1);
            }
            if (room)
            {
                // One read, into this pass's own lane; a stereo bed's sides into theirs (they stay a left / right
                // pair wherever the pass is: see mixRoom).
                const double at = pt.base * m_sampleRate + back;
                auto& lane = k == 0 ? bus.l : bus.r;
                lane[static_cast<size_t>(i)] += h.airL.lowpass(bus.line.readSinc(at, pt.stretch, kernel)) * gain;
                if (sideHeard)
                {
                    const float amount = (sideAmount0 + sideAmountStep * static_cast<float>(i + 1)) * (1.0f - shapeNow);
                    bus.sideL[static_cast<size_t>(i)] += h.airSL.lowpass(bus.sideLine.readSinc(at, pt.stretch, kernel)) * gain * amount;
                }
            }
            else
                hearPass(h, bus.line, pt, back, gain, bus.l[static_cast<size_t>(i)], bus.r[static_cast<size_t>(i)]);
            if (sideHeard && ! room)
            {
                const float amount = (sideAmount0 + sideAmountStep * static_cast<float>(i + 1)) * (1.0f - shapeNow)
                                     * std::sqrt(std::max(0.0f, 2.0f * pt.panL * pt.panR)); // 1 in the middle, 0 hard over
                const float sl = bus.sideLine.readSinc((pt.base + pt.ear) * m_sampleRate + back, pt.stretch, kernel);
                const float sr = bus.sideLine.readSinc((pt.base - pt.ear) * m_sampleRate + back, pt.stretch, kernel);
                bus.sideL[static_cast<size_t>(i)] += h.airSL.lowpass(sl) * gain * amount;
                bus.sideR[static_cast<size_t>(i)] -= h.airSR.lowpass(sr) * gain * amount;
            }

            if (k == 0)
                h.time += dt;
        }
        // Where the head is after this chunk (the second head's time was last set for the chunk's final sample).
        if (k != 0)
            h.time += dt;
        if (h.clockSeconds > 0.0f)
            onClock(h, m_cycleX0 + static_cast<double>(frames) * m_cycleDX);
        h.age = h.progress();
        h.view = passWindow(h.age, shape1);
        h.x = h.dir * std::tanh(5.0f * (h.age - 0.5f));
        if (room)
        {
            const Place place = placeOf(h, pathProgress(h.age, shape1), widthLaw, bus.path);
            bus.at[k] = place.position;
            bus.atDistance[k] = place.distance;
            roomGains(place.position, place.distance, bus.gainTo[k]);
        }
    }

    // Throws: one more pass each, the same on every bus (bus A's Doppler and Width, this bus's Bend, set when it
    // was thrown), always shaped like a Fly-By, reading the tracks at their fader levels from the throw line.
    for (size_t v = 0; v < m_throws.size(); ++v)
    {
        if (! m_throws[v].active)
            continue;
        auto& h = bus.throwHeads[v];
        const float doppler = m_throwDoppler, law = m_throwWidthLaw, level = m_throws[v].level;
        if (room)
            roomAir(h, doppler, m_throwOrbit1, fs, false, 1.0f);
        else
            passAir(h, doppler, law, fs, false);
        const float throwOrbitStep = (m_throwOrbit1 - m_throwOrbit0) / static_cast<float>(frames);
        for (int i = 0; i < frames; ++i)
        {
            const float u = h.progress();
            const float window = passWindow(u, 1.0f);
            if (window > 0.0f)
            {
                const PassPoint pt = room ? roomPoint(h, u, law, m_throwOrbit0 + throwOrbitStep * static_cast<float>(i + 1))
                                          : passPoint(h, u, law);
                const float nearness = pt.nearness;
                const float gain = window * c_flyMakeup * (1.0f + 0.8f * doppler * (pt.loud - 0.35f)) * level;
                if (i == frames - 1)
                {
                    const float near = window * (0.1f + 0.9f * std::sqrt(std::min(nearness, 1.0f)));
                    proximity = std::max(proximity, near);
                    throwNear = std::max(throwNear, near / nearAtClosest(h, m_throwOrbit1));
                }
                if (room) // every throw into the throws' lane (a throw flies bus A's Flight Path on every bus)
                    bus.t[static_cast<size_t>(i)] += h.airL.lowpass(bus.throwLine.readSinc(pt.base * m_sampleRate + static_cast<double>(frames - 1 - i), pt.stretch, kernel)) * gain;
                else
                    hearPass(h, bus.throwLine, pt, static_cast<double>(frames - 1 - i), gain, bus.l[static_cast<size_t>(i)], bus.r[static_cast<size_t>(i)]);
            }
            h.time += dt;
        }
        h.age = h.progress();
        h.view = passWindow(h.age, 1.0f) * level;
        h.x = h.dir * std::tanh(5.0f * (h.age - 0.5f));
    }
    if (room && ! throwGains(bus.throwHeads, m_throwWidthLaw, bus.gainTo[2], &bus.at[2], &bus.atDistance[2]))
        bus.gainTo[2] = bus.gain[2];

    // Stereo beds: what goes into the colour chain, to match the side signal to what comes out of it.
    float sidePreSq = 0.0f;
    if (sideHeard)
        for (int i = 0; i < frames; ++i)
            sidePreSq += bus.l[static_cast<size_t>(i)] * bus.l[static_cast<size_t>(i)] + bus.r[static_cast<size_t>(i)] * bus.r[static_cast<size_t>(i)];

    // Level tracking for the low-end lift and the views.
    float chunkPeak = 0.0f;
    if (room) // the lanes as one stream: each is as loud as a centred pass is in left and in right
        for (int i = 0; i < frames; ++i)
            chunkPeak = std::max(chunkPeak, bus.follower.process(bus.l[static_cast<size_t>(i)] + bus.r[static_cast<size_t>(i)] + (third ? bus.t[static_cast<size_t>(i)] : 0.0f)));
    else
        for (int i = 0; i < frames; ++i)
            chunkPeak = std::max(chunkPeak, bus.follower.process(0.5f * (bus.l[static_cast<size_t>(i)] + bus.r[static_cast<size_t>(i)])));
    bus.env = chunkPeak;
    // The recent peak falls back by 12 dB per pass length (it follows Rate), so one big swell doesn't hold the Filter,
    // Swirl and Low End back for several passes after it.
    bus.peakRef = std::max(bus.env, bus.peakRef * std::exp(-static_cast<float>(frames) / (c_peakPasses * nominalPassSeconds() * fs)));
    // What Low End, Swirl and Filter follow: the level against the recent peak and, the stiller the faders are, how
    // near the nearer pass is. With the ride at its default depth or deeper it is all but the level alone (the ride
    // makes the swells); with Auto Ride off or Swell held the level hardly moves, and the passes move the colours.
    bus.prox += (proximity - bus.prox) * glide(frames, c_proxSeconds, fs);
    const float levelNow = sj::clamp(bus.env / std::max(bus.peakRef, 1.0e-6f), 0.0f, 1.0f);
    const float nearNow = 1.0f - m_proxBlend * (1.0f - bus.prox);
    const float peakness = levelNow * nearNow;

    // Low end: sub octave, overtones, and a lift at the loudest point (a multiband-dynamics style lift).
    if (p.low > 1.0e-4f && room)
    {
        // A room: the low band of the lanes together makes the octave and the overtones, as left and right do in
        // stereo, and each lane takes its share of them by its power (so they are where the sound is: under a
        // lone throw and not under two silent passes).
        const float shelf = sj::dbToGain(p.low * (2.0f + 9.0f * levelNow * levelNow * levelNow * nearNow)) - 1.0f;
        float power[3] = { 1.0e-12f, 1.0e-12f, 0.0f };
        for (int i = 0; i < frames; ++i)
        {
            power[0] += bus.l[static_cast<size_t>(i)] * bus.l[static_cast<size_t>(i)];
            power[1] += bus.r[static_cast<size_t>(i)] * bus.r[static_cast<size_t>(i)];
            if (third)
                power[2] += bus.t[static_cast<size_t>(i)] * bus.t[static_cast<size_t>(i)];
        }
        const float total = power[0] + power[1] + power[2];
        const auto share0 = bus.lowShare;
        const float c = glide(frames, c_shareSeconds, fs);
        for (size_t k = 0; k < 3; ++k)
            bus.lowShare[k] += (power[k] / total - bus.lowShare[k]) * c;
        const float step = 1.0f / static_cast<float>(frames);
        for (int i = 0; i < frames; ++i)
        {
            const float ramp = static_cast<float>(i + 1) * step;
            auto& l = bus.l[static_cast<size_t>(i)];
            auto& r = bus.r[static_cast<size_t>(i)];
            const float low = bus.lowB.lowpass(bus.lowA.lowpass(l + r + (third ? bus.t[static_cast<size_t>(i)] : 0.0f)));
            const float add = p.low * bus.lowAdd.process(low);
            l += add * (share0[0] + (bus.lowShare[0] - share0[0]) * ramp) + shelf * bus.shelfL.lowpass(l);
            r += add * (share0[1] + (bus.lowShare[1] - share0[1]) * ramp) + shelf * bus.shelfR.lowpass(r);
            if (third)
            {
                auto& t = bus.t[static_cast<size_t>(i)];
                t += add * (share0[2] + (bus.lowShare[2] - share0[2]) * ramp) + shelf * bus.shelfT.lowpass(t);
            }
        }
    }
    else if (p.low > 1.0e-4f)
    {
        // The lift is steep in the level (it belongs to the very loudest moments) and even in how near the pass is.
        const float shelf = sj::dbToGain(p.low * (2.0f + 9.0f * levelNow * levelNow * levelNow * nearNow)) - 1.0f;
        for (int i = 0; i < frames; ++i)
        {
            auto& l = bus.l[static_cast<size_t>(i)];
            auto& r = bus.r[static_cast<size_t>(i)];
            const float low = bus.lowB.lowpass(bus.lowA.lowpass(0.5f * (l + r)));
            // A note an octave under the low band (40 Hz .. about 120 Hz) and its overtones: see LowAdd.
            const float add = p.low * bus.lowAdd.process(low);
            l += add + shelf * bus.shelfL.lowpass(l);
            r += add + shelf * bus.shelfR.lowpass(r);
        }
    }

    // What is behind Swirl (design 3.3 to 3.5): its Type. A change of Type fades the bus out over 30 ms where it
    // leaves Swirl (the old type's tails with it), swaps with the bus silent (a Tape Flanger's 8 ms come or go
    // unheard), and fades it back in over 30 ms where it enters Swirl: the other type starts from nothing, so what
    // its delays give back later has been through the fade too, and nothing starts with a step. The Filter and
    // Drive after it are given a sound that goes away and comes back smoothly, so they have nothing to ring with.
    const auto& detail = m_params.swirlDetail[busIndex];
    const float typeFade0 = bus.typeFade;
    bool typeFadeAtInput = bus.typeFadeIn;
    {
        const int wanted = std::clamp(detail.type, 0, static_cast<int>(c_swirlTypes) - 1);
        const float fadeStep = static_cast<float>(frames) / (c_typeFadeSeconds * fs);
        if (! bus.typeFadeIn && wanted != bus.swirlType && bus.typeFade <= 0.0f)
        {
            bus.swirlType = wanted;
            for (auto* stages : { &bus.phL, &bus.phR, &bus.phT })
                for (auto& a : *stages)
                    a = {};
            bus.phFbL = bus.phFbR = bus.phFbT = bus.flFbL = bus.flFbR = bus.flFbT = 0.0f;
            bus.flL.reset();
            bus.flR.reset();
            if (! bus.flT.buffer.empty())
                bus.flT.reset();
            bus.swirlOn = false;
            bus.swirlPre = bus.swirlPost = 0.0f;
            bus.swirlGain = 1.0f;
            bus.typeFadeIn = typeFadeAtInput = true;
        }
        if (bus.typeFadeIn)
        {
            // (On its way back in: it gets there before another change of Type starts.)
            bus.typeFade = std::min(1.0f, bus.typeFade + fadeStep);
            bus.typeFadeIn = bus.typeFade < 1.0f;
        }
        else if (wanted != bus.swirlType)
            bus.typeFade = std::max(0.0f, bus.typeFade - fadeStep);
        else if (bus.typeFade < 1.0f)
            bus.typeFade = std::min(1.0f, bus.typeFade + fadeStep); // (changed back before it was out)
    }
    const float typeFade1 = bus.typeFade;
    auto typeFadeGain = [typeFade0, typeFade1, frames](int i)
    {
        const float x = typeFade0 + (typeFade1 - typeFade0) * static_cast<float>(i + 1) / static_cast<float>(frames);
        return x * x * (3.0f - 2.0f * x); // eased at both ends
    };
    // (Before Swirl on the way in, after it on the way out; a stereo bed's sides with it, and what they are
    // matched to: the bus as it goes into the colours.)
    auto applyTypeFade = [&]
    {
        float gainSq = 0.0f;
        for (int i = 0; i < frames; ++i)
        {
            const auto ii = static_cast<size_t>(i);
            const float g = typeFadeGain(i);
            gainSq += g * g;
            bus.l[ii] *= g;
            bus.r[ii] *= g;
            if (third)
                bus.t[ii] *= g;
            if (sideHeard)
            {
                bus.sideL[ii] *= g;
                bus.sideR[ii] *= g;
            }
        }
        sidePreSq *= gainSq / static_cast<float>(frames);
    };
    if (typeFadeAtInput)
        applyTypeFade();
    const bool classic = bus.swirlType != SwirlTape && bus.swirlType != SwirlDoubler;
    if (classic)
        bus.swirlFx.reset(); // (nothing to do unless it has just been in use)
    // Level match over ~300 ms, within +/-6 dB: Swirl changes the colour, not the loudness.
    auto matchLevel = [&](float preSq, float postSq)
    {
        const float c = 1.0f - std::exp(-static_cast<float>(frames) / (0.3f * fs));
        bus.swirlPre += (preSq / static_cast<float>(frames) - bus.swirlPre) * c;
        bus.swirlPost += (postSq / static_cast<float>(frames) - bus.swirlPost) * c;
        const float want = bus.swirlPost > 1.0e-10f ? sj::clamp(std::sqrt(bus.swirlPre / bus.swirlPost), 0.5f, 2.0f) : 1.0f;
        const float g0 = bus.swirlGain;
        bus.swirlGain += (want - bus.swirlGain) * glide(frames, c_matchSeconds, fs);
        for (int i = 0; i < frames; ++i)
        {
            const float g = g0 + (bus.swirlGain - g0) * static_cast<float>(i + 1) / static_cast<float>(frames);
            bus.l[static_cast<size_t>(i)] *= g;
            bus.r[static_cast<size_t>(i)] *= g;
            if (third)
                bus.t[static_cast<size_t>(i)] *= g;
        }
    };
    auto lanesPower = [&]
    {
        float sq = 0.0f;
        for (int i = 0; i < frames; ++i)
            sq += bus.l[static_cast<size_t>(i)] * bus.l[static_cast<size_t>(i)] + bus.r[static_cast<size_t>(i)] * bus.r[static_cast<size_t>(i)];
        if (third)
            for (int i = 0; i < frames; ++i)
                sq += bus.t[static_cast<size_t>(i)] * bus.t[static_cast<size_t>(i)];
        return sq;
    };

    // Swirl: phaser, then a flanger whose comb sweeps as the passes come and go (Type: both, or either alone).
    if (classic && p.swirl > 1.0e-4f)
    {
        // The knob runs a little faster at the bottom (the first phaser notches arrive sooner), and the result is
        // level-matched so Swirl changes the colour, not the loudness.
        const float swirl = std::pow(p.swirl, 0.7f);
        // Behind the wheel: Feedback (either sign), Speed, Depth (the span of its own sweep) and Follow (how far
        // the sweep goes by the whoosh; at 0 it is a free sweep). Every expression below is the one Swirl had
        // before these were there, at their defaults (0.55, 1, 1, 1).
        const float follow = sj::clamp(detail.follow, 0.0f, 1.0f);
        const float span = sj::clamp(detail.depth, 0.0f, 1.0f);
        const float speed = sj::clamp(detail.speed, 0.1f, 8.0f);
        float phMix = sj::clamp(swirl * 2.0f, 0.0f, 1.0f) * 0.5f;
        float flMix = sj::clamp((swirl - 0.25f) / 0.75f, 0.0f, 1.0f) * 0.5f;
        if (bus.swirlType == SwirlPhaser)
            flMix = 0.0f;
        else if (bus.swirlType == SwirlFlanger)
        {
            flMix = phMix; // the flanger alone comes in from the bottom of the knob
            phMix = 0.0f;
        }
        float preSq = 0.0f, postSq = 0.0f;
        for (int i = 0; i < frames; ++i)
            preSq += bus.l[static_cast<size_t>(i)] * bus.l[static_cast<size_t>(i)] + bus.r[static_cast<size_t>(i)] * bus.r[static_cast<size_t>(i)];
        if (third)
            for (int i = 0; i < frames; ++i)
                preSq += bus.t[static_cast<size_t>(i)] * bus.t[static_cast<size_t>(i)];
        // The level against the recent peak moves in steps (once per chunk); Swirl follows it over ~15 ms, and every
        // setting it leads to (the phaser's all-pass tuning, the flanger's delay, the mixes) is ramped sample by
        // sample from where the last chunk ended, so nothing steps at a chunk edge.
        if (! bus.swirlOn)
            bus.swPeak = peakness;
        bus.swPeak += (peakness - bus.swPeak) * glide(frames, 0.015f, fs);
        const float level = bus.swPeak;
        const float rateLevel = follow * level + (1.0f - follow) * 0.5f;
        bus.lfo += speed * (0.12f + 1.2f * rateLevel * swirl) * static_cast<float>(frames) / fs;
        if (bus.lfo >= 1.0f) bus.lfo -= std::floor(bus.lfo);
        const float lfo = span * std::sin(sj::c_twoPi * bus.lfo);
        const float lfoR = span * std::cos(sj::c_twoPi * bus.lfo);
        // (Phaser + Flanger tunes its all-passes from the top of the band down, which puts the phaser's notches in
        // the top octave, under the flanger's comb. The phaser alone is tuned from the bottom up: its three notches
        // sit near 0.27, 1 and 3.7 times the frequency below, through the middle of the sound.)
        const float phaseSide = bus.swirlType == SwirlPhaser ? -1.0f : 1.0f;
        auto coefFor = [fs, phaseSide](float fc)
        {
            const float t = std::tan(sj::c_pi * sj::clamp(fc, 20.0f, fs * 0.45f) / fs);
            return phaseSide * (t - 1.0f) / (t + 1.0f);
        };
        const float followLevel = follow * level;
        const float aL1 = coefFor(260.0f * std::exp2(3.6f * (0.5f + 0.5f * lfo) + 1.2f * followLevel));
        const float aR1 = coefFor(260.0f * std::exp2(3.6f * (0.5f + 0.5f * lfoR) + 1.2f * followLevel));
        const float fb1 = sj::clamp(detail.feedback, -1.0f, 1.0f) * swirl;
        // The flanger's delay: by the whoosh (long when it is far), or half way and swept by its own sweep alone.
        // (The flanger alone goes by how near the pass is, the same on its way in as on its way out, so its comb
        // sweeps up as the pass comes and back down as it leaves. The level against its recent peak, which Phaser +
        // Flanger goes by, rises fast and falls slowly: that comb is up before the pass arrives.)
        const float nearestPass = sj::clamp(std::max({ passNear[0], passNear[1], throwNear }), 0.0f, 1.0f);
        if (! bus.swirlOn)
            bus.swNear = nearestPass;
        bus.swNear += (nearestPass - bus.swNear) * glide(frames, 0.015f, fs);
        const float sweepBy = bus.swirlType == SwirlFlanger ? bus.swNear : level;
        const float far = follow * ((1.0f - sweepBy) * (1.0f - sweepBy));
        const float free = 6.5f * (1.0f - follow);
        const float msL1 = 0.3f + 6.5f * far * (0.85f + 0.15f * lfo) + free * (0.5f + 0.5f * lfo);
        const float msR1 = 0.3f + 6.5f * far * (0.85f - 0.15f * lfo) + free * (0.5f - 0.5f * lfo);
        // The third lane's sweep is half a turn from the first's (the second's is a quarter).
        const float aT1 = third ? coefFor(260.0f * std::exp2(3.6f * (0.5f - 0.5f * lfo) + 1.2f * followLevel)) : 0.0f;
        const float msT1 = 0.3f + 6.5f * far * (0.85f + 0.15f * lfoR) + free * (0.5f + 0.5f * lfoR);
        if (! bus.swirlOn || thirdNew)
        {
            bus.swAT = aT1;
            bus.swMsT = msT1;
        }
        if (! bus.swirlOn)
        {
            // Swirl was off: start from here (the mixes still fade in from nothing).
            bus.swirlOn = true;
            bus.swAL = aL1; bus.swAR = aR1;
            bus.swMsL = msL1; bus.swMsR = msR1;
            bus.swFb = fb1;
            bus.swPhMix = bus.swFlMix = 0.0f;
        }
        const float aT0 = bus.swAT, msT0 = bus.swMsT;
        const float aL0 = bus.swAL, aR0 = bus.swAR, msL0 = bus.swMsL, msR0 = bus.swMsR;
        const float fb0 = bus.swFb, phMix0 = bus.swPhMix, flMix0 = bus.swFlMix;
        const bool phasing = phMix > 0.0f || phMix0 > 0.0f;
        const bool flanging = flMix > 1.0e-4f || flMix0 > 1.0e-4f;
        const float msToSamples = 0.001f * fs;
        const float step = 1.0f / static_cast<float>(frames);
        for (int i = 0; i < frames; ++i)
        {
            const float t = static_cast<float>(i + 1) * step;
            const float aL = aL0 + (aL1 - aL0) * t, aR = aR0 + (aR1 - aR0) * t;
            // (Each loop stays under 1 whatever Feedback is: the flanger's is 0.6 / 0.55 of the phaser's.)
            const float fbNow = fb0 + (fb1 - fb0) * t;
            const float fb = sj::clamp(fbNow, -c_swirlFeedbackMax, c_swirlFeedbackMax);
            const float flFb = sj::clamp((0.6f / 0.55f) * fbNow, -c_swirlFeedbackMax, c_swirlFeedbackMax);
            const float phNow = phMix0 + (phMix - phMix0) * t;
            auto& l = bus.l[static_cast<size_t>(i)];
            auto& r = bus.r[static_cast<size_t>(i)];
            float ol = l, orr = r;
            if (phasing)
            {
                float yl = l + fb * bus.phFbL, yr = r + fb * bus.phFbR;
                for (size_t k = 0; k < bus.phL.size(); ++k)
                {
                    yl = bus.phL[k].process(yl, aL);
                    yr = bus.phR[k].process(yr, aR);
                }
                bus.phFbL = sj::fastTanh(yl);
                bus.phFbR = sj::fastTanh(yr);
                ol = l + phNow * (yl - l);
                orr = r + phNow * (yr - r);
            }
            if (flanging)
            {
                const float flNow = flMix0 + (flMix - flMix0) * t;
                bus.flL.push(ol + flFb * bus.flFbL);
                bus.flR.push(orr + flFb * bus.flFbR);
                bus.flFbL = bus.flL.read(static_cast<double>((msL0 + (msL1 - msL0) * t) * msToSamples));
                bus.flFbR = bus.flR.read(static_cast<double>((msR0 + (msR1 - msR0) * t) * msToSamples));
                ol += flNow * (bus.flFbL - ol);
                orr += flNow * (bus.flFbR - orr);
            }
            l = ol;
            r = orr;
            postSq += l * l + r * r;
            if (third)
            {
                auto& x = bus.t[static_cast<size_t>(i)];
                float o = x;
                if (phasing)
                {
                    const float aT = aT0 + (aT1 - aT0) * t;
                    float y = x + fb * bus.phFbT;
                    for (auto& stage : bus.phT)
                        y = stage.process(y, aT);
                    bus.phFbT = sj::fastTanh(y);
                    o = x + phNow * (y - x);
                }
                if (flanging)
                {
                    const float flNow = flMix0 + (flMix - flMix0) * t;
                    bus.flT.push(o + flFb * bus.flFbT);
                    bus.flFbT = bus.flT.read(static_cast<double>((msT0 + (msT1 - msT0) * t) * msToSamples));
                    o += flNow * (bus.flFbT - o);
                }
                x = o;
                postSq += x * x;
            }
        }
        bus.swAT = aT1;
        bus.swMsT = msT1;
        bus.swAL = aL1; bus.swAR = aR1;
        bus.swMsL = msL1; bus.swMsR = msR1;
        bus.swFb = fb1;
        bus.swPhMix = phMix;
        bus.swFlMix = flMix;
        matchLevel(preSq, postSq);
    }
    else if (! classic)
    {
        // Tape Flanger and Doubler (SwirlFx): on the bus's lanes, the throws' included. They run with the wheel at
        // 0 too: the Tape Flanger's clean path is 8 ms long whatever the wheel says (it comes and goes with the
        // Type, under the fade), and neither has anything to start up when the wheel is turned.
        SwirlFx::Lanes lanes;
        lanes.lane = { bus.l.data(), bus.r.data(), third ? bus.t.data() : nullptr };
        lanes.count = room ? 3 : 2;
        // With the Pass: a room's lanes are a pass each and the throws; left and right hear whichever is nearest.
        const float nearest = std::max({ passNear[0], passNear[1], throwNear });
        lanes.passNearTo = room ? std::array<float, 3> { passNear[0], passNear[1], throwNear } : std::array<float, 3> { nearest, nearest, nearest };
        lanes.passNearFrom = lanes.passNearTo;
        if (bus.swirlType == SwirlTape)
        {
            if (m_params.varispeedHome[busIndex])
                bus.swirlFx.recentre(); // the offset meter was double-clicked
            bus.swirlFx.processTape(detail.tape, p.swirl, lanes, frames);
            // A stereo bed's sides rejoin after the colours: they are as late as the passes they belong to. (Not
            // heard: silence goes in, so nothing old is left in there for when they are.)
            if (busIndex == c_sideBus)
            {
                if (! sideHeard)
                {
                    std::fill(bus.sideL.begin(), bus.sideL.begin() + frames, 0.0f);
                    std::fill(bus.sideR.begin(), bus.sideR.begin() + frames, 0.0f);
                }
                bus.swirlFx.delaySides(bus.sideL.data(), room ? nullptr : bus.sideR.data(), frames);
            }
            // (Its level is left as it is: the thinning out as the two copies meet is the effect.)
            bus.swirlPre = bus.swirlPost = 0.0f;
            bus.swirlGain = 1.0f;
        }
        else
        {
            const float preSq = lanesPower();
            bus.swirlFx.processDoubler(detail.doubler, p.swirl, lanes, frames);
            if (p.swirl > 1.0e-4f)
                matchLevel(preSq, lanesPower());
            else
            {
                bus.swirlPre = bus.swirlPost = 0.0f;
                bus.swirlGain = 1.0f;
            }
        }
        bus.swirlOn = false;
    }
    else
    {
        bus.swirlPre = bus.swirlPost = 0.0f;
        bus.swirlGain = 1.0f;
        bus.swirlOn = false;
    }
    if (! typeFadeAtInput && (typeFade0 < 1.0f || typeFade1 < 1.0f))
        applyTypeFade();
    // For the offset meter (any thread reads it).
    m_varispeedView[busIndex].store(bus.swirlType == SwirlTape ? bus.swirlFx.getOffsetMs() / SwirlFx::c_maxOffsetMs : 0.0f, std::memory_order_relaxed);

    // Envelope filter: opens as each pass swells and closes in its tail, by the bus's level against its own recent
    // peak (so it works the same on loud and quiet material). Higher settings blend more of it in, close it further
    // at the peak (from 21 kHz at 0% to about 280 Hz at 100%) and, over the top 40%, add a little resonance.
    // Behind the wheel (SweepFilter, design 3.6): its Type and slope, where it sits closed, Resonance, how far it
    // follows the whoosh, and a rhythm of its own. The filter glides from one chunk's place to the next. The
    // rhythm runs whether the filter is heard or not.
    const float f = p.filter;
    const float mix = f <= 0.8f ? (1.0f - std::pow(10.0f, -1.875f * f)) / (1.0f - std::pow(10.0f, -1.5f)) : 1.0f;
    {
        const auto& filter = m_params.filterDetail[busIndex];
        const float rhythm = bus.sweep.advance(filter, frames);
        const float q = SweepFilter::q(filter, f);
        bus.sweep.setTarget(filter, SweepFilter::cutoffHz(filter, f, peakness, rhythm, fs), q);
        if (mix > 1.0e-4f)
        {
            const float trim = SweepFilter::trim(q);
            std::array<float, c_chunk> wet;
            auto lane = [&](int which, std::array<float, c_chunk>& x)
            {
                bus.sweep.process(which, x.data(), wet.data(), frames);
                for (int i = 0; i < frames; ++i)
                    x[static_cast<size_t>(i)] += mix * (sj::fastTanh(wet[static_cast<size_t>(i)] * trim) - x[static_cast<size_t>(i)]);
            };
            lane(0, bus.l);
            lane(1, bus.r);
            if (third)
                lane(2, bus.t);
        }
        bus.sweep.endChunk();
    }

    // Filtered by A: a low-pass that closes (18 kHz down to about 280 Hz) as bus A swells. It stays in the path
    // while its knob is up, wide open when bus A is silent, and hears the bus while the knob is down, so it never
    // comes in with a state that does not belong to the sound (bus A arriving after a silence used to click).
    if (! isKeyBus)
    {
        const bool heard = p.key > 1.0e-4f;
        const float cutoff = 18000.0f * std::exp2(-6.0f * p.key * key);
        bus.keyL.set(cutoff, 0.7071f, fs);
        bus.keyR.set(cutoff, 0.7071f, fs);
        for (int i = 0; i < frames; ++i)
        {
            auto& l = bus.l[static_cast<size_t>(i)];
            auto& r = bus.r[static_cast<size_t>(i)];
            const float fl = bus.keyL.lowpass(l), fr = bus.keyR.lowpass(r);
            if (heard)
            {
                l = fl;
                r = fr;
            }
        }
        if (third)
        {
            bus.keyT.set(cutoff, 0.7071f, fs);
            for (int i = 0; i < frames; ++i)
            {
                const float ft = bus.keyT.lowpass(bus.t[static_cast<size_t>(i)]);
                if (heard)
                    bus.t[static_cast<size_t>(i)] = ft;
            }
        }
    }

    // Drive
    // Drive: the gain into the saturator rises evenly in dB (0 .. 26 dB over the knob). A slow, partial level match
    // keeps it from being mostly a jump in loudness, while the denser, flatter swell (its character) stays.
    if (p.drive > 1.0e-4f)
    {
        const float d = std::sqrt((std::pow(10.0f, 1.32f * std::pow(p.drive, 0.7f)) - 1.0f) / 20.0f);
        const float drive = 1.0f + 20.0f * d * d;
        const float comp = 1.0f / (1.0f + 0.35f * std::log2(drive));
        const float dmix = sj::clamp(d * 2.5f, 0.0f, 1.0f);
        bus.toneL.set(20000.0f - 9000.0f * d, fs);
        bus.toneR.set(20000.0f - 9000.0f * d, fs);
        float preSq = 0.0f, postSq = 0.0f;
        for (int i = 0; i < frames; ++i)
        {
            auto& l = bus.l[static_cast<size_t>(i)];
            auto& r = bus.r[static_cast<size_t>(i)];
            preSq += l * l + r * r;
            l = bus.toneL.lowpass(l + dmix * (sj::fastTanh(drive * l) * comp - l));
            r = bus.toneR.lowpass(r + dmix * (sj::fastTanh(drive * r) * comp - r));
            postSq += l * l + r * r;
        }
        if (third)
        {
            bus.toneT.set(20000.0f - 9000.0f * d, fs);
            for (int i = 0; i < frames; ++i)
            {
                auto& t = bus.t[static_cast<size_t>(i)];
                preSq += t * t;
                t = bus.toneT.lowpass(t + dmix * (sj::fastTanh(drive * t) * comp - t));
                postSq += t * t;
            }
        }
        const float c = 1.0f - std::exp(-static_cast<float>(frames) / fs); // ~1 s
        bus.drivePre += (preSq / static_cast<float>(frames) - bus.drivePre) * c;
        bus.drivePost += (postSq / static_cast<float>(frames) - bus.drivePost) * c;
        const float match = bus.drivePost > 1.0e-10f ? std::min(1.0f, std::pow(bus.drivePre / bus.drivePost, 0.4f)) : 1.0f;
        const float want = 1.0f + dmix * (match - 1.0f);
        const float g0 = bus.driveGain;
        bus.driveGain += (want - bus.driveGain) * glide(frames, c_matchSeconds, fs);
        for (int i = 0; i < frames; ++i)
        {
            const float g = g0 + (bus.driveGain - g0) * static_cast<float>(i + 1) / static_cast<float>(frames);
            bus.l[static_cast<size_t>(i)] *= g;
            bus.r[static_cast<size_t>(i)] *= g;
            if (third)
                bus.t[static_cast<size_t>(i)] *= g;
        }
    }
    else
    {
        bus.drivePre = bus.drivePost = 0.0f;
        bus.driveGain = 1.0f;
    }

    // Stereo beds: the side signal comes back in here, past the colours (they work on the mono sum's passes), as
    // loud against the bus as it went in.
    float bedMidSq = 0.0f, bedSideSq = 0.0f; // the mono sum's passes and the sides, as they leave here
    if (sideHeard)
    {
        float postSq = 0.0f;
        for (int i = 0; i < frames; ++i)
            postSq += bus.l[static_cast<size_t>(i)] * bus.l[static_cast<size_t>(i)] + bus.r[static_cast<size_t>(i)] * bus.r[static_cast<size_t>(i)];
        // For the bed's level (below), as the speakers will have them. A room: the two passes are lanes that play
        // the same sound, so where they share speakers they add as one (their gains' product over the speakers,
        // against the 2 each has alone). Mono: as the two channels fold to one.
        const bool monoOut = m_width == room::Width::Mono;
        if (room)
        {
            float shared = 0.0f, cross = 0.0f;
            for (size_t ch = 0; ch < bus.gainTo[0].size(); ++ch)
                shared += bus.gainTo[0][ch] * bus.gainTo[1][ch];
            for (int i = 0; i < frames; ++i)
                cross += bus.l[static_cast<size_t>(i)] * bus.r[static_cast<size_t>(i)];
            bedMidSq = std::max(0.0f, postSq + shared * cross);
        }
        else if (monoOut)
            for (int i = 0; i < frames; ++i)
                bedMidSq += (bus.l[static_cast<size_t>(i)] + bus.r[static_cast<size_t>(i)]) * (bus.l[static_cast<size_t>(i)] + bus.r[static_cast<size_t>(i)]);
        else
            bedMidSq = postSq;
        const float c = 1.0f - std::exp(-static_cast<float>(frames) / (0.3f * fs));
        bus.sidePre += (sidePreSq / static_cast<float>(frames) - bus.sidePre) * c;
        bus.sidePost += (postSq / static_cast<float>(frames) - bus.sidePost) * c;
        const float want = bus.sidePre > 1.0e-10f ? sj::clamp(std::sqrt(bus.sidePost / bus.sidePre), 0.25f, 2.0f) : 1.0f;
        const float g0 = bus.sideMatch;
        bus.sideMatch += (want - bus.sideMatch) * glide(frames, 0.05f, fs);
        for (int i = 0; i < frames; ++i)
        {
            const float g = g0 + (bus.sideMatch - g0) * static_cast<float>(i + 1) / static_cast<float>(frames);
            if (room) // the sides stay a lane of their own
            {
                bus.sideL[static_cast<size_t>(i)] *= g;
                bedSideSq += bus.sideL[static_cast<size_t>(i)] * bus.sideL[static_cast<size_t>(i)];
                continue;
            }
            const float sl = g * bus.sideL[static_cast<size_t>(i)], sr = g * bus.sideR[static_cast<size_t>(i)];
            bus.l[static_cast<size_t>(i)] += sl;
            bus.r[static_cast<size_t>(i)] += sr;
            // (Mono: the whole fold with the sides in; what they add to it is taken below.)
            bedSideSq += monoOut ? (bus.l[static_cast<size_t>(i)] + bus.r[static_cast<size_t>(i)]) * (bus.l[static_cast<size_t>(i)] + bus.r[static_cast<size_t>(i)])
                                 : sl * sl + sr * sr;
        }
        if (monoOut)
            bedSideSq = std::max(0.0f, bedSideSq - bedMidSq);
    }
    else
    {
        bus.sidePre = bus.sidePost = 0.0f;
        bus.sideMatch = 1.0f;
    }
    // A stereo bed is as loud wide as narrow: the bus is brought to the power its mono sum has alone, as far as
    // the sides add to it (their power against the mono sum's, slow means). So Width, Doppler and Pass Mode, which
    // take the sides out, change how wide a bed is and not how loud. (At Mono too: the sides reach each ear at
    // their own time, so they do not fold away to nothing.)
    if (sideHeard || bus.bedGain < 0.9999f)
    {
        const float c = glide(frames, c_bedLevelSeconds, fs);
        if (sideHeard)
        {
            bus.bedMid += (bedMidSq / static_cast<float>(frames) - bus.bedMid) * c;
            bus.bedSide += (bedSideSq / static_cast<float>(frames) - bus.bedSide) * c;
        }
        else
            bus.bedSide -= bus.bedSide * c; // the sides have gone: back to the mono sum as it is
        const float want = bus.bedMid + bus.bedSide < 1.0e-12f ? 1.0f : std::sqrt(bus.bedMid / (bus.bedMid + bus.bedSide));
        const float g0 = bus.bedGain;
        bus.bedGain += (want - bus.bedGain) * c;
        if (! sideHeard && bus.bedGain >= 0.9999f)
            bus.bedGain = 1.0f;
        const float step = (bus.bedGain - g0) / static_cast<float>(frames);
        for (int i = 0; i < frames; ++i)
        {
            const auto ii = static_cast<size_t>(i);
            const float g = g0 + step * static_cast<float>(i + 1);
            bus.l[ii] *= g;
            bus.r[ii] *= g;
            if (room && third)
                bus.t[ii] *= g;
            if (room && sideHeard)
                bus.sideL[ii] *= g;
        }
    }
    else
        bus.bedMid = bus.bedSide = 0.0f;

    // What the mix takes: left and right, or a room's lanes (all four once the throws or a bed's sides sound).
    bus.lanes = room && (third || sideHeard) ? 4 : 2;
    bus.sidesOn = room && sideHeard;
    if (bus.lanes == 4 && ! third)
        std::fill(bus.t.begin(), bus.t.begin() + frames, 0.0f);
    if (bus.lanes == 4 && ! sideHeard)
        std::fill(bus.sideL.begin(), bus.sideL.begin() + frames, 0.0f);
}

float RigEngine::duckReturnCoef() const
{
    // 0 = the original (the duck follows its key straight away); up to 1 s for the ducked bus to come back.
    const float r = sj::clamp(m_params.duckReturn, 0.0f, 1.0f);
    if (r <= 0.001f)
        return 1.0f;
    return 1.0f - std::exp(-1.0f / (0.001f * (20.0f + 980.0f * r) * m_fs));
}

void RigEngine::duckSample(BusState::Duck& d, float target, float returnCoef, float full, float* const* ch, int n)
{
    // Down at once, back up over the return time.
    if (returnCoef >= 1.0f || target >= d.amount)
        d.amount = target; // fast return: exactly the original
    else
        d.amount += (target - d.amount) * returnCoef;
    const float amt = sj::clamp(d.amount, 0.0f, 1.0f);
    // The band filters always run, so switching Duck mode mid-duck doesn't start from stale filter state.
    float band[4];
    for (int c = 0; c < n; ++c)
        band[c] = d.band[c].bandpass(*ch[c]);
    switch (m_params.duckMode)
    {
        case 1: // Darken: low-pass the ducked bus (an envelope filter keyed by the other bus), a little quieter
        {
            const float fc = 18000.0f * std::exp2(-5.7f * sj::clamp(amt / full, 0.0f, 1.0f)); // 18 kHz .. ~350 Hz
            const float a = 1.0f - std::exp(-sj::c_twoPi * std::min(fc, 0.45f * m_fs) / m_fs);
            const float wet = sj::clamp(amt * 4.0f, 0.0f, 1.0f), gain = 1.0f - 0.25f * amt;
            for (int c = 0; c < n; ++c)
            {
                d.lp[c] += a * (*ch[c] - d.lp[c]);
                *ch[c] = (*ch[c] + (d.lp[c] - *ch[c]) * wet) * gain;
            }
            break;
        }
        case 2: // Mids: only the clashing middle (250 Hz .. 4 kHz) goes down; sub and air stay
        {
            const float g = sj::clamp(1.0f - amt, 0.0f, 1.0f); // as deep as Volume, in the middle band only
            for (int c = 0; c < n; ++c)
            {
                const float x = *ch[c];
                *ch[c] = x - (1.0f - g) * band[c];
                d.lp[c] = x;
            }
            break;
        }
        default: // Volume (original)
            for (int c = 0; c < n; ++c)
                d.lp[c] = *ch[c]; // Darken's low-pass follows the signal, ready for a switch
            for (int c = 0; c < n; ++c)
                *ch[c] *= 1.0f - amt;
            break;
    }
}

void RigEngine::renderHits(int frames)
{
    const int numBuses = c_maxBuses;
    // The gate listens to A + B before any ducking: it opens only at the loudest moments.
    const float sense = sj::clamp(m_params.hitSense, 0.0f, 1.0f);
    const float threshold = sj::dbToGain(-0.5f - 3.0f * sense);         // how close to the recent peak
    const float overAverage = sj::dbToGain(3.0f + 5.0f * (1.0f - sense)); // and how far above the average
    const float boost = sj::dbToGain(m_params.hitBoost);
    const float release = 1.0f - std::exp(-1.0f / (0.001f * sj::lerp(60.0f, 1200.0f, m_params.hitRelease) * m_fs));
    const float attack = coef(2.0f, m_fs);
    const float holdTime = 0.5f * sj::clamp(m_params.hitHold, 0.0f, 1.0f);
    const float floorGain = m_params.hitFloor > 0.001f ? sj::dbToGain(::Params::hitFloorDb(m_params.hitFloor)) : 0.0f;
    const int mode = m_params.hitAim ? 3 : m_params.gateMode; // AIM times the hits itself: the gate stays open
    const float levelUnder = sj::dbToGain(-2.0f - 28.0f * sense);           // Level: within 2 .. 30 dB of the recent peak
    const float jump = sj::dbToGain(5.55f - 4.2f * sense);                   // Jumps: how far above the slow envelope (5.6 .. 1.4 dB)
    const float duckDepth = 1.0f - sj::dbToGain(-::Params::hitDuckDb(m_params.hitDuck)); // at the hits' peaks
    const float slowCoef = 1.0f - std::exp(-1.0f / (0.12f * m_fs));
    const float returnCoef = duckReturnCoef();
    const float decay = std::exp(-1.0f / (6.0f * m_fs));
    const float avgCoef = 1.0f - std::exp(-1.0f / (2.0f * m_fs));
    const float dt = 1.0f / m_fs;
    float gateMax = 0.0f, hitsMax = 0.0f;
    const bool gateMarks = mode != 3 && ! m_params.hitSync && m_hasHitTracks;
    const bool gateStarts = mode != 3 && ! m_params.hitSync; // the gate opening is a hit starting (AIM and SYNC HITS cue their own)
    float gateWas = m_gate;
    // A stereo hit's sides: the second lane (see hitGains), for as long as a stereo hit track plays and a moment after.
    m_hitsSideHold = m_hitsSideFed ? static_cast<int>(c_hitSideTailSeconds * m_fs) : std::max(0, m_hitsSideHold - frames);
    const bool sides = m_hitsSideHold > 0;
    float midSq = 0.0f, sideSq = 0.0f;
    // Stowaway is ducked too, as one stream (before it is placed), in every mode but Sub: a hit wants the low end
    // under it. Leaving for Sub mid-duck, it comes back up at the return time instead of jumping.
    auto& stow = m_stow;
    const bool stowDucks = stow.now.mode != StowSub;
    const bool stowDuck = stow.sounding && (stowDucks || stow.ducked);
    const bool stowSides = stow.now.mode == StowDry && stow.dry.sideOn; // Dry's sides are ducked with it
    const bool stowStereo = stow.now.mode == StowFly;                   // Fly is left and right already

    for (int i = 0; i < frames; ++i)
    {
        const auto ii = static_cast<size_t>(i);
        float mono = 0.0f;
        if (m_room) // each bus's lanes as one stream
            for (const auto& bus : m_buses)
                mono += bus.l[ii] + bus.r[ii] + (bus.lanes == 4 ? bus.t[ii] : 0.0f);
        else
            for (int b = 0; b < numBuses; ++b)
                mono += 0.5f * (m_buses[static_cast<size_t>(b)].l[ii] + m_buses[static_cast<size_t>(b)].r[ii]);
        mono *= 0.5f;
        const float env = m_detect.process(mono);
        m_peakRef = std::max(env, m_peakRef * decay);
        m_average += (env - m_average) * avgCoef;
        m_slowEnv += (env - m_slowEnv) * slowCoef;
        bool open = false;
        switch (mode)
        {
            case 1:  open = env > m_peakRef * levelUnder && env > 0.003f; break;
            case 2:  open = env > m_slowEnv * jump && env > 0.003f; break;
            case 3:  open = true; break;
            default: open = env > m_peakRef * threshold && env > m_average * overAverage && env > 0.003f; break; // Peaks
        }
        if (open)
        {
            m_hold = holdTime;
            m_gate += (1.0f - m_gate) * attack;
        }
        else if (m_hold > 0.0f)
        {
            m_hold -= dt;
        }
        else
        {
            m_gate -= m_gate * release;
        }
        // The hit gate opening (not with AIM or SYNC HITS, which mark their own hits, nor with the gate held open).
        if (gateMarks && gateWas < 0.5f && m_gate >= 0.5f)
            mark(MarkHit, i);
        if (gateStarts && gateWas < 0.5f && m_gate >= 0.5f)
            m_hitStarted = m_hitWantsLoudest = true;
        gateWas = m_gate;
        gateMax = std::max(gateMax, m_gate);

        const float h = m_hitsIn[ii] * (floorGain + (1.0f - floorGain) * m_gate) * boost;
        m_hitsMid[ii] = h;
        const float hs = sides ? m_hitsSideIn[ii] * (floorGain + (1.0f - floorGain) * m_gate) * boost : 0.0f;
        m_hitsSide[ii] = hs;
        midSq += m_hitsIn[ii] * m_hitsIn[ii];
        sideSq += m_hitsSideIn[ii] * m_hitsSideIn[ii];

        // The gated hits duck both buses, so the hit punches through.
        // The duck follows the gated hits only (a raised Floor keeps the hits under the mix, it doesn't hold the buses down).
        // (A stereo hit: its two sides together, so one whose sides oppose each other ducks as well as any.)
        float gated = floorGain > 0.0f ? m_hitsIn[ii] * m_gate * boost : h;
        if (sides)
        {
            const float gatedSide = floorGain > 0.0f ? m_hitsSideIn[ii] * m_gate * boost : hs;
            gated = std::sqrt(gated * gated + gatedSide * gatedSide);
        }
        const float hitsLevel = sj::clamp(m_hitsFollower.process(gated) * 3.0f, 0.0f, 1.0f);
        hitsMax = std::max(hitsMax, hitsLevel);
        const float duck = duckDepth * hitsLevel;
        for (int b = 0; b < numBuses; ++b)
        {
            auto& bus = m_buses[static_cast<size_t>(b)];
            float* lanes[4] = { &bus.l[ii], &bus.r[ii], &bus.t[ii], &bus.sideL[ii] };
            duckSample(bus.byHits, duck, returnCoef, 1.0f - sj::dbToGain(-12.0f), lanes, bus.lanes);
        }
        if (stowDuck && stowStereo && m_room)
        {
            float* lanes[3] = { &stow.l[ii], &stow.r[ii], &stow.mono[ii] }; // Fly's three lanes
            duckSample(stow.byHits, stowDucks ? duck : 0.0f, returnCoef, 1.0f - sj::dbToGain(-12.0f), lanes, 3);
        }
        else if (stowDuck && stowStereo)
            duckSample(stow.byHits, stowDucks ? duck : 0.0f, returnCoef, 1.0f - sj::dbToGain(-12.0f), stow.l[ii], stow.r[ii]);
        else if (stowDuck)
        {
            float other = stow.mono[ii];
            duckSample(stow.byHits, stowDucks ? duck : 0.0f, returnCoef, 1.0f - sj::dbToGain(-12.0f), stow.mono[ii], stowSides ? stow.side[ii] : other);
        }
    }
    if (stowDuck)
        stow.ducked = stow.byHits.amount > 1.0e-4f;
    else if (stow.ducked)
    {
        // It went silent while ducked: nothing of that duck is left for when it sounds again.
        stow.ducked = false;
        stow.byHits.reset();
    }
    // How alike the hits' two sides are (see hitGains): the mid's and the side's power, each a slow mean, and from
    // them the mid's share of the two. The share is kept through the quiet after a hit (it still says what that
    // hit was).
    // A hit starting (this chunk four times as strong as the mean): a few milliseconds of a sound say little of
    // how alike its sides are (a low note whose sides are a quarter cycle apart starts all mid or all side), and
    // the hit's peak is in them. So it starts from what its files are as a whole (CargoData::midShare, by how loud
    // each track plays), counted as c_hitCarrySeconds of this sound, and the mean is over that and everything
    // since, until that is as long as the slow mean looks back.
    {
        const float mid = midSq / static_cast<float>(frames), side = sideSq / static_cast<float>(frames);
        const float now = mid + side, mean = m_hitMidPower + m_hitSidePower;
        m_hitOnset = false;
        if (now > 1.0e-12f && now > 4.0f * mean)
        {
            const float share = m_hitsFileAll > 0.0f ? m_hitsFileMid / m_hitsFileAll : m_hitMidShare;
            m_hitMidPower = share * now;
            m_hitSidePower = (1.0f - share) * now;
            m_hitSince = static_cast<int>(c_hitCarrySeconds * m_fs);
            m_hitOnset = mean < 1.0e-4f * now; // out of the quiet (see mixHits)
        }
        m_hitSince = std::min(m_hitSince + frames, 1 << 24);
        const float c = std::max(glide(frames, c_hitAlikeSeconds, m_fs), static_cast<float>(frames) / static_cast<float>(m_hitSince));
        m_hitMidPower += (mid - m_hitMidPower) * c;
        m_hitSidePower += (side - m_hitSidePower) * c;
        const float both = m_hitMidPower + m_hitSidePower;
        if (both > 1.0e-12f)
            m_hitMidShare = m_hitMidPower / both;
        else if (both < 1.0e-30f)
            m_hitMidPower = m_hitSidePower = 0.0f;
    }
    // The HIT GATE lamp: the gate, or with AIM (gate held open) the aimed hits themselves, so it flashes on each.
    m_gateView.store(m_params.hitAim ? hitsMax : gateMax, std::memory_order_relaxed);
}

void RigEngine::applyTremolo(PeakTremolo& t, float* const* ch, int n, int frames, bool selected, const float* detect)
{
    // A tremolo that deepens as the level nears its own recent peak: past Threshold it eases in, up to Depth at
    // the very peak, so the loudest moments shake and the rest stays steady. Off, or not the selected target, it
    // fades out but keeps tracking, so nothing is stale when it comes back.
    const float depthSet = (m_params.tremOn && selected) ? ::Params::tremDepthAmount(m_params.tremDepth) : 0.0f;
    const double inc = 1.5 * std::pow(20.0, static_cast<double>(sj::clamp(m_params.tremRate, 0.0f, 1.0f))) / m_sampleRate;
    const float th = ::Params::tremThresholdLevel(m_params.tremThreshold);
    const int shape = m_params.tremShape;
    for (int i = 0; i < frames; ++i)
    {
        float x = 0.0f;
        if (detect != nullptr)
            x = std::abs(detect[i]);
        else
            for (int c = 0; c < n; ++c)
                x = std::max(x, std::abs(ch[c][i]));
        t.env += (x - t.env) * (x > t.env ? m_tremAttack : m_tremRelease);
        t.peak = std::max(t.env, t.peak * m_tremPeakDecay);
        const float near = t.peak > 1.0e-4f ? t.env / t.peak : 0.0f;
        const float u = sj::clamp((near - th) / (1.0f - th), 0.0f, 1.0f);
        const float want = depthSet * u * u * (3.0f - 2.0f * u);
        t.depth += (want - t.depth) * (want > t.depth ? m_tremDepthUp : m_tremDepthDown);

        t.phase += inc;
        if (t.phase >= 1.0)
        {
            t.phase -= 1.0;
            t.from = t.to;
            t.to = t.rng.unipolar();
        }
        const float ph = static_cast<float>(t.phase);
        float m = 0.0f; // 0 = full level, 1 = the deepest dip
        switch (shape)
        {
            case 1:  m = 0.5f + 0.5f * std::tanh(6.0f * std::sin(sj::c_twoPi * ph)); break;                 // Square (soft edges)
            case 2:  m = t.from + (t.to - t.from) * ph * ph * (3.0f - 2.0f * ph); break;                    // Flutter
            default: m = 0.5f - 0.5f * std::cos(sj::c_twoPi * ph); break;                                  // Sine
        }
        if (t.depth > 1.0e-6f)
        {
            const float g = 1.0f - t.depth * m;
            for (int c = 0; c < n; ++c)
                ch[c][i] *= g;
        }
        else
            t.depth = 0.0f;
    }
}

float RigEngine::stepSpace(int frames, bool& wasOn)
{
    const float s = sj::clamp(m_spaceSpring.step(static_cast<float>(frames) / m_fs) + m_spaceModNow, 0.0f, 1.0f);
    wasOn = m_spaceOn;
    if (s < 1.0e-4f)
    {
        // Space is off. Empty the echoes and the reverbs once, so old sound isn't kept to come back when Space is
        // turned up again (their send is silent by now: nothing is cut off audibly).
        if (m_spaceOn)
        {
            m_spaceOn = false;
            m_echoL.reset(); m_echoR.reset();
            m_echoLpL.reset(); m_echoLpR.reset(); m_echoHpL.reset(); m_echoHpR.reset();
            m_reverb.reset();
            m_roomReverb.reset();
            m_roomTapsSnap = true;
        }
        return -1.0f;
    }
    m_spaceOn = true;
    return s;
}

void RigEngine::renderEchoes(int frames, float* left, float* right, float s, bool wasOn)
{
    // Ping-pong echo. The two times are not a simple ratio of each other and each drifts by a few milliseconds,
    // so the repeats never settle into a fixed pattern. Coming on, the send fades in (10 ms): fed from the middle
    // of a wave, the lines would start with a step that is heard an echo later as a click.
    const float fb = 0.25f + 0.4f * s;
    const float wet = 0.35f * s;
    const float driftStep = 2.0f / m_fs;
    if (! wasOn)
        m_echoSend = 0.0f;
    const float sendStep = 1.0f / (0.01f * m_fs);
    for (int i = 0; i < frames; ++i)
    {
        auto& l = left[i];
        auto& r = right[i];
        m_echoDriftL += c_echoDriftHzL * driftStep;
        m_echoDriftR += c_echoDriftHzR * driftStep;
        if (m_echoDriftL >= 1.0f) m_echoDriftL -= 2.0f;
        if (m_echoDriftR >= 1.0f) m_echoDriftR -= 2.0f;
        const float tL = (c_echoSecondsL + c_echoDriftSeconds * 4.0f * m_echoDriftL * (1.0f - std::abs(m_echoDriftL))) * m_fs;
        const float tR = (c_echoSecondsR + c_echoDriftSeconds * 4.0f * m_echoDriftR * (1.0f - std::abs(m_echoDriftR))) * m_fs;
        const float wl = m_echoL.read(static_cast<double>(tL)), wr = m_echoR.read(static_cast<double>(tR));
        if (m_echoSend < 1.0f)
            m_echoSend = std::min(1.0f, m_echoSend + sendStep);
        m_echoL.push(m_echoSend * l + fb * m_echoLpR.lowpass(m_echoHpR.highpass(wr)));
        m_echoR.push(m_echoSend * r + fb * m_echoLpL.lowpass(m_echoHpL.highpass(wl)));
        l += wet * wl;
        r += wet * wr;
    }
}

float RigEngine::stepSpaceGains(float s, bool wasOn)
{
    // The reverb's tail grows with Space; the dry is eased down a little as the wet comes up, so more Space is
    // more room and not more level into the limiter.
    const float dryGain = 1.0f - c_spaceDryTrim * s * s;
    const float wetGain = c_spaceWet * s;
    if (! wasOn)
    {
        m_spaceDry = dryGain;
        m_spaceWet = wetGain;
    }
    m_spaceDryFrom = m_spaceDry;
    const float wetFrom = m_spaceWet;
    m_spaceDry = dryGain;
    m_spaceWet = wetGain;
    return wetFrom;
}

bool RigEngine::renderSpace(int frames, float* left, float* right)
{
    bool wasOn = false;
    const float s = stepSpace(frames, wasOn);
    if (s < 0.0f)
        return false;
    renderEchoes(frames, left, right, s, wasOn);
    // Reverb, after the echoes (so they trail into it).
    m_reverb.setDecay(SpaceReverb::decaySecondsFor(s));
    const float wetFrom = stepSpaceGains(s, wasOn);
    m_reverb.process(left, right, frames, m_spaceDryFrom, m_spaceDry, wetFrom, m_spaceWet);
    return true;
}

void RigEngine::roomReverbTaps(int frames, RoomReverb::Taps& taps)
{
    // Which rows of the reverb's lines each speaker reads: L and R as the stereo reverb's L and R (a mid row plus
    // or minus 0.8 of a side row), every other speaker a row of its own. Speakers that never share a width share
    // a row (Ls and Lss, the tops of 7.1.2 and the front tops of 7.1.4), so a channel that turns from one into
    // the other in a change of width keeps reading the same reverb.
    struct Rows { int a; int b; float bSign; };
    auto rowsOf = [](room::Speaker s) -> Rows
    {
        using S = room::Speaker;
        switch (s)
        {
            case S::L:   return { 1, 2, 1.0f };
            case S::R:   return { 1, 2, -1.0f };
            case S::C:   return { 3, -1, 0.0f };
            case S::Ls:  case S::Lss: return { 4, -1, 0.0f };
            case S::Rs:  case S::Rss: return { 5, -1, 0.0f };
            case S::Lsr: return { 6, -1, 0.0f };
            case S::Rsr: return { 7, -1, 0.0f };
            case S::Ltf: case S::Lts: return { 8, -1, 0.0f };
            case S::Rtf: case S::Rts: return { 9, -1, 0.0f };
            case S::Ltr: return { 10, -1, 0.0f };
            case S::Rtr: return { 11, -1, 0.0f };
            case S::Lfe: break;
        }
        return { -1, -1, 0.0f };
    };
    constexpr float c_side = 0.8f, c_pairNorm = 0.78086881f; // 1 / sqrt(1 + 0.8^2): L and R each read one row's power

    // Where the room's sound has been: each channel's power, smoothed over about a third of a second.
    const auto& layout = room::layout(m_width);
    const int channels = layout.numChannels;
    const float leanGlide = glide(frames, c_spaceLeanSeconds, m_fs);
    for (int ch = 0; ch < m_liveChannels; ++ch)
    {
        double sum = 0.0;
        for (int i = 0; i < frames; ++i)
            sum += static_cast<double>(m_mix[static_cast<size_t>(ch)][static_cast<size_t>(i)]) * m_mix[static_cast<size_t>(ch)][static_cast<size_t>(i)];
        auto& e = m_spaceLean[static_cast<size_t>(ch)];
        e += (static_cast<float>(sum / frames) - e) * leanGlide;
    }

    // Each speaker's share: half evenly (the tops as far as Height lets them, the centre as far as Centre Speaker
    // does), half by where the sound has been. With nothing heard yet, all of it evenly.
    std::array<float, room::c_maxChannels> even {}, lean {};
    float evenSum = 0.0f, leanSum = 0.0f;
    for (int ch = 0; ch < channels; ++ch)
    {
        const auto speaker = layout.speakers[static_cast<size_t>(ch)];
        const auto& info = room::info(speaker);
        const float w = info.role == room::Role::Lfe ? 0.0f
                      : info.role == room::Role::Height ? m_heightNow
                      : speaker == room::Speaker::C ? m_centreNow : 1.0f;
        even[static_cast<size_t>(ch)] = w;
        lean[static_cast<size_t>(ch)] = w * m_spaceLean[static_cast<size_t>(ch)];
        evenSum += w;
        leanSum += lean[static_cast<size_t>(ch)];
    }
    const float leanShare = leanSum > 1.0e-12f ? c_spaceLean : 0.0f;

    taps = {};
    for (int ch = 0; ch < channels; ++ch)
    {
        const auto c = static_cast<size_t>(ch);
        const Rows rows = rowsOf(layout.speakers[c]);
        if (rows.a < 0)
            continue;
        const float share = (1.0f - leanShare) * even[c] / std::max(evenSum, 1.0e-6f)
                          + (leanShare > 0.0f ? leanShare * lean[c] / leanSum : 0.0f);
        const float g = std::sqrt(share * RoomReverb::c_stereoTapPower);
        if (rows.b < 0)
            taps[c][static_cast<size_t>(rows.a)] = g;
        else
        {
            taps[c][static_cast<size_t>(rows.a)] = g * c_pairNorm;
            taps[c][static_cast<size_t>(rows.b)] = rows.bSign * c_side * g * c_pairNorm;
        }
    }

    // Glide: no tap moves faster than from nothing to its fullest in 50 ms (a change of width hands a channel to
    // another speaker; Height and Centre Speaker glide already, and the lean is smoothed).
    if (m_roomTapsSnap)
    {
        m_roomTapsSnap = false;
        return;
    }
    const auto& from = m_roomReverb.taps();
    const float mostStep = std::sqrt(RoomReverb::c_stereoTapPower) * static_cast<float>(frames) / (0.05f * m_fs);
    for (size_t c = 0; c < taps.size(); ++c)
        for (size_t r = 0; r < taps[c].size(); ++r)
            taps[c][r] = from[c][r] + sj::clamp(taps[c][r] - from[c][r], -mostStep, mostStep);
}

bool RigEngine::renderSpaceRoom(int frames)
{
    // The echoes hear the room folded to stereo, and what they add comes back in L and R. The fold's gains glide
    // as the speakers' do (see mixRoom): a change of width gives the channels other speakers, and a fold that
    // changed at once would put a step into the echoes.
    const int channels = room::layout(m_width).numChannels;
    const auto& fold = room::fold(m_width, room::Width::Stereo);
    const auto foldFrom = m_spaceFold;
    const float mostStep = static_cast<float>(frames) / (c_gainSeconds * m_fs);
    for (size_t side = 0; side < 2; ++side)
        for (size_t ch = 0; ch < m_spaceFold[side].size(); ++ch)
        {
            const float target = static_cast<int>(ch) < channels ? fold.gain[side][ch] : 0.0f;
            m_spaceFold[side][ch] = m_spaceFoldSet ? m_spaceFold[side][ch] + sj::clamp(target - m_spaceFold[side][ch], -mostStep, mostStep) : target;
        }
    const auto fold0 = m_spaceFoldSet ? foldFrom : m_spaceFold;
    m_spaceFoldSet = true;

    bool wasOn = false;
    const float s = stepSpace(frames, wasOn);
    if (s < 0.0f)
        return false;

    float dryL[c_chunk], dryR[c_chunk];
    const float foldStep = 1.0f / static_cast<float>(frames);
    for (int i = 0; i < frames; ++i)
    {
        const float t = static_cast<float>(i + 1) * foldStep;
        float l = 0.0f, r = 0.0f;
        for (int ch = 0; ch < m_liveChannels; ++ch)
        {
            const auto c = static_cast<size_t>(ch);
            const float x = m_mix[c][static_cast<size_t>(i)];
            l += (fold0[0][c] + (m_spaceFold[0][c] - fold0[0][c]) * t) * x;
            r += (fold0[1][c] + (m_spaceFold[1][c] - fold0[1][c]) * t) * x;
        }
        m_spaceL[static_cast<size_t>(i)] = dryL[i] = l;
        m_spaceR[static_cast<size_t>(i)] = dryR[i] = r;
    }
    renderEchoes(frames, m_spaceL.data(), m_spaceR.data(), s, wasOn);
    for (int i = 0; i < frames; ++i)
    {
        m_mix[0][static_cast<size_t>(i)] += m_spaceL[static_cast<size_t>(i)] - dryL[i];
        m_mix[1][static_cast<size_t>(i)] += m_spaceR[static_cast<size_t>(i)] - dryR[i];
    }

    // The room's reverb, after the echoes (so they trail into it), on every speaker.
    if (! wasOn)
        m_roomTapsSnap = true;
    RoomReverb::Taps taps;
    roomReverbTaps(frames, taps);
    m_roomReverb.setDecay(SpaceReverb::decaySecondsFor(s));
    const float wetFrom = stepSpaceGains(s, wasOn);
    float* speakers[room::c_maxChannels];
    for (size_t ch = 0; ch < m_mix.size(); ++ch)
        speakers[ch] = m_mix[ch].data();
    if (! wasOn)
        m_roomReverb.snapTaps(taps); // coming on: the reverb is empty, so the taps start where they belong
    m_roomReverb.process(speakers, m_liveChannels, frames, m_spaceDryFrom, m_spaceDry, wetFrom, m_spaceWet, taps);
    return true;
}

void RigEngine::renderLfe(int frames)
{
    // The LFE is fed in a width with one; it is channel 3 in all of them. Leaving for Quad (where channel 3 is a
    // surround speaker), it glides out there as any speaker does.
    const auto& layout = room::layout(m_width);
    if (layout.lfe >= 0)
        m_lfeChannel = layout.lfe;
    const int into = m_lfeChannel;

    // Each source's gain into the LFE: LFE Level, for the source LFE Mode chooses. Mixdown takes half the sum of
    // the main speakers, so a sound in the middle of the front pair (each of L and R carrying it at -3 dB) reaches
    // the LFE at the level each of them carries, as the Sub does in Discrete. They glide (30 ms), so a change of
    // mode or level never steps.
    const float level = juce::Decibels::decibelsToGain(sj::clamp(m_params.lfeLevelDb, c_lfeMinDb, c_lfeMaxDb));
    const bool fed = layout.lfe >= 0;
    const float discreteTo = fed && m_params.lfeMode == LfeDiscrete ? level : 0.0f;
    const float mixdownTo = fed && m_params.lfeMode == LfeMixdown ? 0.5f * level : 0.0f;
    const float g = m_lfeSnap ? 1.0f : glide(frames, 0.03f, m_fs);
    const float discrete0 = m_lfeSnap ? discreteTo : m_lfeDiscrete, mixdown0 = m_lfeSnap ? mixdownTo : m_lfeMixdown;
    m_lfeSnap = false;
    m_lfeDiscrete = discrete0 + (discreteTo - discrete0) * g;
    m_lfeMixdown = mixdown0 + (mixdownTo - mixdown0) * g;
    if (m_lfeDiscrete < 1.0e-6f && discreteTo <= 0.0f) m_lfeDiscrete = 0.0f;
    if (m_lfeMixdown < 1.0e-6f && mixdownTo <= 0.0f) m_lfeMixdown = 0.0f;
    const bool sounds = into >= 0 && into < m_liveChannels && (discrete0 > 0.0f || mixdown0 > 0.0f || m_lfeDiscrete > 0.0f || m_lfeMixdown > 0.0f);

    // The mains' phase match (lfe::PhaseMatch): in only while the LFE carries a copy of what the mains carry,
    // which is Mixdown, or Discrete with Stowaway on in Sub (its Sub is in L and R as well). Otherwise the mains
    // go straight through. It comes and goes over half a second by moving the all-pass's frequency, never by
    // fading it against the plain signal: no level changes on the way and nothing cancels. A render that starts
    // with the LFE already set starts with it in place.
    const bool carries = fed && level > 0.0f
                         && (m_params.lfeMode == LfeMixdown || (m_params.lfeMode == LfeDiscrete && m_params.stow.on && m_params.stow.mode == StowSub));
    m_lfeCarries.store(carries, std::memory_order_relaxed);
    if (m_lfeMatchSnap)
    {
        m_lfeMatchSnap = false;
        m_lfeMatch = carries ? 1.0 : 0.0;
        for (auto& f : m_lfePhase)
        {
            f.reset();
            if (carries)
                f.setAmount(1.0);
        }
    }
    const double matchTo = carries ? 1.0 : 0.0;
    const double matchStep = 1.0 / (c_lfeMatchSeconds * static_cast<double>(m_fs));

    const float step = 1.0f / static_cast<float>(frames);
    for (int i = 0; i < frames; ++i)
    {
        const auto ii = static_cast<size_t>(i);
        // The mains' sum for Mixdown, taken before the phase match; then every channel through the match while
        // any of it is in (the LFE's own channel holds nothing here but a speaker gliding out after a change of
        // width).
        float sum = 0.0f;
        for (int ch = 0; ch < m_liveChannels; ++ch)
            if (ch != into)
                sum += m_mix[static_cast<size_t>(ch)][ii];
        const bool moving = ! std::equal_to<double>()(m_lfeMatch, matchTo);
        const bool wasOut = m_lfeMatch <= 0.0;
        if (moving)
            m_lfeMatch = matchTo > m_lfeMatch ? std::min(matchTo, m_lfeMatch + matchStep) : std::max(matchTo, m_lfeMatch - matchStep);
        if (m_lfeMatch > 0.0)
            for (int ch = 0; ch < m_liveChannels; ++ch)
            {
                auto& x = m_mix[static_cast<size_t>(ch)][ii];
                auto& f = m_lfePhase[static_cast<size_t>(ch)];
                if (wasOut)
                    f.startAsWire(x); // coming in from a plain wire: as if it had been one all along
                if (moving)
                    f.setAmount(m_lfeMatch);
                x = f.process(x);
            }
        if (! sounds)
            continue;
        const float t = static_cast<float>(i + 1) * step;
        const float in = (discrete0 + (m_lfeDiscrete - discrete0) * t) * m_stem[ii] + (mixdown0 + (m_lfeMixdown - mixdown0) * t) * sum;
        m_mix[static_cast<size_t>(into)][ii] += m_lfeLow.process(in);
    }
    if (! sounds)
        m_lfeLow.reset(); // nothing in it (its input has been silent since the gains reached 0)
}

//==============================================================================
void RigEngine::process(const float* inL, const float* inR, float* outL, float* outR, int numSamples)
{
    float* out[2] = { outL, outR };
    process(inL, inR, out, 2, numSamples);
}

void RigEngine::process(const float* inL, const float* inR, float* const* out, int numChannels, int numSamples)
{
    juce::ignoreUnused(inL, inR); // sample-based: the plug-in has no audio input
    jassert(numChannels >= 2);
    float* const outL = out[0];
    float* const outR = out[1];
    int offset = 0;
    m_numMarkers = 0;
    while (offset < numSamples)
    {
        const int frames = std::min(c_chunk, numSamples - offset);
        m_chunkOffset = offset;

        // A seeded restart waits for the output to have faded out. It always takes the fade's time, sounding or
        // not, so the new run starts the same number of samples after it was asked for every time. Throws asked
        // for meanwhile wait for the new run.
        if (m_reseedPending && m_outFade <= 0.0f)
            reseedNow();
        // The width (see setWidth), and with it whether this chunk is the stereo engine or a room.
        float widthFade0 = 1.0f, widthFade1 = 1.0f;
        stepWidth(frames, widthFade0, widthFade1);
        const bool room = m_room;
        const int roomChannels = room ? m_liveChannels : 2;
        if (room)
            stepRoom(frames);
        if (! m_reseedPending)
            startThrows(frames);
        renderTracks(frames);

        // Bus A first: it is the key for the others (ducking and filtering).
        const int numBuses = c_maxBuses;
        auto& keyBus = m_buses[0];
        // Turning LINK on or off fades bus B out (~30 ms), swaps (lining B up with A's passes), and fades it back
        // in, so B never jumps to a new place in its delay mid-pass.
        const float linkFade0 = m_linkFade;
        {
            const float linkStep = static_cast<float>(frames) / (0.03f * m_fs);
            if (m_params.linkBuses != m_linkNow)
            {
                m_linkFade = std::max(0.0f, m_linkFade - linkStep);
                if (m_linkFade <= 0.0f)
                {
                    m_linkNow = m_params.linkBuses;
                    if (m_linkNow)
                        for (int b = 1; b < numBuses; ++b)
                            for (size_t k = 0; k < keyBus.heads.size(); ++k)
                                followPass(m_buses[static_cast<size_t>(b)].heads[k], keyBus.heads[k],
                                           sj::clamp(m_buses[static_cast<size_t>(b)].springs[2].value + m_busModNow[static_cast<size_t>(b)][2], 0.0f, 1.0f), true);
                }
            }
            else
                m_linkFade = std::min(1.0f, m_linkFade + linkStep);
        }
        const float linkFade1 = m_linkFade;
        const bool link = m_linkNow;
        if (link)
            m_leadStart = keyBus.heads; // A's passes before this chunk, for B to line up with
        // Markers: which passes and throws are still on their way in before this chunk (see below).
        std::array<std::array<bool, 2>, c_maxBuses> passComing {};
        std::array<bool, c_throws> throwComing {};
        for (size_t b = 0; b < m_buses.size(); ++b)
            for (size_t k = 0; k < 2; ++k)
                passComing[b][k] = m_buses[b].heads[k].time < static_cast<double>(m_buses[b].heads[k].lenIn);
        for (size_t v = 0; v < m_throws.size(); ++v)
            throwComing[v] = m_throws[v].active && keyBus.throwHeads[v].time < static_cast<double>(keyBus.throwHeads[v].lenIn);
        renderBus(keyBus, frames, 0.0f, true);
        for (int b = 1; b < numBuses; ++b)
        {
            auto& bus = m_buses[static_cast<size_t>(b)];
            renderBus(bus, frames, keyLevel(keyBus) * (1.0f - m_morphMixNow), false, link ? &keyBus.heads : nullptr, link ? &m_leadStart : nullptr);
            if (linkFade0 < 1.0f || linkFade1 < 1.0f)
                for (int i = 0; i < frames; ++i)
                {
                    const float g = linkFade0 + (linkFade1 - linkFade0) * static_cast<float>(i + 1) / static_cast<float>(frames);
                    bus.l[static_cast<size_t>(i)] *= g;
                    bus.r[static_cast<size_t>(i)] *= g;
                    if (bus.lanes == 4)
                    {
                        bus.t[static_cast<size_t>(i)] *= g;
                        bus.sideL[static_cast<size_t>(i)] *= g;
                    }
                }

            // B is ducked by A, up to -20 dB at A's peaks (by A's level against its own recent peak). Take turns:
            // whichever is louder (before any ducking, so no feedback) ducks the other.
            // While morphing, A and B carry the same sound, so A mustn't duck B (it would slide B away as X moves).
            const float amount = sj::clamp(bus.springs[7].value + m_busModNow[static_cast<size_t>(b)][7], 0.0f, 1.0f) * (1.0f - m_morphMixNow);
            auto duckFor = [amount](float keyed) { return 1.0f - sj::dbToGain(-20.0f * amount * keyed * std::sqrt(keyed)); };
            float duckTarget = duckFor(keyLevel(keyBus));
            float keyTarget = 0.0f;
            // The other bus must be clearly louder (2 dB) for 30 ms before the turn passes, so similar levels don't chatter.
            if (m_params.takeTurns && b == 1)
            {
                const bool otherLouder = m_talker == 0 ? bus.env > keyBus.env * 1.26f : keyBus.env > bus.env * 1.26f;
                m_talkerHold = otherLouder ? m_talkerHold + static_cast<float>(frames) / m_fs : 0.0f;
                if (m_talkerHold > 0.03f)
                {
                    m_talker = 1 - m_talker;
                    m_talkerHold = 0.0f;
                }
            }
            else if (b == 1)
            {
                m_talker = 0;
                m_talkerHold = 0.0f;
            }
            if (m_params.takeTurns && m_talker == 1)
            {
                keyTarget = duckFor(keyLevel(bus));
                duckTarget = 0.0f;
            }
            const float duck0 = bus.duck, key0 = keyBus.duck;
            const float duckGlide = glide(frames, c_duckSeconds, m_fs);
            bus.duck += (duckTarget - bus.duck) * duckGlide;
            if (b == 1)
                keyBus.duck += (keyTarget - keyBus.duck) * duckGlide;
            const bool duckKey = b == 1 && (m_params.takeTurns || keyBus.duck > 1.0e-4f || key0 > 1.0e-4f || keyBus.byBus.amount > 1.0e-4f);
            const float returnCoef = duckReturnCoef();
            for (int i = 0; i < frames; ++i)
            {
                const auto ii = static_cast<size_t>(i);
                const float t = static_cast<float>(i + 1) / static_cast<float>(frames);
                float* lanes[4] = { &bus.l[ii], &bus.r[ii], &bus.t[ii], &bus.sideL[ii] }; // left and right, or a room's lanes
                duckSample(bus.byBus, duck0 + (bus.duck - duck0) * t, returnCoef, 0.9f, lanes, bus.lanes);
                if (duckKey)
                {
                    float* keyLanes[4] = { &keyBus.l[ii], &keyBus.r[ii], &keyBus.t[ii], &keyBus.sideL[ii] };
                    duckSample(keyBus.byBus, key0 + (keyBus.duck - key0) * t, returnCoef, 0.9f, keyLanes, keyBus.lanes);
                }
            }
        }

        // Markers: a pass or a throw that went by its closest point in this chunk.
        {
            auto crossedAt = [&](const Head& h)
            {
                const double past = (h.time - static_cast<double>(h.lenIn)) * m_sampleRate; // samples since the peak
                return static_cast<int>(sj::clamp(static_cast<float>(frames) - static_cast<float>(past), 0.0f, static_cast<float>(frames - 1)));
            };
            for (size_t b = 0; b < m_buses.size() && b < 2; ++b)
            {
                if (m_releaseGain <= 0.0f || m_buses[b].env < 0.002f || (b > 0 && link))
                    continue;
                for (size_t k = 0; k < 2; ++k)
                    if (const auto& h = m_buses[b].heads[k]; passComing[b][k] && h.time >= static_cast<double>(h.lenIn))
                        mark(b == 0 ? MarkPassA : MarkPassB, crossedAt(h));
            }
            for (size_t v = 0; v < m_throws.size(); ++v)
                if (const auto& h = keyBus.throwHeads[v]; throwComing[v] && m_throws[v].active && h.time >= static_cast<double>(h.lenIn))
                    mark(MarkThrow, crossedAt(h));
        }

        // A throw is over when its tail has gone (its window is shut from there on).
        {
            float peakIn = -1.0f;
            for (size_t v = 0; v < m_throws.size(); ++v)
            {
                if (! m_throws[v].active)
                    continue;
                const auto& h = keyBus.throwHeads[v];
                if (h.time >= static_cast<double>(h.lenIn + c_flyTail * h.lenOut))
                    m_throws[v].active = false;
                else if (h.time < static_cast<double>(h.lenIn))
                    peakIn = std::max(peakIn, static_cast<float>(static_cast<double>(h.lenIn) - h.time)); // the newest is the furthest off
            }
            m_throwPeakView.store(peakIn, std::memory_order_relaxed);
        }

        // Stowaway: after the buses (bus A's level and passes are final for this chunk) and before the hits,
        // which duck it; then it is placed.
        renderStow(frames);
        renderHits(frames);
        placeHits(frames);
        const bool stowSounds = m_stow.sounding;
        const bool stowDry = stowSounds && m_stow.now.mode == StowDry; // it joins after Space (below), not here
        if (stowSounds)
            placeStow(frames);

        // Peak tremolo on the buses and on the hits (each only deepens when it's the selected target). The two buses
        // share the tremolo's timing (so they dip together) while each deepens by its own level.
        {
            const bool buses = m_params.tremTarget == 1;
            const auto lfo = m_trem[1]; // bus A's tremolo before this chunk: bus B starts from the same timing
            // (Left and right, or a room's lanes: the hits are one lane there.)
            float* lanesA[4] = { m_buses[0].l.data(), m_buses[0].r.data(), m_buses[0].t.data(), m_buses[0].sideL.data() };
            applyTremolo(m_trem[1], lanesA, m_buses[0].lanes, frames, buses);
            if (numBuses > 1)
            {
                m_trem[2].phase = lfo.phase;
                m_trem[2].from = lfo.from;
                m_trem[2].to = lfo.to;
                m_trem[2].rng = lfo.rng;
                float* lanesB[4] = { m_buses[1].l.data(), m_buses[1].r.data(), m_buses[1].t.data(), m_buses[1].sideL.data() };
                applyTremolo(m_trem[2], lanesB, m_buses[1].lanes, frames, buses);
            }
            // (The hits: their lane, and a stereo hit's sides with it.)
            float* hits[2] = { m_hitsMid.data(), m_hitsSide.data() };
            applyTremolo(m_trem[3], hits, m_hitsSideHold > 0 ? 2 : 1, frames, m_params.tremTarget == 2);
        }

        if (room)
            mixRoom(frames, stowSounds, stowDry);
        else
        {
            // The hits at their place between left and right (see hitGains): their two lanes into m_hitsOutL and
            // m_hitsOutR.
            mixHits(frames);
            for (int i = 0; i < frames; ++i)
            {
                const auto ii = static_cast<size_t>(i);
                float l = m_hitsOutL[ii], r = m_hitsOutR[ii];
                m_scopes[2].push(0.5f * (l + r));
                m_sectionSquares[2] += 0.5 * (static_cast<double>(l) * l + static_cast<double>(r) * r);
                for (int b = 0; b < numBuses; ++b)
                {
                    const auto& bus = m_buses[static_cast<size_t>(b)];
                    l += bus.l[ii];
                    r += bus.r[ii];
                    if (b < 2)
                    {
                        m_scopes[static_cast<size_t>(b)].push(0.5f * (bus.l[ii] + bus.r[ii]));
                        m_sectionSquares[static_cast<size_t>(b)] += 0.5 * (static_cast<double>(bus.l[ii]) * bus.l[ii]
                                                                           + static_cast<double>(bus.r[ii]) * bus.r[ii]);
                    }
                }
                // Stowaway joins only while it holds sound: with nothing sent to it, the mix is the same sum as without it.
                const float stowL = stowSounds ? m_stow.l[ii] : 0.0f, stowR = stowSounds ? m_stow.r[ii] : 0.0f;
                if (stowSounds)
                {
                    if (! stowDry)
                    {
                        l += stowL;
                        r += stowR;
                    }
                    m_sectionSquares[SectionStow] += 0.5 * (static_cast<double>(stowL) * stowL + static_cast<double>(stowR) * stowR);
                }
                m_scopes[SectionStow].push(0.5f * (stowL + stowR));
                ++m_sectionSamples;
                m_mix[0][ii] = l;
                m_mix[1][ii] = r;
            }
        }
        // Ride make-up (see rideMakeup): ramped across the chunk. Stowaway in Dry is not in the mix yet (it joins
        // after Space, below), so it takes the same make-up here.
        {
            const float g0 = m_makeup;
            m_makeup += (m_makeupTarget - m_makeup) * glide(frames, 0.05f, m_fs);
            const bool eased = g0 < 0.9999f || m_makeupTarget < 0.9999f;
            if (eased)
                for (int i = 0; i < frames; ++i)
                {
                    const float g = g0 + (m_makeup - g0) * static_cast<float>(i + 1) / static_cast<float>(frames);
                    m_mix[0][static_cast<size_t>(i)] *= g;
                    m_mix[1][static_cast<size_t>(i)] *= g;
                    for (int ch = 2; ch < roomChannels; ++ch)
                        m_mix[static_cast<size_t>(ch)][static_cast<size_t>(i)] *= g;
                    if (room)
                        m_roomMono[static_cast<size_t>(i)] *= g;
                    if (stowDry)
                    {
                        m_stow.l[static_cast<size_t>(i)] *= g;
                        m_stow.r[static_cast<size_t>(i)] *= g;
                    }
                }
            else
                m_makeup = 1.0f;
            // A room: Stowaway's Sub as a stem (see getSubStem): what L and R each got of it, make-up and all. The
            // LFE's copy (m_stem) goes on to take what L and R take after this: the tremolo, the fades and Space's dry.
            if (room)
            {
                const bool sub = stowSounds && m_stow.now.mode == StowSub;
                for (int i = 0; i < frames; ++i)
                {
                    const float x = ! sub ? 0.0f
                        : m_stow.mono[static_cast<size_t>(i)] * (eased ? g0 + (m_makeup - g0) * static_cast<float>(i + 1) / static_cast<float>(frames) : 1.0f);
                    m_stem[static_cast<size_t>(i)] = x;
                    if (offset + i < c_stemSamples)
                        m_subStem[static_cast<size_t>(offset + i)] = x;
                }
            }
        }
        // Peak tremolo on everything (by the mix's level), before Space so the echoes and reverb carry it. A room's
        // speakers shake as one, by the mix as one stream (the loudest speaker would dip as a pass crosses from
        // one speaker to the next).
        {
            // (The LFE's copy of the Sub shakes with them: it rides along as one more channel.)
            float* speakers[room::c_maxChannels + 1];
            for (size_t ch = 0; ch < m_mix.size(); ++ch)
                speakers[ch] = m_mix[ch].data();
            speakers[roomChannels] = m_stem.data();
            applyTremolo(m_trem[0], speakers, room ? roomChannels + 1 : roomChannels, frames, m_params.tremTarget == 0, room ? m_roomMono.data() : nullptr);
        }
        // Around a change between stereo and a room the mix fades here as well as at the output (below): what the
        // echoes and the reverb are sent is then silent at the swap too, and nothing in it steps. (This fade is
        // level where the output's begins, so the two together start no more sharply than the output's alone.)
        if (widthFade0 < 1.0f || widthFade1 < 1.0f)
            for (int i = 0; i < frames; ++i)
            {
                const float g = widthFade0 + (widthFade1 - widthFade0) * static_cast<float>(i + 1) / static_cast<float>(frames);
                const float send = g * (2.0f - g);
                for (int ch = 0; ch < roomChannels; ++ch)
                    m_mix[static_cast<size_t>(ch)][static_cast<size_t>(i)] *= send;
                m_stem[static_cast<size_t>(i)] *= send;
            }
        if (room)
        {
            if (renderSpaceRoom(frames))
            {
                // Space eased the dry sound down in every speaker: the LFE's copy of the Sub with it.
                const float dry0 = m_spaceDryFrom, dry1 = m_spaceDry;
                const float step = 1.0f / static_cast<float>(frames);
                for (int i = 0; i < frames; ++i)
                    m_stem[static_cast<size_t>(i)] *= dry0 + (dry1 - dry0) * static_cast<float>(i + 1) * step;
            }
        }
        else
            renderSpace(frames, m_mix[0].data(), m_mix[1].data());
        // Stowaway in Dry: past the Everything tremolo and Space, straight to the output stage.
        if (stowDry)
            for (int i = 0; i < frames; ++i)
            {
                m_mix[0][static_cast<size_t>(i)] += m_stow.l[static_cast<size_t>(i)];
                m_mix[1][static_cast<size_t>(i)] += m_stow.r[static_cast<size_t>(i)];
            }

        if (room)
            renderLfe(frames);

        const int written = std::min(numChannels, roomChannels);
        if (room)
            for (int i = 0; i < frames; ++i)
            {
                float y[room::c_maxChannels];
                float sum = 0.0f;
                bool finite = true;
                for (int ch = 0; ch < roomChannels; ++ch)
                {
                    y[ch] = m_dc[static_cast<size_t>(ch)].process(m_mix[static_cast<size_t>(ch)][static_cast<size_t>(i)]);
                    finite = finite && std::isfinite(y[ch]);
                    sum += y[ch];
                }
                if (! finite)
                {
                    for (int ch = 0; ch < roomChannels; ++ch)
                        y[ch] = 0.0f;
                    sum = 0.0f;
                    reset();
                }
                m_mixFollower.process(0.5f * sum); // a sound at the phantom centre reads as it does in stereo
                for (int ch = 0; ch < written; ++ch)
                    out[ch][offset + i] = y[ch];
            }
        else
            for (int i = 0; i < frames; ++i)
            {
                float l = m_dc[0].process(m_mix[0][static_cast<size_t>(i)]);
                float r = m_dc[1].process(m_mix[1][static_cast<size_t>(i)]);
                if (! std::isfinite(l) || ! std::isfinite(r))
                {
                    l = r = 0.0f;
                    reset();
                }
                m_mixFollower.process(0.5f * (l + r));
                outL[offset + i] = l;
                outR[offset + i] = r;
            }
        for (int ch = written; ch < numChannels; ++ch) // channels the width does not have
            std::fill_n(out[ch] + offset, frames, 0.0f);
        // Around a seeded restart: out in 5 ms, back in as fast.
        if (m_reseedPending || m_outFade < 1.0f)
        {
            const float fade0 = m_outFade;
            const float step = static_cast<float>(frames) / (c_reseedFadeSeconds * m_fs);
            m_outFade = m_reseedPending ? std::max(0.0f, m_outFade - step) : std::min(1.0f, m_outFade + step);
            for (int i = 0; i < frames; ++i)
            {
                const float g = fade0 + (m_outFade - fade0) * static_cast<float>(i + 1) / static_cast<float>(frames);
                outL[offset + i] *= g;
                outR[offset + i] *= g;
                for (int ch = 2; ch < written; ++ch)
                    out[ch][offset + i] *= g;
            }
        }
        // Around a change between stereo and a room.
        if (widthFade0 < 1.0f || widthFade1 < 1.0f)
            for (int i = 0; i < frames; ++i)
            {
                const float g = widthFade0 + (widthFade1 - widthFade0) * static_cast<float>(i + 1) / static_cast<float>(frames);
                for (int ch = 0; ch < written; ++ch)
                    out[ch][offset + i] *= g;
            }
        offset += frames;
    }
    publishViews();
}

void RigEngine::publishViews()
{
    for (int b = 0; b < c_maxBuses; ++b)
    {
        const auto& bus = m_buses[static_cast<size_t>(b)];
        const float level = sj::clamp(bus.env * 3.0f, 0.0f, 1.0f);
        m_busView[static_cast<size_t>(b)].store(level, std::memory_order_relaxed);
        for (int h = 0; h < 2; ++h)
        {
            const auto& head = bus.heads[static_cast<size_t>(h)];
            const auto i = static_cast<size_t>(b * 2 + h);
            m_viewData[i * 4 + 0].store(head.view * level, std::memory_order_relaxed);
            m_viewData[i * 4 + 1].store(head.x, std::memory_order_relaxed);
            m_viewData[i * 4 + 2].store(head.closeness, std::memory_order_relaxed);
            m_viewData[i * 4 + 3].store(head.age, std::memory_order_relaxed);
        }
    }
    // The throws, after the buses' passes: bus A's copy of each (every bus flies the same one).
    for (size_t v = 0; v < m_throws.size(); ++v)
    {
        const auto& head = m_buses[0].throwHeads[v];
        const auto i = static_cast<size_t>(c_maxBuses * 2) + v;
        const bool on = m_throws[v].active;
        const float level = sj::clamp(std::max(m_buses[0].env, m_buses[c_maxBuses - 1].env) * 3.0f, 0.0f, 1.0f);
        m_viewData[i * 4 + 0].store(on ? head.view * level : 0.0f, std::memory_order_relaxed);
        m_viewData[i * 4 + 1].store(head.x, std::memory_order_relaxed);
        m_viewData[i * 4 + 2].store(head.closeness, std::memory_order_relaxed);
        m_viewData[i * 4 + 3].store(on ? head.age : 0.0f, std::memory_order_relaxed);
    }
    m_stowLevelView.store(sj::clamp(m_stow.env * 3.0f, 0.0f, 1.0f), std::memory_order_relaxed);
    m_stowPanView.store(m_stow.pan, std::memory_order_relaxed);
    {
        // Fly's own two passes (nothing while it sleeps or in another mode).
        const bool flying = m_stow.now.mode == StowFly && m_stow.fly.on && m_stow.sounding;
        const float level = sj::clamp(m_stow.env * 3.0f, 0.0f, 1.0f);
        for (size_t h = 0; h < 2; ++h)
        {
            const auto& head = m_stow.fly.heads[h];
            m_stowFlightView[h * 4 + 0].store(flying ? head.view * level : 0.0f, std::memory_order_relaxed);
            m_stowFlightView[h * 4 + 1].store(head.x, std::memory_order_relaxed);
            m_stowFlightView[h * 4 + 2].store(head.closeness, std::memory_order_relaxed);
            m_stowFlightView[h * 4 + 3].store(flying ? head.age : 0.0f, std::memory_order_relaxed);
        }
    }
    m_energyView.store(sj::clamp(m_mixFollower.env * 3.0f, 0.0f, 1.0f), std::memory_order_relaxed);

    // The room's objects: where each is and how loud (nothing at Mono and Stereo).
    auto object = [this](int which, const room::Position& at, float level)
    {
        const auto i = static_cast<size_t>(which) * 4;
        m_objectView[i + 0].store(at.x, std::memory_order_relaxed);
        m_objectView[i + 1].store(at.y, std::memory_order_relaxed);
        m_objectView[i + 2].store(at.z, std::memory_order_relaxed);
        m_objectView[i + 3].store(m_room ? level : 0.0f, std::memory_order_relaxed);
    };
    for (int b = 0; b < c_maxBuses; ++b)
    {
        const auto& bus = m_buses[static_cast<size_t>(b)];
        const float level = sj::clamp(bus.env * 3.0f, 0.0f, 1.0f);
        for (int h = 0; h < 2; ++h)
            object(ObjPassA1 + b * 2 + h, bus.at[static_cast<size_t>(h)], bus.heads[static_cast<size_t>(h)].view * level);
    }
    {
        const float level = sj::clamp(std::max(m_buses[0].env, m_buses[c_maxBuses - 1].env) * 3.0f, 0.0f, 1.0f);
        for (size_t v = 0; v < m_throws.size(); ++v)
            object(ObjThrow1 + static_cast<int>(v), m_buses[0].at[2 + v], m_throws[v].active ? m_buses[0].throwHeads[v].view * level : 0.0f);
    }
    object(ObjHits, m_hitsAt, sj::clamp(m_hitsFollower.env * 3.0f, 0.0f, 1.0f));
    {
        const float apart = c_hitApart + (1.0f - c_hitApart) * m_hitSpreadNow;
        const float view[6] = { m_hitsAt.x, m_hitsAt.y, m_hitsAt.z, m_hitSpreadNow, apart, sj::clamp(m_hitsFollower.env * 3.0f, 0.0f, 1.0f) };
        for (size_t i = 0; i < m_hitView.size(); ++i)
            m_hitView[i].store(view[i], std::memory_order_relaxed);
    }
    {
        const bool flying = m_stow.now.mode == StowFly;
        const float level = m_stow.sounding ? sj::clamp(m_stow.env * 3.0f, 0.0f, 1.0f) : 0.0f;
        object(ObjStow1, m_stow.at[0], flying ? m_stow.fly.heads[0].view * level : level);
        object(ObjStow2, m_stow.at[1], flying ? m_stow.fly.heads[1].view * level : 0.0f);
    }
}

RigEngine::ObjectView RigEngine::getObject(int which) const
{
    ObjectView v;
    const auto i = static_cast<size_t>(juce::jlimit(0, c_objects - 1, which)) * 4;
    v.x = m_objectView[i + 0].load(std::memory_order_relaxed);
    v.y = m_objectView[i + 1].load(std::memory_order_relaxed);
    v.z = m_objectView[i + 2].load(std::memory_order_relaxed);
    v.level = m_objectView[i + 3].load(std::memory_order_relaxed);
    v.active = v.level > 0.01f;
    return v;
}

//==============================================================================
// The room (see the class comment).
void RigEngine::BusState::resetThird()
{
    for (auto* f : { &shelfT, &keyT })
        f->reset();
    sweep.resetLane(2);
    for (auto& a : phT)
        a = {};
    phFbT = flFbT = 0.0f;
    if (! flT.buffer.empty())
        flT.reset();
    toneT.reset();
    thirdOn = false;
    thirdTail = 0;
}

void RigEngine::PathGlide::step(int wanted, float seconds)
{
    if (mix >= 1.0f && wanted != to)
    {
        from = to;
        to = wanted;
        mix = 0.0f;
    }
    if (mix < 1.0f)
        mix = std::min(1.0f, mix + seconds / c_pathSeconds);
}

float RigEngine::pathProgress(float u, float shape)
{
    // Where each window comes within c_heardLevel of its top. Bed: sin(pi u). Fly-By: the raised cosines of
    // passWindow, turned round: 0.5 - 0.5 cos(pi (2u)^approach) on the way in, 0.5 + 0.5 cos(pi (2u - 1) / tail)
    // on the way out.
    static const float bedFrom = std::asin(c_heardLevel) / sj::c_pi;
    static const float flyFrom = 0.5f * std::pow(std::acos(1.0f - 2.0f * c_heardLevel) / sj::c_pi, 1.0f / c_flyApproach);
    static const float flyTo = 0.5f + 0.5f * c_flyTail * std::acos(2.0f * c_heardLevel - 1.0f) / sj::c_pi;
    const float s = sj::clamp(shape, 0.0f, 1.0f);
    const float from = bedFrom + s * (flyFrom - bedFrom);
    const float to = (1.0f - bedFrom) + s * (flyTo - (1.0f - bedFrom));
    // The nearest point stays the middle of the path: the way in is flown over the heard part of the approach,
    // the way out over the heard part of the tail.
    return u < 0.5f ? 0.5f * sj::clamp((u - from) / (0.5f - from), 0.0f, 1.0f)
                    : 0.5f + 0.5f * sj::clamp((u - 0.5f) / (to - 0.5f), 0.0f, 1.0f);
}

float RigEngine::orbitLoudness(Head& h, float doppler, float shape)
{
    const std::array<float, 7> now { doppler, shape, h.machRef, h.lenIn, h.lenOut, h.closest, h.mach };
    if (now == h.orbitFor)
        return h.orbitLoud;
    h.orbitFor = now;
    // A pass's gain is window * (1 + 0.8 Doppler (nearness - 0.35)): on the other paths the nearness swells to 1
    // at the nearest point, round the listener it is held. The held value L that gives the circle the energy of
    // the swell under the same window:  (1 + 0.8 D (L - 0.35))^2 * sum(window^2) = sum(window^2 * (1 + 0.8 D
    // (nearness - 0.35))^2), summed over the pass in time (its two halves can differ in length). How wide the
    // swell is depends on Doppler (the pass's speed), on how long the pass is and on the window, so no one value
    // is right for them all. (The level is that of a pass of the middle closeness, whatever the circle's radius.)
    //
    // The air: a crossing pass is dull far off and bright only as it goes by (roomAir), and a circle held as
    // bright as that brightest moment all the way round was louder for it: with the level held at one value as
    // well, up to 2 dB louder than a crossing at Doppler 100%. It is held at the swell's own brightness, averaged
    // (in octaves) over the pass's energy.
    //
    // The bend: a crossing pass is read faster on the way in and slower on the way out, which moves its sound up
    // and then down the spectrum, and that costs it loudness a circle (no bend) does not lose: measured against
    // Stereo with the Output test's sources, c_orbitBendDb * speed^2 (0.1 dB at the Doppler and Bend the buses
    // open with, 0.7 dB with Doppler at 100%). The circle's level is brought down by as much.
    const float d = sj::clamp(doppler, 0.0f, 1.0f);
    const float middle = 0.004f * std::exp2(5.0f * (1.0f - c_orbitCloseness));
    constexpr int steps = 48;
    float swell = 0.0f, flat = 0.0f, air = 0.0f, airWeight = 0.0f;
    for (int i = 0; i < steps; ++i)
    {
        const float u = (static_cast<float>(i) + 0.5f) / static_cast<float>(steps);
        const float half = u < 0.5f ? h.lenIn : h.lenOut;
        const float x = h.machRef * (2.0f * u - 1.0f) * half;
        const float w = passWindow(u, shape);
        const float g = w * (1.0f + 0.8f * d * (middle / std::sqrt(x * x + middle * middle) - 0.35f));
        swell += g * g * half;
        flat += w * w * half;
        // (The pass's own closeness here: how bright a pass is goes by how near it comes.)
        const float near = std::pow(sj::clamp(0.006f / std::sqrt(x * x + h.closest * h.closest), 0.0f, 1.0f), 0.6f);
        const float own = w * (1.0f + 0.8f * d * (h.closest / std::sqrt(x * x + h.closest * h.closest) - 0.35f));
        air += own * own * half * std::log(std::max(1.0e-3f, near * (0.3f + 0.7f * std::sin(sj::c_pi * u))));
        airWeight += own * own * half;
    }
    h.orbitAir = std::exp(air / std::max(airWeight, 1.0e-9f));
    if (d < 1.0e-3f)
        return h.orbitLoud = 0.35f;
    const float bend = sj::dbToGain(-c_orbitBendDb * h.mach * h.mach);
    return h.orbitLoud = sj::clamp(0.35f + (bend * std::sqrt(swell / std::max(flat, 1.0e-9f)) - 1.0f) / (0.8f * d), 0.0f, 1.0f);
}

RigEngine::Place RigEngine::placeOf(const Head& h, float along, float widthLaw, const PathGlide& path) const
{
    // How far along its path: Across, Through and Overhead go from wall to wall the way a pass crosses the stereo
    // field (quickly through the middle, slowing towards the walls, and only as far as Width lets it); Around goes
    // round at an even speed, once at the Width the buses open with. Width 0 leaves every path at its half-way point.
    static const float fullCircle = passWidthLaw(c_orbitFullWidth);
    const float crossing = 0.5f + 0.5f * passAzimuth(along, 1.0f, widthLaw);
    const float circling = 0.5f + (along - 0.5f) * widthLaw / fullCircle;
    // How high: an Overhead pass flies its arch (a little lower, pass by pass, as Height Varies allows); the others
    // fly at ear level, and up to as high as that arch as Height Varies is turned up.
    const float archHigh = room::c_archTop * (1.0f - c_archVaries * m_variesNow * (1.0f - h.lift));
    const float flatHigh = room::c_archTop * m_variesNow * h.lift;
    auto on = [&](int which)
    {
        const auto p = static_cast<room::Path>(juce::jlimit(0, room::c_numPaths - 1, which));
        return room::flight(p, p == room::Path::Around ? circling : crossing, h.closeness, h.dir, h.side,
                            p == room::Path::Overhead ? archHigh : flatHigh);
    };
    Place place;
    const room::Flight to = on(path.to);
    place.position = to.position;
    place.distance = to.distance;
    if (path.mix < 1.0f)
    {
        // The path is changing: the pass glides from where it would be on the old one.
        const room::Flight from = on(path.from);
        const float t = path.blend();
        place.position.x = from.position.x + (to.position.x - from.position.x) * t;
        place.position.y = from.position.y + (to.position.y - from.position.y) * t;
        place.position.z = from.position.z + (to.position.z - from.position.z) * t;
        place.distance = from.distance + (to.distance - from.distance) * t;
    }
    place.orbit = path.orbit();
    return place;
}

void RigEngine::roomGains(const room::Position& position, float distance, room::Gains& gains) const
{
    // The nearer the listener an object is, the more of the room it fills: right beside the head it is in every
    // speaker by c_nearSpread. (It also keeps a close pass from stepping from one row of speakers to the next.)
    const float near = sj::clamp(1.0f - distance, 0.0f, 1.0f);
    room::PanSettings settings;
    settings.spread = m_spreadNow + (1.0f - m_spreadNow) * c_nearSpread * near * near;
    settings.centre = m_centreNow;
    settings.height = m_heightNow;
    room::pan(m_width, position, settings, gains);
    for (auto& g : gains)
        g *= c_root2; // a lane is as loud as left and right each carry a centred pass in stereo
}

RigEngine::PassPoint RigEngine::roomPoint(Head& h, float u, float widthLaw, float orbit) const
{
    PassPoint pt = passPoint(h, u, widthLaw);
    if (orbit > 0.0f)
    {
        // Around: the pass keeps its distance, so the delay stays put (no bend) and so does the nearness. While
        // the path glides to or from it the delay moves there, and the read's band limit goes by how fast.
        const double still = static_cast<double>(c_baseDelay) + static_cast<double>(h.closest);
        pt.base += static_cast<double>(orbit) * (still - pt.base);
        pt.nearness += orbit * (h.orbitLoud - pt.nearness); // (Low End, Swirl and Filter go by the nearness its level is held at)
        pt.loud += orbit * (h.orbitLoud - pt.loud);
        const double speed = orbit >= 1.0f ? 1.0 : 1.0 - (pt.base - h.lastBase) * m_sampleRate;
        pt.stretch = sj::SincKernel::stretchFor(sj::clamp(static_cast<float>(speed), 0.0f, 4.0f));
    }
    h.lastBase = pt.base;
    return pt;
}

void RigEngine::roomAir(Head& h, float doppler, float orbit, float fs, bool sides, float shape)
{
    // As passAir, without the head shadow (the listener's own head does that in a room). A pass that goes Around
    // is one brightness all the way round (see orbitLoudness); while the path glides, in between (in octaves).
    const float s = static_cast<float>(h.time) - h.lenIn;
    const float x = h.machRef * s;
    const float dist = std::sqrt(x * x + h.closest * h.closest);
    const float near = std::pow(sj::clamp(0.006f / dist, 0.0f, 1.0f), 0.6f);
    float cutoff = sj::clamp(19000.0f * sj::lerp(1.0f, near * (0.3f + 0.7f * std::sin(sj::c_pi * h.progress())), doppler), 600.0f, 20000.0f);
    if (orbit > 0.0f)
    {
        orbitLoudness(h, doppler, shape); // (roomPoint reads the level it holds)
        const float held = sj::clamp(19000.0f * sj::lerp(1.0f, h.orbitAir, doppler), 600.0f, 20000.0f);
        cutoff *= std::pow(held / cutoff, orbit);
    }
    h.airL.set(cutoff, fs);
    if (sides)
        h.airSL.set(cutoff, fs);
}

bool RigEngine::throwGains(const std::array<Head, c_throws>& heads, float widthLaw, room::Gains& gains, room::Position* at, float* distance) const
{
    room::Gains power {};
    float total = 0.0f;
    for (size_t v = 0; v < m_throws.size(); ++v)
    {
        if (! m_throws[v].active)
            continue;
        const float u = heads[v].progress();
        const Place place = placeOf(heads[v], pathProgress(u, 1.0f), widthLaw, m_buses[0].path); // a throw flies bus A's path on every bus
        if (at != nullptr)
            at[v] = place.position;
        if (distance != nullptr)
            distance[v] = place.distance;
        room::Gains g;
        roomGains(place.position, place.distance, g);
        const float w = passWindow(u, 1.0f) * m_throws[v].level;
        const float p = w * w + 1.0e-9f;
        for (size_t ch = 0; ch < g.size(); ++ch)
            power[ch] += p * g[ch] * g[ch];
        total += p;
    }
    if (total <= 0.0f)
        return false;
    for (size_t ch = 0; ch < gains.size(); ++ch)
        gains[ch] = std::sqrt(power[ch] / total);
    return true;
}

void RigEngine::addToRoom(const float* x, const room::Gains& from, const room::Gains& to, int frames)
{
    const int channels = m_liveChannels;
    const float step = 1.0f / static_cast<float>(frames);
    for (int ch = 0; ch < channels; ++ch)
    {
        const float g0 = from[static_cast<size_t>(ch)], g1 = to[static_cast<size_t>(ch)];
        if (g0 == 0.0f && g1 == 0.0f)
            continue;
        auto& mix = m_mix[static_cast<size_t>(ch)];
        const float slope = (g1 - g0) * step;
        for (int i = 0; i < frames; ++i)
            mix[static_cast<size_t>(i)] += x[i] * (g0 + slope * static_cast<float>(i + 1));
    }
}

void RigEngine::stepWidth(int frames, float& fade0, float& fade1)
{
    const float step = static_cast<float>(frames) / (c_widthFadeSeconds * m_fs);
    fade0 = m_widthFade;
    if (m_room)
    {
        // The speakers of a wider room that has just been left have had their time to fade out.
        m_liveHold = std::max(0, m_liveHold - frames);
        if (const int channels = room::layout(m_width).numChannels; m_liveHold == 0 && m_liveChannels > channels)
        {
            for (int ch = channels; ch < m_liveChannels; ++ch)
                m_dc[static_cast<size_t>(ch)].reset(); // clean for when the speaker comes back
            m_liveChannels = channels;
        }
    }
    if (m_widthWanted != m_width && isRoom(m_widthWanted) == m_room)
    {
        // A room to a room (or mono to stereo): every speaker's gain glides to what it is in the new width
        // (see mixRoom), and those the new width does not have glide to nothing.
        m_width = m_widthWanted;
        if (m_room)
        {
            m_liveChannels = std::max(m_liveChannels, room::layout(m_width).numChannels);
            m_liveHold = static_cast<int>(c_leavingSeconds * m_fs);
        }
    }
    if (m_widthWanted != m_width)
    {
        // Between stereo and a room: out, swap at silence, and back in.
        if (m_widthFade <= 0.0f)
        {
            m_width = m_widthWanted;
            m_room = isRoom(m_width);
            for (auto& bus : m_buses)
            {
                bus.resetThird();
                bus.lanes = 2;
                bus.placed = false;
            }
            m_stow.placed = false;
            m_hitsPlaced = false;
            m_roomSnap = true;
            m_spaceFoldSet = false;
            m_liveChannels = room::layout(m_width).numChannels;
            m_liveHold = 0;
            std::fill(m_subStem.begin(), m_subStem.end(), 0.0f);
            // The reverbs start empty in the new width (the output is silent here): the other one's tail would
            // otherwise wait to come back the next time the width changes.
            m_reverb.reset();
            m_roomReverb.reset();
            m_spaceLean.fill(0.0f);
            m_roomTapsSnap = true;
            m_lfeLow.reset();
            for (auto& f : m_lfePhase)
                f.reset();
            m_lfeSnap = true;
            m_lfeMatchSnap = true;
            m_lfeChannel = room::layout(m_width).lfe;
            m_widthFade = std::min(1.0f, step);
        }
        else
            m_widthFade = std::max(0.0f, m_widthFade - step);
    }
    else
        m_widthFade = std::min(1.0f, m_widthFade + step);
    fade1 = m_widthFade;
}

void RigEngine::stepRoom(int frames)
{
    const float c = m_roomSnap ? 1.0f : glide(frames, c_roomSeconds, m_fs);
    m_spreadNow += (sj::clamp(m_params.roomSpread, 0.0f, 1.0f) - m_spreadNow) * c;
    m_heightNow += (sj::clamp(m_params.roomHeight, 0.0f, 1.0f) - m_heightNow) * c;
    m_variesNow += (sj::clamp(m_params.roomHeightVaries, 0.0f, 1.0f) - m_variesNow) * c;
    m_centreNow += ((m_params.roomCentre ? 1.0f : 0.0f) - m_centreNow) * c;
}

RigEngine::HitView RigEngine::getHitView() const
{
    HitView v;
    v.x = m_hitView[0].load(std::memory_order_relaxed);
    v.y = m_hitView[1].load(std::memory_order_relaxed);
    v.z = m_hitView[2].load(std::memory_order_relaxed);
    v.spread = m_hitView[3].load(std::memory_order_relaxed);
    v.apart = m_hitView[4].load(std::memory_order_relaxed);
    v.level = m_hitView[5].load(std::memory_order_relaxed);
    return v;
}

room::Position RigEngine::randomHitPlace() const
{
    // Round the walls, where the speakers stand: a place between two speakers is panned between them.
    const float a = sj::c_twoPi * m_hitTurn;
    const float x = std::sin(a), y = std::cos(a);
    if (! m_room)
        return { c_hitRandomPan * x, 1.0f, 0.0f };
    const float reach = std::max(std::abs(x), std::abs(y));
    // Half the hits stay in the lower quarter of the way up: the speakers overhead are for some of them.
    return { x / reach, y / reach, room::c_archTop * m_hitHigh * m_hitHigh };
}

void RigEngine::placeHits(int frames)
{
    const bool snap = ! m_hitsPlaced;
    // Whether a bus's passes are in flight: it sounds, and the rig plays. A throw is, while it is active.
    auto flying = [this](int b) { return m_releaseGain > 0.0f && m_buses[static_cast<size_t>(b)].env >= 0.002f; };

    // The pass the hits go with. It has gone when its head has started the next pass (or the throw is over).
    auto& f = m_hitFollow;
    if (f.kind == HitFollow::Pass)
    {
        const float age = m_buses[static_cast<size_t>(f.bus)].heads[static_cast<size_t>(f.index)].age;
        if (age < f.age)
            f.kind = HitFollow::None;
        f.age = age;
    }
    else if (f.kind == HitFollow::Throw)
    {
        const float age = m_buses[0].throwHeads[static_cast<size_t>(f.index)].age;
        if (! m_throws[static_cast<size_t>(f.index)].active || age < f.age)
            f.kind = HitFollow::None;
        f.age = age;
    }
    // Not aimed: the loudest pass in flight as the hit starts, a throw first; and the loudest again when that one
    // has gone while the gate is still open.
    const bool cued = m_params.hitAim || m_params.hitSync;
    const bool gone = f.kind == HitFollow::None && ! cued && m_gate >= 0.5f;
    if ((m_hitStarted && m_hitWantsLoudest) || gone)
    {
        float best = 0.0f;
        HitFollow pick;
        for (size_t v = 0; v < m_throws.size(); ++v)
            if (const float w = m_buses[0].throwHeads[v].view; m_throws[v].active && w > best)
            {
                best = w;
                pick.kind = HitFollow::Throw;
                pick.bus = 0;
                pick.index = static_cast<int>(v);
                pick.age = m_buses[0].throwHeads[v].age;
            }
        if (pick.kind == HitFollow::None)
            for (int b = 0; b < c_maxBuses; ++b)
                for (size_t k = 0; k < 2 && flying(b); ++k)
                {
                    const auto& h = m_buses[static_cast<size_t>(b)].heads[k];
                    if (const float w = h.view * m_buses[static_cast<size_t>(b)].env; w > best)
                    {
                        best = w;
                        pick.kind = HitFollow::Pass;
                        pick.bus = b;
                        pick.index = static_cast<int>(k);
                        pick.age = h.age;
                    }
                }
        f = pick;
    }
    m_hitWantsLoudest = false;

    const int position = juce::jlimit(0, static_cast<int>(c_hitPositions) - 1, m_params.hitPosition);
    if (position == HitRandom && (m_hitStarted || snap))
    {
        m_hitTurn = m_hitRng.unipolar();
        m_hitHigh = m_hitRng.unipolar();
    }
    const room::Position front {};
    if (position == HitRandom)
    {
        m_hitsTo = randomHitPlace();
        m_hitsDistanceTo = 1.0f;
    }
    else if (position == HitFront || m_width == room::Width::Mono)
    {
        m_hitsTo = front;
        m_hitsDistanceTo = 1.0f;
    }
    else if (f.kind != HitFollow::None)
    {
        // With the Pass: where its pass is now. (A room: as the bus placed it this chunk. Stereo: across the field.)
        const bool thrown = f.kind == HitFollow::Throw;
        const auto& bus = m_buses[static_cast<size_t>(f.bus)];
        const size_t slot = thrown ? 2 + static_cast<size_t>(f.index) : static_cast<size_t>(f.index);
        if (m_room)
        {
            m_hitsTo = bus.at[slot];
            m_hitsDistanceTo = bus.atDistance[slot];
        }
        else
        {
            const auto& h = thrown ? bus.throwHeads[static_cast<size_t>(f.index)] : bus.heads[static_cast<size_t>(f.index)];
            m_hitsTo = { passAzimuth(h.age, h.dir, thrown ? m_throwWidthLaw : bus.lawNow), 1.0f, 0.0f };
            m_hitsDistanceTo = 1.0f;
        }
    }
    else if (m_hitStarted)
    {
        // A hit with no pass to go with: the front. (One whose pass has gone stays where the pass left it.)
        m_hitsTo = front;
        m_hitsDistanceTo = 1.0f;
    }

    const float move = snap ? 1.0f : glide(frames, c_hitsMoveSeconds, m_fs);
    m_hitsAt.x += (m_hitsTo.x - m_hitsAt.x) * move;
    m_hitsAt.y += (m_hitsTo.y - m_hitsAt.y) * move;
    m_hitsAt.z += (m_hitsTo.z - m_hitsAt.z) * move;
    m_hitsDistance += (m_hitsDistanceTo - m_hitsDistance) * move;
    m_hitSpreadNow += (sj::clamp(m_params.hitSpread, 0.0f, 1.0f) - m_hitSpreadNow) * (snap ? 1.0f : glide(frames, c_roomSeconds, m_fs));
    hitGains(m_hitsAt, m_hitsDistance, m_hitSpreadNow, m_hitsGainTo, m_hitsSideGainTo);
}

void RigEngine::hitGains(const room::Position& at, float distance, float spread, room::Gains& mid, room::Gains& side) const
{
    mid.fill(0.0f);
    side.fill(0.0f);
    if (m_width == room::Width::Mono)
    {
        // Mono: the hits rest in the middle, as their mid alone (the stereo engine's two channels are folded to one).
        mid[0] = mid[1] = 1.0f;
        return;
    }
    const room::Width width = m_room ? m_width : room::Width::Stereo;
    const auto& layout = room::layout(width);
    const float s = sj::clamp(spread, 0.0f, 1.0f);

    // The two objects as points: either side of the place, and bleeding as any object near the listener does.
    const float apart = c_hitApart + (1.0f - c_hitApart) * s;
    const float near = sj::clamp(1.0f - distance, 0.0f, 1.0f);
    room::PanSettings settings;
    settings.spread = c_nearSpread * near * near;
    settings.centre = m_centreNow;
    settings.height = m_heightNow;
    room::Gains left, right;
    room::pan(width, { std::max(-1.0f, at.x - apart), at.y, at.z }, settings, left);
    room::pan(width, { std::min(1.0f, at.x + apart), at.y, at.z }, settings, right);

    // Shared out: the left object over the speakers on the left, the right one over those on the right, each
    // speaker alike (a centre speaker half to each, as far as Centre Speaker lets it; the speakers overhead as
    // far as Height lets them; never the LFE).
    room::Gains evenLeft {}, evenRight {};
    float sumLeft = 0.0f, sumRight = 0.0f;
    const float alike = 2.0f * m_hitMidShare - 1.0f;
    for (int ch = 0; ch < layout.numChannels; ++ch)
    {
        const auto& speaker = room::info(layout.speakers[static_cast<size_t>(ch)]);
        if (speaker.role == room::Role::Lfe)
            continue;
        float weight = speaker.role == room::Role::Height ? m_heightNow : 1.0f;
        if (ch == layout.centre)
            weight *= m_centreNow;
        // (The centre speaker has both objects: a sound that is the same in both adds there as one, so each brings
        // a quarter of a speaker's share; two unrelated sides bring a half each.)
        const float toLeft = speaker.x < 0.0f ? 1.0f : (speaker.x > 0.0f ? 0.0f : 0.5f / (1.0f + sj::clamp(alike, 0.0f, 1.0f)));
        const float toRight = speaker.x > 0.0f ? 1.0f : (speaker.x < 0.0f ? 0.0f : toLeft);
        evenLeft[static_cast<size_t>(ch)] = weight * toLeft;
        evenRight[static_cast<size_t>(ch)] = weight * toRight;
        sumLeft += weight * toLeft;
        sumRight += weight * toRight;
    }
    for (int ch = 0; ch < layout.numChannels; ++ch)
    {
        const auto c = static_cast<size_t>(ch);
        const float l = std::sqrt((1.0f - s) * left[c] * left[c] + s * evenLeft[c] / std::max(sumLeft, 1.0e-9f));
        const float r = std::sqrt((1.0f - s) * right[c] * right[c] + s * evenRight[c] / std::max(sumRight, 1.0e-9f));
        mid[c] = l + r;
        side[c] = l - r;
    }
}

void RigEngine::mixHits(int frames)
{
    const bool snap = ! m_hitsPlaced;
    const bool sides = m_hitsSideHold > 0;
    const int channels = m_room ? m_liveChannels : 2;
    // The lanes' gains this chunk: towards their targets, never faster than c_gainSeconds from nothing to full
    // (a hit that starts somewhere else, a change of Hit Position or of width). The side lane's follow even while
    // it is silent, so the next stereo hit starts where it belongs.
    room::Gains midFrom = m_hitsGain, sideFrom = m_hitsSideGain;
    if (snap)
    {
        midFrom = m_hitsGain = m_hitsGainTo;
        sideFrom = m_hitsSideGain = m_hitsSideGainTo;
    }
    else
    {
        const float mostStep = static_cast<float>(frames) / (c_gainSeconds * m_fs);
        for (size_t ch = 0; ch < m_hitsGain.size(); ++ch)
        {
            m_hitsGain[ch] += sj::clamp(m_hitsGainTo[ch] - m_hitsGain[ch], -mostStep, mostStep);
            m_hitsSideGain[ch] += sj::clamp(m_hitsSideGainTo[ch] - m_hitsSideGain[ch], -mostStep, mostStep);
        }
    }
    // The two objects together, at the power the hit has as a point (its mid alone, in the middle: twice the mid's
    // power). With gains l and r on a speaker the mid's is l + r and the side's l - r, so over the speakers the
    // two have 2 (mid power + side power) + 2 shared (mid power - side power), where shared is the sum of l r:
    // how far the two are in the same speakers. (At Mono the lanes are the mid alone at 1: nothing to level.)
    float level = 1.0f;
    if (m_width != room::Width::Mono)
    {
        float shared = 0.0f;
        for (int ch = 0; ch < channels; ++ch)
            shared += 0.25f * (m_hitsGain[static_cast<size_t>(ch)] * m_hitsGain[static_cast<size_t>(ch)]
                               - m_hitsSideGain[static_cast<size_t>(ch)] * m_hitsSideGain[static_cast<size_t>(ch)]);
        const float mid = m_hitMidShare, side = 1.0f - m_hitMidShare; // (their share of the two: see renderHits)
        level = std::sqrt(mid / std::max(1.0e-9f, 1.0f + sj::clamp(shared, 0.0f, 1.0f) * (mid - side)));
    }
    // (A hit starting out of the quiet: the level it needs from its first sample; the step is in the quiet before it.)
    const float levelFrom = snap || m_hitOnset ? level : m_hitLevel;
    m_hitLevel = level;
    m_hitsPlaced = true;

    if (m_room)
    {
        room::Gains from, to;
        for (size_t ch = 0; ch < from.size(); ++ch)
        {
            from[ch] = midFrom[ch] * levelFrom;
            to[ch] = m_hitsGain[ch] * level;
        }
        addToRoom(m_hitsMid.data(), from, to, frames);
        if (sides)
        {
            for (size_t ch = 0; ch < from.size(); ++ch)
            {
                from[ch] = sideFrom[ch] * levelFrom;
                to[ch] = m_hitsSideGain[ch] * level;
            }
            addToRoom(m_hitsSide.data(), from, to, frames);
        }
        return;
    }
    const float step = 1.0f / static_cast<float>(frames);
    for (int i = 0; i < frames; ++i)
    {
        const auto ii = static_cast<size_t>(i);
        const float t = static_cast<float>(i + 1) * step;
        const float g = levelFrom + (level - levelFrom) * t;
        float l = m_hitsMid[ii] * (midFrom[0] + (m_hitsGain[0] - midFrom[0]) * t);
        float r = m_hitsMid[ii] * (midFrom[1] + (m_hitsGain[1] - midFrom[1]) * t);
        if (sides)
        {
            l += m_hitsSide[ii] * (sideFrom[0] + (m_hitsSideGain[0] - sideFrom[0]) * t);
            r += m_hitsSide[ii] * (sideFrom[1] + (m_hitsSideGain[1] - sideFrom[1]) * t);
        }
        m_hitsOutL[ii] = l * g;
        m_hitsOutR[ii] = r * g;
    }
}

void RigEngine::mixRoom(int frames, bool stowSounds, bool stowDry)
{
    const auto& layout = room::layout(m_width);
    const int channels = layout.numChannels;
    for (int ch = 0; ch < m_liveChannels; ++ch)
        std::fill(m_mix[static_cast<size_t>(ch)].begin(), m_mix[static_cast<size_t>(ch)].begin() + frames, 0.0f);

    // One lane to its speakers: its gains are ramped across the chunk from where the last chunk left them towards
    // `target`. A pass moves them slowly; a change of width, or a pass starting again at the far wall (while it is
    // silent), would step them, so a gain never moves faster than c_gainSeconds from nothing to full.
    const float mostStep = static_cast<float>(frames) / (c_gainSeconds * m_fs);
    auto lane = [&](const float* x, room::Gains& now, const room::Gains& target, bool snap)
    {
        room::Gains next = target;
        if (snap)
            now = target;
        else
            for (size_t ch = 0; ch < next.size(); ++ch)
                next[ch] = now[ch] + sj::clamp(target[ch] - now[ch], -mostStep, mostStep);
        addToRoom(x, now, next, frames);
        now = next;
    };

    // The hits: their lane, and a stereo hit's sides (see hitGains).
    {
        const bool sides = m_hitsSideHold > 0;
        mixHits(frames);
        for (int i = 0; i < frames; ++i)
        {
            const float h = m_hitsMid[static_cast<size_t>(i)], hs = sides ? m_hitsSide[static_cast<size_t>(i)] : 0.0f;
            m_scopes[SectionHits].push(h);
            m_sectionSquares[SectionHits] += static_cast<double>(h) * h + static_cast<double>(hs) * hs;
            m_roomMono[static_cast<size_t>(i)] = h;
        }
    }

    // The sides of a stereo bed: they stay a pair, plus on the left and minus on the right, as loud in L and R as
    // in stereo. Spread shares their power out over every left and every right speaker of the floor.
    room::Gains sidesTo {};
    {
        int perSide = 0;
        for (int ch = 0; ch < channels; ++ch)
            if (const auto& s = room::info(layout.speakers[static_cast<size_t>(ch)]); s.role == room::Role::Main && s.x < 0.0f)
                ++perSide;
        const int left = layout.indexOf(room::Speaker::L), right = layout.indexOf(room::Speaker::R);
        for (int ch = 0; ch < channels; ++ch)
        {
            const auto& s = room::info(layout.speakers[static_cast<size_t>(ch)]);
            if (s.role != room::Role::Main || s.x == 0.0f)
                continue;
            const float power = m_spreadNow / static_cast<float>(std::max(1, perSide)) + ((ch == left || ch == right) ? 1.0f - m_spreadNow : 0.0f);
            sidesTo[static_cast<size_t>(ch)] = (s.x < 0.0f ? 1.0f : -1.0f) * std::sqrt(power);
        }
    }
    // Their gains this chunk, the same for every bus that has sides (a bus with four lanes may have them for its
    // throws alone: its sides lane is silent then, and it must not stand in for the bus whose bed is playing).
    room::Gains sidesFrom = m_sidesGain, sidesNext = sidesTo;
    if (m_roomSnap)
        sidesFrom = sidesTo;
    else
        for (size_t ch = 0; ch < sidesNext.size(); ++ch)
            sidesNext[ch] = sidesFrom[ch] + sj::clamp(sidesTo[ch] - sidesFrom[ch], -mostStep, mostStep);
    bool sidesHeard = false;

    for (size_t b = 0; b < m_buses.size(); ++b)
    {
        auto& bus = m_buses[b];
        const bool snap = ! bus.placed || m_roomSnap;
        lane(bus.l.data(), bus.gain[0], bus.gainTo[0], snap);
        lane(bus.r.data(), bus.gain[1], bus.gainTo[1], snap);
        const bool four = bus.lanes == 4;
        if (four)
            lane(bus.t.data(), bus.gain[2], bus.gainTo[2], snap);
        else
            bus.gain[2] = bus.gainTo[2]; // silent: the next throw starts where it is
        if (four && bus.sidesOn)
        {
            addToRoom(bus.sideL.data(), sidesFrom, sidesNext, frames);
            sidesHeard = true;
        }
        bus.placed = true;
        for (int i = 0; i < frames; ++i)
        {
            const auto ii = static_cast<size_t>(i);
            const float l = bus.l[ii], r = bus.r[ii], t = four ? bus.t[ii] : 0.0f, side = four ? bus.sideL[ii] : 0.0f;
            const float mono = l + r + t;
            m_scopes[b].push(mono);
            m_sectionSquares[b] += static_cast<double>(l) * l + static_cast<double>(r) * r + static_cast<double>(t) * t + static_cast<double>(side) * side;
            m_roomMono[ii] += mono;
        }
    }
    m_sidesGain = sidesHeard ? sidesNext : sidesTo; // silent: they start from where they belong

    // Stowaway. Sub and Dry: equal in L and R (Dry with its sides), exactly as placeStow made them for stereo.
    // Wake: one lane at its place. Fly: its two passes and its throws, three lanes.
    auto& s = m_stow;
    const int mode = s.now.mode;
    if (stowSounds && (mode == StowSub || mode == StowDry))
    {
        for (int i = 0; i < frames; ++i)
        {
            const auto ii = static_cast<size_t>(i);
            const float l = s.l[ii], r = s.r[ii];
            if (! stowDry) // (Dry joins after Space: see process())
            {
                m_mix[0][ii] += l;
                m_mix[1][ii] += r;
                m_roomMono[ii] += 0.5f * (l + r);
            }
            m_sectionSquares[SectionStow] += 0.5 * (static_cast<double>(l) * l + static_cast<double>(r) * r);
            m_scopes[SectionStow].push(0.5f * (l + r));
        }
    }
    else if (stowSounds)
    {
        const bool fly = mode == StowFly;
        const bool snap = ! s.placed || m_roomSnap;
        if (fly)
        {
            lane(s.l.data(), s.gain[0], s.gainTo[0], snap);
            lane(s.r.data(), s.gain[1], s.gainTo[1], snap);
            lane(s.mono.data(), s.gain[2], s.gainTo[2], snap);
        }
        else
            lane(s.mono.data(), s.gain[0], s.gainTo[0], snap);
        s.placed = true;
        for (int i = 0; i < frames; ++i)
        {
            const auto ii = static_cast<size_t>(i);
            const float l = fly ? s.l[ii] : 0.0f, r = fly ? s.r[ii] : 0.0f, m = s.mono[ii];
            m_sectionSquares[SectionStow] += static_cast<double>(l) * l + static_cast<double>(r) * r + static_cast<double>(m) * m;
            m_scopes[SectionStow].push(l + r + m);
            m_roomMono[ii] += l + r + m;
        }
    }
    else
        for (int i = 0; i < frames; ++i)
            m_scopes[SectionStow].push(0.0f);
    m_sectionSamples += frames;
    m_roomSnap = false;
}
