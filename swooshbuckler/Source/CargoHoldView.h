#pragma once

#include <JuceHeader.h>
#include "IconButton.h"
#include "LookAndFeel.h" // ModTarget
#include "PluginProcessor.h"
#include "Theme.h"

/**
 * The Cargo Hold: your own samples as whoosh sources.
 *
 *  - Starts empty. Drop audio anywhere on it (or use LOAD): each sound gets its
 *    own track, which appears as it arrives. Several files make several tracks.
 *  - Every track is scanned on arrival and named after what it is ("Tonal
 *    Content", "Energy Bed", "Impact", "Air / Swish", "Crackle", "Rumble",
 *    "Mixed Content"), which also decides its bus. Click the type to change it,
 *    double-click the name to rename. Click the bus tag to send the track to a
 *    bus by hand. The star makes a track the Master (one at a time); a
 *    right-click on a lane opens its menu (Master Track, Match Pitch To Master,
 *    the bus).
 *  - The waveform shows the sound regions that will be played and dims the
 *    silence that will be skipped. Drag the clip's edge handles to trim; drag
 *    inside to select; double-click a region to grab it.
 *  - Cut / copy / paste / duplicate / trim / delete / reverse / normalise /
 *    fades. Every edit joins the rig's one undo history (Cmd+Z and Shift+Cmd+Z,
 *    handled by the window). Keys: Cmd+X C V D, Delete, T (trim to selection),
 *    Space fires a whoosh.
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

    /** An undo or redo has just run (from the header buttons or the keys): shows the track it brought back. */
    void historyStepped();

    std::function<void()> onSwell; // space bar while the tracks have focus
    /** The panel's own Undo (true = redo): the window runs it as it runs the header's, so a glide still moving is
        finished first and the message says what was undone. Without it the rig's history is stepped directly. */
    std::function<void(bool)> onHistory;
    /** The lanes moved (scrolled, added, removed or resized): modulation plugs on them need placing again. */
    std::function<void()> onLanesMoved;
    /** Each track's Semitones box, as a modulation target (parameter ID -> control). */
    std::map<juce::String, ModTarget*> getModTargets();
    std::function<void(const juce::String&)> onStatus;

    static bool isAudioFile(const juce::String& path);

    /** The track under a screen position (for the morph ropes), or -1. */
    int trackAtScreenPosition(juce::Point<int> screenPos) const;
    /** Where a morph rope ties on to a track's lane, in screen coordinates: the right end of its waveform, kept
        inside the list when the lane is scrolled out of it. False when the track has no lane. */
    bool laneAnchorScreen(int track, juce::Point<int>& screenPos) const;

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
    /** The lanes scroll when there are more than fit; scrolling tells the owner (onLanesMoved). */
    struct LaneViewport : juce::Viewport
    {
        std::function<void()> onMoved;
        void visibleAreaChanged(const juce::Rectangle<int>&) override { if (onMoved) onMoved(); }
    };
    LaneViewport m_viewport;
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
