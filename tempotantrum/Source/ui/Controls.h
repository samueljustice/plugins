#pragma once

#include <JuceHeader.h>

#include <functional>
#include <vector>

// The small custom controls of the window.
namespace tt::ui
{

// A rounded button. The primary one is filled with the pin colour.
class PillButton : public juce::Button
{
public:
    PillButton (const juce::String& text, bool primary = false);

    void setPrimary (bool shouldBePrimary);
    bool isPrimary() const                       { return primary; }

    // The width that shows the whole text.
    int getBestWidth() const;

    void paintButton (juce::Graphics&, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

private:
    bool primary = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PillButton)
};

// A row of joined segments of which one is selected.
class SegmentedSwitch : public juce::Component
{
public:
    SegmentedSwitch();

    void setItems (juce::StringArray newItems);
    void setSelected (int index, juce::NotificationType notification);
    int getSelected() const                      { return selected; }
    void setItemEnabled (int index, bool shouldBeEnabled);

    // The width that shows every item's whole text.
    int getBestWidth() const;

    std::function<void (int)> onChange;

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;
    void focusGained (FocusChangeType) override;
    void focusLost (FocusChangeType) override;
    void enablementChanged() override;

private:
    std::vector<juce::Rectangle<float>> segmentBounds() const;
    int segmentAt (juce::Point<float>) const;
    bool isItemEnabled (int index) const;
    void step (int direction);

    juce::StringArray items;
    std::vector<bool> itemEnabled;
    int selected = -1;
    int hovered = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SegmentedSwitch)
};

// A number with a label above it. Click to type a value; Return commits, Escape cancels.
// The up and down arrows and the mouse wheel change the value by one step.
class NumberBox : public juce::Component
{
public:
    explicit NumberBox (const juce::String& label);

    void setValue (double newValue, juce::NotificationType notification);
    double getValue() const                      { return value; }
    void setRange (double lo, double hi, double step, int decimals);

    // Shows this text in place of the number (for example "auto") until a value is set or typed.
    // An empty string shows the number again.
    void setText (const juce::String& placeholderText);

    // Lets the box be left empty: when everything typed in it is deleted, this text is shown in
    // place of a number and hasValue() is false.
    void setEmptyText (const juce::String& textWhenEmpty);
    bool hasValue() const                        { return placeholder.isEmpty(); }

    // While that text is shown there is no number to step from: the first step with the arrow
    // keys or the wheel goes to this value instead (zero: step from the range's lowest value).
    void setStepStart (double valueToStartFrom)   { stepStart = valueToStartFrom; }

    std::function<void (double)> onChange;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed (const juce::KeyPress&) override;
    void focusGained (FocusChangeType) override;
    void focusLost (FocusChangeType) override;
    void enablementChanged() override;

private:
    juce::Rectangle<int> boxBounds() const;
    juce::String valueText() const;
    double constrain (double) const;
    void step (int direction);
    void beginEdit();
    void endEdit (bool commit);

    juce::String label;
    juce::String placeholder;
    juce::String emptyText;
    double value = 0.0;
    double rangeLo = 0.0, rangeHi = 1.0e9, rangeStep = 1.0;
    int rangeDecimals = 0;
    double stepStart = 0.0;
    float wheelRemainder = 0.0f;
    bool editing = false;
    juce::TextEditor editor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NumberBox)
};

// One line saying how many places need a look: "2 places to check", or "Nothing to check".
// Clicking it calls onClick.
class CheckBadge : public juce::Component
{
public:
    CheckBadge();

    void setCount (int numberOfChecks);
    int getCount() const                         { return count; }

    // The width that shows the whole line.
    int getBestWidth() const;

    std::function<void()> onClick;

    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    juce::String text() const;

    int count = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CheckBadge)
};

} // namespace tt::ui
