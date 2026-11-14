#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "VariationRenderer.h"
#include "Version.h"

namespace
{
    std::unique_ptr<juce::PropertiesFile> openSettings()
    {
        juce::PropertiesFile::Options o;
        o.applicationName = "Swooshbuckler";
        o.folderName = "Sweet Justice";
        o.filenameSuffix = "settings";
        o.osxLibrarySubFolder = "Application Support";
        o.commonToAllUsers = false;
        return std::make_unique<juce::PropertiesFile>(o);
    }
}

SwooshbucklerAudioProcessor::SwooshbucklerAudioProcessor() : SwooshbucklerAudioProcessor(nullptr) {}

SwooshbucklerAudioProcessor::SwooshbucklerAudioProcessor(juce::PropertiesFile* offlineSettings)
    // Sample-based instrument: no audio input (so no feedback loop, no microphone), MIDI in, stereo out.
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true))
    , m_offline(offlineSettings != nullptr)
    , m_state(*this, nullptr, "Swooshbuckler", Params::createLayout(m_stowModeSource))
{
    auto raw = [this](const char* id) { return m_state.getRawParameterValue(id); };
    for (int i = 0; i < ParamIDs::numCargoTracks; ++i)
    {
        m_raw.tracks[static_cast<size_t>(i)] = raw(ParamIDs::cargoTrack[i]);
        m_raw.trackSemis[static_cast<size_t>(i)] = raw(ParamIDs::trackSemis[i]);
        m_raw.trackCents[static_cast<size_t>(i)] = raw(ParamIDs::trackCents[i]);
        m_raw.trackVari[static_cast<size_t>(i)] = raw(ParamIDs::trackVari[i]);
        m_trackPitchTarget[static_cast<size_t>(i)] = ModMatrix::targetIndex(ParamIDs::trackSemis[i]);
    }
    m_raw.pitch = raw(ParamIDs::pitch);
    m_raw.rideOn = raw(ParamIDs::rideOn);
    m_raw.rideFollowMaster = raw(ParamIDs::rideFollowMaster);
    m_raw.energy = raw(ParamIDs::energy);
    m_raw.rate = raw(ParamIDs::rate);
    m_raw.fill = raw(ParamIDs::fill);
    m_raw.chaos = raw(ParamIDs::chaos);
    for (int b = 0; b < ParamIDs::numBuses; ++b)
        for (int k = 0; k < ParamIDs::numBusControls; ++k)
            m_raw.bus[b][k] = ParamIDs::bus[b][k] != nullptr ? raw(ParamIDs::bus[b][k]) : nullptr;
    for (int b = 0; b < ParamIDs::numBuses; ++b)
        m_raw.passMode[b] = raw(ParamIDs::passMode[b]);
    m_raw.stowOn = raw(ParamIDs::stowOn);
    m_raw.stowMode = raw(ParamIDs::stowMode);
    m_stowModeSource->mode.store(m_raw.stowMode); // the Stowaway knobs read by the mode from here on
    for (int k = 0; k < ParamIDs::numStowControls; ++k)
    {
        m_raw.stow[k] = raw(ParamIDs::stow[k]);
        m_stowModeSource->knobs[static_cast<size_t>(k)].store(m_raw.stow[k]);
    }
    // Rumbles ride with Stowaway while it is switched on and a sub. The hold is told as soon as the switch or the
    // Mode moves, on whichever thread moves it (a flag, nothing else), so the routing is right with or without the
    // audio running.
    m_state.addParameterListener(ParamIDs::stowOn, this);
    m_state.addParameterListener(ParamIDs::stowMode, this);
    m_cargo.setRumbleToStowaway(rumblesRideStowaway());
    m_raw.hitSense = raw(ParamIDs::hitSense);
    m_raw.hitBoost = raw(ParamIDs::hitBoost);
    m_raw.hitRelease = raw(ParamIDs::hitRelease);
    m_raw.hitDuck = raw(ParamIDs::hitDuck);
    m_raw.hitSync = raw(ParamIDs::hitSync);
    m_raw.gateMode = raw(ParamIDs::gateMode);
    m_raw.hitHold = raw(ParamIDs::hitHold);
    m_raw.hitFloor = raw(ParamIDs::hitFloor);
    m_raw.duckMode = raw(ParamIDs::duckMode);
    m_raw.duckReturn = raw(ParamIDs::duckReturn);
    m_raw.takeTurns = raw(ParamIDs::takeTurns);
    m_raw.linkBuses = raw(ParamIDs::linkBuses);
    m_raw.hitAim = raw(ParamIDs::hitAim);
    m_raw.tremOn = raw(ParamIDs::tremOn);
    m_raw.tremDepth = raw(ParamIDs::tremDepth);
    m_raw.tremRate = raw(ParamIDs::tremRate);
    m_raw.tremThreshold = raw(ParamIDs::tremThreshold);
    m_raw.tremShape = raw(ParamIDs::tremShape);
    m_raw.tremTarget = raw(ParamIDs::tremTarget);
    m_raw.masterTrim = raw(ParamIDs::masterTrim);
    m_raw.space = raw(ParamIDs::space);
    m_raw.inflate = raw(ParamIDs::inflate);
    m_raw.push = raw(ParamIDs::push);
    m_raw.ceiling = raw(ParamIDs::ceiling);
    m_raw.morphOn = raw(ParamIDs::morphOn);
    m_raw.morphMode = raw(ParamIDs::morphMode);
    m_raw.morphX = raw(ParamIDs::morphX);
    m_raw.morphY = raw(ParamIDs::morphY);
    m_raw.morphWander = raw(ParamIDs::morphWander);
    m_raw.morphSpeed = raw(ParamIDs::morphSpeed);
    m_raw.morphShape = raw(ParamIDs::morphShape);
    m_raw.throwLength = raw(ParamIDs::throwLength);
    m_raw.play = raw(ParamIDs::play);
    m_raw.keysMode = raw(ParamIDs::keysMode);
    m_followHost.store(canFollowHost());
    for (auto& cc : m_ccOf)
        cc.store(-1);
    for (auto& v : m_ccValue)
        v.store(-1.0f);
    MorphWander::prepareTables(); // the wander shapes, before the audio thread reads them
    for (int m = 0; m < ParamIDs::numMods; ++m)
    {
        m_raw.modType[m] = raw(ParamIDs::modType[m]);
        m_raw.modShape[m] = raw(ParamIDs::modShape[m]);
        m_raw.modKnob[m] = raw(ParamIDs::modKnob[m]);
        m_raw.modFollow[m] = raw(ParamIDs::modFollow[m]);
    }
    for (const auto& id : ModMatrix::targets())
        m_modTargets.push_back({ m_state.getParameter(id), m_state.getRawParameterValue(id) });


    m_engine.setCargoHold(&m_cargo);

    // One undo history for the parameters and the track edits. The transport is not an edit.
    m_history = std::make_unique<EditHistory>(*this);
    m_history->isRecorded = [play = m_state.getParameter(ParamIDs::play)->getParameterIndex()](int index) { return index != play; };
    m_cargo.onUndoableEdit = [this](int)
    {
        m_history->add("Track Edit", [this] { m_restoredTrack = m_cargo.undoLatest(); }, [this] { m_restoredTrack = m_cargo.redoLatest(); });
    };
    m_random.setSeedRandomly();
    if (m_offline)
    {
        // An offline copy: its maker's settings (read only), no tape memory and no timer (so no auto-arming).
        m_settings = offlineSettings;
        m_followHost.store(false);
        return;
    }
    if (s_sharedSettings != nullptr)
        m_settings = s_sharedSettings;
    else
    {
        m_ownSettings = openSettings();
        m_settings = m_ownSettings.get();
    }
    m_recorder.setRetroSeconds(getRetroMinutes() * 60);
    // The learned MIDI controllers: "target:cc" pairs.
    for (const auto& pair : juce::StringArray::fromTokens(m_settings->getValue("midiControls"), ",", ""))
    {
        const int target = pair.upToFirstOccurrenceOf(":", false, false).getIntValue();
        const int cc = pair.fromFirstOccurrenceOf(":", false, false).getIntValue();
        if (pair.containsChar(':') && target >= 0 && target < c_numCcTargets && cc >= 0 && cc < 128)
            m_ccOf[static_cast<size_t>(target)].store(cc);
    }
    updateCcPump();
    startTimerHz(4);
}

SwooshbucklerAudioProcessor::~SwooshbucklerAudioProcessor()
{
    m_state.removeParameterListener(ParamIDs::stowOn, this);
    m_state.removeParameterListener(ParamIDs::stowMode, this);
    m_variations.reset(); // stops its thread and lets go of its copy of this rig first
    m_cargo.onUndoableEdit = nullptr;
    m_history.reset();
    stopTimer();
    m_ccPump.stopTimer();
    m_recorder.stop();
}

bool SwooshbucklerAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::stereo() && out != juce::AudioChannelSet::mono())
        return false;
    return layouts.getMainInputChannelSet().isDisabled();
}

void SwooshbucklerAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    m_deviceRate = sampleRate;
    m_trimGain.reset(sampleRate, 0.03); // Master Trim glides over 30 ms
    {
        const float trimDb = m_raw.masterTrim != nullptr ? m_raw.masterTrim->load() : 0.0f;
        m_trimGain.setCurrentAndTargetValue(trimDb <= -59.9f ? 1.0e-4f : juce::Decibels::decibelsToGain(trimDb));
    }
    m_blockSize = juce::jmax(256, samplesPerBlock);
    m_mods.prepare(sampleRate); // the modulators run once per device block
    m_outRing.prepare(sampleRate);
    // A host that prepares again and plays straight on (a bounce) is a transport start too: the ride begins from its
    // seed. No key is held across it either (its note-off may never come).
    m_hostWasPlaying = false;
    m_keysDown = {};
    m_audition.prepare(sampleRate);
    prepareInternal();
}

void SwooshbucklerAudioProcessor::prepareInternal()
{
    // The rig renders at the recording rate when that is higher than the device rate, so a 96 kHz
    // recording really has content up to 48 kHz (and 96 kHz samples keep their top end, like a
    // 96 kHz Reaper project). What you hear is that render low-passed under the device's Nyquist
    // frequency and downsampled, then held to the Ceiling again.
    const double recordRate = static_cast<double>(getRecordRate());
    m_renderRate = recordRate > m_deviceRate + 0.5 ? recordRate : m_deviceRate;
    const double ratio = m_renderRate / m_deviceRate;

    updateEngineParams();
    m_engine.prepare(m_renderRate, m_blockSize);
    m_master.prepare(m_renderRate, 2);
    if (! m_offline)
        m_recorder.prepare(m_renderRate, 2);
    m_work.setSize(2, m_blockSize);

    // Latency: the engine and the limiter (in device samples), and when rendering high, the input the
    // downsampler needs ahead of its output plus the monitor limiter's look-ahead.
    double latency = (static_cast<double>(m_engine.getLatencySamples()) + m_master.getLatencySamples()) / ratio;
    if (ratio > 1.0)
    {
        m_down.prepare(ratio, 2);
        m_highRate.setSize(2, m_down.maxInputsFor(c_maxSection));
        m_highRate.clear();
        m_monitorLimiter.prepare(m_deviceRate, 2);
        latency += m_down.getLatencySamples() + m_monitorLimiter.getLatencySamples();
    }
    setLatencySamples(juce::roundToInt(latency));
}

