#pragma once

#include <JuceHeader.h>
#include "Presets.h"

#include <vector>

/**
 * Your own presets: one small file each, in a folder beside the settings.
 *
 * A file holds the same map of plain values a factory preset is made of (every control a preset owns), and,
 * when it was saved with them, the modulators' own controls and the wires. The file's name is the preset's
 * name, so renaming or copying a file in the Finder / Explorer works, and sharing a preset is sending the file.
 *
 *   <SwooshbucklerPreset name="Dry Jets" version="1">
 *     <Value id="energy" value="0.36"/> ...
 *     <Mods> ... </Mods>                    (only when saved with the modulators and wires)
 *   </SwooshbucklerPreset>
 *
 * Message thread (file I/O).
 */
namespace UserPresets
{
    inline constexpr const char* extension = ".swooshpreset";
    inline constexpr int maxNameLength = 24; // what fits in the preset box in the header

    struct Loaded
    {
        Presets::Preset preset;
        bool hasMods = false;
        juce::ValueTree mods;   // the wires, when hasMods
    };

    /** A name that can be a file name on every system (never empty: "Preset" if nothing is left). */
    juce::String legalName(const juce::String& name);
    juce::File fileFor(const juce::File& folder, const juce::String& name);
    /** The preset files in the folder, by name. */
    std::vector<juce::File> list(const juce::File& folder);
    /** stem, or "stem 2", "stem 3" ... : the first that is neither a file in the folder nor a factory preset. */
    juce::String uniqueName(const juce::File& folder, const juce::String& stem);
    bool isFactoryName(const juce::String& name);

    /** Writes the file (beside its final name first, then swapped in). mods: null = without the wires. */
    bool write(const juce::File& file, const Presets::Preset& preset, const juce::ValueTree* mods, juce::String& error);
    bool read(const juce::File& file, Loaded& loaded, juce::String& error);
    /** Gives a preset file a new name. False (with the reason) when the name is taken or the file can't be moved. */
    bool rename(const juce::File& file, const juce::String& newName, juce::File& renamed, juce::String& error);
}
