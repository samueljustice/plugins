#include "RigEngine.h"

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
    m_loCoef = 1.0f - std::exp(-sj::c_twoPi * 250.0f / m_fs);
    m_midCoef = 1.0f - std::exp(-sj::c_twoPi * 4000.0f / m_fs);
    const float controlRate = m_fs / static_cast<float>(c_chunk);

    for (auto& s : m_levelSprings) s.prepare(controlRate, 2.5f, 0.85f);
    m_swellSpring.prepare(controlRate, 1.8f, 0.7f);
    m_energySpring.prepare(controlRate, 1.5f, 0.8f);
    m_pitchSpring.prepare(controlRate, 1.6f, 0.55f);
    m_morph.prepare(sampleRate, c_chunk);
    m_morphMix.prepare(controlRate, 3.0f, 1.0f);
    for (auto* s : { &m_fillSpring, &m_spaceSpring, &m_inputSpring })
        s->prepare(controlRate, 2.0f, 0.8f);

    m_trackPitchGlide = 1.0f - std::exp(-static_cast<float>(c_chunk) / (0.04f * m_fs)); // ~40 ms
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
        snapBus(bus, m_params.buses[i]);
    }

    m_echoL.reset(); m_echoR.reset();
    m_reverb.reset();
    m_dcL.reset(); m_dcR.reset();
    m_gate = m_hold = 0.0f;
    m_peakRef = 1.0e-3f;
    m_slowEnv = 0.0f;
    m_talker = 0;
    m_talkerHold = 0.0f;
    for (auto& bus : m_buses)
        bus.byBus = bus.byHits = {};

    for (size_t t = 0; t < m_levelSprings.size(); ++t)
        m_levelSprings[t].snap(m_params.levels[t]);
    m_swellSpring.snap(m_params.swell ? 1.0f : 0.0f);
    m_energySpring.snap(m_params.energy);
    m_pitchSpring.snap(m_params.pitch);
    m_fillSpring.snap(m_params.fill);
    m_spaceSpring.snap(m_params.space);
    m_inputSpring.snap(m_params.input);
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
    m_inputSpring.target = p.input;
    m_morphMix.target = p.morphOn ? 1.0f : 0.0f;

    for (size_t i = 0; i < m_buses.size(); ++i)
    {
        const auto& b = p.buses[i];
        const float v[9] = { b.doppler, b.width, b.bend, b.low, b.swirl, b.filter, b.drive, b.duck, b.key };
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
    h.length = sj::clamp(sj::lerp(6.0f, 1.6f, m_params.rate) * std::exp2(c * 0.5f * m_rng.gauss()), 0.8f, 8.0f);
    // Bend scales the speed (= the doppler pitch bend); the pass still swells and pans as set by Doppler and Width.
    h.mach = sj::clamp(0.42f * std::pow(d, 1.3f) * (1.0f + c * 0.6f * m_rng.bipolar()) * sj::clamp(p.bend, 0.0f, 1.0f), 0.0f, 0.5f);
    h.mach = std::min(h.mach, c_maxPassDelay / (0.5f * h.length));
    h.closeness = sj::clamp(0.55f + 0.45f * d * m_rng.bipolar() * (0.4f + c), 0.0f, 1.0f);
    h.closest = 0.004f * std::exp2(5.0f * (1.0f - h.closeness));
    h.dir = m_rng.bipolar() >= 0.0f ? 1.0f : -1.0f;
}

