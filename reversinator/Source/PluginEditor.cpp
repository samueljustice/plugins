#include "PluginEditor.h"

namespace
{
    constexpr int c_margin = 16;
    constexpr int c_headerHeight = 56;
    constexpr int c_displayHeight = 128;
    constexpr int c_sectionGap = 12;
    constexpr int c_sectionHeaderHeight = 32;
    constexpr int c_captionHeight = 18;

    double parseNumber(const juce::String& text)
    {
        return text.retainCharacters("0123456789.-").getDoubleValue();
    }
}

//==============================================================================
ReversinatorAudioProcessorEditor::ReversinatorAudioProcessorEditor(ReversinatorAudioProcessor& p)
    : AudioProcessorEditor(&p)
    , m_processor(p)
    , m_lookAndFeel(*m_fonts)
    , m_content(p, *m_fonts)
    , m_tooltipWindow(this, 700)
{
    // Read the saved size first: setResizeLimits() below already resizes (and so calls resized()).
    const int savedWidth = m_processor.getEditorWidth();

    addAndMakeVisible(m_content);

    // Set after the content is added so every child (including slider value boxes) picks it up.
    setLookAndFeel(&m_lookAndFeel);
    m_tooltipWindow.setLookAndFeel(&m_lookAndFeel);
    m_tooltipWindow.setOpaque(false);   // JUCE marks it opaque, which leaves its rounded corners unpainted
    Controls::LookAndFeel::showFocusRingsAfterTab(*this);   // keyboard focus rings after Tab, hidden by a click

    setResizable(true, true);
    getConstrainer()->setFixedAspectRatio(static_cast<double>(baseWidth) / static_cast<double>(baseHeight));
    setResizeLimits(baseWidth * 3 / 4, baseHeight * 3 / 4, baseWidth * 2, baseHeight * 2);

    const int width = savedWidth > 0 ? juce::jlimit(baseWidth * 3 / 4, baseWidth * 2, savedWidth) : baseWidth;
    setSize(width, juce::roundToInt(width * static_cast<double>(baseHeight) / baseWidth));

    m_constructed = true;
    m_processor.setEditorWidth(getWidth());

    // Looks for a newer release in the background (at most once a day; silent if offline) and shows
    // the popup if there is one. The offline test runner builds with this off, so it never goes online.
#ifndef REVERSINATOR_NO_UPDATE_CHECK
    m_updateCheck = std::make_unique<sammyj::UpdateCheck>("reversinator", "reversinator-v", JucePlugin_VersionString,
                                                          [this](const sammyj::UpdateInfo& info) { showUpdatePopup(info); });
#endif
}

ReversinatorAudioProcessorEditor::~ReversinatorAudioProcessorEditor()
{
    m_updateCheck.reset();   // stop the check before anything it calls back into goes away
    m_tooltipWindow.setLookAndFeel(nullptr);
    setLookAndFeel(nullptr);
}

void ReversinatorAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(Theme::Colours::background);
}

void ReversinatorAudioProcessorEditor::showUpdatePopup(const sammyj::UpdateInfo& info)
{
    m_content.showUpdatePopup(info);
}

void ReversinatorAudioProcessorEditor::resized()
{
    const float scale = std::min(static_cast<float>(getWidth()) / baseWidth, static_cast<float>(getHeight()) / baseHeight);
    const float x = (static_cast<float>(getWidth()) - baseWidth * scale) * 0.5f;
    const float y = (static_cast<float>(getHeight()) - baseHeight * scale) * 0.5f;

    m_content.setBounds(0, 0, baseWidth, baseHeight);
    m_content.setTransform(juce::AffineTransform::scale(scale).translated(x, y));

    if (m_constructed)
        m_processor.setEditorWidth(getWidth());
}

