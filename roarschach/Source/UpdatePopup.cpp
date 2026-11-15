#include "UpdatePopup.h"

namespace
{
    using namespace Theme::Colours;
    namespace Type = Theme::Type;

    // The editor alerts' metrics (RoarschachLookAndFeel), so the card reads as one of them.
    constexpr float c_radius = Theme::Metrics::radius;
    constexpr float c_headerHeight = 36.0f;
    constexpr int c_messageHeight = 54;
    constexpr int c_buttonHeight = 28;
    constexpr int c_buttonGap = 10;
    constexpr int c_buttonMinWidth = 96;
    constexpr int c_sidePad = 18;
    constexpr int c_bottomPad = 16;
    constexpr int c_minCardWidth = 440;

    float lineWidth(const juce::Font& font, const juce::String& text)
    {
        return juce::GlyphArrangement::getStringWidth(font, text + " ") - juce::GlyphArrangement::getStringWidth(font, " ");
    }
}

//==============================================================================
UpdatePopup::DialogButton::DialogButton(const juce::String& buttonText, bool primary)
    : juce::TextButton(buttonText)
    , m_primary(primary)
{
}

void UpdatePopup::DialogButton::paintButton(juce::Graphics& g, bool isMouseOverButton, bool isButtonDown)
{
    // As the alerts' chips (RoarschachLookAndFeel::drawButtonBackground / drawButtonText): raised paper with
    // an ink hairline; the primary action filled with the dyed ink, its label in paper; the focus ring for
    // keyboard focus.
    const auto r = getLocalBounds().toFloat().reduced(0.5f);
    const float corner = Theme::Metrics::radius;

    if (m_primary)
    {
        g.setColour(isButtonDown ? accentInk.withAlpha(0.8f) : accentInk);
        g.fillRoundedRectangle(r, corner);
    }
    else
    {
        g.setColour(isButtonDown ? sheet.darker(0.06f) : sheet);
        g.fillRoundedRectangle(r, corner);
        g.setColour(isMouseOverButton ? ink.withAlpha(0.65f) : rule);
        g.drawRoundedRectangle(r, corner, Theme::Metrics::hairline);
    }

    if (RoarschachLookAndFeel::shouldShowFocusRing(*this))
    {
        g.setColour(focus);
        g.drawRoundedRectangle(r.expanded(1.5f), corner + 1.5f, Theme::Metrics::focusRing);
    }

    g.setFont(m_fonts->label(Type::button));
    g.setColour(m_primary ? sheet : ink);
    g.drawText(getButtonText().toUpperCase(), r, juce::Justification::centred, false);
}

//==============================================================================
UpdatePopup::UpdatePopup(RoarschachLookAndFeel& lookAndFeel, sammyj::UpdateInfo info, std::function<void()> onClose)
    : m_lookAndFeel(lookAndFeel)
    , m_info(std::move(info))
    , m_onClose(std::move(onClose))
{
    setLookAndFeel(&m_lookAndFeel);
    setOpaque(false);
    setInterceptsMouseClicks(true, true); // the backdrop takes the clicks meant for the editor beneath
    setWantsKeyboardFocus(true);
    setFocusContainerType(FocusContainerType::keyboardFocusContainer); // Tab stays among the buttons
    setTitle("Roarschach " + m_info.latestVersion + " is available");
    setDescription("You have " + m_info.currentVersion + ".");

    m_downloadButton.setTooltip("Open the download page in your browser");
    m_laterButton.setTooltip("Close this message; it comes back the next time Roarschach checks");
    m_skipButton.setTooltip("Don't show this version again");

    m_downloadButton.onClick = [this] { download(); };
    m_laterButton.onClick = [this] { later(); };
    m_skipButton.onClick = [this] { skip(); };

    // Tab order: Download, Later, Skip this version (left to right).
    int order = 1;
    for (auto* b : { &m_downloadButton, &m_laterButton, &m_skipButton })
    {
        b->setExplicitFocusOrder(order++);
        addAndMakeVisible(b);
    }
}

UpdatePopup::~UpdatePopup()
{
    setLookAndFeel(nullptr);
}

