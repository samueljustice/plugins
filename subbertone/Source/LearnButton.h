#pragma once

#include <JuceHeader.h>
#include "LearnWorker.h"
#include "Theme.h"

#include <functional>

class LearnGlow;

/**
 * LEARN, large, in the middle of the editor's header: a pill like LO CUT and LIMIT, in DETECT's teal.
 *
 * Click: learns the loop that plays now (when lit, learns it again: what is learned replaces only what it overlaps).
 * While LEARN waits for playback, listens or works the pitch out, a click stops it. Alt-click forgets everything
 * learned (UNDO for 10 s, beside the button and in the view). Double-click opens or closes the view. A mouse click
 * waits for the double-click time (about 0.4 s) before it acts, and a second click within it makes the pair a
 * double-click, so a double-click never starts, stops or forgets anything. A click still does what LEARN offered when
 * it was clicked: if that is over by the time it acts (what it would cancel has finished, or something else has started
 * since, such as METHOD in the view), it does nothing. A right-click (a Ctrl-click on a Mac) does nothing: it asks for a
 * menu, and LEARN has none.
 *
 * From the keyboard it is a button like any other: Return clicks it at once (Alt-Return forgets everything) and
 * Shift-Return opens or closes the view. For a screen reader, its press is a plain click whatever keys are held (a
 * screen reader holds keys of its own) and "show menu" opens or closes the view. Its keyboard focus shows as a ring in
 * the text's colour just inside the pill, clear of its outline, so that the outline still shows LEARN's state: an amber
 * ring on the outline, as on the editor's other controls, would look just like the amber edge of an ambiguous pitch
 * (below).
 *
 * Its look follows LEARN's status, which the editor passes in 20 times a second (update()):
 *   off            nothing learned yet: the outline breathes red with the editor's glow (LearnGlow)
 *   armed          the outline breathes teal once a second: waiting for the loop to play
 *   capturing      the fill pulses, and a 2 px bar shows how much of the loop has been heard (a dot sweeps to and
 *                  fro while the loop's length is not known yet)
 *   analysing      the fill pulses, and the bar shows how far the analysis has got
 *   learned here   solid fill: the sub follows what was learned at the playhead
 *   elsewhere      half fill: something is learned, but not where the playhead is (or USE LEARNED is off)
 *   no timeline    dimmed: the host gives no timeline (the standalone app has none), so nothing can be lined up
 * plus an amber edge when the analysis could not be sure of the line, and a red dot when part of what was learned no
 * longer matches the audio (neither while USE LEARNED is off: nothing learned is followed then). Each state has its
 * own tooltip.
 *
 * It does not repaint itself: the editor repaints it when update() says its look changed, and while it breathes or
 * pulses, the editor's glow repaints it with the header's frames (see LearnGlow). Its outline breathes on the glow's
 * clock, so it holds still with the glow; the fill's pulse while LEARN listens or works keeps moving (its bar moves
 * then, so it is repainted anyway).
 *
 * Message thread only.
 */
class LearnButton : public juce::TextButton
{
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LearnButton)