//==============================================================================
ReversinatorAudioProcessorEditor::Content::Content(ReversinatorAudioProcessor& p, const Theme::Fonts& fonts)
    : m_processor(p)
    , m_fonts(fonts)
    , m_display(p, fonts)
    , m_modeSelector(*p.getParameters().getParameter(ParamIDs::mode), fonts)
    , m_reverserAttachment(p.getParameters(), ParamIDs::reverser, m_reverserButton)
    , m_timeAttachment(p.getParameters(), ParamIDs::time, m_timeSlider)
    , m_feedbackAttachment(p.getParameters(), ParamIDs::feedback, m_feedbackSlider)
    , m_wetAttachment(p.getParameters(), ParamIDs::wetMix, m_wetSlider)
    , m_dryAttachment(p.getParameters(), ParamIDs::dryMix, m_drySlider)
    , m_about(fonts)
    , m_updatePopup(fonts)
{
    using namespace Theme::Colours;

    m_sections = { Section { "MODE",     mode,     {} },
                   Section { "REVERSER", reverser, {} },
                   Section { "VOLUME",   mix,      {} } };

    m_captions = { Caption { "TIME", {} }, Caption { "FEEDBACK DEPTH", {} },
                   Caption { "WET MIX", {} }, Caption { "DRY MIX", {} } };

    addAndMakeVisible(m_display);
    addAndMakeVisible(m_modeSelector);

    m_reverserButton.setTooltip("REVERSER: hear the reversed audio (arrow left) or the original forwards audio (arrow right). "
                                "Switching is crossfaded, so it never clicks.");
    addAndMakeVisible(m_reverserButton);

    setupSlider(m_timeSlider, Controls::SliderStyle::Hourglass, ParamIDs::time, sand,
                "TIME: the length of each snippet that is played backwards (0.5 to 4 seconds). "
                "The default of 2 seconds tends to sound best. Changes are smoothed, so it never clicks.",
                [](double v) { return juce::String(v, 2) + " s"; });

    setupSlider(m_feedbackSlider, Controls::SliderStyle::Loop, ParamIDs::feedback, feedback,
                "FEEDBACK DEPTH: feeds the effect back into itself for repeating, cascading reverses. "
                "Usually not needed, so the default is 0%.",
                [](double v) { return juce::String(juce::roundToInt(v)) + "%"; });

    setupSlider(m_wetSlider, Controls::SliderStyle::Fader, ParamIDs::wetMix, mix,
                "WET MIX: level of the effect, and of the forwards audio when REVERSER is set to forwards.",
                [](double v) { return juce::String(juce::roundToInt(v)) + "%"; });

    setupSlider(m_drySlider, Controls::SliderStyle::Fader, ParamIDs::dryMix, mix,
                "DRY MIX: mixes in the original input. Usually 0%, but use it to hear the forwards sound "
                "along with the backwards sound.",
                [](double v) { return juce::String(juce::roundToInt(v)) + "%"; });

    m_aboutButton.setButtonText("?");
    m_aboutButton.setTooltip("About SammyJs Reversinator");
    m_aboutButton.onClick = [this] { m_about.show(); };
    addAndMakeVisible(m_aboutButton);

    addChildComponent(m_about);         // hidden until "?" is clicked; added last so it sits on top
    addChildComponent(m_updatePopup);   // hidden until a newer release is found; above everything, the About panel too

    // The attachment changes the toggle without notifications (e.g. host automation),
    // so watch the state value to keep the label in step.
    m_reverserButton.getToggleStateValue().addListener(this);
}

ReversinatorAudioProcessorEditor::Content::~Content()
{
    m_reverserButton.getToggleStateValue().removeListener(this);
}

void ReversinatorAudioProcessorEditor::Content::valueChanged(juce::Value&)
{
    repaint(m_reverserLabelBounds);
}

void ReversinatorAudioProcessorEditor::Content::setupSlider(juce::Slider& slider, Controls::SliderStyle style,
                                                            const juce::String& paramId, juce::Colour accent,
                                                            const juce::String& tooltip,
                                                            std::function<juce::String(double)> format)
{
    Controls::setStyle(slider, style);
    slider.setColour(juce::Slider::trackColourId, accent);
    slider.setTooltip(tooltip);
    slider.setVelocityBasedMode(false);
    slider.setMouseDragSensitivity(220);
    slider.textFromValueFunction = std::move(format);
    slider.valueFromTextFunction = [](const juce::String& text) { return parseNumber(text); };

    if (auto* param = m_processor.getParameters().getParameter(paramId))
        slider.setDoubleClickReturnValue(true, param->convertFrom0to1(param->getDefaultValue()));

    slider.updateText();
    addAndMakeVisible(slider);
}

