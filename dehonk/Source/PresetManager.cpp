#include "PresetManager.h"

#include "Parameters.h"
#include "Presets.h"

#include <cmath>

namespace
{
    const juce::String presetTag     { "DeHonkPreset" };
    const juce::String paramTag      { "Param" };
    const juce::String idAttr        { "id" };
    const juce::String valueAttr     { "value" };
    const juce::String nameAttr      { "name" };
    const juce::String versionAttr   { "pluginVersion" };

    const juce::Identifier statePresetName     { "presetName" };
    const juce::Identifier statePresetKind     { "presetKind" };   // "factory" | "user" | "unsaved"
    const juce::Identifier statePresetFile     { "presetFile" };   // user presets: file name only
    const juce::Identifier statePresetModified { "presetModified" };

    // Presets are a few hundred bytes. Anything much bigger is not a preset and is never parsed.
    constexpr juce::int64 maxPresetFileBytes = 64 * 1024;

    std::unique_ptr<juce::XmlElement> readPresetXml (const juce::File& f)
    {
        if (! f.existsAsFile() || f.getSize() > maxPresetFileBytes)
            return {};

        auto xml = juce::XmlDocument (f.loadFileAsString()).getDocumentElement();
        if (xml == nullptr || ! xml->hasTagName (presetTag))
            return {};

        return xml;
    }

    juce::String presetNameIn (const juce::XmlElement& xml, const juce::File& f)
    {
        const auto n = xml.getStringAttribute (nameAttr).trim();
        return n.isNotEmpty() ? n : f.getFileNameWithoutExtension();
    }

    juce::String fileStemFor (const juce::String& presetName)
    {
        auto stem = juce::File::createLegalFileName (presetName.trim());
        stem = stem.trim().trimCharactersAtStart (".").trimCharactersAtEnd (". ");

        while (stem.getNumBytesAsUTF8() > 200)
            stem = stem.dropLastCharacters (1).trimEnd();

        static const juce::StringArray reserved { "CON", "PRN", "AUX", "NUL",
                                                  "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
                                                  "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9" };
        if (reserved.contains (stem, true))
            stem << "_";

        return stem;
    }
}

//==============================================================================
PresetManager::PresetManager (juce::AudioProcessorValueTreeState& apvts, HostNotifier notifyHost)
    : parameters (apvts), hostNotifier (std::move (notifyHost))
{
    const auto& params = parameters.processor.getParameters();
    numParameters = params.size();
    lastValues = std::make_unique<std::atomic<float>[]> (static_cast<size_t> (numParameters));
    inPreset = std::make_unique<bool[]> (static_cast<size_t> (numParameters));

    for (int i = 0; i < numParameters; ++i)
    {
        lastValues[static_cast<size_t> (i)].store (params[i]->getValue());

        auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (params[i]);
        inPreset[static_cast<size_t> (i)] = ranged == nullptr || isPresetParameter (*ranged);
    }

    // Called on whatever thread changes a value (the audio thread for automation): atomics only.
    for (auto* p : params)
        p->addListener (this);

    name = getFactoryName (0);

    const juce::ScopedLock op (operationLock);
    refreshLocked();
}

PresetManager::~PresetManager()
{
    for (auto* p : parameters.processor.getParameters())
        p->removeListener (this);
}

juce::File PresetManager::getUserPresetDirectory()
{
   #if JUCE_MAC
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("Application Support/Sammy J/De-Honk/Presets");
   #else
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("Sammy J/De-Honk/Presets");
   #endif
}

int PresetManager::getNumFactory() noexcept
{
    return FactoryPresets::count;
}

juce::String PresetManager::getFactoryName (int index)
{
    return juce::isPositiveAndBelow (index, FactoryPresets::count) ? juce::String (FactoryPresets::list[index].name)
                                                                    : juce::String();
}

bool PresetManager::isValidPresetName (const juce::String& n)
{
    return fileStemFor (n).isNotEmpty();
}

bool PresetManager::isPresetParameter (const juce::RangedAudioParameter& p)
{
    // Not Bypass, and not the Link percentage that no control sets any more (it is still a
    // parameter so that the ones after it keep their places in a host's list).
    return p.getParameterID() != ParamIDs::bypass && p.getParameterID() != ParamIDs::legacyLink;
}

