#include "RigEngine.h"
#include "Parameters.h"

namespace
{
    constexpr float c_lineSeconds = 4.6f;
    constexpr float c_maxPassDelay = 4.0f;
    constexpr float c_earOffset = 0.09f / 343.0f;
    constexpr float c_baseDelay = 0.002f;
    // Every track rides at its own speed (offset speeds keep the layers from moving together).
    constexpr std::array<float, RigEngine::c_tracks> c_rideOffsets { 1.0f, 1.37f, 0.71f, 1.62f, 0.87f, 1.19f, 0.79f, 1.48f, 0.93f, 1.27f };

    float coef(float ms, float fs) { return 1.0f - std::exp(-1.0f / (0.001f * ms * fs)); }
}

//==============================================================================
float RigEngine::trackSpeed(int track) const
{
    const auto t = static_cast<size_t>(track);
    return m_variNow[t] ? std::exp2(m_trackPitchNow[t] / 12.0f) : 1.0f;
}

std::array<float, 3> RigEngine::takeSectionLevels()
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
    for (auto& scope : m_scopes)
        scope.prepare(sampleRate);
    m_modGlide = 1.0f - std::exp(-static_cast<float>(c_chunk) / (0.015f * m_fs)); // ~15 ms
    const float controlRate = m_fs / static_cast<float>(c_chunk);

    for (auto& s : m_levelSprings) s.prepare(controlRate, 2.5f, 0.85f);
    m_swellSpring.prepare(controlRate, 1.8f, 0.7f);
    m_energySpring.prepare(controlRate, 1.5f, 0.8f);
    m_pitchSpring.prepare(controlRate, 1.6f, 0.55f);
    m_morph.prepare(sampleRate, c_chunk);
    m_morphMix.prepare(controlRate, 3.0f, 1.0f);
    for (auto* s : { &m_fillSpring, &m_spaceSpring })
        s->prepare(controlRate, 2.0f, 0.8f);

    m_trackPitchGlide = 1.0f - std::exp(-static_cast<float>(c_chunk) / (0.04f * m_fs)); // ~40 ms
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
        m_fillLines[static_cast<size_t>(t)].prepare(static_cast<int>(2.2f * m_fs));
        m_fillTone[static_cast<size_t>(t)].set(6000.0f, m_fs);
        m_rideDrift[static_cast<size_t>(t)].prepare(m_fs, 0.3f + 0.11f * static_cast<float>(t), static_cast<std::uint32_t>(4242 + 977 * t));
    }

    for (auto& b : m_buses)
    {
        auto* bus = &b;
        bus->line.prepare(static_cast<int>(c_lineSeconds * m_fs));
        bus->flL.prepare(static_cast<int>(0.02f * m_fs));
        bus->flR.prepare(static_cast<int>(0.02f * m_fs));
        bus->lowA.set(130.0f, 0.7071f, m_fs);
        bus->lowB.set(130.0f, 0.7071f, m_fs);
        bus->subA.set(90.0f, 0.7071f, m_fs);
        bus->subB.set(90.0f, 0.7071f, m_fs);
        bus->harm.set(170.0f, 0.8f, m_fs);
        bus->shelfL.set(110.0f, 0.7071f, m_fs);
        bus->shelfR.set(110.0f, 0.7071f, m_fs);
        bus->lowFollower.set(5.0f, 90.0f, m_fs);
        bus->follower.set(4.0f, 160.0f, m_fs);
        for (auto& s : bus->springs) s.prepare(controlRate, 2.2f, 0.6f);
        for (auto* d : { &bus->byBus, &bus->byHits })
            for (auto& band : d->band)
                band.set(1000.0f, 0.35f, m_fs);
    }

    m_detect.set(1.0f, 60.0f, m_fs);
    m_hitsFollower.set(2.0f, 120.0f, m_fs);
    m_mixFollower.set(10.0f, 300.0f, m_fs);

    m_echoL.prepare(static_cast<int>(1.2f * m_fs));
    m_echoR.prepare(static_cast<int>(1.2f * m_fs));
    m_echoLpL.set(4500.0f, m_fs); m_echoLpR.set(4500.0f, m_fs);
    m_echoHpL.set(220.0f, m_fs);  m_echoHpR.set(220.0f, m_fs);
    m_reverb.setSampleRate(sampleRate);
    m_dcL.set(8.0f, m_fs);
    m_dcR.set(8.0f, m_fs);

    m_rng.seed(0x5EA5EAu);
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
    m_coverageFor.fill(nullptr);
    m_ridePhase.fill(0.0);
    m_rideDepth.fill(0.0f);
    for (size_t t = 0; t < m_ridePhase.size(); ++t)
        m_ridePhase[t] = 0.17 * static_cast<double>(t); // start the rides apart

    for (size_t i = 0; i < m_buses.size(); ++i)
    {
        auto& bus = m_buses[i];
        bus.line.reset();
        bus.flL.reset();
        bus.flR.reset();
        bus.env = 0.0f;
        bus.duck = 0.0f;
        bus.peakRef = 1.0e-3f;
        startPass(bus.heads[0], m_params.buses[i]);
        startPass(bus.heads[1], m_params.buses[i]);
        bus.heads[1].time = 0.5 * bus.heads[1].length;
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

    m_echoL.reset(); m_echoR.reset();
    m_reverb.reset();
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

    for (size_t t = 0; t < m_levelSprings.size(); ++t)
        m_levelSprings[t].snap(m_params.levels[t]);
    m_swellSpring.snap(m_params.swell ? 1.0f : 0.0f);
    m_energySpring.snap(m_params.energy);
    m_pitchSpring.snap(m_params.pitch);
    m_fillSpring.snap(m_params.fill);
    m_spaceSpring.snap(m_params.space);
    m_morph.reset();
    m_morphMix.snap(m_params.morphOn ? 1.0f : 0.0f);
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
}

