#include "LearnButton.h"

#include "LearnView.h"
#include "SubbertoneLookAndFeel.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace
{
    namespace learn = subbertone::learn;
    using Activity = learn::Status::Activity;
    using Timeline = learn::LearnAudio::TimelineState;

    // The button
    constexpr double c_pulseHz = 1.0;               // armed: the outline breathes once a second
    constexpr double c_busyHz = 1.5;                // capturing, analysing: the fill pulses, smoothly (no flashing)
    constexpr double c_sweepSeconds = 1.4;          // the dot's trip there and back while the loop's length is unknown
    constexpr double c_noTimelineHoldMs = 300.0;    // the host must give no timeline this long before the button says so
                                                    // (just after the editor opens, no block may have said yet)
    constexpr double c_partlyInvalidSeconds = 0.2;  // of a pass tracked live before the red dot shows
    constexpr float c_labelHeight = 14.0f;          // design px, in the heading face: the editor's largest label after
                                                    // the title
    constexpr float c_focusRingInset = 3.0f;        // design px from the pill's edge to the focus ring's centre line:
                                                    // 1.5 px clear of the outline, and of the bar while it shows

    // The prompt
    constexpr float c_promptHeight = 12.0f;
    constexpr float c_promptMinHeight = 9.5f;       // a long prompt shrinks to fit, down to this

    // The glow
    constexpr double c_promptBreathHz = 0.6;        // red: a slow breath, far below the 3 Hz where flashing starts
    constexpr double c_busyBreathHz = 0.35;         // teal: slower still, and shallower
    constexpr double c_fadeInSeconds = 0.5;         // the glow comes and goes gently ...
    constexpr double c_fadeOutSeconds = 0.35;
    constexpr double c_breathSmoothSeconds = 0.15;  // ... and moves from one mode's breath to the other's without a step
    constexpr double c_colourSeconds = 0.35;        // red to teal
    constexpr float c_edgeAlpha = 0.55f;            // at the very edge of the editor, at the top of a breath
    constexpr float c_frameAlpha = 0.9f;            // the scope's own frame line
    constexpr double c_framesLostMs = 250.0;        // no display frame for this long: the frames no longer drive it
    constexpr double c_pendingTimeoutMs = 100.0;    // an edge waits this long at most for the one before to be drawn
    constexpr double c_holdMs = 1500.0;             // the glow holds still until nothing else has been redrawn this long
    constexpr int c_ownSlack = 4;                   // design px: an area repainted comes back rounded out to whole pixels
                                                    // in the window and back (about 3 px at the smallest editor size)
    constexpr int c_otherModeColour = 32;           // per channel: further than this from its mode's colour, the glow
                                                    // shows another mode's (its easing can stop a few steps short)

    double nowMs()
    {
        return juce::Time::getMillisecondCounterHiRes();
    }

    juce::String percent(float share)
    {
        return juce::String(juce::roundToInt(juce::jlimit(0.0f, 1.0f, share) * 100.0f)) + "%";
    }

    juce::String seconds(double s)
    {
        return juce::String(std::max(0.0, s), 1) + " s";
    }

    // 0..1, smooth: the armed outline.
    float breathe(double t)
    {
        return static_cast<float>(0.5 - 0.5 * std::cos(juce::MathConstants<double>::twoPi * c_pulseHz * t));
    }

    // 0..1, smooth: the fill while LEARN listens or works.
    float pulse(double t)
    {
        return static_cast<float>(0.5 - 0.5 * std::cos(juce::MathConstants<double>::twoPi * c_busyHz * t));
    }

    float smoothingStep(double dt, double seconds)
    {
        return static_cast<float>(1.0 - std::exp(-dt / seconds));
    }

    // The region the playhead is in: the one the sub follows now, or (stopped) the one at the last position played.
    const learn::Region* regionAtPlayhead(const learn::Status& status, const learn::Snapshot& snapshot)
    {
        if (const auto* region = snapshot.findId(status.regionHere))
            return region;

        if (status.timeline != Timeline::playing && status.playheadKnown)
            return snapshot.find(status.playheadSeconds);

        return nullptr;
    }

    // How much of a region fell back to live tracking: in the pass under way, or the one before if more did there (a
    // pass starts again from none at every wrap, and PARTLY INVALID should not blink off at every loop start).
    double fallbackSeconds(const learn::Region* region)
    {
        if (region == nullptr || region->live == nullptr)
            return 0.0;

        const auto hops = std::max(region->live->invalidHops.load(std::memory_order_relaxed),
                                   region->live->lastInvalidHops.load(std::memory_order_relaxed));
        return static_cast<double>(hops) * static_cast<double>(region->hopSamples) / region->learnedRate;
    }

    // Why the button is dimmed.
    juce::String noTimelineText(bool standalone)
    {
        if (standalone)
            return "The standalone app has no timeline, so Learn can't line up what it hears. Learn works in a host: "
                   "loop the part there, press LEARN and play it once.";

        return "This host isn't reporting a timeline here, so Learn can't line up what it hears.";
    }
}

//==============================================================================
LearnButton::LearnButton(learn::Learner& learner)
    : juce::TextButton("LEARN")
    , m_learner(learner)
{
    setClickingTogglesState(false);
}

LearnButton::~LearnButton()
{
    // A click still held back when the editor closes acts now: a click is never lost. (Nothing to show it on.)
    if (m_clickHeld || m_clickDropped)
        perform(m_heldClick);
}

void LearnButton::refresh()
{
    update(m_learner.getStatus(), *m_learner.getSnapshot(), m_hostTimeline);
    repaint();
}