PresetManager::Values PresetManager::factoryValues (int index)
{
    const auto& v = FactoryPresets::list[index];
    Values values { { ParamIDs::threshold, v.threshold },
                    { ParamIDs::depth, v.depth },
                    { ParamIDs::sharpness, v.sharpness },
                    { ParamIDs::speed, v.speed },
                    { ParamIDs::release, v.release },
                    { ParamIDs::maxCut, v.maxCut },
                    { ParamIDs::impactGuard, v.impactGuard },
                    { ParamIDs::linkMode, static_cast<float> (v.linkMode) },
                    { ParamIDs::dialogue, v.dialogue ? 1.0f : 0.0f },
                    { ParamIDs::detection, static_cast<float> (v.detection) },
                    { ParamIDs::lookahead, static_cast<float> (v.lookahead) },
                    { ParamIDs::mix, v.mix },
                    { ParamIDs::trim, v.trim } };

    static_assert (sizeof (FactoryPresets::Values::modules) / sizeof (bool) == dehonk::kNumModules);
    for (int m = 0; m < dehonk::kNumModules; ++m)
        values.emplace_back (ParamIDs::moduleOn[m], v.modules[m] ? 1.0f : 0.0f);
    values.emplace_back (ParamIDs::snapOn, v.click ? 1.0f : 0.0f);
    for (int e = 0; e < dehonk::kNumEdges; ++e)
        values.emplace_back (ParamIDs::edges[e], dehonk::kDefaultEdgesHz[e]);

    return values;
}

//==============================================================================
void PresetManager::refresh()
{
    const juce::ScopedLock op (operationLock);
    refreshLocked();
}

void PresetManager::refreshLocked()
{
    juce::Array<Preset> list;

    for (int i = 0; i < FactoryPresets::count; ++i)
        list.add ({ getFactoryName (i), true, {} });

    const auto dir = getUserPresetDirectory();
    if (dir.isDirectory())
    {
        auto files = dir.findChildFiles (juce::File::findFiles | juce::File::ignoreHiddenFiles, false,
                                         juce::String ("*") + fileExtension);
        std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b)
                   { return a.getFileName().compareNatural (b.getFileName()) < 0; });

        for (const auto& f : files)
            if (! f.getFileName().startsWithChar ('.'))
                if (auto xml = readPresetXml (f))
                    list.add ({ presetNameIn (*xml, f), false, f });
    }

    const juce::ScopedLock sl (dataLock);
    presets = std::move (list);

    if (kind == Kind::factory)
    {
        listIndex = factoryIndex;
    }
    else if (kind == Kind::user)
    {
        listIndex = -1;
        for (int i = getNumFactory(); i < presets.size(); ++i)
            if (presets.getReference (i).file == file)
            {
                listIndex = i;
                name = presets.getReference (i).name;
            }

        if (listIndex < 0)
        {
            // Removed outside the plug-in: the sound stays, with nothing saved behind it.
            kind = Kind::unsaved;
            file = juce::File();
            modified = true;
        }
    }
    else
    {
        listIndex = -1;
    }
}

juce::Array<PresetManager::Preset> PresetManager::getPresets() const
{
    const juce::ScopedLock sl (dataLock);
    return presets;
}

int PresetManager::getCurrentIndex() const
{
    const juce::ScopedLock sl (dataLock);
    return listIndex;
}

juce::String PresetManager::getCurrentName() const
{
    const juce::ScopedLock sl (dataLock);
    return name;
}

juce::File PresetManager::getCurrentFile() const
{
    const juce::ScopedLock sl (dataLock);
    return kind == Kind::user ? file : juce::File();
}

bool PresetManager::isCurrentUserPreset() const
{
    const juce::ScopedLock sl (dataLock);
    return kind == Kind::user;
}

void PresetManager::notifyHost (bool programChanged) const
{
    if (hostNotifier != nullptr)
        hostNotifier (juce::AudioProcessorListener::ChangeDetails().withProgramChanged (programChanged)
                                                                   .withNonParameterStateChanged (true));
}

