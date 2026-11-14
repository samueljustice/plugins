#include "VariationRenderer.h"
#include "PluginProcessor.h"
#include "Presets.h"
#include "Version.h"

namespace
{
    constexpr int c_block = SwooshbucklerAudioProcessor::c_maxSection;

    juce::String safeName(const juce::String& name)
    {
        const auto safe = juce::File::createLegalFileName(name.trim()).substring(0, 80).trim();
        return safe.isEmpty() ? juce::String("Swooshbuckler") : safe;
    }

    /** The highest number among the files called <name>_<digits>.* in a folder (0 = none). */
    int highestNumberIn(const juce::File& folder, const juce::String& name)
    {
        int highest = 0;
        for (const auto& entry : juce::RangedDirectoryIterator(folder, false, name + "_*", juce::File::findFiles))
        {
            const auto digits = entry.getFile().getFileNameWithoutExtension().substring(name.length() + 1);
            if (digits.isNotEmpty() && digits.length() <= 6 && digits.containsOnly("0123456789"))
                highest = juce::jmax(highest, digits.getIntValue());
        }
        return highest;
    }

    bool isSwitch(const juce::RangedAudioParameter* param)
    {
        return dynamic_cast<const juce::AudioParameterChoice*>(param) != nullptr
            || dynamic_cast<const juce::AudioParameterBool*>(param) != nullptr;
    }
}

juce::String VariationRenderer::modeName(Mode mode)
{
    switch (mode)
    {
        case Mode::SmallChanges:        return "My Settings, Small Changes";
        case Mode::RandomiseEverything: return "Randomise Everything";
        case Mode::NewPasses:
        default:                        return "My Settings, New Passes";
    }
}

juce::String VariationRenderer::eachName(Each each) { return each == Each::WholeRide ? "Whole Ride" : "One Whoosh"; }

juce::String VariationRenderer::namesName(Names names)
{
    switch (names)
    {
        case Names::DateAndTime:         return "Date And Time";
        case Names::PresetAndCounter:    return "Preset And Counter";
        case Names::BatchNameAndCounter: return "Batch Name And Counter";
        case Names::Counter:
        default:                         return "Counter";
    }
}

juce::String VariationRenderer::filesName(Files files)
{
    switch (files)
    {
        case Files::OneWithMarkers: return "One File With Markers";
        case Files::Both:           return "Both";
        case Files::PerVariation:
        default:                    return "One File Per Variation";
    }
}

std::uint32_t VariationRenderer::seedOf(std::uint32_t batchSeed, int index)
{
    // A well-mixed 32-bit hash of the pair (the finaliser of MurmurHash3), never 0.
    std::uint32_t h = batchSeed ^ (0x9E3779B9u * static_cast<std::uint32_t>(index + 1));
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return h != 0 ? h : 0x5EA5EAu;
}

juce::String VariationRenderer::fileNameOf(const juce::String& name, int number, int highestNumber)
{
    const int digits = juce::jmax(3, juce::String(highestNumber).length());
    return safeName(name) + "_" + juce::String(number).paddedLeft('0', digits);
}

juce::File VariationRenderer::datedFolder(const juce::File& recordings, const juce::String& name, juce::Time when)
{
    return recordings.getChildFile(safeName(name) + " Variations " + when.formatted("%Y-%m-%d %H-%M-%S"));
}

