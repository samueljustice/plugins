#include "Controls.h"

#include "Theme.h"

namespace tt::ui
{

//==============================================================================
PillButton::PillButton (const juce::String& text, bool isPrimaryButton)
    : juce::Button (text), primary (isPrimaryButton)
{
    setButtonText (text);
    setTitle (text);
    // A click leaves the keyboard focus where it was, so the window's shortcuts keep working.
    // The button can still be reached with the keyboard.
    setMouseClickGrabsKeyboardFocus (false);
}

void PillButton::setPrimary (bool shouldBePrimary)
{
    if (primary != shouldBePrimary)
    {
        primary = shouldBePrimary;
        repaint();
    }
}

int PillButton::getBestWidth() const
{
    return (int) std::ceil (Theme::textWidth (Theme::ui (13.0f, true), getButtonText())) + 30;
}

void PillButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
    const float radius = juce::jmin (7.0f, bounds.getHeight() * 0.5f);
    const float alpha = isEnabled() ? 1.0f : 0.4f;

    auto fill = primary ? Theme::pin() : Theme::panel();
    auto border = primary ? Theme::pin() : Theme::line();
    const auto ink = primary ? Theme::inkOnPin() : Theme::text();

    if (isEnabled())
    {
        if (down)
        {
            fill = primary ? fill.darker (0.18f) : Theme::line();
            border = primary ? fill : Theme::dim();
        }
        else if (highlighted)
        {
            fill = primary ? fill.brighter (0.25f) : fill.brighter (0.08f);
            border = primary ? fill : Theme::dim().withAlpha (0.6f);
        }
    }

    g.setColour (fill.withMultipliedAlpha (alpha));
    g.fillRoundedRectangle (bounds, radius);
    g.setColour (border.withMultipliedAlpha (alpha));
    g.drawRoundedRectangle (bounds, radius, 1.0f);

    if (hasKeyboardFocus (false))
    {
        g.setColour ((primary ? Theme::inkOnPin() : Theme::text()).withAlpha (0.75f));
        g.drawRoundedRectangle (bounds.reduced (2.0f), juce::jmax (1.0f, radius - 2.0f), 1.0f);
    }

    g.setColour (ink.withMultipliedAlpha (alpha));
    g.setFont (Theme::ui (13.0f, true));
    g.drawText (getButtonText(), getLocalBounds().reduced (juce::jmin (10, getWidth() / 6), 0),
                juce::Justification::centred, true);
}

//==============================================================================
SegmentedSwitch::SegmentedSwitch()
{
    setWantsKeyboardFocus (true);
    setMouseClickGrabsKeyboardFocus (false);
    setDescription ("Choose one. The left and right arrow keys change the choice.");
}

void SegmentedSwitch::setItems (juce::StringArray newItems)
{
    items = std::move (newItems);
    itemEnabled.assign ((size_t) items.size(), true);
    selected = items.isEmpty() ? -1 : juce::jlimit (0, items.size() - 1, selected);
    hovered = -1;
    repaint();
}

void SegmentedSwitch::setSelected (int index, juce::NotificationType notification)
{
    if (index < 0 || index >= items.size() || index == selected)
        return;

    selected = index;
    repaint();

    if (notification != juce::dontSendNotification && onChange)
        onChange (selected);
}

void SegmentedSwitch::setItemEnabled (int index, bool shouldBeEnabled)
{
    if (index < 0 || index >= (int) itemEnabled.size() || itemEnabled[(size_t) index] == shouldBeEnabled)
        return;

    itemEnabled[(size_t) index] = shouldBeEnabled;
    repaint();
}

bool SegmentedSwitch::isItemEnabled (int index) const
{
    return isEnabled() && index >= 0 && index < (int) itemEnabled.size() && itemEnabled[(size_t) index];
}

int SegmentedSwitch::getBestWidth() const
{
    const auto font = Theme::ui (13.0f, true);
    float total = 2.0f;

    for (const auto& item : items)
        total += std::ceil (Theme::textWidth (font, item)) + 22.0f;

    return (int) std::ceil (total);
}

