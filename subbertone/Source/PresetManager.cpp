#include "PresetManager.h"

#include <cmath>
#include <cstring>
#include <map>

namespace
{
    const juce::Identifier c_presetTag      { "SubbertonePreset" };
    const juce::Identifier c_paramTag       { "Param" };
    const juce::Identifier c_idAttr         { "id" };
    const juce::Identifier c_valueAttr      { "value" };
    const juce::Identifier c_nameAttr       { "name" };
    const juce::Identifier c_versionAttr    { "pluginVersion" };

    const juce::Identifier c_statePresetName     { "presetName" };
    const juce::Identifier c_statePresetFactory  { "presetIsFactory" };
    const juce::Identifier c_statePresetKind     { "presetKind" };     // "factory" | "user" | "unsaved"
    const juce::Identifier c_statePresetFile     { "presetFile" };     // user presets: file name in the user folder
    const juce::Identifier c_statePresetModified { "presetModified" };

    // Presets are ~2 KB and flat (<SubbertonePreset> holding <Param/> elements). Anything much
    // bigger or deeper is not a preset, and is never handed to the (recursive) XML parser.
    constexpr juce::int64 c_maxPresetFileBytes = 256 * 1024;
    constexpr int c_maxElementDepth = 16;

    struct FactoryPreset
    {
        const char* name;
        const char* category;                 // menu group ("" = top level)
        std::map<juce::String, float> values; // real values; anything missing = default
    };

    using ValueMap = std::map<juce::String, float>;

    // Sets every harmonic bar explicitly (so presets never inherit stray bars):
    // levels lists {k, percent}; H1 (the sub) defaults to 100% unless given.
    ValueMap withHarmonics(ValueMap values, std::initializer_list<std::pair<int, float>> levels)
    {
        for (int k = 1; k <= 16; ++k)
            values["harmonic" + juce::String(k)] = (k == 1) ? 100.0f : 0.0f;

        for (const auto& [k, percent] : levels)
            values["harmonic" + juce::String(k)] = percent;

        return values;
    }

