#pragma once

#include <JuceHeader.h>

#include "Theme.h"

/** The cover of the tome. When the window opens it lies shut, then swings open on its spine
    (the left edge) to show the page. Dealing a new entry shuts it and opens it again; the
    `whileShut` callback runs at the moment it is closed, which is when the page changes. */
class BookCover : public juce::Component
{
public:
    BookCover();

    void setNight (bool night);

    /** Lies shut until the window is first on screen, then opens. `onOpening` runs as it starts to open. */
    void openWhenShown (std::function<void()> onOpening);
    /** Shuts the book, runs `whileShut`, and opens it again. */
    void shutAndReopen (std::function<void()> whileShut);
    bool isBusy() const noexcept { return stage != Stage::open; }

    static constexpr float introHoldSeconds = 0.5f;
    static constexpr float shutSeconds = 0.36f;
    static constexpr float shutHoldSeconds = 0.12f;
    static constexpr float openSeconds = 0.72f;
    /** How long after the cover starts to open the page should start being written. */
    static constexpr float writeDelaySeconds = 0.4f;

    /** For the preview tool: holds the cover this far open (0 = shut, 1 = fully open). */
    void freezeAt (float openness);

    void paint (juce::Graphics&) override;
    void resized() override { cover = {}; }

private:
    enum class Stage { open, waitingToAppear, held, shutting, opening };

    void tick();
    void renderCover (float scale);
    void paintCover (juce::Graphics&, float openness);

    Stage stage = Stage::open;
    float position = 0.0f;       // 0..1 through the current stage
    float holdSeconds = 0.0f;
    bool night = false, frozen = false;
    float frozenOpenness = 0.0f;
    std::function<void()> onShut, onOpen;
    juce::Image cover;
    float coverScale = 1.0f;
    double lastTick = 0.0;
    juce::VBlankAttachment vblank { this, [this] { tick(); } };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BookCover)
};
