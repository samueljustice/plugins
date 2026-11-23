#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "SettingsFile.h"
#include "VariationRenderer.h"
#include "Version.h"

namespace
{
    std::unique_ptr<juce::PropertiesFile> openSettings()
    {
        // The file every Swooshbuckler shares; an old one that still holds the app's session gives it up first, so
        // this instance never reads or writes megabytes (SettingsFile.h).
        settingsfile::separateAppSession(settingsfile::defaultFile());
        return std::make_unique<juce::PropertiesFile>(settingsfile::defaultFile(), settingsfile::options());
    }
}

SwooshbucklerAudioProcessor::SwooshbucklerAudioProcessor() : SwooshbucklerAudioProcessor(nullptr) {}

SwooshbucklerAudioProcessor::SwooshbucklerAudioProcessor(juce::PropertiesFile* offlineSettings)
    // Sample-based instrument: no audio input (so no feedback loop, no microphone), MIDI in, stereo out by default
    // (a host may make it any of the room's widths: see isBusesLayoutSupported).
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true))
    , m_offline(offlineSettings != nullptr)
    , m_state(*this, nullptr, "Swooshbuckler", Params::createLayout(m_stowModeSource))
{
    // The list of wire targets and Swell's place in it are looked up once, here: the audio thread reads them and
    // must not be the one that builds them.
    juce::ignoreUnused(ModMatrix::swellTarget());
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
    m_raw.rideMode = raw(ParamIDs::rideMode);
    m_raw.togetherPeakAt = raw(ParamIDs::togetherPeakAt);
    m_raw.togetherStagger = raw(ParamIDs::togetherStagger);
    m_raw.togetherHitEvery = raw(ParamIDs::togetherHitEvery);
    m_raw.swell = raw(ParamIDs::swell);
    m_raw.swellAuto = raw(ParamIDs::swellAuto);
    m_raw.swellEvery = raw(ParamIDs::swellEvery);
    m_raw.swellHold = raw(ParamIDs::swellHold);
    m_raw.swellChance = raw(ParamIDs::swellChance);
    m_raw.swellLoose = raw(ParamIDs::swellLoose);
    for (int b = 0; b < ParamIDs::numBuses; ++b)
        for (int k = 0; k < ParamIDs::numBusDetails; ++k)
            m_raw.busDetail[b][k] = raw(ParamIDs::busDetail[b][k]);
    for (int k = 0; k < ParamIDs::numEqParams; ++k)
        m_raw.eq[k] = raw(ParamIDs::eqIds[k]);
    m_raw.energy = raw(ParamIDs::energy);
    m_raw.rate = raw(ParamIDs::rate);
    m_raw.fill = raw(ParamIDs::fill);
    m_raw.chaos = raw(ParamIDs::chaos);
    for (int b = 0; b < ParamIDs::numBuses; ++b)
        for (int k = 0; k < ParamIDs::numBusControls; ++k)
            m_raw.bus[b][k] = ParamIDs::bus[b][k] != nullptr ? raw(ParamIDs::bus[b][k]) : nullptr;
    for (int b = 0; b < ParamIDs::numBuses; ++b)
    {
        m_raw.passMode[b] = raw(ParamIDs::passMode[b]);
        m_raw.flightPath[b] = raw(ParamIDs::flightPath[b]);
    }
    m_raw.stowFlightPath = raw(ParamIDs::stowFlightPath);
    m_raw.roomSpread = raw(ParamIDs::roomSpread);
    m_raw.roomHeight = raw(ParamIDs::roomHeight);
    m_raw.roomCentre = raw(ParamIDs::roomCentre);
    m_raw.roomHeightVaries = raw(ParamIDs::roomHeightVaries);
    for (int b = 0; b < ParamIDs::numBuses; ++b)
    {
        m_raw.direction[b] = raw(ParamIDs::direction[b]);
        m_raw.side[b] = raw(ParamIDs::side[b]);
        m_directionTarget[b] = ModMatrix::targetIndex(ParamIDs::direction[b]);
        m_sideTarget[b] = ModMatrix::targetIndex(ParamIDs::side[b]);
    }
    m_raw.stowDirection = raw(ParamIDs::stowDirection);
    m_raw.stowSide = raw(ParamIDs::stowSide);
    m_directionTarget[ParamIDs::numBuses] = ModMatrix::targetIndex(ParamIDs::stowDirection);
    m_sideTarget[ParamIDs::numBuses] = ModMatrix::targetIndex(ParamIDs::stowSide);
    m_raw.roomLfeMode = raw(ParamIDs::roomLfeMode);
    m_raw.roomLfeLevel = raw(ParamIDs::roomLfeLevel);
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
    m_raw.hitPosition = raw(ParamIDs::hitPosition);
    m_raw.hitSpread = raw(ParamIDs::hitSpread);
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
    // Nor are Swell's presses (a host records those) or a lean on Varispeed (it springs back by itself).
    m_history->isRecorded = [play = m_state.getParameter(ParamIDs::play)->getParameterIndex(),
                             swell = m_state.getParameter(ParamIDs::swell)->getParameterIndex(),
                             leanA = m_state.getParameter(ParamIDs::busDetail[0][ParamIDs::Varispeed])->getParameterIndex(),
                             leanB = m_state.getParameter(ParamIDs::busDetail[1][ParamIDs::Varispeed])->getParameterIndex()](int index)
    {
        return index != play && index != swell && index != leanA && index != leanB;
    };
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
    // The saved last minutes take their memory at the first Play, arm or sound, not here: an instance that is
    // never played costs nothing for them.
    m_recorder.setRetroOnDemand();
    m_recorder.setRetroSeconds(getRetroMinutes() * 60);
    m_listening.store(juce::jlimit(0, room::c_numWidths - 1, m_settings->getIntValue("listening", static_cast<int>(room::Width::Stereo))));
    // The learned MIDI controllers: "target:cc" pairs.
    for (const auto& pair : juce::StringArray::fromTokens(m_settings->getValue("midiControls"), ",", ""))
    {
        const int target = pair.upToFirstOccurrenceOf(":", false, false).getIntValue();
        const int cc = pair.fromFirstOccurrenceOf(":", false, false).getIntValue();
        if (pair.containsChar(':') && target >= 0 && target < c_numCcTargets && cc >= 0 && cc < 128)
            m_ccOf[static_cast<size_t>(target)].store(cc);
    }
    updateCcPump();
    if (! m_offline)
        m_retroPump.startTimerHz(50);
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
    m_retroPump.stopTimer();
    m_recorder.stop();
}

