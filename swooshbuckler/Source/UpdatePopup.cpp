#include "UpdatePopup.h"
#include "LookAndFeel.h"

namespace TC = Theme::Colours;

UpdatePopup::UpdatePopup(const Theme::Fonts& fonts) : m_fonts(fonts)
{
    setWantsKeyboardFocus(true);
    for (auto* b : { &m_download, &m_later, &m_skip })
    {
        b->setColour(juce::TextButton::buttonOnColourId, TC::flight);
        b->setWantsKeyboardFocus(false);
        addAndMakeVisible(*b);
    }
    m_download.setToggleState(true, juce::dontSendNotification); // lit in gold: the main action
    m_download.setTooltip("Open the download page in your browser");
    m_skip.setTooltip("Don't remind me about this version again");

    m_download.onClick = [this] { m_info.openDownloadPage(); close(); };
    m_later.onClick = [this] { close(); };
    m_skip.onClick = [this] { sammyj::UpdateCheck::skipVersion(m_info); close(); };
}

void UpdatePopup::show(const sammyj::UpdateInfo& info)
{
    m_info = info;
    setVisible(true);
    toFront(false);
    repaint();
    if (isShowing())
        grabKeyboardFocus();
}

void UpdatePopup::close()
{
    setVisible(false);
    if (auto* parent = getParentComponent(); parent != nullptr && parent->isShowing())
        parent->grabKeyboardFocus();
}

void UpdatePopup::resized()
{
    m_panel = getLocalBounds().withSizeKeepingCentre(520, 168);
    auto r = m_panel.reduced(28, 0).withTrimmedTop(50).withTrimmedBottom(18);
    auto bottom = r.removeFromBottom(30);
    m_download.setBounds(bottom.removeFromRight(110));
    bottom.removeFromRight(8);
    m_later.setBounds(bottom.removeFromRight(90));
    m_skip.setBounds(bottom.removeFromLeft(150));
    m_textArea = r;
}

void UpdatePopup::paint(juce::Graphics& g)
{
    g.fillAll(TC::background.withAlpha(0.8f));
    SwooshLookAndFeel::drawPanel(g, m_panel.toFloat(), "Swooshbuckler " + m_info.latestVersion + " is available", {},
                                 TC::flight, m_fonts);

    auto r = m_textArea;
    SwooshLookAndFeel::drawCutlasses(g, juce::Rectangle<float>(static_cast<float>(r.getX()), static_cast<float>(r.getY()) + 6.0f, 28.0f, 25.0f),
                                     TC::master, TC::brass);
    g.setFont(m_fonts.body(13.0f));
    g.setColour(TC::text);
    g.drawText("You have " + m_info.currentVersion + ".", r.removeFromTop(38).withTrimmedLeft(40), juce::Justification::centredLeft, true);
}

void UpdatePopup::mouseDown(const juce::MouseEvent& e)
{
    // Like About and Settings: a click outside the card puts it away (Later).
    if (! m_panel.contains(e.getPosition()))
        close();
}

bool UpdatePopup::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        m_later.triggerClick();
        return true;
    }
    if (key == juce::KeyPress::returnKey)
    {
        m_download.triggerClick();
        return true;
    }
    return false;
}