VariationRenderer::Plan VariationRenderer::planOf(const Job& job)
{
    Plan plan;
    switch (job.names)
    {
        case Names::DateAndTime:         plan.stem = safeName(job.name) + "_" + job.when.formatted("%Y-%m-%d_%H%M"); break;
        case Names::PresetAndCounter:    plan.stem = safeName(job.preset); break;
        case Names::BatchNameAndCounter: plan.stem = job.batchName.trim().isEmpty() ? juce::String("Batch") : safeName(job.batchName); break;
        case Names::Counter:
        default:                         plan.stem = safeName(job.name); break;
    }
    // Numbers carry on after the files the folder already has, so nothing is ever written over.
    plan.firstNumber = highestNumberIn(job.folder, plan.stem) + 1;
    plan.lastNumber = plan.firstNumber + juce::jlimit(1, c_maxCount, job.count) - 1;
    plan.firstName = fileNameOf(plan.stem, plan.firstNumber, plan.lastNumber);
    const auto extension = TapeRecorder::extensionOf(job.options.type);
    plan.combined = job.folder.getChildFile(plan.stem + "_All" + extension);
    for (int n = 2; plan.combined.exists(); ++n)
        plan.combined = job.folder.getChildFile(plan.stem + "_All_" + juce::String(n) + extension);
    return plan;
}

//==============================================================================
// The batches rendered lately: one to a line of the setting, newest first, as
// number|time|count|mode|amount|each|seconds|ride seconds|name (a name made safe for a file has no | or ; in it).
std::vector<VariationRenderer::Batch> VariationRenderer::recentBatches() const
{
    std::vector<Batch> batches;
    for (const auto& line : juce::StringArray::fromTokens(m_rig.getVariationValue("Batches"), ";", ""))
    {
        const auto f = juce::StringArray::fromTokens(line, "|", "");
        if (f.size() < 9 || f[0].getLargeIntValue() <= 0)
            continue;
        Batch b;
        b.number = static_cast<std::uint32_t>(f[0].getLargeIntValue());
        b.when = juce::Time(f[1].getLargeIntValue());
        b.count = juce::jlimit(1, c_maxCount, f[2].getIntValue());
        b.mode = static_cast<Mode>(juce::jlimit(0, 2, f[3].getIntValue()));
        b.amount = juce::jlimit(0.0f, 1.0f, f[4].getFloatValue());
        b.each = static_cast<Each>(juce::jlimit(0, 1, f[5].getIntValue()));
        b.seconds = juce::jlimit(c_minSeconds, c_maxSeconds, f[6].getDoubleValue());
        b.rideSeconds = juce::jlimit(c_minRideSeconds, c_maxRideSeconds, f[7].getDoubleValue());
        b.name = f[8];
        batches.push_back(b);
    }
    return batches;
}

void VariationRenderer::rememberBatch(const Batch& batch)
{
    auto batches = recentBatches();
    for (auto it = batches.begin(); it != batches.end();) // rendered again: it moves to the front
        it = it->number == batch.number ? batches.erase(it) : it + 1;
    batches.insert(batches.begin(), batch);
    if (batches.size() > static_cast<size_t>(c_maxBatches))
        batches.resize(static_cast<size_t>(c_maxBatches));
    juce::StringArray lines;
    for (const auto& b : batches)
        lines.add(juce::StringArray { juce::String(b.number), juce::String(b.when.toMilliseconds()), juce::String(b.count),
                                      juce::String(static_cast<int>(b.mode)), juce::String(b.amount, 3), juce::String(static_cast<int>(b.each)),
                                      juce::String(b.seconds, 3), juce::String(b.rideSeconds, 3), safeName(b.name).removeCharacters("|;") }
                      .joinIntoString("|"));
    m_rig.setVariationValue("Batches", lines.joinIntoString(";"));
}

juce::String VariationRenderer::batchTitle(const Batch& batch)
{
    const auto& t = batch.when;
    return batch.name + ", " + juce::String(t.getDayOfMonth()) + " " + t.getMonthName(true) + " " + juce::String(t.getYear()) + ", "
         + juce::String(t.getHours()).paddedLeft('0', 2) + ":" + juce::String(t.getMinutes()).paddedLeft('0', 2);
}