void SwooshbucklerAudioProcessor::renderAtRenderRate(const float* inL, const float* inR, float* l, float* r, int n)
{
    const bool playing = m_playing.load();
    m_engine.setReleased(! playing); // Stop: the sources let go, the echoes and Space ring out
    m_engine.process(inL, inR, l, r, n);
    // The pass peaks and hits of this block, for the take's markers: they reach the tape after the limiter's look-ahead.
    static_assert(static_cast<int>(RigEngine::MarkPassA) == static_cast<int>(TapeRecorder::MarkPassA)
                  && static_cast<int>(RigEngine::MarkHit) == static_cast<int>(TapeRecorder::MarkHit)
                  && static_cast<int>(RigEngine::MarkPassStow) == static_cast<int>(TapeRecorder::MarkPassStow), "the same marker kinds");
    for (int i = 0; i < m_engine.getNumMarkers() && ! m_offline; ++i)
        m_recorder.addMarker(m_engine.getMarker(i).offset + m_master.getLatencySamples(), m_engine.getMarker(i).kind);
    const bool throwing = m_engine.isThrowing(); // a throw with the rig stopped: its tail is waited for afterwards

    // Inflate, then Push (drive into the limiter; the ceiling stays put), then the limiter.
    float* chans[2] = { l, r };
    m_master.process(chans, 2, n);

    // Transport, ahead of the tape so a take starts and ends as cleanly as what you hear. Stopped, the tail rings
    // until it has stayed under the floor for 100 ms (not before the sources have gone) or the time is up.
    float target = 1.0f;
    if (playing || throwing)
    {
        m_tailElapsed = m_tailQuiet = 0;
        m_tailDone = false;
    }
    else if (! m_tailDone)
    {
        float peak = 0.0f;
        for (int i = 0; i < n; ++i)
            peak = juce::jmax(peak, std::abs(l[i]), std::abs(r[i]));
        m_tailElapsed += n;
        m_tailQuiet = peak < juce::Decibels::decibelsToGain(c_tailFloorDb) ? m_tailQuiet + n : 0;
        m_tailDone = static_cast<double>(m_tailElapsed) > c_tailMaxSeconds * m_renderRate
                  || (static_cast<double>(m_tailElapsed) > 0.15 * m_renderRate && static_cast<double>(m_tailQuiet) > 0.1 * m_renderRate);
    }
    if (m_tailDone)
        target = 0.0f;
    if (! m_offline)
        m_recorder.setGateHold(! playing && ! m_tailDone); // Skip Silence doesn't cut the tail short
    if (m_transportGain != target || target < 1.0f)
    {
        // Up in 60 ms. Down in 250 ms: usually the tail is already inaudible, but one that outlasts the time
        // limit is faded rather than chopped.
        const float step = static_cast<float>(1.0 / ((target > 0.0f ? 0.06 : 0.25) * juce::jmax(1.0, m_renderRate)));
        for (int i = 0; i < n; ++i)
        {
            m_transportGain = target > m_transportGain ? juce::jmin(target, m_transportGain + step)
                                                       : juce::jmax(target, m_transportGain - step);
            l[i] *= m_transportGain;
            r[i] *= m_transportGain;
        }
    }
    if (! m_offline)
        m_recorder.process(chans, 2, n); // the tape gets the full-rate signal
}

void SwooshbucklerAudioProcessor::releaseResources()
{
    m_engine.letGoOfCargo(); // the audio has stopped: nothing reads the tracks' sample data now
}

float SwooshbucklerAudioProcessor::modValue(std::atomic<float>* raw)
{
    const float value = raw->load();
    for (size_t i = 0; i < m_modTargets.size(); ++i)
    {
        if (m_modTargets[i].raw != raw)
            continue;
        const int t = static_cast<int>(i);
        const float offset = m_mods.offset(t);
        const bool wired = m_mods.isWired(t);
        if (! wired && offset == 0.0f)
            return value;
        auto* param = m_modTargets[i].param;
        const float norm = juce::jlimit(0.0f, 1.0f, param->convertTo0to1(value) + offset);
        if (wired)
            m_mods.publishTarget(t, norm);
        return param->convertFrom0to1(norm);
    }
    return value;
}

void SwooshbucklerAudioProcessor::publishModViews()
{
    for (size_t i = 0; i < m_modTargets.size(); ++i)
    {
        const int t = static_cast<int>(i);
        if (m_mods.isWired(t))
            m_mods.publishTarget(t, juce::jlimit(0.0f, 1.0f, m_modTargets[i].param->convertTo0to1(m_modTargets[i].raw->load()) + m_mods.offset(t)));
    }
}

void SwooshbucklerAudioProcessor::runModulators(int numSamples)
{
    std::array<ModMatrix::SourceSettings, ModMatrix::c_numSources> settings;
    for (int m = 0; m < ModMatrix::c_numSources; ++m)
    {
        auto& set = settings[static_cast<size_t>(m)];
        set.type = static_cast<ModMatrix::Type>(juce::jlimit(0, 3, juce::roundToInt(m_raw.modType[m]->load())));
        set.shape = static_cast<ModMatrix::Shape>(juce::jlimit(0, 4, juce::roundToInt(m_raw.modShape[m]->load())));
        set.knob = m_raw.modKnob[m]->load();
        set.follow = juce::jlimit(0, ModMatrix::c_numFollowerInputs - 1, juce::roundToInt(m_raw.modFollow[m]->load()));
    }
    m_mods.process(settings, numSamples, m_followerLevels);
}

void SwooshbucklerAudioProcessor::updateEngineParams()
{
    static_assert(ParamIDs::numCargoTracks == CargoHold::c_numTracks, "one level parameter per track");

    RigEngine::Params p;
    for (size_t i = 0; i < p.levels.size(); ++i)
    {
        p.levels[i] = Params::faderGain(m_raw.tracks[i]->load());
        // Semitones plus cents, plus any modulator wires on the Semitones box (smooth, not stepped: a 100% wire
        // swings the whole 48 semitones), held to two octaves either way so VARI on and off reach the same extremes.
        float pitch = std::round(m_raw.trackSemis[i]->load()) + std::round(m_raw.trackCents[i]->load()) / 100.0f;
        if (const int t = m_trackPitchTarget[i]; t >= 0 && (m_mods.isWired(t) || m_mods.offset(t) != 0.0f))
        {
            pitch += 48.0f * m_mods.offset(t);
            if (m_mods.isWired(t))
                m_mods.publishTarget(t, juce::jlimit(0.0f, 1.0f, (pitch + 24.0f) / 48.0f));
        }
        p.trackPitch[i] = juce::jlimit(-24.0f, 24.0f, pitch);
        p.trackVari[i] = m_raw.trackVari[i]->load() > 0.5f;
    }
    // Knobs that glide on springs in the engine get their own value plus the modulators' offset separately, so the
    // engine can add the offset after the glide (a fast modulator would otherwise be smoothed away).
    auto modDelta = [this](std::atomic<float>* raw) { return modValue(raw) - raw->load(); };
    p.pitch = m_raw.pitch->load();
    p.pitchMod = modDelta(m_raw.pitch);
    p.rideOn = m_raw.rideOn->load() > 0.5f;
    p.followMaster = m_raw.rideFollowMaster->load() > 0.5f;
    p.energy = m_raw.energy->load();
    p.energyMod = modDelta(m_raw.energy);
    p.rate = modValue(m_raw.rate);
    p.fill = m_raw.fill->load();
    p.fillMod = modDelta(m_raw.fill);
    p.chaos = modValue(m_raw.chaos);
    for (int b = 0; b < RigEngine::c_maxBuses; ++b)
    {
        auto get = [&](int k) { return m_raw.bus[b][k] != nullptr ? m_raw.bus[b][k]->load() : 0.0f; };
        for (int k = 0; k < ParamIDs::numBusControls; ++k)
            p.busMod[static_cast<size_t>(b)][static_cast<size_t>(k)] = m_raw.bus[b][k] != nullptr ? modDelta(m_raw.bus[b][k]) : 0.0f;
        auto& d = p.buses[static_cast<size_t>(b)];
        d.doppler = get(ParamIDs::Doppler);
        d.width = get(ParamIDs::Width);
        d.bend = get(ParamIDs::Bend);
        d.low = get(ParamIDs::Low);
        d.swirl = get(ParamIDs::Swirl);
        d.filter = get(ParamIDs::Filter);
        d.drive = get(ParamIDs::Drive);
        d.duck = get(ParamIDs::Duck);
        d.key = get(ParamIDs::Key);
        d.passMode = juce::jlimit(0, 1, juce::roundToInt(m_raw.passMode[b]->load()));
    }
    {
        static_assert(static_cast<int>(RigEngine::c_stowModes) == static_cast<int>(ParamIDs::numStowModes)
                      && static_cast<int>(RigEngine::c_stowKnobs) == static_cast<int>(ParamIDs::numStowControls)
                      && static_cast<int>(RigEngine::StowWidth) == static_cast<int>(ParamIDs::StowWidth), "the same Stowaway modes and knobs");
        p.stow.on = isStowawayOn();
        p.stow.mode = stowMode();
        float* const knobs[ParamIDs::numStowControls] = { &p.stow.follow, &p.stow.low, &p.stow.filter, &p.stow.drive, &p.stow.width };
        for (int k = 0; k < ParamIDs::numStowControls; ++k)
        {
            *knobs[k] = m_raw.stow[k]->load();
            p.stowMod[static_cast<size_t>(k)] = modDelta(m_raw.stow[k]);
        }
        m_cargo.setRumbleToStowaway(p.stow.on && p.stow.mode == ParamIDs::StowSub);
    }
    p.hitSense = modValue(m_raw.hitSense);
    p.hitBoost = modValue(m_raw.hitBoost);
    p.hitRelease = modValue(m_raw.hitRelease);
    p.hitDuck = modValue(m_raw.hitDuck);
    p.hitSync = m_raw.hitSync->load() > 0.5f;
    p.gateMode = juce::jlimit(0, 3, juce::roundToInt(m_raw.gateMode->load()));
    p.hitHold = modValue(m_raw.hitHold);
    p.hitFloor = modValue(m_raw.hitFloor);
    p.duckMode = juce::jlimit(0, 2, juce::roundToInt(m_raw.duckMode->load()));
    p.duckReturn = modValue(m_raw.duckReturn);
    p.takeTurns = m_raw.takeTurns->load() > 0.5f;
    p.linkBuses = m_raw.linkBuses->load() > 0.5f;
    p.hitAim = m_raw.hitAim->load() > 0.5f;
    p.tremOn = m_raw.tremOn->load() > 0.5f;
    p.tremDepth = modValue(m_raw.tremDepth);
    p.tremRate = modValue(m_raw.tremRate);
    p.tremThreshold = modValue(m_raw.tremThreshold);
    p.tremShape = juce::jlimit(0, 2, juce::roundToInt(m_raw.tremShape->load()));
    p.tremTarget = juce::jlimit(0, 2, juce::roundToInt(m_raw.tremTarget->load()));
    p.space = m_raw.space->load();
    p.spaceMod = modDelta(m_raw.space);
    p.morphOn = m_raw.morphOn->load() > 0.5f;
    p.morphMode = juce::roundToInt(m_raw.morphMode->load());
    // The morph dot: the X / Y controls, or where the wander has flown it (see runWander).
    p.morphX = m_morphX;
    p.morphY = m_morphY;
    p.throwSeconds = m_raw.throwLength->load();

    // MIDI keys. Swell: any held key holds Swell. Pitch: the held keys set pitches. Throw: the last chord's keys do.
    const int keys = keysMode();
    const auto& pitchKeys = keys == KeysThrow ? m_keysLatched : m_keysDown;
    if (keys != KeysSwell)
        for (int note = 0; note < 128 && p.numKeys < RigEngine::c_maxKeys; ++note)
            if ((pitchKeys[static_cast<size_t>(note >> 6)] >> (note & 63)) & 1u)
                p.keys[static_cast<size_t>(p.numKeys++)] = static_cast<float>(note - 60); // from middle C
    for (size_t half = 0; half < 2; ++half)
        m_keysView[half].store(keys == KeysSwell ? m_keysDown[half] : pitchKeys[half], std::memory_order_relaxed);
    p.swell = m_uiSwell.load() || (keys == KeysSwell && (m_keysDown[0] | m_keysDown[1]) != 0);

    m_engine.setParams(p);

    MasterProcessor::Parameters mp;
    // Inflate: the first 70% blends the effect in; the last 30% moves its curve from neutral to its hardest.
    const float inflate = modValue(m_raw.inflate);
    mp.inflateEffect = juce::jmin(1.0f, inflate / 0.7f);
    mp.inflateCurve = juce::jlimit(0.0f, 1.0f, (inflate - 0.7f) / 0.3f);
    mp.limiterOn = true;
    mp.ceilingDb = modValue(m_raw.ceiling);
    mp.pushDb = modValue(m_raw.push);
    // Programme-dependent release: a short over (a hit) is let go in a few tens of milliseconds, so it leaves no
    // hole; reduction that lasts is held steadily and let go over a second, so a loud swell keeps its shape
    // instead of being levelled and bass is not modulated cycle by cycle.
    mp.releaseMs = 20.0f;
    mp.sustainReleaseMs = 1000.0f;
    m_master.setParameters(mp);

    // The limiter after the downsampler (only run when rendering above the device rate): the Ceiling, nothing else.
    MasterProcessor::Parameters monitor;
    monitor.limiterOn = true;
    monitor.ceilingDb = mp.ceilingDb;
    monitor.releaseMs = 50.0f;
    m_monitorLimiter.setParameters(monitor);
}

void SwooshbucklerAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    if (n == 0)
        return;

    // The Play parameter moved (host automation, or a restored session): start or stop.
    {
        // Only a change of the parameter acts, and only if the rig isn't there already (setPlaying moves the rig
        // first and the parameter after it).
        const bool wanted = m_raw.play->load() > 0.5f;
        if (wanted != m_playParamWas)
        {
            m_playParamWas = wanted;
            if (m_playing.load() != wanted)
            {
                if (wanted)
                    m_restartPending.store(true);
                m_playing.store(wanted);
            }
        }
    }
    // Follow Host: the rig starts with the host's transport (from the seed, so a bounce repeats) and stops with it.
    // Only a change of the transport acts, so Play still starts and stops the rig by hand in between.
    {
        bool hostPlaying = false;
        if (auto* playHead = getPlayHead())
            if (const auto position = playHead->getPosition())
                hostPlaying = position->getIsPlaying();
        if (hostPlaying != m_hostWasPlaying)
        {
            m_hostWasPlaying = hostPlaying;
            if (m_followHost.load())
            {
                m_playing.store(hostPlaying);
                if (hostPlaying)
                {
                    m_reseedPending = true;
                    m_restartPending.store(false);
                }
                else
                    m_hostStopped.store(true); // the Play parameter is put back on the message thread
            }
        }
    }

    // A throw wakes a resting rig: from the button or the F key, or a key played with Keys set to Throw.
    const int mode = keysMode();
    bool throwWanted = m_uiThrows.load() > 0;
    if (mode == KeysThrow && ! throwWanted)
        for (const auto meta : midi)
            throwWanted = throwWanted || meta.getMessage().isNoteOn();
    m_sinceMidiThrow = juce::jmin(1 << 30, m_sinceMidiThrow + n);

    // Stopped and rung out: rest (the engine isn't run, so nothing starts blasting when the app opens).
    const bool playing = m_playing.load();
    if (playing || m_transportGain > 0.0f || throwWanted)
        m_drainLeft = getLatencySamples() + 64;
    else if (m_drainLeft > 0)
        m_drainLeft -= n;
    if (! playing && m_transportGain <= 0.0f && m_drainLeft <= 0)
    {
        // The tape isn't fed while the rig rests: Skip Silence's gate closes here, not at the next Play.
        if (! m_tapeRested)
            m_tapeRested = m_offline || m_recorder.rest();
        // Keep the modulator scopes, knob dots and pulled-wire glides moving while stopped (the output is silent).
        m_followerLevels = {};
        runModulators(n);
        publishModViews();
        runWander(n);
        // The keys still count while stopped: a key released can't leave Swell stuck on, and the keys held when
        // Play is pressed already set the pitches.
        for (const auto meta : midi)
            handleMidi(meta.getMessage(), 0, false);
        for (size_t half = 0; half < 2; ++half)
            m_keysView[half].store(mode == KeysThrow ? m_keysLatched[half] : m_keysDown[half], std::memory_order_relaxed);
        buffer.clear();
        m_audition.process(buffer, 0, n); // a take being auditioned plays whether the rig does or not
        publishOutputPeak(0.0f, n);
        return;
    }

    m_tapeRested = false;
    if (m_reseedPending)
    {
        // The transport started: the engine, the modulators and the wander all begin again from the same place.
        m_reseedPending = false;
        m_restartPending.store(false);
        m_engine.restartSeeded();
        m_mods.reset();
        m_wanderOn = false; // the wander sets off again from where the X / Y controls are
        m_wanderBlend = 0.0f;
        m_morphX = m_raw.morphX->load();
        m_morphY = m_raw.morphY->load();
    }
    else if (m_restartPending.exchange(false))
        m_engine.restart();
    if (m_uiThrows.exchange(0) > 0)
        m_engine.throwWhoosh(0, -1.0f); // as close as the bus draws it

    // Hosts may send more than the prepared block size (offline / freewheel). Never reallocate here: work in
    // sections of the prepared size instead.
    // Sections are also capped so the modulators (and the parameters they move) update at least every 512 samples,
    // so an offline bounce with huge blocks sounds like realtime playback.
    const int maxSection = juce::jmin(m_work.getNumSamples(), c_maxSection);
    if (maxSection <= 0)
    {
        buffer.clear();
        return;
    }

    const int inputChannels = getTotalNumInputChannels();
    const int outChannels = buffer.getNumChannels();
    float* outL = m_work.getWritePointer(0);
    float* outR = m_work.getWritePointer(1);
    for (int start = 0; start < n; start += maxSection)
    {
        const int len = juce::jmin(maxSection, n - start);
        runModulators(len);
        runWander(len);

        // The keys of this section (a thrown whoosh starts at its note's own sample).
        for (const auto meta : midi)
            if (meta.samplePosition >= start && meta.samplePosition < start + len)
                handleMidi(meta.getMessage(), meta.samplePosition - start, true);
        updateEngineParams();

        const float* inL = inputChannels > 0 ? buffer.getReadPointer(0, start) : nullptr;
        const float* inR = inputChannels > 1 ? buffer.getReadPointer(1, start) : nullptr;

        if (m_renderRate <= m_deviceRate + 0.5)
        {
            renderAtRenderRate(inL, inR, outL, outR, len);
        }
        else
        {
            // Render exactly what the downsampler needs for `len` device samples at the high rate (always the
            // same amount ahead, so the latency is constant), low-pass and downsample it, and hold the Ceiling.
            // (The audio input is at the device rate, so it isn't used as a source while rendering high.)
            const int needed = m_down.inputsNeeded(len);
            jassert(needed <= m_highRate.getNumSamples()); // sized for a whole section in prepareInternal
            float* hiL = m_highRate.getWritePointer(0);
            float* hiR = m_highRate.getWritePointer(1);
            for (int pos = 0; pos < needed; pos += 256)
                renderAtRenderRate(nullptr, nullptr, hiL + pos, hiR + pos, juce::jmin(256, needed - pos));
            const float* in[2] = { hiL, hiR };
            float* out[2] = { outL, outR };
            m_down.process(in, out, len);
            m_monitorLimiter.process(out, 2, len);
        }

        // Master Trim (what you hear; the tape above already has the full-level signal).
        double preTrimSquares = 0.0;
        float preTrimPeak = 0.0f;
        {
            const float trimDb = m_raw.masterTrim->load();
            m_trimGain.setTargetValue(trimDb <= -59.9f ? 1.0e-4f : juce::Decibels::decibelsToGain(trimDb));
            preTrimSquares = 0.0;
            for (int i = 0; i < len; ++i)
            {
                // The Output Follower hears the rig before the trim, so turning Trim down never changes the sound.
                preTrimSquares += 0.5 * (static_cast<double>(outL[i]) * outL[i] + static_cast<double>(outR[i]) * outR[i]);
                preTrimPeak = juce::jmax(preTrimPeak, std::abs(outL[i]), std::abs(outR[i])); // the OUT meter: what the tape gets
                const float trim = m_trimGain.getNextValue();
                const float g = trim <= 1.0e-4f ? 0.0f : trim;
                outL[i] *= g;
                outR[i] *= g;
            }
        }

        // Output
        if (outChannels >= 2)
        {
            buffer.copyFrom(0, start, m_work, 0, 0, len);
            buffer.copyFrom(1, start, m_work, 1, 0, len);
            for (int ch = 2; ch < outChannels; ++ch)
                buffer.clear(ch, start, len);
        }
        else if (outChannels == 1)
        {
            buffer.copyFrom(0, start, m_work, 0, 0, len);
            buffer.addFrom(0, start, m_work, 1, 0, len);
            buffer.applyGain(0, start, len, 0.5f);
        }

        // Scope + meter
        int w = m_scopeWrite.load(std::memory_order_relaxed);
        for (int i = 0; i < len; ++i)
        {
            const float v = 0.5f * (outL[i] + outR[i]);
            m_outRing.push(v);
            m_scope[static_cast<size_t>(w)] = v;
            w = (w + 1) & (c_scopeSize - 1);
        }
        m_scopeWrite.store(w, std::memory_order_release);
        m_followerLevels[ModMatrix::FollowOutput] = static_cast<float>(std::sqrt(preTrimSquares / juce::jmax(1, len)));
        const auto sections = m_engine.takeSectionLevels();
        m_followerLevels[ModMatrix::FollowBusA] = sections[0];
        m_followerLevels[ModMatrix::FollowBusB] = sections[1];
        m_followerLevels[ModMatrix::FollowHits] = sections[2];
        m_followerLevels[ModMatrix::FollowStow] = sections[RigEngine::SectionStow];
        const auto& master = m_engine.getMasterFollow();
        m_followerLevels[ModMatrix::FollowMaster] = master[0];
        m_followerLevels[ModMatrix::FollowMasterPitch] = master[1];
        m_followerLevels[ModMatrix::FollowMasterBright] = master[2];
        publishOutputPeak(preTrimPeak, len);
    }
    // A take being auditioned: on top of the output, after the tape, the meters and the trim.
    m_audition.process(buffer, 0, n);
}

int SwooshbucklerAudioProcessor::stowMode() const
{
    return juce::jlimit(0, ParamIDs::numStowModes - 1, juce::roundToInt(m_raw.stowMode->load()));
}

bool SwooshbucklerAudioProcessor::isStowawayOn() const
{
    return m_raw.stowOn->load() > 0.5f;
}

void SwooshbucklerAudioProcessor::setStowawayOn(bool on)
{
    if (auto* param = m_state.getParameter(ParamIDs::stowOn); param != nullptr && (param->getValue() > 0.5f) != on)
    {
        param->beginChangeGesture();
        param->setValueNotifyingHost(on ? 1.0f : 0.0f);
        param->endChangeGesture();
    }
}