    // Saturation indices: 0 Tape, 1 Valve, 2 Console, 3 Transformer, 4 Rectifier, 5 Wavefolder
    // Filter modes:       0 Lowpass, 1 Bandpass, 2 Notch
    // Glide: the default is 12 ms (Subbertone 1.1.0's fixed glide). Presets that need a smoother
    // or tighter sub (engines, drones, hum, kicks) set pitchGlide explicitly.
    const std::vector<FactoryPreset>& factoryPresets()
    {
        static const std::vector<FactoryPreset> presets
        {
            { "Init", "", {} },

            // MUSIC
            { "Clean Sub", "Music",
              { { "mix", 35.0f }, { "distortion", 0.0f } } },

            { "Sub Only", "Music",
              { { "mix", 100.0f }, { "distortion", 15.0f }, { "postDriveLowpass", 300.0f } } },

            { "Warm Octave", "Music",
              { { "mix", 45.0f }, { "distortion", 50.0f }, { "distortionType", 1.0f }, { "distortionTone", 800.0f } } },

            { "Even Octave", "Music",
              { { "mix", 45.0f }, { "distortion", 60.0f }, { "distortionType", 4.0f }, { "distortionTone", 1500.0f } } },

            { "Bass Octaver", "Music",
              withHarmonics({ { "mix", 40.0f }, { "distortion", 12.0f }, { "distortionType", 0.0f }, { "overtoneAmount", 55.0f }, { "fundamentalLimit", 300.0f }, { "pitchGlide", 8.0f }, { "pitchThreshold", -45.0f }, { "postDriveLowpass", 1000.0f } },
                            { { 1, 100.0f }, { 3, 22.0f }, { 5, 8.0f }, { 7, 3.0f } }) },
            { "Kick Sub Tail", "Music",
              withHarmonics({ { "mix", 40.0f }, { "distortion", 10.0f }, { "distortionType", 0.0f }, { "overtoneAmount", 45.0f }, { "fundamentalLimit", 150.0f }, { "pitchGlide", 8.0f }, { "pitchThreshold", -48.0f }, { "postDriveLowpass", 250.0f } },
                            { { 1, 100.0f }, { 3, 22.0f }, { 5, 8.0f } }) },
            { "808 Octave Drop", "Music",
              withHarmonics({ { "mix", 38.0f }, { "distortion", 15.0f }, { "distortionType", 0.0f }, { "overtoneAmount", 60.0f }, { "fundamentalLimit", 140.0f }, { "pitchGlide", 15.0f }, { "pitchThreshold", -45.0f }, { "postDriveLowpass", 800.0f }, { "loCutOn", 1.0f }, { "loCutFreq", 25.0f } },
                            { { 1, 70.0f }, { 3, 32.0f }, { 5, 14.0f }, { 7, 14.0f }, { 9, 10.0f }, { 11, 6.0f } }) },
            { "Vocal Giant", "Music",
              withHarmonics({ { "mix", 38.0f }, { "distortion", 10.0f }, { "distortionType", 0.0f }, { "overtoneAmount", 70.0f }, { "fundamentalLimit", 420.0f }, { "pitchGlide", 10.0f }, { "pitchThreshold", -45.0f }, { "postDriveLowpass", 3000.0f } },
                            { { 1, 100.0f }, { 3, 30.0f }, { 5, 24.0f }, { 7, 14.0f }, { 9, 7.0f }, { 11, 3.0f } }) },
            { "Drone Undertone", "Music",
              withHarmonics({ { "mix", 45.0f }, { "distortion", 6.0f }, { "distortionType", 0.0f }, { "overtoneAmount", 55.0f }, { "fundamentalLimit", 280.0f }, { "pitchGlide", 250.0f }, { "pitchThreshold", -55.0f }, { "postDriveLowpass", 2000.0f } },
                            { { 1, 100.0f }, { 2, 18.0f }, { 3, 24.0f }, { 4, 12.0f }, { 5, 12.0f }, { 6, 6.0f }, { 7, 6.0f }, { 8, 3.0f }, { 9, 3.0f }, { 10, 2.0f } }) },

            // CHARACTER: drive, filter and overtone colours
            { "Transformer Body", "Character",
              { { "mix", 40.0f }, { "distortion", 40.0f }, { "distortionType", 3.0f }, { "distortionTone", 1200.0f } } },

            { "Console Grit", "Character",
              { { "mix", 45.0f }, { "distortion", 80.0f }, { "distortionType", 2.0f }, { "distortionTone", 2500.0f },
                { "filterResonance", 25.0f }, { "postDriveLowpass", 8000.0f } } },

            { "Talking Growl", "Character",
              { { "mix", 50.0f }, { "distortion", 75.0f }, { "distortionType", 0.0f }, { "distortionTone", 400.0f },
                { "filterMode", 1.0f }, { "filterResonance", 60.0f }, { "filterEnvAmount", 2.5f }, { "filterEnvRelease", 150.0f } } },

            { "Fold Sweep", "Character",
              { { "mix", 45.0f }, { "distortion", 70.0f }, { "distortionType", 5.0f }, { "distortionTone", 600.0f },
                { "filterResonance", 40.0f }, { "filterEnvAmount", 3.0f }, { "filterEnvRelease", 300.0f } } },

            { "Hollow Notch", "Character",
              { { "mix", 45.0f }, { "distortion", 65.0f }, { "distortionType", 1.0f }, { "distortionTone", 900.0f },
                { "filterMode", 2.0f }, { "filterResonance", 50.0f }, { "filterEnvAmount", -1.5f } } },

            { "Upper Partials", "Character",
              withHarmonics({ { "mix", 45.0f }, { "distortion", 30.0f }, { "distortionType", 1.0f }, { "overtoneAmount", 60.0f } },
                            { { 5, 30.0f }, { 6, 25.0f }, { 7, 21.0f }, { 8, 19.0f }, { 9, 17.0f }, { 10, 15.0f } }) },

            // TRANSLATION: MaxxBass-style overtones so the sub reads on small speakers
            { "Small Speaker", "Translation",
              withHarmonics({ { "mix", 40.0f }, { "distortion", 0.0f }, { "overtoneAmount", 70.0f } },
                            { { 2, 60.0f }, { 3, 40.0f }, { 4, 30.0f }, { 5, 24.0f }, { 6, 20.0f } }) },

            { "Laptop Lift", "Translation",
              withHarmonics({ { "mix", 35.0f }, { "distortion", 0.0f }, { "overtoneAmount", 100.0f }, { "postDriveLowpass", 6000.0f } },
                            { { 1, 60.0f }, { 3, 50.0f }, { 4, 38.0f }, { 5, 30.0f }, { 6, 25.0f }, { 7, 21.0f }, { 8, 19.0f } }) },

            { "Phantom Sub", "Translation",
              withHarmonics({ { "mix", 45.0f }, { "distortion", 0.0f }, { "distortionType", 0.0f }, { "overtoneAmount", 65.0f }, { "fundamentalLimit", 250.0f }, { "pitchGlide", 10.0f }, { "pitchThreshold", -48.0f }, { "postDriveLowpass", 5000.0f }, { "limiterOn", 1.0f }, { "limiterCeiling", -1.0f } },
                            { { 1, 0.0f }, { 3, 60.0f }, { 4, 18.0f }, { 5, 52.0f }, { 6, 14.0f }, { 7, 42.0f }, { 8, 10.0f }, { 9, 32.0f }, { 10, 6.0f }, { 11, 22.0f }, { 12, 4.0f }, { 13, 14.0f }, { 14, 2.0f }, { 15, 8.0f }, { 16, 2.0f } }) },

            // ENGINES: harmonic recipes from engine-order theory and what the tracker locks onto
            { "Engine Rumble", "Engines",
              { { "mix", 40.0f }, { "distortion", 35.0f }, { "distortionType", 0.0f }, { "distortionTone", 700.0f },
                { "pitchThreshold", -50.0f }, { "fundamentalLimit", 400.0f }, { "outputGain", 3.0f }, { "pitchGlide", 120.0f } } },

            { "Inline-4 Rasp", "Engines",
              withHarmonics({ { "mix", 40.0f }, { "distortion", 8.0f }, { "distortionType", 4.0f }, { "overtoneAmount", 65.0f }, { "fundamentalLimit", 280.0f }, { "pitchGlide", 35.0f }, { "pitchThreshold", -50.0f }, { "postDriveLowpass", 2500.0f } },
                            { { 1, 100.0f }, { 2, 30.0f }, { 3, 16.0f }, { 4, 25.0f }, { 5, 8.0f }, { 6, 16.0f }, { 8, 9.0f }, { 10, 5.0f }, { 12, 3.0f } }) },
            { "Inline-6 Purr", "Engines",
              withHarmonics({ { "mix", 40.0f }, { "distortion", 8.0f }, { "distortionType", 4.0f }, { "overtoneAmount", 60.0f }, { "fundamentalLimit", 400.0f }, { "pitchGlide", 40.0f }, { "pitchThreshold", -50.0f }, { "postDriveLowpass", 1500.0f } },
                            { { 1, 100.0f }, { 2, 30.0f }, { 3, 5.0f }, { 4, 26.0f }, { 6, 10.0f }, { 8, 5.0f } }) },
            { "Flat-6 Howl", "Engines",
              withHarmonics({ { "mix", 40.0f }, { "distortion", 8.0f }, { "distortionType", 4.0f }, { "overtoneAmount", 65.0f }, { "fundamentalLimit", 420.0f }, { "pitchGlide", 30.0f }, { "pitchThreshold", -50.0f }, { "postDriveLowpass", 3500.0f } },
                            { { 1, 90.0f }, { 2, 28.0f }, { 3, 10.0f }, { 4, 42.0f }, { 5, 4.0f }, { 6, 28.0f }, { 8, 18.0f }, { 10, 10.0f }, { 12, 5.0f } }) },
            { "V8 Muscle Burble", "Engines",
              withHarmonics({ { "mix", 45.0f }, { "distortion", 6.0f }, { "distortionType", 0.0f }, { "overtoneAmount", 65.0f }, { "fundamentalLimit", 450.0f }, { "pitchGlide", 40.0f }, { "pitchThreshold", -50.0f }, { "postDriveLowpass", 2200.0f }, { "loCutOn", 1.0f }, { "loCutFreq", 30.0f } },
                            { { 1, 100.0f }, { 2, 28.0f }, { 3, 6.0f }, { 4, 22.0f }, { 5, 2.0f }, { 6, 14.0f }, { 8, 8.0f }, { 10, 3.0f }, { 12, 2.0f } }) },
            { "Flat-Plane V8 Wail", "Engines",
              withHarmonics({ { "mix", 40.0f }, { "distortion", 8.0f }, { "distortionType", 4.0f }, { "overtoneAmount", 70.0f }, { "fundamentalLimit", 560.0f }, { "pitchGlide", 30.0f }, { "pitchThreshold", -50.0f }, { "postDriveLowpass", 5000.0f } },
                            { { 1, 90.0f }, { 2, 30.0f }, { 3, 5.0f }, { 4, 42.0f }, { 5, 3.0f }, { 6, 18.0f }, { 8, 20.0f }, { 10, 6.0f }, { 12, 8.0f }, { 14, 3.0f }, { 16, 3.0f } }) },
            { "V10 Shriek", "Engines",
              withHarmonics({ { "mix", 38.0f }, { "distortion", 8.0f }, { "distortionType", 4.0f }, { "overtoneAmount", 65.0f }, { "fundamentalLimit", 700.0f }, { "pitchGlide", 30.0f }, { "pitchThreshold", -50.0f }, { "postDriveLowpass", 5000.0f } },
                            { { 1, 90.0f }, { 2, 30.0f }, { 3, 4.0f }, { 4, 42.0f }, { 5, 2.0f }, { 6, 26.0f }, { 8, 20.0f }, { 10, 12.0f }, { 12, 8.0f }, { 14, 4.0f }, { 16, 3.0f } }) },
            { "V12 Tearing Silk", "Engines",
              withHarmonics({ { "mix", 35.0f }, { "distortion", 8.0f }, { "distortionType", 1.0f }, { "overtoneAmount", 60.0f }, { "fundamentalLimit", 800.0f }, { "pitchGlide", 30.0f }, { "pitchThreshold", -50.0f }, { "postDriveLowpass", 4500.0f } },
                            { { 1, 85.0f }, { 2, 35.0f }, { 3, 15.0f }, { 4, 38.0f }, { 5, 5.0f }, { 6, 22.0f }, { 7, 3.0f }, { 8, 14.0f }, { 10, 8.0f }, { 12, 6.0f }, { 14, 2.0f }, { 16, 2.0f } }) },
            { "V-Twin High Revs", "Engines",
              withHarmonics({ { "mix", 35.0f }, { "distortion", 8.0f }, { "distortionType", 4.0f }, { "overtoneAmount", 60.0f }, { "fundamentalLimit", 120.0f }, { "pitchGlide", 60.0f }, { "pitchThreshold", -50.0f }, { "postDriveLowpass", 900.0f } },
                            { { 1, 60.0f }, { 2, 24.0f }, { 4, 26.0f }, { 6, 12.0f }, { 8, 6.0f }, { 10, 6.0f } }) },
            { "Single High Revs", "Engines",
              withHarmonics({ { "mix", 35.0f }, { "distortion", 8.0f }, { "distortionType", 4.0f }, { "overtoneAmount", 65.0f }, { "fundamentalLimit", 100.0f }, { "pitchGlide", 40.0f }, { "pitchThreshold", -48.0f }, { "postDriveLowpass", 900.0f } },
                            { { 1, 60.0f }, { 2, 35.0f }, { 4, 30.0f }, { 6, 16.0f }, { 8, 9.0f }, { 10, 5.0f }, { 12, 2.0f } }) },
            { "Boxer Rumble", "Engines",
              withHarmonics({ { "mix", 40.0f }, { "distortion", 8.0f }, { "distortionType", 4.0f }, { "overtoneAmount", 60.0f }, { "fundamentalLimit", 180.0f }, { "pitchGlide", 40.0f }, { "pitchThreshold", -50.0f }, { "postDriveLowpass", 1500.0f }, { "loCutOn", 1.0f }, { "loCutFreq", 30.0f } },
                            { { 1, 100.0f }, { 2, 30.0f }, { 3, 5.0f }, { 4, 20.0f }, { 5, 2.0f }, { 6, 9.0f }, { 8, 5.0f }, { 10, 2.0f } }) },
            { "2-Stroke Brap", "Engines",
              withHarmonics({ { "mix", 40.0f }, { "distortion", 8.0f }, { "distortionType", 4.0f }, { "overtoneAmount", 65.0f }, { "fundamentalLimit", 220.0f }, { "pitchGlide", 25.0f }, { "pitchThreshold", -48.0f }, { "postDriveLowpass", 3500.0f } },
                            { { 1, 90.0f }, { 2, 30.0f }, { 3, 8.0f }, { 4, 32.0f }, { 5, 3.0f }, { 6, 20.0f }, { 8, 13.0f }, { 10, 8.0f }, { 12, 5.0f }, { 14, 3.0f }, { 16, 2.0f } }) },

            // UTILITY
            { "Mains Hum 50-60", "Utility",
              withHarmonics({ { "mix", 45.0f }, { "distortion", 10.0f }, { "distortionType", 4.0f }, { "overtoneAmount", 60.0f }, { "fundamentalLimit", 130.0f }, { "pitchGlide", 400.0f }, { "pitchThreshold", -60.0f }, { "postDriveLowpass", 1500.0f } },
                            { { 1, 90.0f }, { 2, 30.0f }, { 4, 18.0f }, { 6, 10.0f }, { 8, 6.0f }, { 10, 3.0f }, { 12, 2.0f } }) },
        };

        return presets;
    }

