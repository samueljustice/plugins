#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
// AboutWindow implementation
AboutWindow::AboutWindow()
    : DocumentWindow("About SammyJs Reversinator", 
                     juce::Colours::darkgrey,
                     juce::DocumentWindow::closeButton)
{
    setUsingNativeTitleBar(true);
    content = std::make_unique<AboutContent>();
    setContentOwned(content.release(), false);
    
    centreWithSize(500, 600);
    setVisible(true);
    setResizable(false, false);
}

AboutWindow::~AboutWindow()
{
}

void AboutWindow::closeButtonPressed()
{
    setVisible(false);
}

//==============================================================================
// AboutContent implementation
AboutWindow::AboutContent::AboutContent()
{
    titleLabel.setText("SammyJs Reversinator", juce::dontSendNotification);
    titleLabel.setFont(juce::Font(24.0f, juce::Font::bold));
    titleLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(titleLabel);
    
    juce::String versionText = "Version " PLUGIN_VERSION;
    versionLabel.setText(versionText, juce::dontSendNotification);
    versionLabel.setFont(juce::Font(16.0f));
    versionLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(versionLabel);
    
    authorLabel.setText("Created by Samuel Justice", juce::dontSendNotification);
    authorLabel.setFont(juce::Font(14.0f));
    authorLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(authorLabel);
    
    emailButton.setButtonText("sam@sweetjusticesound.com");
    emailButton.setURL(juce::URL("mailto:sam@sweetjusticesound.com"));
    emailButton.setFont(juce::Font(14.0f), false);
    addAndMakeVisible(emailButton);
    
    websiteButton.setButtonText("www.sweetjusticesound.com");
    websiteButton.setURL(juce::URL("https://www.sweetjusticesound.com"));
    websiteButton.setFont(juce::Font(14.0f), false);
    addAndMakeVisible(websiteButton);
    
    checkUpdateButton.onClick = [this] { checkForUpdates(); };
    addAndMakeVisible(checkUpdateButton);
    
    updateStatusLabel.setText("", juce::dontSendNotification);
    updateStatusLabel.setJustificationType(juce::Justification::centred);
    updateStatusLabel.setFont(juce::Font(12.0f));
    addAndMakeVisible(updateStatusLabel);
    
    licenseInfo.setMultiLine(true);
    licenseInfo.setReadOnly(true);
    licenseInfo.setScrollbarsShown(true);
    licenseInfo.setCaretVisible(false);
    licenseInfo.setColour(juce::TextEditor::backgroundColourId, juce::Colours::darkgrey.darker());
    licenseInfo.setColour(juce::TextEditor::textColourId, juce::Colours::lightgrey);
    licenseInfo.setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    
    juce::String licenseText = 
        "Real-time Audio Reversing Effect\n\n"
        "Inspired by the classic Backwards Machine plugin\n\n"
        "Technologies Used:\n\n"
        "JUCE Framework\n"
        "Copyright (c) 2022 - Raw Material Software Limited\n"
        "Licensed under the GPL/Commercial license\n\n"
        
        "Features:\n"
        "• Reverse Playback - Continuous reverse effect\n"
        "• Forward Backwards - Smooth crossfade\n"
        "• Reverse Repeat - Double playback with vibrato\n"
        "• Adjustable window time (0.1 - 5 seconds)\n"
        "• Feedback control\n"
        "• Wet/Dry mix controls";
    
    licenseInfo.setText(licenseText);
    addAndMakeVisible(licenseInfo);
}

void AboutWindow::AboutContent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::darkgrey);
}