void SwooshbucklerAudioProcessor::parameterChanged(const juce::String& parameterID, float newValue)
{
    // The one that changed is read from newValue, the other from the parameter.
    if (parameterID == ParamIDs::stowMode)
    {
        m_cargo.setRumbleToStowaway(isStowawayOn() && juce::roundToInt(newValue) == ParamIDs::StowSub);
        // Filter and Width read by the Mode ("120 Hz" or "Open", "Mono" or "100%"): the host is told to read its
        // parameters' texts again. That has to be said on the message thread, so the timer says it.
        m_stowReadoutsStale.store(true);
    }
    else if (parameterID == ParamIDs::stowOn)
        m_cargo.setRumbleToStowaway(newValue > 0.5f && stowMode() == ParamIDs::StowSub);
}

void SwooshbucklerAudioProcessor::refreshHostReadouts()
{
    // Stowaway's Mode changed: its knobs read differently now, and the host shows what they read.
    if (m_stowReadoutsStale.exchange(false))
        updateHostDisplay(ChangeDetails().withParameterInfoChanged(true));
}

int SwooshbucklerAudioProcessor::keysMode() const
{
    return juce::jlimit(0, 2, juce::roundToInt(m_raw.keysMode->load()));
}

void SwooshbucklerAudioProcessor::handleMidi(const juce::MidiMessage& msg, int offsetSamples, bool mayThrow)
{
    auto bit = [](int note) { return std::uint64_t { 1 } << (note & 63); };
    if (msg.isNoteOn())
    {
        const int note = juce::jlimit(0, 127, msg.getNoteNumber());
        const auto half = static_cast<size_t>(note >> 6);
        const bool first = (m_keysDown[0] | m_keysDown[1]) == 0;
        m_keysDown[half] |= bit(note);
        // Throw: a key played from nothing starts a new chord; keys added while others are held join it.
        if (first)
            m_keysLatched = {};
        m_keysLatched[half] |= bit(note);
        if (mayThrow && keysMode() == KeysThrow)
        {
            // Keys that land together (within 30 ms: a chord) are one whoosh, with their pitches dealt over the tracks.
            if (static_cast<double>(m_sinceMidiThrow) > 0.03 * m_deviceRate)
            {
                const double toRender = m_renderRate / juce::jmax(1.0, m_deviceRate);
                m_engine.throwWhoosh(juce::roundToInt(offsetSamples * toRender), msg.getFloatVelocity());
            }
            m_sinceMidiThrow = 0;
        }
    }
    else if (msg.isNoteOff())
    {
        const int note = juce::jlimit(0, 127, msg.getNoteNumber());
        m_keysDown[static_cast<size_t>(note >> 6)] &= ~bit(note);
    }
    else if (msg.isAllNotesOff() || msg.isAllSoundOff())
        m_keysDown = {};
    else if (msg.isController() && msg.getControllerNumber() < 120) // 120 and up are channel mode messages, not controls
    {
        // A learned controller: its position is left for the message thread, which moves the parameter.
        const int cc = msg.getControllerNumber();
        int learn = m_ccLearn.load(std::memory_order_relaxed);
        if (learn >= 0 && learn < c_numCcTargets && m_ccLearn.compare_exchange_strong(learn, -1))
        {
            m_ccOf[static_cast<size_t>(learn)].store(cc, std::memory_order_relaxed);
            m_ccLearned.store(learn, std::memory_order_relaxed);
        }
        const float value = static_cast<float>(msg.getControllerValue()) / 127.0f;
        for (size_t t = 0; t < static_cast<size_t>(c_numCcTargets); ++t)
            if (m_ccOf[t].load(std::memory_order_relaxed) == cc)
                m_ccValue[t].store(value, std::memory_order_relaxed);
    }
}

//==============================================================================
// MIDI control
juce::String SwooshbucklerAudioProcessor::midiTargetName(int target)
{
    return target == c_ccEnergy ? juce::String("Energy") : "Track " + juce::String(target + 1) + " Fader";
}

juce::RangedAudioParameter* SwooshbucklerAudioProcessor::midiTargetParameter(int target) const
{
    if (target < 0 || target >= c_numCcTargets)
        return nullptr;
    return m_state.getParameter(target == c_ccEnergy ? ParamIDs::energy : ParamIDs::cargoTrack[target]);
}

int SwooshbucklerAudioProcessor::getMidiControl(int target) const
{
    return target >= 0 && target < c_numCcTargets ? m_ccOf[static_cast<size_t>(target)].load(std::memory_order_relaxed) : -1;
}

void SwooshbucklerAudioProcessor::updateCcPump()
{
    bool wanted = m_ccLearn.load() >= 0;
    for (const auto& cc : m_ccOf)
        wanted = wanted || cc.load() >= 0;
    if (wanted && ! m_offline)
        m_ccPump.startTimerHz(60);
    else
        m_ccPump.stopTimer();
}

void SwooshbucklerAudioProcessor::learnMidiControl(int target)
{
    m_ccLearned.store(-1);
    m_ccLearn.store(target >= 0 && target < c_numCcTargets ? target : -1);
    updateCcPump();
}

void SwooshbucklerAudioProcessor::saveMidiControls()
{
    juce::StringArray pairs;
    for (int t = 0; t < c_numCcTargets; ++t)
        if (const int cc = getMidiControl(t); cc >= 0)
            pairs.add(juce::String(t) + ":" + juce::String(cc));
    if (m_settings != nullptr && ! m_offline)
    {
        m_settings->setValue("midiControls", pairs.joinIntoString(","));
        m_settings->saveIfNeeded();
    }
    ++m_ccGeneration;
}

void SwooshbucklerAudioProcessor::setMidiControl(int target, int ccOrMinusOne)
{
    if (target < 0 || target >= c_numCcTargets)
        return;
    m_ccOf[static_cast<size_t>(target)].store(juce::jlimit(-1, 127, ccOrMinusOne));
    m_ccValue[static_cast<size_t>(target)].store(-1.0f);
    saveMidiControls();
    updateCcPump();
}

int SwooshbucklerAudioProcessor::applyMidiControls()
{
    const int learned = m_ccLearned.exchange(-1);
    if (learned >= 0)
        saveMidiControls();
    for (int t = 0; t < c_numCcTargets; ++t)
    {
        const float value = m_ccValue[static_cast<size_t>(t)].exchange(-1.0f, std::memory_order_relaxed);
        if (value < 0.0f)
            continue;
        // Like host automation: the control moves, and it is not an undo step.
        if (auto* param = midiTargetParameter(t); param != nullptr && std::abs(param->getValue() - value) > 1.0e-6f)
            param->setValueNotifyingHost(value);
    }
    return learned;
}

void SwooshbucklerAudioProcessor::runWander(int numSamples)
{
    // The dot's wander, at control rate (once per section, at most 512 samples): it flies with the window closed and
    // in an offline bounce. The X / Y parameters are left alone; the wander's place is blended in over them (over
    // about 50 ms each way, so switching it on or off never jumps) and published for the pad to draw.
    const bool on = m_raw.morphWander->load() > 0.5f;
    const int shape = juce::roundToInt(m_raw.morphShape->load());
    const float x = m_raw.morphX->load(), y = m_raw.morphY->load();
    const float seconds = static_cast<float>(numSamples) / static_cast<float>(juce::jmax(1.0, m_deviceRate));
    if (on && ! m_wanderOn)
        m_wander.join(shape, m_morphX, m_morphY); // from where the dot is
    m_wanderOn = on;
    if (on)
        m_wander.step(seconds, shape, m_raw.morphSpeed->load());
    m_wanderBlend += ((on ? 1.0f : 0.0f) - m_wanderBlend) * (1.0f - std::exp(-seconds / 0.05f));
    if (std::abs((on ? 1.0f : 0.0f) - m_wanderBlend) < 1.0e-3f)
        m_wanderBlend = on ? 1.0f : 0.0f;
    m_morphX = x + m_wanderBlend * (m_wander.getX() - x);
    m_morphY = y + m_wanderBlend * (m_wander.getY() - y);
    m_morphViewX.store(m_morphX, std::memory_order_relaxed);
    m_morphViewY.store(m_morphY, std::memory_order_relaxed);
    m_morphViewRotation.store(on ? m_wander.getRotation() : 0.0f, std::memory_order_relaxed);
}

void SwooshbucklerAudioProcessor::setPlaying(bool on)
{
    if (on && ! m_playing.load())
        m_restartPending.store(true);
    m_playing.store(on);
    // The Play parameter follows (so a host sees and saves it). The rig is already there, so the audio thread has
    // nothing left to do when it sees the parameter move. Off the message thread the parameter is left alone: it
    // only says what was asked last.
    if (juce::MessageManager::existsAndIsCurrentThread())
        if (auto* param = m_state.getParameter(ParamIDs::play); param != nullptr && (param->getValue() > 0.5f) != on)
        {
            param->beginChangeGesture();
            param->setValueNotifyingHost(on ? 1.0f : 0.0f);
            param->endChangeGesture();
        }
}

void SwooshbucklerAudioProcessor::publishOutputPeak(float peak, int numSamples)
{
    // The meter falls back by time (20 dB in about 0.7 s), not by block, so it moves the same at any buffer size.
    const float decay = std::exp(-static_cast<float>(numSamples) / (0.3f * static_cast<float>(juce::jmax(1.0, m_deviceRate))));
    m_outputPeak.store(juce::jmax(peak, m_outputPeak.load(std::memory_order_relaxed) * decay), std::memory_order_relaxed);

    // The peak-hold's maximum since the meter last read it. The reader may reset it between this load and store;
    // the worst that does is show one peak for one more reading.
    if (peak > m_outputPeakHold.load(std::memory_order_relaxed))
        m_outputPeakHold.store(peak, std::memory_order_relaxed);
}

void SwooshbucklerAudioProcessor::copyScope(float* dest, int count) const
{
    count = juce::jmin(count, c_scopeSize);
    const int w = m_scopeWrite.load(std::memory_order_acquire);
    for (int i = 0; i < count; ++i)
        dest[i] = m_scope[static_cast<size_t>((w - count + i) & (c_scopeSize - 1))];
}

