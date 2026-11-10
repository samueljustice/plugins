#include "UpdatePopup.h"
#include "Controls.h"

namespace
{
    constexpr int c_cardWidth = 440;
    constexpr int c_cardHeight = 164;
    constexpr int c_buttonHeight = 28;
}

UpdatePopup::UpdatePopup(const Theme::Fonts& fonts)
    : m_fonts(fonts)
{
    using namespace Theme::Colours;

    // Pills like the About panel's CHECK FOR UPDATES, labels in capitals like the rest of the chrome.
    const auto setUp = [this](juce::TextButton& button, const juce::String& caption, int focusOrder)
    {
        button.setButtonText(caption);
        Controls::LookAndFeel::setLabelDrawnInCapitals(button);
        button.setExplicitFocusOrder(focusOrder);
        addAndMakeVisible(button);
    };

    setUp(m_downloadButton, "Download", 1);
    setUp(m_laterButton, "Later", 2);
    setUp(m_skipButton, "Skip this version", 3);

    // DOWNLOAD is the one to press: the logo blue, tinted, with bright text.
    m_downloadButton.setColour(juce::TextButton::buttonColourId, reverser.withAlpha(0.24f));
    m_downloadButton.setColour(juce::TextButton::textColourOffId, text);

    m_downloadButton.onClick = [this]
    {
        m_info.openDownloadPage();
        close();
    };
    m_laterButton.onClick = [this] { close(); };
    m_skipButton.onClick = [this]
    {
        sammyj::UpdateCheck::skipVersion(m_info);
        close();
    };

    // While the card is up, Tab cycles through its own buttons only (not the controls behind the scrim).
    setFocusContainerType(juce::Component::FocusContainerType::keyboardFocusContainer);
    setWantsKeyboardFocus(true);
    setInterceptsMouseClicks(true, true);
    setVisible(false);
}

void UpdatePopup::show(const sammyj::UpdateInfo& info)
{
    m_info = info;
    // Only remember focus inside this editor (never another plug-in's window).
    auto* focused = juce::Component::getCurrentlyFocusedComponent();
    m_focusBeforeShow = (getParentComponent() != nullptr && getParentComponent()->isParentOf(focused) && ! isParentOf(focused)) ? focused : nullptr;
    setVisible(true);
    toFront(false);
    repaint();
    if (isShowing())
        m_downloadButton.grabKeyboardFocus();   // its ring shows only after Tab
}

void UpdatePopup::close()
{
    setVisible(false);

    // Hand focus back to whatever had it (the About card's button, say) so Tab and Escape keep working.
    if (auto* previous = m_focusBeforeShow.getComponent(); previous != nullptr && previous->isShowing() && previous->getWantsKeyboardFocus())
        previous->grabKeyboardFocus();
    m_focusBeforeShow = nullptr;
}

juce::Rectangle<int> UpdatePopup::cardBounds() const
{
    return getLocalBounds().withSizeKeepingCentre(c_cardWidth, std::min(c_cardHeight, getHeight() - 24));
}

void UpdatePopup::mouseUp(const juce::MouseEvent& e)
{
    // A click outside the card is LATER, as a click outside the About card closes it.
    if (! cardBounds().contains(e.getPosition()))
        close();
}

bool UpdatePopup::keyPressed(const juce::KeyPress& key)
{
    if (key.isKeyCode(juce::KeyPress::escapeKey))
    {
        close();
        return true;
    }

    // Return on a focused button presses that button (JUCE handles it there); anywhere else it is DOWNLOAD.
    if (key.isKeyCode(juce::KeyPress::returnKey))
    {
        m_downloadButton.triggerClick();
        return true;
    }

    return false;
}

void UpdatePopup::paint(juce::Graphics& g)
{
    using namespace Theme::Colours;

    g.fillAll(juce::Colours::black.withAlpha(0.6f));

    const auto card = cardBounds();
    const auto r = card.toFloat();
    g.setColour(panel);
    g.fillRoundedRectangle(r, 10.0f);
    g.setColour(outline);
    g.drawRoundedRectangle(r.reduced(0.5f), 10.0f, 1.0f);

    auto content = card.reduced(24, 0);

    // The header's backwards arrow, small, beside the title.
    auto titleRow = juce::Rectangle<int>(content.getX(), card.getY() + 24, content.getWidth(), 26);
    {
        const auto mark = titleRow.removeFromLeft(22).toFloat().withSizeKeepingCentre(22.0f, 13.0f);
        juce::Path arrow;
        arrow.addTriangle(mark.getX(), mark.getCentreY(), mark.getX() + 7.0f, mark.getY() + 1.0f, mark.getX() + 7.0f, mark.getBottom() - 1.0f);
        g.setColour(reverser);
        g.fillPath(arrow);

        juce::Path tail;
        tail.startNewSubPath(mark.getX() + 6.0f, mark.getCentreY());
        tail.lineTo(mark.getRight() - 3.0f, mark.getCentreY());
        tail.quadraticTo(mark.getRight(), mark.getCentreY(), mark.getRight(), mark.getCentreY() + 3.0f);
        g.strokePath(tail, juce::PathStrokeType(1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        titleRow.removeFromLeft(10);
    }

    g.setColour(text);
    g.setFont(m_fonts.heading(16.0f));
    g.drawFittedText("Reversinator " + m_info.latestVersion + " is available", titleRow, juce::Justification::centredLeft, 1, 0.8f);

    g.setColour(textDim);
    g.setFont(m_fonts.body(13.0f));
    g.drawFittedText("You have " + m_info.currentVersion + ".",
                     juce::Rectangle<int>(content.getX(), card.getY() + 58, content.getWidth(), 20),
                     juce::Justification::centredLeft, 1, 1.0f);

    g.setColour(outlineSoft);
    g.drawHorizontalLine(card.getBottom() - 64, static_cast<float>(card.getX() + 20), static_cast<float>(card.getRight() - 20));
}

void UpdatePopup::resized()
{
    const auto card = cardBounds();
    auto row = juce::Rectangle<int>(card.getX() + 20, card.getBottom() - 46, card.getWidth() - 40, c_buttonHeight);

    // SKIP on its own at the left, LATER and DOWNLOAD together at the right.
    m_skipButton.setBounds(row.removeFromLeft(168));
    m_downloadButton.setBounds(row.removeFromRight(116));
    row.removeFromRight(8);
    m_laterButton.setBounds(row.removeFromRight(84));
}
