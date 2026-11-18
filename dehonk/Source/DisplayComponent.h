#pragma once

#include <JuceHeader.h>

#include "DeHonkCore.h"
#include "PluginProcessor.h"
#include "scene/BandView.h"
#include "scene/PondScene.h"

#include <array>
#include <atomic>
#include <functional>
#include <memory>

/**
 * The pond: hosts the picture (dehonk::scene::PondScene) as a card. The tops of the trees that
 * mark the range edges rise a little above the card's top edge, into a strip kept clear for them.
 *
 * It owns the display's clock. Between setActive (true) and setActive (false) it runs from the
 * screen's own refresh: a picture at every refresh while anything moves (up to the refresh of the
 * screen, 120 a second on a 120 Hz screen), each moved on by the time that has really passed, and
 * 5 a second while the pond is still and nothing new has arrived. Each time, it takes the frames
 * it has not seen from the processor (every open window sees every frame) and asks for the next picture. onFrame and onTick (the band view and
 * whatever else follows the frames) are called from the same clock, at the same rate unless told
 * otherwise (see setFollowerRateHz). If no frame arrives for a quarter of a second (the host has
 * stopped), the held frame's cuts and levels fall away so that the pictures come to rest.
 *
 * The pictures are drawn on a thread of their own, a refresh ahead: at each refresh the message
 * thread puts up the picture that is ready (a swap, no copy) and hands over what the next one
 * should show. The two threads share nothing but a few slots passed back and forth through atomic
 * exchanges: neither ever waits for the other, and a busy message thread never holds up the
 * drawing (nor the other way round). The picture is drawn at the size of the card in the screen's
 * own pixels, so that it goes to the window pixel for pixel whatever the scale.
 *
 * Only the picture's own rectangle is repainted each time, and the picture is opaque, so nothing
 * behind it is drawn again. The strip with the treetops is repainted only when a tree moves or
 * changes colour.
 *
 * The picture takes the mouse: whoever is under the pointer can be clicked (the pointer turns to a
 * hand), and reacts. A click changes nothing in the sound.
 */
