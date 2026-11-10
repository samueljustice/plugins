#include "UpdatePopup.h"

namespace
{
    // Design-size metrics, as the plug-in's dialogs (PitchFlattenerLookAndFeel's alert drawing).
    constexpr int c_cardWidth = 616;
    constexpr int c_cardHeight = 140;
    constexpr float c_radius = 8.0f;          // the editor's panel radius
    constexpr float c_headerHeight = 32.0f;   // the editor's section header height
    constexpr int c_buttonHeight = 28;
    constexpr int c_buttonGap = 10;
    constexpr int c_buttonMinWidth = 96;
    constexpr int c_edge = 16;                // the editor's margin: the card never gets closer to its edge

    // Information, not a question or a warning: the dialogs' teal.
    const juce::Colour c_accent = PitchFlattenerLookAndFeel::accentFor (juce::MessageBoxIconType::InfoIcon);
}

//==============================================================================
UpdatePopup::Card::Card (const sammyj::UpdateInfo& info)
    : m_title ("Pitch Flattener " + info.latestVersion + " is available")
    , m_message ("You have " + info.currentVersion + ".")
{
    setOpaque (false); // rounded corners show the backdrop

    PitchFlattenerLookAndFeel::setDialogButton (m_download, c_accent, true);
    PitchFlattenerLookAndFeel::setDialogButton (m_later, c_accent, false);
    PitchFlattenerLookAndFeel::setDialogButton (m_skip, c_accent, false);

    for (auto* b : { &m_download, &m_later, &m_skip })
    {
        b->setWantsKeyboardFocus (true);
        addAndMakeVisible (*b);
    }
}

void UpdatePopup::Card::paint (juce::Graphics& g)
{
    using namespace Theme::Colours;
    const auto bounds = getLocalBounds().toFloat();

    // A panel, as the dialogs: fill, outline, accent dot, the title in capitals, a divider.
    g.setColour (panel);
    g.fillRoundedRectangle (bounds, c_radius);
    g.setColour (outline);
    g.drawRoundedRectangle (bounds.reduced (0.5f), c_radius, 1.0f);

    auto header = bounds.withHeight (c_headerHeight).reduced (14.0f, 0.0f);
    g.setColour (c_accent);
    g.fillEllipse (header.removeFromLeft (6.0f).withSizeKeepingCentre (6.0f, 6.0f));
    header.removeFromLeft (8.0f);
    g.setFont (m_fonts->heading (11.5f));
    g.setColour (text.withAlpha (0.85f));
    g.drawFittedText (m_title.toUpperCase(), header.toNearestInt(), juce::Justification::centredLeft, 1, 1.0f);
    g.setColour (outlineSoft);
    g.fillRect (juce::Rectangle<float> (12.0f, c_headerHeight, bounds.getWidth() - 24.0f, 1.0f));

    // The message, centred between the header and the buttons.
    const float buttonsTop = (float) m_download.getY();
    g.setFont (m_fonts->body (12.5f));
    g.setColour (text);
    g.drawFittedText (m_message, juce::Rectangle<float> (16.0f, c_headerHeight + 1.0f, bounds.getWidth() - 32.0f,
                                                         buttonsTop - c_headerHeight - 1.0f).toNearestInt(),
                      juce::Justification::centred, 2, 1.0f);
}

void UpdatePopup::Card::resized()
{
    // One width for every button, so the row reads as a set (as the dialogs).
    const auto font = m_fonts->label (12.0f);
    int width = c_buttonMinWidth;
    for (auto* b : { &m_download, &m_later, &m_skip })
        width = std::max (width, juce::roundToInt (juce::GlyphArrangement::getStringWidth (font, b->getButtonText().toUpperCase())) + 44);

    const int rowWidth = 3 * width + 2 * c_buttonGap;
    auto row = juce::Rectangle<int> ((getWidth() - rowWidth) / 2, getHeight() - 18 - c_buttonHeight, rowWidth, c_buttonHeight);

    m_download.setBounds (row.removeFromLeft (width));
    row.removeFromLeft (c_buttonGap);
    m_later.setBounds (row.removeFromLeft (width));
    row.removeFromLeft (c_buttonGap);
    m_skip.setBounds (row.removeFromLeft (width));
}

//==============================================================================
UpdatePopup::UpdatePopup (const sammyj::UpdateInfo& info, std::function<void()> onDismiss)
    : m_info (info)
    , m_onDismiss (std::move (onDismiss))
    , m_card (info)
{
    setOpaque (false);
    setInterceptsMouseClicks (true, true); // the backdrop takes every click (and does nothing), as a modal dialog

    // Tab cycles through the popup's buttons only; the popup itself takes the keys (Return, Escape) first.
    setWantsKeyboardFocus (true);
    setFocusContainerType (juce::Component::FocusContainerType::keyboardFocusContainer);

    m_card.m_download.onClick = [this]
    {
        m_info.openDownloadPage();
        dismiss();
    };
    m_card.m_later.onClick = [this] { dismiss(); };
    m_card.m_skip.onClick = [this]
    {
        sammyj::UpdateCheck::skipVersion (m_info);
        dismiss();
    };

    addAndMakeVisible (m_card);
    m_card.setSize (c_cardWidth, c_cardHeight);
}

void UpdatePopup::setEditorScale (float scale)
{
    m_editorScale = scale;
    resized();
}

void UpdatePopup::paint (juce::Graphics& g)
{
    g.fillAll (Theme::Colours::background.withAlpha (0.72f));
}

void UpdatePopup::resized()
{
    // As the editor's menus: follows the editor's scale, never smaller than its default size, and always fits.
    const float fit = std::min ((float) (getWidth() - 2 * c_edge) / (float) c_cardWidth,
                                (float) (getHeight() - 2 * c_edge) / (float) c_cardHeight);
    const float scale = std::max (0.1f, std::min (std::max (1.0f, m_editorScale), fit));

    const float x = ((float) getWidth() - (float) c_cardWidth * scale) * 0.5f;
    const float y = ((float) getHeight() - (float) c_cardHeight * scale) * 0.5f;
    m_card.setTransform (juce::AffineTransform::scale (scale).translated (std::round (x), std::round (y)));
}

bool UpdatePopup::keyPressed (const juce::KeyPress& key)
{
    // A focused button handles its own Return (and Space); keys it doesn't take come here.
    if (key.isKeyCode (juce::KeyPress::escapeKey))
    {
        m_card.m_later.triggerClick();
        return true;
    }

    if (key.isKeyCode (juce::KeyPress::returnKey))
    {
        m_card.m_download.triggerClick();
        return true;
    }

    return false;
}

void UpdatePopup::dismiss()
{
    // The owner deletes this popup: call a copy.
    setVisible (false);
    if (auto onDismiss = m_onDismiss)
        onDismiss();
}
