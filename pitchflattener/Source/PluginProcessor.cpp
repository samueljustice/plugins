#include "PluginProcessor.h"
#include "PluginEditor.h"

#include "DiagnosticLog.h"
#include "SoundCodec.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace
{
    namespace core = pitchflattener::core;

    juce::ParameterID pid (const char* id) { return { id, 1 }; }

    const juce::StringArray c_targetNames { "High Point", "Centre", "Low Point", "Start", "Note" };
    const juce::StringArray c_sourceNames { "Auto", "Voice", "Engine", "Tone" };
    const juce::StringArray c_characterNames { "Harmonic", "Transients", "Voice" };
    const juce::Identifier c_viewProperty { "displayView" };
    const juce::Identifier c_learnedProperty { "learnedId" };
    const juce::Identifier c_editsType { "EDITS" }, c_lineType { "LINE" };
    constexpr char c_stateMagic[4] = { 'P', 'F', 'S', '2' };

    using pitchflattener::DiagnosticLog;
    using pitchflattener::LogEvent;
    using pitchflattener::ThreadKind;
    using pitchflattener::EngineEvent;

    std::atomic<std::uint32_t> g_instances { 0 };

    juce::String hexId (std::uint64_t id) { return juce::String::toHexString ((juce::int64) id).paddedLeft ('0', 16); }
    std::uint64_t parseId (const juce::String& text) { return (std::uint64_t) text.getHexValue64(); }

    //==========================================================================
    // Log lines for the engine's events (formatted later, on the log's own thread).
    const char* targetName (std::int64_t t)
    {
        static const char* const names[] = { "high point", "centre", "low point", "start", "note" };
        return t >= 0 && t < 5 ? names[t] : "?";
    }

    const char* lineName (std::int64_t l)
    {
        static const char* const names[] = { "voice", "engine", "tone" };
        return l >= 0 && l < 3 ? names[l] : "none";
    }

    void formatDiagnosticEvents (const LogEvent& e, char* out, std::size_t size);

    void formatEngineEvent (const LogEvent& e, char* out, std::size_t size)
    {
        using Kind = EngineEvent::Kind;
        const auto id = [&e] (int k) { return (unsigned long long) (std::uint64_t) e.i[k]; };
        switch ((Kind) e.flags)
        {
            case Kind::CaptureStarted:
                std::snprintf (out, size, "learn: capture started (%lld ch, %.0f Hz)", (long long) e.i[0], e.d[0]); return;
            case Kind::CaptureEnded:
            {
                static const char* const why[] = { "a second of silence", "the audio stopped arriving", "Learn was disarmed", "two minutes reached", "too short: forgotten, Learn stays armed" };
                std::snprintf (out, size, "learn: capture ended (%s), %.3f s", why[juce::jlimit (0, 4, (int) e.i[0])], e.d[0] > 0.0 ? (double) e.i[1] / e.d[0] : 0.0);
                return;
            }
            case Kind::Analysed:
                std::snprintf (out, size, "analysed %s sound %016llx: %.3f s, %lld ch, %.0f Hz, reading %s, in %.0f ms",
                               e.i[4] != 0 ? "restored" : "learned", id (0), e.d[0] > 0.0 ? (double) e.i[3] / e.d[0] : 0.0,
                               (long long) e.i[2], e.d[0], lineName (e.i[1]), e.d[1]);
                return;
            case Kind::Stored:
                if (e.i[2] != 0)
                    std::snprintf (out, size, "sound %016llx is over %lld MB compressed: the state carries its id only", id (0),
                                   (long long) (pitchflattener::Engine::kMaxStoredBytes / (1024 * 1024)));
                else
                    std::snprintf (out, size, "sound %016llx compressed for the state: %lld bytes in %.0f ms", id (0), (long long) e.i[1], e.d[0]);
                return;
            case Kind::Rendered:
                if (e.i[4] == 0)
                    std::snprintf (out, size, "rendered: nothing to flatten (no steady pitch)");
                else
                    std::snprintf (out, size, "rendered: target %s, reading %s, %s, amount %.0f%%, %lld corrections: %.3f s, in %.0f ms",
                                   targetName (e.i[0]), lineName (e.i[1]), e.i[2] != 0 ? "keep length" : "varispeed", e.d[0] * 100.0,
                                   (long long) e.i[5], e.d[2] > 0.0 ? (double) e.i[3] / e.d[2] : 0.0, e.d[1]);
                return;
            case Kind::Cleared:
                std::snprintf (out, size, "cleared"); return;
            case Kind::Cancelled:
            {
                static const char* const what[] = { "armed", "listening", "analysing", "idle" };
                std::snprintf (out, size, "learn cancelled (while %s)", what[juce::jlimit (0, 3, (int) e.i[0])]);
                return;
            }
            case Kind::Adopted:
                std::snprintf (out, size, "took sound %016llx from another instance in this process%s", id (0),
                               e.i[1] != 0 ? ", with its render" : "; rendering it for these options");
                return;
            case Kind::RestoreStarted:
                std::snprintf (out, size, "restoring sound %016llx from the state (%lld bytes): decoding and analysing", id (0), (long long) e.i[1]); return;
            case Kind::RestoreFailed:
                std::snprintf (out, size, "could not decode sound %016llx from the state", id (0)); return;
            case Kind::Wait:
            {
                static const char* const what[] = { "the search", "a render", "the analysis" };
                std::snprintf (out, size, "waited %.1f ms for %s%s", e.d[0], what[juce::jlimit (0, 2, (int) e.i[0])], e.i[1] != 0 ? ": GAVE UP" : "");
                return;
            }
            case Kind::PaceChanged:
                std::snprintf (out, size, "audio now arriving %s (%.1fx real time)", e.i[0] != 0 ? "FASTER THAN REAL TIME: treated as a render" : "in real time", e.d[0]);
                return;
            case Kind::RunStarted:
                std::snprintf (out, size, "run %lld started: first block %lld, %lld ch, %.0f Hz, host says %s%s", (long long) e.i[0], (long long) e.i[1],
                               (long long) e.i[2], e.d[0], e.i[3] != 0 ? "offline" : "real time", e.i[4] != 0 ? ", bypassed" : "");
                return;
            case Kind::RunEnded:
            {
                const double audio = e.d[0] > 0.0 ? (double) e.i[2] / e.d[0] : 0.0;
                const auto flags = e.i[12];
                const char* mode = (flags & 1) != 0 ? "offline (host)" : (flags & 2) != 0 ? "faster than real time (measured)" : "real time";
                char match[160] = "no match";
                if (e.i[14] != 0)
                    std::snprintf (match, sizeof (match), "first match at input %.3f s = learned %.3f s", (double) e.i[6] / e.d[0], (double) e.i[7] / e.d[0]);
                char after[160] = "no learned sound ended in it";
                if (e.i[11] >= 0)
                    std::snprintf (after, sizeof (after), "input after the learned sound's end %lld samples (%s the latency)", (long long) e.i[11],
                                   e.i[11] >= e.i[10] ? "covers" : "SHORT OF");
                std::snprintf (out, size, "run %lld ended: %lld blocks, %.3f s of audio in %.3f s (%.1fx), %s; %lld searches, %lld matches (%s), %lld cancelled starts; "
                                          "output flattened %lld samples, dry %lld; latency %lld; %s; %lld waits (%.0f ms)%s",
                               (long long) e.i[0], (long long) e.i[1], audio, e.d[1], e.d[1] > 0.0 ? audio / e.d[1] : 0.0, mode, (long long) e.i[3],
                               (long long) e.i[4], match, (long long) e.i[5], (long long) e.i[8], (long long) e.i[9], (long long) e.i[10], after,
                               (long long) e.i[13], e.d[2], (flags & 4) != 0 ? "; A WAIT GAVE UP" : "");
                return;
            }
            case Kind::Searched:
            case Kind::Extended:
            case Kind::MatchFailed:
            case Kind::Reset:
            case Kind::Diagnosed:
                formatDiagnosticEvents (e, out, size);
                return;
        }
        std::snprintf (out, size, "engine event %u", (unsigned) e.flags);
    }

    void formatDiagnosticEvents (const LogEvent& e, char* out, std::size_t size)
    {
        using Kind = EngineEvent::Kind;
        const double rate = e.d[1] > 0.0 ? e.d[1] : 48000.0;
        const auto kind = (Kind) e.flags;
        if (kind == Kind::Searched)
        {
            static const char* const outcome[] = { "MATCHED", "matches in several places but too quiet to tell: refused", "residual too high: not the learned recording here", "nothing like it" };
            char next[96] = "";
            if (e.i[4] >= 0)
                std::snprintf (next, sizeof (next), "; next try %.0f ms later%s", 1000.0 * (double) e.i[4] / rate, e.i[5] != 0 ? " (quick retry at the sound's start)" : "");
            std::snprintf (out, size, "search: probe at input %.3f s; best place learned %.3f s, residual %.1f dB, %lld place(s) passed: %s; %.1f ms%s",
                           (double) e.i[0] / rate, e.i[1] >= 0 ? (double) e.i[1] / rate : -1.0, e.d[0], (long long) e.i[2],
                           outcome[juce::jlimit (0, 3, (int) e.i[3])], e.d[2], next);
            return;
        }
        else if (kind == Kind::Extended)
        {
            const double sr = e.d[0] > 0.0 ? e.d[0] : 48000.0;
            static const char* const why[] = { "nothing before it to take", "reached the sound's start", "reached the sound's start (a difference in its first 30 ms accepted)",
                                               "stopped: other material before it", "stopped at a reset or the learned sound's start" };
            std::snprintf (out, size, "match extended back from input %.3f s to %.3f s (%.1f ms): %s", (double) e.i[0] / sr, (double) e.i[1] / sr,
                           1000.0 * (double) (e.i[0] - e.i[1]) / sr, why[juce::jlimit (0, 4, (int) e.i[2])]);
            return;
        }
        else if (kind == Kind::MatchFailed)
        {
            const double sr = e.d[1] > 0.0 ? e.d[1] : 48000.0;
            static const char* const why[] = { "not the learned recording", "the input went silent where the sound is not", "other audio where the sound is silent" };
            std::snprintf (out, size, "match %s at input %.3f s: %s (window residual %.1f dB)", e.i[1] != 0 ? "CANCELLED before it was heard" : "let go",
                           (double) e.i[0] / sr, why[juce::jlimit (0, 2, (int) e.i[2])], e.d[0]);
            return;
        }
        else if (kind == Kind::Reset)
        {
            if (e.i[1] < 0)
                std::snprintf (out, size, "reset x%lld (before prepareToPlay)", (long long) e.i[0]);
            else
                std::snprintf (out, size, "reset x%lld at input %.3f s, %s", (long long) e.i[0], (double) e.i[1] / (e.d[0] > 0.0 ? e.d[0] : 48000.0),
                               e.i[2] != 0 ? "after audio had arrived in this run" : "before any audio in this run");
            return;
        }
        else if (kind == Kind::Diagnosed)
        {
            const double sr = e.d[0] > 0.0 ? e.d[0] : 48000.0;
            const auto db = [&e] (int k) { return (double) e.i[k] / 100.0; };
            std::snprintf (out, size,
                           "compare (%s) at input %.3f s = learned %.3f s, input %s, learned %s, residual dB, level free: "
                           "L/L %.1f, R/R %.1f, L/R %.1f, R/L %.1f, L/mix %.1f, R/mix %.1f, sum/sum %.1f, diff/diff %.1f; "
                           "L-R level input %+.2f dB, learned %+.2f dB; input L-R correlation %.3f; "
                           "best offset %+.3f samples (input later when +): mix residual %.1f dB there, %.1f dB at 0; "
                       "through a short filter %.1f dB (%.0f%% in 3 adjacent taps)",
                           e.i[2] != 0 ? "let go" : "near miss", (double) e.i[0] / sr, (double) e.i[1] / sr, e.i[3] != 0 ? "stereo" : "mono (or L = R)",
                           e.i[4] != 0 ? "stereo" : "mono", db (5), db (6), db (7), db (8), db (9), db (10), db (11), db (12), db (13), db (14),
                           (double) e.i[15] / 1000.0, e.d[1], e.d[2], e.d[3], e.d[4], 100.0 * e.d[5]);
            return;
        }
        std::snprintf (out, size, "engine event %u", (unsigned) e.flags);
    }

    void formatBypass (const LogEvent& e, char* out, std::size_t size)
    {
        std::snprintf (out, size, "%s", e.i[0] != 0 ? "bypassed" : "bypass off");
    }

    void formatNonRealtime (const LogEvent& e, char* out, std::size_t size)
    {
        std::snprintf (out, size, "setNonRealtime(%s)", e.i[0] != 0 ? "true" : "false");
    }

    ThreadKind threadOf (EngineEvent::Kind kind)
    {
        using Kind = EngineEvent::Kind;
        return kind == Kind::RunStarted || kind == Kind::PaceChanged || kind == Kind::Wait || kind == Kind::CaptureStarted || kind == Kind::MatchFailed
                       || kind == Kind::Reset
                   ? ThreadKind::Audio : ThreadKind::Other;
    }

    /** "A4", "C#3", or a plain number: the MIDI note it names (with cents after it, "A4 +12"). */
    double parseNote (const juce::String& text)
    {
        static const char* const names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        auto t = text.trim().toUpperCase();
        double cents = 0.0;

        if (const int space = t.indexOfChar (' '); space > 0)
        {
            cents = t.substring (space + 1).retainCharacters ("+-0123456789.").getDoubleValue();
            t = t.substring (0, space);
        }

        for (int pc = 11; pc >= 0; --pc)   // sharps first, so "C#" is not read as "C"
        {
            const juce::String name (names[pc]);
            if (t.startsWith (name) && t.length() > name.length())
            {
                const auto rest = t.substring (name.length());
                if (rest.containsOnly ("-0123456789"))
                    return (rest.getIntValue() + 1) * 12 + pc + cents / 100.0;
            }
        }

        return t.getDoubleValue();
    }
}