//==============================================================================
void SwooshbucklerAudioProcessor::newSession()
{
    setPlaying(false);
    if (isRecording())
        stopRecording();

    // Everything below is one undo step: the tracks (each clear is a track edit), the parameters, and what
    // neither of those covers, put back by hand.
    struct Extras
    {
        juce::ValueTree wires;
        std::array<bool, CargoHold::c_numLanes> mute {}, solo {};
        juce::String sessionPath, presetName;
        unsigned locks = 0;
        int stowPanel = -1;
        std::array<Look, 2> ab;
        std::array<juce::String, 2> abNames;
        int abSlot = 0;
    };
    auto capture = [this]
    {
        Extras e;
        e.wires = m_mods.toValueTree();
        for (int lane = 0; lane < CargoHold::c_numLanes; ++lane)
        {
            e.mute[static_cast<size_t>(lane)] = m_cargo.getMute(lane);
            e.solo[static_cast<size_t>(lane)] = m_cargo.getSolo(lane);
        }
        e.presetName = currentPresetName;
        e.locks = m_lockedSections.load();
        e.stowPanel = m_stowPanel.load();
        const juce::ScopedLock sl(m_stateLock);
        e.sessionPath = m_sessionPath;
        e.ab = m_ab;
        e.abNames = m_abNames;
        e.abSlot = m_abSlot;
        return e;
    };
    auto restore = [this](const Extras& e)
    {
        m_mods.fromValueTree(e.wires);
        for (int lane = 0; lane < CargoHold::c_numLanes; ++lane)
        {
            m_cargo.setMute(lane, e.mute[static_cast<size_t>(lane)]);
            m_cargo.setSolo(lane, e.solo[static_cast<size_t>(lane)]);
        }
        currentPresetName = e.presetName;
        m_lockedSections.store(e.locks);
        m_stowPanel.store(e.stowPanel);
        const juce::ScopedLock sl(m_stateLock);
        m_sessionPath = e.sessionPath;
        m_ab = e.ab;
        m_abNames = e.abNames;
        m_abSlot = e.abSlot;
    };

    EditHistory::Group group(*m_history, "New Session");
    const auto before = capture();
    for (int t = 0; t < CargoHold::c_numTracks; ++t)
    {
        m_cargo.clear(t);
        m_cargo.rename(t, {});
        m_cargo.setRoleOverride(t, -1);
        m_cargo.setStart(t, 0.0f);
    }
    for (int lane = 0; lane < CargoHold::c_numLanes; ++lane)
    {
        m_cargo.setMute(lane, false);
        m_cargo.setSolo(lane, false);
    }
    for (auto* p : getParameters())
        if (auto* param = dynamic_cast<juce::RangedAudioParameter*>(p))
        {
            param->beginChangeGesture();
            param->setValueNotifyingHost(param->getDefaultValue());
            param->endChangeGesture();
        }
    m_mods.clear();
    currentPresetName = "Default";
    m_lockedSections.store(0);
    m_stowPanel.store(-1);
    {
        const juce::ScopedLock sl(m_stateLock);
        m_sessionPath.clear();
        m_ab = {};
        m_abNames = {};
        m_abSlot = 0;
    }
    m_rolls.clear();
    m_rollAt = -1;
    const auto after = capture();
    m_history->add("New Session", [restore, before] { restore(before); }, [restore, after] { restore(after); });
}

//==============================================================================
// Keeping work: undo, presets, Randomise, A/B
bool SwooshbucklerAudioProcessor::undoEdit(juce::String* what)
{
    m_restoredTrack = -1;
    return m_history->undo(what);
}

bool SwooshbucklerAudioProcessor::redoEdit(juce::String* what)
{
    m_restoredTrack = -1;
    return m_history->redo(what);
}

SwooshbucklerAudioProcessor::Look SwooshbucklerAudioProcessor::captureLook(bool withMods, bool outputStage) const
{
    Look look;
    for (auto* p : getParameters())
        if (auto* param = dynamic_cast<juce::RangedAudioParameter*>(p))
        {
            const auto id = param->getParameterID();
            if (Presets::isSessionParameter(id) && ! ((withMods && id.startsWith("mod")) || (outputStage && Presets::isOutputStageRoll(id))))
                continue;
            look[id] = param->convertFrom0to1(param->getValue());
        }
    return look;
}

void SwooshbucklerAudioProcessor::setPresetName(const juce::String& name)
{
    if (name == currentPresetName)
        return;
    const auto before = currentPresetName;
    currentPresetName = name;
    m_history->add("Preset", [this, before] { currentPresetName = before; }, [this, name] { currentPresetName = name; });
}

void SwooshbucklerAudioProcessor::applyLook(const Presets::Preset& look, Presets::Unlisted unlisted, const juce::String& undoName,
                                            const juce::String& presetName, bool outputStage)
{
    EditHistory::Group group(*m_history, undoName);
    if (presetName.isNotEmpty())
        setPresetName(presetName);
    for (const auto& [param, target] : Presets::targets(getParameters(), look, unlisted, outputStage))
    {
        if (std::abs(param->getValue() - target) < 1.0e-6f)
            continue;
        param->beginChangeGesture();
        param->setValueNotifyingHost(target);
        param->endChangeGesture();
    }
}

Presets::Preset SwooshbucklerAudioProcessor::rollLook(int section)
{
    // The stop you are on keeps how you left it; anything after it is forgotten, as in any history.
    auto now = captureLook(false, true);
    const int from = m_rollAt;
    if (m_rolls.empty() || m_rollAt < 0)
    {
        m_rolls.clear();
        m_rolls.push_back({ now, currentPresetName });
        m_rollAt = 0;
    }
    else
    {
        m_rolls[static_cast<size_t>(m_rollAt)] = { now, currentPresetName };
        m_rolls.resize(static_cast<size_t>(m_rollAt) + 1);
    }
    auto roll = section < 0 ? Presets::roll(m_random, getLockedSections()) : Presets::rollSection(m_random, section);
    for (const auto& [id, value] : roll.values)
        now[id] = value;
    m_rolls.push_back({ std::move(now), roll.name });
    ++m_rollAt;
    int shift = 0;
    if (static_cast<int>(m_rolls.size()) > c_maxRolls + 1)
    {
        m_rolls.erase(m_rolls.begin());
        --m_rollAt;
        shift = 1;
    }
    // Undoing the roll steps the history back with it.
    const int to = m_rollAt, back = from - shift;
    auto place = [this](int at) { m_rollAt = m_rolls.empty() ? -1 : juce::jlimit(0, static_cast<int>(m_rolls.size()) - 1, at); };
    m_history->add("Randomise", [place, back] { place(back); }, [place, to] { place(to); });
    return roll;
}

bool SwooshbucklerAudioProcessor::canStepRoll(int delta) const
{
    const int to = m_rollAt + delta;
    return m_rollAt >= 0 && delta != 0 && to >= 0 && to < static_cast<int>(m_rolls.size());
}

const Presets::Preset* SwooshbucklerAudioProcessor::stepRoll(int delta)
{
    if (! canStepRoll(delta))
        return nullptr;
    const int from = m_rollAt, to = m_rollAt + delta;
    m_rolls[static_cast<size_t>(from)] = { captureLook(false, true), currentPresetName };
    m_rollAt = to;
    m_rollTarget = { m_rolls[static_cast<size_t>(to)].name, m_rolls[static_cast<size_t>(to)].look };
    auto place = [this](int at) { m_rollAt = m_rolls.empty() ? -1 : juce::jlimit(0, static_cast<int>(m_rolls.size()) - 1, at); };
    m_history->add("Randomise", [place, from] { place(from); }, [place, to] { place(to); });
    return &m_rollTarget;
}

void SwooshbucklerAudioProcessor::setSectionLocked(int section, bool locked)
{
    if (section < 0 || section >= Presets::numSections || isSectionLocked(section) == locked)
        return;
    const unsigned bit = 1u << section;
    m_lockedSections.store(locked ? (m_lockedSections.load() | bit) : (m_lockedSections.load() & ~bit));
    updateHostDisplay(ChangeDetails().withNonParameterStateChanged(true)); // it is part of the session: the host and the autosave hear of it
}

juce::File SwooshbucklerAudioProcessor::getPresetFolder() const
{
    return m_settings->getFile().getSiblingFile("Swooshbuckler Presets");
}

bool SwooshbucklerAudioProcessor::saveUserPreset(const juce::String& name, bool withMods, juce::File& written, juce::String& error)
{
    const auto legal = UserPresets::legalName(name);
    if (UserPresets::isFactoryName(legal))
    {
        error = "\"" + legal + "\" is a factory preset's name";
        return false;
    }
    const Presets::Preset preset { legal, captureLook(withMods) };
    const auto file = UserPresets::fileFor(getPresetFolder(), legal);
    const auto wires = m_mods.toValueTree();
    if (! UserPresets::write(file, preset, withMods ? &wires : nullptr, error))
        return false;
    written = file;
    currentPresetName = legal;
    return true;
}

//==============================================================================
// Routing
void SwooshbucklerAudioProcessor::setMasterTrack(int trackOrMinusOne)
{
    const int before = m_cargo.getMaster();
    m_cargo.setMaster(trackOrMinusOne);
    const int after = m_cargo.getMaster();
    if (after != before)
        m_history->add("Master Track", [this, before] { m_cargo.setMaster(before); }, [this, after] { m_cargo.setMaster(after); });
}

void SwooshbucklerAudioProcessor::setTrackBus(int track, int busOrMinusOne)
{
    if (track < 0 || track >= CargoHold::c_numTracks)
        return;
    const int before = m_cargo.getBusOverride(track);
    m_cargo.setBusOverride(track, busOrMinusOne);
    const int after = m_cargo.getBusOverride(track);
    if (after != before)
        m_history->add("Track Bus", [this, track, before] { m_cargo.setBusOverride(track, before); },
                       [this, track, after] { m_cargo.setBusOverride(track, after); });
}

void SwooshbucklerAudioProcessor::setMorphSource(int side, int trackOrMinusOne)
{
    const std::array<int, 2> before { m_cargo.getMorphSource(0), m_cargo.getMorphSource(1) };
    m_cargo.setMorphSource(side, trackOrMinusOne);
    const std::array<int, 2> after { m_cargo.getMorphSource(0), m_cargo.getMorphSource(1) };
    if (after == before)
        return;
    auto place = [this](std::array<int, 2> ropes)
    {
        m_cargo.setMorphSource(0, -1);
        m_cargo.setMorphSource(1, ropes[1]);
        m_cargo.setMorphSource(0, ropes[0]);
    };
    m_history->add("Morph Rope", [place, before] { place(before); }, [place, after] { place(after); });
}

juce::String SwooshbucklerAudioProcessor::matchPitchToMaster(int track, bool* done)
{
    if (done != nullptr)
        *done = false;
    const int master = m_cargo.getMaster();
    if (master < 0)
        return "Choose a Master track first (the star on a lane).";
    if (track < 0 || track >= CargoHold::c_numTracks || track == master)
        return "This is the Master track: the others are matched to it.";
    const float masterHz = m_cargo.getMedianPitchHz(master), trackHz = m_cargo.getMedianPitchHz(track);
    if (masterHz <= 0.0f)
        return "The Master track has no clear pitch to match.";
    if (trackHz <= 0.0f)
        return "This track has no clear pitch to match.";

    // The Master as it is tuned: its own Semitones and Cents count (the Ride's Pitch and held keys move every track
    // alike). The nearest octave the two-octave range reaches is used.
    auto plain = [this](const char* id) { return m_state.getRawParameterValue(id)->load(); };
    const float masterOwn = plain(ParamIDs::trackSemis[master]) + plain(ParamIDs::trackCents[master]) / 100.0f;
    float semitones = 12.0f * std::log2(masterHz / trackHz) + masterOwn;
    while (semitones > 24.5f) semitones -= 12.0f;
    while (semitones < -24.5f) semitones += 12.0f;
    const float whole = juce::jlimit(-24.0f, 24.0f, std::round(semitones));
    const float cents = juce::jlimit(-100.0f, 100.0f, std::round((semitones - whole) * 100.0f));

    EditHistory::Group group(*m_history, "Match Pitch");
    auto set = [this](const char* id, float value)
    {
        if (auto* param = m_state.getParameter(id))
        {
            param->beginChangeGesture();
            param->setValueNotifyingHost(param->convertTo0to1(value));
            param->endChangeGesture();
        }
    };
    set(ParamIDs::trackSemis[track], whole);
    set(ParamIDs::trackCents[track], cents);
    if (done != nullptr)
        *done = true;
    const int centsInt = juce::roundToInt(cents);
    return "Matched to the Master: " + juce::String(whole > 0.0f ? "+" : "") + juce::String(juce::roundToInt(whole)) + " st"
           + (centsInt != 0 ? juce::String(centsInt > 0 ? " +" : " ") + juce::String(centsInt) + " c" : juce::String()) + ".";
}

