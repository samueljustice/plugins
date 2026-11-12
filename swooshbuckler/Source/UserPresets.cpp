#include "UserPresets.h"

namespace UserPresets
{
    juce::String legalName(const juce::String& name)
    {
        auto legal = juce::File::createLegalFileName(name.trim()).trim();
        while (legal.endsWithChar('.'))
            legal = legal.dropLastCharacters(1).trim();
        return legal.isEmpty() ? juce::String("Preset") : legal.substring(0, maxNameLength).trim();
    }

    juce::File fileFor(const juce::File& folder, const juce::String& name)
    {
        return folder.getChildFile(legalName(name) + extension);
    }

    std::vector<juce::File> list(const juce::File& folder)
    {
        std::vector<juce::File> files;
        if (folder.isDirectory())
            for (const auto& file : folder.findChildFiles(juce::File::findFiles, false, juce::String("*") + extension))
                if (! file.getFileName().startsWithChar('.'))
                    files.push_back(file);
        std::sort(files.begin(), files.end(), [](const juce::File& a, const juce::File& b)
        {
            return a.getFileNameWithoutExtension().compareNatural(b.getFileNameWithoutExtension()) < 0;
        });
        return files;
    }

    bool isFactoryName(const juce::String& name)
    {
        if (name.equalsIgnoreCase("Default") || name.equalsIgnoreCase("Randomised"))
            return true;
        for (const auto& preset : Presets::factory())
            if (preset.name.equalsIgnoreCase(name))
                return true;
        return false;
    }

    juce::String uniqueName(const juce::File& folder, const juce::String& stem)
    {
        const auto base = legalName(stem);
        for (int n = 1; n < 10000; ++n)
        {
            const auto name = n == 1 ? base : base + " " + juce::String(n);
            if (! isFactoryName(name) && ! fileFor(folder, name).existsAsFile())
                return name;
        }
        return base + " " + juce::String(juce::Time::currentTimeMillis());
    }

    bool write(const juce::File& file, const Presets::Preset& preset, const juce::ValueTree* mods, juce::String& error)
    {
        juce::XmlElement xml("SwooshbucklerPreset");
        xml.setAttribute("name", preset.name);
        xml.setAttribute("version", 1);
        for (const auto& [id, value] : preset.values)
        {
            auto* v = xml.createNewChildElement("Value");
            v->setAttribute("id", id);
            v->setAttribute("value", static_cast<double>(value));
        }
        if (mods != nullptr && mods->isValid())
            if (auto wires = mods->createXml())
                xml.addChildElement(wires.release());

        if (! file.getParentDirectory().createDirectory())
        {
            error = "Couldn't make the presets folder";
            return false;
        }
        juce::TemporaryFile temp(file);
        if (! xml.writeTo(temp.getFile()) || ! temp.overwriteTargetFileWithTemporary())
        {
            error = "Couldn't write " + file.getFileName();
            return false;
        }
        return true;
    }

    bool read(const juce::File& file, Loaded& loaded, juce::String& error)
    {
        const auto xml = juce::XmlDocument::parse(file);
        if (xml == nullptr || ! xml->hasTagName("SwooshbucklerPreset"))
        {
            error = file.getFileName() + " isn't a Swooshbuckler preset";
            return false;
        }
        loaded = {};
        loaded.preset.name = file.getFileNameWithoutExtension();
        for (auto* v : xml->getChildWithTagNameIterator("Value"))
        {
            const auto id = v->getStringAttribute("id");
            const double value = v->getDoubleAttribute("value");
            if (id.isNotEmpty() && std::isfinite(value))
                loaded.preset.values[id] = static_cast<float>(value);
        }
        if (auto* wires = xml->getChildByName("Mods"))
        {
            loaded.hasMods = true;
            loaded.mods = juce::ValueTree::fromXml(*wires);
        }
        if (loaded.preset.values.empty())
        {
            error = file.getFileName() + " has no settings in it";
            return false;
        }
        return true;
    }

    bool rename(const juce::File& file, const juce::String& newName, juce::File& renamed, juce::String& error)
    {
        const auto name = legalName(newName);
        const auto target = fileFor(file.getParentDirectory(), name);
        if (isFactoryName(name))
        {
            error = "\"" + name + "\" is a factory preset's name";
            return false;
        }
        if (target == file)
        {
            renamed = file;
            return true;
        }
        // A change of capitals only is the same file on most disks: that is a rename, not a clash.
        if (target.existsAsFile() && ! target.getFileName().equalsIgnoreCase(file.getFileName()))
        {
            error = "There is already a preset called \"" + name + "\"";
            return false;
        }
        if (! file.moveFileTo(target))
        {
            error = "Couldn't rename " + file.getFileName();
            return false;
        }
        // The name inside follows the file's (the file name is what counts; this keeps the two the same).
        if (auto xml = juce::XmlDocument::parse(target); xml != nullptr && xml->hasTagName("SwooshbucklerPreset"))
        {
            xml->setAttribute("name", name);
            juce::TemporaryFile temp(target);
            if (xml->writeTo(temp.getFile()))
                temp.overwriteTargetFileWithTemporary();
        }
        renamed = target;
        return true;
    }
}