//==============================================================================
PitchFlattenerAudioProcessor::PitchFlattenerAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    , parameters (*this, nullptr, "PitchFlattener", createParameterLayout())
    , m_instance (++g_instances)
{
    DiagnosticLog::instance().retain();
    DiagnosticLog::instance().post (m_instance, ThreadKind::Message, "instance created in %s (%s), version %s",
                                    juce::PluginHostType().getHostDescription(), juce::AudioProcessor::getWrapperTypeDescription (wrapperType),
                                    JucePlugin_VersionString);

    pitchflattener::Engine::Hooks hooks;
    hooks.encode = [] (const core::Channels& sound, double rate) { return pitchflattener::codec::encode (sound, rate); };
    hooks.decode = [] (const std::vector<char>& bytes, core::Channels& sound, double& rate) { return pitchflattener::codec::decode (bytes, sound, rate); };
    hooks.optionsFor = [this] (const pitchflattener::Learned& learned) { return optionsFor (&learned); };
    hooks.saveCapture = [this] (const std::string& name, const core::Channels& audio, double rate) { saveCapture (name, audio, rate); };
    m_engine.setHooks (std::move (hooks));
    m_engine.setLogSink (&PitchFlattenerAudioProcessor::engineSink, this);

    syncOptions();
    startTimerHz (20);
}