void SwooshbucklerAudioProcessor::applyPresetWires(const UserPresets::Loaded& loaded)
{
    if (! loaded.hasMods)
        return;
    const auto before = m_mods.toValueTree();
    m_mods.fromValueTree(loaded.mods.isValid() ? loaded.mods.createCopy() : juce::ValueTree("Mods"));
    const auto after = m_mods.toValueTree();
    m_history->add("Wires", [this, before] { m_mods.fromValueTree(before); }, [this, after] { m_mods.fromValueTree(after); });
}

void SwooshbucklerAudioProcessor::switchAB()
{
    const auto now = captureLook();
    const int from = m_abSlot, to = 1 - from;
    Presets::Preset target;
    {
        const juce::ScopedLock sl(m_stateLock);
        m_ab[static_cast<size_t>(from)] = now;
        m_abNames[static_cast<size_t>(from)] = currentPresetName;
        if (m_ab[static_cast<size_t>(to)].empty()) // never used: it starts as a copy of this one
        {
            m_ab[static_cast<size_t>(to)] = now;
            m_abNames[static_cast<size_t>(to)] = currentPresetName;
        }
        target = { m_abNames[static_cast<size_t>(to)], m_ab[static_cast<size_t>(to)] };
        m_abSlot = to;
    }
    EditHistory::Group group(*m_history, to == 1 ? "Switch to B" : "Switch to A");
    auto place = [this](int slot)
    {
        const juce::ScopedLock sl(m_stateLock);
        m_abSlot = slot;
    };
    m_history->add("A/B", [place, from] { place(from); }, [place, to] { place(to); });
    applyLook(target, Presets::Unlisted::Keep, {}, target.name);
    updateHostDisplay(ChangeDetails().withNonParameterStateChanged(true));
}

void SwooshbucklerAudioProcessor::copyABToOther()
{
    const auto now = captureLook();
    {
        const juce::ScopedLock sl(m_stateLock);
        m_ab[static_cast<size_t>(1 - m_abSlot)] = now;
        m_abNames[static_cast<size_t>(1 - m_abSlot)] = currentPresetName;
    }
    updateHostDisplay(ChangeDetails().withNonParameterStateChanged(true));
}

//==============================================================================
// Session files
juce::File SwooshbucklerAudioProcessor::getSessionFile() const
{
    const juce::ScopedLock sl(m_stateLock);
    return m_sessionPath.isEmpty() ? juce::File() : juce::File(m_sessionPath);
}

bool SwooshbucklerAudioProcessor::saveSession(const juce::File& file, juce::String& error)
{
    const auto target = file.hasFileExtension(c_sessionExtension) ? file : file.withFileExtension(c_sessionExtension);
    juce::String before;
    {
        const juce::ScopedLock sl(m_stateLock);
        before = m_sessionPath;
        m_sessionPath = target.getFullPathName(); // the file says where it is
    }
    juce::MemoryBlock data;
    getStateInformation(data);
    bool ok = data.getSize() > 0 && target.getParentDirectory().createDirectory();
    if (ok)
    {
        // Written beside its final name and swapped in, so a failed save leaves the previous file intact.
        juce::TemporaryFile temp(target);
        ok = temp.getFile().replaceWithData(data.getData(), data.getSize()) && temp.overwriteTargetFileWithTemporary();
    }
    if (! ok)
    {
        const juce::ScopedLock sl(m_stateLock);
        m_sessionPath = before;
        error = "Couldn't write " + target.getFileName();
        return false;
    }
    noteRecentSession(target);
    updateHostDisplay(ChangeDetails().withNonParameterStateChanged(true));
    return true;
}

bool SwooshbucklerAudioProcessor::openSession(const juce::File& file, juce::String& error)
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    juce::MemoryBlock data;
    if (! file.existsAsFile() || ! file.loadFileAsData(data) || data.getSize() == 0 || data.getSize() >= static_cast<size_t>(0x7fffffff))
    {
        error = "Couldn't read " + file.getFileName();
        return false;
    }
    juce::ValueTree tree;
    if (! readState(data.getData(), static_cast<int>(data.getSize()), tree))
    {
        error = file.getFileName() + " isn't a Swooshbuckler session";
        return false;
    }
    data.reset();
    restoreStateTree(tree);
    {
        const juce::ScopedLock sl(m_stateLock);
        m_sessionPath = file.getFullPathName(); // where it is now, wherever it was saved
    }
    noteRecentSession(file);
    updateHostDisplay(ChangeDetails().withNonParameterStateChanged(true));
    return true;
}

juce::Array<juce::File> SwooshbucklerAudioProcessor::getRecentSessions() const
{
    juce::Array<juce::File> files;
    for (const auto& path : juce::StringArray::fromLines(m_settings->getValue("recentSessions")))
        if (path.trim().isNotEmpty() && juce::File::isAbsolutePath(path.trim()) && juce::File(path.trim()).existsAsFile())
            files.add(juce::File(path.trim()));
    return files;
}

void SwooshbucklerAudioProcessor::noteRecentSession(const juce::File& file)
{
    if (m_offline)
        return;
    auto paths = juce::StringArray::fromLines(m_settings->getValue("recentSessions"));
    paths.removeEmptyStrings();
    paths.removeString(file.getFullPathName());
    paths.insert(0, file.getFullPathName());
    while (paths.size() > 10)
        paths.remove(paths.size() - 1);
    m_settings->setValue("recentSessions", paths.joinIntoString("\n"));
    m_settings->saveIfNeeded();
}

void SwooshbucklerAudioProcessor::clearRecentSessions()
{
    m_settings->removeValue("recentSessions");
    m_settings->saveIfNeeded();
}

bool SwooshbucklerAudioProcessor::isUserEdit(int) const
{
    // Nothing moves a parameter by itself: the morph wander flies the dot inside the processor and leaves X and Y alone.
    return true;
}

bool SwooshbucklerAudioProcessor::arm(juce::String& error)
{
    m_recorder.setSignalGate(getSkipSilence());
    TapeRecorder::Options o;
    o.bitDepth = getBitDepth();
    o.fileRate = static_cast<double>(getRecordRate()); // 0 = the session rate
    o.type = static_cast<TapeRecorder::FileType>(getFileType());
    o.mono = getMono();
    o.perWhoosh = getOneFilePerWhoosh();
    refreshTakes();
    m_filesThisTake = 0;
    updateTapeNaming();
    return m_recorder.start(getRecordFolder(), o, error);
}

bool SwooshbucklerAudioProcessor::startRecording(juce::String& error)
{
    // One continuous take in one file, whatever the settings say about whooshes.
    TapeRecorder::Options o;
    o.bitDepth = getBitDepth();
    o.fileRate = static_cast<double>(getRecordRate()); // 0 = the session rate
    o.type = static_cast<TapeRecorder::FileType>(getFileType());
    o.mono = getMono();
    refreshTakes();
    m_filesThisTake = 0;
    updateTapeNaming();
    return m_recorder.start(getRecordFolder(), o, error);
}

int SwooshbucklerAudioProcessor::getRecordRate() const
{
    const int rate = m_recordRateOverride >= 0 ? m_recordRateOverride : m_settings->getIntValue("recordRate", 0);
    switch (rate)
    {
        case 44100: case 48000: case 88200: case 96000: case 176400: case 192000: return rate;
        default: return 0;
    }
}

void SwooshbucklerAudioProcessor::setRecordRate(int rate)
{
    m_settings->setValue("recordRate", rate);
    m_settings->saveIfNeeded();
    if (m_deviceRate > 0.0 && ! isRecording())
    {
        // Re-render at the new rate (the tape stays closed while it changes).
        suspendProcessing(true);
        prepareInternal();
        suspendProcessing(false);
    }
}

int SwooshbucklerAudioProcessor::getFileType() const { return juce::jlimit(0, 2, m_settings->getIntValue("fileType", 0)); }
void SwooshbucklerAudioProcessor::setFileType(int type) { m_settings->setValue("fileType", juce::jlimit(0, 2, type)); m_settings->saveIfNeeded(); }
bool SwooshbucklerAudioProcessor::getMono() const { return m_settings->getBoolValue("mono", false); }
void SwooshbucklerAudioProcessor::setMono(bool mono) { m_settings->setValue("mono", mono); m_settings->saveIfNeeded(); }

void SwooshbucklerAudioProcessor::stopRecording()
{
    m_recorder.stop();
    refreshTakes();
}

bool SwooshbucklerAudioProcessor::saveRetro(juce::File& written, juce::String& error)
{
    // The same file type, bit depth, sample rate and channels as the tape.
    TapeRecorder::Options o;
    o.bitDepth = getBitDepth();
    o.fileRate = static_cast<double>(getRecordRate()); // 0 = the session rate
    o.type = static_cast<TapeRecorder::FileType>(getFileType());
    o.mono = getMono();
    updateTapeNaming();
    if (! m_recorder.saveRetro(getRecordFolder(), o, written, error))
        return false;
    registerTake(written, getRetroMinutes() == 1 ? juce::String("Last 60 s") : "Last " + juce::String(getRetroMinutes()) + " Min");
    return true;
}

//==============================================================================
// Render Variations
VariationRenderer& SwooshbucklerAudioProcessor::getVariations()
{
    jassert(! m_offline);
    if (m_variations == nullptr)
        m_variations = std::make_unique<VariationRenderer>(*this);
    return *m_variations;
}

int SwooshbucklerAudioProcessor::getVariationCount() const { return juce::jlimit(1, VariationRenderer::c_maxCount, m_settings->getIntValue("variationCount", 100)); }
void SwooshbucklerAudioProcessor::setVariationCount(int count) { m_settings->setValue("variationCount", juce::jlimit(1, VariationRenderer::c_maxCount, count)); m_settings->saveIfNeeded(); }
int SwooshbucklerAudioProcessor::getVariationMode() const { return juce::jlimit(0, 2, m_settings->getIntValue("variationMode", 0)); }
void SwooshbucklerAudioProcessor::setVariationMode(int mode) { m_settings->setValue("variationMode", juce::jlimit(0, 2, mode)); m_settings->saveIfNeeded(); }
float SwooshbucklerAudioProcessor::getVariationAmount() const { return juce::jlimit(0.0f, 1.0f, static_cast<float>(m_settings->getDoubleValue("variationAmount", 0.3))); }
void SwooshbucklerAudioProcessor::setVariationAmount(float amount) { m_settings->setValue("variationAmount", static_cast<double>(juce::jlimit(0.0f, 1.0f, amount))); m_settings->saveIfNeeded(); }
juce::String SwooshbucklerAudioProcessor::getVariationFolder() const { return m_settings->getValue("variationFolder").trim(); }
void SwooshbucklerAudioProcessor::setVariationFolder(const juce::String& path) { m_settings->setValue("variationFolder", path.trim()); m_settings->saveIfNeeded(); }
juce::String SwooshbucklerAudioProcessor::getVariationValue(const juce::String& key, const juce::String& fallback) const { return m_settings->getValue("variation" + key, fallback); }
void SwooshbucklerAudioProcessor::setVariationValue(const juce::String& key, const juce::String& value)
{
    if (m_settings->getValue("variation" + key) == value && m_settings->containsKey("variation" + key))
        return;
    m_settings->setValue("variation" + key, value);
    m_settings->saveIfNeeded();
}