//==============================================================================
bool PresetManager::loadPreset (int index)
{
    const juce::ScopedLock op (operationLock);

    Preset preset;
    {
        const juce::ScopedLock sl (dataLock);
        if (! juce::isPositiveAndBelow (index, presets.size()))
            return false;
        preset = presets[index];
    }

    return preset.isFactory ? applyFactory (index, false) : applyUserFile (preset.file);
}

bool PresetManager::loadFactoryPresetForHost (int index)
{
    const juce::ScopedLock op (operationLock);
    return applyFactory (index, true);
}

bool PresetManager::loadPresetFile (const juce::File& f)
{
    const juce::ScopedLock op (operationLock);
    return applyUserFile (f);
}

bool PresetManager::loadNext()
{
    const juce::ScopedLock op (operationLock);
    return step (1);
}

bool PresetManager::loadPrevious()
{
    const juce::ScopedLock op (operationLock);
    return step (-1);
}

bool PresetManager::step (int direction)
{
    int size = 0, origin = 0;
    {
        const juce::ScopedLock sl (dataLock);
        size = presets.size();
        origin = listIndex >= 0 ? listIndex : (direction > 0 ? -1 : size);
    }

    // Presets that cannot be read are skipped, until the whole list has been tried.
    for (int n = 1; n <= size; ++n)
    {
        const int index = ((origin + direction * n) % size + size) % size;

        Preset preset;
        {
            const juce::ScopedLock sl (dataLock);
            if (! juce::isPositiveAndBelow (index, presets.size()))
                continue;
            preset = presets[index];
        }

        if (preset.isFactory ? applyFactory (index, false) : applyUserFile (preset.file))
            return true;
    }

    return false;
}

bool PresetManager::applyFactory (int index, bool fromHost)
{
    if (! juce::isPositiveAndBelow (index, getNumFactory()))
        return false;

    const bool programChanged = currentFactory.load() != index;

    {
        const ScopedApply applying (*this);
        applyValues (factoryValues (index));

        const juce::ScopedLock sl (dataLock);
        kind = Kind::factory;
        factoryIndex = index;
        file = juce::File();
        name = getFactoryName (index);
        listIndex = index;
        modified = false;
        currentFactory = index;
    }

    // A host that set the program itself already knows.
    if (! fromHost)
        notifyHost (programChanged);

    return true;
}

bool PresetManager::applyUserFile (const juce::File& f)
{
    Values values;
    juce::String presetName;

    if (! readPresetFile (f, values, presetName))
        return false;

    const bool programChanged = currentFactory.load() >= 0;

    {
        const ScopedApply applying (*this);
        applyValues (values);

        const juce::ScopedLock sl (dataLock);
        kind = Kind::user;
        factoryIndex = -1;
        file = f;
        name = presetName;
        listIndex = -1;
        for (int i = getNumFactory(); i < presets.size(); ++i)
            if (presets.getReference (i).file == f)
                listIndex = i;
        modified = false;
        currentFactory = -1;
    }

    notifyHost (programChanged);
    return true;
}

//==============================================================================
juce::File PresetManager::targetFileFor (const juce::String& presetName) const
{
    const auto stem = fileStemFor (presetName);
    if (stem.isEmpty())
        return {};

    {
        // Saving the current user preset under its own name writes back to its file.
        const juce::ScopedLock sl (dataLock);
        if (kind == Kind::user && presetName == name && file != juce::File())
            return file;

        for (int i = getNumFactory(); i < presets.size(); ++i)
            if (presets.getReference (i).name == presetName)
                return presets.getReference (i).file;
    }

    return getUserPresetDirectory().getChildFile (stem + fileExtension);
}

bool PresetManager::userPresetExists (const juce::String& presetName) const
{
    const auto target = targetFileFor (presetName.trim());
    return target != juce::File() && target.exists();
}