bool LearnButton::update(const learn::Status& status, const learn::Snapshot& snapshot, bool hostTimeline)
{
    const double now = nowMs();
    m_hostTimeline = hostTimeline;

    if (! hostTimeline)
    {
        if (m_noTimelineSinceMs < 0.0)
            m_noTimelineSinceMs = now;
    }
    else
    {
        m_noTimelineSinceMs = -1.0;
    }

    const bool noTimeline = m_standalone || (m_noTimelineSinceMs >= 0.0 && now - m_noTimelineSinceMs >= c_noTimelineHoldMs);

    // The region the button speaks of: the one at the playhead, else the newest.
    const auto* here = regionAtPlayhead(status, snapshot);
    const auto* shown = here != nullptr ? here : snapshot.findId(status.region);
    const double fallback = fallbackSeconds(shown);

    auto look = Look::off;
    float bar = c_noBar;
    bool uncertain = false;
    bool partlyInvalid = false;

    switch (status.activity)
    {
        case Activity::analysing:
            look = Look::analysing;
            bar = juce::jlimit(0.0f, 1.0f, status.progress);
            break;

        case Activity::capturing:
            look = Look::capturing;
            bar = status.progress >= 0.0f ? juce::jlimit(0.0f, 1.0f, status.progress) : c_sweep;
            break;

        case Activity::armed:
            look = noTimeline ? Look::noTimeline : Look::armed;
            break;

        case Activity::idle:
            if (noTimeline)
            {
                look = Look::noTimeline;
            }
            else if (status.numRegions > 0)
            {
                // With USE LEARNED off nothing learned is followed: what may be wrong with it does not matter then.
                look = here != nullptr && status.follow ? Look::learnedHere : Look::learnedElsewhere;
                uncertain = status.follow && shown != nullptr && shown->uncertain;
                partlyInvalid = status.follow && fallback >= c_partlyInvalidSeconds;
            }
            break;
    }

    const bool changed = look != m_look || uncertain != m_uncertain || partlyInvalid != m_partlyInvalid
                      || std::abs(bar - m_bar) > 1.0e-3f;

    m_look = look;
    m_uncertain = uncertain;
    m_partlyInvalid = partlyInvalid;
    m_bar = bar;

    // The tooltip names the figures as they are now: a tip on show follows them.
    if (const auto tip = tooltipFor(status, shown, fallback); tip != getTooltip())
        setTooltip(tip);

    return changed;
}

learn::Method LearnButton::methodToLearn(const learn::Status& status, const learn::Snapshot& snapshot) const
{
    // Learning a region again keeps the method chosen for it; anything new starts with Auto.
    if (const auto* here = regionAtPlayhead(status, snapshot))
        return here->requested;

    return learn::Method::Auto;
}

juce::String LearnButton::tooltipFor(const learn::Status& status, const learn::Region* shown, double fallback) const
{
    const auto method = status.method == learn::Method::Auto ? juce::String()
                                                             : " (" + LearnView::describeMethod(status.method) + ")";

    switch (m_look)
    {
        case Look::armed:
            return "Waiting for playback: start the loop" + method + ". Click to cancel.";

        case Look::capturing:
            if (status.progress >= 0.0f)
                return "Listening to the loop (" + percent(status.progress) + "). Keep it playing. Click to cancel.";

            // No loop heard yet: say where to loop. A clip launcher repeats the audio while the song position runs on,
            // so no wrap ever comes, and LEARN would listen to its cap and learn positions the playhead has left.
            return "Listening (" + seconds(status.heardSeconds) + " so far). Keep the loop playing, or stop after a second "
                   "or more to learn what was heard. Loop the passage on your host's timeline, not in a clip launcher such "
                   "as Live's Session View: there the timeline runs on while the clip repeats, so LEARN never hears the "
                   "loop come round. Click to cancel.";

        case Look::analysing:
            return "Working out the pitch" + method + " (" + percent(status.progress) + "). Click to cancel.";

        case Look::noTimeline:
            return noTimelineText(m_standalone);

        case Look::learnedHere:
        case Look::learnedElsewhere:
        {
            const auto again = juce::String(" Click to learn again, Alt-click to forget everything learned.");

            // The red dot and the amber edge can show together: then the tooltip explains both.
            if (m_partlyInvalid && shown != nullptr)
                return "Part of what was learned no longer matches the audio (an edit, another take, or a change before "
                       "Subbertone): " + seconds(fallback) + " of " + seconds(shown->durationSeconds()) + " is tracked "
                       "live." + (m_uncertain ? " The pitch was ambiguous too: double-click to check the line it follows."
                                              : "")
                       + " Click to learn again.";

            if (m_uncertain)
                return "Learned, but the pitch was ambiguous: double-click to check the line it follows." + again;

            // (USE LEARNED is a button in the view: a double-click only opens the view.)
            if (! status.follow)
                return "USE LEARNED is off, so the pitch is tracked live everywhere. To follow what was learned again, "
                       "switch USE LEARNED back on in the view (double-click to open it).";

            if (m_look == Look::learnedHere && shown != nullptr)
                return "Following what it learned here (" + LearnView::describeMethod(shown->used) + ", line "
                       + LearnView::describeRatio(shown->lineRatio) + "). Click to learn again, Alt-click to forget "
                       "everything learned, double-click to choose the line.";

            const bool one = status.numRegions == 1;
            return juce::String(status.numRegions) + (one ? " region" : " regions") + " learned; outside "
                   + (one ? "it" : "them") + " the pitch is tracked live. Click to learn the loop that plays now, "
                   "Alt-click to forget everything learned, double-click to see what was learned.";
        }

        case Look::off:
            break;
    }

    if (status.outcome == learn::Status::Outcome::failed)
    {
        if (status.failure == learn::Status::Failure::nothingPitched)
            return "Nothing in what was heard had a pitch to learn. Check the Threshold, then click to try again.";

        if (status.failure == learn::Status::Failure::outOfMemory)
            return "There wasn't enough memory to listen to the loop. Click to try again.";

        if (status.failure == learn::Status::Failure::loopTooShort)
            return "This loop is shorter than a second. LEARN needs a loop of at least one second: lengthen it in your "
                   "host, then click to try again.";
    }

    if (status.canUndo)
        return "Everything learned was forgotten. To bring it back, click UNDO beside this button (or in the view) "
               "within 10 s. Click to learn the loop that plays now.";

    return "Learn: loop the tricky part in your host, click here and play it once. Subbertone listens to one full pass, "
           "works the pitch out with hindsight, and from then on follows what it learned there, in playback and in "
           "bounces. Double-click (or Shift-Return) to choose the line it follows.";
}

