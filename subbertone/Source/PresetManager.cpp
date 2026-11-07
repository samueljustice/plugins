#include "PresetManager.h"

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
    const juce::Identifier c_statePresetModified { "presetModified" };

    struct FactoryPreset
    {
        const char* name;
        const char* category;                 // menu group ("" = top level)
        std::map<juce::String, float> values; // real values; anything missing = default
    };

    using Values = std::map<juce::String, float>;

    // Sets every harmonic bar explicitly (so presets never inherit stray bars):
    // levels lists {k, percent}; H1 (the sub) defaults to 100% unless given.
    Values withHarmonics(Values values, std::initializer_list<std::pair<int, float>> levels)
    {
        for (int k = 1; k <= 16; ++k)
            values["harmonic" + juce::String(k)] = (k == 1) ? 100.0f : 0.0f;

        for (const auto& [k, percent] : levels)
            values["harmonic" + juce::String(k)] = percent;

        return values;
    }

    // Saturation indices: 0 Tape, 1 Valve, 2 Console, 3 Transformer, 4 Rectifier, 5 Wavefolder
    // Filter modes:       0 Lowpass, 1 Bandpass, 2 Notch
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
}

//==============================================================================
PresetManager::PresetManager(juce::AudioProcessorValueTreeState& parameters)
    : m_parameters(parameters)
{
    for (auto* p : m_parameters.processor.getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(p))
            m_parameters.addParameterListener(ranged->getParameterID(), this);

    refresh();
    m_currentIndex = 0;
    m_currentName = m_presets.isEmpty() ? juce::String("Init") : m_presets.getReference(0).name;
}