public:
    enum class Look
    {
        off,
        armed,
        capturing,
        analysing,
        learnedHere,
        learnedElsewhere,
        noTimeline
    };

    explicit LearnButton(subbertone::learn::Learner& learner);
    ~LearnButton() override;

    /** LEARN's status now (the editor's timer), and whether the host gives a timeline (see
        SubbertoneAudioProcessor::hasHostTimeline()). True if what it shows changed (it wants a repaint). */
    bool update(const subbertone::learn::Status& status, const subbertone::learn::Snapshot& snapshot, bool hostTimeline);

    /** Reads the status from the learner, updates and repaints (after a command, so the button answers at once). */
    void refresh();

    /** Whether it breathes or pulses (off, armed, capturing, analysing): repainted with every frame of the header. */
    bool isAnimating() const noexcept;

    /** Whether it pulses (capturing, analysing): this keeps moving while the glow holds still. */
    bool isPulsing() const noexcept;

    /** The glow whose clock its outline breathes on (without one, the outline breathes on its own). */
    void setGlow(const LearnGlow* glow) noexcept { m_glow = glow; }

    /** In the standalone app the tooltip of the dimmed button says that the app has no timeline. */
    void setStandaloneApp(bool standalone) noexcept { m_standalone = standalone; }
    bool isStandaloneApp() const noexcept { return m_standalone; }

    /** A double-click, Shift-Return or a screen reader's "show menu": open or close the view. */
    std::function<void()> onToggleView;

    // What it shows, for tests.
    Look getLook() const noexcept { return m_look; }
    bool showsUncertain() const noexcept { return m_uncertain; }
    bool showsPartlyInvalid() const noexcept { return m_partlyInvalid; }
    float getBarValue() const noexcept { return m_bar; }   // 0..1, or c_sweep, or c_noBar

    static constexpr float c_sweep = -1.0f;   // the loop's length is not known yet: a dot sweeps
    static constexpr float c_noBar = -2.0f;

    void clicked(const juce::ModifierKeys& modifiers) override;
    bool keyPressed(const juce::KeyPress& key) override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
    void mouseDoubleClick(const juce::MouseEvent& event) override;
    void paintButton(juce::Graphics& g, bool isMouseOverButton, bool isButtonDown) override;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
    // What a click asks for, decided by what LEARN shows when it is clicked.
    struct Click
    {
        enum class Intent
        {
            learn,    // learn the loop that plays now
            stop,     // stop what is under way
            forget    // Alt: forget everything learned
        };

        Intent intent = Intent::learn;
        subbertone::learn::Method method = subbertone::learn::Method::Auto;   // to learn with
    };

    subbertone::learn::Method methodToLearn(const subbertone::learn::Status& status,
                                            const subbertone::learn::Snapshot& snapshot) const;
    juce::String tooltipFor(const subbertone::learn::Status& status, const subbertone::learn::Region* shown,
                            double fallback) const;

    Click clickFor(const juce::ModifierKeys& modifiers) const;   // what a click with these keys asks for now
    void perform(const Click& click);                            // ... done, unless that is over by now
    void act(const Click& click);                                // ... and shown at once
    void actOnHeldClick();                                       // a click held back, once no second click can follow
    void toggleView();

    subbertone::learn::Learner& m_learner;
    juce::SharedResourcePointer<Theme::Fonts> m_fonts;
    const LearnGlow* m_glow = nullptr;

    Look m_look = Look::off;
    bool m_uncertain = false;
    bool m_partlyInvalid = false;
    float m_bar = c_noBar;
    bool m_hostTimeline = true;          // as last passed to update()
    bool m_standalone = false;
    double m_noTimelineSinceMs = -1.0;   // when the host last started to give no timeline (-1: it gives one)

    // A mouse click is held back for the double-click time: if it acted at once, the double-click would have to take
    // it back, and that cannot always be done (a capture would start again from nothing, the audio kept for METHOD
    // would be given up).
    juce::TimedCallback m_heldClickTimer { [this] { actOnHeldClick(); } };
    Click m_heldClick;                   // what the held click asked for when it was made
    bool m_clickHeld = false;            // a mouse click waits to see whether a second follows
    bool m_clickDropped = false;         // ... a second press came: if it ends as no double-click, the click acts
    bool m_mouseClick = false;           // inside a mouse-up: the click is a mouse click
    bool m_mouseClicked = false;         // ... and the mouse-up clicked LEARN (it was let go on it)
    bool m_secondClick = false;          // inside the mouse-up of a double-click's second click
    int m_plainPresses = 0;              // plain presses in a row close enough to make a multiple click, this one too
                                         // (a press let go off LEARN clicks nothing and ends the row)
    bool m_popupPress = false;           // this press is a right-click: it neither presses nor clicks LEARN
};

//==============================================================================
/**
 * What to do next, beside LEARN: "PLEASE PRESS LEARN" in red while nothing is learned (breathing with the editor's
 * glow), "PLAY THE LOOP ONCE" while LEARN waits, how far it has got while it listens or works it out, and nothing once
 * something is learned. Its tooltip says how in one line. Right after everything learned was forgotten it offers UNDO
 * (unless a later try to learn failed: then it says why, and UNDO stays in the view): a click on it brings everything
 * back, for as long as that can be undone. While it offers UNDO it also takes the keyboard focus (Return or Space).
 *
 * A screen reader reads what it says, hears when that changes (not every percentage while LEARN listens or works), and
 * can press UNDO.
 *
 * Repainted like the button: by the editor when update() says it changed, and with the header's frames while it
 * breathes (on the glow's clock: it holds still with the glow).
 *
 * Message thread only.
 */