//==============================================================================
juce::String VariationRenderer::roughly(double seconds)
{
    // Halves round up (juce::roundToInt would say 2.5 minutes is "about 2").
    auto whole = [](double x) { return juce::String(static_cast<int>(std::floor(x + 0.5))); };
    if (seconds < 1.5)
        return "about a second";
    if (seconds < 89.5)
        return "about " + whole(seconds) + " seconds";
    if (seconds < 89.5 * 60.0)
        return "about " + whole(seconds / 60.0) + " minutes";
    return "about " + whole(seconds / 3600.0) + " hours";
}

VariationRenderer::Estimate VariationRenderer::estimate(const Job& job) const
{
    // How fast this machine rendered the last batch of the kind (seconds of sound kept per second of work), and
    // how long a whoosh's tail rang on average; until a batch has been rendered, what a laptop of 2021 does.
    const bool ride = job.each == Each::WholeRide;
    const double speed = juce::jlimit(0.5, 500.0, m_rig.getVariationValue(ride ? "SpeedRide" : "SpeedWhoosh", ride ? "20" : "12").getDoubleValue());
    const double tail = juce::jlimit(0.0, static_cast<double>(SwooshbucklerAudioProcessor::c_tailMaxSeconds), m_rig.getVariationValue("Tail", "1.2").getDoubleValue());
    const int count = juce::jlimit(1, c_maxCount, job.count);
    const double each = ride ? job.rideSeconds : job.seconds + tail;
    Estimate e;
    e.files = job.files == Files::PerVariation ? count : job.files == Files::OneWithMarkers ? 1 : count + 1;
    e.soundSeconds = each * count + (job.files == Files::PerVariation ? 0.0 : job.gapSeconds * (count - 1));
    e.renderSeconds = each * count / speed;
    if (job.files != Files::PerVariation)
    {
        const bool wav = job.options.type == TapeRecorder::FileType::Wav;
        const int bits = ! wav && job.options.bitDepth == 32 ? 24 : job.options.bitDepth;
        e.oneFileBytes = e.soundSeconds * job.rate * (job.options.mono ? 1.0 : 2.0) * bits / 8.0;
    }
    return e;
}

bool VariationRenderer::oneFileTooBig(const Job& job, const Estimate& e)
{
    return job.files != Files::PerVariation && job.options.type != TapeRecorder::FileType::Flac && e.oneFileBytes > c_maxOneFileBytes;
}

juce::String VariationRenderer::estimateText(const Job& job, const Estimate& e)
{
    const int count = juce::jlimit(1, c_maxCount, job.count);
    const bool ride = job.each == Each::WholeRide;
    const juce::String things = juce::String(count) + (ride ? (count == 1 ? " ride" : " rides") : (count == 1 ? " whoosh" : " whooshes"));
    const juce::String thing = ride ? "ride" : "whoosh";
    juce::String what;
    switch (job.files)
    {
        case Files::OneWithMarkers: what = "1 file with " + things + " in it"; break;
        case Files::Both:           what = juce::String(e.files) + " files (" + things + ", and one file with all of them)"; break;
        case Files::PerVariation:
        default:                    what = count == 1 ? "1 file with one " + thing + " in it" : juce::String(count) + " files, one " + thing + " each"; break;
    }
    return what + ": " + roughly(e.soundSeconds) + " of sound, ready in " + roughly(e.renderSeconds) + ".";
}

//==============================================================================
VariationRenderer::VariationRenderer(SwooshbucklerAudioProcessor& rig) : juce::Thread("Render Variations"), m_rig(rig) {}

VariationRenderer::~VariationRenderer()
{
    cancelPendingUpdate();
    stopThread(20000); // a block of rendering, or one file being written, at the most
    releaseCopy();
}