std::vector<juce::Rectangle<float>> SegmentedSwitch::segmentBounds() const
{
    std::vector<juce::Rectangle<float>> result;

    if (items.isEmpty())
        return result;

    const auto font = Theme::ui (13.0f, true);
    std::vector<float> natural;
    float total = 0.0f;

    for (const auto& item : items)
    {
        natural.push_back (Theme::textWidth (font, item) + 22.0f);
        total += natural.back();
    }

    // Share the width in proportion to each item's text, wider or narrower as the bounds allow.
    const auto bounds = getLocalBounds().toFloat();
    const float scale = total > 0.0f ? bounds.getWidth() / total : 1.0f;
    float x = bounds.getX();

    for (size_t i = 0; i < natural.size(); ++i)
    {
        const float w = natural[i] * scale;
        result.emplace_back (x, bounds.getY(), w, bounds.getHeight());
        x += w;
    }

    return result;
}

int SegmentedSwitch::segmentAt (juce::Point<float> p) const
{
    const auto segments = segmentBounds();

    for (size_t i = 0; i < segments.size(); ++i)
        if (segments[i].contains (p))
            return (int) i;

    return -1;
}

void SegmentedSwitch::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    const float radius = juce::jmin (Theme::cornerRadius, bounds.getHeight() * 0.5f);
    const float alpha = isEnabled() ? 1.0f : 0.4f;
    const auto segments = segmentBounds();

    {
        juce::Graphics::ScopedSaveState state (g);
        juce::Path outline;
        outline.addRoundedRectangle (bounds, radius);
        g.reduceClipRegion (outline);

        g.setColour (Theme::panel().withMultipliedAlpha (alpha));
        g.fillRect (bounds);

        for (size_t i = 0; i < segments.size(); ++i)
        {
            const int index = (int) i;
            const bool on = index == selected;
            const bool usable = isItemEnabled (index);

            if (on)
            {
                g.setColour (Theme::wave().withMultipliedAlpha (usable ? 1.0f : 0.4f));
                g.fillRect (segments[i]);
            }
            else if (usable && index == hovered)
            {
                g.setColour (Theme::line().withAlpha (0.6f));
                g.fillRect (segments[i]);
            }

            if (i > 0 && index != selected && index - 1 != selected)
            {
                g.setColour (Theme::line().withMultipliedAlpha (alpha));
                g.fillRect (juce::Rectangle<float> (std::round (segments[i].getX()), bounds.getY() + 5.0f,
                                                    1.0f, juce::jmax (0.0f, bounds.getHeight() - 10.0f)));
            }

            auto colour = on ? Theme::inkOnWave() : (index == hovered && usable ? Theme::text() : Theme::dim());
            g.setColour (colour.withMultipliedAlpha (usable ? 1.0f : 0.4f));
            g.setFont (Theme::ui (13.0f, on));
            g.drawText (items[index], segments[i].reduced (juce::jmin (8.0f, segments[i].getWidth() * 0.15f), 0.0f),
                        juce::Justification::centred, true);
        }
    }

    g.setColour ((hasKeyboardFocus (false) ? Theme::dim() : Theme::line()).withMultipliedAlpha (alpha));
    g.drawRoundedRectangle (bounds.reduced (0.5f), radius, 1.0f);
}

void SegmentedSwitch::mouseMove (const juce::MouseEvent& e)
{
    const int now = segmentAt (e.position);

    if (now != hovered)
    {
        hovered = now;
        repaint();
    }
}

void SegmentedSwitch::mouseExit (const juce::MouseEvent&)
{
    if (hovered != -1)
    {
        hovered = -1;
        repaint();
    }
}

void SegmentedSwitch::mouseDown (const juce::MouseEvent& e)
{
    const int index = segmentAt (e.position);

    if (isItemEnabled (index))
        setSelected (index, juce::sendNotification);
}

void SegmentedSwitch::step (int direction)
{
    for (int i = selected + direction; i >= 0 && i < items.size(); i += direction)
    {
        if (isItemEnabled (i))
        {
            setSelected (i, juce::sendNotification);
            return;
        }
    }
}

bool SegmentedSwitch::keyPressed (const juce::KeyPress& key)
{
    if (key.isKeyCode (juce::KeyPress::leftKey))   { step (-1); return true; }
    if (key.isKeyCode (juce::KeyPress::rightKey))  { step (1);  return true; }

    return false;
}

void SegmentedSwitch::focusGained (FocusChangeType)  { repaint(); }
void SegmentedSwitch::focusLost (FocusChangeType)    { repaint(); }
void SegmentedSwitch::enablementChanged()            { repaint(); }

