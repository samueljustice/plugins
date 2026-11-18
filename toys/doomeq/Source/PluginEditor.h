#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "DoomEngine.h"
#include "WadArt.h"

namespace Palette
{
    inline const juce::Colour background  { 0xff100e0c };
    inline const juce::Colour text        { 0xffe8dcc0 };
    inline const juce::Colour textDim     { 0xffa89870 };
    inline const juce::Colour gold        { 0xffe8b84a };
    inline const juce::Colour hudYellow   { 0xfff4d442 };
    inline const juce::Colour doomRed     { 0xffd22a1c };
    inline const juce::Colour bevelLight  { 0xffa49c8a };
    inline const juce::Colour bevelDark   { 0xff181512 };

    inline const juce::Colour bands[DoomEQProcessor::numBands] = {
        juce::Colour (0xffe8503c), juce::Colour (0xfff09030), juce::Colour (0xffeed040),
        juce::Colour (0xff70cc58), juce::Colour (0xff4cb4e0), juce::Colour (0xffb078e8)
    };
}

/** The textures every part of the UI shares, cut from the IWAD once. */
struct Textures
{
    explicit Textures (const WadArt&);
    juce::Image background, panel, plate, hellish, hazard, groove;
};

class DoomEQLookAndFeel : public juce::LookAndFeel_V4
{
public:
    DoomEQLookAndFeel (const WadArt&, const Textures&);

    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider&) override;
    void drawLinearSlider (juce::Graphics&, int x, int y, int w, int h, float pos, float minPos, float maxPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool highlighted, bool down) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool highlighted, bool down) override;
    void drawComboBox (juce::Graphics&, int w, int h, bool down, int bx, int by, int bw, int bh, juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;
    void drawLabel (juce::Graphics&, juce::Label&) override;
    juce::Label* createSliderTextBox (juce::Slider&) override;

    void drawPopupMenuBackground (juce::Graphics&, int w, int h) override;
    void drawPopupMenuItem (juce::Graphics&, const juce::Rectangle<int>& area, bool isSeparator, bool isActive,
                            bool isHighlighted, bool isTicked, bool hasSubMenu, const juce::String& text,
                            const juce::String& shortcutKeyText, const juce::Drawable* icon, const juce::Colour* textColour) override;
    void getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator, int standardMenuItemHeight,
                                    int& idealWidth, int& idealHeight) override;

private:
    const WadArt& art;
    const Textures& tex;
};

//==============================================================================
/** The big screen: DOOM, with the analyser and EQ curve laid over it. */
class EqDisplay : public juce::Component
{
public:
    EqDisplay (DoomEQProcessor&, const WadArt&, const Textures&);

    void setCommandMode (bool inCommand);
    bool isInCommand() const { return commandMode; }
    std::function<void (bool)> onCommandChange;

    void setOverlayOpacity (float);

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

    static bool isTakeCommandKey (const juce::KeyPress&);
    static bool isLeaveCommandKey (const juce::KeyPress&);

private:
    struct BandView
    {
        bool on = true;
        eq::Type type = eq::Type::Bell;
        float freq = 1000.0f, gain = 0.0f, q = 1.0f;
        eq::BandCoeffs coeffs;
    };

    void readBands();
    juce::Point<float> handlePosition (int band) const;
    int hitTestHandle (juce::Point<float>) const;
    float xForFreq (float f) const;
    float freqForX (float x) const;
    float yForDb (float db) const;
    float dbForY (float y) const;
    juce::RangedAudioParameter* param (int band, const char* name) const;
    void setParam (int band, const char* name, float value);
    void releaseAllKeys();
    void updateAnalyser();
    juce::Rectangle<float> doomArea() const;
    void rebuildBackdrop();

    void paintGrid (juce::Graphics&, juce::Rectangle<float>);
    void paintSpectrum (juce::Graphics&, juce::Rectangle<float>);
    void paintCurves (juce::Graphics&, juce::Rectangle<float>);
    void paintHandles (juce::Graphics&);

    DoomEQProcessor& proc;
    const WadArt& art;
    const Textures& tex;
    std::array<BandView, DoomEQProcessor::numBands> bandViews;
    double sampleRate = 44100.0;

    bool commandMode = false;
    float overlay = 0.7f;
    int hoverBand = -1, dragBand = -1;

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
class DoomEQEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit DoomEQEditor (DoomEQProcessor&);
    ~DoomEQEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    void timerCallback() override;
    void setCommandMode (bool);

    struct Strip
    {
        juce::ToggleButton on;
        juce::ComboBox type;
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
    juce::OpenGLContext glContext;

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
    juce::TextButton commandButton;

    juce::Image skull[2], faces[5][3], ouch;
    int frameCount = 0, faceLook = 1, faceHurt = 0, nextLookChange = 30;
    bool faceOuch = false;

    juce::Rectangle<int> headerArea, titleArea, skullArea, stripsArea, outputArea, faceArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DoomEQEditor)
};