PitchFlattenerAudioProcessor::~PitchFlattenerAudioProcessor()
{
    stopTimer();
    m_engine.closeRun();   // its summary, while the log is still attached
    DiagnosticLog::instance().post (m_instance, ThreadKind::Message, "instance destroyed");
    m_engine.setLogSink (nullptr, nullptr);
    m_engine.setHooks ({});
    DiagnosticLog::instance().release();
}

void PitchFlattenerAudioProcessor::engineSink (void* context, const EngineEvent& event) noexcept
{
    // Any thread, the audio thread included: no text is formatted here (see DiagnosticLog.h).
    const auto* self = static_cast<const PitchFlattenerAudioProcessor*> (context);
    LogEvent e;
    e.instance = self->m_instance;
    e.thread = threadOf (event.kind);
    e.format = formatEngineEvent;
    e.flags = (std::uint32_t) event.kind;
    for (int k = 0; k < 16; ++k)
        e.i[k] = event.i[k];
    for (int k = 0; k < 6; ++k)
        e.d[k] = event.d[k];
    DiagnosticLog::instance().postEvent (e);
}

juce::AudioProcessorValueTreeState::ParameterLayout PitchFlattenerAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<juce::AudioParameterChoice> (pid (targetId), "Target", c_targetNames, 0));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        pid (noteId), "Note", juce::NormalisableRange<float> (24.0f, 108.0f, 0.01f), 69.0f,
        juce::AudioParameterFloatAttributes()
            .withStringFromValueFunction ([] (float v, int) { return noteText (v); })
            .withValueFromStringFunction ([] (const juce::String& t) { return (float) juce::jlimit (24.0, 108.0, parseNote (t)); })));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        pid (amountId), "Amount", juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f,
        juce::AudioParameterFloatAttributes()
            .withLabel ("%")
            .withStringFromValueFunction ([] (float v, int) { return juce::String (juce::roundToInt (v)) + "%"; })
            .withValueFromStringFunction ([] (const juce::String& t) { return t.retainCharacters ("0123456789.").getFloatValue(); })));

    layout.add (std::make_unique<juce::AudioParameterChoice> (pid (sourceId), "Source", c_sourceNames, 0));
    layout.add (std::make_unique<juce::AudioParameterBool> (pid (keepLengthId), "Keep Length", false,
        juce::AudioParameterBoolAttributes().withStringFromValueFunction ([] (bool on, int) { return on ? "Keep length" : "Varispeed"; })));
    layout.add (std::make_unique<juce::AudioParameterChoice> (pid (characterId), "Character", c_characterNames, 0));

    return layout;
}

