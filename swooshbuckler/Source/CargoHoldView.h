#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "Theme.h"

/**
 * The Cargo Hold: your own samples as whoosh sources.
 *
 *  - Starts empty. Drop audio anywhere on it (or use LOAD): each sound gets its
 *    own track, which appears as it arrives. Several files make several tracks.
 *  - Every track is scanned on arrival and named after what it is ("Tonal
 *    content", "Energy bed", "Impact", "Air / swish", "Crackle", "Rumble"),
 *    which also decides how it rides the whoosh. Click the type to change it,
 *    double-click the name to rename.
 *  - The waveform shows the sound regions that will be played and dims the
 *    silence that will be skipped. Drag the clip's edge handles to trim; drag
 *    inside to select; double-click a region to grab it.
 *  - Cut / copy / paste / duplicate / trim / delete / reverse / normalise /
 *    fades, with undo and redo per track. Keys: Cmd+X C V D Z, Shift+Cmd+Z,
 *    Delete, T (trim to selection), Space fires a whoosh.
 */
class CargoHoldView : public juce::Component,
                      public juce::FileDragAndDropTarget,
                      private juce::ChangeListener,
                      private juce::Timer
{
public:
    CargoHoldView(SwooshbucklerAudioProcessor& processor, const Theme::Fonts& fonts);
    ~CargoHoldView() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;
    void mouseUp(const juce::MouseEvent&) override; // a click on the drop zone browses for sounds

    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void fileDragEnter(const juce::StringArray&, int, int) override;
    void fileDragExit(const juce::StringArray&) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;

    /** Loads files into the first empty tracks (or into `track` and onwards). Returns a short status line. */
    juce::String loadFiles(const juce::StringArray& files, int track = -1);

    std::function<void()> onSwell; // space bar while the tracks have focus
    std::function<void(const juce::String&)> onStatus;

    static bool isAudioFile(const juce::String& path);

    /** The track under a screen position (for the morph ropes), or -1. */
    int trackAtScreenPosition(juce::Point<int> screenPos) const;

private:
    class Lane;
    enum class Action { Load, Cut, Copy, Paste, Duplicate, Trim, Delete, Reverse, Normalise, FadeIn, FadeOut, Undo, Redo };

    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void perform(Action action);
    void setActiveTrack(int track);
    void updateButtons();
    void chooseFiles();
    void status(const juce::String& text);

    SwooshbucklerAudioProcessor& m_processor;
    CargoHold& m_hold;
    const Theme::Fonts& m_fonts;

    std::vector<std::pair<Action, std::unique_ptr<juce::TextButton>>> m_buttons;
    std::array<std::unique_ptr<Lane>, CargoHold::c_numTracks> m_lanes;
    juce::Viewport m_viewport;        // the lanes scroll when there are more than fit
    juce::Component m_laneHolder;
    int m_active = 0;
    HitWindow m_hitWindow;      // hit sync's shared beat window (source seconds), for the aligned lanes
    float m_beatFlash = 0.0f;   // 1 on a sync beat, decays
    int m_lastBeat = 0;
    bool m_dragOver = false;
    std::unique_ptr<juce::FileChooser> m_chooser;
    juce::String m_status;
    juce::Rectangle<int> m_toolbar, m_statusArea, m_dropZone;
};