    //==========================================================================
    // Checks XML text without recursion before the (recursive) parser sees it: every tag,
    // comment, CDATA section and processing instruction is terminated, there is no DOCTYPE or
    // ENTITY declaration (JUCE expands entities recursively, and could load external files),
    // closing tags never outnumber opening ones, the document ends balanced and elements nest
    // at most maxDepth deep.
    bool isSafeXmlText(const juce::String& text, int maxDepth)
    {
        auto p = text.getCharPointer();
        int depth = 0;
        bool sawElement = false;

        auto skipPast = [&p](const char* terminator)
        {
            const int index = p.indexOf(juce::CharPointer_ASCII(terminator));
            if (index < 0)
                return false;

            p += index + static_cast<int>(std::strlen(terminator));
            return true;
        };

        while (!p.isEmpty())
        {
            if (p.getAndAdvance() != '<')
                continue;

            if (*p == '?')
            {
                if (!skipPast("?>"))
                    return false;
                continue;
            }

            if (*p == '!')
            {
                if (p.compareUpTo(juce::CharPointer_ASCII("!--"), 3) == 0)
                {
                    p += 3;
                    if (!skipPast("-->"))
                        return false;
                    continue;
                }

                if (p.compareUpTo(juce::CharPointer_ASCII("![CDATA["), 8) == 0)
                {
                    p += 8;
                    if (!skipPast("]]>"))
                        return false;
                    continue;
                }

                return false; // <!DOCTYPE, <!ENTITY, ...: never written by Subbertone
            }

            const bool closing = (*p == '/');
            juce::juce_wchar quote = 0, previous = 0;
            bool terminated = false;

            while (!p.isEmpty())
            {
                const auto c = p.getAndAdvance();

                if (quote != 0)
                {
                    if (c == quote)
                    {
                        quote = 0;
                        previous = c;
                    }
                    continue;
                }

                if (c == '"' || c == '\'')
                {
                    quote = c;
                    continue;
                }

                if (c == '<')
                    return false;

                if (c == '>')
                {
                    terminated = true;
                    break;
                }

                previous = c;
            }

            if (!terminated)
                return false;

            if (closing)
            {
                if (--depth < 0)
                    return false;
            }
            else
            {
                sawElement = true;

                if (previous != '/' && ++depth > maxDepth)
                    return false;
            }
        }

        return sawElement && depth == 0;
    }