void VariationRenderer::handleAsyncUpdate()
{
    if (m_rendering.load()) // a new batch began before the last one's message arrived
        return;
    waitForThreadToExit(5000); // it has said it is done: it is on its way out
    releaseCopy();
    // How fast it went, for the next batch's estimate.
    const auto p = getProgress();
    if (p.state == State::Finished && p.done >= 3 && p.elapsedSeconds > 0.05 && p.audioSeconds > 0.0)
    {
        const bool ride = m_job.each == Each::WholeRide;
        m_rig.setVariationValue(ride ? "SpeedRide" : "SpeedWhoosh", juce::String(p.audioSeconds / p.elapsedSeconds, 2));
        if (! ride)
            m_rig.setVariationValue("Tail", juce::String(juce::jmax(0.0, p.audioSeconds / p.done - m_job.seconds), 2));
    }
}

void VariationRenderer::releaseCopy()
{
    if (isThreadRunning())
        return;
    m_take.setSize(0, 0); // the render buffer (seconds of audio) is only held while a batch runs
    if (m_copy != nullptr)
    {
        m_copy.reset();
        m_rig.getCargo().endLending();
    }
}

VariationRenderer::Job VariationRenderer::defaultJob() const
{
    Job job;
    job.count = m_rig.getVariationCount();
    job.mode = static_cast<Mode>(m_rig.getVariationMode());
    job.amount = m_rig.getVariationAmount();
    job.each = static_cast<Each>(juce::jlimit(0, 1, m_rig.getVariationValue("Each", "0").getIntValue()));
    auto& state = m_rig.getState();
    job.seconds = juce::jlimit(c_minSeconds, c_maxSeconds,
                               static_cast<double>(Params::throwSeconds(state.getRawParameterValue(ParamIDs::throwLength)->load(),
                                                                        state.getRawParameterValue(ParamIDs::rate)->load())));
    job.rideSeconds = juce::jlimit(c_minRideSeconds, c_maxRideSeconds, m_rig.getVariationValue("RideSeconds", "10").getDoubleValue());
    job.batchSeed = static_cast<std::uint32_t>(juce::Random::getSystemRandom().nextInt(900000)) + 100000u; // six digits
    job.names = static_cast<Names>(juce::jlimit(0, 3, m_rig.getVariationValue("Names", "0").getIntValue()));
    job.name = m_rig.getEffectiveTakeName();
    job.preset = m_rig.currentPresetName;
    job.batchName = m_rig.getVariationValue("BatchName");
    job.when = juce::Time::getCurrentTime();
    job.files = static_cast<Files>(juce::jlimit(0, 2, m_rig.getVariationValue("Files", "0").getIntValue()));
    job.gapSeconds = juce::jlimit(0.0, c_maxGapSeconds, m_rig.getVariationValue("Gap", "0.5").getDoubleValue());
    const auto chosen = m_rig.getVariationFolder();
    job.folder = chosen.isNotEmpty() ? juce::File(chosen) : datedFolder(m_rig.getRecordFolder(), job.name, job.when);
    // The tape's rate: the one chosen in Settings, or the session's.
    job.rate = m_rig.getRecordRate() > 0 ? static_cast<double>(m_rig.getRecordRate())
                                         : (m_rig.getDeviceRate() > 0.0 ? m_rig.getDeviceRate() : 48000.0);
    job.options.bitDepth = m_rig.getBitDepth();
    job.options.type = static_cast<TapeRecorder::FileType>(m_rig.getFileType());
    job.options.mono = m_rig.getMono();
    job.creator = m_rig.getCreator().isNotEmpty() ? m_rig.getCreator() : juce::String("Samuel Justice");
    return job;
}