bool SwooshbucklerAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    // Any of the room's seven widths (Mono, Stereo, Quad, 5.1, 7.1, 7.1.2, 7.1.4) as JUCE's own channel set. In
    // AAX each one becomes a stem format with a plug-in ID of its own (JUCE's default IDs come from a fixed table
    // of channel sets, so Mono and Stereo keep theirs); VST3 and AU map them to their speaker arrangements.
    // The app takes its Listening width and no other: AppOutput sets it, and the audio device is never matched to
    // the rig by its output count (a device with six outputs would otherwise make a Stereo listener's rig 5.1).
    const auto width = room::widthFor(layouts.getMainOutputChannelSet());
    if (! width.has_value())
        return false;
    // (An offline copy has no device: Render Variations prepares it at any width, in the app too.)
    if (isApp() && *width != getListening())
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
    m_sustain = false;
    m_midiSwell.store(false);
    m_audition.prepare(sampleRate, room::widthFor(getBusesLayout().getMainOutputChannelSet()).value_or(room::Width::Stereo));
    m_sumGuard.prepare(sampleRate);
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

    // The room: the track's width (the output bus). The audio is carried at the pipe's width (Mono as Stereo).
    m_width = room::widthFor(getBusesLayout().getMainOutputChannelSet()).value_or(room::Width::Stereo);
    m_roomWidth.store(static_cast<int>(m_width), std::memory_order_relaxed);
    m_pipeChannels = RoomMaster::pipeChannels(m_width);
    {
        const auto& pipe = room::layout(RoomMaster::pipeWidth(m_width));
        const auto& toMono = room::fold(pipe.width, room::Width::Mono);
        m_loudnessWeight.fill(0.0f);
        m_monoFold.fill(0.0f);
        for (int ch = 0; ch < pipe.numChannels; ++ch)
        {
            m_loudnessWeight[static_cast<size_t>(ch)] = room::loudnessWeight(pipe.speakers[static_cast<size_t>(ch)]);
            m_monoFold[static_cast<size_t>(ch)] = toMono.gain[0][static_cast<size_t>(ch)];
        }
    }
    for (auto& peak : m_channelPeak)
        peak.store(0.0f, std::memory_order_relaxed);

    // The stream's grid starts again with the engine (see processBlock): nothing of the last quantum is left to
    // play (it was rendered by the engine as it was, at the rate and the width it had).
    m_carry = 0;
    m_sinceControl = 0;
    m_followSquares = 0.0;
    m_followSamples = 0;

    updateEngineParams();
    m_outputEq.prepare(m_renderRate, m_pipeChannels); // (after the settings above: its glides start where they are)
    m_engine.setWidth(RoomMaster::pipeWidth(m_width)); // Quad and wider: every pass is an object in the room
    m_engine.prepare(m_renderRate, m_blockSize);
    m_master.prepare(m_renderRate, m_width);
    if (! m_offline)
        m_recorder.prepare(m_renderRate, m_pipeChannels); // the tape records the room at its width (Mono as Stereo)
    m_work.setSize(m_pipeChannels, m_blockSize);

    // Latency: the engine and the limiter (in device samples), and when rendering high, the input the
    // downsampler needs ahead of its output plus the monitor limiter's look-ahead.
    double latency = (static_cast<double>(m_engine.getLatencySamples()) + m_master.getLatencySamples()) / ratio;
    if (ratio > 1.0)
    {
        m_down.prepare(ratio, m_pipeChannels);
        m_down.prime(); // the first sound after a prepare is as late as the latency below, like every later one
        m_highRate.setSize(m_pipeChannels, m_down.maxInputsFor(c_maxSection));
        m_highRate.clear();
        m_monitorLimiter.prepare(m_deviceRate, m_width);
        latency += m_down.getLatencySamples() + m_monitorLimiter.getLatencySamples();
    }
    setLatencySamples(juce::roundToInt(latency));
}

void SwooshbucklerAudioProcessor::renderAtRenderRate(const float* inL, const float* inR, float* const* chans, int n)
{
    const int numChans = m_pipeChannels;
    const bool playing = m_playing.load();
    m_engine.setReleased(! playing); // Stop: the sources let go, the echoes and Space ring out
    m_engine.process(inL, inR, chans, numChans, n);
    // The pass peaks and hits of this block, for the take's markers: they reach the tape after the limiter's look-ahead.
    static_assert(static_cast<int>(RigEngine::MarkPassA) == static_cast<int>(TapeRecorder::MarkPassA)
                  && static_cast<int>(RigEngine::MarkHit) == static_cast<int>(TapeRecorder::MarkHit)
                  && static_cast<int>(RigEngine::MarkPassStow) == static_cast<int>(TapeRecorder::MarkPassStow), "the same marker kinds");
    for (int i = 0; i < m_engine.getNumMarkers() && ! m_offline; ++i)
        m_recorder.addMarker(m_engine.getMarker(i).offset + m_master.getLatencySamples(), m_engine.getMarker(i).kind);
    const bool throwing = m_engine.isThrowing(); // a throw with the rig stopped: its tail is waited for afterwards

    // The Output EQ: after Space, before Inflate and the limiters, on every channel alike (the LFE too).
    m_outputEq.process(chans, numChans, n);

    // Inflate, then Push (drive into the limiter; the ceiling stays put), then the limiter: one linked across the
    // main channels, one of its own on the LFE.
    m_master.process(chans, numChans, n);

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
        for (int ch = 0; ch < numChans; ++ch)
            for (int i = 0; i < n; ++i)
                peak = juce::jmax(peak, std::abs(chans[ch][i]));
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
            for (int ch = 0; ch < numChans; ++ch)
                chans[ch][i] *= m_transportGain;
        }
    }
    if (! m_offline)
        m_recorder.process(chans, numChans, n); // the full-rate signal, every channel of the room
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
            m_mods.publishTarget(t, t == ModMatrix::swellTarget() ? (m_mods.swellGate() ? 1.0f : 0.0f) // Swell's wires hold it or not
                                                                  : juce::jlimit(0.0f, 1.0f, m_modTargets[i].param->convertTo0to1(m_modTargets[i].raw->load()) + m_mods.offset(t)));
    }
    publishDirectionViews();
}