    // Reads and parses a preset file defensively: size cap, structure check, no file access
    // beyond the file itself. outerElementOnly parses just <SubbertonePreset ...> (for listing).
    std::unique_ptr<juce::XmlElement> readPresetXml(const juce::File& file, bool outerElementOnly)
    {
        if (!file.existsAsFile())
            return {};

        const auto size = file.getSize();
        if (size <= 0 || size > c_maxPresetFileBytes)
            return {};

        juce::MemoryBlock data;
        if (!file.loadFileAsData(data) || data.getSize() == 0 || static_cast<juce::int64>(data.getSize()) > c_maxPresetFileBytes)
            return {};

        // Handles UTF-8 (with or without a byte-order mark) and UTF-16 text from editors.
        const auto text = juce::String::createStringFromData(data.getData(), static_cast<int>(data.getSize()));

        if (!isSafeXmlText(text, c_maxElementDepth))
            return {};

        // Parsed from the string, so the parser has no file to resolve external references against.
        auto xml = juce::XmlDocument(text).getDocumentElement(outerElementOnly);

        if (xml == nullptr || !xml->hasTagName(c_presetTag.toString()))
            return {};

        return xml;
    }

    juce::String presetNameIn(const juce::XmlElement& xml, const juce::File& file)
    {
        const auto name = xml.getStringAttribute(c_nameAttr.toString()).trim();
        return name.isNotEmpty() ? name : file.getFileNameWithoutExtension();
    }

    // The file name (without extension) a preset name is saved under.
    juce::String fileStemFor(const juce::String& name)
    {
        // Strips "#@,;:<>*^|?\/ and limits the length to 128 characters.
        const auto legal = juce::File::createLegalFileName(name.trim());

        juce::String stem;
        for (auto p = legal.getCharPointer(); !p.isEmpty();)
        {
            const auto c = p.getAndAdvance();
            if (c >= 32 && c != 127)
                stem += juce::String::charToString(c);
        }

        // A leading dot would make a hidden file; Windows drops trailing dots and spaces.
        stem = stem.trim().trimCharactersAtStart(".").trim().trimCharactersAtEnd(". ");

        // Stay well inside the 255-byte file name limit of APFS / HFS+ (UTF-8) with the extension.
        while (stem.getNumBytesAsUTF8() > 200)
            stem = stem.dropLastCharacters(1).trimEnd();

        // Windows device names cannot be used as file names, even with an extension.
        static const juce::StringArray reserved { "CON", "PRN", "AUX", "NUL",
                                                  "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
                                                  "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9" };
        if (reserved.contains(stem, true))
            stem << "_";

        return stem;
    }

