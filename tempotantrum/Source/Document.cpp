#include "Document.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace tt
{

namespace
{
    constexpr double suppressWithin = 0.015;   // a deleted engine pin stays deleted within 15 ms
    constexpr double sameUserPin = 0.001;      // a user pin is the one shown if within 1 ms
    constexpr double replaceUserPinWithin = 0.015;
    constexpr double maxStemLengthDifference = 0.5;
    constexpr double maxLengthSeconds = 60.0 * 60.0;
}

namespace detail
{

//==============================================================================
// What the analysis thread reads: mono copies of the audio, never changed once made.
struct AnalysisAudio
{
    int id = 0;
    double sampleRate = 0.0;
    std::vector<float> mixMono, drumMono;
};

struct AnalysisJob
{
    int generation = 0;
    std::shared_ptr<const AnalysisAudio> audio;
    MapInputs inputs;
};

struct AnalysisResult
{
    int generation = 0;
    juce::int64 onsetKey = -1;
    MapResult map;
    BeatGrid grid;
    std::shared_ptr<const std::vector<float>> click;
};

struct OnsetCache
{
    juce::int64 key = -1;
    std::vector<Onset> onsets;
};

static juce::int64 onsetKeyFor (const AnalysisJob& job)
{
    return (juce::int64) job.audio->id * 2 + (job.inputs.listenToDrums && ! job.audio->drumMono.empty() ? 1 : 0);
}

// User pins in time order. The edits keep them moving forward in both time and beat; if a saved
// map holds two that do not, the engine leaves one out and says so in a check.
static std::vector<Pin> orderedUserPins (std::vector<Pin> pins)
{
    std::stable_sort (pins.begin(), pins.end(), [] (const Pin& a, const Pin& b) { return a.time < b.time; });

    for (auto& p : pins)
        p.user = true;

    return pins;
}

// The whole analysis for one request. Returns nothing if `abandon` says a newer request came in.
static std::optional<AnalysisResult> runJob (const AnalysisJob& job, OnsetCache& cache,
                                             const std::function<bool()>& abandon)
{
    const auto& audio = *job.audio;
    const auto numSamples = audio.mixMono.size();
    const double duration = audio.sampleRate > 0.0 ? (double) numSamples / audio.sampleRate : 0.0;
    const auto key = onsetKeyFor (job);

    if (cache.key != key)
    {
        const bool useDrums = (key & 1) != 0;
        const float* detectOn = useDrums ? audio.drumMono.data() : audio.mixMono.data();
        const float* refineOn = useDrums ? audio.mixMono.data() : nullptr;

        cache.onsets = findOnsets (detectOn, numSamples, audio.sampleRate, refineOn);
        cache.key = key;
    }

    if (abandon())
        return std::nullopt;

    AnalysisResult r;
    r.generation = job.generation;
    r.onsetKey = key;
    r.map = mapCue (cache.onsets, duration, job.inputs.hint, orderedUserPins (job.inputs.userPins), job.inputs.options);

    if (r.map.onsets.empty())
        r.map.onsets = cache.onsets;

    // Engine pins the user deleted stay deleted.
    const auto& suppressed = job.inputs.suppressed;
    r.map.pins.erase (std::remove_if (r.map.pins.begin(), r.map.pins.end(), [&] (const Pin& p)
    {
        return ! p.user && std::any_of (suppressed.begin(), suppressed.end(),
                                        [&] (double t) { return std::abs (t - p.time) <= suppressWithin; });
    }), r.map.pins.end());

    if (abandon())
        return std::nullopt;

    const double fallbackBpm = r.map.bpm > 0.0 ? r.map.bpm : (job.inputs.hint.bpm > 0.0 ? job.inputs.hint.bpm : 120.0);
    r.grid = BeatGrid (r.map.pins, fallbackBpm);

    if (! r.grid.empty())
        r.click = std::make_shared<const std::vector<float>> (
            renderClick (r.grid, job.inputs.hint.beatsPerBar, audio.sampleRate, numSamples));
    else
        r.click = std::make_shared<const std::vector<float>>();

    return r;
}

//==============================================================================
// The analysis thread. It keeps one waiting request (a newer one replaces it) and one finished
// result for the message thread to collect.
class AnalysisWorker : private juce::Thread
{
public:
    // `finishedCallback` is called on this thread when a result is ready to collect.
    explicit AnalysisWorker (std::function<void()> finishedCallback)
        : juce::Thread ("Tempo Tantrum analysis"), onFinished (std::move (finishedCallback)) {}

    ~AnalysisWorker() override
    {
        signalThreadShouldExit();
        notify();
        stopThread (20000);
    }

    void submit (AnalysisJob job)
    {
        {
            const juce::ScopedLock sl (lock);
            pending = std::move (job);
        }

        if (! isThreadRunning())
            startThread();

        notify();
    }

    std::optional<AnalysisResult> takeResult()
    {
        const juce::ScopedLock sl (lock);
        auto r = std::move (finished);
        finished.reset();
        return r;
    }

private:
    std::function<void()> onFinished;
    juce::CriticalSection lock;
    std::optional<AnalysisJob> pending;
    std::optional<AnalysisResult> finished;
    OnsetCache cache;   // used on this thread only

    bool hasPending() const
    {
        const juce::ScopedLock sl (lock);
        return pending.has_value();
    }

    void run() override
    {
        while (! threadShouldExit())
        {
            std::optional<AnalysisJob> job;

            {
                const juce::ScopedLock sl (lock);
                job = std::move (pending);
                pending.reset();
            }

            if (! job.has_value())
            {
                wait (-1);
                continue;
            }

            auto result = runJob (*job, cache, [this] { return threadShouldExit() || hasPending(); });

            if (result.has_value())
            {
                {
                    const juce::ScopedLock sl (lock);
                    finished = std::move (result);
                }

                onFinished();
            }
        }
    }
};

} // namespace detail

//==============================================================================
class Document::SetInputsAction : public juce::UndoableAction
{
public:
    SetInputsAction (Document& d, MapInputs beforeEdit, MapInputs afterEdit)
        : doc (d), before (std::move (beforeEdit)), after (std::move (afterEdit)),
          changesListening (before.listenToDrums != after.listenToDrums) {}

    bool perform() override   { doc.applyInputs (after, changesListening); return true; }
    bool undo() override      { doc.applyInputs (before, changesListening); return true; }
    int getSizeInUnits() override { return 1; }

private:
    Document& doc;
    MapInputs before, after;

    // Only the step that switched between the mix and the stem switches it back. Every other
    // step leaves what is listened to alone, so undoing an edit made before a stem was added
    // does not stop listening to the stem.
    bool changesListening;
};

//==============================================================================
Document::Document()
    : syncCache (std::make_unique<detail::OnsetCache>()),
      worker (std::make_unique<detail::AnalysisWorker> ([this] { triggerAsyncUpdate(); }))
{
    formats.registerBasicFormats();
}

Document::~Document()
{
    worker.reset();
    cancelPendingUpdate();
}

double Document::getLengthSeconds() const
{
    return sampleRate > 0.0 ? (double) mix.getNumSamples() / sampleRate : 0.0;
}

void Document::setStatus (const juce::String& text)
{
    status = text;
    sendSynchronousChangeMessage();
}

//==============================================================================
juce::Result Document::readAudio (const juce::File& file, juce::AudioBuffer<float>& out, double& rate)
{
    if (! file.existsAsFile())
        return juce::Result::fail ("Could not find " + file.getFileName());

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

    if (reader == nullptr)
        return juce::Result::fail (file.getFileName() + " is not an audio file this app can read");

    if (reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0)
        return juce::Result::fail (file.getFileName() + " has no audio in it");

    if ((double) reader->lengthInSamples / reader->sampleRate > maxLengthSeconds)
        return juce::Result::fail (file.getFileName() + " is longer than an hour");

    const int numSamples = (int) reader->lengthInSamples;
    out.setSize (juce::jlimit (1, 2, (int) reader->numChannels), numSamples);
    out.clear();

    if (! reader->read (&out, 0, numSamples, 0, true, true))
        return juce::Result::fail ("Could not read all of " + file.getFileName());

    rate = reader->sampleRate;
    return juce::Result::ok();
}

static std::vector<float> monoOf (const juce::AudioBuffer<float>& b)
{
    std::vector<float> mono ((size_t) b.getNumSamples(), 0.0f);
    const int channels = b.getNumChannels();

    if (channels == 0)
        return mono;

    const float scale = 1.0f / (float) channels;

    for (int ch = 0; ch < channels; ++ch)
    {
        const float* src = b.getReadPointer (ch);
        for (size_t i = 0; i < mono.size(); ++i)
            mono[i] += src[i] * scale;
    }

    return mono;
}

void Document::rebuildAnalysisAudio()
{
    auto a = std::make_shared<detail::AnalysisAudio>();
    a->id = ++audioVersion;
    a->sampleRate = sampleRate;
    a->mixMono = monoOf (mix);

    if (hasDrums())
        a->drumMono = monoOf (drums);

    analysisAudio = std::move (a);
}

void Document::clearResults()
{
    map = {};
    grid = {};
    ++mapVersion;
    click = std::make_shared<const std::vector<float>>();
    deliveredOnsetKey = -1;
    listening = false;
    mapPending = false;
    ++generation;
}

juce::Result Document::loadMusic (const juce::File& file)
{
    juce::AudioBuffer<float> newMix;
    double newRate = 0.0;
    const auto read = readAudio (file, newMix, newRate);

    if (read.failed())
        return read;

    if (onAudioAboutToChange)
        onAudioAboutToChange (true);

    mix = std::move (newMix);
    drums.setSize (0, 0);
    sampleRate = newRate;
    musicFile = file;
    drumFile = juce::File();

    inputs = MapInputs();
    undoManager.clearUndoHistory();
    changedSinceSave = false;
    clearResults();

    status = "Opened " + file.getFileName();
    statusToKeep.clear();

    // A map saved beside the music is opened with it, if it was made from the same audio.
    const auto saved = getMapFile();

    if (saved.existsAsFile())
    {
        mapfile::Contents contents;
        const auto loaded = mapfile::load (saved, contents);

        if (loaded.failed())
        {
            status = loaded.getErrorMessage();
        }
        else if (contents.lengthInSamples != mix.getNumSamples()
                 || contents.fingerprint != mapfile::fingerprint (mix))
        {
            status = "The map beside this file was made from different audio, so it was not opened";
        }
        else
        {
            inputs = contents.inputs;
            status = "Opened the saved map beside the music";

            if (contents.drumStem.isNotEmpty())
            {
                const auto stem = file.getParentDirectory().getChildFile (contents.drumStem);
                juce::AudioBuffer<float> stemAudio;
                double stemRate = 0.0;

                if (stem.existsAsFile() && readAudio (stem, stemAudio, stemRate).wasOk()
                    && juce::exactlyEqual (stemRate, sampleRate)
                    && std::abs (stemAudio.getNumSamples() - mix.getNumSamples()) <= maxStemLengthDifference * sampleRate)
                {
                    drums.setSize (stemAudio.getNumChannels(), mix.getNumSamples());
                    drums.clear();
                    for (int ch = 0; ch < drums.getNumChannels(); ++ch)
                        drums.copyFrom (ch, 0, stemAudio, ch, 0, std::min (stemAudio.getNumSamples(), mix.getNumSamples()));
                    drumFile = stem;
                    status << " and its drum stem";
                }
                else
                {
                    inputs.listenToDrums = false;
                    status << ". Its drum stem could not be opened, so it listens to the full mix";
                }
            }
            else
            {
                inputs.listenToDrums = false;
            }
        }

        statusToKeep = status;
    }

    rebuildAnalysisAudio();
    requestMap();
    sendSynchronousChangeMessage();
    return juce::Result::ok();
}

juce::Result Document::loadDrumStem (const juce::File& file)
{
    if (! hasMusic())
        return juce::Result::fail ("Open the music before adding a drum stem");

    juce::AudioBuffer<float> stem;
    double rate = 0.0;
    const auto read = readAudio (file, stem, rate);

    if (read.failed())
        return read;

    if (! juce::exactlyEqual (rate, sampleRate))
        return juce::Result::fail ("The drum stem is at " + juce::String (rate / 1000.0, 1) + " kHz and the music at "
                                   + juce::String (sampleRate / 1000.0, 1) + " kHz. Use a stem at the same sample rate");

    const double difference = std::abs ((double) (stem.getNumSamples() - mix.getNumSamples())) / sampleRate;

    if (difference > maxStemLengthDifference)
        return juce::Result::fail ("The drum stem is " + juce::String (difference, 2)
                                   + " s longer or shorter than the music. It must be the same length, within half a second");

    if (onAudioAboutToChange)
        onAudioAboutToChange (false);

    drums.setSize (stem.getNumChannels(), mix.getNumSamples());
    drums.clear();
    for (int ch = 0; ch < drums.getNumChannels(); ++ch)
        drums.copyFrom (ch, 0, stem, ch, 0, std::min (stem.getNumSamples(), mix.getNumSamples()));

    drumFile = file;

    // Listening to the stem is what adding one is for. Not an undo step: the audio is not undoable.
    inputs.listenToDrums = true;
    changedSinceSave = true;
    status = "Added the drum stem " + file.getFileName();

    rebuildAnalysisAudio();
    requestMap();
    sendSynchronousChangeMessage();
    return juce::Result::ok();
}

//==============================================================================
void Document::edit (MapInputs newInputs, const juce::String& name, bool joinPrevious)
{
    if (! hasMusic() || newInputs == inputs)
        return;

    if (! joinPrevious)
        undoManager.beginNewTransaction (name);

    undoManager.perform (new SetInputsAction (*this, inputs, std::move (newInputs)), name);
}

void Document::applyInputs (const MapInputs& newInputs, bool listeningToo)
{
    const bool wasListeningToDrums = inputs.listenToDrums;
    inputs = newInputs;

    if (! listeningToo)
        inputs.listenToDrums = wasListeningToDrums;

    if (inputs.listenToDrums && ! hasDrums())
        inputs.listenToDrums = false;

    changedSinceSave = true;
    ++editSerial;

    requestMap();
    sendSynchronousChangeMessage();
}

bool Document::undo()
{
    if (! undoManager.canUndo())
        return false;

    const auto name = undoManager.getUndoDescription();
    const bool done = undoManager.undo();

    if (done && name.isNotEmpty())
        setStatus ("Undid " + name.toLowerCase());

    return done;
}

bool Document::redo()
{
    if (! undoManager.canRedo())
        return false;

    const auto name = undoManager.getRedoDescription();
    const bool done = undoManager.redo();

    if (done && name.isNotEmpty())
        setStatus ("Redid " + name.toLowerCase());

    return done;
}

double Document::nearestBeatAt (double time) const
{
    return std::round (grid.beatAtTime (time));
}

int Document::findUserPin (const std::vector<Pin>& pins, double time, double tolerance)
{
    int best = -1;
    double bestDistance = tolerance;

    for (int i = 0; i < (int) pins.size(); ++i)
    {
        const double d = std::abs (pins[(size_t) i].time - time);
        if (d <= bestDistance)
        {
            best = i;
            bestDistance = d;
        }
    }

    return best;
}

// The position as the window shows it: "46|1", or "46|1|480" off the beat.
static juce::String positionText (double beat)
{
    const juce::String full (formatBarBeat (beat, Document::beatsPerBar));
    return full.endsWith ("|000") ? full.dropLastCharacters (4) : full;
}

// Adds a user pin, replacing any user pin within `replaceWithin` of it or with the same beat.
// User pins move forward in both time and beat. If the new pin would not, against another user
// pin, nothing is added and `whyNot` says which pin is in the way.
bool Document::placeUserPin (std::vector<Pin>& pins, Pin pin, double replaceWithin, juce::String& whyNot)
{
    pin.user = true;

    pins.erase (std::remove_if (pins.begin(), pins.end(), [&] (const Pin& p)
    {
        return std::abs (p.time - pin.time) <= replaceWithin || std::abs (p.beat - pin.beat) < 1.0e-6;
    }), pins.end());

    for (const auto& p : pins)
    {
        if (p.time < pin.time && p.beat >= pin.beat)
        {
            whyNot = positionText (pin.beat) + " here would not be after the pin before it at " + positionText (p.beat);
            return false;
        }

        if (p.time > pin.time && p.beat <= pin.beat)
        {
            whyNot = positionText (pin.beat) + " here would not be before the pin after it at " + positionText (p.beat);
            return false;
        }
    }

    const auto at = std::lower_bound (pins.begin(), pins.end(), pin,
                                      [] (const Pin& a, const Pin& b) { return a.time < b.time; });
    pins.insert (at, pin);
    return true;
}

void Document::movePin (int pinIndex, double newTime, bool joinPrevious)
{
    if (pinIndex < 0 || pinIndex >= (int) map.pins.size())
        return;

    const auto old = map.pins[(size_t) pinIndex];
    auto next = inputs;

    if (old.user)
    {
        const int i = findUserPin (next.userPins, old.time, sameUserPin);
        if (i >= 0)
            next.userPins.erase (next.userPins.begin() + i);
    }

    juce::String whyNot;

    if (! placeUserPin (next.userPins, { old.beat, newTime, true, old.restart }, replaceUserPinWithin, whyNot))
        return setStatus ("The pin was not moved: " + whyNot);

    edit (std::move (next), "Move pin", joinPrevious);
}

void Document::addPin (double time)
{
    if (! hasMap())
        return;

    auto next = inputs;
    juce::String whyNot;

    if (! placeUserPin (next.userPins, { nearestBeatAt (time), time, true, false }, replaceUserPinWithin, whyNot))
        return setStatus ("No pin was added: " + whyNot);

    // Placing a pin by hand where one was deleted brings that hit back.
    next.suppressed.erase (std::remove_if (next.suppressed.begin(), next.suppressed.end(),
                                           [&] (double t) { return std::abs (t - time) <= suppressWithin; }),
                           next.suppressed.end());
    edit (std::move (next), "Add pin");
}

void Document::deletePin (int pinIndex)
{
    if (pinIndex < 0 || pinIndex >= (int) map.pins.size())
        return;

    const auto old = map.pins[(size_t) pinIndex];
    auto next = inputs;

    if (old.user)
    {
        const int i = findUserPin (next.userPins, old.time, sameUserPin);
        if (i >= 0)
            next.userPins.erase (next.userPins.begin() + i);
    }
    else if (std::none_of (next.suppressed.begin(), next.suppressed.end(),
                           [&] (double t) { return std::abs (t - old.time) < sameUserPin; }))
    {
        next.suppressed.push_back (old.time);
        std::sort (next.suppressed.begin(), next.suppressed.end());
    }

    edit (std::move (next), "Delete pin");
}

void Document::restartHere (double time)
{
    if (! hasMap())
        return;

    auto next = inputs;
    const double beat = std::ceil (grid.beatAtTime (time) - 1.0e-6);
    juce::String whyNot;

    if (! placeUserPin (next.userPins, { beat, time, true, true }, replaceUserPinWithin, whyNot))
        return setStatus ("No restart was added: " + whyNot);

    edit (std::move (next), "Restart the beat");
}

void Document::firstHitHere (double time)
{
    auto next = inputs;
    next.hint.firstHitTime = std::max (0.0, time);
    edit (std::move (next), "Set the first hit");
}

void Document::setPinBeat (int pinIndex, double newBeat)
{
    if (pinIndex < 0 || pinIndex >= (int) map.pins.size())
        return;

    const auto old = map.pins[(size_t) pinIndex];
    auto next = inputs;

    if (old.user)
    {
        const int i = findUserPin (next.userPins, old.time, sameUserPin);
        if (i >= 0)
            next.userPins.erase (next.userPins.begin() + i);
    }

    juce::String whyNot;

    // Bars only go up: the new bar and beat must be after those of the pin before this one.
    if (pinIndex > 0 && map.pins[(size_t) pinIndex - 1].beat >= newBeat)
        return setStatus ("The bar was not changed: " + positionText (newBeat) + " here would not be after the pin before it at "
                          + positionText (map.pins[(size_t) pinIndex - 1].beat));

    if (! placeUserPin (next.userPins, { newBeat, old.time, true, old.restart }, sameUserPin, whyNot))
        return setStatus ("The bar was not changed: " + whyNot);

    edit (std::move (next), "Change the bar");
}

// The user's pins were given their beats under the first hit's old bar and beat, so they move
// with it: the whole map is renumbered, in one undo step.
void Document::setFirstHit (int bar, int beat, const juce::String& name)
{
    auto next = inputs;
    next.hint.bar = bar;
    next.hint.beat = juce::jlimit (1, next.hint.beatsPerBar, beat);

    const double shift = (double) ((next.hint.bar - inputs.hint.bar) * inputs.hint.beatsPerBar
                                   + (next.hint.beat - inputs.hint.beat));

    for (auto& p : next.userPins)
        p.beat += shift;

    edit (std::move (next), name);
}

void Document::setFirstHitBar (int bar)
{
    setFirstHit (bar, inputs.hint.beat, "Change the first bar");
}

void Document::setFirstHitBeat (int beat)
{
    setFirstHit (inputs.hint.bar, beat, "Change the first beat");
}

void Document::setTempo (double bpm, bool joinPrevious)
{
    auto next = inputs;
    next.hint.bpm = std::max (0.0, bpm);
    edit (std::move (next), "Change the tempo", joinPrevious);
}

void Document::setPinEveryBars (int bars)
{
    auto next = inputs;
    next.options.pinEveryBars = juce::jlimit (0, 64, bars);
    edit (std::move (next), "Change how often pins are placed");
}

void Document::setListenToDrums (bool useDrums)
{
    auto next = inputs;
    next.listenToDrums = useDrums && hasDrums();
    edit (std::move (next), useDrums ? "Listen to the drum stem" : "Listen to the full mix");
}

//==============================================================================
void Document::requestMap()
{
    if (analysisAudio == nullptr || analysisAudio->mixMono.empty())
        return;

    detail::AnalysisJob job;
    job.generation = ++generation;
    job.audio = analysisAudio;
    job.inputs = inputs;

    const bool needsHits = detail::onsetKeyFor (job) != deliveredOnsetKey;

    if (synchronous)
    {
        if (auto r = detail::runJob (job, *syncCache, [] { return false; }))
            applyResult (std::move (*r));
        return;
    }

    listening = needsHits;
    mapPending = true;
    worker->submit (std::move (job));
}

void Document::handleAsyncUpdate()
{
    if (auto r = worker->takeResult())
    {
        if (r->generation == generation)
        {
            applyResult (std::move (*r));
            sendSynchronousChangeMessage();
        }
    }
}

void Document::applyResult (detail::AnalysisResult&& r)
{
    const bool firstForTheseHits = r.onsetKey != deliveredOnsetKey;

    map = std::move (r.map);
    grid = std::move (r.grid);
    ++mapVersion;
    click = std::move (r.click);
    deliveredOnsetKey = r.onsetKey;
    listening = false;
    mapPending = false;

    if (firstForTheseHits)
    {
        juce::String found;

        if (map.onsets.empty())
            found = "No hits found in " + (inputs.listenToDrums ? drumFile : musicFile).getFileName();
        else if (map.pins.empty())
            found = "Found " + juce::String ((int) map.onsets.size()) + " hits but no steady beat. Set the first hit and the tempo";
        else
            found = "Mapped " + juce::String ((int) map.sections.size()) + (map.sections.size() == 1 ? " section, " : " sections, ")
                  + juce::String ((int) map.pins.size()) + " pins";

        // What happened when the music was opened (a saved map, or why it was not used) stays in front.
        status = statusToKeep.isNotEmpty() ? statusToKeep + ". " + found : found;
        statusToKeep.clear();
    }
}

//==============================================================================
juce::File Document::getMapFile() const
{
    return musicFile == juce::File() ? juce::File() : mapfile::fileFor (musicFile);
}

juce::Result Document::saveMap()
{
    if (! hasMusic())
        return juce::Result::fail ("Open some music first");

    // The saved pins are the map made from the saved inputs, so both wait for the map.
    if (mapPending)
    {
        const auto wait = juce::Result::fail ("The map is still being worked out. Save again when it is shown");
        setStatus (wait.getErrorMessage());
        return wait;
    }

    mapfile::Contents c;
    c.appVersion = ProjectInfo::versionString;
    c.musicFileName = musicFile.getFileName();
    c.lengthInSamples = mix.getNumSamples();
    c.sampleRate = sampleRate;
    c.fingerprint = mapfile::fingerprint (mix);
    c.drumStem = drumFile == juce::File() ? juce::String() : drumFile.getRelativePathFrom (musicFile.getParentDirectory());
    c.inputs = inputs;
    c.pins = map.pins;

    const auto file = getMapFile();
    const auto result = mapfile::save (file, c);

    if (result.wasOk())
        changedSinceSave = false;

    setStatus (result.wasOk() ? "Saved the map as " + file.getFileName() : result.getErrorMessage());
    return result;
}

juce::File Document::getDefaultClickFile() const
{
    if (musicFile == juce::File())
        return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("CLICK.wav");

    return musicFile.getSiblingFile (musicFile.getFileNameWithoutExtension() + " CLICK.wav");
}

juce::Result Document::exportClick (const juce::File& file)
{
    if (! hasMap() || click == nullptr || click->empty())
        return juce::Result::fail ("There is no map to make a click from yet");

    const auto result = mapfile::writeClick (file, *click, sampleRate, mix.getNumSamples());
    setStatus (result.wasOk() ? "Exported the click as " + file.getFileName() : result.getErrorMessage());
    return result;
}

} // namespace tt
