#pragma once

#include <JuceHeader.h>

#include "Theme.h"

/** The leaf that turns. It holds a picture of the page as it was and lifts it away to show the
    page underneath: forwards (the right edge folds over to the left, bottom corner first),
    backwards (mirrored), or a plain dissolve for the change between paper and night. */
class PageTurn : public juce::Component
{
public:
    enum class Kind { forward, backward, dissolve };

    PageTurn();

    /** Starts turning `oldPage` (a snapshot of the whole window, `scale` pixels per point). */
    void begin (juce::Image oldPage, float scale, Kind kind, const Theme::Palette& oldPalette);
    bool isRunning() const noexcept { return progress < 1.0f; }

    /** How long a turn takes, and how far in the next page starts being written. */
    static constexpr float turnSeconds = 0.62f;
    static constexpr float dissolveSeconds = 0.32f;
    static constexpr float writeDelaySeconds = 0.3f;

    /** For the preview tool: holds the turn at this point (0..1). */
    void freezeAt (float position);

    void paint (juce::Graphics&) override;

private:
    void paintTurn (juce::Graphics&, float eased);
    void tick();

    juce::Image page;
    float pageScale = 1.0f;
    Kind kind = Kind::forward;
    juce::Colour paper, ink;
    float progress = 1.0f;
    bool frozen = false;
    double lastTick = 0.0;
    juce::VBlankAttachment vblank { this, [this] { tick(); } };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PageTurn)
};