juce::String PitchFlattenerAudioProcessor::noteText (double midi)
{
    static const char* const names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const int nearest = (int) std::lround (midi);
    const int cents = (int) std::lround ((midi - nearest) * 100.0);
    auto text = juce::String (names[((nearest % 12) + 12) % 12]) + juce::String (nearest / 12 - 1);

    if (cents != 0)
        text << (cents > 0 ? " +" : " ") << cents;

    return text;
}

int PitchFlattenerAudioProcessor::getDisplayView() const
{
    const int v = (int) parameters.state.getProperty (c_viewProperty, kViewUnset);
    return v == kViewScope || v == kViewFlight ? v : kViewUnset;
}

void PitchFlattenerAudioProcessor::setDisplayView (int view)
{
    // A property of the parameters' tree, so it is saved and restored with them.
    parameters.state.setProperty (c_viewProperty, view == kViewScope || view == kViewFlight ? view : kViewUnset, nullptr);
}

//==============================================================================
juce::File PitchFlattenerAudioProcessor::captureFolder()
{
    return juce::File (juce::String::fromUTF8 (DiagnosticLog::instance().filePath().c_str())).getParentDirectory().getChildFile ("PitchFlattener captures");
}

void PitchFlattenerAudioProcessor::saveCapture (const std::string& name, const core::Channels& audio, double rate)
{
    // The engine's house thread. At most 3 captures (input and learned) per process run, and the
    // folder keeps the newest 3; nothing happens if the folder cannot be written.
    static std::atomic<int> s_files { 0 };
    if (audio.empty() || audio[0].empty() || s_files.fetch_add (1) >= 6)
        return;
    auto folder = captureFolder();
    if (! folder.createDirectory())
        return;
    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H%M%S");
    const auto file = folder.getChildFile (stamp + " inst " + juce::String (m_instance) + " " + juce::String::fromUTF8 (name.c_str()) + ".wav");
    file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    if (stream == nullptr)
        return;
    std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), rate, (unsigned int) audio.size(), 32, {}, 0));
    if (writer == nullptr)
        return;
    stream.release();   // the writer owns it now
    juce::AudioBuffer<float> buffer ((int) audio.size(), (int) audio[0].size());
    for (int c = 0; c < (int) audio.size(); ++c)
        buffer.copyFrom (c, 0, audio[(size_t) c].data(), (int) audio[(size_t) c].size());
    const bool ok = writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
    writer.reset();
    DiagnosticLog::instance().post (m_instance, ThreadKind::Other, "%s: %s (%.2f s, %d ch, 32-bit float)", ok ? "captured" : "could not capture",
                                    file.getFullPathName().toRawUTF8(), (double) audio[0].size() / rate, (int) audio.size());
    // Keep the newest 3 captures (6 files) in the folder.
    auto files = folder.findChildFiles (juce::File::findFiles, false, "*.wav");
    std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b) { return a.getLastModificationTime() > b.getLastModificationTime(); });
    for (int i = 6; i < files.size(); ++i)
        files.getReference (i).deleteFile();
}