bool LearnButton::isAnimating() const noexcept
{
    return m_look == Look::off || m_look == Look::armed || isPulsing();
}

bool LearnButton::isPulsing() const noexcept
{
    return m_look == Look::capturing || m_look == Look::analysing;
}

//==============================================================================
LearnButton::Click LearnButton::clickFor(const juce::ModifierKeys& modifiers) const
{
    const auto status = m_learner.getStatus();
    const bool busy = status.activity != Activity::idle;

    // Alt: forget everything learned. With nothing learned yet, that is whatever is under way.
    if (modifiers.isAltDown())
        return { status.numRegions == 0 && busy ? Click::Intent::stop : Click::Intent::forget, learn::Method::Auto };

    if (busy)
        return { Click::Intent::stop, learn::Method::Auto };

    return { Click::Intent::learn, methodToLearn(status, *m_learner.getSnapshot()) };
}

void LearnButton::perform(const Click& click)
{
    // A click held back acts a moment after it was made. What it asked for may be over by then: what it would cancel
    // has finished (a click decided anew from LEARN as it is now would arm it instead), or something has started since,
    // such as METHOD in the view (learning now would cancel that). Then it does nothing.
    const bool busy = m_learner.getStatus().activity != Activity::idle;

    switch (click.intent)
    {
        case Click::Intent::forget:
            m_learner.forgetAll();
            break;

        case Click::Intent::stop:
            if (busy)
                m_learner.stopLearning();
            break;

        case Click::Intent::learn:
            if (! busy)
                m_learner.startLearning(click.method);
            break;
    }
}

void LearnButton::act(const Click& click)
{
    perform(click);
    refresh();
}

void LearnButton::actOnHeldClick()
{
    m_heldClickTimer.stopTimer();

    if (std::exchange(m_clickHeld, false))
        act(m_heldClick);
}

void LearnButton::toggleView()
{
    if (onToggleView)
        onToggleView();
}

void LearnButton::clicked(const juce::ModifierKeys& modifiers)
{
    if (m_secondClick)
        return;   // the second click of a double-click: mouseDoubleClick() answers it

    if (m_mouseClick)
    {
        // Held back until no second click can pair with it (JUCE counts presses up to the double-click time apart, and
        // this timer starts later, at the release). What it asks for is decided now, by what LEARN shows.
        m_mouseClicked = true;
        m_heldClick = clickFor(modifiers);
        m_clickHeld = true;
        m_heldClickTimer.startTimer(juce::MouseEvent::getDoubleClickTimeout());
        return;
    }

    // Return: no double-click can follow a key, so it acts at once (after a mouse click still held back, which came
    // first).
    actOnHeldClick();
    act(clickFor(modifiers));
}

bool LearnButton::keyPressed(const juce::KeyPress& key)
{
    // Shift-Return is the keyboard's double-click: it opens or closes the view.
    if (key.isKeyCode(juce::KeyPress::returnKey) && key.getModifiers().isShiftDown())
    {
        toggleView();
        return true;
    }

    return juce::TextButton::keyPressed(key);
}

void LearnButton::mouseDown(const juce::MouseEvent& event)
{
    // A right-click (a Ctrl-click on a Mac) asks for a menu, not for LEARN (as on the harmonic strip): it neither
    // presses nor clicks it, and is no half of a double-click.
    m_popupPress = event.mods.isPopupMenu();

    if (m_popupPress)
    {
        m_plainPresses = 0;
        return;
    }

    // JUCE counts a press close enough in time and place to the one before as the next of a multiple click (a
    // Ctrl-click before it included, or a press let go off LEARN): only plain presses that clicked count here (see
    // mouseUp()).
    m_plainPresses = event.getNumberOfClicks() >= 2 && m_plainPresses > 0 ? m_plainPresses + 1 : 1;

    // A click held back from before a press that ended without a release (never expected): it acts first.
    if (std::exchange(m_clickDropped, false))
        act(m_heldClick);

    if (m_clickHeld)
    {
        m_heldClickTimer.stopTimer();
        m_clickHeld = false;

        if (m_plainPresses >= 2)
            m_clickDropped = true;     // the second press of a double-click: the first click never acts ...
        else
            act(m_heldClick);          // a press too late (or too far) to pair: the click before it acts first
    }

    juce::TextButton::mouseDown(event);
}

void LearnButton::mouseDrag(const juce::MouseEvent& event)
{
    if (! m_popupPress)
        juce::TextButton::mouseDrag(event);   // (a right-drag would leave the pill drawn pressed)
}