void SwooshbucklerAudioProcessor::publishDirectionViews()
{
    // A wired Direction or Side shows the choice its modulator makes right now (the second or the third of the
    // three: forwards or backwards, left or right), not a place between two choices.
    for (const auto* targets : { m_directionTarget, m_sideTarget })
        for (int k = 0; k <= ParamIDs::numBuses; ++k)
            if (const int t = targets[k]; t >= 0 && m_mods.isWired(t))
                m_mods.publishTarget(t, m_mods.centred(t) >= 0.0f ? 0.5f : 1.0f);
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
    static_assert(static_cast<int>(RigEngine::c_rideModes) == static_cast<int>(ParamIDs::numRideModes)
                  && static_cast<int>(RigEngine::RideFollowMaster) == static_cast<int>(ParamIDs::RideFollowMaster), "the same Ride Modes");
    p.rideMode = juce::jlimit(0, ParamIDs::numRideModes - 1, juce::roundToInt(m_raw.rideMode->load()));
    p.together.peakAt = m_raw.togetherPeakAt->load();
    p.together.stagger = m_raw.togetherStagger->load();
    p.together.hitEvery = 1 + juce::jlimit(0, ParamIDs::numHitEvery - 1, juce::roundToInt(m_raw.togetherHitEvery->load()));
    static_assert(static_cast<int>(RigEngine::c_autoSwellModes) == static_cast<int>(ParamIDs::numAutoSwells), "the same Auto Swell modes");
    p.autoSwell.mode = juce::jlimit(0, ParamIDs::numAutoSwells - 1, juce::roundToInt(m_raw.swellAuto->load()));
    p.autoSwell.every = m_raw.swellEvery->load();
    p.autoSwell.hold = juce::jmin(m_raw.swellHold->load(), ParamIDs::swellHoldShareMax * p.autoSwell.every);
    p.autoSwell.chance = m_raw.swellChance->load();
    p.autoSwell.loose = m_raw.swellLoose->load();
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
        d.path = juce::jlimit(0, ParamIDs::numFlightPaths - 1, juce::roundToInt(m_raw.flightPath[b]->load()));
        // Direction and Side: the choice, and where a modulator wired to it stands (it decides as a pass starts).
        d.direction = juce::jlimit(0, ParamIDs::numDirections - 1, juce::roundToInt(m_raw.direction[b]->load()));
        d.side = juce::jlimit(0, ParamIDs::numDirections - 1, juce::roundToInt(m_raw.side[b]->load()));
        d.directionWired = m_mods.isWired(m_directionTarget[b]);
        d.directionMod = m_mods.centred(m_directionTarget[b]);
        d.sideWired = m_mods.isWired(m_sideTarget[b]);
        d.sideMod = m_mods.centred(m_sideTarget[b]);

        // What is behind Swirl and Filter (only Varispeed is a wire target).
        static_assert(static_cast<int>(RigEngine::c_swirlTypes) == static_cast<int>(ParamIDs::numSwirlTypes)
                      && static_cast<int>(RigEngine::SwirlTape) == static_cast<int>(ParamIDs::SwirlTape)
                      && static_cast<int>(SweepFilter::c_types) == static_cast<int>(ParamIDs::numFilterTypes)
                      && static_cast<int>(SweepFilter::c_shapes) == static_cast<int>(ParamIDs::numFilterShapes)
                      && static_cast<int>(SwirlFx::ByHand) == static_cast<int>(ParamIDs::TapeByHand)
                      && static_cast<int>(SwirlFx::WithFilter) == static_cast<int>(ParamIDs::TapeWithFilter)
                      && static_cast<int>(SwirlFx::WaveTriangle) == static_cast<int>(ParamIDs::WaveTriangle), "the same detail choices");
        auto detail = [&](int k) { return m_raw.busDetail[b][k]->load(); };
        auto choice = [&](int k, int count) { return juce::jlimit(0, count - 1, juce::roundToInt(detail(k))); };
        auto& sw = p.swirlDetail[static_cast<size_t>(b)];
        sw.type = choice(ParamIDs::SwirlType, ParamIDs::numSwirlTypes);
        sw.feedback = detail(ParamIDs::SwirlFeedback);
        sw.speed = detail(ParamIDs::SwirlSpeed);
        sw.depth = detail(ParamIDs::SwirlDepth);
        sw.follow = detail(ParamIDs::SwirlFollow);
        sw.tape.motion = choice(ParamIDs::TapeMotion, ParamIDs::numTapeMotions);
        sw.tape.invert = detail(ParamIDs::TapeInvert) > 0.5f;
        sw.tape.depthMs = detail(ParamIDs::TapeDepth);
        sw.tape.rateHz = detail(ParamIDs::TapeRate);
        sw.tape.feedback = detail(ParamIDs::TapeFeedback);
        sw.tape.wobble = detail(ParamIDs::TapeWobble);
        sw.tape.varispeed = modValue(m_raw.busDetail[b][ParamIDs::Varispeed]);
        sw.doubler.wave = choice(ParamIDs::DoublerWave, ParamIDs::numDoublerWaves);
        sw.doubler.thicken = detail(ParamIDs::DoublerThicken) > 0.5f;
        sw.doubler.spread = detail(ParamIDs::DoublerSpread) > 0.5f;
        sw.doubler.delayMs = detail(ParamIDs::DoublerDelay);
        sw.doubler.wander = detail(ParamIDs::DoublerWander);
        sw.doubler.rateHz = detail(ParamIDs::DoublerRate);
        sw.doubler.feedback = detail(ParamIDs::DoublerFeedback);
        auto& fd = p.filterDetail[static_cast<size_t>(b)];
        fd.type = choice(ParamIDs::FilterType, ParamIDs::numFilterTypes);
        fd.steep = detail(ParamIDs::FilterSteep) > 0.5f;
        fd.shape = choice(ParamIDs::FilterShape, ParamIDs::numFilterShapes);
        fd.closedAtHz = detail(ParamIDs::FilterClosedAt);
        fd.resonance = detail(ParamIDs::FilterResonance);
        fd.follow = detail(ParamIDs::FilterFollow);
        fd.sweep = detail(ParamIDs::FilterSweep);
        fd.rateHz = detail(ParamIDs::FilterRate);
        p.varispeedHome[static_cast<size_t>(b)] = m_varispeedHome[static_cast<size_t>(b)].exchange(false);
    }
    p.stow.direction = juce::jlimit(0, ParamIDs::numDirections - 1, juce::roundToInt(m_raw.stowDirection->load()));
    p.stow.side = juce::jlimit(0, ParamIDs::numDirections - 1, juce::roundToInt(m_raw.stowSide->load()));
    p.stow.directionWired = m_mods.isWired(m_directionTarget[ParamIDs::numBuses]);
    p.stow.directionMod = m_mods.centred(m_directionTarget[ParamIDs::numBuses]);
    p.stow.sideWired = m_mods.isWired(m_sideTarget[ParamIDs::numBuses]);
    p.stow.sideMod = m_mods.centred(m_sideTarget[ParamIDs::numBuses]);
    static_assert(ParamIDs::numFlightPaths == room::c_numPaths, "the same Flight Paths");
    p.stow.path = juce::jlimit(0, ParamIDs::numFlightPaths - 1, juce::roundToInt(m_raw.stowFlightPath->load()));
    p.roomSpread = modValue(m_raw.roomSpread);
    publishDirectionViews();
    p.roomHeight = modValue(m_raw.roomHeight);
    p.roomHeightVaries = m_raw.roomHeightVaries->load();
    p.roomCentre = m_raw.roomCentre->load() > 0.5f;
    static_assert(ParamIDs::numLfeModes == RigEngine::c_lfeModes, "the same LFE modes"); // (the levels: checked by the SpaceLfe test)
    p.lfeMode = juce::jlimit(0, ParamIDs::numLfeModes - 1, juce::roundToInt(m_raw.roomLfeMode->load()));
    p.lfeLevelDb = m_raw.roomLfeLevel->load();
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
    static_assert(static_cast<int>(ParamIDs::numHitPositions) == static_cast<int>(RigEngine::c_hitPositions), "the same Hit Positions");
    p.hitPosition = juce::jlimit(0, ParamIDs::numHitPositions - 1, juce::roundToInt(m_raw.hitPosition->load()));
    p.hitSpread = modValue(m_raw.hitSpread);
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
    // Swell now: the parameter (a host's recorded presses), a press it does not record, MIDI (a note with Keys set
    // to Swell, the sustain pedal in every mode) and the wires. Auto Swell is added by the engine.
    p.swell = m_raw.swell->load() > 0.5f || m_uiSwell.load() || updateMidiSwell() || m_mods.swellGate();

    m_engine.setParams(p);
    m_outputEq.setParams(getOutputEqParams());

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
    m_audition.setCeilingDb(m_raw.ceiling->load()); // a folded take being auditioned is held under the Ceiling too

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
        m_carry = 0;        // (silence: the rig has rung out) the stream's grid starts again when it wakes
        m_sinceControl = 0;
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
        publishChannelPeaks(nullptr, n);
        return;
    }

    m_tapeRested = false;
    if (m_reseedPending)
    {
        // The transport started: the engine, the modulators and the wander all begin again from the same place.
        m_reseedPending = false;
        m_restartPending.store(false);
        m_engine.restartSeeded();
        // The Random and Steps modulators from the engine's seed too, so they repeat from one bounce to the next.
        m_mods.setSeed(static_cast<juce::int64>(m_engine.getSeed()));
        m_mods.reset();
        // The stream's grid starts again here, at this block's first sample: what was left of the last quantum
        // belongs to the ride that has just been stopped, and played first it would start this one that many
        // samples late (a different number each time).
        m_carry = 0;
        m_sinceControl = 0;
        m_followSquares = 0.0;
        m_followSamples = 0;
        m_wanderOn = false; // the wander sets off again from where the X / Y controls are
        m_wanderBlend = 0.0f;
        m_morphX = m_raw.morphX->load();
        m_morphY = m_raw.morphY->load();
    }
    else if (m_restartPending.exchange(false))
        m_engine.restart();
    if (m_uiThrows.exchange(0) > 0)
        m_engine.throwWhoosh(0, -1.0f, m_offlineFlight); // as close as the bus draws it

    // THE STREAM'S OWN GRID. The rig is rendered a quantum (c_quantum samples) at a time, counted from when it woke,
    // not from the start of the host's block: what is left of a quantum when the host's block ends is played at the
    // start of the next one. The modulators, the wander and the parameters they move are updated every
    // c_maxSection samples of that same stream. So the sound does not depend on the size of the host's blocks
    // (odd sizes, blocks split at loop points or by automation, one sample at a time), and a tiny block costs no
    // more than a quantum's share. A host whose blocks are multiples of the quantum never has anything left over.
    // (A key or a parameter change that arrives inside a quantum already rendered acts at the start of the next:
    // at most c_quantum - 1 samples late, and only in a host whose blocks are not multiples of it.)
    static_assert(c_maxSection % c_quantum == 0, "the modulators are updated on a quantum's edge");
    if (m_work.getNumSamples() < c_quantum)
    {
        buffer.clear();
        return;
    }

    const int inputChannels = getTotalNumInputChannels();
    const int outChannels = buffer.getNumChannels();
    const int pipe = m_pipeChannels;
    std::array<float*, room::c_maxChannels> work {};
    for (int ch = 0; ch < pipe; ++ch)
        work[static_cast<size_t>(ch)] = m_work.getWritePointer(ch);
    auto nextEvent = midi.cbegin();
    for (int start = 0; start < n;)
    {
        // What is left of the last quantum first.
        if (m_carry > 0)
        {
            const int take = juce::jmin(m_carry, n - start), from = c_quantum - m_carry;
            if (outChannels == 1 && pipe == 2) // Mono, folded from Stereo
            {
                buffer.copyFrom(0, start, m_work, 0, from, take);
                buffer.addFrom(0, start, m_work, 1, from, take);
                buffer.applyGain(0, start, take, 0.5f);
            }
            else
            {
                for (int ch = 0; ch < outChannels; ++ch)
                {
                    if (ch < pipe)
                        buffer.copyFrom(ch, start, m_work, ch, from, take);
                    else
                        buffer.clear(ch, start, take);
                }
            }
            m_carry -= take;
            start += take;
            continue;
        }

        const int len = c_quantum;
        const bool control = m_sinceControl <= 0; // the modulators and the parameters: every c_maxSection samples
        if (control)
        {
            runModulators(c_maxSection);
            runWander(c_maxSection);
            m_sinceControl = c_maxSection;
        }
        m_sinceControl -= len;

        // The keys of this quantum (a thrown whoosh starts at its note's own sample), and any that arrived inside
        // what was already rendered.
        for (; nextEvent != midi.cend() && (*nextEvent).samplePosition < start + len; ++nextEvent)
        {
            handleMidi((*nextEvent).getMessage(), juce::jmax(0, (*nextEvent).samplePosition - start), true);
            m_keysMoved = true; // a key sets pitches: the engine has them before this quantum is rendered
        }
        if (control || m_keysMoved)
            updateEngineParams();
        m_keysMoved = false;
        juce::ignoreUnused(inputChannels);
        const float* const inL = nullptr; // sample-based: the plug-in has no audio input
        const float* const inR = nullptr;

        if (m_renderRate <= m_deviceRate + 0.5)
        {
            renderAtRenderRate(inL, inR, work.data(), len);
        }
        else
        {
            // Render exactly what the downsampler needs for `len` device samples at the high rate (always the
            // same amount ahead, so the latency is constant), low-pass and downsample it, and hold the Ceiling.
            // (The audio input is at the device rate, so it isn't used as a source while rendering high.)
            const int needed = m_down.inputsNeeded(len);
            jassert(needed <= m_highRate.getNumSamples()); // sized for a whole section in prepareInternal
            std::array<float*, room::c_maxChannels> hi {};
            for (int pos = 0; pos < needed; pos += 256)
            {
                for (int ch = 0; ch < pipe; ++ch)
                    hi[static_cast<size_t>(ch)] = m_highRate.getWritePointer(ch) + pos;
                renderAtRenderRate(nullptr, nullptr, hi.data(), juce::jmin(256, needed - pos));
            }
            std::array<const float*, room::c_maxChannels> in {};
            for (int ch = 0; ch < pipe; ++ch)
                in[static_cast<size_t>(ch)] = m_highRate.getReadPointer(ch);
            m_down.process(in.data(), work.data(), len);
            m_monitorLimiter.process(work.data(), pipe, len);
        }

        // Master Trim (what you hear; the tape above already has the full-level signal).
        double preTrimSquares = 0.0;
        float preTrimPeak = 0.0f;
        std::array<float, room::c_maxChannels> channelPeaks {};
        {
            // The Output Follower hears the rig before the trim, so turning Trim down never changes the sound. It
            // reads the room's loudness: each speaker's power weighted as ITU-R BS.1770 weights it (the side
            // surrounds +1.5 dB, the LFE left out), scaled so that Stereo reads as the mean of L and R.
            for (int ch = 0; ch < pipe; ++ch)
            {
                const float* x = work[static_cast<size_t>(ch)];
                const double weight = 0.5 * static_cast<double>(m_loudnessWeight[static_cast<size_t>(ch)]);
                double squares = 0.0;
                float peak = 0.0f;
                for (int i = 0; i < len; ++i)
                {
                    squares += static_cast<double>(x[i]) * x[i];
                    peak = juce::jmax(peak, std::abs(x[i]));
                }
                preTrimSquares += weight * squares;
                channelPeaks[static_cast<size_t>(ch)] = peak;
                preTrimPeak = juce::jmax(preTrimPeak, peak); // the OUT meter: what the tape gets
            }
            if (m_width == room::Width::Mono)
            {
                // One speaker: its meter shows the mono the output is folded to.
                float peak = 0.0f;
                for (int i = 0; i < len; ++i)
                    peak = juce::jmax(peak, std::abs(0.5f * (work[0][i] + work[1][i])));
                channelPeaks = {};
                channelPeaks[0] = peak;
            }
            const float trimDb = m_raw.masterTrim->load();
            m_trimGain.setTargetValue(trimDb <= -59.9f ? 1.0e-4f : juce::Decibels::decibelsToGain(trimDb));
            for (int i = 0; i < len; ++i)
            {
                const float trim = m_trimGain.getNextValue();
                const float g = trim <= 1.0e-4f ? 0.0f : trim;
                for (int ch = 0; ch < pipe; ++ch)
                    work[static_cast<size_t>(ch)][i] *= g;
            }
        }

        m_carry = len; // played by the top of the loop, as far as the host's block goes

        // Scope + meter
        int w = m_scopeWrite.load(std::memory_order_relaxed);
        for (int i = 0; i < len; ++i)
        {
            float v = 0.0f; // the room folded to mono (0.5 (L + R) from Stereo)
            for (int ch = 0; ch < pipe; ++ch)
                v += m_monoFold[static_cast<size_t>(ch)] * work[static_cast<size_t>(ch)][i];
            m_outRing.push(v);
            m_scope[static_cast<size_t>(w)] = v;
            w = (w + 1) & (c_scopeSize - 1);
        }
        m_scopeWrite.store(w, std::memory_order_release);
        // The Followers hear the rig a section at a time (the levels over the last c_maxSection samples).
        m_followSquares += preTrimSquares;
        m_followSamples += len;
        if (m_sinceControl <= 0)
        {
            m_followerLevels[ModMatrix::FollowOutput] = static_cast<float>(std::sqrt(m_followSquares / juce::jmax(1, m_followSamples)));
            m_followSquares = 0.0;
            m_followSamples = 0;
            const auto sections = m_engine.takeSectionLevels();
            m_followerLevels[ModMatrix::FollowBusA] = sections[0];
            m_followerLevels[ModMatrix::FollowBusB] = sections[1];
            m_followerLevels[ModMatrix::FollowHits] = sections[2];
            m_followerLevels[ModMatrix::FollowStow] = sections[RigEngine::SectionStow];
            const auto& master = m_engine.getMasterFollow();
            m_followerLevels[ModMatrix::FollowMaster] = master[0];
            m_followerLevels[ModMatrix::FollowMasterPitch] = master[1];
            m_followerLevels[ModMatrix::FollowMasterBright] = master[2];
            // Where each bus's whoosh and filter are (places, not loudness). The engine that renders tells its own
            // modulators, on this thread: nothing is shared, and a render or an audition reads its own engine.
            const auto places = m_engine.takeFollowerPlaces();
            m_followerLevels[ModMatrix::FollowPassA] = places.pass[0];
            m_followerLevels[ModMatrix::FollowPassB] = places.pass[1];
            m_followerLevels[ModMatrix::FollowFilterA] = places.filter[0];
            m_followerLevels[ModMatrix::FollowFilterB] = places.filter[1];
        }
        publishOutputPeak(preTrimPeak, len);
        publishChannelPeaks(channelPeaks.data(), len);
    }
    // Keys that arrived inside what was already rendered, in a block that rendered nothing new: they act at the
    // start of the next quantum.
    for (; nextEvent != midi.cend(); ++nextEvent)
    {
        handleMidi((*nextEvent).getMessage(), 0, true);
        m_keysMoved = true;
    }
    // A take being auditioned: on top of the output, after the tape, the meters and the trim. The rig and the take
    // are each under the Ceiling; together they are held under it here (only what you hear: the tape has the rig
    // alone). With no take playing nothing is touched.
    if (m_audition.process(buffer, 0, n) || m_sumGuard.isWorking())
        m_sumGuard.process(buffer, 0, n, juce::Decibels::decibelsToGain(m_raw.ceiling->load()));
}