void RigEngine::renderTracks(const float* inL, const float* inR, int offset, int frames)
{
    for (auto& bus : m_buses)
        std::fill(bus.in.begin(), bus.in.begin() + frames, 0.0f);
    std::fill(m_hitsIn.begin(), m_hitsIn.begin() + frames, 0.0f);
    const int numBuses = c_maxBuses;

    const float energy = sj::clamp(m_energySpring.step(), 0.0f, 1.0f);
    const float swell = sj::clamp(m_swellSpring.step(), 0.0f, 1.0f);
    const float pitchRatio = std::exp2(m_pitchSpring.step() / 12.0f);
    const float fill = sj::clamp(m_fillSpring.step(), 0.0f, 1.0f);
    const float input = sj::clamp(m_inputSpring.step(), 0.0f, 1.0f);
    const float chaos = m_params.chaos;
    const float baseHz = 0.04f + 0.5f * std::pow(m_params.rate, 1.6f);
    const float invFrames = 1.0f / static_cast<float>(frames);
    const float morphMix = sj::clamp(m_morphMix.step(), 0.0f, 1.0f);
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

    // Hit sync: one shared beat for every hit track; each fires its next sound so the transients
    // (the regions' focus points) land together. Switching it either way restarts the hit tracks.
    const bool hitSync = m_params.hitSync;
    if (hitSync != m_lastHitSync)
    {
        m_lastHitSync = hitSync;
        m_hitClock = 0.0;
        for (int t = 0; t < c_tracks; ++t)
            if (m_cargo != nullptr && m_cargo->getBus(t) == CargoHold::c_hitsBus)
                m_players[static_cast<size_t>(t)].reset();
    }
    // The last cued beat reaches its transients: publish it for the track view.
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
    if (! hitSync)
        m_beatViewWait = -1.0;

    CargoCue hitCue;
    if (hitSync && m_cargo != nullptr)
    {
        m_hitClock -= frames;
        if (m_hitClock <= 0.0)
        {
            // The window comes from the regions exactly as each player will fire them, so skip silence (off = the
            // whole file is one hit, aligned on its first transient) and the start markers are respected.
            // Each track at its own speed (global Pitch times its own VARI pitch), in output seconds. A VARI-off track
            // is heard later by its shifter's latency, so it needs that much more lead-in to fire early enough.
            double maxPre = 0.0, maxPost = 0.0;
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
            const double period = std::min(maxPre + maxPost + 0.08, 4.0);
            hitCue.active = true;
            hitCue.secondsToPeak = maxPre;
            hitCue.flightSeconds = period;
            m_hitClock += period * m_sampleRate;
            m_beatViewWait = maxPre * m_sampleRate; // the view moves when the transients land
            const double ratio = juce::jmax(0.01, static_cast<double>(pitchRatio));
            m_hitPreView.store(maxPre * ratio, std::memory_order_relaxed);
            m_hitPostView.store(juce::jmax(0.0, period - maxPre - 0.08) * ratio, std::memory_order_relaxed);
        }
    }


    for (int t = 0; t < c_tracks; ++t)
    {
        const auto ti = static_cast<size_t>(t);
        const float level = sj::clamp(m_levelSprings[ti].step(), 0.0f, 1.2f);

        // Ride: an LFO on the fader at this track's own offset speed, plus a random hand on it.
        m_ridePhase[ti] += static_cast<double>(baseHz * c_rideOffsets[ti] * (1.0f + 0.3f * chaos * m_rideDrift[ti].current)
                                               * static_cast<float>(frames) / m_fs);
        if (m_ridePhase[ti] >= 1.0) m_ridePhase[ti] -= 1.0;
        const float lfo = 0.5f - 0.5f * std::cos(sj::c_twoPi * static_cast<float>(m_ridePhase[ti]));
        // Higher energy = deeper and more peaked swells (the fader spends longer down low).
        const float shaped = std::pow(lfo, 1.0f + 2.5f * energy);
        const float drift = m_rideDrift[ti].next(frames);
        const float r = sj::clamp(shaped + 0.3f * chaos * drift, 0.0f, 1.0f);
        float ride = sj::lerp(1.0f, r, sj::clamp(energy * 1.35f, 0.0f, 1.0f));
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
        const bool syncedHit = hitSync && m_cargo != nullptr && m_cargo->getBus(t) == CargoHold::c_hitsBus;
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

        const float fillAmount = fill * m_fillAmount[ti];
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
            line.push(x + 0.3f * delayed);
            const float frac = static_cast<float>(i + 1) * invFrames;
            const float y = (x + fillAmount * delayed) * (modeGain0 + (modeGain1 - modeGain0) * frac);
            const float g = g0 + (target - g0) * frac;
            dest[i] += y * g;
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
        const float x = sj::clamp(m_params.morphX, 0.0f, 1.0f);
        for (int i = 0; i < frames; ++i)
        {
            const auto ii = static_cast<size_t>(i);
            a[ii] += morphMix * ((1.0f - x) * m_morphOut[ii] - a[ii]);
            b[ii] += morphMix * (x * m_morphOut[ii] - b[ii]);
        }
    }

    if (input > 1.0e-4f && inL != nullptr)
        for (int i = 0; i < frames; ++i)
        {
            const float x = (inR != nullptr) ? 0.5f * (inL[offset + i] + inR[offset + i]) : inL[offset + i];
            m_buses[0].in[static_cast<size_t>(i)] += x * input;
        }
}

