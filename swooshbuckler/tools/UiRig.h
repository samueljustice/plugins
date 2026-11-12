#pragma once

// The offline editor, shared by tools/UiShot.cpp and Tests/UiTest.cpp: the processor and its real editor with no
// window on screen and no message loop, played block by block with the interface's timers stepped by hand.
//
// These two programs reach into the editor (its panels, its buttons, its timers), which has no reason to offer any
// of that to the rest of the plug-in: so, for the files that include this header only, its private parts are read
// as public.

#include <JuceHeader.h>
#if defined(__clang__)
 #pragma clang diagnostic push
 #pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#define protected public
#include "PluginProcessor.h"
#include "PluginEditor.h"
#undef private
#undef protected
#if defined(__clang__)
 #pragma clang diagnostic pop
#endif

namespace UiRig
{
    using Editor = SwooshbucklerAudioProcessorEditor;
    using Proc = SwooshbucklerAudioProcessor;

    constexpr double c_fs = 48000.0;
    constexpr int c_block = 512;

    /** Steps every timer the interface runs. */
    inline void tick(juce::Component* c)
    {
        if (auto* x = dynamic_cast<Editor*>(c)) x->timerCallback();
        if (auto* x = dynamic_cast<SeaVisualiser*>(c)) x->timerCallback();
        if (auto* x = dynamic_cast<CargoHoldView*>(c)) x->timerCallback();
        if (auto* x = dynamic_cast<MorphPad*>(c)) x->timerCallback();
        if (auto* x = dynamic_cast<ModPanel*>(c)) x->timerCallback();
        if (auto* x = dynamic_cast<SectionScope*>(c)) x->timerCallback();
        if (auto* x = dynamic_cast<WheelKnob*>(c)) x->timerCallback();
        if (auto* x = dynamic_cast<TakesView*>(c); x != nullptr && x->isVisible()) x->timerCallback();
        if (auto* x = dynamic_cast<VariationsView*>(c); x != nullptr && x->isVisible()) x->timerCallback();
        if (auto* x = dynamic_cast<Editor::SwellButton*>(c)) x->timerCallback();
        if (auto* x = dynamic_cast<Editor::TapeButton*>(c)) x->timerCallback();
        for (auto* child : c->getChildren())
            tick(child);
    }

    /** A processor's own settings file and folders in a scratch folder, so nothing of the user's is read or written,
        and no editor goes on line. Make it before the processor; it must outlive it. */
    struct Sandbox
    {
        explicit Sandbox(const juce::File& folder, const juce::String& name)
        {
            folder.createDirectory();
            juce::PropertiesFile::Options options;
            options.applicationName = name;
            options.filenameSuffix = ".settings";
            const auto file = folder.getChildFile(name + ".settings");
            file.deleteFile();
            settings = std::make_unique<juce::PropertiesFile>(file, options);
            Proc::useSettings(settings.get());
            Editor::checkForUpdates = false;
        }
        ~Sandbox() { Proc::useSettings(nullptr); }
        std::unique_ptr<juce::PropertiesFile> settings;
    };

    struct Rig
    {
        explicit Rig(Proc& p, float windowScale = 1.0f) : proc(p), scale(windowScale) {}

        /** Makes the editor at the window scale (an open one is not told about new sounds: there is no message loop). */
        void open()
        {
            editor.reset(dynamic_cast<Editor*>(proc.createEditor()));
            editor->setVisible(true);
            editor->setSize(juce::roundToInt(static_cast<float>(Editor::c_width) * scale), juce::roundToInt(static_cast<float>(Editor::c_height) * scale));
        }

        /** Plays for a while: about real time for the interface, whose timers are stepped as it goes. */
        void run(double seconds)
        {
            const int blocks = static_cast<int>(seconds * c_fs / c_block);
            for (int i = 0; i < blocks; ++i)
            {
                block.clear();
                midi.clear();
                proc.processBlock(block, midi);
                if ((i & 1) == 0 && editor != nullptr)
                {
                    tick(editor.get());
                    if ((i % 24) == 0)
                        proc.timerCallback(); // four times a second: the takes
                }
            }
            if (editor != nullptr)
                tick(editor.get());
        }

        Proc& proc;
        std::unique_ptr<Editor> editor;
        float scale = 1.0f;
        juce::AudioBuffer<float> block { 2, c_block };
        juce::MidiBuffer midi;
    };

    /** A processor ready to play at 48 kHz. */
    inline std::unique_ptr<juce::AudioProcessor> makeProcessor()
    {
        std::unique_ptr<juce::AudioProcessor> base(createPluginFilter());
        base->setPlayConfigDetails(0, 2, c_fs, c_block);
        base->prepareToPlay(c_fs, c_block);
        return base;
    }
}