SwooshbucklerAudioProcessor::LfeFeed SwooshbucklerAudioProcessor::lfeFeedFor(room::Width width, int lfeMode, bool stowawayOn, int stowawayMode, bool trackOnStowaway)
{
    if (! room::hasLfe(width))
        return LfeFeed::NoLfe;
    if (lfeMode == RigEngine::LfeMixdown)
        return LfeFeed::Mixdown;
    if (lfeMode != RigEngine::LfeDiscrete)
        return LfeFeed::Off;
    if (! stowawayOn || stowawayMode != ParamIDs::StowSub)
        return LfeFeed::NoSub;
    return trackOnStowaway ? LfeFeed::Sub : LfeFeed::NoSubTrack;
}

SwooshbucklerAudioProcessor::LfeFeed SwooshbucklerAudioProcessor::getLfeFeedAt(room::Width width) const
{
    const int mode = juce::jlimit(0, ParamIDs::numLfeModes - 1, juce::roundToInt(m_raw.roomLfeMode->load()));
    return lfeFeedFor(width, mode, isStowawayOn(), stowMode(), hasTrackOnStowaway());
}

bool SwooshbucklerAudioProcessor::hasTrackOnStowaway() const
{
    for (int track = 0; track < CargoHold::c_numTracks; ++track)
        if (m_cargo.getLive(track) != nullptr && m_cargo.getBus(track) == CargoHold::c_stowBus)
            return true;
    return false;
}