    juce::String kindToString(PresetManager::StateInfo::Kind kind)
    {
        switch (kind)
        {
            case PresetManager::StateInfo::Kind::factory: return "factory";
            case PresetManager::StateInfo::Kind::user:    return "user";
            case PresetManager::StateInfo::Kind::unsaved: break;
        }

        return "unsaved";
    }
}

//==============================================================================
PresetManager::PresetManager(juce::AudioProcessorValueTreeState& parameters, HostNotifier notifyHost)
    : m_parameters(parameters), m_notifyHost(std::move(notifyHost))
{
    const auto& params = m_parameters.processor.getParameters();
    m_numParameters = params.size();
    m_lastValues = std::make_unique<std::atomic<float>[]>(static_cast<size_t>(m_numParameters));

    for (int i = 0; i < m_numParameters; ++i)
        m_lastValues[static_cast<size_t>(i)].store(params[i]->getValue());

    // A plain parameter listener: called on whatever thread changes the value (the audio
    // thread for host automation), so the callback only touches atomics.
    for (auto* p : params)
        p->addListener(this);

    const juce::ScopedLock op(m_operationLock);
    refreshLocked();
}

PresetManager::~PresetManager()
{
    for (auto* p : m_parameters.processor.getParameters())
        p->removeListener(this);
}

juce::File PresetManager::getUserPresetDirectory()
{
#if JUCE_MAC
    return juce::File::getSpecialLocation(juce::File::userHomeDirectory)
               .getChildFile("Library/Audio/Presets/SammyJs/Subbertone");
#elif JUCE_WINDOWS
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
               .getChildFile("SammyJs/Subbertone/Presets");
#else
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
               .getChildFile("SammyJs/Subbertone/Presets");
#endif
}

int PresetManager::getNumFactory()
{
    return static_cast<int>(factoryPresets().size());
}

juce::String PresetManager::getFactoryPresetName(int factoryIndex)
{
    return juce::isPositiveAndBelow(factoryIndex, getNumFactory()) ? juce::String(factoryPresets()[static_cast<size_t>(factoryIndex)].name)
                                                                   : juce::String();
}

int PresetManager::findFactory(const juce::String& name)
{
    const auto& presets = factoryPresets();

    for (size_t i = 0; i < presets.size(); ++i)
        if (name == presets[i].name)
            return static_cast<int>(i);

    return -1;
}

bool PresetManager::isValidPresetName(const juce::String& name)
{
    return fileStemFor(name).isNotEmpty();
}

bool PresetManager::isInUserFolder(const juce::File& file)
{
    return file != juce::File()
        && file.hasFileExtension(c_fileExtension)
        && file.getParentDirectory() == getUserPresetDirectory();
}

//==============================================================================
std::vector<PresetManager::UserEntry> PresetManager::scanUserFolder()
{
    std::vector<UserEntry> users;

    const auto dir = getUserPresetDirectory();
    if (!dir.isDirectory())
        return users;

    // Hidden files are skipped: saves write through a hidden temporary file.
    auto files = dir.findChildFiles(juce::File::findFiles | juce::File::ignoreHiddenFiles, false,
                                    juce::String("*") + c_fileExtension);
    std::sort(files.begin(), files.end(), [](const juce::File& a, const juce::File& b)
              { return a.getFileName().compareNatural(b.getFileName()) < 0; });

    for (const auto& file : files)
    {
        // Saved presets never start with a dot; such files are temporaries of a save in progress
        // (not "hidden" on Windows).
        if (file.getFileName().startsWithChar('.'))
            continue;

        // Only the outer element is parsed here; files that are not readable presets are not
        // listed (there would be nothing to load).
        if (auto xml = readPresetXml(file, true))
            users.push_back({ file, presetNameIn(*xml, file) });
    }

    return users;
}

juce::Array<PresetManager::Preset> PresetManager::buildList(const std::vector<UserEntry>& users)
{
    juce::Array<Preset> list;

    for (const auto& f : factoryPresets())
        list.add({ f.name, true, {}, f.category, f.name });

    for (const auto& user : users)
    {
        int sameName = 0;
        for (const auto& other : users)
            sameName += (other.name == user.name) ? 1 : 0;

        const auto menuName = sameName > 1 ? user.name + " (" + user.file.getFileName() + ")" : user.name;
        list.add({ user.name, false, user.file, {}, menuName });
    }

    return list;
}

void PresetManager::refresh()
{
    const juce::ScopedLock op(m_operationLock);
    refreshLocked();
}

void PresetManager::refreshLocked()
{
    auto list = buildList(scanUserFolder()); // file I/O without the data lock

    const juce::ScopedLock sl(m_lock);
    m_presets = std::move(list);

    if (m_kind == Kind::factory)
    {
        m_listIndex = m_factoryIndex;
    }
    else if (m_kind == Kind::user)
    {
        // Keep pointing at the same FILE (names need not be unique).
        m_listIndex = -1;
        for (int i = getNumFactory(); i < m_presets.size(); ++i)
        {
            if (m_presets.getReference(i).file == m_file)
            {
                m_listIndex = i;
                m_name = m_presets.getReference(i).name;
                break;
            }
        }

        if (m_listIndex < 0 && !m_file.existsAsFile())
        {
            // Removed outside the plugin: the sound stays, but nothing is saved behind it any more.
            m_kind = Kind::unsaved;
            m_file = juce::File();
            m_modified = true;
            publishCurrentLocked();
        }
    }
    else
    {
        m_listIndex = -1;
    }
}

juce::Array<PresetManager::Preset> PresetManager::getPresets() const
{
    const juce::ScopedLock sl(m_lock);
    return m_presets;
}

PresetManager::Snapshot PresetManager::getSnapshot() const
{
    const juce::ScopedLock sl(m_lock);

    Snapshot s;
    s.presets = m_presets;
    s.currentIndex = m_listIndex;
    s.currentName = m_name;
    s.currentFile = m_kind == Kind::user ? m_file : juce::File();
    s.currentIsFactory = m_kind == Kind::factory;
    s.currentIsUser = m_kind == Kind::user;
    s.modified = m_modified.load();
    return s;
}