std::unique_ptr<SwooshbucklerAudioProcessor> SwooshbucklerAudioProcessor::makeOfflineCopy()
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    std::unique_ptr<SwooshbucklerAudioProcessor> copy(new SwooshbucklerAudioProcessor(m_settings));
    auto state = m_state.copyState();
    state.removeChild(state.getChildWithName("Cargo"), nullptr);
    state.removeChild(state.getChildWithName("Mods"), nullptr);
    copy->m_state.replaceState(state);
    copy->m_mods.fromValueTree(m_mods.toValueTree());
    copy->currentPresetName = currentPresetName;
    copy->m_stowPanel.store(m_stowPanel.load());
    copy->m_lockedSections.store(m_lockedSections.load()); // Randomise Everything leaves locked sections alone too
    // Stopped, and Master Trim out of the way: what it renders is what the tape would get.
    if (auto* play = copy->m_state.getParameter(ParamIDs::play))
        play->setValueNotifyingHost(0.0f);
    if (auto* trim = copy->m_state.getParameter(ParamIDs::masterTrim))
        trim->setValueNotifyingHost(trim->getDefaultValue());
    m_cargo.beginLending();
    copy->m_cargo.shareFrom(m_cargo);
    return copy;
}

void SwooshbucklerAudioProcessor::prepareOffline(double sampleRate, std::uint32_t seed)
{
    jassert(m_offline);
    // Stopped, nothing held, nothing waiting.
    m_playing.store(false);
    m_restartPending.store(false);
    m_uiThrows.store(0);
    m_uiSwell.store(false);
    m_reseedPending = false;
    m_playParamWas = m_raw.play->load() > 0.5f; // the Play parameter as it was copied starts nothing here
    m_keysLatched = {};
    m_transportGain = 0.0f;
    m_tailElapsed = m_tailQuiet = 0;
    m_tailDone = false;
    m_drainLeft = 0;
    m_tapeRested = false;
    m_followerLevels = {};
    // Every random source from the seed: the engine's passes, sounds and rides, and the Random / Steps modulators.
    m_engine.setSeed(seed);
    m_mods.setSeed(static_cast<juce::int64>(seed));
    m_recordRateOverride = 0; // the rig renders at the rate it is prepared at
    setPlayConfigDetails(0, 2, sampleRate, c_maxSection);
    prepareToPlay(sampleRate, c_maxSection); // empties the engine, the modulators and the output stage
    // The morph wander sets off from where the X / Y controls are.
    m_wanderOn = false;
    m_wanderBlend = 0.0f;
    m_morphX = m_raw.morphX->load();
    m_morphY = m_raw.morphY->load();
}

//==============================================================================
// Names, descriptions and the takes
bool SwooshbucklerAudioProcessor::getOneFilePerWhoosh() const { return m_settings->getBoolValue("oneFilePerWhoosh", false); }

void SwooshbucklerAudioProcessor::setOneFilePerWhoosh(bool on)
{
    if (on == getOneFilePerWhoosh())
        return;
    m_settings->setValue("oneFilePerWhoosh", on);
    m_settings->saveIfNeeded();
    if (isRecording())
    {
        // The open take is written the old way: close it and carry on in a new one.
        stopRecording();
        juce::String error;
        arm(error);
    }
}

juce::String SwooshbucklerAudioProcessor::getTakeName() const { return m_settings->getValue("takeName").trim(); }
void SwooshbucklerAudioProcessor::setTakeName(const juce::String& name) { m_settings->setValue("takeName", name.trim()); m_settings->saveIfNeeded(); updateTapeNaming(); }
juce::String SwooshbucklerAudioProcessor::getCreator() const { return m_settings->getValue("creator").trim(); }
void SwooshbucklerAudioProcessor::setCreator(const juce::String& creator) { m_settings->setValue("creator", creator.trim()); m_settings->saveIfNeeded(); updateTapeNaming(); }
int SwooshbucklerAudioProcessor::getNameStyle() const { return juce::jlimit(0, 1, m_settings->getIntValue("nameStyle", 0)); }
void SwooshbucklerAudioProcessor::setNameStyle(int style) { m_settings->setValue("nameStyle", juce::jlimit(0, 1, style)); m_settings->saveIfNeeded(); updateTapeNaming(); }

int SwooshbucklerAudioProcessor::getRetroMinutes() const
{
    const int minutes = m_settings->getIntValue("retroMinutes", 1);
    return minutes == 3 || minutes == 5 ? minutes : 1;
}

void SwooshbucklerAudioProcessor::setRetroMinutes(int minutes)
{
    m_settings->setValue("retroMinutes", minutes == 3 || minutes == 5 ? minutes : 1);
    m_settings->saveIfNeeded();
    m_recorder.setRetroSeconds(getRetroMinutes() * 60);
}

juce::String SwooshbucklerAudioProcessor::getEffectiveTakeName() const
{
    if (const auto name = getTakeName(); name.isNotEmpty())
        return name;
    const bool message = juce::MessageManager::existsAndIsCurrentThread();
    const auto preset = message ? currentPresetName : juce::String();
    return preset.isEmpty() || preset == "Default" ? juce::String("Swooshbuckler") : preset;
}

juce::String SwooshbucklerAudioProcessor::describeSettings() const
{
    // Short enough for a BWF description (256 characters): the version, the preset, and the controls that say most
    // about how the take was made.
    auto text = [this](const char* id)
    {
        auto* param = m_state.getParameter(id);
        return param != nullptr ? param->getCurrentValueAsText() : juce::String();
    };
    juce::String d;
    d << "Swooshbuckler " << Version::number() << ". Preset: " << currentPresetName
      << ". Energy " << text(ParamIDs::energy) << ", Rate " << text(ParamIDs::rate) << ", Fill " << text(ParamIDs::fill)
      << ", Chaos " << text(ParamIDs::chaos) << ", Pitch " << text(ParamIDs::pitch) << ", Space " << text(ParamIDs::space)
      << ". Pass A " << text(ParamIDs::passMode[0]) << ", Pass B " << text(ParamIDs::passMode[1])
      << ". Morph " << text(ParamIDs::morphOn) << ", Aim " << text(ParamIDs::hitAim) << ".";
    return d.substring(0, 256);
}

void SwooshbucklerAudioProcessor::updateTapeNaming()
{
    TapeRecorder::Naming naming;
    naming.prefix = getEffectiveTakeName();
    naming.style = getNameStyle() == 1 ? TapeRecorder::NameStyle::DateTime : TapeRecorder::NameStyle::Counter;
    naming.description = describeSettings();
    naming.creator = getCreator().isNotEmpty() ? getCreator() : juce::String("Samuel Justice");
    naming.software = "Swooshbuckler " + Version::number();
    m_recorder.setNaming(naming);
}

void SwooshbucklerAudioProcessor::refreshTakes()
{
    for (const auto& take : m_recorder.takeFinishedTakes())
    {
        ++m_filesThisTake;
        registerTake(take.file, take.whoosh ? "Whoosh" : "Tape", take.seconds, take.markers);
    }
    if (m_variations != nullptr)
        for (const auto& written : m_variations->takeWritten())
            registerTake(written.file, written.markers > 0 ? "Variations" : "Variation", written.seconds, written.markers); // one file that holds them all is one take
}

void SwooshbucklerAudioProcessor::registerTake(const juce::File& fileToAdd, const juce::String& kindToShow, double seconds, int markers)
{
    const juce::File file(fileToAdd);     // copies: the caller may have handed us a take of the list that is about
    const juce::String kind(kindToShow);  // to be replaced
    if (! juce::MessageManager::existsAndIsCurrentThread())
    {
        juce::WeakReference<SwooshbucklerAudioProcessor> weak(this);
        juce::MessageManager::callAsync([weak, file, kind, seconds, markers]
        {
            if (auto* self = weak.get())
                self->registerTake(file, kind, seconds, markers);
        });
        return;
    }
    if (seconds < 0.0)
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        if (std::unique_ptr<juce::AudioFormatReader> reader { formats.createReaderFor(file) })
            seconds = reader->sampleRate > 0.0 ? static_cast<double>(reader->lengthInSamples) / reader->sampleRate : 0.0;
    }
    for (auto it = m_takes.begin(); it != m_takes.end();) // the same file again (written over): one entry
        it = it->file == file ? m_takes.erase(it) : it + 1;
    Take take;
    take.id = m_nextTakeId++;
    take.file = file;
    take.kind = kind;
    take.seconds = juce::jmax(0.0, seconds);
    take.markers = markers;
    m_takes.insert(m_takes.begin(), take);
    // Full: the oldest take that isn't starred leaves the list (its file stays where it is); all starred, the oldest.
    while (static_cast<int>(m_takes.size()) > c_maxTakes)
    {
        auto oldest = m_takes.end() - 1;
        for (auto it = m_takes.end(); it != m_takes.begin();)
            if (! (--it)->starred)
            {
                oldest = it;
                break;
            }
        m_takes.erase(oldest);
    }
    ++m_takesGeneration;
}

void SwooshbucklerAudioProcessor::setTakeStarred(int id, bool starred)
{
    for (auto& take : m_takes)
        if (take.id == id && take.starred != starred)
        {
            take.starred = starred;
            ++m_takesGeneration;
        }
}

bool SwooshbucklerAudioProcessor::deleteTake(int id, bool toTrash)
{
    for (auto it = m_takes.begin(); it != m_takes.end(); ++it)
        if (it->id == id)
        {
            if (m_audition.getFile() == it->file)
                m_audition.close(); // it holds the file open, and an open file can't be moved everywhere
            if (it->file.existsAsFile() && ! (toTrash ? it->file.moveToTrash() : it->file.deleteFile()))
                return false;
            m_takes.erase(it);
            ++m_takesGeneration;
            return true;
        }
    return false;
}

bool SwooshbucklerAudioProcessor::auditionTake(int id, juce::String& error)
{
    for (const auto& take : m_takes)
        if (take.id == id)
            return m_audition.play(take.file, error);
    error = "That take is no longer in the list";
    return false;
}

int SwooshbucklerAudioProcessor::getAuditionTake() const
{
    if (! m_audition.isPlaying())
        return -1;
    const auto file = m_audition.getFile();
    for (const auto& take : m_takes)
        if (take.file == file)
            return take.id;
    return -1;
}

juce::File SwooshbucklerAudioProcessor::getRecordFolder() const
{
    const auto path = m_settings->getValue("recordFolder");
    const juce::File folder = path.isNotEmpty() ? juce::File(path) : TapeRecorder::defaultFolder();
    return folder;
}

void SwooshbucklerAudioProcessor::setRecordFolder(const juce::File& folder)
{
    m_settings->setValue("recordFolder", folder.getFullPathName());
    m_settings->saveIfNeeded();
}

int SwooshbucklerAudioProcessor::getBitDepth() const
{
    const int bits = m_settings->getIntValue("bitDepth", 24);
    return (bits == 16 || bits == 24 || bits == 32) ? bits : 24;
}

void SwooshbucklerAudioProcessor::setBitDepth(int bits)
{
    m_settings->setValue("bitDepth", bits);
    m_settings->saveIfNeeded();
}

bool SwooshbucklerAudioProcessor::getAutoRecord() const
{
    return m_settings->getBoolValue("autoRecord", false);
}

void SwooshbucklerAudioProcessor::setAutoRecord(bool on)
{
    m_settings->setValue("autoRecord", on);
    m_settings->saveIfNeeded();
}

bool SwooshbucklerAudioProcessor::getSkipSilence() const
{
    return m_settings->getBoolValue("skipSilence", true);
}

void SwooshbucklerAudioProcessor::setSkipSilence(bool on)
{
    m_settings->setValue("skipSilence", on);
    m_settings->saveIfNeeded();
    m_recorder.setSignalGate(on); // an open take follows at once
}

