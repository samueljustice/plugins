#include "UpdatePopup.h"

namespace
{
    // The editor's alert metrics (SubbertoneLookAndFeel): header row, pill height, button widths.
    constexpr int c_headerHeight = 32;
    constexpr int c_messageHeight = 58;
    constexpr int c_buttonHeight = 28;
    constexpr int c_buttonGap = 10;
    constexpr int c_buttonMinWidth = 96;
    constexpr int c_sidePadding = 20;
    constexpr int c_bottomPadding = 18;
    constexpr int c_minCardWidth = 380;
}

UpdatePopup::UpdatePopup(const SubbertoneLookAndFeel& lookAndFeel, sammyj::UpdateInfo info, std::function<void()> onClose)
    : m_lookAndFeel(lookAndFeel)
    , m_info(std::move(info))
    , m_onClose(std::move(onClose))
    , m_title("Subbertone " + m_info.latestVersion + " is available")
    , m_message("You have " + m_info.currentVersion + ".")
    , m_accent(SubbertoneLookAndFeel::accentFor(juce::MessageBoxIconType::NoIcon))
{
    setTitle(m_title);
    setDescription(m_message);

    // Keyboard focus stays in the popup (Tab cycles its buttons); the popup itself takes it when shown.
    setWantsKeyboardFocus(true);
    setFocusContainerType(FocusContainerType::keyboardFocusContainer);

    const std::pair<juce::TextButton*, bool> buttons[] { { &m_downloadButton, true }, { &m_laterButton, false }, { &m_skipButton, false } };
    for (auto [button, primary] : buttons)
    {
        SubbertoneLookAndFeel::setDrawnAsDialogButton(*button, primary, m_accent);
        addAndMakeVisible(*button);
    }

    m_downloadButton.setTooltip("Open the download page for Subbertone " + m_info.latestVersion);
    m_skipButton.setTooltip("Don't show this again until a newer version comes out");

    m_downloadButton.onClick = [this] { download(); };
    m_laterButton.onClick = [this] { later(); };
    m_skipButton.onClick = [this] { skip(); };
}

UpdatePopup::~UpdatePopup() = default;

juce::Rectangle<int> UpdatePopup::cardBounds() const
{
    // One width for every button, as in the editor's alerts.
    const auto font = m_fonts->label(12.0f);
    int buttonWidth = c_buttonMinWidth;
    for (auto* b : { &m_downloadButton, &m_laterButton, &m_skipButton })
        buttonWidth = std::max(buttonWidth, juce::roundToInt(juce::GlyphArrangement::getStringWidth(font, b->getButtonText().toUpperCase())) + 44);

    const int width = std::max(c_minCardWidth, 3 * buttonWidth + 2 * c_buttonGap + 2 * c_sidePadding);
    const int height = c_headerHeight + c_messageHeight + c_buttonHeight + c_bottomPadding;

    // Centred; never wider or taller than the editor (it isn't, at any size the editor allows).
    const auto area = getLocalBounds().reduced(8);
    return juce::Rectangle<int>(std::min(width, area.getWidth()), std::min(height, area.getHeight())).withCentre(area.getCentre());
}

void UpdatePopup::resized()
{
    const auto card = cardBounds();
    auto row = card.withTrimmedTop(c_headerHeight + c_messageHeight).withHeight(c_buttonHeight).reduced(c_sidePadding, 0);

    const int buttonWidth = (row.getWidth() - 2 * c_buttonGap) / 3;
    m_downloadButton.setBounds(row.removeFromLeft(buttonWidth));
    row.removeFromLeft(c_buttonGap);
    m_laterButton.setBounds(row.removeFromLeft(buttonWidth));
    row.removeFromLeft(c_buttonGap);
    m_skipButton.setBounds(row.removeFromLeft(buttonWidth));
}

void UpdatePopup::paint(juce::Graphics& g)
{
    // Backdrop: the editor, dimmed.
    g.fillAll(Theme::Colours::background.withAlpha(0.72f));

    const auto card = cardBounds().toFloat();

    juce::Path cardShape;
    cardShape.addRoundedRectangle(card, 8.0f);
    juce::DropShadow(juce::Colours::black.withAlpha(0.55f), 28, { 0, 8 }).drawForPath(g, cardShape);

    const auto body = m_lookAndFeel.drawDialogCard(g, card, m_title, m_accent);

    // The message, centred between the header and the buttons (as in the alerts).
    const auto messageArea = body.withHeight(static_cast<float>(c_messageHeight) - 1.0f);
    g.setFont(m_fonts->body(12.5f));
    g.setColour(findColour(juce::AlertWindow::textColourId));
    g.drawFittedText(m_message, messageArea.toNearestInt().reduced(c_sidePadding, 0), juce::Justification::centred, 2);
}

bool UpdatePopup::keyPressed(const juce::KeyPress& key)
{
    // A focused button handles Return itself (Button::keyPressed); here Return means the primary action.
    if (key.isKeyCode(juce::KeyPress::returnKey))
    {
        download();
        return true;
    }

    if (key.isKeyCode(juce::KeyPress::escapeKey))
    {
        later();
        return true;
    }

    return false;
}

void UpdatePopup::download()
{
    if (m_closing)
        return;

    m_info.openDownloadPage();
    close();
}

void UpdatePopup::later()
{
    close();
}

void UpdatePopup::skip()
{
    if (m_closing)
        return;

    sammyj::UpdateCheck::skipVersion(m_info);
    close();
}

void UpdatePopup::close()
{
    if (m_closing)
        return;

    m_closing = true;
    setVisible(false);

    // The owner deletes the popup; not from inside the button's (or key's) own callback.
    juce::MessageManager::callAsync([safe = juce::Component::SafePointer<UpdatePopup>(this)]
    {
        if (safe != nullptr && safe->m_onClose)
            safe->m_onClose();
    });
}

std::unique_ptr<juce::AccessibilityHandler> UpdatePopup::createAccessibilityHandler()
{
    return std::make_unique<juce::AccessibilityHandler>(*this, juce::AccessibilityRole::dialogWindow);
}