class LearnPrompt final : public juce::Component, public juce::SettableTooltipClient
{
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LearnPrompt)

public:
    explicit LearnPrompt(subbertone::learn::Learner& learner);
    ~LearnPrompt() override;

    /** After LearnButton::update(): what the button shows, and LEARN's status. True if what it shows changed. */
    bool update(const LearnButton& button, const subbertone::learn::Status& status);

    /** The glow whose clock it breathes on (without one, it breathes on its own). */
    void setGlow(const LearnGlow* glow) noexcept { m_glow = glow; }

    /** After a click on UNDO brought back what was forgotten. */
    std::function<void()> onUndo;

    // What it shows, for tests.
    const juce::String& getText() const noexcept { return m_text; }
    bool offersUndo() const noexcept { return m_undo; }

    void paint(juce::Graphics&) override;
    void mouseEnter(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    bool keyPressed(const juce::KeyPress&) override;
    void focusGained(FocusChangeType) override;
    void focusLost(FocusChangeType) override;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
    juce::Font fontFor(const juce::String& text) const;
    void undo();

    subbertone::learn::Learner& m_learner;
    juce::SharedResourcePointer<Theme::Fonts> m_fonts;
    const LearnGlow* m_glow = nullptr;

    juce::String m_text;
    juce::Colour m_colour;
    bool m_breathes = false;   // red, with the editor's glow
    bool m_undo = false;
    juce::String m_kind;       // what it says without its figures: a screen reader hears when that changes
    bool m_shown = false;      // update() has run: what it says at first is no news
};

//==============================================================================
/**
 * The editor's call to learn: while nothing is learned yet and the host has a timeline, a slow red glow breathes along
 * the editor's edges and on the scope's frame; while LEARN waits for the loop, listens to it or works it out, a calmer
 * teal one; once something is learned, none.
 *
 * It is painted only into the editor's outer margins, outside every panel, so it never covers or recolours a control.
 * The scope's OpenGL view would cover anything painted over it, so the scope tints its own frame instead
 * (onFrameColour): it draws itself at the display's rate anyway.
 *
 * Cheap: in each display frame it repaints one edge, the four in turn, and none more than 30 times a second. (On macOS
 * every area repainted within one frame is merged into the one rectangle around them all, so the four edges at once
 * would repaint the whole editor.) An edge waits until the one before has been drawn, so two never meet in one redraw
 * when the system falls behind. LEARN's button and prompt breathe with it; they are repainted in the top edge's frames
 * (onHeaderFrame), so that those frames stay within the header. Nothing runs once the glow has faded out.
 *
 * For the same reason it holds still while anything else in the editor is redrawn (meters during playback, a knob being
 * dragged, the Learn view): an edge repainted in the same frame as a meter would redraw the rectangle around both, most
 * of the editor. It sees those redraws in paint() (a redraw reaching beyond the edge it asked for), and from the first
 * of them until none has come for 1.5 s its clock stops: no edge is repainted, and the button and the prompt, which
 * breathe on the same clock, hold still too. It breathes on from where it held, without a jump. A change it must show
 * meanwhile (a new mode, the glow coming or going) is shown at once, each edge repainted once.
 *
 * Message thread only.
 */
class LearnGlow final
{
public:
    enum class Mode
    {
        off,
        prompt,   // nothing learned: red, breathing about 0.6 times a second
        busy      // LEARN waits, listens or works: teal, slower and softer
    };

    enum Edge
    {
        top,      // with the header's frame (onHeaderFrame)
        right,
        bottom,
        left,
        numEdges
    };

    /** surface: the component whose paint() calls paint() last (the editor's content). */
    explicit LearnGlow(juce::Component& surface);
    ~LearnGlow();

    void setMode(Mode mode);
    Mode getMode() const noexcept { return m_mode; }

    /** The glow that goes with what LEARN's button shows: red while nothing is learned, teal while it waits, listens
        or works, none once something is learned or when there is no timeline. */
    static Mode modeFor(LearnButton::Look look) noexcept;