class DisplayComponent : public juce::Component,
                         private juce::Timer
{
public:
    explicit DisplayComponent (DeHonkAudioProcessor&);
    ~DisplayComponent() override;

    /** Starts or stops the clock (the editor calls this as the window shows or hides). */
    void setActive (bool shouldBeActive);

    /** Called on the message thread when a new frame has arrived, at the followers' rate. */
    std::function<void (const dehonk::DisplayFrame&)> onFrame;

    /** Called at the followers' rate after the scene has run, with what the band view should show
        and the time since the last call. */
    std::function<void (const dehonk::scene::BandViewState&, double seconds)> onTick;

    /** How often onFrame and onTick are called while the pond runs at its full rate: 0 (the
        default) for every picture; otherwise this many times a second at most (15 and up). */
    void setFollowerRateHz (int hz);

    /** The range whose creatures are at work: 0 Woof, 1 Tub, 2 Fog, 3 Bark, 4 Snap, or -1 for all
        of them. With one in focus, everyone else in the picture holds still, a little dimmed, and
        the recordist minds only that range. */
    void setFocusModule (int module);
    int getFocusModule() const noexcept { return focusModule; }

    /** Where the frequency axis starts and ends across the picture's card, 0..1: the span of the
        band view's plot, so that the trees and posts in the pond stand over its handles. */
    void setAxisSpan (float left01, float right01);

    /** The edges as the core is using them. */
    std::array<float, dehonk::kNumEdges> edgesAsUsed() const noexcept;

    /** The picture's card inside this component (everything above it is the strip for the treetops). */
    juce::Rectangle<int> pictureBounds() const noexcept;

    /** The height of the strip kept clear above the card for the treetops. */
    static constexpr int treeRoom = 12;

    void paint (juce::Graphics&) override;
    void resized() override;
    void parentHierarchyChanged() override;
    bool hitTest (int x, int y) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;

    /** The clock's rate now: full (every refresh), 24 with only rain falling, 5 while still, 0
        while stopped. For checks. */
    int timerRateHz() const noexcept { return rateHz; }

    /** What the last pictures cost, for checks: averages and the worst, in milliseconds, since the
        last call with `reset` true. `scene` is moving the scene on and drawing it into its image
        (on the drawing thread); `paint` is putting that image and its frame on the window.
        `pictures` counts the pictures put up. */
    struct Timing { double sceneMs = 0, sceneWorstMs = 0, paintMs = 0, paintWorstMs = 0; int pictures = 0, paints = 0; double seconds = 0;
                    double clockMs = 0, clockWorstMs = 0; int clockTicks = 0; };   // clock: the message thread's own work at each refresh
    Timing timing (bool reset);

    /** The size of the picture now on show, in pixels, for checks. */
    juce::Point<int> pictureSize() const noexcept;

private:
    static constexpr int numWatched = dehonk::kNumEdges + dehonk::kNumModules + 7;   // edges, switches, snap, threshold, bypass, max cut, impact guard, solo, diff
    static constexpr int fullRateHz = 60, rainRateHz = 24, stillRateHz = 5;

    class Canvas;
    class Renderer;

    void timerCallback() override;
    void tick (double nowMs, bool fromRefresh = true);   // fromRefresh: called for a refresh of the screen (not the fallback timer)
    bool takePicture();                 // puts up the newest picture, if there is one
    void askForPicture (double seconds, double atMs);
    void watchCost (double sceneMs);
    void paintPicture (juce::Graphics&);
    void makeFrame();          // the card's corners and outline, drawn once for this size
    void treesChanged();
    void tellFollowers (double seconds, bool settling);
    bool settleHeldFrame (double seconds);   // false once the held frame has come to rest
    bool frameMatters (const dehonk::DisplayFrame&);   // true if a new frame differs visibly from the last one that did
    dehonk::scene::SceneInput gatherInput() const;
    std::array<float, numWatched> watchedValues() const;
    bool moduleOn (int m) const noexcept;
    float value (const juce::RangedAudioParameter&) const noexcept;

    DeHonkAudioProcessor& processor;
    DeHonkAudioProcessor::DisplayReader frameReader;   // this window's place in the processor's frames
    std::array<juce::RangedAudioParameter*, dehonk::kNumEdges> edges {};
    std::array<juce::RangedAudioParameter*, dehonk::kNumModules> switches {};
    juce::RangedAudioParameter* snapSwitch = nullptr;
    juce::RangedAudioParameter* threshold = nullptr;
    juce::RangedAudioParameter* bypass = nullptr;
    juce::RangedAudioParameter* maxCut = nullptr;
    juce::RangedAudioParameter* impactGuard = nullptr;

    std::unique_ptr<Renderer> renderer;
    std::unique_ptr<Canvas> canvas;
    std::unique_ptr<juce::VBlankAttachment> vblank;
    std::array<juce::Image, 4> corners;      // the card's corners: what lies behind them, and the outline's arc
    std::array<dehonk::scene::PondScene::TreeTop, dehonk::kNumEdges> tops {};
    float deviceScale = 1.0f;                // device pixels to the point, as last painted
    bool frameStale = true, capturing = false;

    dehonk::DisplayFrame frame;
    dehonk::DisplayFrame shownFrame;   // the last frame that changed the picture
    bool hasFrame = false;
    bool wasSettling = false;  // the held frame was still falling at the last picture
    bool freshNews = false, changed = false, poked = false, frameForFollowers = false;   // since the last picture
    float followerSnap = 0.0f;           // a click caught since the followers were last told
    float snapWaiting = 0.0f, snapSent = 0.0f;   // a click caught for the scene: not yet sent; sent, until the scene has taken it
    std::uint32_t snapSentWith = 0;      // the request that carries snapSent
    std::uint32_t requests = 0;          // pictures asked for
    int clicks = 0;                      // clicks on the picture, all told
    float clickX = 0.0f, clickY = 0.0f;  // where the last one was, 0..1
    std::array<float, numWatched> lastValues {};
    double lastPictureMs = 0.0, lastVBlankMs = 0.0, lastNewFrameMs = 0.0, lastTickMs = 0.0, refreshMs = 1000.0 / 60.0;
    double followerSeconds = 0.0;
    int refreshesSincePicture = 0;
    int followerHz = 0;
    int focusModule = -1;
    float axisLeft = 0.0f, axisRight = 1.0f;
    int quality = 2;           // how finely the scene is drawn: 2 as fine as the display can show, 1 never more finely than that, 0 at half size
    int costCount = 0, retryIn = 0, retryAfter = 8;   // retry: in spans of 120 pictures
    double costMs = 0.0;       // what the scenes of this span have cost, in all
    int rateHz = 0;
    bool sceneMoving = true, sceneRaining = false;   // as the newest picture found the scene
    Timing stats;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DisplayComponent)
};
