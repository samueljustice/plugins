#pragma once

#include <JuceHeader.h>

#include "DeHonkCore.h"
#include "scene/BandView.h"
#include "scene/PondScene.h"

#include <array>
#include <functional>

class DeHonkAudioProcessor;

/**
 * The pond: hosts the picture (dehonk::scene::PondScene) as a card, with nothing drawn over it.
 *
 * It also owns the display's clock. Its timer drains the processor's display FIFO, runs the
 * scene, and then calls onTick so that the band view (and anything else) is updated from the
 * same frame at the same moment. The timer runs only between setActive (true) and
 * setActive (false): 30 Hz, dropping to 5 Hz while the scene is still and nothing new has arrived.
 * If no frame arrives for a quarter of a second (the host has stopped), the held frame's cuts and
 * levels fall away so that both pictures come to rest.
 */
class DisplayComponent : public juce::Component,
                         private juce::Timer
{
public:
    explicit DisplayComponent (DeHonkAudioProcessor&);
    ~DisplayComponent() override;

    /** Starts or stops the timer (the editor calls this as the window shows or hides). */
    void setActive (bool shouldBeActive);

    /** Called on the message thread whenever a new frame has arrived. */
    std::function<void (const dehonk::DisplayFrame&)> onFrame;

    /** Called every tick after the scene has run, with what the band view should show and the
        time since the last tick. */
    std::function<void (const dehonk::scene::BandViewState&, double seconds)> onTick;

    /** The edges as the core is using them. */
    std::array<float, dehonk::kNumEdges> edgesAsUsed() const noexcept;

    void paint (juce::Graphics&) override;

    /** The timer's rate now (0 while stopped). For checks. */
    int timerRateHz() const noexcept { return rateHz; }

private:
    static constexpr int numWatched = dehonk::kNumEdges + dehonk::kNumModules + 7;   // edges, switches, snap, threshold, bypass, max cut, impact guard, solo, diff

    void timerCallback() override;
    void runScene (double seconds);
    bool settleHeldFrame (double seconds);   // false once the held frame has come to rest
    bool frameMatters (const dehonk::DisplayFrame&);   // true if a new frame differs visibly from the last one that did
    dehonk::scene::SceneInput gatherInput() const;
    std::array<float, numWatched> watchedValues() const;
    bool moduleOn (int m) const noexcept;
    float value (const juce::RangedAudioParameter&) const noexcept;

    DeHonkAudioProcessor& processor;
    std::array<juce::RangedAudioParameter*, dehonk::kNumEdges> edges {};
    std::array<juce::RangedAudioParameter*, dehonk::kNumModules> switches {};
    juce::RangedAudioParameter* snapSwitch = nullptr;
    juce::RangedAudioParameter* threshold = nullptr;
    juce::RangedAudioParameter* bypass = nullptr;
    juce::RangedAudioParameter* maxCut = nullptr;
    juce::RangedAudioParameter* impactGuard = nullptr;

    dehonk::scene::PondScene scene;
    juce::Image picture;

    dehonk::DisplayFrame frame;
    dehonk::DisplayFrame shownFrame;   // the last frame that changed the picture
    bool hasFrame = false;
    bool wasSettling = false;  // the held frame was still falling at the last tick
    std::array<float, numWatched> lastValues {};
    double lastTickMs = 0.0;
    double sinceFrame = 0.0;   // seconds since the last new frame
    int rateHz = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DisplayComponent)
};