//==============================================================================
NumberBox::NumberBox (const juce::String& labelText)
    : label (labelText)
{
    setTitle (labelText);
    setDescription ("Click to type a value. The up and down arrow keys and the mouse wheel change it by one step.");
    setWantsKeyboardFocus (true);
    setMouseClickGrabsKeyboardFocus (false);

    addChildComponent (editor);
    editor.setTitle (labelText);
    editor.setFont (Theme::mono (13.0f));
    editor.setJustification (juce::Justification::centredLeft);
    editor.setBorder ({});
    editor.setIndents (0, 0);
    editor.setSelectAllWhenFocused (true);
    editor.setInputRestrictions (14, "0123456789.,-");
    editor.setColour (juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
    editor.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    editor.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
    editor.setColour (juce::TextEditor::textColourId, Theme::text());
    editor.setColour (juce::TextEditor::highlightColourId, Theme::wave().withAlpha (0.45f));
    editor.setColour (juce::TextEditor::highlightedTextColourId, Theme::text());
    editor.setColour (juce::CaretComponent::caretColourId, Theme::pin());

    editor.onReturnKey = [this] { endEdit (true); };
    editor.onEscapeKey = [this] { endEdit (false); };
    editor.onFocusLost = [this] { endEdit (true); };
}

juce::Rectangle<int> NumberBox::boxBounds() const
{
    auto area = getLocalBounds();

    // With too little height for both, the label is left out and the box takes it all.
    if (area.getHeight() >= 36)
        area.removeFromTop (18);

    return area;
}

juce::String NumberBox::valueText() const
{
    if (rangeDecimals <= 0)
        return juce::String ((juce::int64) std::llround (value));

    return juce::String (value, rangeDecimals);
}

double NumberBox::constrain (double v) const
{
    if (rangeStep > 0.0)
        v = rangeLo + std::round ((v - rangeLo) / rangeStep) * rangeStep;

    return juce::jlimit (rangeLo, rangeHi, v);
}

void NumberBox::setRange (double lo, double hi, double stepSize, int decimals)
{
    rangeLo = juce::jmin (lo, hi);
    rangeHi = juce::jmax (lo, hi);
    rangeStep = juce::jmax (0.0, stepSize);
    rangeDecimals = juce::jlimit (0, 6, decimals);
    value = juce::jlimit (rangeLo, rangeHi, value);
    repaint();
}

void NumberBox::setValue (double newValue, juce::NotificationType notification)
{
    newValue = juce::jlimit (rangeLo, rangeHi, newValue);
    const bool changed = ! juce::approximatelyEqual (newValue, value) || placeholder.isNotEmpty();

    value = newValue;
    placeholder.clear();
    repaint();

    if (changed && notification != juce::dontSendNotification && onChange)
        onChange (value);
}

void NumberBox::setText (const juce::String& placeholderText)
{
    if (placeholder != placeholderText)
    {
        placeholder = placeholderText;
        repaint();
    }
}

void NumberBox::step (int direction)
{
    if (! isEnabled())
        return;

    if (placeholder.isNotEmpty() && stepStart > 0.0)
    {
        setValue (constrain (stepStart), juce::sendNotification);
        return;
    }

    const double amount = rangeStep > 0.0 ? rangeStep : 1.0;
    setValue (constrain (value + amount * (double) direction), juce::sendNotification);
}

void NumberBox::beginEdit()
{
    if (editing || ! isEnabled())
        return;

    editing = true;
    editor.setText (placeholder.isNotEmpty() ? juce::String() : valueText(), false);
    resized();
    editor.setVisible (true);
    editor.grabKeyboardFocus();
    editor.selectAll();
    repaint();
}

void NumberBox::endEdit (bool commit)
{
    if (! editing)
        return;

    // Cleared first: hiding the editor takes its focus away, which calls back into here.
    editing = false;
    const auto typed = editor.getText().trim().replaceCharacter (',', '.');
    editor.setVisible (false);
    repaint();

    if (commit && typed.isNotEmpty() && typed.containsAnyOf ("0123456789"))
        setValue (constrain (typed.getDoubleValue()), juce::sendNotification);
}

void NumberBox::paint (juce::Graphics& g)
{
    const float alpha = isEnabled() ? 1.0f : 0.4f;
    const auto box = boxBounds();

    if (box.getY() > 0)
    {
        g.setColour (Theme::dim().withMultipliedAlpha (alpha));
        g.setFont (Theme::fieldLabel());
        g.drawText (label.toUpperCase(), getLocalBounds().removeFromTop (14),
                    juce::Justification::centredLeft, true);
    }

    const auto r = box.toFloat().reduced (0.5f);
    const float radius = juce::jmin (Theme::cornerRadius, r.getHeight() * 0.5f);

    g.setColour (Theme::panel().withMultipliedAlpha (alpha));
    g.fillRoundedRectangle (r, radius);
    g.setColour ((editing ? Theme::pin() : (hasKeyboardFocus (false) ? Theme::dim() : Theme::line()))
                     .withMultipliedAlpha (alpha));
    g.drawRoundedRectangle (r, radius, 1.0f);

    if (! editing)
    {
        const bool showPlaceholder = placeholder.isNotEmpty();
        g.setColour ((showPlaceholder ? Theme::dim() : Theme::text()).withMultipliedAlpha (alpha));
        g.setFont (Theme::mono (13.0f));
        g.drawText (showPlaceholder ? placeholder : valueText(), box.reduced (juce::jmin (10, box.getWidth() / 6), 0),
                    juce::Justification::centredLeft, true);
    }
}

void NumberBox::resized()
{
    const auto box = boxBounds();
    editor.setBounds (box.reduced (juce::jmin (10, box.getWidth() / 6), 2));
}

void NumberBox::mouseUp (const juce::MouseEvent& e)
{
    if (e.mouseWasClicked() && ! e.mods.isPopupMenu() && boxBounds().contains (e.getPosition()))
        beginEdit();
}

void NumberBox::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (editing || ! isEnabled() || std::abs (wheel.deltaY) < std::abs (wheel.deltaX))
    {
        juce::Component::mouseWheelMove (e, wheel);
        return;
    }

    if (wheel.isInertial)
        return;

    if (! wheel.isSmooth)
    {
        if (! juce::exactlyEqual (wheel.deltaY, 0.0f))
            step (wheel.deltaY > 0.0f ? 1 : -1);

        return;
    }

    // A trackpad sends many small movements: one step for each 0.08 of travel.
    wheelRemainder += wheel.deltaY;

    while (std::abs (wheelRemainder) >= 0.08f)
    {
        const int direction = wheelRemainder > 0.0f ? 1 : -1;
        wheelRemainder -= 0.08f * (float) direction;
        step (direction);
    }
}