void AboutWindow::AboutContent::resized()
{
    auto area = getLocalBounds().reduced(20);
    
    titleLabel.setBounds(area.removeFromTop(40));
    versionLabel.setBounds(area.removeFromTop(30));
    area.removeFromTop(10);
    
    authorLabel.setBounds(area.removeFromTop(25));
    emailButton.setBounds(area.removeFromTop(25).withSizeKeepingCentre(250, 25));
    websiteButton.setBounds(area.removeFromTop(25).withSizeKeepingCentre(250, 25));
    area.removeFromTop(20);
    
    checkUpdateButton.setBounds(area.removeFromTop(30).withSizeKeepingCentre(150, 30));
    updateStatusLabel.setBounds(area.removeFromTop(25));
    area.removeFromTop(20);
    
    licenseInfo.setBounds(area);
}

void AboutWindow::AboutContent::checkForUpdates()
{
    updateStatusLabel.setText("Checking for updates...", juce::dontSendNotification);
    updateStatusLabel.setColour(juce::Label::textColourId, juce::Colours::yellow);
    
    // Create URL for GitHub API to check latest release with reversinator tag
    juce::URL apiUrl("https://api.github.com/repos/samueljustice/plugins/releases");
    
    // Use a thread to download the data
    juce::Thread::launch([this, apiUrl]
    {
        auto stream = apiUrl.createInputStream(juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                                               .withConnectionTimeoutMs(5000));
        
        if (!stream)
        {
            juce::MessageManager::callAsync([this]
            {
                updateStatusLabel.setText("Failed to check for updates", juce::dontSendNotification);
                updateStatusLabel.setColour(juce::Label::textColourId, juce::Colours::red);
            });
            return;
        }
        
        auto content = stream->readEntireStreamAsString();
        auto releases = juce::JSON::parse(content);
        
        if (auto* releasesArray = releases.getArray())
        {
            juce::String latestVersion;
            
            // Look for releases with reversinator tag
            for (const auto& release : *releasesArray)
            {
                if (auto* obj = release.getDynamicObject())
                {
                    auto tagName = obj->getProperty("tag_name").toString();
                    
                    // Check if this is a reversinator release
                    if (tagName.startsWith("reversinator-v"))
                    {
                        latestVersion = tagName.substring(15); // Skip "reversinator-v"
                        break; // Found the latest reversinator release
                    }
                }
            }
            
            if (latestVersion.isNotEmpty())
            {
                auto currentVersion = juce::String(PLUGIN_VERSION);
                
                juce::MessageManager::callAsync([this, latestVersion, currentVersion]
                {
                    // Simple version comparison
                    if (latestVersion > currentVersion)
                    {
                        updateStatusLabel.setText("New version " + latestVersion + " available!", 
                                                juce::dontSendNotification);
                        updateStatusLabel.setColour(juce::Label::textColourId, juce::Colours::lightgreen);
                    }
                    else
                    {
                        updateStatusLabel.setText("You have the latest version", 
                                                juce::dontSendNotification);
                        updateStatusLabel.setColour(juce::Label::textColourId, juce::Colours::lightblue);
                    }
                });
            }
            else
            {
                juce::MessageManager::callAsync([this]
                {
                    updateStatusLabel.setText("No releases found", juce::dontSendNotification);
                    updateStatusLabel.setColour(juce::Label::textColourId, juce::Colours::orange);
                });
            }
        }
    });
}

//==============================================================================
void ModeButton::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    
    if (isSelected)
    {
        g.setColour(juce::Colour(0xff5cb85c));
        g.fillEllipse(bounds.reduced(4));
        g.setColour(juce::Colours::darkgreen);
        g.drawEllipse(bounds.reduced(4), 2.0f);
    }
    else
    {
        g.setColour(juce::Colour(0xff3e342a));
        g.fillEllipse(bounds.reduced(4));
        g.setColour(juce::Colour(0xff5e4e3a));
        g.drawEllipse(bounds.reduced(4), 2.0f);
    }
    
    g.setColour(juce::Colours::black);
    g.setFont(11.0f);
    g.drawText(buttonText, bounds.withY(bounds.getBottom()), 
               juce::Justification::centredTop, false);
}