    /** In the surface's coordinates: the area whose edges glow. */
    void setLayout(juce::Rectangle<int> edges);

    /** In the surface's coordinates: LEARN's button and prompt, which the top edge's frames repaint too. */
    void setHeaderArea(juce::Rectangle<int> header) noexcept { m_header = header; }

    /** From the surface's paint(), after everything else. */
    void paint(juce::Graphics& g);

    /** True while it shows or fades. */
    bool isRunning() const noexcept { return ! m_frames.isEmpty(); }

    /** True while the display's frames drive it (it has had one lately): then the header's frames repaint LEARN's
        button and prompt, and nothing else should. A display that gives no frames never leaves them stale. */
    bool drivesHeader() const noexcept;

    /** True while it holds still because the editor has lately redrawn something else (see the class notes): then the
        header's frames repaint nothing, and LEARN's button and prompt are repainted only when what they show changes. */
    bool isHeld() const noexcept;

    /** Seconds on the clock everything LEARN breathes on (the glow, the button's outline, the prompt): it stops while
        the glow holds still. */
    double getClock() const noexcept;

    /** Told the colour the scope's frame should take at every frame (transparent: its own). */
    std::function<void(juce::Colour)> onFrameColour;

    /** In the frames that repaint the top edge: repaint LEARN's button and prompt (they lie in the header too). */
    std::function<void()> onHeaderFrame;

    /** One display frame at a time (milliseconds on the hi-res counter): moves the animation on and repaints the next
        edge if its turn has come. The display's frames call it; tests may too. Returns the edge repainted, or -1. */
    int frame(double nowMs);

    /** Moves the animation on to a time without repainting (for tests). */
    void advanceTo(double nowMs);

    /** How strongly it glows now, 0..1, and in what colour (for tests). */
    float getLevel() const noexcept { return m_level; }
    juce::Colour getColour() const noexcept { return m_colour; }

    /** The area an edge repaints, in the surface's coordinates (empty without a layout). */
    juce::Rectangle<int> getEdgeArea(int edge) const noexcept;

    /** 0..1: the breath of a mode at a time (seconds), shared with LEARN's button and prompt, so that everything
        red breathes together. Smooth: no hard flashing, never faster than once a second. */
    static float breath(Mode mode, double seconds) noexcept;

    static constexpr int c_maxEdgeHz = 30;         // the most an edge is repainted in a second
    static constexpr float c_edgeWidth = 10.0f;    // design px: inside the editor's 16 px margins

private:
    void start();
    void stop();
    juce::Colour frameColour() const noexcept;
    bool isHeldAt(double nowMs) const noexcept;
    double clockAt(double nowMs) const noexcept;
    void noteOtherRedraw(double nowMs) noexcept;

    juce::Component& m_surface;
    juce::Rectangle<int> m_edges;
    juce::Rectangle<int> m_header;               // LEARN's button and prompt
    juce::VBlankAttachment m_frames;             // the display's frames, while it runs

    Mode m_mode = Mode::off;
    float m_presence = 0.0f;                     // 0..1: fades in when a mode starts, out when it ends
    float m_breath = 0.0f;                       // 0..1: the mode's breath, smoothed across a change of mode
    float m_level = 0.0f;                        // presence x breath: never jumps
    juce::Colour m_colour { Theme::Colours::bad };
    double m_lastStepMs = -1.0;
    double m_lastEdgeMs = -1.0;                  // when an edge was last repainted
    double m_lastFrameMs = -1.0;                 // when the display last gave a frame
    int m_nextEdge = top;
    int m_pendingEdge = -1;                      // asked to be repainted, not drawn yet
    int m_edgesToClear = 0;                      // faded out: edges still showing some glow
    int m_edgesToRefresh = 0;                    // changed while held (a new mode): edges to repaint once

    // Holding still. The clock is the hi-res counter less the time spent held.
    double m_lastOtherRedrawMs = -1.0;           // when a redraw last reached beyond the glow's own areas
    double m_holdStartMs = -1.0;                 // when the latest hold began (-1: none yet)
    double m_heldBeforeMs = 0.0;                 // how long the holds before it lasted
};
