#pragma once

#include <JuceHeader.h>
#include "PitchFlattenerLookAndFeel.h"
#include "PluginProcessor.h"
#include "Theme.h"

//==============================================================================
/**
 * The editor scales one fixed-size content component (designed at
 * baseWidth x baseHeight) with an AffineTransform, so every size the host or
 * the corner resizer picks renders the same layout. The aspect ratio is fixed.
 *
 * Everything the editor opens (ComboBox popups, the preset dialogs, tooltips,
 * the About window) is drawn with its PitchFlattenerLookAndFeel, and is owned
 * by it or closed when it is deleted, so nothing outlives it or its LookAndFeel.
 */
class PitchFlattenerAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    explicit PitchFlattenerAudioProcessorEditor (PitchFlattenerAudioProcessor&);
    ~PitchFlattenerAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int baseWidth = 1000;
    static constexpr int baseHeight = 640;

private:
    class Content;

    /** Tooltips for the editor's chrome, drawn inside the editor (it has no native child views that could
        cover them) at the editor's own scale, not that of the scaled content, like Subbertone's. */
    class EditorTooltipWindow final : public juce::TooltipWindow
    {
    public:
        explicit EditorTooltipWindow (juce::Component& editor);
    };

    juce::SharedResourcePointer<Theme::Fonts> m_fonts;
    PitchFlattenerLookAndFeel m_lookAndFeel; // before every component, menu, alert and window that uses it
    std::unique_ptr<Content> m_content;
    EditorTooltipWindow m_tooltipWindow { *this }; // after m_lookAndFeel, so it is deleted first

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchFlattenerAudioProcessorEditor)
};