void LearnButton::mouseUp(const juce::MouseEvent& event)
{
    if (std::exchange(m_popupPress, false))
        return;

    // The second click of a double-click is not a click of its own: mouseDoubleClick() follows it.
    m_secondClick = m_plainPresses >= 2 && event.getNumberOfClicks() >= 2;

    // ... unless the second press was held or dragged too long to make one (JUCE then counts one click): the first
    // click acts after all, and this one is a click of its own if it ends on LEARN.
    if (std::exchange(m_clickDropped, false) && ! m_secondClick)
        act(m_heldClick);

    m_mouseClick = true;
    m_mouseClicked = false;
    juce::TextButton::mouseUp(event);
    m_mouseClick = false;

    // Only a click can be the first half of a double-click. JUCE pairs presses by time and place alone, so after a
    // press let go off LEARN (no click) a click right after it would count as the second half, and open the view
    // instead of acting. A click of its own (held back now) is the first half of whatever pair comes next.
    if (! m_secondClick)
        m_plainPresses = m_mouseClicked ? 1 : 0;

    m_secondClick = false;
}

void LearnButton::mouseDoubleClick(const juce::MouseEvent& event)
{
    // The second of two plain clicks only: a third click has been answered, and a right-click is no half of a pair.
    if (m_plainPresses != 2 || event.getNumberOfClicks() < 2 || event.mods.isPopupMenu())
        return;

    toggleView();
}

std::unique_ptr<juce::AccessibilityHandler> LearnButton::createAccessibilityHandler()
{
    // As JUCE's own for a button (its label as the title, the tooltip, which follows LEARN's state, as help), but its
    // press is a plain click whatever keys are held (a screen reader holds keys of its own, never meant as Alt-click),
    // and "show menu" opens or closes the view.
    class Handler final : public juce::AccessibilityHandler
    {
    public:
        explicit Handler(LearnButton& button)
            : juce::AccessibilityHandler(button, juce::AccessibilityRole::button,
                                         juce::AccessibilityActions()
                                             .addAction(juce::AccessibilityActionType::press,
                                                        [&button] { button.actOnHeldClick(); button.act(button.clickFor({})); })
                                             .addAction(juce::AccessibilityActionType::showMenu, [&button] { button.toggleView(); }))
            , m_button(button)
        {
        }

        juce::String getTitle() const override
        {
            const auto title = juce::AccessibilityHandler::getTitle();
            return title.isNotEmpty() ? title : m_button.getButtonText();
        }

        juce::String getHelp() const override { return m_button.getTooltip(); }

    private:
        LearnButton& m_button;
    };

    return std::make_unique<Handler>(*this);
}

//==============================================================================
void LearnButton::paintButton(juce::Graphics& g, bool isMouseOverButton, bool isButtonDown)
{
    using namespace Theme::Colours;

    // The pill of LO CUT and LIMIT (SubbertoneLookAndFeel::drawButtonBackground), larger, with LEARN's states in it.
    // The outline breathes on the glow's clock, so it holds still with the glow; the pulse and the sweeping dot while
    // LEARN listens or works keep their own time.
    const double t = nowMs() * 0.001;
    const double breathTime = m_glow != nullptr ? m_glow->getClock() : t;
    const auto r = getLocalBounds().toFloat().reduced(1.0f);
    const float corner = r.getHeight() * 0.5f;
    const bool hot = isMouseOverButton && isEnabled();
    const float lift = isButtonDown ? 0.2f : hot ? 0.1f : 0.0f;

    auto edge = hot ? textDim : outline;
    float edgeWidth = 1.0f;
    auto label = Theme::Colours::text;   // (a Button has a member called text)
    float fill = 0.0f;                   // teal over the pill
    bool dimmed = false;

    switch (m_look)
    {
        case Look::off:
            // Nothing learned yet: the outline breathes red with the editor's glow.
            edge = bad.withAlpha(0.35f + 0.65f * LearnGlow::breath(LearnGlow::Mode::prompt, breathTime));
            edgeWidth = 1.5f;
            break;

        case Look::armed:
            edge = input.withAlpha(0.3f + 0.7f * breathe(breathTime));
            edgeWidth = 1.5f;
            break;

        case Look::capturing:
        case Look::analysing:
            fill = 0.1f + 0.22f * pulse(t);
            edge = input.withAlpha(0.7f);
            break;

        case Look::learnedHere:
            fill = 0.34f;   // lit, a little more than LO CUT and LIMIT when on, in their accents
            edge = input.withAlpha(hot ? 1.0f : 0.75f);
            break;

        case Look::learnedElsewhere:
            fill = 0.12f;   // half lit: something is learned, but nothing is followed here
            edge = hot ? textDim : input.withAlpha(0.25f);
            label = hot ? Theme::Colours::text : textDim;
            break;

        case Look::noTimeline:
            dimmed = true;
            label = textFaint;
            edge = outline.withMultipliedAlpha(0.6f);
            break;
    }

    if (m_uncertain)
    {
        edge = output.withAlpha(hot ? 1.0f : 0.85f);
        edgeWidth = 1.5f;
    }

    g.setColour(panelRaised.withMultipliedAlpha(dimmed ? 0.5f : 1.0f).brighter(lift));
    g.fillRoundedRectangle(r, corner);

    if (fill > 0.0f)
    {
        g.setColour(input.withAlpha(fill).brighter(lift));
        g.fillRoundedRectangle(r, corner);
    }

    // The state's outline, with the keyboard focus too (the stroke stays inside the pill whatever its width) ...
    const float inset = 0.5f * (edgeWidth - 1.0f);
    g.setColour(edge);
    g.drawRoundedRectangle(r.reduced(inset), corner - inset, edgeWidth);

    // ... and the focus as a ring just inside it, clear of the outline and in the text's colour: the editor's amber
    // focus ring, drawn on the outline, would hide it and look just like the amber edge of an ambiguous pitch. (Inside,
    // because the editor's glow along its top edge ends a pixel above the button: a ring around the pill would reach
    // into it.)
    if (SubbertoneLookAndFeel::shouldShowFocusRing(*this))
    {
        g.setColour(Theme::Colours::text);
        g.drawRoundedRectangle(r.reduced(c_focusRingInset), corner - c_focusRingInset, 1.0f);
    }

    // Label: centred, a little higher while the bar shows.
    const bool hasBar = m_bar > c_noBar;
    g.setFont(m_fonts->heading(c_labelHeight));
    g.setColour(label);
    g.drawText(getButtonText(), r.translated(0.0f, hasBar ? -2.0f : 0.0f), juce::Justification::centred, false);

    if (m_partlyInvalid)
    {
        // At the right end, clear of the label.
        g.setColour(bad);
        g.fillEllipse(juce::Rectangle<float>(7.0f, 7.0f).withCentre({ r.getRight() - corner, r.getCentreY() }));
    }

    if (hasBar)
    {
        // A 2 px bar along the bottom, inside the pill's straight part.
        const auto track = juce::Rectangle<float>(r.getX() + corner, r.getBottom() - 7.0f, r.getWidth() - 2.0f * corner, 2.0f);
        g.setColour(input.withAlpha(0.22f));
        g.fillRoundedRectangle(track, 1.0f);
        g.setColour(input);

        if (m_bar >= 0.0f)
        {
            if (m_bar > 0.0f)
                g.fillRoundedRectangle(track.withWidth(std::max(2.0f, track.getWidth() * m_bar)), 1.0f);
        }
        else
        {
            // The loop's length is not known yet: a dot sweeps to and fro.
            const float dot = std::min(12.0f, track.getWidth() * 0.25f);
            const auto travel = static_cast<float>(0.5 - 0.5 * std::cos(juce::MathConstants<double>::twoPi * t / c_sweepSeconds));
            g.fillRoundedRectangle(track.withWidth(dot).withX(track.getX() + travel * (track.getWidth() - dot)), 1.0f);
        }
    }
}