void RigEngine::snapBus(BusState& bus, const BusParams& b)
{
    const float v[9] = { b.doppler, b.width, b.bend, b.low, b.swirl, b.filter, b.drive, b.duck, b.key };
    for (size_t k = 0; k < 9; ++k)
        bus.springs[k].snap(v[k]);
}

float RigEngine::getLatencySamples() const { return 0.0f; }

void RigEngine::glideMods()
{
    const float a = m_modGlide;
    m_energyModNow += (m_params.energyMod - m_energyModNow) * a;
    m_pitchModNow += (m_params.pitchMod - m_pitchModNow) * a;
    m_fillModNow += (m_params.fillMod - m_fillModNow) * a;
    m_spaceModNow += (m_params.spaceMod - m_spaceModNow) * a;
    for (size_t b = 0; b < m_busModNow.size(); ++b)
        for (size_t k = 0; k < m_busModNow[b].size(); ++k)
            m_busModNow[b][k] += (m_params.busMod[b][k] - m_busModNow[b][k]) * a;
}

void RigEngine::restart()
{
    // Play pressed: every track starts again from its start marker, the rides from the top.
    for (auto& p : m_players)
        p.reset();
    for (size_t t = 0; t < m_ridePhase.size(); ++t)
        m_ridePhase[t] = 0.17 * static_cast<double>(t);
    m_gain.fill(0.0f);
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

//==============================================================================
void RigEngine::startPass(Head& h, const BusParams& p)
{
    // One pass of the moving source. Speed, distance and direction are randomly
    // automated (doppler speed etc.).
    const float c = m_params.chaos;
    const float d = sj::clamp(p.doppler, 0.0f, 1.0f);
    h.time = 0.0;
    // Pass length follows Rate exponentially (6 s .. 1.6 s), like the ride speed.
    h.length = sj::clamp(6.0f * std::pow(1.6f / 6.0f, sj::clamp(m_params.rate, 0.0f, 1.0f)) * std::exp2(c * 0.5f * m_rng.gauss()), 0.8f, 8.0f);
    // Bend scales the speed (= the doppler pitch bend); the pass still swells and pans as set by Doppler and Width.
    h.machBase = 0.42f * std::pow(d, 1.3f) * (1.0f + c * 0.6f * m_rng.bipolar());
    h.mach = sj::clamp(h.machBase * sj::clamp(p.bend, 0.0f, 1.0f), 0.0f, 0.5f);
    h.mach = std::min(h.mach, c_maxPassDelay / (0.5f * h.length));
    h.closeness = sj::clamp(0.55f + 0.45f * d * m_rng.bipolar() * (0.4f + c), 0.0f, 1.0f);
    h.closest = 0.004f * std::exp2(5.0f * (1.0f - h.closeness));
    h.dir = m_rng.bipolar() >= 0.0f ? 1.0f : -1.0f;
    h.aimed = false;
}

void RigEngine::followPass(Head& h, const Head& from, float bend, bool newPass)
{
    // The same pass as the lead bus (when, how long, how close, which way), at this bus's own Bend.
    h.time = from.time;
    h.length = from.length;
    h.machBase = from.machBase;
    h.closest = from.closest;
    h.closeness = from.closeness;
    h.dir = from.dir;
    if (newPass)
        h.aimed = false;
    if (newPass)
        h.mach = std::min(sj::clamp(h.machBase * sj::clamp(bend, 0.0f, 1.0f), 0.0f, 0.5f), c_maxPassDelay / (0.5f * h.length));
}

void RigEngine::renderTracks(int frames)
{
    for (auto& bus : m_buses)
        std::fill(bus.in.begin(), bus.in.begin() + frames, 0.0f);
    std::fill(m_hitsIn.begin(), m_hitsIn.begin() + frames, 0.0f);
    const int numBuses = c_maxBuses;

    // Energy: how deep each track's fader dips, even in dB across the knob (-4 dB per 10%, then silence at the top).
    // Modulator offsets go on after the springs (see Params::energyMod).
    glideMods();
    const float energyKnob = sj::clamp(m_energySpring.step() + (m_params.rideOn ? m_energyModNow : 0.0f), 0.0f, 1.0f);
    const float energy = ::Params::energyDepth(energyKnob);
    const float swell = sj::clamp(m_swellSpring.step(), 0.0f, 1.0f);
    const float pitchRatio = std::exp2(sj::clamp(m_pitchSpring.step() + m_pitchModNow, -24.0f, 24.0f) / 12.0f);
    // Fill: echoes after the fader (so they hang into the ride's dips), up to about +6 dB of echo send.
    const float fillKnob = sj::clamp(m_fillSpring.step() + m_fillModNow, 0.0f, 1.0f);
    const float fill = 2.0f * std::pow(fillKnob, 1.2f);
    const float chaos = m_params.chaos;
    const float baseHz = ::Params::rideHz(m_params.rate);
    const float invFrames = 1.0f / static_cast<float>(frames);
    const float morphMix = sj::clamp(m_morphMix.step(), 0.0f, 1.0f);
    m_morphMixNow = morphMix;
    const bool morphing = morphMix > 1.0e-4f;

    // Each track's own pitch, settled before the hit window below (which depends on every track's speed and mode).
    // The pitch glides to a new setting. A VARI switch fades the track out (~15 ms) and swaps the mode; switching to
    // the shifter then waits for its latency (its delay line refills) before fading back in, so neither a jump in
    // speed nor a step in the delayed sound is heard as a click.
    std::array<float, c_tracks> modeFrom {}, modeTo {};
    {
        const float modeStep = static_cast<float>(frames) / (0.015f * m_fs);
        for (size_t ti = 0; ti < static_cast<size_t>(c_tracks); ++ti)
        {
            auto& now = m_trackPitchNow[ti];
            const float want = m_params.trackPitch[ti];
            now += (want - now) * m_trackPitchGlide;
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
        const double ratio = juce::jmax(0.01, static_cast<double>(pitchRatio));
        m_hitPreView.store(toPeak * ratio, std::memory_order_relaxed);
        m_hitPostView.store(juce::jmax(0.0, period - toPeak - 0.08) * ratio, std::memory_order_relaxed);
        return period;
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
            for (auto& bus : m_buses)
                for (auto& h : bus.heads)
                {
                    const double toPeak = 0.5 * static_cast<double>(h.length) - h.time;
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
            if (bestHead != nullptr)
            {
                bestHead->aimed = true;
                m_lastAimTarget = m_aimClock + best * m_sampleRate;
                publishCue(hitCue, best, maxPost);
            }
        }
    }
    m_aimClock += frames;

    for (int t = 0; t < c_tracks; ++t)
    {
        const auto ti = static_cast<size_t>(t);
        const float level = sj::clamp(m_levelSprings[ti].step(), 0.0f, 1.2f);

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
        const float r = sj::clamp(shaped * (1.0f - 0.6f * chaos * m_rideDepth[ti]) * (1.0f + 0.2f * chaos * drift), 0.0f, 1.0f);
        float ride = sj::lerp(1.0f, r, energy);
        ride = sj::lerp(ride, 1.0f, swell);
        m_rideView[ti].store(ride, std::memory_order_relaxed);

        const CargoData* data = m_cargo != nullptr ? m_cargo->getLive(t) : nullptr;
        const float laneGain = m_cargo != nullptr ? m_cargo->laneGain(CargoHold::c_numBuiltIns + t) : 1.0f;
        const float target = data != nullptr ? level * ride * laneGain : 0.0f;

        if (data == nullptr && m_gain[ti] < 1.0e-5f)
        {
            m_gain[ti] = 0.0f;
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

        const bool skip = m_cargo == nullptr || m_cargo->getSkipSilence(t);
        m_players[ti].setStart(m_cargo != nullptr ? m_cargo->getStart(t) : 0.0f);
        const float rate = pitchRatio * (vari ? ownRatio : 1.0f);
        const bool syncedHit = timing != 0 && m_cargo != nullptr && m_cargo->getBus(t) == CargoHold::c_hitsBus;
        if (syncedHit)
        {
            CargoCue cue = hitCue;
            if (cue.active && ! vari) // heard later by the shifter's latency: fire that much earlier
                cue.secondsToPeak = std::max(0.0, cue.secondsToPeak - static_cast<double>(m_shifters[ti].getLatencySamples()) / m_sampleRate);
            m_players[ti].render(data, CargoRole::Impact, skip, rate, chaos, m_rng, 0.15f, cue, m_trackScratch.data(), frames);
        }
        else
            m_players[ti].render(data, CargoRole::Bed, skip, rate, chaos, m_rng, 0.6f, CargoCue {}, m_trackScratch.data(), frames);
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
        const float fillAmount = fill * (skip ? std::max(0.5f, m_fillAmount[ti]) : m_fillAmount[ti]);
        auto& line = m_fillLines[ti];
        // Routed by type: hits go to the hit gate, the rest to bus A or B.
        const int bus = m_cargo != nullptr ? m_cargo->getBus(t) : 0;
        float* dest = bus == CargoHold::c_hitsBus ? m_hitsIn.data()
                                                  : m_buses[static_cast<size_t>(juce::jlimit(0, numBuses - 1, bus))].in.data();
        const float g0 = m_gain[ti];
        for (int i = 0; i < frames; ++i)
        {
            const float x = m_trackScratch[static_cast<size_t>(i)];
            // Long delays (0.5 / 1 / 2 s) that fill the gaps of unsteady sounds.
            const float delayed = m_fillTone[ti].lowpass(0.5f * line.readLinear(0.5f * m_fs)
                                                       + 0.35f * line.readLinear(1.0f * m_fs)
                                                       + 0.25f * line.readLinear(2.0f * m_fs));
            const float frac = static_cast<float>(i + 1) * invFrames;
            const float g = g0 + (target - g0) * frac;
            const float dry = x * (modeGain0 + (modeGain1 - modeGain0) * frac) * g;
            line.push(dry + 0.3f * delayed);
            dest[i] += dry + fillAmount * delayed;
        }
        m_gain[ti] = target;
    }

    if (morphing)
    {
        SpectralMorph::Params mp;
        mp.x = m_params.morphX;
        mp.y = m_params.morphY;
        mp.mode = juce::jlimit(0, SpectralMorph::NumModes - 1, m_params.morphMode);
        m_morph.setParams(mp);
        // Morph between what feeds bus A and bus B, then hand the result back to the two buses in
        // proportion to X, so it still moves with A's or B's doppler and colour.
        auto& a = m_buses[0].in;
        auto& b = m_buses[1].in;
        std::copy(a.begin(), a.begin() + frames, m_morphA.begin());
        std::copy(b.begin(), b.begin() + frames, m_morphB.begin());
        m_morph.process(m_morphA.data(), m_morphB.data(), m_morphOut.data(), frames);
        // Equal power, so the middle of the pad isn't a dip in level.
        const float x = sj::clamp(m_params.morphX, 0.0f, 1.0f);
        const float toA = std::cos(0.5f * sj::c_pi * x), toB = std::sin(0.5f * sj::c_pi * x);
        for (int i = 0; i < frames; ++i)
        {
            const auto ii = static_cast<size_t>(i);
            a[ii] += morphMix * (toA * m_morphOut[ii] - a[ii]);
            b[ii] += morphMix * (toB * m_morphOut[ii] - b[ii]);
        }
    }

}

float RigEngine::keyLevel(const BusState& bus)
{
    const float rel = sj::clamp(bus.env / std::max(bus.peakRef, 1.0e-3f), 0.0f, 1.0f);
    const float db = 20.0f * std::log10(std::max(bus.env, 1.0e-6f));
    return rel * sj::clamp((db + 40.0f) / 20.0f, 0.0f, 1.0f); // nothing below -40 dBFS, full from -20 dBFS
}

void RigEngine::renderBus(BusState& bus, int frames, float key, bool isKeyBus,
                          const std::array<Head, 2>* lead, const std::array<Head, 2>* leadStart)
{
    // The knobs (after their springs) plus any modulator offsets.
    const size_t busIndex = static_cast<size_t>(&bus - m_buses.data());
    const auto& mod = m_busModNow[busIndex];
    // Linked: bus B flies A's passes with A's Doppler and Width, so it takes A's modulation of them too.
    const auto& leadMod = (m_linkNow && busIndex > 0) ? m_busModNow[0] : mod;
    auto knob = [&](size_t k, float offset) { return sj::clamp(bus.springs[k].step() + offset, 0.0f, 1.0f); };
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

    for (int i = 0; i < frames; ++i)
        bus.line.push(bus.in[static_cast<size_t>(i)]);

    std::fill(bus.l.begin(), bus.l.begin() + frames, 0.0f);
    std::fill(bus.r.begin(), bus.r.begin() + frames, 0.0f);

    // Linked: line up with bus A's passes as they were at the start of this chunk (A has already rendered it).
    if (leadStart != nullptr)
        for (size_t k = 0; k < bus.heads.size(); ++k)
            followPass(bus.heads[k], (*leadStart)[k], p.bend, false);

    // Doppler: two overlapping passes (sine windows half a pass apart keep the power steady).
    for (size_t k = 0; k < bus.heads.size(); ++k)
    {
        auto& h = bus.heads[k];
        {
            const double s = (h.time / h.length - 0.5) * h.length;
            const float x = h.dir * h.mach * static_cast<float>(s);
            const float dist = std::sqrt(x * x + h.closest * h.closest);
            const float u = static_cast<float>(h.time / h.length);
            const float near = std::pow(sj::clamp(0.006f / dist, 0.0f, 1.0f), 0.6f);
            const float cutoff = sj::clamp(19000.0f * sj::lerp(1.0f, near * (0.3f + 0.7f * std::sin(sj::c_pi * u)), p.doppler), 600.0f, 20000.0f);
            h.airL.set(cutoff, fs);
            h.airR.set(cutoff, fs);
        }

        for (int i = 0; i < frames; ++i)
        {
            if (h.time >= h.length)
            {
                if (lead != nullptr)
                {
                    // Take the pass bus A started at this same moment (it rendered this chunk first).
                    followPass(h, (*lead)[k], p.bend, true);
                    h.time = 0.0;
                }
                else
                    startPass(h, p);
            }

            const float u = static_cast<float>(h.time / h.length);
            const double s = (h.time / h.length - 0.5) * h.length;
            const float x = h.dir * h.mach * static_cast<float>(s);
            const float c2 = h.closest * h.closest;
            const float dist = std::sqrt(x * x + c2);
            // Where the pass is across the field, scaled by Width: it pans the pass and sets the time difference
            // between the ears (so Width, not the amount of bend, sets how wide a pass is; Width 0 is centred).
            const float lateral = static_cast<float>(s) / (0.5f * h.length);
            const float side = std::copysign(std::pow(std::abs(lateral), 0.6f), lateral);
            const float az = std::pow(p.width, 0.8f) * sj::clamp(h.dir * side, -1.0f, 1.0f);
            const float distL = dist + c_earOffset * az;
            const float distR = dist - c_earOffset * az;
            const float back = static_cast<float>(frames - 1 - i);

            const float l = bus.line.read((c_baseDelay + distL) * fs + back);
            const float r = bus.line.read((c_baseDelay + distR) * fs + back);

            const float window = std::sin(sj::c_pi * u);
            const float nearness = h.closest / std::max(dist, 1.0e-6f);
            const float gain = window * (1.0f + 0.8f * p.doppler * (nearness - 0.35f));
            const float theta = (0.95f * az + 1.0f) * (sj::c_pi * 0.25f);

            bus.l[static_cast<size_t>(i)] += h.airL.lowpass(l) * gain * std::cos(theta) * 1.41421356f;
            bus.r[static_cast<size_t>(i)] += h.airR.lowpass(r) * gain * std::sin(theta) * 1.41421356f;

            h.time += dt;
        }
        h.view = std::sin(sj::c_pi * static_cast<float>(h.time / h.length));
        h.x = h.dir * std::tanh(5.0f * static_cast<float>(h.time / h.length - 0.5));
    }

    // Level tracking for the low-end lift and the views.
    float chunkPeak = 0.0f;
    for (int i = 0; i < frames; ++i)
        chunkPeak = std::max(chunkPeak, bus.follower.process(0.5f * (bus.l[static_cast<size_t>(i)] + bus.r[static_cast<size_t>(i)])));
    bus.env = chunkPeak;
    bus.peakRef = std::max(bus.env, bus.peakRef * std::exp(-static_cast<float>(frames) / (3.0f * fs)));
    const float peakness = sj::clamp(bus.env / std::max(bus.peakRef, 1.0e-6f), 0.0f, 1.0f);

    // Low end: sub octave, overtones, and a lift at the loudest point (a multiband-dynamics style lift).
    if (p.low > 1.0e-4f)
    {
        const float shelf = sj::dbToGain(p.low * (2.0f + 9.0f * peakness * peakness * peakness)) - 1.0f;
        for (int i = 0; i < frames; ++i)
        {
            auto& l = bus.l[static_cast<size_t>(i)];
            auto& r = bus.r[static_cast<size_t>(i)];
            const float low = bus.lowB.lowpass(bus.lowA.lowpass(0.5f * (l + r)));
            if (bus.lastLow <= 0.0f && low > 0.0f)
                bus.flip = -bus.flip;
            bus.lastLow = low;
            const float level = bus.lowFollower.process(low);
            const float sub = bus.subB.lowpass(bus.subA.lowpass(bus.flip * level));
            const float harm = bus.harm.bandpass(sj::fastTanh(4.0f * std::abs(low)));
            const float add = p.low * (1.3f * sub + 0.45f * harm);
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
        bus.lfo += (0.12f + 1.2f * peakness * swirl) * static_cast<float>(frames) / fs;
        if (bus.lfo >= 1.0f) bus.lfo -= 1.0f;
        const float lfo = std::sin(sj::c_twoPi * bus.lfo);
        const float lfoR = std::cos(sj::c_twoPi * bus.lfo);
        auto coefFor = [fs](float fc)
        {
            const float t = std::tan(sj::c_pi * sj::clamp(fc, 20.0f, fs * 0.45f) / fs);
            return (t - 1.0f) / (t + 1.0f);
        };
        const float aL = coefFor(260.0f * std::exp2(3.6f * (0.5f + 0.5f * lfo) + 1.2f * peakness));
        const float aR = coefFor(260.0f * std::exp2(3.6f * (0.5f + 0.5f * lfoR) + 1.2f * peakness));
        const float fb = 0.55f * swirl;
        const float far = (1.0f - peakness) * (1.0f - peakness);
        for (int i = 0; i < frames; ++i)
        {
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
            float ol = l + phMix * (yl - l), orr = r + phMix * (yr - r);
            if (flMix > 1.0e-4f)
            {
                const float msL = 0.3f + 6.5f * far * (0.85f + 0.15f * lfo);
                const float msR = 0.3f + 6.5f * far * (0.85f - 0.15f * lfo);
                bus.flL.push(ol + 0.6f * swirl * bus.flFbL);
                bus.flR.push(orr + 0.6f * swirl * bus.flFbR);
                bus.flFbL = bus.flL.read(msL * 0.001f * fs);
                bus.flFbR = bus.flR.read(msR * 0.001f * fs);
                ol += flMix * (bus.flFbL - ol);
                orr += flMix * (bus.flFbR - orr);
            }
            l = ol;
            r = orr;
            postSq += l * l + r * r;
        }
        // Level match over ~300 ms, within +/-6 dB.
        const float c = 1.0f - std::exp(-static_cast<float>(frames) / (0.3f * fs));
        bus.swirlPre += (preSq / static_cast<float>(frames) - bus.swirlPre) * c;
        bus.swirlPost += (postSq / static_cast<float>(frames) - bus.swirlPost) * c;
        const float want = bus.swirlPost > 1.0e-10f ? sj::clamp(std::sqrt(bus.swirlPre / bus.swirlPost), 0.5f, 2.0f) : 1.0f;
        const float g0 = bus.swirlGain;
        bus.swirlGain += (want - bus.swirlGain) * 0.2f;
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
        bus.driveGain += (want - bus.driveGain) * 0.2f;
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
    const float s = sj::clamp(m_spaceSpring.step() + m_spaceModNow, 0.0f, 1.0f);
    if (s < 1.0e-4f)
        return;

    const float fb = 0.25f + 0.4f * s;
    const float wet = 0.35f * s;
    const float tL = 0.34f * m_fs, tR = 0.51f * m_fs;
    for (int i = 0; i < frames; ++i)
    {
        auto& l = m_mixL[static_cast<size_t>(i)];
        auto& r = m_mixR[static_cast<size_t>(i)];
        const float wl = m_echoL.read(tL), wr = m_echoR.read(tR);
        m_echoL.push(l + fb * m_echoLpR.lowpass(m_echoHpR.highpass(wr)));
        m_echoR.push(r + fb * m_echoLpL.lowpass(m_echoHpL.highpass(wl)));
        l += wet * wl;
        r += wet * wr;
    }

    juce::Reverb::Parameters rp;
    rp.roomSize = 0.6f + 0.35f * s;
    rp.damping = 0.35f;
    rp.wetLevel = 0.2f * s;
    rp.dryLevel = 0.5f;
    rp.width = 1.0f;
    m_reverb.setParameters(rp);
    m_reverb.processStereo(m_mixL.data(), m_mixR.data(), frames);
}

//==============================================================================
void RigEngine::process(const float* inL, const float* inR, float* outL, float* outR, int numSamples)
{
    juce::ignoreUnused(inL, inR); // sample-based: the plug-in has no audio input
    int offset = 0;
    while (offset < numSamples)
    {
        const int frames = std::min(c_chunk, numSamples - offset);

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
            bus.duck += (duckTarget - bus.duck) * 0.3f;
            if (b == 1)
                keyBus.duck += (keyTarget - keyBus.duck) * 0.3f;
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

        renderHits(frames);

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
            ++m_sectionSamples;
            m_mixL[ii] = l;
            m_mixR[ii] = r;
        }
        // Peak tremolo on everything (by the mix's level), before Space so the echoes and reverb carry it.
        applyTremolo(m_trem[0], m_mixL.data(), m_mixR.data(), frames, m_params.tremTarget == 0);
        renderSpace(frames);

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
            m_viewData[i * 4 + 3].store(static_cast<float>(head.time / head.length), std::memory_order_relaxed);
        }
    }
    m_energyView.store(sj::clamp(m_mixFollower.env * 3.0f, 0.0f, 1.0f), std::memory_order_relaxed);
}