bool VariationRenderer::start(const Job& job, juce::String& error)
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    if (m_rendering.load())
    {
        error = "A batch is already rendering.";
        return false;
    }
    cancelPendingUpdate();
    stopThread(5000); // the last batch's thread, on its way out
    releaseCopy();

    m_job = job;
    m_job.count = juce::jlimit(1, c_maxCount, job.count);
    m_job.seconds = juce::jlimit(c_minSeconds, c_maxSeconds, job.seconds);
    m_job.amount = juce::jlimit(0.0f, 1.0f, job.amount);
    m_job.rideSeconds = juce::jlimit(c_minRideSeconds, c_maxRideSeconds, job.rideSeconds);
    m_job.gapSeconds = juce::jlimit(0.0, c_maxGapSeconds, job.gapSeconds);
    m_job.rate = job.rate > 0.0 ? job.rate : 48000.0;
    m_job.options.fileRate = 0.0; // the rig renders at the files' rate: nothing to convert
    m_job.options.perWhoosh = false;
    if (oneFileTooBig(m_job, estimate(m_job)))
    {
        error = "One file with all of them would be over 4 GB, too big for markers. Make fewer or shorter variations, "
                "choose FLAC in Settings, or choose One File Per Variation.";
        return false;
    }
    if (m_job.folder == juce::File() || (! m_job.folder.isDirectory() && ! m_job.folder.createDirectory()))
    {
        error = "Couldn't create the folder " + m_job.folder.getFullPathName();
        return false;
    }

    // The second rig: the session as it is now.
    m_copy = m_rig.makeOfflineCopy();
    m_params.clear();
    m_base.clear();
    for (auto* p : m_copy->getParameters())
        if (auto* param = dynamic_cast<juce::RangedAudioParameter*>(p))
        {
            m_params.push_back(param);
            m_base.push_back(param->getValue());
        }
    m_baseStarts.clear();
    for (int t = 0; t < CargoHold::c_numTracks; ++t)
        m_baseStarts.push_back(m_copy->getCargo().getStart(t));

    {
        const juce::ScopedLock sl(m_lock);
        m_progress = Progress {};
        m_progress.state = State::Rendering;
        m_progress.total = m_job.count;
        m_progress.folder = m_job.folder;
        m_progress.batchSeed = m_job.batchSeed;
        m_progress.each = m_job.each;
        m_progress.filing = m_job.files;
        m_files.clear();
    }
    rememberBatch({ m_job.batchSeed, planOf(m_job).stem, juce::Time::getCurrentTime(), m_job.count, m_job.mode, m_job.amount, m_job.each,
                    m_job.seconds, m_job.rideSeconds });
    m_rendering.store(true);
    startThread();
    return true;
}

void VariationRenderer::cancel()
{
    signalThreadShouldExit();
}

VariationRenderer::Progress VariationRenderer::getProgress() const
{
    const juce::ScopedLock sl(m_lock);
    return m_progress;
}

std::vector<juce::File> VariationRenderer::getFiles() const
{
    const juce::ScopedLock sl(m_lock);
    return m_files;
}

std::vector<VariationRenderer::Written> VariationRenderer::takeWritten()
{
    const juce::ScopedLock sl(m_lock);
    std::vector<Written> out;
    out.swap(m_written);
    return out;
}

void VariationRenderer::setProgress(const std::function<void(Progress&)>& change)
{
    const juce::ScopedLock sl(m_lock);
    change(m_progress);
}

//==============================================================================
juce::String VariationRenderer::describeHow() const
{
    juce::String how = modeName(m_job.mode);
    if (m_job.mode == Mode::SmallChanges)
        how << " " << juce::roundToInt(100.0f * m_job.amount) << "%";
    how << ", Batch " << juce::String(m_job.batchSeed) << ", " << eachName(m_job.each) << ", Length "
        << juce::String(m_job.each == Each::WholeRide ? m_job.rideSeconds : m_job.seconds, 1) << " s";
    return how;
}

juce::String VariationRenderer::describeSettings() const
{
    juce::StringArray settings;
    for (auto* param : m_params)
        if (! Presets::isSessionParameter(param->getParameterID()))
            settings.add(param->getName(40) + " " + param->getCurrentValueAsText());
    return settings.joinIntoString(", ") + ".";
}

