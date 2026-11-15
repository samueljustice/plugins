#include "RigEngine.h"
#include "Parameters.h"

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
        bus->lowA.set(130.0f, 0.7071f, m_fs);
        bus->lowB.set(130.0f, 0.7071f, m_fs);
        bus->lowAdd.prepare(m_fs);
        bus->shelfL.set(110.0f, 0.7071f, m_fs);
        bus->shelfR.set(110.0f, 0.7071f, m_fs);
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
    m_dcL.set(8.0f, m_fs);
    m_dcR.set(8.0f, m_fs);

    m_rng.seed(m_seed);
    m_stow.fly.rng.seed(m_seed ^ c_flySeed);
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
    for (auto& v : m_throws) v = ThrowVoice {};
    for (auto& q : m_throwQueue) q = QueuedThrow {};
    m_throwPeakView.store(-1.0f, std::memory_order_relaxed);
    m_restartPending = false;
    m_coverageFor.fill(nullptr);
    m_ridePhase.fill(0.0);
    m_rideDepth.fill(0.0f);
    for (size_t t = 0; t < m_ridePhase.size(); ++t)
        m_ridePhase[t] = 0.17 * static_cast<double>(t); // start the rides apart

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
        bus.flL.reset();
        bus.flR.reset();
        bus.env = 0.0f;
        bus.duck = 0.0f;
        bus.peakRef = 1.0e-3f;
        bus.swirlOn = false;
        bus.lowAdd.armed = false;
        bus.passShape = m_params.buses[(m_params.linkBuses && i > 0) ? 0 : i].passMode == 1 ? 1.0f : 0.0f;
        bus.prox = 1.0f;
        // The second pass starts half a pass from the first (at its closest point), and stays there.
        startPass(bus.heads[0], m_params.buses[i]);
        startPass(bus.heads[1], m_params.buses[i], bus.heads[0].lenIn);
        bus.heads[1].time = static_cast<double>(bus.heads[1].lenIn);
        // Linked: B starts on A's Doppler and Width and on A's passes (A was set up just before it).
        auto snapped = m_params.buses[i];
        if (m_params.linkBuses && i > 0)
        {
            snapped.doppler = m_params.buses[0].doppler;
            snapped.width = m_params.buses[0].width;
            for (size_t k = 0; k < bus.heads.size(); ++k)
                followPass(bus.heads[k], m_buses[0].heads[k], snapped.bend, true);
        }
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
        s.throwKey = 0.0f;
        s.byHits.amount = 0.0f;
        s.byHits.lp[0] = s.byHits.lp[1] = 0.0f;
        s.byHits.band[0].reset();
        s.byHits.band[1].reset();
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
    m_spaceOn = false;
    m_echoDriftL = 0.0f; m_echoDriftR = 0.5f; // the same start every time, so a bounce repeats
    m_spaceDry = 1.0f; m_spaceWet = 0.0f;
    m_dcL.reset(); m_dcR.reset();
    m_gate = m_hold = 0.0f;
    m_peakRef = 1.0e-3f;
    m_slowEnv = 0.0f;
    m_talker = 0;
    m_talkerHold = 0.0f;
    for (auto& bus : m_buses)
        for (auto* d : { &bus.byBus, &bus.byHits })
        {
            d->amount = 0.0f;
            d->lp[0] = d->lp[1] = 0.0f;
            d->band[0].reset();
            d->band[1].reset();
        }
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
        m_makeup = m_makeupTarget = rideMakeup(m_makeupDepth, m_makeupChaos);
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
        m_followMix = m_params.followMaster && master >= 0 && m_cargo->getLive(master) != nullptr ? 1.0f : 0.0f;
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
    m_stow.fly.rng.seed(m_seed ^ c_flySeed);
    for (int t = 0; t < c_tracks; ++t)
        m_rideDrift[static_cast<size_t>(t)].prepare(m_fs, 0.3f + 0.11f * static_cast<float>(t), rideSeed(t));
    const auto waitingThrows = m_throwQueue; // a throw asked for with the restart belongs to the new run
    reset();
    m_throwQueue = waitingThrows;
    for (auto& bus : m_buses)
    {
        for (auto* f : { &bus.lowA, &bus.lowB, &bus.shelfL, &bus.shelfR, &bus.fL, &bus.fR, &bus.keyL, &bus.keyR })
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
    for (auto& f : m_fillTone) f.reset();
    m_detect.env = m_hitsFollower.env = m_mixFollower.env = 0.0f;
    m_average = 0.0f;
    m_hitClock = 0.0;
    m_aimClock = 0.0;
    m_lastAimTarget = -1.0e12;
    m_lastHitTiming = -1;
    m_hitMarks.fill(-1.0);
    m_releaseGain = 0.0f;
    m_reseedPending = false;
}