juce::String SwooshbucklerAudioProcessor::lfeFeedReason(LfeFeed feed)
{
    if (feed == LfeFeed::NoSub)
        return "Nothing feeds the LFE: Discrete plays Stowaway's Sub, and Stowaway is off or not in Sub. Switch Stowaway on in Sub mode, or choose Mixdown.";
    if (feed == LfeFeed::NoSubTrack)
        return "Nothing feeds the LFE: Discrete plays Stowaway's Sub, and no track is on Stowaway. Send a track to Stowaway, or choose Mixdown.";
    return {};
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
    {
        m_keysDown = {};
        if (msg.isAllSoundOff())
            m_sustain = false;
    }
    else if (msg.isResetAllControllers())
        m_sustain = false;
    else if (msg.isController() && msg.getControllerNumber() < 120) // 120 and up are channel mode messages, not controls
    {
        // The sustain pedal holds Swell, whatever Keys is set to (it may be learned for a control as well).
        if (msg.isSustainPedalOn())
            m_sustain = true;
        else if (msg.isSustainPedalOff())
            m_sustain = false;
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
    updateMidiSwell();
}

bool SwooshbucklerAudioProcessor::updateMidiSwell()
{
    const bool held = m_sustain || (keysMode() == KeysSwell && (m_keysDown[0] | m_keysDown[1]) != 0);
    m_midiSwell.store(held, std::memory_order_relaxed);
    return held;
}

int SwooshbucklerAudioProcessor::getSwellSources() const
{
    int sources = 0;
    if (m_raw.swell->load(std::memory_order_relaxed) > 0.5f) sources |= SwellFromParameter;
    if (m_uiSwell.load(std::memory_order_relaxed)) sources |= SwellFromPress;
    if (m_midiSwell.load(std::memory_order_relaxed)) sources |= SwellFromMidi;
    if (m_mods.getSwellGateView()) sources |= SwellFromWire;
    if (m_engine.isAutoSwelling()) sources |= SwellFromAuto;
    return sources;
}

void SwooshbucklerAudioProcessor::setSwellParameter(bool held)
{
    auto* param = m_state.getParameter(ParamIDs::swell);
    if (param == nullptr)
        return;
    if (held)
    {
        if (! m_swellGesture)
            param->beginChangeGesture();
        m_swellGesture = true;
        param->setValueNotifyingHost(1.0f);
    }
    else
    {
        param->setValueNotifyingHost(0.0f);
        if (m_swellGesture)
            param->endChangeGesture();
        m_swellGesture = false;
    }
}

OutputEq::Params SwooshbucklerAudioProcessor::getOutputEqParams() const
{
    auto get = [this](int k) { return m_raw.eq[k]->load(std::memory_order_relaxed); };
    OutputEq::Params p;
    p.on = get(0) > 0.5f;
    p.lowCutHz = Params::eqLowCutIsOff(get(1)) ? 0.0f : get(1);
    p.lowCutSlope = juce::roundToInt(get(2)) == 0 ? 12 : 24;
    p.lowShelfHz = get(3);
    p.lowShelfDb = get(4);
    p.lowMidHz = get(5);
    p.lowMidDb = get(6);
    p.lowMidQ = get(7);
    p.highMidHz = get(8);
    p.highMidDb = get(9);
    p.highMidQ = get(10);
    p.highShelfHz = get(11);
    p.highShelfDb = get(12);
    p.highCutHz = Params::eqHighCutIsOff(get(13)) ? 0.0f : get(13);
    return p;
}

juce::String SwooshbucklerAudioProcessor::describeDetail() const
{
    juce::StringArray items;
    for (int b = 0; b < ParamIDs::numBuses; ++b)
        for (int k = 0; k < ParamIDs::numBusDetails; ++k)
            if (auto* param = m_state.getParameter(ParamIDs::busDetail[b][k]);
                param != nullptr && Presets::isDetailParameter(param->getParameterID()) && std::abs(param->getValue() - param->getDefaultValue()) > 1.0e-6f)
                items.add(param->getName(40) + " " + param->getCurrentValueAsText());
    if (isOutputEqActive())
    {
        juce::StringArray eq;
        for (int k = 1; k < ParamIDs::numEqParams; ++k)
            if (auto* param = m_state.getParameter(ParamIDs::eqIds[k]); param != nullptr && std::abs(param->getValue() - param->getDefaultValue()) > 1.0e-6f)
                eq.add(param->getName(40).fromFirstOccurrenceOf("EQ ", false, false) + " " + param->getCurrentValueAsText());
        items.add("Output EQ: " + eq.joinIntoString(", "));
    }
    return items.isEmpty() ? juce::String() : items.joinIntoString(", ") + ".";
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

void SwooshbucklerAudioProcessor::RetroPump::timerCallback()
{
    // A host's transport or a key played the rig before anything on this thread asked for the saved last minutes.
    owner.m_recorder.allocateRetroIfAsked();
    if (owner.m_recorder.isRetroInUse())
        stopTimer(); // taken (here, or by Play or arm): prepare keeps it from now on
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
        storeSetting("midiControls", pairs.joinIntoString(","));
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
    if (on && ! m_offline && juce::MessageManager::existsAndIsCurrentThread())
        m_recorder.useRetro(); // the saved last minutes' memory, before the first sound (it allocates)
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

void SwooshbucklerAudioProcessor::publishChannelPeaks(const float* peaks, int numSamples)
{
    // Each speaker's meter falls back like the OUT meter (see publishOutputPeak). nullptr: silence.
    const float decay = std::exp(-static_cast<float>(numSamples) / (0.3f * static_cast<float>(juce::jmax(1.0, m_deviceRate))));
    for (size_t ch = 0; ch < m_channelPeak.size(); ++ch)
    {
        const float peak = peaks != nullptr ? peaks[ch] : 0.0f;
        m_channelPeak[ch].store(juce::jmax(peak, m_channelPeak[ch].load(std::memory_order_relaxed) * decay), std::memory_order_relaxed);
        if (peak > m_channelPeakHold[ch].load(std::memory_order_relaxed))
            m_channelPeakHold[ch].store(peak, std::memory_order_relaxed);
    }
}

float SwooshbucklerAudioProcessor::getChannelPeak(int channel) const
{
    return channel >= 0 && channel < room::c_maxChannels ? m_channelPeak[static_cast<size_t>(channel)].load(std::memory_order_relaxed) : 0.0f;
}

float SwooshbucklerAudioProcessor::takeChannelPeak(int channel)
{
    return channel >= 0 && channel < room::c_maxChannels ? m_channelPeakHold[static_cast<size_t>(channel)].exchange(0.0f, std::memory_order_relaxed) : 0.0f;
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
    storeSetting("recentSessions", paths.joinIntoString("\n"));
}

void SwooshbucklerAudioProcessor::clearRecentSessions()
{
    settingsfile::remove(*m_settings, "recentSessions");
}

bool SwooshbucklerAudioProcessor::isUserEdit(int) const
{
    // Nothing moves a parameter by itself: the morph wander flies the dot inside the processor and leaves X and Y alone.
    return true;
}

bool SwooshbucklerAudioProcessor::arm(juce::String& error)
{
    m_recorder.useRetro();
    m_recorder.setSignalGate(getSkipSilence());
    auto o = tapeOptions();
    o.perWhoosh = getOneFilePerWhoosh();
    refreshTakes();
    m_filesThisTake = 0;
    updateTapeNaming();
    return m_recorder.start(getRecordFolder(), o, error);
}

bool SwooshbucklerAudioProcessor::startRecording(juce::String& error)
{
    m_recorder.useRetro();
    // One continuous take in one file, whatever the settings say about whooshes.
    const auto o = tapeOptions();
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
    storeSetting("recordRate", rate);
    if (m_deviceRate > 0.0 && ! isRecording())
    {
        // Re-render at the new rate (the tape stays closed while it changes).
        suspendProcessing(true);
        prepareInternal();
        suspendProcessing(false);
    }
}

int SwooshbucklerAudioProcessor::getFileType() const { return juce::jlimit(0, 2, m_settings->getIntValue("fileType", 0)); }
void SwooshbucklerAudioProcessor::setFileType(int type) { storeSetting("fileType", juce::jlimit(0, 2, type)); }
SwooshbucklerAudioProcessor::TapeFold SwooshbucklerAudioProcessor::getTapeFold() const
{
    return static_cast<TapeFold>(juce::jlimit(0, 2, m_settings->getIntValue("tapeFold", 0)));
}

void SwooshbucklerAudioProcessor::setTapeFold(TapeFold fold)
{
    storeSetting("tapeFold", static_cast<int>(fold));
}

room::Width SwooshbucklerAudioProcessor::getTapeWidth() const
{
    const auto room = getRoomWidth();
    switch (getTapeFold())
    {
        case TapeFold::Mono:   return room::Width::Mono;
        case TapeFold::Stereo: return room::canFold(room, room::Width::Stereo) ? room::Width::Stereo : room;
        case TapeFold::AsPlayed:
        default:               return room;
    }
}

bool SwooshbucklerAudioProcessor::setListening(room::Width width)
{
    if (! isApp())
        return false; // in a host the track decides
    if (width == getListening())
        return true;
    if (isRecording())
        return false; // the tape is written at the room's width: it cannot change under a take
    m_listening.store(static_cast<int>(width));
    storeSetting("listening", static_cast<int>(width));
    if (onListeningChanged)
        onListeningChanged();
    return true;
}

void SwooshbucklerAudioProcessor::setDeviceOutputs(int switchedOn, int available)
{
    m_deviceOutputs.store(juce::jmax(0, switchedOn), std::memory_order_relaxed);
    m_deviceOutputsAvailable.store(juce::jmax(0, available), std::memory_order_relaxed);
}

bool SwooshbucklerAudioProcessor::listeningFolded(room::Width width, int deviceOutputs)
{
    return deviceOutputs > 0 && deviceOutputs < room::layout(width).numChannels;
}

juce::String SwooshbucklerAudioProcessor::listeningStatus() const
{
    return isApp() ? listeningStatus(getListening(), getDeviceOutputs(), getDeviceOutputsAvailable()) : juce::String();
}

juce::String SwooshbucklerAudioProcessor::listeningStatus(room::Width width, int deviceOutputs, int available)
{
    if (! listeningFolded(width, deviceOutputs))
        return {};
    juce::String s;
    s << "Playing " << room::layout(width).name << " folded to " << (deviceOutputs >= 2 ? "stereo" : "mono") << ": ";
    if (available > deviceOutputs)
        s << deviceOutputs << " of this audio device's " << available << " outputs are switched on.";
    else
        s << "this audio device has " << deviceOutputs << (deviceOutputs == 1 ? " output." : " outputs.");
    return s;
}

juce::String SwooshbucklerAudioProcessor::listeningLine(room::Width width, int deviceOutputs, int available)
{
    const auto& room = room::layout(width);
    if (deviceOutputs <= 0)
        return "No audio device is playing. Choose one with Audio Device.";
    if (listeningFolded(width, deviceOutputs))
        return listeningStatus(width, deviceOutputs, available) + " The tape and Render Variations still get all "
               + juce::String(room.numChannels) + " channels.";
    if (width == room::Width::Mono)
        return deviceOutputs >= 2 ? "The one channel plays on the first two outputs switched on."
                                  : "The one channel plays on the one output.";
    juce::String order;
    const auto channels = room::deviceChannels(width);
    for (int ch = 0; ch < room.numChannels; ++ch)
        order << (ch > 0 ? " " : "") << room::info(room.speakers[static_cast<size_t>(channels[static_cast<size_t>(ch)])]).name;
    return "Plays on the first " + juce::String(room.numChannels) + " outputs switched on, in this order: " + order + ".";
}

room::FileOrder SwooshbucklerAudioProcessor::getChannelOrder() const
{
    return m_settings->getIntValue("channelOrder", 0) == 1 ? room::FileOrder::WavStandard : room::FileOrder::ProTools;
}

void SwooshbucklerAudioProcessor::setChannelOrder(room::FileOrder order)
{
    storeSetting("channelOrder", order == room::FileOrder::WavStandard ? 1 : 0);
}

takefile::Layout SwooshbucklerAudioProcessor::getFileLayout() const
{
    return m_settings->getIntValue("fileLayout", 0) == 1 ? takefile::Layout::MultiMono : takefile::Layout::Interleaved;
}

void SwooshbucklerAudioProcessor::setFileLayout(takefile::Layout layout)
{
    storeSetting("fileLayout", layout == takefile::Layout::MultiMono ? 1 : 0);
}

TapeRecorder::Options SwooshbucklerAudioProcessor::tapeOptions() const
{
    TapeRecorder::Options o;
    o.bitDepth = getBitDepth();
    o.fileRate = static_cast<double>(getRecordRate()); // 0 = the session rate
    o.type = static_cast<TapeRecorder::FileType>(getFileType());
    // The tape gets the pipe (a Mono room is carried as Stereo, so As Played there is the Mono fold of it).
    const auto width = getTapeWidth();
    o.mono = width == room::Width::Mono;
    o.stereo = width == room::Width::Stereo;
    o.played = getRoomWidth();
    o.order = getChannelOrder();
    o.layout = getFileLayout();
    if (m_raw.ceiling != nullptr)
        o.foldCeilingDb = m_raw.ceiling->load();
    return o;
}

juce::String SwooshbucklerAudioProcessor::retroLabel() const
{
    // What the button saves: the length asked for, or (where the memory cap cuts it short) what it really holds.
    const int asked = getRetroMinutes() * 60;
    const int kept = static_cast<int>(getRetroSecondsKept());
    if (kept > 0 && kept < asked)
        return kept < 60 ? "Last " + juce::String(kept) + " s" : juce::String::formatted("Last %d:%02d", kept / 60, kept % 60);
    return asked == 60 ? juce::String("Last 60 s") : "Last " + juce::String(asked / 60) + " Min";
}

void SwooshbucklerAudioProcessor::stopRecording()
{
    m_recorder.stop();
    refreshTakes();
}

bool SwooshbucklerAudioProcessor::saveRetro(juce::File& written, juce::String& error)
{
    // The same file type, bit depth, sample rate, channels, order and layout as the tape.
    updateTapeNaming();
    const auto options = tapeOptions();
    if (! m_recorder.saveRetro(getRecordFolder(), options, written, error))
        return false;
    registerTake(written, retroLabel(), -1.0, 0, TapeRecorder::foldedFrom(options, m_pipeChannels));
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
void SwooshbucklerAudioProcessor::setVariationCount(int count) { storeSetting("variationCount", juce::jlimit(1, VariationRenderer::c_maxCount, count)); }
int SwooshbucklerAudioProcessor::getVariationMode() const { return juce::jlimit(0, 2, m_settings->getIntValue("variationMode", 0)); }
void SwooshbucklerAudioProcessor::setVariationMode(int mode) { storeSetting("variationMode", juce::jlimit(0, 2, mode)); }
float SwooshbucklerAudioProcessor::getVariationAmount() const { return juce::jlimit(0.0f, 1.0f, static_cast<float>(m_settings->getDoubleValue("variationAmount", 0.3))); }
void SwooshbucklerAudioProcessor::setVariationAmount(float amount) { storeSetting("variationAmount", static_cast<double>(juce::jlimit(0.0f, 1.0f, amount))); }
juce::String SwooshbucklerAudioProcessor::getVariationFolder() const { return m_settings->getValue("variationFolder").trim(); }
void SwooshbucklerAudioProcessor::setVariationFolder(const juce::String& path) { storeSetting("variationFolder", path.trim()); }
juce::String SwooshbucklerAudioProcessor::getVariationValue(const juce::String& key, const juce::String& fallback) const { return m_settings->getValue("variation" + key, fallback); }
void SwooshbucklerAudioProcessor::setVariationValue(const juce::String& key, const juce::String& value)
{
    if (m_settings->getValue("variation" + key) == value && m_settings->containsKey("variation" + key))
        return;
    storeSetting("variation" + key, value);
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
    // Nothing held: Swell let go, and each Varispeed back in the middle (a hand on it is not a setting).
    for (const char* id : { ParamIDs::swell, ParamIDs::busDetail[0][ParamIDs::Varispeed], ParamIDs::busDetail[1][ParamIDs::Varispeed] })
        if (auto* param = copy->m_state.getParameter(id))
            param->setValueNotifyingHost(param->getDefaultValue());
    m_cargo.beginLending();
    copy->m_cargo.shareFrom(m_cargo);
    return copy;
}

void SwooshbucklerAudioProcessor::prepareOffline(double sampleRate, std::uint32_t seed, room::Width width)
{
    jassert(m_offline);
    // Stopped, nothing held, nothing waiting.
    m_playing.store(false);
    m_restartPending.store(false);
    m_uiThrows.store(0);
    m_uiSwell.store(false);
    m_sustain = false;
    m_midiSwell.store(false);
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
    // The room it plays in: the output bus as JUCE's own set for the width (a discrete set of the same count would
    // not be a room), whatever the live rig's track or device is. An offline copy is no app, so every width is
    // allowed (isBusesLayoutSupported).
    auto layout = getBusesLayout();
    if (! layout.outputBuses.isEmpty())
        layout.outputBuses.getReference(0) = room::channelSet(width);
    for (auto& input : layout.inputBuses)
        input = juce::AudioChannelSet::disabled();
    const bool set = setBusesLayout(layout);
    jassert(set);
    juce::ignoreUnused(set);
    setRateAndBufferSizeDetails(sampleRate, c_maxSection);
    prepareToPlay(sampleRate, c_maxSection); // empties the engine, the modulators and the output stage
    // The morph wander sets off from where the X / Y controls are.
    m_wanderOn = false;
    m_wanderBlend = 0.0f;
    m_morphX = m_raw.morphX->load();
    m_morphY = m_raw.morphY->load();
}

void SwooshbucklerAudioProcessor::carryOn(const RigEngine::Places& places)
{
    jassert(m_offline);
    m_restartPending.store(false);
    m_engine.setPlaces(places);
}

void SwooshbucklerAudioProcessor::scatter(std::uint32_t seed)
{
    jassert(m_offline);
    juce::Random random(static_cast<juce::int64>(seed));
    m_mods.scatter(random);
    m_engine.scatterRide(static_cast<std::uint32_t>(random.nextInt()) | 1u);
}

//==============================================================================
// Names, descriptions and the takes
bool SwooshbucklerAudioProcessor::getOneFilePerWhoosh() const { return m_settings->getBoolValue("oneFilePerWhoosh", false); }

void SwooshbucklerAudioProcessor::setOneFilePerWhoosh(bool on)
{
    if (on == getOneFilePerWhoosh())
        return;
    storeSetting("oneFilePerWhoosh", on);
    if (isRecording())
    {
        // The open take is written the old way: close it and carry on in a new one.
        stopRecording();
        juce::String error;
        arm(error);
    }
}

juce::String SwooshbucklerAudioProcessor::getTakeName() const { return m_settings->getValue("takeName").trim(); }
void SwooshbucklerAudioProcessor::setTakeName(const juce::String& name) { storeSetting("takeName", name.trim()); updateTapeNaming(); }
juce::String SwooshbucklerAudioProcessor::getCreator() const { return m_settings->getValue("creator").trim(); }
void SwooshbucklerAudioProcessor::setCreator(const juce::String& creator) { storeSetting("creator", creator.trim()); updateTapeNaming(); }
int SwooshbucklerAudioProcessor::getNameStyle() const { return juce::jlimit(0, 1, m_settings->getIntValue("nameStyle", 0)); }
void SwooshbucklerAudioProcessor::setNameStyle(int style) { storeSetting("nameStyle", juce::jlimit(0, 1, style)); updateTapeNaming(); }

int SwooshbucklerAudioProcessor::getRetroMinutes() const
{
    const int minutes = m_settings->getIntValue("retroMinutes", 1);
    return minutes == 3 || minutes == 5 ? minutes : 1;
}

void SwooshbucklerAudioProcessor::setRetroMinutes(int minutes)
{
    storeSetting("retroMinutes", minutes == 3 || minutes == 5 ? minutes : 1);
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

juce::String SwooshbucklerAudioProcessor::describeRoom(room::Width width) const
{
    if (room::layout(width).numChannels <= 2)
        return {};
    auto text = [this](const char* id)
    {
        auto* param = m_state.getParameter(id);
        return param != nullptr ? param->getCurrentValueAsText() : juce::String();
    };
    // A Flight Path with the way and the side its passes are held to, where they are: "Through (Front to Back, Left)".
    auto flight = [this, &text](const char* pathId, const char* directionId, const char* sideId)
    {
        auto choice = [this](const char* id) { return juce::roundToInt(m_state.getRawParameterValue(id)->load()); };
        const int path = choice(pathId);
        juce::StringArray held;
        if (const int way = choice(directionId); way != ParamIDs::DirectionRandom)
            held.add(Params::directionLabel(path, way));
        if (const int which = choice(sideId); which != ParamIDs::SideRandom && Params::sideLabel(path, which) != nullptr)
            held.add(Params::sideLabel(path, which));
        return text(pathId) + (held.isEmpty() ? juce::String() : " (" + held.joinIntoString(", ") + ")");
    };
    juce::String d;
    d << "Flight Paths A " << flight(ParamIDs::flightPath[0], ParamIDs::direction[0], ParamIDs::side[0])
      << ", B " << (m_raw.linkBuses != nullptr && m_raw.linkBuses->load() > 0.5f ? juce::String("as A (Link)")
                                                                                 : flight(ParamIDs::flightPath[1], ParamIDs::direction[1], ParamIDs::side[1]))
      // (Stowaway's only while it flies: on, in Fly. Off or in another mode nothing of it takes a path.)
      << (isStowawayOn() && stowMode() == ParamIDs::StowFly
              ? ", Stowaway " + flight(ParamIDs::stowFlightPath, ParamIDs::stowDirection, ParamIDs::stowSide) : juce::String())
      << ". Spread " << text(ParamIDs::roomSpread) << ", Height " << text(ParamIDs::roomHeight) << ", Height Varies " << text(ParamIDs::roomHeightVaries)
      << ", Centre Speaker " << text(ParamIDs::roomCentre) << ", LFE " << text(ParamIDs::roomLfeMode);
    if (const auto* mode = m_state.getRawParameterValue(ParamIDs::roomLfeMode); mode != nullptr && mode->load() > 0.5f)
        d << " " << text(ParamIDs::roomLfeLevel);
    return d + ".";
}

juce::String SwooshbucklerAudioProcessor::describeSettings() const
{
    // The most telling first, because a WAV file's BWF description keeps only its first 256 bytes (cut after a
    // whole item): the version, the room, the preset, then the controls that say most about how the take was made.
    auto text = [this](const char* id)
    {
        auto* param = m_state.getParameter(id);
        return param != nullptr ? param->getCurrentValueAsText() : juce::String();
    };
    const auto width = getRoomWidth();
    juce::String d;
    d << "Swooshbuckler " << Version::number() << ". " << room::layout(width).name << ".";
    if (const auto roomText = describeRoom(width); roomText.isNotEmpty())
        d << " " << roomText;
    d << " Preset: " << currentPresetName
      << ". Energy " << text(ParamIDs::energy) << ", Rate " << text(ParamIDs::rate) << ", Fill " << text(ParamIDs::fill)
      << ", Chaos " << text(ParamIDs::chaos) << ", Pitch " << text(ParamIDs::pitch) << ", Space " << text(ParamIDs::space)
      << ". Pass A " << text(ParamIDs::passMode[0]) << ", Pass B " << text(ParamIDs::passMode[1])
      << ". Morph " << text(ParamIDs::morphOn) << ", Aim " << text(ParamIDs::hitAim)
      << ", Hits " << text(ParamIDs::hitPosition) << ", Hit Spread " << text(ParamIDs::hitSpread) << ".";
    if (const auto detail = describeDetail(); detail.isNotEmpty())
        d << " " << detail;
    return d;
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
        registerTake(take.file, take.whoosh ? "Whoosh" : "Tape", take.seconds, take.markers, take.foldedFrom);
    }
    if (m_variations != nullptr)
        for (const auto& written : m_variations->takeWritten())
            registerTake(written.file, written.markers > 0 ? "Variations" : "Variation", written.seconds, written.markers); // one file that holds them all is one take
}

juce::String SwooshbucklerAudioProcessor::widthTag(const Take& take)
{
    const juce::String width(room::layout(take.format.width).name);
    return take.foldedFrom.has_value() ? width + " Fold" : width;
}

void SwooshbucklerAudioProcessor::registerTake(const juce::File& fileToAdd, const juce::String& kindToShow, double seconds, int markers,
                                               std::optional<room::Width> foldedFrom)
{
    const juce::File file(fileToAdd);     // copies: the caller may have handed us a take of the list that is about
    const juce::String kind(kindToShow);  // to be replaced
    if (! juce::MessageManager::existsAndIsCurrentThread())
    {
        juce::WeakReference<SwooshbucklerAudioProcessor> weak(this);
        juce::MessageManager::callAsync([weak, file, kind, seconds, markers, foldedFrom]
        {
            if (auto* self = weak.get())
                self->registerTake(file, kind, seconds, markers, foldedFrom);
        });
        return;
    }
    // The take the file belongs to: a Multi-Mono set is one take, whichever of its files is handed in.
    const auto found = takefile::find(file);
    if (seconds < 0.0)
        if (const auto reader = takefile::openReader(found))
            seconds = reader->sampleRate > 0.0 ? static_cast<double>(reader->lengthInSamples) / reader->sampleRate : 0.0;
    const auto first = found.files.isEmpty() ? file : found.files[0];
    for (auto it = m_takes.begin(); it != m_takes.end();) // the same take again (written over): one entry
        it = it->file == first ? m_takes.erase(it) : it + 1;
    Take take;
    take.id = m_nextTakeId++;
    take.file = first;
    take.files = found.files.isEmpty() ? juce::Array<juce::File> { file } : found.files;
    take.format = found.format;
    take.foldedFrom = foldedFrom;
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
                m_audition.close(); // it holds the files open, and an open file can't be moved everywhere
            bool all = true;
            for (const auto& f : it->files)
                if (f.existsAsFile() && ! (toTrash ? f.moveToTrash() : f.deleteFile()))
                    all = false;
            if (! all)
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
    storeSetting("recordFolder", folder.getFullPathName());
}

int SwooshbucklerAudioProcessor::getBitDepth() const
{
    const int bits = m_settings->getIntValue("bitDepth", 24);
    return (bits == 16 || bits == 24 || bits == 32) ? bits : 24;
}

void SwooshbucklerAudioProcessor::setBitDepth(int bits)
{
    storeSetting("bitDepth", bits);
}

bool SwooshbucklerAudioProcessor::getAutoRecord() const
{
    return m_settings->getBoolValue("autoRecord", false);
}

void SwooshbucklerAudioProcessor::setAutoRecord(bool on)
{
    storeSetting("autoRecord", on);
}

bool SwooshbucklerAudioProcessor::getSkipSilence() const
{
    return m_settings->getBoolValue("skipSilence", true);
}

void SwooshbucklerAudioProcessor::setSkipSilence(bool on)
{
    storeSetting("skipSilence", on);
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

    // A lean on Varispeed is a hand on the wheel, not a setting: a state saved while one was held comes back with
    // the wheel in the middle (left leaning, the offset would run to its end stop and stay there).
    for (int b = 0; b < ParamIDs::numBuses; ++b)
        if (auto lean = tree.getChildWithProperty("id", ParamIDs::busDetail[b][ParamIDs::Varispeed]); lean.isValid())
            lean.setProperty("value", ParamIDs::busDetailDefaults[ParamIDs::Varispeed], nullptr);
    // Nor is a press of Swell: a state saved while it was held (a host saving mid-press) comes back with Swell let
    // go. A host that automates it writes its own value as it plays.
    if (auto held = tree.getChildWithProperty("id", ParamIDs::swell); held.isValid())
        held.setProperty("value", 0.0f, nullptr);

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

void SwooshbucklerAudioProcessor::storeSetting(const juce::String& key, const juce::var& value)
{
    // Only this key goes into the file as it is now: another instance's (or the app's) newer values stay.
    settingsfile::store(*m_settings, key, value);
}

juce::AudioProcessorEditor* SwooshbucklerAudioProcessor::createEditor()
{
    return new SwooshbucklerAudioProcessorEditor(*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SwooshbucklerAudioProcessor();
}