int PitchFlattenerAudioProcessor::lineFor (const core::Analysis& analysis) const
{
    const int source = juce::jlimit (0, core::kNumTrackers, (int) parameters.getRawParameterValue (sourceId)->load()) - 1;
    if (source >= 0 && analysis.lines[(size_t) source].usable)
        return source;
    return analysis.pick;
}

core::Options PitchFlattenerAudioProcessor::optionsFor (const pitchflattener::Learned* learned) const
{
    core::Options o;
    const auto target = (int) parameters.getRawParameterValue (targetId)->load();
    o.target = (core::Target) juce::jlimit (0, 4, target);
    o.noteMidi = parameters.getRawParameterValue (noteId)->load();
    o.amount = parameters.getRawParameterValue (amountId)->load() / 100.0;
    o.line = juce::jlimit (0, core::kNumTrackers, (int) parameters.getRawParameterValue (sourceId)->load()) - 1;   // Auto = -1
    o.keepLength = parameters.getRawParameterValue (keepLengthId)->load() > 0.5f;
    o.character = (core::Character) juce::jlimit (0, 2, (int) parameters.getRawParameterValue (characterId)->load());
    // The corrections on the reading in use, if they were made on this sound.
    if (learned != nullptr)
    {
        const std::lock_guard<std::mutex> lock (m_editsMutex);
        if (learned->id == m_editsFor)
            if (const int line = lineFor (learned->analysis); line >= 0)
                o.edits = m_edits[(size_t) line];
    }
    return o;
}

