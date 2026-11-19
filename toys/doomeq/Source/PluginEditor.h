#pragma once

#include <JuceHeader.h>
#include <optional>
#include "PluginProcessor.h"
#include "DoomEngine.h"
#include "WadArt.h"

#include "DoomLook.h"
#include "DoomOverlays.h"

namespace sammyj { class UpdateCheck; struct UpdateInfo; }

//==============================================================================
/** The big screen: DOOM, with the analyser and EQ curve laid over it. */
class EqDisplay : public juce::Component
{
public:
    EqDisplay (DoomEQProcessor&, const WadArt&, const Textures&);
    ~EqDisplay() override;

    /** Sets a band's type the way a user would: a bell or shelf becoming a cut starts flat. */
    void setBandType (int band, int newType);

    void setCommandMode (bool inCommand);
    bool isInCommand() const { return commandMode; }
    std::function<void (bool)> onCommandChange;

    void setOverlayOpacity (float);

    void setHelpVisible (bool);
    bool isHelpVisible() const { return showHelp; }

    /** Called from the editor's timer. */
    void tick();

    void paint (juce::Graphics&) override;
    void resized() override;

    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    bool keyPressed (const juce::KeyPress&) override;
    bool keyStateChanged (bool isKeyDown) override;
    void modifierKeysChanged (const juce::ModifierKeys&) override;
    void focusLost (FocusChangeType) override;
    void focusGained (FocusChangeType) override;

    static bool isTakeCommandKey (const juce::KeyPress&);

private:
    struct BandView
    {
        bool on = true;
        eq::Type type = eq::Type::Bell;
        float freq = 1000.0f, gain = 0.0f, q = 1.0f;
        eq::BandCoeffs coeffs;
    };

    bool readBands();
    juce::Point<float> handlePosition (int band) const;
    int hitTestHandle (juce::Point<float>) const;
    float xForFreq (float f) const;
    float freqForX (float x) const;
    float yForDb (float db) const;
    float dbForY (float y) const;
    juce::RangedAudioParameter* param (int band, const char* name) const;
    void setParam (int band, const char* name, float value);
    void setParamGesture (int band, const char* name, float value);
    void endDragGestures();
    void syncModifiers (const juce::ModifierKeys&);
    juce::String currentHint() const;
    void releaseAllKeys();
    bool updateAnalyser();
    juce::Rectangle<float> doomArea() const;
    void rebuildBackdrop();

    void paintGrid (juce::Graphics&, juce::Rectangle<float>);
    void paintSpectrum (juce::Graphics&, juce::Rectangle<float>);
    void paintCurves (juce::Graphics&, juce::Rectangle<float>);
    void paintHandles (juce::Graphics&);
    void paintHelp (juce::Graphics&, juce::Rectangle<float>);
    void updateLayers (float scale);

    DoomEQProcessor& proc;
    const WadArt& art;
    const Textures& tex;
    std::array<BandView, DoomEQProcessor::numBands> bandViews;
    double sampleRate = 44100.0;

    bool commandMode = false;
    float overlay = 0.7f;
    int hoverBand = -1, dragBand = -1;
    juce::Point<float> dragVirtual, dragLast;
    bool showHelp = false;
    bool needsFullRepaint = true;
    juce::String lastHint;
    int lastEngineState = -1;
    int ticksWithoutAudio = 0;
    bool spectrumWasVisible = false;

    std::vector<int> binColumn;
    std::vector<float> binTilt, columns;
    int spectrumTableWidth = 0;
    double spectrumTableRate = 0.0;

    // The grid and the curves only change when something is edited, so they are drawn once
    // into images (at the screen's pixel scale) and reused every frame.
    juce::Image gridLayer, curveLayer;
    uint64_t gridLayerKey = 0, curveLayerKey = 0;

    juce::Image doomFrame { juce::Image::ARGB, DoomEngine::width, DoomEngine::height, true };
    uint32_t doomSerial = 0;
    juce::Image backdrop;