PresetManager::~PresetManager()
{
    for (auto* p : m_parameters.processor.getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(p))
            m_parameters.removeParameterListener(ranged->getParameterID(), this);
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

int PresetManager::getNumFactoryPresets() const
{
    return static_cast<int>(factoryPresets().size());
}

void PresetManager::refresh()
{
    const auto previousName = m_currentName;
    const bool previousFactory = !isCurrentUserPreset();

    m_presets.clear();

    for (const auto& f : factoryPresets())
        m_presets.add({ f.name, true, {}, f.category });

    auto files = getUserPresetDirectory().findChildFiles(juce::File::findFiles, false, juce::String("*") + c_fileExtension);
    std::sort(files.begin(), files.end(), [](const juce::File& a, const juce::File& b)
              { return a.getFileNameWithoutExtension().compareNatural(b.getFileNameWithoutExtension()) < 0; });

    for (const auto& file : files)
    {
        auto name = file.getFileNameWithoutExtension();

        if (auto xml = juce::parseXML(file); xml != nullptr && xml->hasTagName(c_presetTag.toString()))
            name = xml->getStringAttribute(c_nameAttr.toString(), name);

        m_presets.add({ name, false, file });
    }

    // Keep pointing at the same preset after the list changes.
    const int idx = findIndex(previousName, previousFactory);
    if (idx >= 0)
        m_currentIndex = idx;
}

bool PresetManager::isCurrentUserPreset() const
{
    return juce::isPositiveAndBelow(m_currentIndex, m_presets.size())
        && !m_presets.getReference(m_currentIndex).isFactory
        && m_presets.getReference(m_currentIndex).name == m_currentName;
}

int PresetManager::findIndex(const juce::String& name, bool factory) const
{
    for (int i = 0; i < m_presets.size(); ++i)
        if (m_presets.getReference(i).isFactory == factory && m_presets.getReference(i).name == name)
            return i;

    return -1;
}

bool PresetManager::userPresetExists(const juce::String& name) const
{
    return findIndex(name, false) >= 0;
}

//==============================================================================
bool PresetManager::loadPreset(int index)
{
    if (!juce::isPositiveAndBelow(index, m_presets.size()))
        return false;

    const auto& preset = m_presets.getReference(index);
    Values values;

    if (preset.isFactory)
    {
        values = factoryPresets()[static_cast<size_t>(index)].values;
    }
    else if (!readPresetFile(preset.file, values))
    {
        return false;
    }

    applyValues(values);
    m_currentIndex = index;
    m_currentName = preset.name;
    m_modified = false;
    return true;
}

void PresetManager::loadNext()
{
    if (!m_presets.isEmpty())
        loadPreset((m_currentIndex + 1) % m_presets.size());
}

void PresetManager::loadPrevious()
{
    if (!m_presets.isEmpty())
        loadPreset((m_currentIndex - 1 + m_presets.size()) % m_presets.size());
}

bool PresetManager::saveUserPreset(const juce::String& rawName)
{
    const auto name = rawName.trim();
    if (name.isEmpty())
        return false;

    const auto dir = getUserPresetDirectory();
    if (!dir.createDirectory())
        return false;

    juce::XmlElement xml(c_presetTag.toString());
    xml.setAttribute(c_nameAttr.toString(), name);
    xml.setAttribute(c_versionAttr.toString(), JucePlugin_VersionString);

    for (const auto& [id, value] : captureValues())
    {
        auto* child = xml.createNewChildElement(c_paramTag.toString());
        child->setAttribute(c_idAttr.toString(), id);
        child->setAttribute(c_valueAttr.toString(), static_cast<double>(value));
    }

    const auto file = dir.getChildFile(juce::File::createLegalFileName(name) + c_fileExtension);
    if (!xml.writeTo(file))
        return false;

    m_currentName = name;
    m_modified = false;
    refresh();

    const int idx = findIndex(name, false);
    if (idx >= 0)
        m_currentIndex = idx;

    return true;
}

bool PresetManager::deleteUserPreset(int index)
{
    if (!juce::isPositiveAndBelow(index, m_presets.size()) || m_presets.getReference(index).isFactory)
        return false;

    const bool wasCurrent = (index == m_currentIndex);

    if (!m_presets.getReference(index).file.deleteFile())
        return false;

    if (wasCurrent)
    {
        // Keep the sound; it just no longer has a saved preset behind it.
        m_modified = true;
        m_currentIndex = 0;
    }

    refresh();
    return true;
}

//==============================================================================
PresetManager::Values PresetManager::captureValues() const
{
    Values values;

    for (auto* p : m_parameters.processor.getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(p))
            values[ranged->getParameterID()] = ranged->convertFrom0to1(ranged->getValue());

    return values;
}

void PresetManager::applyValues(const Values& values)
{
    m_applying = true;

    for (auto* p : m_parameters.processor.getParameters())
    {
        auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(p);
        if (ranged == nullptr)
            continue;

        const auto it = values.find(ranged->getParameterID());
        const float normalised = it != values.end() ? ranged->convertTo0to1(it->second)
                                                    : ranged->getDefaultValue();

        // Gestures let hosts record the change as one undoable step / automation event.
        ranged->beginChangeGesture();
        ranged->setValueNotifyingHost(normalised);
        ranged->endChangeGesture();
    }

    m_applying = false;
}

bool PresetManager::readPresetFile(const juce::File& file, Values& values)
{
    const auto xml = juce::parseXML(file);
    if (xml == nullptr || !xml->hasTagName(c_presetTag.toString()))
        return false;

    for (auto* child : xml->getChildWithTagNameIterator(c_paramTag.toString()))
        values[child->getStringAttribute(c_idAttr.toString())] = static_cast<float>(child->getDoubleAttribute(c_valueAttr.toString()));

    return true;
}

void PresetManager::parameterChanged(const juce::String&, float)
{
    if (!m_applying.load())
        m_modified = true;
}

//==============================================================================
void PresetManager::writeToState(juce::ValueTree& state) const
{
    state.setProperty(c_statePresetName, m_currentName, nullptr);
    state.setProperty(c_statePresetFactory, !isCurrentUserPreset(), nullptr);
    state.setProperty(c_statePresetModified, m_modified.load(), nullptr);
}

void PresetManager::restoreFromState(const juce::ValueTree& state)
{
    refresh();

    if (!state.hasProperty(c_statePresetName))
    {
        // State from before presets existed: the sound is whatever was saved.
        m_currentIndex = 0;
        m_currentName = "Init";
        m_modified = true;
        return;
    }

    m_currentName = state.getProperty(c_statePresetName).toString();
    const bool factory = state.getProperty(c_statePresetFactory, true);
    const int idx = findIndex(m_currentName, factory);
    m_currentIndex = idx >= 0 ? idx : 0;

    // If the saved user preset no longer exists on this machine, the sound is
    // still recalled exactly (parameters come from the state); mark it edited.
    m_modified = static_cast<bool>(state.getProperty(c_statePresetModified, false)) || idx < 0;
}
