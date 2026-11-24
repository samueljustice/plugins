#pragma once

#include <JuceHeader.h>

#include "MapFile.h"
#include "capture/CaptureInbox.h"
#include "core/Engine.h"

#include <functional>
#include <memory>
#include <vector>

namespace tt
{

namespace detail
{
    struct AnalysisAudio;
    struct OnsetCache;
    class AnalysisWorker;
    struct AnalysisResult;
}

// One piece of music being mapped: the audio (and an optional drum stem), what the user has said
// about it, and the map the engine makes from that. The map is worked out on a background thread;
// results arrive on the message thread, and a newer request replaces an older one. The hits are
// found once per audio and source; edits only re-run the mapping and the click.
//
// Every edit is one undo step. Everything here is used on the message thread only.
class Document : public juce::ChangeBroadcaster,
                 private juce::AsyncUpdater
{
public:
    static constexpr int beatsPerBar = 4;

    Document();
    ~Document() override;

    // Runs the analysis on the calling thread instead of in the background (for --snapshot).
    void setSynchronous (bool shouldRunInline)              { synchronous = shouldRunInline; }

    // Called just before the audio buffers are replaced, so anything that holds a pointer to them
    // (the view, the player) can let go first. `musicToo` is false when only the drum stem changes.
    std::function<void (bool musicToo)> onAudioAboutToChange;

    //==============================================================================
    juce::AudioFormatManager& getFormats()                  { return formats; }

    // Opens the music: clears the drum stem, the user's inputs and the undo history, then opens
    // the map saved beside the file if there is one for the same audio.
    juce::Result loadMusic (const juce::File& file);

    // Opens a capture from Pro Tools as the music, as loadMusic() does, and keeps where it came
    // from: its time 0 is the capture's start sample on the session's timeline.
    juce::Result loadCapture (const capture::Capture& captured);

    // Where the music came from when it was captured from Pro Tools (known is false otherwise).
    const ProToolsOrigin& getProToolsOrigin() const         { return proTools; }

    // The name shown for the music: the capture's name, or the file's.
    juce::String getMusicName() const;

    // Same sample rate as the music and at most 0.5 s longer or shorter. A shorter stem is
    // padded with silence, a longer one is cut to the music's length.
    juce::Result loadDrumStem (const juce::File& file);

    bool hasMusic() const                                   { return mix.getNumSamples() > 0; }
    bool hasDrums() const                                   { return drums.getNumSamples() > 0; }
    const juce::File& getMusicFile() const                  { return musicFile; }
    const juce::File& getDrumFile() const                   { return drumFile; }
    const juce::AudioBuffer<float>& getMix() const          { return mix; }
    const juce::AudioBuffer<float>& getDrums() const        { return drums; }
    double getSampleRate() const                            { return sampleRate; }
    juce::int64 getLengthInSamples() const                  { return mix.getNumSamples(); }
    double getLengthSeconds() const;

    // Changes whenever the audio is replaced.
    int getAudioVersion() const                             { return audioVersion; }

    //==============================================================================
    const MapInputs& getInputs() const                      { return inputs; }

    // Pin indices are indices into getMap().pins. Pins move forward in both time and bar: an edit
    // that would put a pin at or before the bar of the pin before it (or at or after the bar of a
    // hand-placed pin after it) is not made, and the status line says why.
    // With joinPrevious the move is folded into the previous undo step (a run of nudges is one step).
    void movePin (int pinIndex, double newTime, bool joinPrevious = false);
    void addPin (double time);
    void deletePin (int pinIndex);
    void restartHere (double time);
    void firstHitHere (double time);
    void setPinBeat (int pinIndex, double newBeat);

    // The hand-placed pins are renumbered with the first hit, so the map keeps its shape.
    void setFirstHitBar (int bar);
    void setFirstHitBeat (int beat);

    // Zero asks the engine to estimate the tempo. With joinPrevious the change is folded into
    // the previous undo step (a run of taps is one step).
    void setTempo (double bpm, bool joinPrevious = false);
    void setListenToDrums (bool useDrums);

    // How often a pin is kept on beat 1 where a hit is there: every this many bars, or 0 for
    // pins only where the tempo needs them.
    void setPinEveryBars (int bars);

    // Goes up with every edit, undo and redo.
    int getEditSerial() const                               { return editSerial; }

    bool undo();
    bool redo();
    bool canUndo() const                                    { return undoManager.canUndo(); }
    bool canRedo() const                                    { return undoManager.canRedo(); }

    //==============================================================================
    const MapResult& getMap() const                         { return map; }
    const BeatGrid& getGrid() const                         { return grid; }
    bool hasMap() const                                     { return ! grid.empty(); }

    // Changes whenever the map is replaced.
    int getMapVersion() const                               { return mapVersion; }

    // The click for the current map, at the music's sample rate and length.
    std::shared_ptr<const std::vector<float>> getClick() const { return click; }

    // True while the hits are being found (not while only the mapping is re-run).
    bool isListening() const                                { return listening; }

    const juce::String& getStatus() const                   { return status; }
    void setStatus (const juce::String& text);

    //==============================================================================
    juce::File getMapFile() const;

    // Fails, with the reason as the status, while a map is still being worked out.
    juce::Result saveMap();

    // True when the inputs or the drum stem have changed since the map was opened or saved.
    bool hasUnsavedChanges() const                          { return hasMusic() && changedSinceSave; }

    juce::File getDefaultClickFile() const;
    juce::Result exportClick (const juce::File& file);

private:
    class SetInputsAction;

    juce::AudioFormatManager formats;

    juce::File musicFile, drumFile;
    ProToolsOrigin proTools;
    juce::AudioBuffer<float> mix, drums;
    double sampleRate = 0.0;
    int audioVersion = 0;
    std::shared_ptr<const detail::AnalysisAudio> analysisAudio;

    MapInputs inputs;
    juce::UndoManager undoManager;

    MapResult map;
    BeatGrid grid;
    std::shared_ptr<const std::vector<float>> click;
    bool listening = false;
    bool mapPending = false;            // a map has been asked for and has not arrived
    bool changedSinceSave = false;
    juce::int64 deliveredOnsetKey = -1;
    int generation = 0;
    int mapVersion = 0;
    int editSerial = 0;

    juce::String status, statusToKeep;
    bool synchronous = false;

    std::unique_ptr<detail::OnsetCache> syncCache;
    std::unique_ptr<detail::AnalysisWorker> worker;

    juce::Result loadAudio (const juce::File& file, const ProToolsOrigin* origin);
    juce::Result readAudio (const juce::File& file, juce::AudioBuffer<float>& out, double& rate);
    void rebuildAnalysisAudio();
    void clearResults();

    void edit (MapInputs newInputs, const juce::String& name, bool joinPrevious = false);
    void applyInputs (const MapInputs& newInputs, bool listeningToo);
    void setFirstHit (int bar, int beat, const juce::String& undoName);
    void requestMap();
    void applyResult (detail::AnalysisResult&& result);
    void handleAsyncUpdate() override;

    double nearestBeatAt (double time) const;
    static int findUserPin (const std::vector<Pin>& pins, double time, double tolerance);
    static bool placeUserPin (std::vector<Pin>& pins, Pin pin, double replaceWithin, juce::String& whyNot);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Document)
};

} // namespace tt