int VariationRenderer::renderOne(int index, int& first, juce::String& description)
{
    auto& rig = *m_copy;
    juce::Random random(static_cast<juce::int64>(seedOf(m_job.batchSeed, index)));
    const auto engineSeed = static_cast<std::uint32_t>(random.nextInt()) | 1u;

    // From the settings as they were when Render was pressed...
    for (size_t i = 0; i < m_params.size(); ++i)
        if (m_params[i]->getValue() != m_base[i])
            m_params[i]->setValueNotifyingHost(m_base[i]);
    for (int t = 0; t < CargoHold::c_numTracks; ++t)
        rig.getCargo().setStart(t, m_baseStarts[static_cast<size_t>(t)]);

    // ...to this variation's.
    juce::String preset = rig.currentPresetName;
    switch (m_job.mode)
    {
        case Mode::RandomiseEverything:
            for (const auto& [param, target] : Presets::targets(rig.getParameters(), Presets::roll(random, rig.getLockedSections()), Presets::Unlisted::Keep))
                param->setValueNotifyingHost(target);
            preset = "Randomised";
            break;

        case Mode::SmallChanges:
        {
            auto nudge = [&random] { return random.nextFloat() + random.nextFloat() - 1.0f; }; // -1..1, mostly near 0
            for (auto* param : m_params)
            {
                const auto id = param->getParameterID();
                if (Presets::isSessionParameter(id) || isSwitch(param))
                    continue;
                const float by = nudge();
                if (id == ParamIDs::pitch) // whole semitones, four either way at the most
                    param->setValueNotifyingHost(param->convertTo0to1(juce::jlimit(-24.0f, 24.0f,
                        std::round(param->convertFrom0to1(param->getValue()) + 4.0f * m_job.amount * by))));
                else
                    param->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, param->getValue() + 0.5f * m_job.amount * by));
            }
            // Each track starts a little further into its sample.
            for (int t = 0; t < CargoHold::c_numTracks; ++t)
                rig.getCargo().setStart(t, juce::jmin(0.9f, m_baseStarts[static_cast<size_t>(t)] + 0.5f * m_job.amount * random.nextFloat()));
            break;
        }

        case Mode::NewPasses:
        default:
            break;
    }
    const bool ride = m_job.each == Each::WholeRide;
    if (auto* length = rig.getState().getParameter(ParamIDs::throwLength); length != nullptr && ! ride)
        length->setValueNotifyingHost(length->convertTo0to1(static_cast<float>(m_job.seconds)));

    // What the file says about itself: which variation of which batch it is, then the settings it was made with.
    description = "Swooshbuckler " + Version::number() + ". Variation " + juce::String(index + 1) + " of " + juce::String(m_job.count)
                + ", " + describeHow() + ". Preset: " + preset + ". " + describeSettings();

    rig.prepareOffline(m_job.rate, engineSeed);
    juce::MidiBuffer midi;
    const int capacity = m_take.getNumSamples();
    int length = 0;
    auto loud = [this](int i, float over) { return std::abs(m_take.getSample(0, i)) > over || std::abs(m_take.getSample(1, i)) > over; };

    if (ride)
    {
        // The rig as it plays: a pre-roll that is thrown away (so the file does not open on the rig starting up),
        // then the length asked for, with a short fade at each end because both ends are cut out of running sound.
        const int preRoll = juce::roundToInt(c_ridePreRollSeconds * m_job.rate);
        const int wanted = juce::roundToInt(m_job.rideSeconds * m_job.rate);
        rig.setPlaying(true);
        while (length < preRoll + wanted && length + c_block <= capacity)
        {
            if (threadShouldExit())
                return -1;
            juce::AudioBuffer<float> block(m_take.getArrayOfWritePointers(), 2, length, c_block);
            rig.processBlock(block, midi);
            length += c_block;
        }
        first = preRoll;
        const int samples = juce::jmin(wanted, length - preRoll);
        bool sound = false;
        for (int i = first; i < first + samples && ! sound; ++i)
            sound = loud(i, c_silence);
        if (samples <= 0 || ! sound)
            return 0;
        const int fadeIn = juce::jmin(samples / 2, juce::roundToInt(c_rideFadeInSeconds * m_job.rate));
        const int fadeOut = juce::jmin(samples / 2, juce::roundToInt(c_rideFadeOutSeconds * m_job.rate));
        for (int ch = 0; ch < 2; ++ch)
        {
            m_take.applyGainRamp(ch, first, fadeIn, 0.0f, 1.0f);
            m_take.applyGainRamp(ch, first + samples - fadeOut, fadeOut, 1.0f, 0.0f);
        }
        return samples;
    }

    // One thrown whoosh, from silence until the rig has rung out and come to rest.
    rig.throwWhoosh();
    while (length + c_block <= capacity)
    {
        if (threadShouldExit())
            return -1;
        juce::AudioBuffer<float> block(m_take.getArrayOfWritePointers(), 2, length, c_block);
        rig.processBlock(block, midi);
        length += c_block;
        if (rig.isAtRest())
            break;
    }

    // The silence in front comes off, and what is left of the rest at the end.
    for (first = 0; first < length && ! loud(first, c_silence); ++first) {}
    if (first >= length)
        return 0;
    int end = length;
    while (end > first + 1 && ! loud(end - 1, 1.0e-6f))
        --end;
    // A short fade at the very end (a tail that outlasted the time allowed is not chopped).
    const int fade = juce::jmin(end - first, juce::roundToInt(c_endFadeSeconds * m_job.rate));
    for (int ch = 0; ch < 2; ++ch)
        m_take.applyGainRamp(ch, end - fade, fade, 1.0f, 0.0f);
    return end - first;
}