bool PresetManager::saveUserPreset (const juce::String& rawName)
{
    const auto presetName = rawName.trim();
    const juce::ScopedLock op (operationLock);

    const auto target = targetFileFor (presetName);
    if (target == juce::File())
        return false;

    const auto dir = getUserPresetDirectory();
    if (! dir.createDirectory() || ! dir.isDirectory() || target.isDirectory())
        return false;

    const bool programChanged = currentFactory.load() >= 0;
    const bool wasModified = modified.exchange (false);  // an edit made while saving marks it again

    juce::XmlElement xml (presetTag);
    xml.setAttribute (nameAttr, presetName);
    xml.setAttribute (versionAttr, JucePlugin_VersionString);

    for (const auto& [id, value] : captureValues())
    {
        auto* child = xml.createNewChildElement (paramTag);
        child->setAttribute (idAttr, id);
        child->setAttribute (valueAttr, static_cast<double> (value));
    }

    // Written to a hidden temporary file first, then moved over the target in one step.
    bool written = false;
    {
        juce::TemporaryFile temp (target, juce::TemporaryFile::useHiddenFile);
        {
            juce::FileOutputStream out (temp.getFile());
            if (out.openedOk())
            {
                xml.writeTo (out);
                out.flush();
                written = ! out.getStatus().failed();
            }
        }
        written = written && temp.overwriteTargetFileWithTemporary();
    }

    if (! written)
    {
        if (wasModified)
            modified = true;
        return false;
    }

    {
        const juce::ScopedLock sl (dataLock);
        kind = Kind::user;
        factoryIndex = -1;
        file = target;
        name = presetName;
        currentFactory = -1;
    }

    refreshLocked();
    notifyHost (programChanged);
    return true;
}

bool PresetManager::deleteUserPreset (const juce::File& f)
{
    const juce::ScopedLock op (operationLock);

    if (f == juce::File() || ! f.hasFileExtension (fileExtension)
        || f.getParentDirectory() != getUserPresetDirectory() || ! f.existsAsFile() || ! f.deleteFile())
        return false;

    {
        const juce::ScopedLock sl (dataLock);
        if (kind == Kind::user && file == f)
        {
            // The sound stays; it just has no saved preset behind it any more.
            kind = Kind::unsaved;
            file = juce::File();
            modified = true;
        }
    }

    refreshLocked();
    notifyHost (false);
    return true;
}

//==============================================================================
PresetManager::Values PresetManager::captureValues() const
{
    Values values;

    for (auto* p : parameters.processor.getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p); ranged != nullptr && isPresetParameter (*ranged))
            values.emplace_back (ranged->getParameterID(), ranged->convertFrom0to1 (ranged->getValue()));

    return values;
}

void PresetManager::applyValues (const Values& values)
{
    // Gestures make the change one undoable step for hosts. They belong on the message thread;
    // elsewhere (a host's program change) values are set without them.
    const bool withGestures = juce::MessageManager::existsAndIsCurrentThread();

    for (auto* p : parameters.processor.getParameters())
    {
        auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p);
        if (ranged == nullptr || ! isPresetParameter (*ranged))
            continue;

        float normalised = ranged->getDefaultValue();
        for (const auto& [id, value] : values)
            if (id == ranged->getParameterID())
            {
                normalised = ranged->convertTo0to1 (value);
                break;
            }

        if (juce::approximatelyEqual (ranged->getValue(), normalised))
            continue;

        if (withGestures)
            ranged->beginChangeGesture();

        ranged->setValueNotifyingHost (normalised);

        if (withGestures)
            ranged->endChangeGesture();
    }
}

bool PresetManager::readPresetFile (const juce::File& f, Values& values, juce::String& presetName)
{
    const auto xml = readPresetXml (f);
    if (xml == nullptr)
        return false;

    presetName = presetNameIn (*xml, f);

    for (auto* child : xml->getChildWithTagNameIterator (paramTag))
    {
        const auto id = child->getStringAttribute (idAttr);
        if (id.isEmpty() || ! child->hasAttribute (valueAttr))
            continue;

        const auto value = child->getDoubleAttribute (valueAttr);
        if (std::isfinite (value))
            values.emplace_back (id, static_cast<float> (value));
    }

    // A preset from the first test build has Link as a percentage: Linked from 50 % up, Unlinked below.
    const auto has = [&values] (const char* wanted)
    {
        return std::any_of (values.begin(), values.end(), [wanted] (const auto& v) { return v.first == wanted; });
    };
    if (! has (ParamIDs::linkMode))
        for (const auto& [id, value] : values)
            if (id == ParamIDs::legacyLink)
            {
                values.emplace_back (ParamIDs::linkMode, static_cast<float> (ParamRanges::legacyLinkMode (value)));
                break;
            }

    return true;
}