core::Options PitchFlattenerAudioProcessor::optionsFromParameters() const
{
    const auto r = m_engine.result();
    return optionsFor (r != nullptr ? r->learned.get() : nullptr);
}

int PitchFlattenerAudioProcessor::lineInUse() const
{
    const auto r = m_engine.result();
    if (r == nullptr || r->learned == nullptr)
        return -1;
    return lineFor (r->learned->analysis);
}

std::vector<core::CurvePoint> PitchFlattenerAudioProcessor::getEdits (int line) const
{
    const auto r = m_engine.result();
    const std::lock_guard<std::mutex> lock (m_editsMutex);
    if (line < 0 || line >= core::kNumTrackers || r == nullptr || r->learned == nullptr || r->learned->id != m_editsFor)
        return {};
    return m_edits[(size_t) line];
}

void PitchFlattenerAudioProcessor::setEdits (int line, std::vector<core::CurvePoint> edits)
{
    const auto r = m_engine.result();
    if (line < 0 || line >= core::kNumTrackers || r == nullptr || r->learned == nullptr)
        return;
    {
        const std::lock_guard<std::mutex> lock (m_editsMutex);
        if (r->learned->id != m_editsFor)
        {
            m_edits = {};
            m_editsFor = r->learned->id;
        }
        if (edits.size() > kMaxEdits)
            edits.resize (kMaxEdits);
        std::sort (edits.begin(), edits.end(), [] (const core::CurvePoint& a, const core::CurvePoint& b) { return a.seconds < b.seconds; });
        m_edits[(size_t) line] = std::move (edits);
    }
    syncOptions();
}

void PitchFlattenerAudioProcessor::syncOptions()
{
    // Another sound learned (or none, or one still being restored): the corrections were for the
    // one before, unless the sound being restored is the one they belong to.
    {
        const auto current = m_engine.learnedId();
        const std::lock_guard<std::mutex> lock (m_editsMutex);
        if (m_editsFor != 0 && current != m_editsFor)
        {
            m_edits = {};
            m_editsFor = 0;
        }
    }
    m_engine.setOptions (optionsFromParameters());
}

void PitchFlattenerAudioProcessor::setParameterFromEditor (const char* id, float plainValue)
{
    if (auto* param = parameters.getParameter (id))
    {
        param->beginChangeGesture();
        param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
        param->endChangeGesture();
    }
}

void PitchFlattenerAudioProcessor::timerCallback()
{
    syncOptions();

    // A render longer than the sound (flattened to its low point) carries on after the input
    // stops: that much tail, for hosts that render one.
    double tail = 0.0;
    if (const auto r = m_engine.result(); r != nullptr && r->playable())
        tail = std::max (0.0, (double) r->flattened.channels[0].size() - (double) r->learned->locator.length()) / r->learned->sampleRate;
    m_tailSeconds.store (tail);
}

double PitchFlattenerAudioProcessor::getTailLengthSeconds() const
{
    return m_tailSeconds.load();
}

//==============================================================================
void PitchFlattenerAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    m_engine.prepare (sampleRate, samplesPerBlock);
    setLatencySamples (m_engine.latencySamples());
    DiagnosticLog::instance().post (m_instance, ThreadKind::Message,
                                    "prepareToPlay: %.0f Hz, block %d, %d in / %d out, host says %s; latency %d samples (%.3f s)",
                                    sampleRate, samplesPerBlock, getTotalNumInputChannels(), getTotalNumOutputChannels(),
                                    isNonRealtime() ? "offline" : "real time", m_engine.latencySamples(),
                                    sampleRate > 0.0 ? m_engine.latencySamples() / sampleRate : 0.0);
    syncOptions();
}

void PitchFlattenerAudioProcessor::releaseResources()
{
    m_engine.closeRun();
    DiagnosticLog::instance().post (m_instance, ThreadKind::Message, "releaseResources");
}

void PitchFlattenerAudioProcessor::reset()
{
    // Some hosts call this from the message thread (an AU's bypass does): only a request here,
    // which the audio thread carries out at its next block.
    m_engine.requestReset();   // the engine logs it (a burst of them as one line)
}