void VariationRenderer::run()
{
    const double started = juce::Time::getMillisecondCounterHiRes();
    const bool ride = m_job.each == Each::WholeRide;
    // The longest a render can be: a whoosh, the longest tail after it, the closing fade and the output stage
    // emptying; or a ride and its pre-roll.
    const double longest = ride ? c_ridePreRollSeconds + m_job.rideSeconds + 0.1
                                : m_job.seconds + SwooshbucklerAudioProcessor::c_tailMaxSeconds + 1.5;
    m_take.setSize(2, c_block * (1 + static_cast<int>(longest * m_job.rate) / c_block));

    TapeRecorder::Naming naming;
    naming.creator = m_job.creator;
    naming.software = "Swooshbuckler " + Version::number();
    const auto extension = TapeRecorder::extensionOf(m_job.options.type);
    const auto plan = planOf(m_job);
    naming.prefix = plan.stem;
    const bool separate = m_job.files != Files::OneWithMarkers, together = m_job.files != Files::PerVariation;
    const int gap = juce::roundToInt(m_job.gapSeconds * m_job.rate);

    State outcome = State::Finished;
    juce::String failure;
    int written = 0, silent = 0;

    // The one file that holds every variation: opened now, written as each is rendered, its markers added at the end.
    TapeRecorder::FileWriter all;
    std::vector<TapeRecorder::Cue> markers;
    if (together)
    {
        naming.description = "Swooshbuckler " + Version::number() + ". " + juce::String(m_job.count) + (m_job.count == 1 ? " variation" : " variations")
                           + " one after another, a marker at the start of each. " + describeHow() + ". Preset: " + m_copy->currentPresetName + "."
                           + (m_job.mode == Mode::NewPasses ? " " + describeSettings()
                              : m_job.mode == Mode::SmallChanges ? " Starting from: " + describeSettings() : juce::String());
        if (all.open(plan.combined, m_job.rate, 2, m_job.options, naming, failure))
            setProgress([&](Progress& p) { p.combinedFile = plan.combined.getFileName(); });
        else
            outcome = State::Failed;
    }

    for (int index = 0; index < m_job.count && outcome == State::Finished; ++index)
    {
        const auto name = fileNameOf(plan.stem, plan.firstNumber + written, plan.lastNumber);
        const auto file = m_job.folder.getChildFile(name + extension);
        setProgress([&](Progress& p) { p.currentFile = separate ? file.getFileName() : name; });

        int first = 0;
        const int samples = renderOne(index, first, naming.description);
        if (samples < 0 || threadShouldExit())
        {
            outcome = State::Cancelled; // nothing of this variation has been written
            break;
        }
        if (samples == 0)
        {
            ++silent;
            // Nothing but silence, three times running, and nothing kept yet: there is nothing to render.
            if (written == 0 && silent >= juce::jmin(3, m_job.count))
            {
                outcome = State::Failed;
                failure = ride ? "There was only silence to render. Load a sample or turn a track up, and try Play first."
                               : "There was only silence to render. Load a sample or turn a track up, and try Throw first.";
                break;
            }
        }
        else
        {
            juce::String error;
            if (separate && (file.exists() || ! TapeRecorder::writeFile(file, m_take, first, samples, m_job.rate, m_job.options, naming, error)))
            {
                if (error.isEmpty())
                    error = file.getFileName() + " is already there, and nothing is ever written over.";
                else
                    file.deleteFile();
                outcome = State::Failed;
                failure = error;
                break;
            }
            if (together)
            {
                if (written > 0)
                    all.writeSilence(gap);
                markers.push_back({ all.position(), name });
                all.write(m_take, first, samples);
            }
            ++written;
            const double seconds = static_cast<double>(samples) / m_job.rate;
            const juce::ScopedLock sl(m_lock);
            if (separate)
            {
                m_files.push_back(file);
                m_written.push_back({ file, seconds, 0 });
            }
            m_progress.audioSeconds += seconds;
        }

        const double elapsed = (juce::Time::getMillisecondCounterHiRes() - started) * 0.001;
        setProgress([&](Progress& p)
        {
            p.done = written;
            p.silent = silent;
            p.files = static_cast<int>(m_files.size());
            p.elapsedSeconds = elapsed;
            p.secondsLeft = elapsed / static_cast<double>(index + 1) * static_cast<double>(m_job.count - index - 1);
        });
    }

    // The one file is closed whole with what it has, however the batch ended; with nothing in it, it goes.
    if (all.isOpen())
    {
        const double seconds = static_cast<double>(all.position()) / m_job.rate;
        if (written == 0)
            all.abandon();
        else if (all.close(markers))
        {
            const bool marked = all.markersAdded();
            {
                const juce::ScopedLock sl(m_lock);
                m_files.push_back(plan.combined);
                m_written.push_back({ plan.combined, seconds, marked ? static_cast<int>(markers.size()) : 0 });
            }
            if (! marked && outcome != State::Failed)
            {
                outcome = State::Failed;
                failure = plan.combined.getFileName() + " holds every variation, but it came to 4 GB or more, too big for markers.";
            }
        }
        else if (outcome == State::Finished)
        {
            outcome = State::Failed;
            failure = "Writing " + plan.combined.getFileName() + " failed.";
        }
    }

    if (outcome == State::Finished && written == 0)
    {
        outcome = State::Failed;
        failure = ride ? "There was only silence to render. Load a sample or turn a track up, and try Play first."
                       : "There was only silence to render. Load a sample or turn a track up, and try Throw first.";
    }
    const double elapsed = (juce::Time::getMillisecondCounterHiRes() - started) * 0.001;
    setProgress([&](Progress& p)
    {
        p.state = outcome;
        p.error = failure;
        p.done = written;
        p.silent = silent;
        p.files = static_cast<int>(m_files.size());
        p.currentFile.clear();
        p.elapsedSeconds = elapsed;
        p.secondsLeft = 0.0;
    });
    m_rendering.store(false);
    triggerAsyncUpdate(); // the second rig is let go of on the message thread
}