void ModeButton::mouseDown(const juce::MouseEvent&)
{
    if (onClick)
        onClick();
}

HourglassDisplay::HourglassDisplay()
{
    startTimerHz(30);
}

void HourglassDisplay::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    auto hourglassBounds = bounds.reduced(10);
    
    g.setColour(juce::Colour(0xffcccccc));
    g.fillRoundedRectangle(hourglassBounds, 5.0f);
    
    g.setColour(juce::Colour(0xff666666));
    g.drawRoundedRectangle(hourglassBounds, 5.0f, 2.0f);
    
    auto centerX = hourglassBounds.getCentreX();
    auto topY = hourglassBounds.getY() + 10;
    auto bottomY = hourglassBounds.getBottom() - 10;
    auto width = hourglassBounds.getWidth() * 0.6f;
    auto middleY = hourglassBounds.getCentreY();
    
    juce::Path hourglass;
    hourglass.startNewSubPath(centerX - width/2, topY);
    hourglass.lineTo(centerX + width/2, topY);
    hourglass.lineTo(centerX + 3, middleY);
    hourglass.lineTo(centerX + width/2, bottomY);
    hourglass.lineTo(centerX - width/2, bottomY);
    hourglass.lineTo(centerX - 3, middleY);
    hourglass.closeSubPath();
    
    g.setColour(juce::Colour(0xff999999));
    g.strokePath(hourglass, juce::PathStrokeType(2.0f));
    
    float sandLevel = 0.3f + 0.2f * std::sin(animationPhase);
    float topSandY = topY + (middleY - topY) * (1.0f - sandLevel);
    float bottomSandY = middleY + (bottomY - middleY) * sandLevel;
    
    g.setColour(juce::Colour(0xffffcc66));
    
    juce::Path topSand;
    topSand.startNewSubPath(centerX - width/2 + 5, topSandY);
    topSand.lineTo(centerX + width/2 - 5, topSandY);
    topSand.lineTo(centerX + 2, middleY - 2);
    topSand.lineTo(centerX - 2, middleY - 2);
    topSand.closeSubPath();
    g.fillPath(topSand);
    
    juce::Path bottomSand;
    bottomSand.startNewSubPath(centerX - 2, middleY + 2);
    bottomSand.lineTo(centerX + 2, middleY + 2);
    bottomSand.lineTo(centerX + width/2 - 5, bottomSandY);
    bottomSand.lineTo(centerX - width/2 + 5, bottomSandY);
    bottomSand.closeSubPath();
    g.fillPath(bottomSand);
    
    g.setColour(juce::Colours::orange.withAlpha(0.5f));
    g.drawLine(centerX, middleY - 1, centerX, middleY + 1, 1.0f);
}

void HourglassDisplay::timerCallback()
{
    animationPhase += 0.05f;
    if (animationPhase > juce::MathConstants<float>::twoPi)
        animationPhase -= juce::MathConstants<float>::twoPi;
    repaint();
}