int PresetManager::getCurrentIndex() const
{
    const juce::ScopedLock sl(m_lock);
    return m_listIndex;
}

juce::String PresetManager::getCurrentName() const
{
    const juce::ScopedLock sl(m_lock);
    return m_name;
}

juce::File PresetManager::getCurrentFile() const
{
    const juce::ScopedLock sl(m_lock);
    return m_kind == Kind::user ? m_file : juce::File();
}

bool PresetManager::isCurrentUserPreset() const
{
    const juce::ScopedLock sl(m_lock);
    return m_kind == Kind::user;
}

int PresetManager::getCurrentFactoryIndex() const
{
    return m_currentFactory.load(); // lock-free: hosts ask from any thread
}

void PresetManager::publishCurrentLocked()
{
    m_currentFactory = m_kind == Kind::factory ? m_factoryIndex : -1;
}

int PresetManager::currentProgram() const
{
    const int factoryIndex = m_currentFactory.load();
    return factoryIndex >= 0 ? factoryIndex : getNumFactory();
}

void PresetManager::notify(bool programChanged, bool stateChanged) const
{
    if (m_notifyHost != nullptr && (programChanged || stateChanged))
        m_notifyHost(juce::AudioProcessorListener::ChangeDetails().withProgramChanged(programChanged)
                                                                  .withNonParameterStateChanged(stateChanged));
}

//==============================================================================
bool PresetManager::loadPreset(int index)
{
    const juce::ScopedLock op(m_operationLock);

    Preset preset;
    {
        const juce::ScopedLock sl(m_lock);
        if (!juce::isPositiveAndBelow(index, m_presets.size()))
            return false;
        preset = m_presets[index];
    }

    return preset.isFactory ? applyFactory(index, Origin::ui) : applyUserFile(preset.file, Origin::ui);
}

bool PresetManager::loadUserPreset(const juce::File& file)
{
    const juce::ScopedLock op(m_operationLock);
    return isInUserFolder(file) && applyUserFile(file, Origin::ui);
}

bool PresetManager::loadFactoryPresetForHost(int factoryIndex)
{
    const juce::ScopedLock op(m_operationLock);
    return applyFactory(factoryIndex, Origin::host);
}

bool PresetManager::applyFactory(int factoryIndex, Origin origin)
{
    if (!juce::isPositiveAndBelow(factoryIndex, getNumFactory()))
        return false;

    const auto& preset = factoryPresets()[static_cast<size_t>(factoryIndex)];
    const int programBefore = currentProgram();

    {
        const ScopedApply applying(*this);
        applyValues(Values(preset.values.begin(), preset.values.end()));

        const juce::ScopedLock sl(m_lock);
        m_kind = Kind::factory;
        m_factoryIndex = factoryIndex;
        m_file = juce::File();
        m_name = preset.name;
        m_listIndex = factoryIndex;
        m_modified = false;
        publishCurrentLocked();
    }

    // A host that set the program itself already knows; only report changes made in the plugin.
    if (origin == Origin::ui)
        notify(programBefore != factoryIndex, true);

    return true;
}

bool PresetManager::applyUserFile(const juce::File& file, Origin origin)
{
    Values values;
    juce::String name;

    if (!readPresetFile(file, values, name))
        return false;

    const int programBefore = currentProgram();

    {
        const ScopedApply applying(*this);
        applyValues(values);

        const juce::ScopedLock sl(m_lock);
        m_kind = Kind::user;
        m_factoryIndex = -1;
        m_file = file;
        m_name = name;
        m_listIndex = -1;
        for (int i = getNumFactory(); i < m_presets.size(); ++i)
            if (m_presets.getReference(i).file == file)
                m_listIndex = i;
        m_modified = false;
        publishCurrentLocked();
    }

    if (origin == Origin::ui)
        notify(programBefore != getNumFactory(), true);

    return true;
}

bool PresetManager::loadNext()
{
    const juce::ScopedLock op(m_operationLock);
    return step(1);
}

bool PresetManager::loadPrevious()
{
    const juce::ScopedLock op(m_operationLock);
    return step(-1);
}

bool PresetManager::step(int direction)
{
    int size = 0, origin = 0;
    {
        const juce::ScopedLock sl(m_lock);
        size = m_presets.size();
        origin = m_listIndex >= 0 ? m_listIndex : (direction > 0 ? -1 : size);
    }

    // Skip presets that cannot be read (a file deleted since the last scan, a broken file),
    // but stop once the whole list has been tried.
    for (int n = 1; n <= size; ++n)
    {
        const int index = ((origin + direction * n) % size + size) % size;

        Preset preset;
        {
            const juce::ScopedLock sl(m_lock);
            if (!juce::isPositiveAndBelow(index, m_presets.size()))
                continue;
            preset = m_presets[index];
        }

        if (preset.isFactory ? applyFactory(index, Origin::ui) : applyUserFile(preset.file, Origin::ui))
            return true;
    }

    return false;
}

