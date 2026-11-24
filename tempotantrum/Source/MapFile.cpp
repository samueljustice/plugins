#include "MapFile.h"

#include <cmath>

namespace tt
{

static bool samePins (const std::vector<Pin>& a, const std::vector<Pin>& b)
{
    if (a.size() != b.size())
        return false;

    for (size_t i = 0; i < a.size(); ++i)
        if (! juce::exactlyEqual (a[i].beat, b[i].beat) || ! juce::exactlyEqual (a[i].time, b[i].time) || a[i].user != b[i].user || a[i].restart != b[i].restart)
            return false;

    return true;
}

bool MapInputs::operator== (const MapInputs& o) const
{
    return juce::exactlyEqual (hint.firstHitTime, o.hint.firstHitTime) && hint.bar == o.hint.bar && hint.beat == o.hint.beat
        && juce::exactlyEqual (hint.bpm, o.hint.bpm) && hint.beatsPerBar == o.hint.beatsPerBar
        && options.bridge == o.options.bridge && options.detectJumps == o.options.detectJumps
        && juce::exactlyEqual (options.pinToleranceMs, o.options.pinToleranceMs)
        && options.pinEveryBars == o.options.pinEveryBars
        && samePins (userPins, o.userPins) && suppressed == o.suppressed && listenToDrums == o.listenToDrums;
}

namespace mapfile
{

juce::File fileFor (const juce::File& musicFile)
{
    return musicFile.getSiblingFile (musicFile.getFileName() + ".tempotantrum.json");
}

juce::uint64 fingerprint (const juce::AudioBuffer<float>& audio)
{
    // FNV-1a over the length and a coarse envelope. The envelope is rounded to steps of 1/4096 so
    // the same audio read by a different decoder still matches.
    juce::uint64 hash = 1469598103934665603ull;
    const auto mix = [&hash] (juce::uint64 value)
    {
        for (int i = 0; i < 8; ++i)
        {
            hash ^= (value >> (i * 8)) & 0xffu;
            hash *= 1099511628211ull;
        }
    };

    const int numSamples = audio.getNumSamples();
    const int numChannels = audio.getNumChannels();
    mix ((juce::uint64) numSamples);

    constexpr int block = 1024;

    for (int start = 0; start < numSamples; start += block)
    {
        const int n = std::min (block, numSamples - start);
        double sum = 0.0;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float* data = audio.getReadPointer (ch, start);

            for (int i = 0; i < n; ++i)
                sum += std::abs (data[i]);
        }

        const double mean = numChannels > 0 ? sum / (double) (n * numChannels) : 0.0;
        mix ((juce::uint64) std::llround (mean * 4096.0));
    }

    return hash;
}

//==============================================================================
static juce::var pinToVar (const Pin& p, double sampleRate)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("beat", p.beat);
    o->setProperty ("time", p.time);
    if (sampleRate > 0.0)
        o->setProperty ("sample", (juce::int64) std::llround (p.time * sampleRate));
    o->setProperty ("user", p.user);
    o->setProperty ("restart", p.restart);
    return juce::var (o);
}

static Pin pinFromVar (const juce::var& v)
{
    Pin p;
    p.beat = (double) v.getProperty ("beat", 0.0);
    p.time = (double) v.getProperty ("time", 0.0);
    p.user = (bool) v.getProperty ("user", false);
    p.restart = (bool) v.getProperty ("restart", false);
    return p;
}

static juce::var pinsToVar (const std::vector<Pin>& pins, double sampleRate)
{
    juce::Array<juce::var> list;
    for (const auto& p : pins)
        list.add (pinToVar (p, sampleRate));
    return juce::var (list);
}

static std::vector<Pin> pinsFromVar (const juce::var& v)
{
    std::vector<Pin> pins;
    if (const auto* list = v.getArray())
        for (const auto& item : *list)
            if (item.isObject())
                pins.push_back (pinFromVar (item));
    return pins;
}