void RigEngine::throwWhoosh(int delaySamples, float velocity)
{
    for (auto& q : m_throwQueue)
        if (! q.pending)
        {
            q.pending = true;
            q.delay = std::max(0, delaySamples);
            q.velocity = velocity;
            return;
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
        // Length says: half of it is the approach, so the peak is heard half a Throw Length from now.
        auto knobNow = [this](size_t bus, size_t k) { return sj::clamp(m_buses[bus].springs[k].value + m_busModNow[bus][k], 0.0f, 1.0f); };
        BusParams lead;
        lead.doppler = knobNow(0, 0);
        lead.bend = knobNow(0, 2);
        const float seconds = ::Params::throwSeconds(m_params.throwSeconds, m_params.rate);
        auto& first = m_buses[0].throwHeads[voice];
        startPass(first, lead, 0.5f * seconds);
        auto& v = m_throws[voice];
        v = ThrowVoice {};
        if (q.velocity >= 0.0f)
        {
            const float vel = sj::clamp(q.velocity, 0.0f, 1.0f);
            first.closeness = sj::lerp(c_throwSoftCloseness, 1.0f, vel);
            first.closest = 0.004f * std::exp2(5.0f * (1.0f - first.closeness));
            v.level = sj::lerp(c_throwSoftLevel, 1.0f, vel);
        }
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
    h.dir = draw.bipolar() >= 0.0f ? 1.0f : -1.0f;
    h.aimed = false;
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
    if (newPass)
        h.aimed = false;
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
    const float swell = sj::clamp(m_swellSpring.step(seconds), 0.0f, 1.0f);
    {
        // How still the faders are, for the buses (see m_proxBlend), and the ride make-up for the mix. The make-up
        // follows Energy and Auto Ride as they glide, and a held Swell slowly (it lets go quickly).
        const float still = 1.0f - energy * (1.0f - swell);
        m_proxBlend = still * still;
        m_makeupSwell += (swell - m_makeupSwell) * glide(frames, swell > m_makeupSwell ? c_makeupSwellSeconds : c_makeupReleaseSeconds, m_fs);
        const float depth = energy * (1.0f - m_makeupSwell);
        if (std::abs(depth - m_makeupDepth) > 1.0e-4f || std::abs(m_params.chaos - m_makeupChaos) > 1.0e-4f)
        {
            m_makeupDepth = depth;
            m_makeupChaos = m_params.chaos;
            m_makeupTarget = rideMakeup(depth, m_makeupChaos);
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
            for (auto& at : m_hitMarks)
                if (at < 0.0)
                {
                    at = toPeak * m_sampleRate;
                    break;
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

    CargoCue hitCue;
    if (hitSync && m_cargo != nullptr)
    {
        m_hitClock -= frames;
        if (m_hitClock <= 0.0)
        {
            double maxPre = 0.0, maxPost = 0.0;
            hitWindow(maxPre, maxPost);
            m_hitClock += publishCue(hitCue, maxPre, maxPost) * m_sampleRate;
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
            // A throw always earns its hit, and goes first. Its sources stay open until the hits have rung out.
            size_t bestThrow = m_throws.size();
            for (size_t v = 0; v < m_throws.size(); ++v)
            {
                if (! m_throws[v].active || m_throws[v].aimed)
                    continue;
                const auto& h = m_buses[0].throwHeads[v];
                const double toPeak = static_cast<double>(h.lenIn) - h.time;
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
            }
            // The rig's own passes: not while it is stopped (only a throw's hits play then).
            const bool ridePasses = bestThrow == m_throws.size() && m_releaseGain > 0.0f;
            for (auto& bus : m_buses)
                for (auto& h : bus.heads)
                {
                    if (! ridePasses)
                        break;
                    const double toPeak = static_cast<double>(h.lenIn) - h.time;
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
                    }
                }
            if (bestHead != nullptr && bestThrow == m_throws.size())
            {
                bestHead->aimed = true;
                m_lastAimTarget = m_aimClock + best * m_sampleRate;
                publishCue(hitCue, best, maxPost);
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
    m_followMix += ((m_params.followMaster && masterPlays ? 1.0f : 0.0f) - m_followMix) * glide(frames, c_followSeconds, m_fs);
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
        if (m_followMix > 0.0f)
            r = sj::lerp(r, t == master ? 1.0f : followRide, m_followMix);
        float ride = sj::lerp(1.0f, r, energy);
        ride = sj::lerp(ride, 1.0f, swell);
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
        if (onStow && data != nullptr && subRide < 1.0f)
            ride = subRide <= 0.0f ? 1.0f : std::pow(ride, subRide);
        m_rideView[ti].store(ride, std::memory_order_relaxed);

        const float laneGain = m_cargo != nullptr ? m_cargo->laneGain(CargoHold::c_numBuiltIns + t) : 1.0f;
        const float target = data != nullptr ? level * ride * laneGain * m_releaseGain : 0.0f;
        const float throwTarget = data != nullptr ? level * laneGain * m_throwOpen : 0.0f;

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
        const bool wantSide = data != nullptr && data->hasSide() && (bus == static_cast<int>(c_sideBus) || dryStow) && vari;
        // (A track that has just arrived somewhere that takes no sides brings none with it: they went out with the
        // fade it left by, and must not carry on into bus B.)
        const float sideGain0 = ! wantSide && busFade0 <= 0.0f ? 0.0f : m_sideGain[ti];
        const float sideGain1 = wantSide ? std::min(1.0f, sideGain0 + seconds / c_sideFadeSeconds)
                                         : std::max(0.0f, sideGain0 - seconds / c_sideFadeSeconds);
        m_sideGain[ti] = sideGain1;
        float* const sideOut = (sideGain0 > 0.0f || sideGain1 > 0.0f) && ! syncedHit ? m_sideScratch.data() : nullptr;
        if (syncedHit)
        {
            CargoCue cue = hitCue;
            if (cue.active && ! vari) // heard later by the shifter's latency: fire that much earlier
                cue.secondsToPeak = std::max(0.0, cue.secondsToPeak - static_cast<double>(m_shifters[ti].getLatencySamples()) / m_sampleRate);
            m_players[ti].render(data, CargoRole::Impact, skip, rate, chaos, m_rng, 0.15f, cue, m_trackScratch.data(), frames);
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
                throwDest[i] += m_trackScratch[static_cast<size_t>(i)] * (modeGain0 + (modeGain1 - modeGain0) * frac)
                                * throwGain * variGain * (busFade0 + (busFade1 - busFade0) * frac);
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
            const bool morphed = rope[0] == t || rope[1] == t || (rope[1] < 0 && ! onStow);
            const float keep0 = morphed ? 1.0f - morphMixBefore : 1.0f, keep1 = morphed ? 1.0f - morphMix : 1.0f;
            float* sideDest = onStow ? m_stow.sideIn.data() : m_buses[c_sideBus].sideIn.data();
            m_stow.sideFed = m_stow.sideFed || onStow;
            for (int i = 0; i < frames; ++i)
            {
                const float frac = static_cast<float>(i + 1) * invFrames;
                sideDest[i] += m_sideScratch[static_cast<size_t>(i)] * (modeGain0 + (modeGain1 - modeGain0) * frac)
                               * (g0 + (target - g0) * frac) * variGain * (rope0 + (rope1 - rope0) * frac)
                               * (sideGain0 + (sideGain1 - sideGain0) * frac) * (keep0 + (keep1 - keep0) * frac);
            }
        }
        m_gain[ti] = target;
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
        s.leaveMode();
        return;
    }
    const float invFrames = 1.0f / static_cast<float>(frames);
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
    const float law = passWidthLaw(s.now.width);
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
    for (auto& h : f.heads)
        passAir(h, c_flyDoppler, law, fs, false);
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
            if (window > 0.0f)
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
            passAir(h, c_flyDoppler, law, fs, false);
            const float level = m_throws[v].level;
            for (int i = 0; i < frames; ++i)
            {
                const float u = h.progress();
                const float window = passWindow(u, 1.0f);
                if (window > 0.0f)
                {
                    const PassPoint pt = passPoint(h, u, law);
                    const float gain = window * c_flyMakeup * (1.0f + 0.8f * c_flyDoppler * (pt.nearness - 0.35f)) * level;
                    hearPass(h, f.throwLine, pt, static_cast<double>(frames - 1 - i), gain, s.l[static_cast<size_t>(i)], s.r[static_cast<size_t>(i)]);
                }
                h.time += dt;
            }
        }
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
    auto trail = [&](const Head& h, float out, float level, float& bloom)
    {
        const float b = wakeBloom(out) * level;
        bloom = std::max(bloom, b);
        panSum += (b + 1.0e-3f) * h.dir * std::pow(c_wakeLag * sj::clamp(out, 0.0f, 1.0f), 0.6f);
        weight += b + 1.0e-3f;
    };
    for (const auto& h : a.heads)
        if (const float u = h.progress(); u > 0.5f)
            trail(h, 2.0f * u - 1.0f, 1.0f, passBloom);
    for (size_t v = 0; v < m_throws.size(); ++v)
        if (const float u = a.throwHeads[v].progress(); m_throws[v].active && u > 0.5f)
            trail(a.throwHeads[v], (2.0f * u - 1.0f) / c_flyTail, m_throws[v].level, throwBloom);
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
    const double readSpeed = 1.0 - (v * vs / std::max(root, 1.0e-12) - v * v) / k2;
    pt.stretch = sj::SincKernel::stretchFor(readSpeed);
    // How near it is: by the pass's own speed, so Bend changes the pitch and leaves the swell alone.
    const float xRef = h.machRef * static_cast<float>(s);
    const float distRef = std::sqrt(xRef * xRef + h.closest * h.closest);
    pt.nearness = h.closest / std::max(distRef, 1.0e-6f);
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
    float proximity = 0.0f; // of the nearer pass, at the end of this chunk

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
    const auto& kernel = sj::SincKernel::get();
    const double leadTime = bus.heads[0].time;                              // the first head before this chunk,
    const double leadLength = static_cast<double>(bus.heads[0].length);     // for placing the second
    for (size_t k = 0; k < bus.heads.size(); ++k)
    {
        auto& h = bus.heads[k];
        passAir(h, p.doppler, widthLaw, fs, sideHeard);

        double t0 = leadTime, len0 = leadLength; // the first head's clock again, sample by sample (second head only)
        for (int i = 0; i < frames; ++i)
        {
            if (k == 0)
            {
                if (h.time >= static_cast<double>(h.length))
                {
                    // The next pass starts where this one ran out (the fraction of a sample over is kept, so the
                    // second head, at its closest point right now, carries on without a step).
                    const double over = h.time - static_cast<double>(h.length);
                    if (lead != nullptr)
                        followPass(h, (*lead)[k], p.bend, true); // the pass bus A started at this same moment
                    else
                        startPass(h, p);
                    h.time = over;
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
                }
                const double half = 0.5 * len0;
                if (t0 >= half)
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

            const PassPoint pt = passPoint(h, h.progress(), widthLaw);
            const double back = static_cast<double>(frames - 1 - i);
            // The swell: the window, and how near the pass is.
            const float shapeNow = shape0 + shapeStep * static_cast<float>(i + 1);
            const float window = passWindow(pt.u, shapeNow);
            const float nearness = pt.nearness;
            // Fly-By's passes don't overlap, so the bus is quieter between them: a little make-up keeps it as loud
            // as Bed on the same material.
            const float gain = window * (1.0f + (c_flyMakeup - 1.0f) * shapeNow) * (1.0f + 0.8f * p.doppler * (nearness - 0.35f));
            if (i == frames - 1)
                proximity = std::max(proximity, window * (0.1f + 0.9f * std::sqrt(std::min(nearness, 1.0f))));
            hearPass(h, bus.line, pt, back, gain, bus.l[static_cast<size_t>(i)], bus.r[static_cast<size_t>(i)]);
            if (sideHeard)
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
        h.age = h.progress();
        h.view = passWindow(h.age, shape1);
        h.x = h.dir * std::tanh(5.0f * (h.age - 0.5f));
    }

    // Throws: one more pass each, the same on every bus (bus A's Doppler and Width, this bus's Bend, set when it
    // was thrown), always shaped like a Fly-By, reading the tracks at their fader levels from the throw line.
    for (size_t v = 0; v < m_throws.size(); ++v)
    {
        if (! m_throws[v].active)
            continue;
        auto& h = bus.throwHeads[v];
        const float doppler = m_throwDoppler, law = m_throwWidthLaw, level = m_throws[v].level;
        passAir(h, doppler, law, fs, false);
        for (int i = 0; i < frames; ++i)
        {
            const float u = h.progress();
            const float window = passWindow(u, 1.0f);
            if (window > 0.0f)
            {
                const PassPoint pt = passPoint(h, u, law);
                const float nearness = pt.nearness;
                const float gain = window * c_flyMakeup * (1.0f + 0.8f * doppler * (nearness - 0.35f)) * level;
                if (i == frames - 1)
                    proximity = std::max(proximity, window * (0.1f + 0.9f * std::sqrt(std::min(nearness, 1.0f))));
                hearPass(h, bus.throwLine, pt, static_cast<double>(frames - 1 - i), gain, bus.l[static_cast<size_t>(i)], bus.r[static_cast<size_t>(i)]);
            }
            h.time += dt;
        }
        h.age = h.progress();
        h.view = passWindow(h.age, 1.0f) * level;
        h.x = h.dir * std::tanh(5.0f * (h.age - 0.5f));
    }

    // Stereo beds: what goes into the colour chain, to match the side signal to what comes out of it.
    float sidePreSq = 0.0f;
    if (sideHeard)
        for (int i = 0; i < frames; ++i)
            sidePreSq += bus.l[static_cast<size_t>(i)] * bus.l[static_cast<size_t>(i)] + bus.r[static_cast<size_t>(i)] * bus.r[static_cast<size_t>(i)];

    // Level tracking for the low-end lift and the views.
    float chunkPeak = 0.0f;
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
    if (p.low > 1.0e-4f)
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

    // Swirl: phaser, then a flanger whose comb sweeps as the passes come and go.
    if (p.swirl > 1.0e-4f)
    {
        // The knob runs a little faster at the bottom (the first phaser notches arrive sooner), and the result is
        // level-matched so Swirl changes the colour, not the loudness.
        const float swirl = std::pow(p.swirl, 0.7f);
        const float phMix = sj::clamp(swirl * 2.0f, 0.0f, 1.0f) * 0.5f;
        const float flMix = sj::clamp((swirl - 0.25f) / 0.75f, 0.0f, 1.0f) * 0.5f;
        float preSq = 0.0f, postSq = 0.0f;
        for (int i = 0; i < frames; ++i)
            preSq += bus.l[static_cast<size_t>(i)] * bus.l[static_cast<size_t>(i)] + bus.r[static_cast<size_t>(i)] * bus.r[static_cast<size_t>(i)];
        // The level against the recent peak moves in steps (once per chunk); Swirl follows it over ~15 ms, and every
        // setting it leads to (the phaser's all-pass tuning, the flanger's delay, the mixes) is ramped sample by
        // sample from where the last chunk ended, so nothing steps at a chunk edge.
        if (! bus.swirlOn)
            bus.swPeak = peakness;
        bus.swPeak += (peakness - bus.swPeak) * glide(frames, 0.015f, fs);
        const float level = bus.swPeak;
        bus.lfo += (0.12f + 1.2f * level * swirl) * static_cast<float>(frames) / fs;
        if (bus.lfo >= 1.0f) bus.lfo -= 1.0f;
        const float lfo = std::sin(sj::c_twoPi * bus.lfo);
        const float lfoR = std::cos(sj::c_twoPi * bus.lfo);
        auto coefFor = [fs](float fc)
        {
            const float t = std::tan(sj::c_pi * sj::clamp(fc, 20.0f, fs * 0.45f) / fs);
            return (t - 1.0f) / (t + 1.0f);
        };
        const float aL1 = coefFor(260.0f * std::exp2(3.6f * (0.5f + 0.5f * lfo) + 1.2f * level));
        const float aR1 = coefFor(260.0f * std::exp2(3.6f * (0.5f + 0.5f * lfoR) + 1.2f * level));
        const float fb1 = 0.55f * swirl;
        const float far = (1.0f - level) * (1.0f - level);
        const float msL1 = 0.3f + 6.5f * far * (0.85f + 0.15f * lfo);
        const float msR1 = 0.3f + 6.5f * far * (0.85f - 0.15f * lfo);
        if (! bus.swirlOn)
        {
            // Swirl was off: start from here (the mixes still fade in from nothing).
            bus.swirlOn = true;
            bus.swAL = aL1; bus.swAR = aR1;
            bus.swMsL = msL1; bus.swMsR = msR1;
            bus.swFb = fb1;
            bus.swPhMix = bus.swFlMix = 0.0f;
        }
        const float aL0 = bus.swAL, aR0 = bus.swAR, msL0 = bus.swMsL, msR0 = bus.swMsR;
        const float fb0 = bus.swFb, phMix0 = bus.swPhMix, flMix0 = bus.swFlMix;
        const bool flanging = flMix > 1.0e-4f || flMix0 > 1.0e-4f;
        const float msToSamples = 0.001f * fs;
        const float step = 1.0f / static_cast<float>(frames);
        for (int i = 0; i < frames; ++i)
        {
            const float t = static_cast<float>(i + 1) * step;
            const float aL = aL0 + (aL1 - aL0) * t, aR = aR0 + (aR1 - aR0) * t;
            const float fb = fb0 + (fb1 - fb0) * t;
            const float phNow = phMix0 + (phMix - phMix0) * t;
            auto& l = bus.l[static_cast<size_t>(i)];
            auto& r = bus.r[static_cast<size_t>(i)];
            float yl = l + fb * bus.phFbL, yr = r + fb * bus.phFbR;
            for (size_t k = 0; k < bus.phL.size(); ++k)
            {
                yl = bus.phL[k].process(yl, aL);
                yr = bus.phR[k].process(yr, aR);
            }
            bus.phFbL = sj::fastTanh(yl);
            bus.phFbR = sj::fastTanh(yr);
            float ol = l + phNow * (yl - l), orr = r + phNow * (yr - r);
            if (flanging)
            {
                const float flNow = flMix0 + (flMix - flMix0) * t;
                const float flFb = (0.6f / 0.55f) * fb;
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
        }
        bus.swAL = aL1; bus.swAR = aR1;
        bus.swMsL = msL1; bus.swMsR = msR1;
        bus.swFb = fb1;
        bus.swPhMix = phMix;
        bus.swFlMix = flMix;
        // Level match over ~300 ms, within +/-6 dB.
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
        }
    }
    else
    {
        bus.swirlPre = bus.swirlPost = 0.0f;
        bus.swirlGain = 1.0f;
        bus.swirlOn = false;
    }

    // Envelope filter: opens as each pass swells and closes in its tail, by the bus's level against its own recent
    // peak (so it works the same on loud and quiet material). Higher settings blend more of it in, close it further
    // at the peak (from 21 kHz at 0% to about 280 Hz at 100%) and, over the top 40%, add a little resonance.
    const float f = p.filter;
    const float mix = f <= 0.8f ? (1.0f - std::pow(10.0f, -1.875f * f)) / (1.0f - std::pow(10.0f, -1.5f)) : 1.0f;
    if (mix > 1.0e-4f)
    {
        const float cutoff = std::max(170.0f, 170.0f * std::exp2(7.0f * std::sqrt(peakness) * (1.0f - 0.9f * std::sqrt(f))));
        const float resonance = sj::clamp((f - 0.6f) / 0.4f, 0.0f, 1.0f);
        const float q = 0.7f + 1.5f * resonance * resonance;
        bus.fL.set(cutoff, q, fs);
        bus.fR.set(cutoff, q, fs);
        const float trim = 1.0f / (1.0f + 0.35f * (q - 0.7f));
        for (int i = 0; i < frames; ++i)
        {
            auto& l = bus.l[static_cast<size_t>(i)];
            auto& r = bus.r[static_cast<size_t>(i)];
            l += mix * (sj::fastTanh(bus.fL.lowpass(l) * trim) - l);
            r += mix * (sj::fastTanh(bus.fR.lowpass(r) * trim) - r);
        }
    }

    // Filtered by A: a low-pass that closes (18 kHz down to about 280 Hz) as bus A swells.
    const float keyed = p.key * key;
    if (keyed > 1.0e-4f)
    {
        const float cutoff = 18000.0f * std::exp2(-6.0f * keyed);
        bus.keyL.set(cutoff, 0.7071f, fs);
        bus.keyR.set(cutoff, 0.7071f, fs);
        for (int i = 0; i < frames; ++i)
        {
            auto& l = bus.l[static_cast<size_t>(i)];
            auto& r = bus.r[static_cast<size_t>(i)];
            l = bus.keyL.lowpass(l);
            r = bus.keyR.lowpass(r);
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
        }
    }
    else
    {
        bus.drivePre = bus.drivePost = 0.0f;
        bus.driveGain = 1.0f;
    }

    // Stereo beds: the side signal comes back in here, past the colours (they work on the mono sum's passes), as
    // loud against the bus as it went in.
    if (sideHeard)
    {
        float postSq = 0.0f;
        for (int i = 0; i < frames; ++i)
            postSq += bus.l[static_cast<size_t>(i)] * bus.l[static_cast<size_t>(i)] + bus.r[static_cast<size_t>(i)] * bus.r[static_cast<size_t>(i)];
        const float c = 1.0f - std::exp(-static_cast<float>(frames) / (0.3f * fs));
        bus.sidePre += (sidePreSq / static_cast<float>(frames) - bus.sidePre) * c;
        bus.sidePost += (postSq / static_cast<float>(frames) - bus.sidePost) * c;
        const float want = bus.sidePre > 1.0e-10f ? sj::clamp(std::sqrt(bus.sidePost / bus.sidePre), 0.25f, 2.0f) : 1.0f;
        const float g0 = bus.sideMatch;
        bus.sideMatch += (want - bus.sideMatch) * glide(frames, 0.05f, fs);
        for (int i = 0; i < frames; ++i)
        {
            const float g = g0 + (bus.sideMatch - g0) * static_cast<float>(i + 1) / static_cast<float>(frames);
            bus.l[static_cast<size_t>(i)] += g * bus.sideL[static_cast<size_t>(i)];
            bus.r[static_cast<size_t>(i)] += g * bus.sideR[static_cast<size_t>(i)];
        }
    }
    else
    {
        bus.sidePre = bus.sidePost = 0.0f;
        bus.sideMatch = 1.0f;
    }
}

float RigEngine::duckReturnCoef() const
{
    // 0 = the original (the duck follows its key straight away); up to 1 s for the ducked bus to come back.
    const float r = sj::clamp(m_params.duckReturn, 0.0f, 1.0f);
    if (r <= 0.001f)
        return 1.0f;
    return 1.0f - std::exp(-1.0f / (0.001f * (20.0f + 980.0f * r) * m_fs));
}

void RigEngine::duckSample(BusState::Duck& d, float target, float returnCoef, float full, float& l, float& r)
{
    // Down at once, back up over the return time.
    if (returnCoef >= 1.0f || target >= d.amount)
        d.amount = target; // fast return: exactly the original
    else
        d.amount += (target - d.amount) * returnCoef;
    const float amt = sj::clamp(d.amount, 0.0f, 1.0f);
    float* ch[2] = { &l, &r };
    // The band filters always run, so switching Duck mode mid-duck doesn't start from stale filter state.
    float band[2];
    for (int c = 0; c < 2; ++c)
        band[c] = d.band[c].bandpass(*ch[c]);
    switch (m_params.duckMode)
    {
        case 1: // Darken: low-pass the ducked bus (an envelope filter keyed by the other bus), a little quieter
        {
            const float fc = 18000.0f * std::exp2(-5.7f * sj::clamp(amt / full, 0.0f, 1.0f)); // 18 kHz .. ~350 Hz
            const float a = 1.0f - std::exp(-sj::c_twoPi * std::min(fc, 0.45f * m_fs) / m_fs);
            const float wet = sj::clamp(amt * 4.0f, 0.0f, 1.0f), gain = 1.0f - 0.25f * amt;
            for (int c = 0; c < 2; ++c)
            {
                d.lp[c] += a * (*ch[c] - d.lp[c]);
                *ch[c] = (*ch[c] + (d.lp[c] - *ch[c]) * wet) * gain;
            }
            break;
        }
        case 2: // Mids: only the clashing middle (250 Hz .. 4 kHz) goes down; sub and air stay
        {
            const float g = sj::clamp(1.0f - amt, 0.0f, 1.0f); // as deep as Volume, in the middle band only
            for (int c = 0; c < 2; ++c)
            {
                const float x = *ch[c];
                *ch[c] = x - (1.0f - g) * band[c];
                d.lp[c] = x;
            }
            break;
        }
        default: // Volume (original)
            for (int c = 0; c < 2; ++c)
                d.lp[c] = *ch[c]; // Darken's low-pass follows the signal, ready for a switch
            l *= 1.0f - amt;
            r *= 1.0f - amt;
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
    float gateWas = m_gate;
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
        gateWas = m_gate;
        gateMax = std::max(gateMax, m_gate);

        const float h = m_hitsIn[ii] * (floorGain + (1.0f - floorGain) * m_gate) * boost;
        m_hitsL[ii] = h;
        m_hitsR[ii] = h;

        // The gated hits duck both buses, so the hit punches through.
        // The duck follows the gated hits only (a raised Floor keeps the hits under the mix, it doesn't hold the buses down).
        const float gated = floorGain > 0.0f ? m_hitsIn[ii] * m_gate * boost : h;
        const float hitsLevel = sj::clamp(m_hitsFollower.process(gated) * 3.0f, 0.0f, 1.0f);
        hitsMax = std::max(hitsMax, hitsLevel);
        const float duck = duckDepth * hitsLevel;
        for (int b = 0; b < numBuses; ++b)
        {
            auto& bus = m_buses[static_cast<size_t>(b)];
            duckSample(bus.byHits, duck, returnCoef, 1.0f - sj::dbToGain(-12.0f), bus.l[ii], bus.r[ii]);
        }
        if (stowDuck && stowStereo)
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
        stow.byHits.amount = 0.0f;
        stow.byHits.lp[0] = stow.byHits.lp[1] = 0.0f;
        stow.byHits.band[0].reset();
        stow.byHits.band[1].reset();
    }
    // The HIT GATE lamp: the gate, or with AIM (gate held open) the aimed hits themselves, so it flashes on each.
    m_gateView.store(m_params.hitAim ? hitsMax : gateMax, std::memory_order_relaxed);
}

void RigEngine::applyTremolo(PeakTremolo& t, float* l, float* r, int frames, bool selected)
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
        const float x = std::max(std::abs(l[i]), std::abs(r[i]));
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
            l[i] *= g;
            r[i] *= g;
        }
        else
            t.depth = 0.0f;
    }
}

void RigEngine::renderSpace(int frames)
{
    const float s = sj::clamp(m_spaceSpring.step(static_cast<float>(frames) / m_fs) + m_spaceModNow, 0.0f, 1.0f);
    if (s < 1.0e-4f)
    {
        // Space is off. Empty the echoes and the reverb once, so old sound isn't kept to come back when Space is
        // turned up again (their send is silent by now: nothing is cut off audibly).
        if (m_spaceOn)
        {
            m_spaceOn = false;
            m_echoL.reset(); m_echoR.reset();
            m_echoLpL.reset(); m_echoLpR.reset(); m_echoHpL.reset(); m_echoHpR.reset();
            m_reverb.reset();
        }
        return;
    }
    const bool wasOn = m_spaceOn;
    m_spaceOn = true;

    // Ping-pong echo. The two times are not a simple ratio of each other and each drifts by a few milliseconds,
    // so the repeats never settle into a fixed pattern.
    const float fb = 0.25f + 0.4f * s;
    const float wet = 0.35f * s;
    const float driftStep = 2.0f / m_fs;
    for (int i = 0; i < frames; ++i)
    {
        auto& l = m_mixL[static_cast<size_t>(i)];
        auto& r = m_mixR[static_cast<size_t>(i)];
        m_echoDriftL += c_echoDriftHzL * driftStep;
        m_echoDriftR += c_echoDriftHzR * driftStep;
        if (m_echoDriftL >= 1.0f) m_echoDriftL -= 2.0f;
        if (m_echoDriftR >= 1.0f) m_echoDriftR -= 2.0f;
        const float tL = (c_echoSecondsL + c_echoDriftSeconds * 4.0f * m_echoDriftL * (1.0f - std::abs(m_echoDriftL))) * m_fs;
        const float tR = (c_echoSecondsR + c_echoDriftSeconds * 4.0f * m_echoDriftR * (1.0f - std::abs(m_echoDriftR))) * m_fs;
        const float wl = m_echoL.read(static_cast<double>(tL)), wr = m_echoR.read(static_cast<double>(tR));
        m_echoL.push(l + fb * m_echoLpR.lowpass(m_echoHpR.highpass(wr)));
        m_echoR.push(r + fb * m_echoLpL.lowpass(m_echoHpL.highpass(wl)));
        l += wet * wl;
        r += wet * wr;
    }

    // Reverb, after the echoes (so they trail into it). Its tail grows with Space; the dry is eased down a little
    // as the wet comes up, so more Space is more room and not more level into the limiter.
    m_reverb.setDecay(SpaceReverb::decaySecondsFor(s));
    const float dryGain = 1.0f - c_spaceDryTrim * s * s;
    const float wetGain = c_spaceWet * s;
    if (! wasOn)
    {
        m_spaceDry = dryGain;
        m_spaceWet = wetGain;
    }
    m_reverb.process(m_mixL.data(), m_mixR.data(), frames, m_spaceDry, dryGain, m_spaceWet, wetGain);
    m_spaceDry = dryGain;
    m_spaceWet = wetGain;
}

//==============================================================================
void RigEngine::process(const float* inL, const float* inR, float* outL, float* outR, int numSamples)
{
    juce::ignoreUnused(inL, inR); // sample-based: the plug-in has no audio input
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
                duckSample(bus.byBus, duck0 + (bus.duck - duck0) * t, returnCoef, 0.9f, bus.l[ii], bus.r[ii]);
                if (duckKey)
                    duckSample(keyBus.byBus, key0 + (keyBus.duck - key0) * t, returnCoef, 0.9f, keyBus.l[ii], keyBus.r[ii]);
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
        const bool stowSounds = m_stow.sounding;
        const bool stowDry = stowSounds && m_stow.now.mode == StowDry; // it joins after Space (below), not here
        if (stowSounds)
            placeStow(frames);

        // Peak tremolo on the buses and on the hits (each only deepens when it's the selected target). The two buses
        // share the tremolo's timing (so they dip together) while each deepens by its own level.
        {
            const bool buses = m_params.tremTarget == 1;
            const auto lfo = m_trem[1]; // bus A's tremolo before this chunk: bus B starts from the same timing
            applyTremolo(m_trem[1], m_buses[0].l.data(), m_buses[0].r.data(), frames, buses);
            if (numBuses > 1)
            {
                m_trem[2].phase = lfo.phase;
                m_trem[2].from = lfo.from;
                m_trem[2].to = lfo.to;
                m_trem[2].rng = lfo.rng;
                applyTremolo(m_trem[2], m_buses[1].l.data(), m_buses[1].r.data(), frames, buses);
            }
            applyTremolo(m_trem[3], m_hitsL.data(), m_hitsR.data(), frames, m_params.tremTarget == 2);
        }

        for (int i = 0; i < frames; ++i)
        {
            const auto ii = static_cast<size_t>(i);
            float l = m_hitsL[ii], r = m_hitsR[ii];
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
            m_mixL[ii] = l;
            m_mixR[ii] = r;
        }
        // Ride make-up (see rideMakeup): ramped across the chunk. Stowaway in Dry is not in the mix yet (it joins
        // after Space, below), so it takes the same make-up here.
        {
            const float g0 = m_makeup;
            m_makeup += (m_makeupTarget - m_makeup) * glide(frames, 0.05f, m_fs);
            if (g0 < 0.9999f || m_makeupTarget < 0.9999f)
                for (int i = 0; i < frames; ++i)
                {
                    const float g = g0 + (m_makeup - g0) * static_cast<float>(i + 1) / static_cast<float>(frames);
                    m_mixL[static_cast<size_t>(i)] *= g;
                    m_mixR[static_cast<size_t>(i)] *= g;
                    if (stowDry)
                    {
                        m_stow.l[static_cast<size_t>(i)] *= g;
                        m_stow.r[static_cast<size_t>(i)] *= g;
                    }
                }
            else
                m_makeup = 1.0f;
        }
        // Peak tremolo on everything (by the mix's level), before Space so the echoes and reverb carry it.
        applyTremolo(m_trem[0], m_mixL.data(), m_mixR.data(), frames, m_params.tremTarget == 0);
        renderSpace(frames);
        // Stowaway in Dry: past the Everything tremolo and Space, straight to the output stage.
        if (stowDry)
            for (int i = 0; i < frames; ++i)
            {
                m_mixL[static_cast<size_t>(i)] += m_stow.l[static_cast<size_t>(i)];
                m_mixR[static_cast<size_t>(i)] += m_stow.r[static_cast<size_t>(i)];
            }

        for (int i = 0; i < frames; ++i)
        {
            float l = m_dcL.process(m_mixL[static_cast<size_t>(i)]);
            float r = m_dcR.process(m_mixR[static_cast<size_t>(i)]);
            if (! std::isfinite(l) || ! std::isfinite(r))
            {
                l = r = 0.0f;
                reset();
            }
            m_mixFollower.process(0.5f * (l + r));
            outL[offset + i] = l;
            outR[offset + i] = r;
        }
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
            }
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
}