//==============================================================================
PresetManager::SaveTarget PresetManager::resolveSaveTarget(const juce::String& name, const std::vector<UserEntry>& users) const
{
    const auto stem = fileStemFor(name);
    if (stem.isEmpty())
        return {};

    Kind kind;
    juce::File currentFile;
    juce::String currentName;
    {
        const juce::ScopedLock sl(m_lock);
        kind = m_kind;
        currentFile = m_file;
        currentName = m_name;
    }

    // 1. Saving the current user preset under its own name writes back to the file it came from.
    if (kind == Kind::user && name == currentName)
        for (const auto& user : users)
            if (user.file == currentFile)
                return { user.file, true, user.name };

    // 2. A user preset with exactly this name.
    for (const auto& user : users)
        if (user.name == name)
            return { user.file, true, user.name };

    // 3. The file this name maps to. File names compare case-insensitively (APFS, NTFS), and
    //    different names can map to the same file once illegal characters are dropped.
    const auto fileName = stem + c_fileExtension;
    for (const auto& user : users)
        if (user.file.getFileName().equalsIgnoreCase(fileName))
            return { user.file, true, user.name };

    const auto target = getUserPresetDirectory().getChildFile(fileName);

    // Something already there that is not a listed preset (unreadable, or written since the scan).
    if (target.exists())
    {
        if (auto xml = readPresetXml(target, true))
            return { target, true, presetNameIn(*xml, target) };

        return { target, true, target.getFileNameWithoutExtension() };
    }

    return { target, false, {} };
}

bool PresetManager::userPresetExists(const juce::String& name) const
{
    return resolveSaveTarget(name.trim(), scanUserFolder()).replacesExisting;
}

juce::String PresetManager::getNameOfPresetSavedAs(const juce::String& name) const
{
    const auto target = resolveSaveTarget(name.trim(), scanUserFolder());
    return target.replacesExisting ? target.replacedName : juce::String();
}

bool PresetManager::saveUserPreset(const juce::String& rawName)
{
    const auto name = rawName.trim();
    if (name.isEmpty())
        return false;

    const juce::ScopedLock op(m_operationLock);

    const auto target = resolveSaveTarget(name, scanUserFolder());
    if (target.file == juce::File())
        return false; // nothing legal is left of the name

    const auto dir = getUserPresetDirectory();
    if (!dir.createDirectory() || !dir.isDirectory() || target.file.isDirectory())
        return false;

    const int programBefore = currentProgram();

    // Cleared before the values are captured: an edit made while saving marks the preset again.
    const bool wasModified = m_modified.exchange(false);

    juce::XmlElement xml(c_presetTag.toString());
    xml.setAttribute(c_nameAttr.toString(), name);
    xml.setAttribute(c_versionAttr.toString(), JucePlugin_VersionString);

    for (const auto& [id, value] : captureValues())
    {
        auto* child = xml.createNewChildElement(c_paramTag.toString());
        child->setAttribute(c_idAttr.toString(), id);
        child->setAttribute(c_valueAttr.toString(), static_cast<double>(value));
    }

    // Written to a hidden temporary file first, then moved over the target in one step.
    bool written = false;
    {
        juce::TemporaryFile temp(target.file, juce::TemporaryFile::useHiddenFile);
        {
            juce::FileOutputStream out(temp.getFile());
            if (out.openedOk())
            {
                xml.writeTo(out);
                out.flush();
                written = !out.getStatus().failed();
            }
        }
        written = written && temp.overwriteTargetFileWithTemporary();
    }

    if (!written)
    {
        if (wasModified)
            m_modified = true;
        return false;
    }

    {
        const juce::ScopedLock sl(m_lock);
        m_kind = Kind::user;
        m_factoryIndex = -1;
        m_file = target.file;
        m_name = name;
        publishCurrentLocked();
    }

    refreshLocked();

    notify(programBefore != getNumFactory(), true);
    return true;
}

bool PresetManager::deleteUserPreset(const juce::File& file)
{
    const juce::ScopedLock op(m_operationLock);

    if (!isInUserFolder(file) || !file.existsAsFile() || !file.deleteFile())
        return false;

    {
        const juce::ScopedLock sl(m_lock);
        if (m_kind == Kind::user && m_file == file)
        {
            // Keep the sound; it just no longer has a saved preset behind it.
            m_kind = Kind::unsaved;
            m_file = juce::File();
            m_modified = true;
            publishCurrentLocked();
        }
    }

    refreshLocked();
    notify(false, true);
    return true;
}

bool PresetManager::deleteUserPreset(int index)
{
    juce::File file;
    {
        const juce::ScopedLock sl(m_lock);
        if (!juce::isPositiveAndBelow(index, m_presets.size()) || m_presets.getReference(index).isFactory)
            return false;
        file = m_presets.getReference(index).file;
    }

    return deleteUserPreset(file);
}

//==============================================================================
PresetManager::Values PresetManager::captureValues() const
{
    Values values;

    for (auto* p : m_parameters.processor.getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(p))
            values.emplace_back(ranged->getParameterID(), ranged->convertFrom0to1(ranged->getValue()));

    return values;
}

void PresetManager::applyValues(const Values& values)
{
    // Gestures let hosts record the change as one undoable step / automation event. They
    // belong on the message thread; elsewhere (a host's program change) the values are set
    // without them, which is allowed on any thread.
    const bool withGestures = juce::MessageManager::existsAndIsCurrentThread();

    for (auto* p : m_parameters.processor.getParameters())
    {
        auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(p);
        if (ranged == nullptr)
            continue;

        float normalised = ranged->getDefaultValue();
        for (const auto& [id, value] : values)
        {
            if (id == ranged->getParameterID())
            {
                normalised = ranged->convertTo0to1(value);
                break;
            }
        }

        if (juce::approximatelyEqual(ranged->getValue(), normalised))
            continue;

        if (withGestures)
            ranged->beginChangeGesture();

        ranged->setValueNotifyingHost(normalised);

        if (withGestures)
            ranged->endChangeGesture();
    }
}

bool PresetManager::readPresetFile(const juce::File& file, Values& values, juce::String& name)
{
    const auto xml = readPresetXml(file, false);
    if (xml == nullptr)
        return false;

    name = presetNameIn(*xml, file);

    for (auto* child : xml->getChildWithTagNameIterator(c_paramTag.toString()))
    {
        const auto id = child->getStringAttribute(c_idAttr.toString());
        if (id.isEmpty() || !child->hasAttribute(c_valueAttr.toString()))
            continue;

        const auto value = child->getDoubleAttribute(c_valueAttr.toString());
        if (!std::isfinite(value))
            continue; // a hand-edited "nan" or "inf": that parameter takes its default

        values.emplace_back(id, static_cast<float>(value));
    }

    return true;
}