juce::Result save (const juce::File& file, const Contents& c)
{
    auto* root = new juce::DynamicObject();
    root->setProperty ("format", "Tempo Tantrum map");
    root->setProperty ("formatVersion", currentFormatVersion);
    root->setProperty ("appVersion", c.appVersion);
    root->setProperty ("musicFile", c.musicFileName);
    root->setProperty ("lengthInSamples", c.lengthInSamples);
    root->setProperty ("sampleRate", c.sampleRate);
    root->setProperty ("fingerprint", juce::String::toHexString (c.fingerprint));
    root->setProperty ("drumStem", c.drumStem);
    root->setProperty ("listenTo", c.inputs.listenToDrums ? "drums" : "mix");

    auto* hint = new juce::DynamicObject();
    hint->setProperty ("firstHitTime", c.inputs.hint.firstHitTime);
    hint->setProperty ("bar", c.inputs.hint.bar);
    hint->setProperty ("beat", c.inputs.hint.beat);
    hint->setProperty ("bpm", c.inputs.hint.bpm);
    hint->setProperty ("beatsPerBar", c.inputs.hint.beatsPerBar);
    root->setProperty ("hint", juce::var (hint));

    auto* options = new juce::DynamicObject();
    options->setProperty ("bridge", c.inputs.options.bridge == Options::Bridge::halfBeat ? "halfBeat" : "wholeBeat");
    options->setProperty ("detectJumps", c.inputs.options.detectJumps);
    options->setProperty ("pinToleranceMs", c.inputs.options.pinToleranceMs);
    options->setProperty ("pinEveryBars", c.inputs.options.pinEveryBars);
    root->setProperty ("options", juce::var (options));

    root->setProperty ("userPins", pinsToVar (c.inputs.userPins, c.sampleRate));

    juce::Array<juce::var> suppressed;
    for (double t : c.inputs.suppressed)
        suppressed.add (t);
    root->setProperty ("suppressedPins", juce::var (suppressed));

    root->setProperty ("pins", pinsToVar (c.pins, c.sampleRate));

    if (c.proTools.known)
    {
        auto* origin = new juce::DynamicObject();
        origin->setProperty ("startKnown", c.proTools.startKnown);
        origin->setProperty ("selectionStart", c.proTools.startSample);
        origin->setProperty ("selectionEnd", c.proTools.endSample);
        origin->setProperty ("sessionRate", c.proTools.sessionRate);
        origin->setProperty ("session", c.proTools.sessionName);
        origin->setProperty ("name", c.proTools.name);
        origin->setProperty ("source", c.proTools.source);
        root->setProperty ("proTools", juce::var (origin));
    }

    // 15 decimal places, so every time and beat reads back as the same number.
    const auto text = juce::JSON::toString (juce::var (root), false, 15);

    if (! file.replaceWithText (text))
        return juce::Result::fail ("Could not write " + file.getFileName());

    return juce::Result::ok();
}

