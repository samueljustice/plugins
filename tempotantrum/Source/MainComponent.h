#pragma once

#include <JuceHeader.h>

#include "Document.h"
#include "Player.h"
#include "ui/Controls.h"
#include "ui/Theme.h"
#include "ui/WaveformView.h"

#include <functional>
#include <memory>
#include <vector>

namespace tt
{

// The window's contents: a header with the file and the playhead, a row of fields for what the
// user knows about the music, the waveform with the grid and pins, and a footer for playing,
// checking, saving and exporting.
class MainComponent : public juce::Component,
                      public juce::FileDragAndDropTarget,
                      private juce::ChangeListener,
                      private juce::Timer
{
public:
    // Without audio no output device is opened (for --snapshot).
    explicit MainComponent (bool withAudio = true);
    ~MainComponent() override;

    Document& getDocument()                             { return document; }

    // Asks first whether to save the map when the open one has changes that are not saved.
    void openMusic (const juce::File& file);
    void openDrumStem (const juce::File& file);

    // Calls `then` straight away when no changes would be lost. Otherwise asks whether to save
    // the map: `then` is called after Save (if the save worked) or Don't save, and not after Cancel.
    void askToSaveThen (std::function<void()> then);

    // Pulls everything shown from the document.
    void refresh();

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray&, int, int) override;
    void fileDragExit (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    class CheckBadge;
    class DropChoice;

    Document document;
    Player player;
    bool audioEnabled = true;

    juce::TooltipWindow tooltips { this, 500 };

    // Fields
    ui::PillButton openButton { juce::String::fromUTF8 ("Open\xe2\x80\xa6") };
    ui::SegmentedSwitch listenSwitch;
    ui::PillButton addStemButton { juce::String::fromUTF8 ("Add drum stem\xe2\x80\xa6") };
    ui::NumberBox barBox { "First hit is bar" };
    ui::NumberBox beatBox { "Beat" };
    ui::NumberBox tempoBox { "Tempo" };
    ui::PillButton tapButton { "Tap" };

    ui::WaveformView waveform;

    // Footer
    ui::PillButton playButton { "Play with click" };
    juce::Slider clickLevel { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
    juce::Slider musicLevel { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
    std::unique_ptr<CheckBadge> checkBadge;
    ui::PillButton exportButton { juce::String::fromUTF8 ("Export click\xe2\x80\xa6") };
    ui::PillButton saveButton { "Save map" };
    ui::PillButton writeButton { "Write to Pro Tools", true };

    std::unique_ptr<DropChoice> dropChoice;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::ScopedMessageBox saveQuestion;

    // Areas worked out in resized() for paint()
    juce::Rectangle<int> headerArea, fieldsArea, musicNameArea, tempoNoteArea, meterArea,
                         waveArea, statusArea, footerArea, clickLabelArea, musicLabelArea;
    juce::Rectangle<int> musicCaption, listenCaption, meterCaption;

    int shownAudioVersion = -1;
    int mapVersionShown = -1;
    bool needsPlayerMusic = true;
    juce::Range<double> rangeToKeep;    // what was in view when a drum stem replaced the audio
    const std::vector<float>* shownClick = nullptr;
    Hint shownHint;
    bool hintShown = false;
    int checkIndex = -1;
    bool dragOver = false;
    juce::String counterText, tempoText, refinedTempoText;
    std::vector<double> taps;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;

    void chooseMusic();
    void chooseDrumStem();
    void exportClick();
    void saveMap();

    void togglePlay();
    void stopLooping();
    void jumpToCheck (int direction);
    void selectNeighbourPin (int direction);
    void tap();
    void updatePlayButton();
    void updateCounter();
    void layoutFields (juce::Rectangle<int> area);

    juce::String describeFile() const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace tt