void SwooshbucklerAudioProcessor::timerCallback()
{
    // Mirror the editor fields for host saves from other threads (they are message-thread objects).
    {
        const juce::ScopedLock sl(m_stateLock);
        m_mirrorWidth = savedEditorWidth;
        m_mirrorPreset = currentPresetName;
    }

    // The host's transport stopped the rig: the Play parameter says so too (it would start the rig again when the
    // session is opened otherwise).
    if (m_hostStopped.exchange(false) && ! m_playing.load())
        setPlaying(false);

    refreshHostReadouts();

    // The tape: the files it has closed go into the takes, and the next file is named and described by the
    // settings as they are now.
    refreshTakes();
    if (isRecording())
        updateTapeNaming();

    // Auto-roll the tape once audio is running (message thread, never the audio thread).
    if (! m_autoRecordDone && getSampleRate() > 0.0)
    {
        m_autoRecordDone = true;
        if (getAutoRecord() && ! isRecording())
        {
            juce::String error;
            arm(error);
        }
    }
}

//==============================================================================
double SwooshbucklerAudioProcessor::getRecordingDroppedSeconds() const
{
    return m_recorder.getDroppedSeconds(); // by the rate the tape runs at (the render rate), not the device's
}

namespace
{
    // A saved state is these eight bytes, then the state as a binary ValueTree stream. (Sessions saved before were
    // XML, with the audio in base64: a third bigger. Those are still read.)
    constexpr char c_stateMagic[8] = { 'S', 'W', 'B', 'K', 'S', 'T', '1', '\n' };
    const juce::Identifier c_stateType("Swooshbuckler");

    juce::ValueTree lookToTree(const juce::String& name, const std::map<juce::String, float>& look)
    {
        juce::ValueTree set("Set");
        set.setProperty("name", name, nullptr);
        for (const auto& [id, value] : look)
        {
            juce::ValueTree v("Value");
            v.setProperty("id", id, nullptr);
            v.setProperty("value", value, nullptr);
            set.appendChild(v, nullptr);
        }
        return set;
    }
}

void SwooshbucklerAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = m_state.copyState();
    state.removeChild(state.getChildWithName("Cargo"), nullptr);
    state.removeChild(state.getChildWithName("Mods"), nullptr);
    state.removeChild(state.getChildWithName("AB"), nullptr);
    state.appendChild(m_mods.toValueTree(), nullptr); // the wires (thread safe snapshot)

    juce::ValueTree cargo;
    int width = 0;
    juce::String preset, sessionPath;
    {
        const juce::ScopedLock sl(m_stateLock);
        if (m_pending.valid)
        {
            // A restore that hasn't reached the message thread yet: save what was restored, not the old cargo.
            cargo = m_pending.cargo.createCopy();
            width = m_pending.editorWidth;
            preset = m_pending.presetName;
        }
        else
        {
            width = m_mirrorWidth;
            preset = m_mirrorPreset;
        }
        sessionPath = m_sessionPath;
        // A/B: the two sets and which one you are on (left out until the other set has been used).
        if (! m_ab[0].empty() || ! m_ab[1].empty())
        {
            juce::ValueTree ab("AB");
            ab.setProperty("slot", m_abSlot, nullptr);
            for (size_t i = 0; i < m_ab.size(); ++i)
                ab.appendChild(lookToTree(m_abNames[i], m_ab[i]), nullptr);
            state.appendChild(ab, nullptr);
        }
    }

    if (! cargo.isValid())
    {
        if (juce::MessageManager::existsAndIsCurrentThread())
        {
            width = savedEditorWidth;
            preset = currentPresetName;
        }
        cargo = m_cargo.toValueTree(); // snapshots under CargoHold's lock: safe from a host save thread
    }

    state.setProperty("editorWidth", width, nullptr);
    state.setProperty("presetName", preset, nullptr);
    state.setProperty("followHost", getFollowHost(), nullptr);
    state.setProperty("sectionLocks", static_cast<int>(m_lockedSections.load()), nullptr);
    state.setProperty("stowawayPanel", m_stowPanel.load(), nullptr);
    if (sessionPath.isNotEmpty())
        state.setProperty("sessionFile", sessionPath, nullptr);
    else
        state.removeProperty("sessionFile", nullptr);
    state.removeProperty("throwPath", nullptr); // v2 sessions
    state.appendChild(cargo, nullptr);

    juce::MemoryOutputStream out(destData, false);
    out.write(c_stateMagic, sizeof(c_stateMagic));
    state.writeToStream(out);
}

bool SwooshbucklerAudioProcessor::readState(const void* data, int sizeInBytes, juce::ValueTree& state)
{
    state = {};
    if (data == nullptr || sizeInBytes <= 0)
        return false;
    const size_t size = static_cast<size_t>(sizeInBytes);
    if (size > sizeof(c_stateMagic) && std::memcmp(data, c_stateMagic, sizeof(c_stateMagic)) == 0)
        state = juce::ValueTree::readFromData(static_cast<const char*>(data) + sizeof(c_stateMagic), size - sizeof(c_stateMagic));
    else if (auto xml = getXmlFromBinary(data, sizeInBytes))
        state = juce::ValueTree::fromXml(*xml);
    return state.isValid() && state.hasType(c_stateType);
}

void SwooshbucklerAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    juce::ValueTree tree;
    if (readState(data, sizeInBytes, tree))
        restoreStateTree(tree);
}

void SwooshbucklerAudioProcessor::restoreStateTree(juce::ValueTree tree)
{
    const int width = tree.getProperty("editorWidth", 0);
    const juce::String preset = tree.getProperty("presetName", "Default").toString();
    tree.removeProperty("throwPath", nullptr); // v2 sessions
    m_followHost.store(canFollowHost() && static_cast<bool>(tree.getProperty("followHost", true)));
    m_lockedSections.store(static_cast<unsigned>(static_cast<int>(tree.getProperty("sectionLocks", 0))) & ((1u << Presets::numSections) - 1u));
    m_stowPanel.store(juce::jlimit(-1, 1, static_cast<int>(tree.getProperty("stowawayPanel", -1))));
    {
        // The session's file, and the A/B sets (a state without them has none).
        const juce::ScopedLock sl(m_stateLock);
        m_sessionPath = tree.getProperty("sessionFile", juce::String()).toString();
        if (! juce::File::isAbsolutePath(m_sessionPath)) // a state from another kind of machine, or not a path at all
            m_sessionPath.clear();
        m_ab = {};
        m_abNames = {};
        m_abSlot = 0;
        if (auto ab = tree.getChildWithName("AB"); ab.isValid())
        {
            m_abSlot = juce::jlimit(0, 1, static_cast<int>(ab.getProperty("slot", 0)));
            for (int i = 0; i < juce::jmin(2, ab.getNumChildren()); ++i)
            {
                const auto set = ab.getChild(i);
                m_abNames[static_cast<size_t>(i)] = set.getProperty("name").toString();
                for (const auto& v : set)
                    m_ab[static_cast<size_t>(i)][v.getProperty("id").toString()] = static_cast<float>(v.getProperty("value", 0.0f));
            }
            tree.removeChild(ab, nullptr);
        }
    }
    // The app always opens stopped, so nothing blasts out; in a host the session comes back as it was left (a
    // bounce of a session left playing has sound).
    if (! canFollowHost())
        if (auto play = tree.getChildWithProperty("id", ParamIDs::play); play.isValid())
            play.setProperty("value", 0.0f, nullptr);

    // Sessions saved before AIM existed keep the hit gate (and SYNC HITS) they were made with: AIM is on only for new
    // sessions. A parameter missing from a saved state would otherwise keep its default (on).
    if (! tree.getChildWithProperty("id", ParamIDs::hitAim).isValid())
    {
        juce::ValueTree aim("PARAM");
        aim.setProperty("id", ParamIDs::hitAim, nullptr);
        aim.setProperty("value", 0.0f, nullptr);
        tree.appendChild(aim, nullptr);
    }

    // The wires (a state without them has none).
    {
        auto mods = tree.getChildWithName("Mods");
        m_mods.fromValueTree(mods.isValid() ? mods.createCopy() : juce::ValueTree("Mods"));
        if (mods.isValid())
            tree.removeChild(mods, nullptr);
    }

    // A state without cargo (host default state, parameters only) clears the sample lanes.
    auto cargoChild = tree.getChildWithName("Cargo");
    const auto cargo = cargoChild.isValid() ? cargoChild.createCopy() : juce::ValueTree("Cargo");
    if (cargoChild.isValid())
        tree.removeChild(cargoChild, nullptr);

    if (juce::MessageManager::existsAndIsCurrentThread())
    {
        m_state.replaceState(tree);
        applyRestoredState(cargo, width, preset);
        return;
    }

    // Off the message thread: CargoHold and the editor fields are message-thread objects. Keep the state pending
    // (saves return it until applied) and apply it on the message thread, guarded against our destruction.
    {
        const juce::ScopedLock sl(m_stateLock);
        m_pending.valid = true;
        m_pending.id = ++m_pendingCounter;
        m_pending.cargo = cargo;
        m_pending.editorWidth = width;
        m_pending.presetName = preset;
    }
    m_state.replaceState(tree);

    juce::WeakReference<SwooshbucklerAudioProcessor> weak(this);
    juce::MessageManager::callAsync([weak]
    {
        if (auto* self = weak.get())
            self->applyPendingState();
    });
}

void SwooshbucklerAudioProcessor::applyPendingState()
{
    PendingState pending;
    {
        const juce::ScopedLock sl(m_stateLock);
        if (! m_pending.valid)
            return;
        pending = m_pending;
    }

    applyRestoredState(pending.cargo, pending.editorWidth, pending.presetName);

    // Only now may saves read the live cargo again (unless a newer restore arrived meanwhile).
    const juce::ScopedLock sl(m_stateLock);
    if (m_pending.valid && m_pending.id == pending.id)
        m_pending = PendingState {};
}

void SwooshbucklerAudioProcessor::applyRestoredState(const juce::ValueTree& cargo, int editorWidth, const juce::String& presetName)
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    m_mods.notifyChanged(); // the wires were restored (possibly off the message thread): redraw them
    // Another session: nothing of the old one is left to undo or to step back to.
    m_history->clear();
    m_rolls.clear();
    m_rollAt = -1;
    // A session file that is no longer there (the state came from another machine, the file was moved or its disk
    // is away) is forgotten, so Save Session asks where to save instead of writing to a place that has gone.
    if (const auto file = getSessionFile(); file != juce::File() && ! file.existsAsFile())
    {
        const juce::ScopedLock sl(m_stateLock);
        if (m_sessionPath == file.getFullPathName())
            m_sessionPath.clear();
    }
    savedEditorWidth = editorWidth;
    // Sessions saved before the preset names were Title Case ("Steady energy"): show today's spelling.
    currentPresetName = presetName;
    for (const auto& preset : Presets::factory())
        if (preset.name.equalsIgnoreCase(presetName))
            currentPresetName = preset.name;
    {
        const juce::ScopedLock sl(m_stateLock);
        m_mirrorWidth = editorWidth;
        m_mirrorPreset = presetName;
    }
    m_cargo.fromValueTree(cargo);
    stateGeneration.fetch_add(1);
}

std::function<void()> SwooshbucklerAudioProcessor::showAudioSettings;
juce::PropertiesFile* SwooshbucklerAudioProcessor::s_sharedSettings = nullptr;

juce::AudioProcessorEditor* SwooshbucklerAudioProcessor::createEditor()
{
    return new SwooshbucklerAudioProcessorEditor(*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SwooshbucklerAudioProcessor();
}