juce::Result load (const juce::File& file, Contents& c)
{
    if (! file.existsAsFile())
        return juce::Result::fail ("There is no saved map at " + file.getFileName());

    juce::var root;
    const auto parsed = juce::JSON::parse (file.loadFileAsString(), root);

    if (parsed.failed() || ! root.isObject())
        return juce::Result::fail (file.getFileName() + " is not a map file it can read");

    if (root.getProperty ("format", {}).toString() != "Tempo Tantrum map")
        return juce::Result::fail (file.getFileName() + " is not a Tempo Tantrum map");

    c.formatVersion = (int) root.getProperty ("formatVersion", 0);

    if (c.formatVersion < 1 || c.formatVersion > currentFormatVersion)
        return juce::Result::fail (file.getFileName() + " was saved by a later version of Tempo Tantrum");

    c.appVersion = root.getProperty ("appVersion", {}).toString();
    c.musicFileName = root.getProperty ("musicFile", {}).toString();
    c.lengthInSamples = (juce::int64) root.getProperty ("lengthInSamples", 0);
    c.sampleRate = (double) root.getProperty ("sampleRate", 0.0);
    c.fingerprint = (juce::uint64) root.getProperty ("fingerprint", {}).toString().getHexValue64();
    c.drumStem = root.getProperty ("drumStem", {}).toString();

    MapInputs in;
    in.listenToDrums = root.getProperty ("listenTo", {}).toString() == "drums";

    const auto hint = root.getProperty ("hint", {});
    in.hint.firstHitTime = (double) hint.getProperty ("firstHitTime", -1.0);
    in.hint.bar = (int) hint.getProperty ("bar", 1);
    in.hint.beat = (int) hint.getProperty ("beat", 1);
    in.hint.bpm = (double) hint.getProperty ("bpm", 0.0);
    in.hint.beatsPerBar = juce::jlimit (1, 16, (int) hint.getProperty ("beatsPerBar", 4));

    const auto options = root.getProperty ("options", {});
    in.options.bridge = options.getProperty ("bridge", {}).toString() == "halfBeat" ? Options::Bridge::halfBeat
                                                                                    : Options::Bridge::wholeBeat;
    in.options.detectJumps = (bool) options.getProperty ("detectJumps", true);
    in.options.pinToleranceMs = (double) options.getProperty ("pinToleranceMs", 10.0);
    in.options.pinEveryBars = juce::jlimit (0, 64, (int) options.getProperty ("pinEveryBars", Options().pinEveryBars));

    in.userPins = pinsFromVar (root.getProperty ("userPins", {}));
    for (auto& p : in.userPins)
        p.user = true;

    if (const auto* list = root.getProperty ("suppressedPins", {}).getArray())
        for (const auto& t : *list)
            in.suppressed.push_back ((double) t);

    c.inputs = std::move (in);
    c.pins = pinsFromVar (root.getProperty ("pins", {}));

    if (const auto origin = root.getProperty ("proTools", {}); origin.isObject())
    {
        c.proTools.known = true;
        c.proTools.startKnown = (bool) origin.getProperty ("startKnown", true);
        c.proTools.startSample = (juce::int64) origin.getProperty ("selectionStart", 0);
        c.proTools.endSample = (juce::int64) origin.getProperty ("selectionEnd", 0);
        c.proTools.sessionRate = (double) origin.getProperty ("sessionRate", 0.0);
        c.proTools.sessionName = origin.getProperty ("session", {}).toString();
        c.proTools.name = origin.getProperty ("name", {}).toString();
        c.proTools.source = origin.getProperty ("source", {}).toString();
    }

    return juce::Result::ok();
}

//==============================================================================
juce::Result writeClick (const juce::File& file, const std::vector<float>& click,
                         double sampleRate, juce::int64 numSamples)
{
    if (sampleRate <= 0.0 || numSamples <= 0)
        return juce::Result::fail ("There is no click to export");

    juce::TemporaryFile temp (file);
    auto stream = temp.getFile().createOutputStream();

    if (stream == nullptr)
        return juce::Result::fail ("Could not write to " + file.getParentDirectory().getFullPathName());

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), sampleRate, 1, 24, {}, 0));

    if (writer == nullptr)
        return juce::Result::fail ("Could not make a WAV file at this sample rate");

    stream.release(); // the writer owns the stream now

    constexpr int chunk = 65536;
    std::vector<float> block ((size_t) chunk);

    for (juce::int64 pos = 0; pos < numSamples; pos += chunk)
    {
        const int n = (int) std::min<juce::int64> (chunk, numSamples - pos);

        for (int i = 0; i < n; ++i)
        {
            const auto index = (size_t) (pos + i);
            block[(size_t) i] = index < click.size() ? click[index] : 0.0f;
        }

        const float* channels[] = { block.data() };

        if (! writer->writeFromFloatArrays (channels, 1, n))
            return juce::Result::fail ("Could not write the click");
    }

    writer.reset();

    if (! temp.overwriteTargetFileWithTemporary())
        return juce::Result::fail ("Could not replace " + file.getFileName());

    return juce::Result::ok();
}

} // namespace mapfile
} // namespace tt