void PitchFlattenerAudioProcessor::setNonRealtime (bool isNonRealtime) noexcept
{
    AudioProcessor::setNonRealtime (isNonRealtime);
    m_engine.setNonRealtime (isNonRealtime);
    if (m_nonRealtimeLogged.exchange (isNonRealtime ? 1 : 0) != (isNonRealtime ? 1 : 0))
    {
        LogEvent e;
        e.instance = m_instance;
        e.format = formatNonRealtime;
        e.i[0] = isNonRealtime ? 1 : 0;
        DiagnosticLog::instance().postEvent (e);
    }
}

bool PitchFlattenerAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();

    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;

    return in == out;
}

void PitchFlattenerAudioProcessor::noteBypass (bool bypassed) noexcept
{
    if (bypassed == m_wasBypassed)
        return;
    m_wasBypassed = bypassed;
    LogEvent e;
    e.instance = m_instance;
    e.thread = ThreadKind::Audio;
    e.format = formatBypass;
    e.i[0] = bypassed ? 1 : 0;
    DiagnosticLog::instance().postEvent (e);
}

void PitchFlattenerAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    noteBypass (false);
    const int channels = juce::jmin (buffer.getNumChannels(), getTotalNumInputChannels(), pitchflattener::Engine::kMaxChannels);

    for (int c = getTotalNumInputChannels(); c < buffer.getNumChannels(); ++c)
        buffer.clear (c, 0, buffer.getNumSamples());

    m_engine.process (buffer.getArrayOfWritePointers(), channels, buffer.getNumSamples());
}

void PitchFlattenerAudioProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    noteBypass (true);
    const int channels = juce::jmin (buffer.getNumChannels(), getTotalNumInputChannels(), pitchflattener::Engine::kMaxChannels);

    for (int c = getTotalNumInputChannels(); c < buffer.getNumChannels(); ++c)
        buffer.clear (c, 0, buffer.getNumSamples());

    // Delayed like the processed audio, so bypassing does not shift the track in time.
    m_engine.processBypassed (buffer.getArrayOfWritePointers(), channels, buffer.getNumSamples());
}

//==============================================================================
juce::AudioProcessorEditor* PitchFlattenerAudioProcessor::createEditor()
{
    return new PitchFlattenerAudioProcessorEditor (*this);
}

void PitchFlattenerAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    const auto started = std::chrono::steady_clock::now();
    auto state = parameters.copyState();
    state.setProperty ("stateVersion", stateVersion, nullptr);

    // The learned sound: its id, its hand corrections, and the sound itself, compressed once when
    // it was learned (a copy of those bytes is all a save costs).
    const auto r = m_engine.result();
    const auto* learned = r != nullptr ? r->learned.get() : nullptr;
    std::shared_ptr<const std::vector<char>> stored;
    if (learned != nullptr)
    {
        state.setProperty (c_learnedProperty, hexId (learned->id), nullptr);
        stored = learned->stored;
        const std::lock_guard<std::mutex> lock (m_editsMutex);
        if (m_editsFor == learned->id)
        {
            juce::ValueTree edits (c_editsType);
            for (int line = 0; line < core::kNumTrackers; ++line)
            {
                if (m_edits[(size_t) line].empty())
                    continue;
                juce::String points;
                for (const auto& p : m_edits[(size_t) line])
                    points << juce::String (p.seconds, 6) << ":" << juce::String (p.semitones, 6) << ";";
                juce::ValueTree child (c_lineType);
                child.setProperty ("index", line, nullptr);
                child.setProperty ("points", points, nullptr);
                edits.appendChild (child, nullptr);
            }
            state.appendChild (edits, nullptr);
        }
    }
    else
        state.removeProperty (c_learnedProperty, nullptr);

    const auto xml = state.toXmlString();
    const auto utf8 = xml.toUTF8();
    const auto xmlBytes = (int) xml.getNumBytesAsUTF8();
    juce::MemoryOutputStream out (destData, false);
    out.write (c_stateMagic, 4);
    out.writeInt (stateVersion);
    out.writeInt (xmlBytes);
    out.write (utf8.getAddress(), (size_t) xmlBytes);
    out.writeInt64 (stored != nullptr ? (juce::int64) stored->size() : 0);
    if (stored != nullptr)
        out.write (stored->data(), stored->size());
    out.flush();

    DiagnosticLog::instance().post (m_instance, ThreadKind::Other, "getStateInformation: %lld bytes in %.1f ms; %s%s",
                                    (long long) destData.getSize(),
                                    std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - started).count(),
                                    learned != nullptr ? ("learned sound " + hexId (learned->id)).toRawUTF8() : "nothing learned",
                                    learned == nullptr ? "" : stored != nullptr ? ", with its audio" : ", id only (the audio is too large to store)");
}

void PitchFlattenerAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    // Version 2: "PFS2", the XML, the sound. Version 1: JUCE's XML binary (no sound).
    std::unique_ptr<juce::XmlElement> xml;
    std::shared_ptr<const std::vector<char>> stored;
    int version = 1;
    if (data != nullptr && sizeInBytes >= 12 && std::memcmp (data, c_stateMagic, 4) == 0)
    {
        juce::MemoryInputStream in (data, (size_t) sizeInBytes, false);
        in.skipNextBytes (4);
        version = in.readInt();
        const int xmlBytes = in.readInt();
        if (xmlBytes > 0 && xmlBytes <= in.getNumBytesRemaining())
        {
            juce::MemoryBlock text;
            in.readIntoMemoryBlock (text, xmlBytes);
            xml = juce::parseXML (text.toString());
        }
        const auto soundBytes = in.getNumBytesRemaining() >= 8 ? in.readInt64() : 0;
        if (soundBytes > 0 && soundBytes <= in.getNumBytesRemaining())
        {
            std::vector<char> bytes ((size_t) soundBytes);
            in.read (bytes.data(), (int) soundBytes);
            stored = std::make_shared<const std::vector<char>> (std::move (bytes));
        }
    }
    else
        xml = getXmlFromBinary (data, sizeInBytes);

    if (xml == nullptr || ! xml->hasTagName (parameters.state.getType()))
    {
        DiagnosticLog::instance().post (m_instance, ThreadKind::Other, "setStateInformation: %d bytes, not a state of this plug-in: ignored", sizeInBytes);
        return;   // not a state of this plug-in: keep the defaults
    }

    const auto tree = juce::ValueTree::fromXml (*xml);
    version = (int) tree.getProperty ("stateVersion", version);
    if (version <= stateVersion)
    {
        auto parametersOnly = tree.createCopy();
        parametersOnly.removeChild (parametersOnly.getChildWithName (c_editsType), nullptr);
        parameters.replaceState (parametersOnly);
    }
    else
    {
        // From a newer version: take the values of the parameters this version knows, by ID, and
        // leave the rest of what it holds (its meaning may have changed).
        for (const auto& child : tree)
            if (auto* param = parameters.getParameter (child.getProperty ("id").toString()))
                if (child.hasProperty ("value"))
                    param->setValueNotifyingHost (param->convertTo0to1 ((float) child.getProperty ("value")));
    }

    // The learned sound it names, if any (a version 1 state names none: what this instance has stays).
    const auto id = parseId (tree.getProperty (c_learnedProperty).toString());
    const char* outcome = "no learned sound in it";
    if (id != 0)
    {
        {
            const std::lock_guard<std::mutex> lock (m_editsMutex);
            m_edits = {};
            m_editsFor = id;
            for (const auto& child : tree.getChildWithName (c_editsType))
            {
                const int line = (int) child.getProperty ("index", -1);
                if (line < 0 || line >= core::kNumTrackers)
                    continue;
                auto& points = m_edits[(size_t) line];
                for (const auto& token : juce::StringArray::fromTokens (child.getProperty ("points").toString(), ";", ""))
                    if (token.containsChar (':') && points.size() < kMaxEdits)
                        points.push_back ({ token.upToFirstOccurrenceOf (":", false, false).getDoubleValue(),
                                            token.fromFirstOccurrenceOf (":", false, false).getDoubleValue() });
            }
        }
        if (m_engine.learnedId() == id)
            outcome = "this instance has that sound already";
        else if (const auto learned = pitchflattener::Engine::findRegistered (id); learned != nullptr && m_engine.adoptLearned (id, optionsFor (learned.get())))
            outcome = "found in this process (registry hit): taken at once";
        else if (stored != nullptr)
        {
            m_engine.restoreLearned (id, stored);
            outcome = "not in this process (registry miss): restoring from the audio in the state";
        }
        else
            outcome = "not in this process (registry miss) and no audio in the state: nothing to restore";
    }
    DiagnosticLog::instance().post (m_instance, ThreadKind::Other, "setStateInformation: %d bytes, version %d, %s%s; %s; audio in it: %s",
                                    sizeInBytes, version, id != 0 ? "learned sound " : "", id != 0 ? hexId (id).toRawUTF8() : "", outcome,
                                    stored != nullptr ? (juce::String ((juce::int64) stored->size()) + " bytes").toRawUTF8() : "none");
    syncOptions();
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PitchFlattenerAudioProcessor();
}