ReversinatorAudioProcessorEditor::ReversinatorAudioProcessorEditor (ReversinatorAudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p)
{
    // Allow flexible resizing with proper scaling
    setResizeLimits(400, 300, 1200, 1000);
    setSize (defaultWidth, defaultHeight);
    
    reverserButton.setButtonText("REVERSER");
    reverserButton.setToggleState(false, juce::dontSendNotification);
    addAndMakeVisible(reverserButton);
    
    timeSlider.setSliderStyle(juce::Slider::LinearVertical);
    timeSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    timeSlider.setColour(juce::Slider::backgroundColourId, juce::Colour(0xff666666));
    timeSlider.setColour(juce::Slider::trackColourId, juce::Colour(0xffffcc66));
    timeSlider.setColour(juce::Slider::thumbColourId, juce::Colour(0xffffcc66));
    timeSlider.addListener(this);
    addAndMakeVisible(timeSlider);
    
    timeLabel.setText("TIME", juce::dontSendNotification);
    timeLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(timeLabel);
    
    timeValueLabel.setText("2.00 sec", juce::dontSendNotification);
    timeValueLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(timeValueLabel);
    
    feedbackSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    feedbackSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    feedbackSlider.setColour(juce::Slider::backgroundColourId, juce::Colour(0xff666666));
    feedbackSlider.setColour(juce::Slider::trackColourId, juce::Colour(0xff999999));
    addAndMakeVisible(feedbackSlider);
    
    feedbackLabel.setText("FEEDBACK DEPTH", juce::dontSendNotification);
    feedbackLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(feedbackLabel);
    
    wetMixSlider.setSliderStyle(juce::Slider::LinearVertical);
    wetMixSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    wetMixSlider.setColour(juce::Slider::backgroundColourId, juce::Colour(0xff666666));
    wetMixSlider.setColour(juce::Slider::trackColourId, juce::Colour(0xff6699ff));
    addAndMakeVisible(wetMixSlider);
    
    wetMixLabel.setText("WET", juce::dontSendNotification);
    wetMixLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(wetMixLabel);
    
    dryMixSlider.setSliderStyle(juce::Slider::LinearVertical);
    dryMixSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    dryMixSlider.setColour(juce::Slider::backgroundColourId, juce::Colour(0xff666666));
    dryMixSlider.setColour(juce::Slider::trackColourId, juce::Colour(0xff6699ff));
    addAndMakeVisible(dryMixSlider);
    
    dryMixLabel.setText("DRY", juce::dontSendNotification);
    dryMixLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(dryMixLabel);
    
    volumeSlider.setSliderStyle(juce::Slider::LinearVertical);
    volumeSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    volumeSlider.setRange(0.0, 1.0);
    volumeSlider.setValue(1.0);
    volumeSlider.setColour(juce::Slider::backgroundColourId, juce::Colour(0xff666666));
    volumeSlider.setColour(juce::Slider::trackColourId, juce::Colour(0xff6699ff));
    addAndMakeVisible(volumeSlider);
    
    volumeLabel.setText("Volume\nControl", juce::dontSendNotification);
    volumeLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(volumeLabel);
    
    addAndMakeVisible(reversePlaybackButton);
    addAndMakeVisible(forwardBackwardsButton);
    addAndMakeVisible(reverseRepeatButton);
    addAndMakeVisible(hourglassDisplay);
    
    reversePlaybackButton.onClick = [this]() {
        audioProcessor.getValueTreeState().getParameter("mode")->setValueNotifyingHost(0.0f);
        updateModeButtons(0);
    };
    
    forwardBackwardsButton.onClick = [this]() {
        audioProcessor.getValueTreeState().getParameter("mode")->setValueNotifyingHost(0.5f);
        updateModeButtons(1);
    };
    
    reverseRepeatButton.onClick = [this]() {
        audioProcessor.getValueTreeState().getParameter("mode")->setValueNotifyingHost(1.0f);
        updateModeButtons(2);
    };
    
    updateModeButtons(0);
    
    reverserAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "reverser", reverserButton);
    timeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "time", timeSlider);
    feedbackAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "feedback", feedbackSlider);
    wetMixAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "wetmix", wetMixSlider);
    dryMixAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "drymix", dryMixSlider);
    
    // Setup website link
    websiteLink.setButtonText("www.sweetjusticesound.com");
    websiteLink.setURL(juce::URL("https://www.sweetjusticesound.com"));
    websiteLink.setJustificationType(juce::Justification::centred);
    websiteLink.setColour(juce::HyperlinkButton::textColourId, juce::Colours::lightblue);
    websiteLink.setTooltip("Visit Sweet Justice Sound website for more plugins and music");
    addAndMakeVisible(websiteLink);
    
    // Setup about button
    aboutButton.setColour(juce::TextButton::buttonColourId, darkPanelColour);
    aboutButton.setColour(juce::TextButton::textColourOnId, lightTextColour);
    aboutButton.onClick = [this] 
    {
        if (!aboutWindow)
            aboutWindow = std::make_unique<AboutWindow>();
        else
            aboutWindow->setVisible(true);
    };
    addAndMakeVisible(aboutButton);
}