    static constexpr int fftOrder = 12, fftSize = 1 << fftOrder;
    juce::dsp::FFT fft { fftOrder };
    juce::dsp::WindowingFunction<float> window { (size_t) fftSize, juce::dsp::WindowingFunction<float>::hann };
    std::vector<float> ring = std::vector<float> ((size_t) fftSize, 0.0f);
    int ringPos = 0;
    std::vector<float> fftData = std::vector<float> ((size_t) fftSize * 2, 0.0f);
    std::vector<float> spectrumDb = std::vector<float> ((size_t) fftSize / 2, -120.0f);
    std::vector<float> pullScratch = std::vector<float> (1 << 15);

    std::map<int, int> heldKeys; // JUCE key code -> doom key
    bool ctrlDown = false, shiftDown = false, altDown = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EqDisplay)
};

//==============================================================================
class DoomEQEditor : public juce::AudioProcessorEditor, public juce::SettableTooltipClient, private juce::Timer
{
public:
    explicit DoomEQEditor (DoomEQProcessor&);
    ~DoomEQEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void visibilityChanged() override;
    void parentHierarchyChanged() override;

private:
    // Presets (the Sammy J preset bar: < NAME > SAVE, and its menu)
    void setupPresetBar();
    void updatePresetDisplay();
    void showPresetMenu();
    void promptSavePreset (const juce::String& suggestedName);
    void checkNameAndSave (const juce::String& name);
    void confirmOverwrite (const juce::String& name, const juce::String& replacedName);
    void confirmDeletePreset (const juce::File& file, const juce::String& displayName);
    void savePresetAs (const juce::String& name);
    void showDialog (DoomDialogOptions options);

    // About and updates
    void showAbout();
    void startUpdateCheckWhenShown();
    void showUpdatePopup (const sammyj::UpdateInfo& info);
    void overlayClosed();
    std::optional<sammyj::UpdateInfo> pendingUpdate; // found while a dialog or About was open

    void rebuildChrome (float scale);
    void fixValueBoxScales();
    void timerCallback() override;
    void setCommandMode (bool);

    struct Strip
    {
        juce::ToggleButton on;
        TypeCombo type;
        juce::Slider freq, gain, q;
        juce::Label freqLabel, gainLabel, qLabel;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> onAttachment;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> typeAttachment;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> freqAttachment, gainAttachment, qAttachment;
        juce::Rectangle<int> bounds;
    };

    DoomEQProcessor& proc;
    juce::SharedResourcePointer<WadArt> art;
    Textures tex;
    DoomEQLookAndFeel lnf;
    juce::SharedResourcePointer<DoomEngine> doom;

    EqDisplay display;
    std::array<std::unique_ptr<Strip>, DoomEQProcessor::numBands> strips;

    juce::Slider output;
    juce::Label outputLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> outputAttachment;

    juce::ToggleButton doomSound;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> doomSoundAttachment;
    juce::Slider doomLevel, overlaySlider;
    juce::Label doomLevelLabel, overlayLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> doomLevelAttachment;
    juce::TextButton commandButton, helpButton;
    juce::TextButton presetPrev { "<" }, presetName, presetNext { ">" }, presetSave { "SAVE" };
    juce::ToggleButton bypass;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> bypassAttachment;
    juce::TooltipWindow tooltips { this, 600 };

    juce::Image skull[2], faces[5][3], ouch;
    int frameCount = 0, faceLook = 1, faceHurt = 0, nextLookChange = 30;
    bool faceOuch = false;
    bool sizeRestored = false;
    float facePeak = 0.0f;
    uint32_t lastBlockCount = 0, lastIdleBlockCount = 0;
    int hostIdleTicks = 0;

    // Everything static (textures, panels, rivets, title) drawn once at the screen's pixel scale.
    juce::Image chrome;
    float chromeScale = 0.0f;

    // Declared last: destroyed first (the destructor also resets them in a set order).
    std::unique_ptr<DoomDialog> dialog;
    std::unique_ptr<AboutPanel> about;
    std::unique_ptr<UpdatePopup> updatePopup;
    std::unique_ptr<sammyj::UpdateCheck> updateCheck;

    juce::Rectangle<int> titleClickArea, siteLinkArea;
    juce::Rectangle<int> headerArea, controlRow, titleArea, skullArea, stripsArea, outputArea, faceArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DoomEQEditor)
};
