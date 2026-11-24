#pragma once

#include <JuceHeader.h>

#include "../core/Engine.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace tt::ui
{

// The waveform with the grid, the pins and the playhead on it.
//
// From top to bottom: a ruler with bar numbers, the tempo of each stretch between pins and the
// pin heads, the music lane, the drum lane (when a drum buffer is set) and an overview of the
// whole file with the part in view marked on it.
// The view never changes the map itself: every edit is reported through a callback and the
// owner answers with setMap().
class WaveformView : public juce::Component,
                     private juce::Timer
{
public:
    // What a pin lands on when it is dragged or added.
    enum class Snap { off, hits, grid };

    WaveformView();
    ~WaveformView() override;

    // The buffers stay owned by the caller and must outlive the view's use of them; nullptr clears.
    // Only the peaks are read, once, during this call. Call on the message thread.
    void setAudio (const juce::AudioBuffer<float>* mix, const juce::AudioBuffer<float>* drums, double sampleRate);
    void setMap (const tt::MapResult& map, int beatsPerBar);     // copies what it needs
    void setPlayhead (double seconds);                           // repaints two thin strips only
    void setBusy (bool analysing);                               // dims the lanes and shows "Listening…"
    void showRange (double startSeconds, double endSeconds);     // zoom to this range
    void showAll();
    double getViewStart() const                     { return viewStart; }   // the range in view, in seconds
    double getViewEnd() const                       { return viewEnd; }
    double getPlayhead() const                      { return playhead; }
    int getSelectedPin() const;                                  // index into map.pins or -1
    void setSelectedPin (int pinIndex);                           // -1 selects none; calls nothing back

    // Moving about. The zooms glide over about 120 ms unless macOS is set to reduce motion.
    void zoomBy (double factor);                // above 1 zooms in. Centred on the selected pin if it is in view, else the playhead if it is, else the middle
    void showAllAnimated();
    void zoomToPinsAround();                    // from the pin before to the pin after the selected pin, or the playhead
    void toggleZoom();                          // about two bars around the selected pin or the playhead; again goes back
    void reveal (double seconds);               // scrolls, only if needed, so this time is in view
    void stepWaveHeight (int steps);            // each step draws the waveform 1.41 times taller or shorter
    double getWaveHeight() const                    { return waveHeight; }
    void setWaveHeight (double factor);

    void setSnap (Snap newSnap)                     { snap = newSnap; }
    Snap getSnap() const                            { return snap; }

    // Moves the selected pin by this much, kept between its neighbours, and reports it through
    // onPinNudged. Returns false when there is no selected pin to move.
    bool nudgeSelectedPin (double seconds);

    std::function<void (double seconds)> onSeek;                          // click on empty space, or a drag in the ruler
    std::function<void (int pinIndex, double newTime)> onPinMoved;        // drag finished; the time is snapped as the Snap setting says
    std::function<void (int pinIndex, double newTime)> onPinNudged;       // moved from the keyboard
    std::function<void (double time)> onPinAdded;                         // double-click (time snapped as the Snap setting says)
    std::function<void (int pinIndex)> onPinDeleted;                      // Backspace/Delete on the selected pin, or the menu
    std::function<void (double time)> onRestartHere;                      // menu "The beat restarts here"
    std::function<void (double time)> onFirstHitHere;                     // menu "This is the first hit"
    std::function<void (int pinIndex, double newBeat)> onPinBeatChanged;  // menu "This hit is bar…"

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMagnify (const juce::MouseEvent&, float scaleFactor) override;
    bool keyPressed (const juce::KeyPress&) override;

    // For the picture taken by --snapshot: shows a pin as it looks part-way through a drag to
    // this time, snapped as the Snap setting says.
    void showDragForSnapshot (int pinIndex, double time);

private:
    // Minimum, maximum and RMS level of the audio in buckets of several sizes, so that drawing
    // reads a handful of values for each pixel column whatever the zoom.
    struct PeakCache
    {
        struct Level
        {
            int samplesPerBucket = 0;
            std::vector<std::int16_t> lo, hi;
            std::vector<std::uint16_t> rms;
        };

        void build (const juce::AudioBuffer<float>&);
        void clear()                                { levels.clear(); numSamples = 0; }
        bool empty() const                          { return levels.empty(); }
        void read (double firstSample, double lastSample, double samplesPerPixel, float& lo, float& hi,
                   float* rms = nullptr) const;

        std::vector<Level> levels;
        juce::int64 numSamples = 0;
    };

    struct Layout
    {
        juce::Rectangle<int> ruler, music, drums, overview, lanes;
    };

    enum class SnapKind { none, hit, grid };

    struct GridLine
    {
        float x = 0.0f;
        bool bar = false;
        long long barIndex = 0;     // zero-based
    };

    Layout getLayout() const;
    bool hasAudio() const                           { return duration > 0.0 && ! mixPeaks.empty(); }
    double span() const                             { return viewEnd - viewStart; }
    double pixelsPerSecond() const;
    float timeToX (double seconds) const;
    double xToTime (float x) const;
    void setView (double start, double end);        // straight away; ends a glide
    void applyView (double start, double end);
    void glideTo (double start, double end);
    void timerCallback() override;
    juce::Range<double> targetView() const;         // where the view is, or where it is gliding to
    void zoomAbout (float x, double factor);
    void scrollBy (double seconds);
    double anchorTime (bool mustBeInView, bool& found) const;   // the selected pin, else the playhead

    int pinAt (juce::Point<float>, const Layout&) const;
    double pinTime (int index) const;               // the dragged position while a drag is under way
    juce::Range<double> pinLimits (int index) const;            // a pin stays between its neighbours
    bool snapToOnset (double& time, float withinPixels, double lowest, double highest) const;
    bool snapToGrid (double& time, double lowest, double highest) const;
    SnapKind snapTime (double& time, bool free, double lowest, double highest) const;
    double pixelsPerBeatAt (double seconds) const;
    juce::String pinLabel (double beat) const;
    double bpmAfterPin (int index) const;

    void collectGrid (std::vector<GridLine>&, int& labelStep, double& pixelsPerBar) const;
    void drawWave (juce::Graphics&, const PeakCache&, juce::Rectangle<int> lane, juce::Colour, juce::Rectangle<int> clip) const;
    void drawWaveShape (juce::Graphics&, const PeakCache&, juce::Rectangle<int> lane, juce::Colour, juce::Rectangle<int> clip) const;
    void drawOverview (juce::Graphics&, juce::Rectangle<int> strip) const;
    void drawDragReadout (juce::Graphics&, const Layout&) const;
    void moveViewFromOverview (float x);
    void drawCentredMessage (juce::Graphics&, const juce::String&, juce::Rectangle<int>) const;
    void repaintPlayheadAt (double seconds);

    void showMenu (double time, int pinIndex);
    void showBarEditor (int pinIndex);
    void hideBarEditor (bool commit);

    PeakCache mixPeaks, drumPeaks;
    double sampleRate = 0.0;
    double duration = 0.0;

    tt::MapResult map;
    tt::BeatGrid grid;
    int beatsPerBar = 4;

    double viewStart = 0.0, viewEnd = 0.0;
    double playhead = 0.0;
    bool busy = false;

    // A glide from one view to another.
    bool gliding = false;
    double glideFromStart = 0.0, glideFromEnd = 0.0, glideToStart = 0.0, glideToEnd = 0.0;
    double glideBegan = 0.0;            // milliseconds

    // The view to go back to on the second press of the zoom toggle.
    bool zoomToggled = false;
    double beforeToggleStart = 0.0, beforeToggleEnd = 0.0;

    double waveHeight = 1.0;            // the waveform is drawn this many times taller
    double waveHeightShownUntil = 0.0;  // milliseconds: the factor is written on the lane until then

    Snap snap = Snap::hits;

    int selectedPin = -1;
    int hoverPin = -1;

    // A press on a pin becomes a drag once the pointer has moved a few pixels.
    int pressedPin = -1;
    bool dragging = false;
    double dragTime = 0.0;
    SnapKind dragSnap = SnapKind::none; // what the dragged pin is sitting on
    double keepSelectionNear = -1.0;   // after an edit, select the pin the next map has at this time
    double nudgedTo = -1.0;            // where the selected pin was last nudged to, until the map comes back

    // A drag in the ruler moves the playhead; a drag in the overview moves the view.
    enum class Gesture { none, scrub, overview };
    Gesture gesture = Gesture::none;
    double overviewGrabOffset = 0.0;    // seconds from the start of the view to where the box was taken hold of

    juce::TextEditor barEditor;
    int barEditorPin = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformView)
};

} // namespace tt::ui