void ReversinatorAudioProcessorEditor::Content::resized()
{
    auto bounds = getLocalBounds();
    m_about.setBounds(bounds);
    m_updatePopup.setBounds(bounds);

    auto header = bounds.removeFromTop(c_headerHeight).reduced(c_margin, 0);
    m_aboutButton.setBounds(header.removeFromRight(26).withSizeKeepingCentre(26, 26));

    bounds.reduce(c_margin, 0);
    bounds.removeFromBottom(c_margin);

    m_display.setBounds(bounds.removeFromTop(c_displayHeight));
    bounds.removeFromTop(c_sectionGap);

    m_sections[0].bounds = bounds.removeFromLeft(232);
    bounds.removeFromLeft(c_sectionGap);
    m_sections[2].bounds = bounds.removeFromRight(184);
    bounds.removeFromRight(c_sectionGap);
    m_sections[1].bounds = bounds;

    const auto bodyOf = [](const Section& s) { return s.bounds.withTrimmedTop(c_sectionHeaderHeight).reduced(8, 8); };

    // MODE: three lamps stacked.
    m_modeSelector.setBounds(bodyOf(m_sections[0]));

    // REVERSER: the button on top, TIME and FEEDBACK DEPTH side by side below.
    auto centre = bodyOf(m_sections[1]);
    auto top = centre.removeFromTop(96);
    m_reverserButton.setBounds(top.removeFromTop(74).withSizeKeepingCentre(74, 74));
    m_reverserLabelBounds = top.removeFromTop(18);

    auto row = centre.withTrimmedTop(4);
    auto captions = row.removeFromBottom(c_captionHeight);
    const int half = row.getWidth() / 2;
    m_timeSlider.setBounds(row.removeFromLeft(half).reduced(10, 0));
    m_feedbackSlider.setBounds(row.reduced(10, 0));
    m_captions[0].bounds = captions.removeFromLeft(half);
    m_captions[1].bounds = captions;

    // VOLUME: wet and dry faders.
    auto volume = bodyOf(m_sections[2]);
    auto volumeCaptions = volume.removeFromBottom(c_captionHeight);
    const int faderWidth = volume.getWidth() / 2;
    m_wetSlider.setBounds(volume.removeFromLeft(faderWidth));
    m_drySlider.setBounds(volume);
    m_captions[2].bounds = volumeCaptions.removeFromLeft(faderWidth);
    m_captions[3].bounds = volumeCaptions;
}

void ReversinatorAudioProcessorEditor::Content::paint(juce::Graphics& g)
{
    using namespace Theme::Colours;

    g.fillAll(background);

    // Header: a backwards arrow, the name and the family line.
    auto header = getLocalBounds().removeFromTop(c_headerHeight).reduced(c_margin, 0).toFloat();
    {
        const auto mark = header.removeFromLeft(34.0f).withSizeKeepingCentre(34.0f, 20.0f);
        juce::Path arrow;
        arrow.addTriangle(mark.getX(), mark.getCentreY(), mark.getX() + 11.0f, mark.getY() + 2.0f, mark.getX() + 11.0f, mark.getBottom() - 2.0f);
        g.setColour(reverser);
        g.fillPath(arrow);

        juce::Path tail;
        tail.startNewSubPath(mark.getX() + 10.0f, mark.getCentreY());
        tail.lineTo(mark.getRight() - 4.0f, mark.getCentreY());
        tail.quadraticTo(mark.getRight(), mark.getCentreY(), mark.getRight(), mark.getCentreY() + 4.0f);
        g.strokePath(tail, juce::PathStrokeType(2.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    header.removeFromLeft(12.0f);
    const auto titleFont = m_fonts.heading(20.0f);
    const juce::String title = "REVERSINATOR";
    g.setFont(titleFont);
    g.setColour(text);
    const float titleWidth = juce::GlyphArrangement::getStringWidth(titleFont, title);
    g.drawText(title, header.removeFromLeft(titleWidth + 4.0f), juce::Justification::centredLeft, false);

    header.removeFromLeft(14.0f);
    g.setFont(m_fonts.label(10.5f));
    g.setColour(textFaint);
    g.drawText("SAMMYJS  /  REAL-TIME REVERSER", header.withTrimmedTop(4.0f), juce::Justification::centredLeft, false);

    // Section panels
    for (const auto& section : m_sections)
    {
        const auto r = section.bounds.toFloat();
        g.setColour(panel);
        g.fillRoundedRectangle(r, 8.0f);
        g.setColour(outlineSoft);
        g.drawRoundedRectangle(r.reduced(0.5f), 8.0f, 1.0f);

        auto headerRow = r.withHeight(static_cast<float>(c_sectionHeaderHeight)).reduced(12.0f, 0.0f);
        g.setColour(section.accent);
        g.fillEllipse(headerRow.removeFromLeft(6.0f).withSizeKeepingCentre(6.0f, 6.0f));
        headerRow.removeFromLeft(8.0f);
        g.setFont(m_fonts.heading(11.0f));
        g.setColour(text.withAlpha(0.85f));
        g.drawText(section.title, headerRow, juce::Justification::centredLeft, false);

        g.setColour(outlineSoft);
        g.drawHorizontalLine(section.bounds.getY() + c_sectionHeaderHeight, r.getX() + 12.0f, r.getRight() - 12.0f);
    }

    // Captions under the controls
    g.setFont(m_fonts.label(10.5f));
    g.setColour(textDim);
    for (const auto& caption : m_captions)
        g.drawText(caption.text, caption.bounds, juce::Justification::centred, false);

    // REVƎRSƎЯ: the label reverses itself, as on the original.
    g.setColour(m_reverserButton.getToggleState() ? text : textDim);
    Controls::drawMirroredText(g, "REVERSER", m_fonts.heading(12.0f), m_reverserLabelBounds.toFloat(), { 3, 6, 7 });
}