void PresetManager::parameterValueChanged (int parameterIndex, float newValue)
{
    // Any thread, including the audio thread: atomics only.
    if (! juce::isPositiveAndBelow (parameterIndex, numParameters))
        return;

    const float previous = lastValues[static_cast<size_t> (parameterIndex)].exchange (newValue);

    if (applyDepth.load() != 0 || juce::approximatelyEqual (previous, newValue))
        return;

    // Bypass is not part of a preset, so switching it does not mark the preset edited.
    if (inPreset[static_cast<size_t> (parameterIndex)])
        modified = true;
}

//==============================================================================
void PresetManager::writeToState (juce::ValueTree& state) const
{
    const juce::ScopedLock sl (dataLock);

    state.setProperty (statePresetName, name, nullptr);
    state.setProperty (statePresetKind, kind == Kind::factory ? "factory" : kind == Kind::user ? "user" : "unsaved", nullptr);
    state.setProperty (statePresetModified, modified.load(), nullptr);

    // Only the file name: the folder is always the user preset folder.
    if (kind == Kind::user)
        state.setProperty (statePresetFile, file.getFileName(), nullptr);
    else
        state.removeProperty (statePresetFile, nullptr);
}

void PresetManager::restoreState (const juce::XmlElement& state, const std::function<void()>& replaceParameters)
{
    const juce::ScopedLock op (operationLock);

    {
        const ScopedApply applying (*this);  // values from a session are not an edit
        if (replaceParameters != nullptr)
            replaceParameters();
    }

    refreshLocked();

    auto savedName = state.getStringAttribute (statePresetName.toString());
    const auto savedKind = state.getStringAttribute (statePresetKind.toString());
    // Factory presets that have been renamed: a session saved on one under its old name still
    // comes back on it.
    if (savedKind == "factory")
    {
        static const std::pair<const char*, const char*> renamed[]
        {
            { "After RBass", "After a bass enhancer" },          // renamed in 0.0.2
            { "Heavy", "Master bus, heavy" },                     // renamed in 0.0.3
            { "Narrow resonances only", "Narrow focus only" },    // renamed in 0.0.3
        };
        for (const auto& [oldName, newName] : renamed)
            if (savedName == oldName)
                savedName = newName;
    }
    const auto savedFile = state.getStringAttribute (statePresetFile.toString())
                               .fromLastOccurrenceOf ("/", false, false)
                               .fromLastOccurrenceOf ("\\", false, false);
    bool savedModified = state.getBoolAttribute (statePresetModified.toString(), true);

    const juce::ScopedLock sl (dataLock);

    kind = Kind::unsaved;
    factoryIndex = -1;
    file = juce::File();
    name = savedName.isNotEmpty() ? savedName : getFactoryName (0);
    listIndex = -1;

    if (savedKind == "factory")
    {
        for (int i = 0; i < getNumFactory(); ++i)
            if (getFactoryName (i) == savedName)
            {
                kind = Kind::factory;
                factoryIndex = i;
                listIndex = i;
            }
    }
    else if (savedKind == "user")
    {
        const auto wanted = getUserPresetDirectory().getChildFile (savedFile);
        for (int i = getNumFactory(); i < presets.size(); ++i)
            if ((savedFile.isNotEmpty() && presets.getReference (i).file == wanted)
                || (listIndex < 0 && presets.getReference (i).name == savedName))
            {
                kind = Kind::user;
                file = presets.getReference (i).file;
                listIndex = i;
            }
    }

    if (kind == Kind::unsaved)
        savedModified = true;  // the preset is not on this machine; the sound is still recalled exactly

    modified = savedModified;
    currentFactory = kind == Kind::factory ? factoryIndex : -1;
}