void RigEngine::renderBus(BusState& bus, int frames, float keyEnv, bool isKeyBus)
{
    BusParams p;
    p.doppler = sj::clamp(bus.springs[0].step(), 0.0f, 1.0f);
    p.width = sj::clamp(bus.springs[1].step(), 0.0f, 1.0f);
    p.bend = sj::clamp(bus.springs[2].step(), 0.0f, 1.0f);
    p.low = sj::clamp(bus.springs[3].step(), 0.0f, 1.0f);
    p.swirl = sj::clamp(bus.springs[4].step(), 0.0f, 1.0f);
    p.filter = sj::clamp(bus.springs[5].step(), 0.0f, 1.0f);
    p.drive = sj::clamp(bus.springs[6].step(), 0.0f, 1.0f);
    p.duck = isKeyBus ? 0.0f : sj::clamp(bus.springs[7].step(), 0.0f, 1.0f);
    p.key = isKeyBus ? 0.0f : sj::clamp(bus.springs[8].step(), 0.0f, 1.0f);
    const float keyAmount = p.key;

    const float fs = m_fs;
    const double dt = 1.0 / m_sampleRate;

    for (int i = 0; i < frames; ++i)
        bus.line.push(bus.in[static_cast<size_t>(i)]);

    std::fill(bus.l.begin(), bus.l.begin() + frames, 0.0f);
    std::fill(bus.r.begin(), bus.r.begin() + frames, 0.0f);

    // Doppler: two overlapping passes (sine windows half a pass apart keep the power steady).
    for (auto& h : bus.heads)
    {
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
                startPass(h, p);

            const float u = static_cast<float>(h.time / h.length);
            const double s = (h.time / h.length - 0.5) * h.length;
            const float x = h.dir * h.mach * static_cast<float>(s);
            const float c2 = h.closest * h.closest;
            const float dist = std::sqrt(x * x + c2);
            const float distL = std::sqrt((x + c_earOffset) * (x + c_earOffset) + c2);
            const float distR = std::sqrt((x - c_earOffset) * (x - c_earOffset) + c2);
            const float back = static_cast<float>(frames - 1 - i);

            const float l = bus.line.read((c_baseDelay + distL) * fs + back);
            const float r = bus.line.read((c_baseDelay + distR) * fs + back);

            const float window = std::sin(sj::c_pi * u);
            const float nearness = h.closest / std::max(dist, 1.0e-6f);
            const float gain = window * (1.0f + 0.8f * p.doppler * (nearness - 0.35f));
            // Pan from the pass position (independent of how much it bends), scaled by Width.
            const float lateral = static_cast<float>(s) / (0.5f * h.length);
            const float az = p.width * sj::clamp(h.dir * lateral * std::sqrt(p.doppler), -1.0f, 1.0f);
            const float theta = (0.8f * az + 1.0f) * (sj::c_pi * 0.25f);

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
        const float phMix = sj::clamp(p.swirl * 2.0f, 0.0f, 1.0f) * 0.5f;
        const float flMix = sj::clamp((p.swirl - 0.25f) / 0.75f, 0.0f, 1.0f) * 0.5f;
        bus.lfo += (0.12f + 1.2f * peakness * p.swirl) * static_cast<float>(frames) / fs;
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
        const float fb = 0.55f * p.swirl;
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
                bus.flL.push(ol + 0.6f * p.swirl * bus.flFbL);
                bus.flR.push(orr + 0.6f * p.swirl * bus.flFbR);
                bus.flFbL = bus.flL.read(msL * 0.001f * fs);
                bus.flFbR = bus.flR.read(msR * 0.001f * fs);
                ol += flMix * (bus.flFbL - ol);
                orr += flMix * (bus.flFbR - orr);
                ol *= 1.0f + 0.4f * flMix;
                orr *= 1.0f + 0.4f * flMix;
            }
            l = ol;
            r = orr;
        }
    }

    // Envelope filter (opens with the bus), optionally keyed by the other bus.
    const float keyed = keyAmount * sj::clamp(keyEnv * 3.0f, 0.0f, 1.0f);
    const float mix = sj::clamp(std::max(p.filter * 1.6f, keyed), 0.0f, 1.0f);
    if (mix > 1.0e-4f)
    {
        const float follow = sj::clamp(bus.env * 2.8f, 0.0f, 1.0f);
        float cutoff = 170.0f * std::exp2(7.0f * std::sqrt(follow));
        if (p.filter < 1.0e-3f)
            cutoff = 18000.0f;
        cutoff = std::max(220.0f, cutoff * (1.0f - keyed) * (1.0f - keyed));
        const float q = 0.7f + 5.0f * p.filter * p.filter;
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

    // Drive
    if (p.drive > 1.0e-4f)
    {
        const float drive = 1.0f + 20.0f * p.drive * p.drive;
        const float comp = 1.0f / (1.0f + 0.35f * std::log2(drive));
        const float dmix = sj::clamp(p.drive * 2.5f, 0.0f, 1.0f);
        bus.toneL.set(20000.0f - 9000.0f * p.drive, fs);
        bus.toneR.set(20000.0f - 9000.0f * p.drive, fs);
        for (int i = 0; i < frames; ++i)
        {
            auto& l = bus.l[static_cast<size_t>(i)];
            auto& r = bus.r[static_cast<size_t>(i)];
            l = bus.toneL.lowpass(l + dmix * (sj::fastTanh(drive * l) * comp - l));
            r = bus.toneR.lowpass(r + dmix * (sj::fastTanh(drive * r) * comp - r));
        }
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

void RigEngine::duckSample(BusState::Duck& d, float target, float returnCoef, float& l, float& r)
{
    // Down at once, back up over the return time.
    if (returnCoef >= 1.0f || target >= d.amount)
        d.amount = target; // fast return: exactly the original
    else
        d.amount += (target - d.amount) * returnCoef;
    const float amt = sj::clamp(d.amount, 0.0f, 1.0f);
    float* ch[2] = { &l, &r };
    // The band splitters always run, so switching Duck mode mid-duck doesn't start from stale filter state.
    for (int c = 0; c < 2; ++c)
    {
        d.lo[c] += m_loCoef * (*ch[c] - d.lo[c]);
        d.mid[c] += m_midCoef * (*ch[c] - d.mid[c]);
    }
    switch (m_params.duckMode)
    {
        case 1: // Darken: low-pass the ducked bus (an envelope filter keyed by the other bus), a little quieter
        {
            const float fc = 18000.0f * std::exp2(-5.7f * sj::clamp(amt / 0.7f, 0.0f, 1.0f)); // 18 kHz .. ~350 Hz
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
            const float g = sj::clamp(1.0f - 1.2f * amt, 0.0f, 1.0f);
            for (int c = 0; c < 2; ++c)
            {
                const float x = *ch[c];
                const float mid = d.mid[c] - d.lo[c], hi = x - d.mid[c];
                *ch[c] = d.lo[c] + mid * g + hi;
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
    const float boost = sj::dbToGain(m_params.hitBoost - 6.0f); // the Boost readout shows this (-6 .. +12 dB)
    const float release = 1.0f - std::exp(-1.0f / (0.001f * sj::lerp(60.0f, 1200.0f, m_params.hitRelease) * m_fs));
    const float attack = coef(2.0f, m_fs);
    const float holdTime = 0.5f * sj::clamp(m_params.hitHold, 0.0f, 1.0f);
    const float floorGain = m_params.hitFloor > 0.001f ? sj::dbToGain(-40.0f + 37.0f * sj::clamp(m_params.hitFloor, 0.0f, 1.0f)) : 0.0f;
    const int mode = m_params.gateMode;
    const float levelThreshold = sj::dbToGain(-12.0f - 30.0f * sense);       // Level: a fixed level (-12 .. -42 dBFS)
    const float jump = sj::dbToGain(1.0f + 7.0f * (1.0f - sense));           // Jumps: how far above the slow envelope
    const float slowCoef = 1.0f - std::exp(-1.0f / (0.12f * m_fs));
    const float returnCoef = duckReturnCoef();
    const float decay = std::exp(-1.0f / (6.0f * m_fs));
    const float avgCoef = 1.0f - std::exp(-1.0f / (2.0f * m_fs));
    const float dt = 1.0f / m_fs;
    float gateMax = 0.0f;

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
            case 1:  open = env > levelThreshold; break;
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
        const float duck = 0.6f * m_params.hitDuck * sj::clamp(m_hitsFollower.process(gated) * 3.0f, 0.0f, 1.0f);
        for (int b = 0; b < numBuses; ++b)
        {
            auto& bus = m_buses[static_cast<size_t>(b)];
            duckSample(bus.byHits, duck, returnCoef, bus.l[ii], bus.r[ii]);
        }
    }
    m_gateView.store(gateMax, std::memory_order_relaxed);
}

void RigEngine::renderSpace(int frames)
{
    const float s = sj::clamp(m_spaceSpring.step(), 0.0f, 1.0f);
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

    const float widen = 1.0f + 0.5f * s;
    for (int i = 0; i < frames; ++i)
    {
        auto& l = m_mixL[static_cast<size_t>(i)];
        auto& r = m_mixR[static_cast<size_t>(i)];
        const float mid = 0.5f * (l + r), side = 0.5f * (l - r) * widen;
        l = mid + side;
        r = mid - side;
    }
}

//==============================================================================
void RigEngine::process(const float* inL, const float* inR, float* outL, float* outR, int numSamples)
{
    int offset = 0;
    while (offset < numSamples)
    {
        const int frames = std::min(c_chunk, numSamples - offset);

        renderTracks(inL, inR, offset, frames);

        // Bus A first: it is the key for the others (ducking and filtering).
        const int numBuses = c_maxBuses;
        auto& keyBus = m_buses[0];
        renderBus(keyBus, frames, 0.0f, true);
        for (int b = 1; b < numBuses; ++b)
        {
            auto& bus = m_buses[static_cast<size_t>(b)];
            renderBus(bus, frames, keyBus.env, false);

            // B is ducked by A. Take turns: whichever is louder (before any ducking, so no feedback) ducks the other.
            const float amount = sj::clamp(bus.springs[7].value, 0.0f, 1.0f);
            float duckTarget = amount * sj::clamp(keyBus.env * 2.5f, 0.0f, 1.0f);
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
                keyTarget = amount * sj::clamp(bus.env * 2.5f, 0.0f, 1.0f);
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
                duckSample(bus.byBus, 0.7f * (duck0 + (bus.duck - duck0) * t), returnCoef, bus.l[ii], bus.r[ii]);
                if (duckKey)
                    duckSample(keyBus.byBus, 0.7f * (key0 + (keyBus.duck - key0) * t), returnCoef, keyBus.l[ii], keyBus.r[ii]);
            }
        }

        renderHits(frames);

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