bool NumberBox::keyPressed (const juce::KeyPress& key)
{
    if (editing)
        return false;

    if (key.isKeyCode (juce::KeyPress::upKey))      { step (1);    return true; }
    if (key.isKeyCode (juce::KeyPress::downKey))    { step (-1);   return true; }
    if (key.isKeyCode (juce::KeyPress::returnKey))  { beginEdit(); return true; }

    return false;
}

void NumberBox::focusGained (FocusChangeType)  { repaint(); }
void NumberBox::focusLost (FocusChangeType)    { repaint(); }
void NumberBox::enablementChanged()            { repaint(); }

//==============================================================================
CheckBadge::CheckBadge()
{
    setTitle ("Places to check");
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void CheckBadge::setCount (int numberOfChecks)
{
    numberOfChecks = juce::jmax (0, numberOfChecks);

    if (count != numberOfChecks)
    {
        count = numberOfChecks;
        setDescription (text());
        repaint();
    }
}

juce::String CheckBadge::text() const
{
    if (count == 0)
        return "Nothing to check";

    return juce::String (count) + (count == 1 ? " place to check" : " places to check");
}

int CheckBadge::getBestWidth() const
{
    return (int) std::ceil (Theme::textWidth (Theme::ui (13.0f, true), text())) + 16;
}

void CheckBadge::paint (juce::Graphics& g)
{
    const float alpha = isEnabled() ? 1.0f : 0.4f;
    const auto colour = (count > 0 ? Theme::check() : Theme::ok()).withMultipliedAlpha (alpha);
    auto area = getLocalBounds();

    g.setColour (colour);
    g.fillEllipse (juce::Rectangle<float> (8.0f, 8.0f)
                       .withCentre ({ (float) area.getX() + 4.0f, (float) area.getCentreY() }));
    area.removeFromLeft (14);

    g.setFont (Theme::ui (13.0f, true));
    g.drawText (text(), area, juce::Justification::centredLeft, true);
}

void CheckBadge::mouseUp (const juce::MouseEvent& e)
{
    if (isEnabled() && e.mouseWasClicked() && onClick)
        onClick();
}

} // namespace tt::ui