ReversinatorAudioProcessorEditor::~ReversinatorAudioProcessorEditor()
{
}

void ReversinatorAudioProcessorEditor::paint (juce::Graphics& g)
{
    // Calculate scale factor and center offset
    float scaledWidth = defaultWidth * currentScale;
    float scaledHeight = defaultHeight * currentScale;
    float xOffset = (getWidth() - scaledWidth) * 0.5f;
    float yOffset = (getHeight() - scaledHeight) * 0.5f;
    
    // Apply scaling and centering transform
    g.addTransform(juce::AffineTransform::scale(currentScale)
                  .translated(xOffset / currentScale, yOffset / currentScale));
    
    // Draw at default size (the transform will scale it)
    auto scaledBounds = juce::Rectangle<int>(0, 0, defaultWidth, defaultHeight);
    
    g.fillAll(backgroundColour);
    
    auto topBar = scaledBounds.removeFromTop(60);
    g.setColour(darkPanelColour);
    g.fillRect(topBar);
    
    g.setColour(lightTextColour);
    g.setFont(juce::Font(24.0f, juce::Font::bold));
    g.drawText("BACKWARDS", topBar.removeFromLeft(200).withTrimmedTop(15), juce::Justification::centred);
    g.setFont(juce::Font(16.0f));
    g.drawText("MACHINE", topBar.removeFromLeft(100).withTrimmedTop(20), juce::Justification::left);
    
    auto bounds = scaledBounds.withTrimmedTop(60);
    
    auto leftPanel = bounds.removeFromLeft(200).reduced(10);
    g.setColour(panelColour);
    g.fillRoundedRectangle(leftPanel.toFloat(), 10.0f);
    g.setColour(darkPanelColour);
    g.drawRoundedRectangle(leftPanel.toFloat(), 10.0f, 2.0f);
    
    auto centerPanel = bounds.removeFromLeft(300).reduced(10);
    g.setColour(panelColour);
    g.fillRoundedRectangle(centerPanel.toFloat(), 10.0f);
    g.setColour(darkPanelColour);
    g.drawRoundedRectangle(centerPanel.toFloat(), 10.0f, 2.0f);
    
    auto rightPanel = bounds.reduced(10);
    g.setColour(darkPanelColour);
    g.fillRoundedRectangle(rightPanel.toFloat(), 10.0f);
    
    auto waveformArea = topBar.withX(defaultWidth - 200).withWidth(180).reduced(5);
    g.setColour(juce::Colour(0xff000000));
    g.fillRoundedRectangle(waveformArea.toFloat(), 5.0f);
    
    g.setColour(juce::Colour(0xff00ff00));
    juce::Path waveform;
    float centerY = waveformArea.getCentreY();
    waveform.startNewSubPath(waveformArea.getX(), centerY);
    for (int x = 0; x < waveformArea.getWidth(); x += 2)
    {
        float y = centerY + std::sin(x * 0.1f) * 10.0f * std::sin(x * 0.02f);
        waveform.lineTo(waveformArea.getX() + x, y);
    }
    g.strokePath(waveform, juce::PathStrokeType(1.0f));
}