//==============================================================================
LearnPrompt::LearnPrompt(learn::Learner& learner)
    : m_learner(learner)
{
    setOpaque(false);
    setInterceptsMouseClicks(false, false);
}

LearnPrompt::~LearnPrompt() = default;

bool LearnPrompt::update(const LearnButton& button, const learn::Status& status)
{
    juce::String text, tip;
    juce::String kind;   // the text without its figures
    auto colour = Theme::Colours::input;
    bool breathes = false;
    bool undo = false;

    switch (button.getLook())
    {
        case LearnButton::Look::off:
            colour = Theme::Colours::bad;
            breathes = true;

            // A try to learn that failed after a forget is the newer news (it says what to do now, as LEARN's tooltip
            // does): UNDO stays in the view until its time runs out.
            if (status.outcome == learn::Status::Outcome::failed
                && status.failure == learn::Status::Failure::nothingPitched)
            {
                text = "NO PITCH HEARD: TRY AGAIN";
                tip = "Nothing in what was heard had a pitch to learn. Check the Threshold, then press LEARN and play the "
                      "loop once.";
            }
            else if (status.outcome == learn::Status::Outcome::failed
                     && status.failure == learn::Status::Failure::outOfMemory)
            {
                text = "OUT OF MEMORY: TRY AGAIN";
                tip = "There wasn't enough memory to listen to the loop. Press LEARN to try again.";
            }
            else if (status.outcome == learn::Status::Outcome::failed
                     && status.failure == learn::Status::Failure::loopTooShort)
            {
                text = "LOOP TOO SHORT (1 S MIN)";
                tip = "This loop is shorter than a second. LEARN needs a loop of at least one second: lengthen it in your "
                      "host, then press LEARN.";
            }
            else if (status.canUndo)
            {
                text = "FORGOTTEN: CLICK TO UNDO";
                tip = "Everything learned was just forgotten. Click here to bring it back (for 10 s).";
                colour = Theme::Colours::output;
                breathes = false;
                undo = true;
            }
            else
            {
                text = "PLEASE PRESS LEARN";
                tip = "Loop the part you want, press LEARN and play it once.";
            }

            kind = text;
            break;

        case LearnButton::Look::armed:
            text = kind = "PLAY THE LOOP ONCE";
            tip = "Start the loop in your host and let it play through once. Click LEARN to cancel.";
            break;

        case LearnButton::Look::capturing:
            kind = "LISTENING";
            text = kind + "  " + (status.progress >= 0.0f ? percent(status.progress) : seconds(status.heardSeconds).toUpperCase());
            tip = "Keep the loop playing until it has played through once (without a loop, stop after a second or more). "
                  "Loop the passage on your host's timeline, not in a clip launcher such as Live's Session View.";
            break;

        case LearnButton::Look::analysing:
            kind = "WORKING IT OUT";
            text = kind + "  " + percent(status.progress);
            tip = "Working out the pitch of what was heard, with hindsight. Click LEARN to cancel.";
            break;

        case LearnButton::Look::noTimeline:
            text = kind = "NEEDS A HOST TIMELINE";
            tip = noTimelineText(button.isStandaloneApp());
            colour = Theme::Colours::textFaint;
            break;

        case LearnButton::Look::learnedHere:
        case LearnButton::Look::learnedElsewhere:
            break;   // learned: nothing to ask for
    }

    if (tip != getTooltip())
        setTooltip(tip);

    if (text.isEmpty() != m_text.isEmpty() || undo != m_undo)
    {
        setInterceptsMouseClicks(text.isNotEmpty(), false);   // for its tooltip while it says something, and UNDO
        setMouseCursor(undo ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    }

    if (undo != m_undo)
    {
        // UNDO from the keyboard too, for as long as it is offered; once it is gone, the focus goes back to LEARN.
        setWantsKeyboardFocus(undo);

        if (! undo && hasKeyboardFocus(false))
            moveKeyboardFocusToSibling(false);
    }

    const bool changed = text != m_text || colour != m_colour || breathes != m_breathes || undo != m_undo;
    const bool textChanged = text != m_text;
    const bool undoChanged = undo != m_undo;
    m_text = text;
    m_colour = colour;
    m_breathes = breathes;
    m_undo = undo;

    // A screen reader: the role and the press come and go with UNDO; otherwise only the text changes.
    if (undoChanged)
        invalidateAccessibilityHandler();
    else if (textChanged)
        if (auto* handler = getAccessibilityHandler())
            handler->notifyAccessibilityEvent(juce::AccessibilityEvent::titleChanged);

    // It hears when what the prompt asks for changes (once, not with every percentage while LEARN listens or works).
    if (kind != m_kind)
    {
        if (m_shown && kind.isNotEmpty())
            juce::AccessibilityHandler::postAnnouncement(tip, juce::AccessibilityHandler::AnnouncementPriority::medium);

        m_kind = kind;
    }

    m_shown = true;
    return changed;
}

juce::Font LearnPrompt::fontFor(const juce::String& text) const
{
    // The heading face, as large as fits (a long prompt shrinks a little rather than being cut).
    const float width = static_cast<float>(getWidth());

    for (float height = c_promptHeight; height > c_promptMinHeight; height -= 0.5f)
    {
        const auto font = m_fonts->heading(height);

        if (juce::GlyphArrangement::getStringWidth(font, text) <= width)
            return font;
    }

    return m_fonts->heading(c_promptMinHeight);
}

void LearnPrompt::paint(juce::Graphics& g)
{
    if (m_text.isEmpty())
        return;

    auto colour = m_colour;
    const bool hot = m_undo && (isMouseOver() || SubbertoneLookAndFeel::shouldShowFocusRing(*this));

    if (m_breathes)
        colour = colour.withMultipliedAlpha(0.7f + 0.3f * LearnGlow::breath(LearnGlow::Mode::prompt,
                                                                            m_glow != nullptr ? m_glow->getClock() : nowMs() * 0.001));
    else if (hot)
        colour = colour.brighter(0.25f);

    const auto font = fontFor(m_text);
    const auto area = getLocalBounds().toFloat();
    g.setFont(font);
    g.setColour(colour);
    g.drawText(m_text, area, juce::Justification::centredLeft, true);

    if (hot)
    {
        // Underlined under the mouse, or with the keyboard focus, as the editor's links are.
        const float width = std::min(area.getWidth(), juce::GlyphArrangement::getStringWidth(font, m_text));
        g.fillRect(juce::Rectangle<float>(area.getX(), std::round(area.getCentreY() + font.getHeight() * 0.5f) + 1.0f, width, 1.0f));
    }
}

void LearnPrompt::mouseEnter(const juce::MouseEvent&)
{
    if (m_undo)
        repaint();
}

void LearnPrompt::mouseExit(const juce::MouseEvent&)
{
    if (m_undo)
        repaint();
}

void LearnPrompt::mouseUp(const juce::MouseEvent& event)
{
    // Decided where it is let go, as a button's click is: on the prompt, however long it was held or however much it
    // moved on the way. (JUCE's "dragged" is true of any press held 300 ms, so it would turn away a deliberate click.)
    // A right-click asks for a menu, not for UNDO.
    if (! m_undo || event.mods.isPopupMenu() || ! getLocalBounds().contains(event.getPosition()))
        return;

    undo();
}

bool LearnPrompt::keyPressed(const juce::KeyPress& key)
{
    if (m_undo && (key.isKeyCode(juce::KeyPress::returnKey) || key.isKeyCode(juce::KeyPress::spaceKey)))
    {
        undo();
        return true;
    }

    return false;
}

void LearnPrompt::focusGained(FocusChangeType)
{
    repaint();
}

void LearnPrompt::focusLost(FocusChangeType)
{
    repaint();
}

void LearnPrompt::undo()
{
    if (m_learner.undoForget() && onUndo)
        onUndo();
}

std::unique_ptr<juce::AccessibilityHandler> LearnPrompt::createAccessibilityHandler()
{
    // What it says, as text; while it offers UNDO, a button that brings back what was forgotten. Made again when UNDO
    // comes or goes (update()).
    class Handler final : public juce::AccessibilityHandler
    {
    public:
        explicit Handler(LearnPrompt& prompt)
            : juce::AccessibilityHandler(prompt, prompt.m_undo ? juce::AccessibilityRole::button : juce::AccessibilityRole::staticText,
                                         prompt.m_undo ? juce::AccessibilityActions().addAction(juce::AccessibilityActionType::press,
                                                                                                [&prompt] { prompt.undo(); })
                                                       : juce::AccessibilityActions())
            , m_prompt(prompt)
        {
        }

        juce::String getTitle() const override { return m_prompt.m_text; }
        juce::String getHelp() const override { return m_prompt.getTooltip(); }

        juce::AccessibleState getCurrentState() const override
        {
            // With nothing to say (something is learned) there is nothing to read.
            const auto state = juce::AccessibilityHandler::getCurrentState();
            return m_prompt.m_text.isEmpty() ? state.withIgnored() : state;
        }

    private:
        LearnPrompt& m_prompt;
    };

    return std::make_unique<Handler>(*this);
}

//==============================================================================
LearnGlow::LearnGlow(juce::Component& surface)
    : m_surface(surface)
{
}

LearnGlow::~LearnGlow()
{
    stop();
}

float LearnGlow::breath(Mode mode, double seconds) noexcept
{
    const double hz = mode == Mode::busy ? c_busyBreathHz : c_promptBreathHz;
    return static_cast<float>(0.5 - 0.5 * std::cos(juce::MathConstants<double>::twoPi * hz * seconds));
}

LearnGlow::Mode LearnGlow::modeFor(LearnButton::Look look) noexcept
{
    switch (look)
    {
        case LearnButton::Look::off:
            return Mode::prompt;

        case LearnButton::Look::armed:
        case LearnButton::Look::capturing:
        case LearnButton::Look::analysing:
            return Mode::busy;

        case LearnButton::Look::learnedHere:
        case LearnButton::Look::learnedElsewhere:
        case LearnButton::Look::noTimeline:
            break;
    }

    return Mode::off;
}

void LearnGlow::setMode(Mode mode)
{
    if (mode != m_mode)
    {
        m_mode = mode;

        if (mode != Mode::off)
        {
            m_edgesToClear = 0;
            start();
        }
    }

    // Faded out, and every edge cleared: let go of the display's frames (here, never from inside one of them).
    if (m_mode == Mode::off && m_level <= 0.0f && m_edgesToClear <= 0)
        stop();
}

void LearnGlow::start()
{
    if (isRunning())
        return;

    m_lastStepMs = -1.0;
    m_lastEdgeMs = -1.0;
    m_lastFrameMs = -1.0;
    m_frames = juce::VBlankAttachment(&m_surface, [this](double) { frame(nowMs()); });
}

void LearnGlow::stop()
{
    m_frames = juce::VBlankAttachment();
}

bool LearnGlow::drivesHeader() const noexcept
{
    return isRunning() && m_lastFrameMs >= 0.0 && nowMs() - m_lastFrameMs < c_framesLostMs;
}

bool LearnGlow::isHeld() const noexcept
{
    return isHeldAt(nowMs());
}

double LearnGlow::getClock() const noexcept
{
    return clockAt(nowMs());
}

bool LearnGlow::isHeldAt(double now) const noexcept
{
    return m_lastOtherRedrawMs >= 0.0 && now >= m_lastOtherRedrawMs && now - m_lastOtherRedrawMs < c_holdMs;
}

double LearnGlow::clockAt(double now) const noexcept
{
    // The hi-res counter less the time spent held: the latest hold counts up to now, or to its end.
    double held = m_heldBeforeMs;

    if (m_holdStartMs >= 0.0)
        held += std::max(0.0, std::min(now, m_lastOtherRedrawMs + c_holdMs) - m_holdStartMs);

    return (now - held) * 0.001;
}

void LearnGlow::noteOtherRedraw(double now) noexcept
{
    if (! isHeldAt(now))
    {
        // A new hold. The one before ended c_holdMs after the last redraw it saw.
        if (m_holdStartMs >= 0.0)
            m_heldBeforeMs += std::max(0.0, m_lastOtherRedrawMs + c_holdMs - m_holdStartMs);

        m_holdStartMs = now;
    }

    m_lastOtherRedrawMs = now;
}

void LearnGlow::setLayout(juce::Rectangle<int> edges)
{
    if (edges == m_edges)
        return;

    for (int edge = 0; edge < numEdges; ++edge)
        m_surface.repaint(getEdgeArea(edge));   // where it was

    m_edges = edges;

    for (int edge = 0; edge < numEdges; ++edge)
        m_surface.repaint(getEdgeArea(edge));
}

juce::Rectangle<int> LearnGlow::getEdgeArea(int edge) const noexcept
{
    if (m_edges.isEmpty())
        return {};

    // The top and bottom edges take the corners; the sides run between them, so every pixel is one edge's.
    const int width = static_cast<int>(std::ceil(c_edgeWidth));

    switch (edge)
    {
        case top:    return m_edges.withHeight(width);
        case bottom: return m_edges.withTop(m_edges.getBottom() - width);
        case left:   return m_edges.withWidth(width).reduced(0, width);
        case right:  return m_edges.withLeft(m_edges.getRight() - width).reduced(0, width);
        default:     break;
    }

    return {};
}

int LearnGlow::frame(double now)
{
    m_lastFrameMs = now;
    advanceTo(now);

    if (m_edges.isEmpty() || (m_level <= 0.0f && m_edgesToClear <= 0 && m_edgesToRefresh <= 0))
        return -1;   // nothing shows, and nothing is left to clear

    // Held: an edge is repainted only to show a change (a new mode, or the glow gone), never to breathe.
    const bool held = isHeldAt(now);

    if (held && m_edgesToClear <= 0 && m_edgesToRefresh <= 0)
        return -1;

    // One edge per frame, in turn, and none more often than c_maxEdgeHz (frames come at the display's rate, which
    // jitters a little: hence the slack).
    const double interval = 1000.0 / static_cast<double>(c_maxEdgeHz * numEdges);

    if (m_lastEdgeMs >= 0.0 && now >= m_lastEdgeMs && now - m_lastEdgeMs < 0.8 * interval)
        return -1;

    // The edge before has not been drawn yet (the system is behind): asked for now, this one would be drawn with it,
    // in one rectangle around both. Not for ever, though: a window that is not drawn at all never says.
    if (m_pendingEdge >= 0 && now >= m_lastEdgeMs && now - m_lastEdgeMs < c_pendingTimeoutMs)
        return -1;

    m_lastEdgeMs = now;
    const int edge = m_nextEdge;
    m_nextEdge = (m_nextEdge + 1) % numEdges;
    m_pendingEdge = edge;
    m_surface.repaint(getEdgeArea(edge));

    // (Held, the button and the prompt do not breathe: the editor repaints them when what they show changes.)
    if (edge == top && onHeaderFrame && ! held)
        onHeaderFrame();

    if (m_edgesToClear > 0)
        --m_edgesToClear;

    if (m_edgesToRefresh > 0)
        --m_edgesToRefresh;

    return edge;
}

void LearnGlow::advanceTo(double now)
{
    const double dt = m_lastStepMs >= 0.0 ? juce::jlimit(0.0, 0.25, (now - m_lastStepMs) * 0.001) : 0.0;
    m_lastStepMs = now;

    // The breath of the mode (everything red breathes together: the button and the prompt use the same clock), and
    // its presence, which fades in and out.
    const double t = clockAt(now);
    float breathLevel = m_breath;
    auto colour = m_colour;

    switch (m_mode)
    {
        case Mode::prompt:
            breathLevel = 0.3f + 0.7f * breath(Mode::prompt, t);
            colour = Theme::Colours::bad;
            break;

        case Mode::busy:
            breathLevel = 0.3f + 0.25f * breath(Mode::busy, t);
            colour = Theme::Colours::input;
            break;

        case Mode::off:
            break;
    }

    const float present = m_mode != Mode::off ? 1.0f : 0.0f;

    if (isHeldAt(now))
    {
        // Held, nothing moves. What must show (another mode's colour, the glow coming or going) is taken at once, and
        // each edge is repainted once to show it.
        const bool otherColour = std::abs(colour.getRed() - m_colour.getRed()) > c_otherModeColour
                              || std::abs(colour.getGreen() - m_colour.getGreen()) > c_otherModeColour
                              || std::abs(colour.getBlue() - m_colour.getBlue()) > c_otherModeColour;

        if ((m_mode != Mode::off && otherColour) || std::abs(present - m_presence) > 0.01f)
        {
            if (m_mode != Mode::off)
            {
                m_breath = breathLevel;
                m_colour = colour;
            }

            m_presence = present;
            m_edgesToRefresh = numEdges;
        }
    }
    else
    {
        if (m_presence <= 0.0f)
        {
            // Appearing: in this mode's breath and colour from the start.
            m_breath = breathLevel;
            m_colour = colour;
        }
        else
        {
            m_breath += (breathLevel - m_breath) * smoothingStep(dt, c_breathSmoothSeconds);
            m_colour = m_colour.interpolatedWith(colour, smoothingStep(dt, c_colourSeconds));
        }

        m_presence += (present - m_presence) * smoothingStep(dt, m_mode != Mode::off ? c_fadeInSeconds : c_fadeOutSeconds);
    }

    if (m_mode == Mode::off && m_presence < 0.01f)
        m_presence = 0.0f;

    const bool showed = m_level > 0.0f;
    m_level = m_presence * m_breath;

    // Faded out: each edge is repainted once more, so none keeps the last of the glow.
    if (showed && m_level <= 0.0f)
        m_edgesToClear = numEdges;

    if (onFrameColour)
        onFrameColour(frameColour());
}

juce::Colour LearnGlow::frameColour() const noexcept
{
    return m_level > 0.0f ? m_colour.withAlpha(juce::jlimit(0.0f, 1.0f, m_level * c_frameAlpha)) : juce::Colours::transparentBlack;
}

void LearnGlow::paint(juce::Graphics& g)
{
    const auto clip = g.getClipBounds();

    // A redraw reaching beyond what the glow asked for (the edge it repainted, and in the top edge's frames LEARN's
    // button and prompt) is the editor at work elsewhere, or drawn in one with it: hold still. (Only while the display's
    // frames drive the glow: without them no edge is repainted, so there is nothing to hold.)
    if (drivesHeader())
    {
        auto own = m_pendingEdge >= 0 ? getEdgeArea(m_pendingEdge) : juce::Rectangle<int>();

        if (m_pendingEdge == top)
            own = own.getUnion(m_header);

        if (own.isEmpty() || ! own.expanded(c_ownSlack).contains(clip))
            noteOtherRedraw(nowMs());
    }

    // Drawn: the next edge may be asked for.
    if (m_pendingEdge >= 0 && clip.intersects(getEdgeArea(m_pendingEdge)))
        m_pendingEdge = -1;

    if (m_level <= 0.0f || m_edges.isEmpty())
        return;

    // Along the editor's edges, fading inwards: one 1 px ring per step, so the corners fade as evenly as the sides.
    // (Only the edge being repainted is inside the clip: the rest costs nothing.)
    const auto edges = m_edges.toFloat();

    for (int i = 0; i < static_cast<int>(c_edgeWidth); ++i)
    {
        const float d = (static_cast<float>(i) + 0.5f) / c_edgeWidth;
        g.setColour(m_colour.withAlpha(juce::jlimit(0.0f, 1.0f, c_edgeAlpha * (1.0f - d) * (1.0f - d) * m_level)));
        g.drawRect(edges.reduced(static_cast<float>(i)), 1.0f);
    }
}