juce::Rectangle<int> UpdatePopup::getCardBounds() const
{
    // One width for every button, so the row reads as a set (as the alerts do).
    const auto font = m_fonts->label(Type::button);
    int buttonWidth = c_buttonMinWidth;
    for (const auto* b : { &m_downloadButton, &m_laterButton, &m_skipButton })
        buttonWidth = std::max(buttonWidth, juce::roundToInt(lineWidth(font, b->getButtonText().toUpperCase())) + 44);

    const auto titleWidth = juce::roundToInt(lineWidth(m_fonts->title(Type::heading), getTitle())) + 2 * 16 + 14;
    const int width = std::max({ c_minCardWidth, 3 * buttonWidth + 2 * c_buttonGap + 2 * c_sidePad, titleWidth });
    const int height = juce::roundToInt(c_headerHeight) + c_messageHeight + c_buttonHeight + c_bottomPad;

    return getLocalBounds().withSizeKeepingCentre(std::min(width, getWidth() - 16), height);
}

void UpdatePopup::resized()
{
    const auto card = getCardBounds();
    auto row = card.withTrimmedLeft(c_sidePad).withTrimmedRight(c_sidePad)
                   .removeFromBottom(c_bottomPad + c_buttonHeight).withTrimmedBottom(c_bottomPad);

    const int buttonWidth = (row.getWidth() - 2 * c_buttonGap) / 3;
    const int used = 3 * buttonWidth + 2 * c_buttonGap;
    row = row.withSizeKeepingCentre(used, row.getHeight());

    m_downloadButton.setBounds(row.removeFromLeft(buttonWidth));
    row.removeFromLeft(c_buttonGap);
    m_laterButton.setBounds(row.removeFromLeft(buttonWidth));
    row.removeFromLeft(c_buttonGap);
    m_skipButton.setBounds(row.removeFromLeft(buttonWidth));
}

void UpdatePopup::paint(juce::Graphics& g)
{
    // Backdrop: the editor dimmed beneath.
    g.fillAll(paper.withAlpha(0.78f));

    const auto card = getCardBounds().toFloat();

    juce::Path shape;
    shape.addRoundedRectangle(card, c_radius);
    juce::DropShadow(shadow, 28, { 0, 8 }).drawForPath(g, shape);

    // A sheet of raised paper, as the editor's alerts (RoarschachLookAndFeel::drawAlertBox).
    g.setColour(sheet);
    g.fillRoundedRectangle(card, c_radius);
    g.setColour(rule);
    g.drawRoundedRectangle(card.reduced(0.5f), c_radius, Theme::Metrics::hairline);

    // Header row: the title in the display face, a short dye rule under it.
    auto header = card.withHeight(c_headerHeight).reduced(16.0f, 0.0f);
    g.setFont(m_fonts->title(Type::heading));
    g.setColour(ink);
    g.drawText(getTitle(), header.translated(0.0f, 2.0f), juce::Justification::centredLeft, true);
    g.setColour(accent);
    g.fillRect(juce::Rectangle<float>(card.getX() + 16.0f, card.getY() + c_headerHeight - 3.0f, 24.0f, 1.5f));

    // The message, centred between the header and the buttons.
    const auto message = card.withTrimmedTop(c_headerHeight).withHeight(static_cast<float>(c_messageHeight));
    g.setFont(m_fonts->body(Type::body));
    g.setColour(ink);
    g.drawText(getDescription(), message, juce::Justification::centred, false);
}

bool UpdatePopup::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        later();
        return true;
    }

    if (key == juce::KeyPress::returnKey)
    {
        // The focused button (after Tab), otherwise the primary action.
        for (auto* b : { &m_downloadButton, &m_laterButton, &m_skipButton })
        {
            if (b->hasKeyboardFocus(false))
            {
                b->triggerClick();
                return true;
            }
        }

        download();
        return true;
    }

    return false;
}

//==============================================================================
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

    // Deferred: we may be inside one of our own buttons' callbacks.
    juce::MessageManager::callAsync([safe = juce::Component::SafePointer<UpdatePopup>(this)]
    {
        if (safe == nullptr)
            return;

        const auto onClose = safe->m_onClose; // a copy: calling it deletes this popup
        if (onClose)
            onClose();
    });
}