void ReversinatorAudioProcessorEditor::resized()
{
    // Calculate scale factor based on current window size
    float widthScale = static_cast<float>(getWidth()) / static_cast<float>(defaultWidth);
    float heightScale = static_cast<float>(getHeight()) / static_cast<float>(defaultHeight);
    currentScale = juce::jmin(widthScale, heightScale);
    
    // Calculate center offset
    float scaledWidth = defaultWidth * currentScale;
    float scaledHeight = defaultHeight * currentScale;
    float xOffset = (getWidth() - scaledWidth) * 0.5f;
    float yOffset = (getHeight() - scaledHeight) * 0.5f;
    
    // Create a transform to scale and center all components
    auto transform = juce::AffineTransform::scale(currentScale)
                    .translated(xOffset, yOffset);
    
    // Apply the transform to all child components
    for (auto* child : getChildren())
        child->setTransform(transform);
    
    // Layout components at their default positions (unscaled)
    auto defaultBounds = juce::Rectangle<int>(0, 0, defaultWidth, defaultHeight);
    
    // Website link at the top
    websiteLink.setBounds(defaultBounds.removeFromTop(20));
    
    auto bounds = defaultBounds.withTrimmedTop(40);
    
    auto leftPanel = bounds.removeFromLeft(200).reduced(20);
    auto buttonSize = 60;
    auto buttonY = leftPanel.getY() + 20;
    
    reversePlaybackButton.setBounds(leftPanel.getX() + (leftPanel.getWidth() - buttonSize) / 2, 
                                   buttonY, buttonSize, buttonSize + 20);
    forwardBackwardsButton.setBounds(leftPanel.getX() + (leftPanel.getWidth() - buttonSize) / 2, 
                                    buttonY + 90, buttonSize, buttonSize + 20);
    reverseRepeatButton.setBounds(leftPanel.getX() + (leftPanel.getWidth() - buttonSize) / 2, 
                                 buttonY + 180, buttonSize, buttonSize + 20);
    
    auto centerPanel = bounds.removeFromLeft(300).reduced(20);
    
    reverserButton.setBounds(centerPanel.getX() + 170, centerPanel.getY() + 20, 100, 30);
    
    hourglassDisplay.setBounds(centerPanel.getX() + 20, centerPanel.getY() + 70, 80, 120);
    
    timeSlider.setBounds(centerPanel.getX() + 120, centerPanel.getY() + 70, 30, 120);
    timeLabel.setBounds(centerPanel.getX() + 90, centerPanel.getY() + 195, 80, 20);
    timeValueLabel.setBounds(centerPanel.getX() + 20, centerPanel.getY() + 50, 80, 20);
    
    feedbackSlider.setBounds(centerPanel.getX() + 20, centerPanel.getY() + 250, 250, 30);
    feedbackLabel.setBounds(centerPanel.getX() + 20, centerPanel.getY() + 285, 250, 20);
    
    auto rightPanel = bounds.reduced(20);
    
    volumeLabel.setBounds(rightPanel.getX(), rightPanel.getY() + 10, 80, 40);
    
    wetMixLabel.setBounds(rightPanel.getX() + 10, rightPanel.getY() + 200, 30, 20);
    wetMixSlider.setBounds(rightPanel.getX() + 10, rightPanel.getY() + 60, 30, 140);
    
    dryMixLabel.setBounds(rightPanel.getX() + 50, rightPanel.getY() + 200, 30, 20);
    dryMixSlider.setBounds(rightPanel.getX() + 50, rightPanel.getY() + 60, 30, 140);
    
    // About button at the bottom right
    aboutButton.setBounds(defaultWidth - 70, defaultHeight - 30, 60, 25);
}

void ReversinatorAudioProcessorEditor::sliderValueChanged(juce::Slider* slider)
{
    if (slider == &timeSlider)
    {
        float value = slider->getValue();
        timeValueLabel.setText(juce::String::formatted("%.2f sec", value), juce::dontSendNotification);
        hourglassDisplay.setTimeValue(value);
    }
}

void ReversinatorAudioProcessorEditor::updateModeButtons(int mode)
{
    reversePlaybackButton.setSelected(mode == 0);
    forwardBackwardsButton.setSelected(mode == 1);
    reverseRepeatButton.setSelected(mode == 2);
}