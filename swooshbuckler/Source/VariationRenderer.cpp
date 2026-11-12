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
        case Mode::SmallChanges: return "Small Changes";
        case Mode::SeedOnly:     return "Seed Only";
        case Mode::RandomiseEverything:
        default:                 return "Randomise Everything";
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
    auto& state = m_rig.getState();
    job.seconds = juce::jlimit(c_minSeconds, c_maxSeconds,
                               static_cast<double>(Params::throwSeconds(state.getRawParameterValue(ParamIDs::throwLength)->load(),
                                                                        state.getRawParameterValue(ParamIDs::rate)->load())));
    job.batchSeed = static_cast<std::uint32_t>(juce::Random::getSystemRandom().nextInt(900000)) + 100000u; // six digits: easy to note down
    job.name = m_rig.getEffectiveTakeName();
    const auto chosen = m_rig.getVariationFolder();
    job.folder = chosen.isNotEmpty() ? juce::File(chosen) : datedFolder(m_rig.getRecordFolder(), job.name, juce::Time::getCurrentTime());
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
    m_job.rate = job.rate > 0.0 ? job.rate : 48000.0;
    m_job.name = safeName(job.name);
    m_job.options.fileRate = 0.0; // the rig renders at the files' rate: nothing to convert
    m_job.options.perWhoosh = false;
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
        m_files.clear();
    }
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

    // ...to this file's.
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

        case Mode::SeedOnly:
        default:
            break;
    }
    if (auto* length = rig.getState().getParameter(ParamIDs::throwLength))
        length->setValueNotifyingHost(length->convertTo0to1(static_cast<float>(m_job.seconds)));

    // What the file says about itself: which file of which batch it is, then the settings it was made with.
    {
        juce::String how = modeName(m_job.mode);
        if (m_job.mode == Mode::SmallChanges)
            how << " " << juce::roundToInt(100.0f * m_job.amount) << "%";
        description = "Swooshbuckler " + Version::number() + ". Variation " + juce::String(index + 1) + " of " + juce::String(m_job.count)
                    + ", " + how + ", Batch Seed " + juce::String(m_job.batchSeed) + ", Length " + juce::String(m_job.seconds, 1) + " s. Preset: " + preset + ".";
        juce::StringArray settings;
        for (auto* param : m_params)
            if (! Presets::isSessionParameter(param->getParameterID()))
                settings.add(param->getName(40) + " " + param->getCurrentValueAsText());
        description << " " << settings.joinIntoString(", ") << ".";
    }

    // One thrown whoosh, from silence until the rig has rung out and come to rest.
    rig.prepareOffline(m_job.rate, engineSeed);
    rig.throwWhoosh();
    juce::MidiBuffer midi;
    const int capacity = m_take.getNumSamples();
    int length = 0;
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
    auto loud = [this](int i, float over) { return std::abs(m_take.getSample(0, i)) > over || std::abs(m_take.getSample(1, i)) > over; };
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
    // The longest a render can be: the throw, the longest tail after it, the closing fade and the output stage emptying.
    m_take.setSize(2, c_block * (1 + static_cast<int>((m_job.seconds + SwooshbucklerAudioProcessor::c_tailMaxSeconds + 1.5) * m_job.rate) / c_block));

    TapeRecorder::Naming naming;
    naming.prefix = m_job.name;
    naming.creator = m_job.creator;
    naming.software = "Swooshbuckler " + Version::number();
    const auto extension = TapeRecorder::extensionOf(m_job.options.type);
    // Numbers carry on after the files the folder already has, so nothing is ever written over.
    const int firstNumber = highestNumberIn(m_job.folder, m_job.name) + 1;
    const int lastNumber = firstNumber + m_job.count - 1;

    State outcome = State::Finished;
    juce::String failure;
    int written = 0, silent = 0;
    for (int index = 0; index < m_job.count; ++index)
    {
        const auto file = m_job.folder.getChildFile(fileNameOf(m_job.name, firstNumber + written, lastNumber) + extension);
        setProgress([&](Progress& p) { p.currentFile = file.getFileName(); });

        int first = 0;
        const int samples = renderOne(index, first, naming.description);
        if (samples < 0 || threadShouldExit())
        {
            outcome = State::Cancelled; // nothing of this file has been written
            break;
        }
        if (samples == 0)
        {
            ++silent;
            // Nothing but silence, three times running, and no file yet: there is nothing to render.
            if (written == 0 && silent >= juce::jmin(3, m_job.count))
            {
                outcome = State::Failed;
                failure = "There was only silence to render. Load a sample or turn a track up, and try Throw first.";
                break;
            }
        }
        else
        {
            juce::String error;
            if (! TapeRecorder::writeFile(file, m_take, first, samples, m_job.rate, m_job.options, naming, error))
            {
                file.deleteFile();
                outcome = State::Failed;
                failure = error;
                break;
            }
            ++written;
            const double seconds = static_cast<double>(samples) / m_job.rate;
            const juce::ScopedLock sl(m_lock);
            m_files.push_back(file);
            m_written.push_back({ file, seconds });
            m_progress.audioSeconds += seconds;
        }

        const double elapsed = (juce::Time::getMillisecondCounterHiRes() - started) * 0.001;
        setProgress([&](Progress& p)
        {
            p.done = written;
            p.silent = silent;
            p.elapsedSeconds = elapsed;
            p.secondsLeft = elapsed / static_cast<double>(index + 1) * static_cast<double>(m_job.count - index - 1);
        });
    }

    if (outcome == State::Finished && written == 0)
    {
        outcome = State::Failed;
        failure = "There was only silence to render. Load a sample or turn a track up, and try Throw first.";
    }
    const double elapsed = (juce::Time::getMillisecondCounterHiRes() - started) * 0.001;
    setProgress([&](Progress& p)
    {
        p.state = outcome;
        p.error = failure;
        p.done = written;
        p.silent = silent;
        p.currentFile.clear();
        p.elapsedSeconds = elapsed;
        p.secondsLeft = 0.0;
    });
    m_rendering.store(false);
    triggerAsyncUpdate(); // the second rig is let go of on the message thread
}