void PresetManager::parameterValueChanged(int parameterIndex, float newValue)
{
    // Any thread, including the audio thread: atomics only.
    if (!juce::isPositiveAndBelow(parameterIndex, m_numParameters))
        return;

    const float previous = m_lastValues[static_cast<size_t>(parameterIndex)].exchange(newValue);

    if (m_applyDepth.load() == 0 && !juce::approximatelyEqual(previous, newValue))
        m_modified = true;
}

//==============================================================================
void PresetManager::writeToState(juce::ValueTree& state) const
{
    const juce::ScopedLock sl(m_lock);

    const auto kind = m_kind == Kind::factory ? StateInfo::Kind::factory
                    : m_kind == Kind::user    ? StateInfo::Kind::user
                                              : StateInfo::Kind::unsaved;

    state.setProperty(c_statePresetName, m_name, nullptr);
    state.setProperty(c_statePresetFactory, m_kind == Kind::factory, nullptr);
    state.setProperty(c_statePresetKind, kindToString(kind), nullptr);
    state.setProperty(c_statePresetModified, m_modified.load(), nullptr);

    // Only the file name: the folder is always the user preset folder (and the session should
    // not carry the user's home folder path).
    if (m_kind == Kind::user)
        state.setProperty(c_statePresetFile, m_file.getFileName(), nullptr);
    else
        state.removeProperty(c_statePresetFile, nullptr);
}

PresetManager::StateInfo PresetManager::readStateInfo(const juce::XmlElement& state)
{
    StateInfo info;

    if (!state.hasAttribute(c_statePresetName.toString()))
        return info; // state from before presets existed

    info.hasPresetInfo = true;
    info.name = state.getStringAttribute(c_statePresetName.toString());
    info.modified = state.getBoolAttribute(c_statePresetModified.toString(), false);
    // Just a file name inside the user folder; anything path-like is cut down to its last part.
    info.fileName = state.getStringAttribute(c_statePresetFile.toString())
                         .fromLastOccurrenceOf("/", false, false)
                         .fromLastOccurrenceOf("\\", false, false);

    const auto kind = state.getStringAttribute(c_statePresetKind.toString());
    if (kind == "factory")
        info.kind = StateInfo::Kind::factory;
    else if (kind == "user")
        info.kind = StateInfo::Kind::user;
    else if (kind == "unsaved")
        info.kind = StateInfo::Kind::unsaved;
    else
        info.kind = state.getBoolAttribute(c_statePresetFactory.toString(), true) ? StateInfo::Kind::factory : StateInfo::Kind::user;

    return info;
}

void PresetManager::restoreState(const StateInfo& info, const std::function<void()>& replaceParameters)
{
    const juce::ScopedLock op(m_operationLock);

    {
        // The parameters come from the state; that is not an edit.
        const ScopedApply applying(*this);
        if (replaceParameters != nullptr)
            replaceParameters();
    }

    Kind kind = Kind::unsaved;
    int factoryIndex = -1;
    juce::File file;
    juce::String name = info.name;
    bool modified = info.modified;

    if (!info.hasPresetInfo)
    {
        // State from before presets existed: the sound is whatever was saved, not a preset.
        name = "Init";
        modified = true;
    }
    else if (info.kind == StateInfo::Kind::factory)
    {
        factoryIndex = findFactory(info.name);
        if (factoryIndex >= 0)
            kind = Kind::factory;
        else
            modified = true; // a factory preset this version doesn't have
    }
    else if (info.kind == StateInfo::Kind::user)
    {
        auto find = [this, &info](bool& nameMatches) -> juce::File
        {
            const juce::ScopedLock sl(m_lock);
            const auto dir = getUserPresetDirectory();

            // By file first (names need not be unique), then by name.
            if (info.fileName.isNotEmpty())
                for (int i = getNumFactory(); i < m_presets.size(); ++i)
                    if (m_presets.getReference(i).file == dir.getChildFile(info.fileName))
                    {
                        nameMatches = m_presets.getReference(i).name == info.name;
                        return m_presets.getReference(i).file;
                    }

            for (int i = getNumFactory(); i < m_presets.size(); ++i)
                if (m_presets.getReference(i).name == info.name)
                {
                    nameMatches = true;
                    return m_presets.getReference(i).file;
                }

            return {};
        };

        bool nameMatches = false;
        file = find(nameMatches);

        if (file == juce::File())
        {
            refreshLocked(); // the folder may have changed since the last scan
            file = find(nameMatches);
        }

        if (file != juce::File())
        {
            kind = Kind::user;
            // The file was edited since the session was saved: its values may no longer match.
            modified = modified || !nameMatches;
        }
        else
        {
            // The saved user preset isn't on this machine: the sound is still recalled exactly
            // (the parameters come from the state); mark it edited.
            modified = true;
        }
    }

    const juce::ScopedLock sl(m_lock);
    m_kind = kind;
    m_factoryIndex = kind == Kind::factory ? factoryIndex : -1;
    m_file = kind == Kind::user ? file : juce::File();
    m_name = name;
    m_listIndex = -1;

    if (kind == Kind::factory)
    {
        m_listIndex = factoryIndex;
    }
    else if (kind == Kind::user)
    {
        for (int i = getNumFactory(); i < m_presets.size(); ++i)
            if (m_presets.getReference(i).file == file)
            {
                m_listIndex = i;
                m_name = m_presets.getReference(i).name;
            }
    }

    m_modified = modified;
    publishCurrentLocked();
}
