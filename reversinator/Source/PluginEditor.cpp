#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "CustomFonts.h"

//==============================================================================
// AboutWindow implementation
AboutWindow::AboutWindow()
    : DocumentWindow("About SammyJs Reversinator", 
                     juce::Colour(0xff75fb87),
                     juce::DocumentWindow::closeButton)
{
    setUsingNativeTitleBar(true);
    content = std::make_unique<AboutContent>();
    setContentOwned(content.release(), false);
    
    centreWithSize(500, 600);
    setVisible(true);
    setResizable(false, false);
    setAlwaysOnTop(true);
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
    juce::Colour accentColor = juce::Colours::black;
    
    titleLabel.setText("SammyJs Reversinator", juce::dontSendNotification);
    titleLabel.setFont(getCustomFonts()->getFont(24.0f, juce::Font::bold));
    titleLabel.setJustificationType(juce::Justification::centred);
    titleLabel.setColour(juce::Label::textColourId, accentColor);
    addAndMakeVisible(titleLabel);
    
    juce::String versionText = "Version " PLUGIN_VERSION;
    versionLabel.setText(versionText, juce::dontSendNotification);
    versionLabel.setFont(getCustomFonts()->getFont(16.0f));
    versionLabel.setJustificationType(juce::Justification::centred);
    versionLabel.setColour(juce::Label::textColourId, juce::Colours::black);
    addAndMakeVisible(versionLabel);
    
    authorLabel.setText("Created by Samuel Justice", juce::dontSendNotification);
    authorLabel.setFont(getCustomFonts()->getFont(14.0f));
    authorLabel.setJustificationType(juce::Justification::centred);
    authorLabel.setColour(juce::Label::textColourId, juce::Colours::black);
    addAndMakeVisible(authorLabel);
    
    emailButton.setButtonText("sam@sweetjusticesound.com");
    emailButton.setURL(juce::URL("mailto:sam@sweetjusticesound.com"));
    emailButton.setFont(getCustomFonts()->getFont(14.0f), false);
    emailButton.setColour(juce::HyperlinkButton::textColourId, accentColor);
    addAndMakeVisible(emailButton);
    
    websiteButton.setButtonText("www.sweetjusticesound.com");
    websiteButton.setURL(juce::URL("https://www.sweetjusticesound.com"));
    websiteButton.setFont(getCustomFonts()->getFont(14.0f), false);
    websiteButton.setColour(juce::HyperlinkButton::textColourId, accentColor);
    addAndMakeVisible(websiteButton);
    
    checkUpdateButton.setButtonText("Check for Updates");
    checkUpdateButton.onClick = [this] { checkForUpdates(); };
    checkUpdateButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff65ad6b));
    checkUpdateButton.setColour(juce::TextButton::textColourOnId, juce::Colours::black);
    checkUpdateButton.setColour(juce::TextButton::textColourOffId, juce::Colours::black);
    addAndMakeVisible(checkUpdateButton);
    
    updateStatusLabel.setText("", juce::dontSendNotification);
    updateStatusLabel.setJustificationType(juce::Justification::centred);
    updateStatusLabel.setFont(getCustomFonts()->getFont(12.0f));
    updateStatusLabel.setColour(juce::Label::textColourId, juce::Colours::black);
    addAndMakeVisible(updateStatusLabel);
    
    licenseInfo.setMultiLine(true);
    licenseInfo.setReadOnly(true);
    licenseInfo.setScrollbarsShown(true);
    licenseInfo.setCaretVisible(false);
    licenseInfo.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff75fb87).darker(0.3f));
    licenseInfo.setColour(juce::TextEditor::textColourId, juce::Colours::black);
    licenseInfo.setColour(juce::TextEditor::outlineColourId, juce::Colour(0xffd22d66).withAlpha(0.3f));
    licenseInfo.setFont(getCustomFonts()->getFont(12.0f));
    
    juce::String licenseText = 
        "Real-time Audio Reversing Effect\n\n"
        "Inspired by the classic Backwards Machine plugin\n\n"
        "Technologies Used:\n\n"
        "JUCE Framework\n"
        "Copyright (c) 2022 - Raw Material Software Limited\n"
        "Licensed under the GPL/Commercial license\n\n"
        
        "Features:\n"
        "- Reverse Playback - Continuous reverse effect\n"
        "- Forward Backwards - Smooth crossfade\n"
        "- Reverse Repeat - Double playback with vibrato\n"
        "- Adjustable window time (30ms - 2 seconds)\n"
        "- Feedback control\n"
        "- Wet/Dry mix controls";
    
    licenseInfo.setText(licenseText);
    addAndMakeVisible(licenseInfo);
}

void AboutWindow::AboutContent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff75fb87));
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
ReversinatorAudioProcessorEditor::ReversinatorAudioProcessorEditor (ReversinatorAudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p)
{
    // Define our color scheme
    juce::Colour accentColor = juce::Colours::black;
    juce::Colour backgroundGreen(0xff75fb87);
    juce::Colour sectionGreen(0xff65ad6b);
    
    // Set up look and feel with our color scheme
    lookAndFeel.setColour(juce::Slider::textBoxTextColourId, juce::Colours::black);
    lookAndFeel.setColour(juce::Slider::textBoxBackgroundColourId, backgroundGreen.darker(0.2f));
    lookAndFeel.setColour(juce::Slider::textBoxOutlineColourId, accentColor.withAlpha(0.5f));
    lookAndFeel.setColour(juce::Slider::rotarySliderFillColourId, accentColor);
    lookAndFeel.setColour(juce::Slider::rotarySliderOutlineColourId, sectionGreen.brighter(0.3f));
    lookAndFeel.setColour(juce::Slider::thumbColourId, accentColor);
    lookAndFeel.setColour(juce::Slider::trackColourId, sectionGreen.brighter(0.2f));
    lookAndFeel.setColour(juce::Slider::backgroundColourId, sectionGreen.darker(0.3f));
    setLookAndFeel(&lookAndFeel);
    
    // Set size and resizing at the end
    setSize (defaultWidth, defaultHeight);
    setResizable(true, true);
    setResizeLimits(600, 320, 1200, 800);
    
    // Website link
    websiteLink.setButtonText("www.sweetjusticesound.com");
    websiteLink.setURL(juce::URL("https://www.sweetjusticesound.com"));
    websiteLink.setJustificationType(juce::Justification::centred);
    websiteLink.setFont(getCustomFonts()->getFont(14.0f, juce::Font::bold), false);
    websiteLink.setColour(juce::HyperlinkButton::textColourId, accentColor);
    websiteLink.setTooltip("Visit Sweet Justice Sound website for more plugins and music");
    addAndMakeVisible(websiteLink);
    
    // Title
    titleLabel.setText("SammyJs Reversinator", juce::dontSendNotification);
    titleLabel.setFont(getCustomFonts()->getFont(28.0f, juce::Font::bold));
    titleLabel.setJustificationType(juce::Justification::centred);
    titleLabel.setColour(juce::Label::textColourId, accentColor);
    titleLabel.setTooltip("Real-time audio reversing effect");
    addAndMakeVisible(titleLabel);
    
    // Mode selector
    modeLabel.setText("Effect Mode", juce::dontSendNotification);
    modeLabel.setFont(getCustomFonts()->getFont(16.0f, juce::Font::bold));
    modeLabel.setJustificationType(juce::Justification::centred);
    modeLabel.setColour(juce::Label::textColourId, accentColor);
    modeLabel.setTooltip("Select the reverse effect mode");
    addAndMakeVisible(modeLabel);
    
    modeSelector.addItem("Reverse Playback", 1);
    modeSelector.addItem("Forward Backwards", 2);
    modeSelector.addItem("Reverse Repeat", 3);
    modeSelector.setSelectedId(1);
    modeSelector.setJustificationType(juce::Justification::centred);
    // Note: ComboBox doesn't have setFont method
    modeSelector.setColour(juce::ComboBox::backgroundColourId, sectionGreen.darker(0.2f));
    modeSelector.setColour(juce::ComboBox::textColourId, juce::Colours::black);
    modeSelector.setColour(juce::ComboBox::outlineColourId, accentColor.withAlpha(0.5f));
    modeSelector.setColour(juce::ComboBox::arrowColourId, accentColor);
    modeSelector.setTooltip("Select the reverse effect mode: Reverse Playback (continuous reverse), Forward Backwards (smooth crossfade), or Reverse Repeat (double playback with vibrato)");
    addAndMakeVisible(modeSelector);
    
    // Reverser toggle
    reverserLabel.setText("Reverser", juce::dontSendNotification);
    reverserLabel.setFont(getCustomFonts()->getFont(16.0f, juce::Font::bold));
    reverserLabel.setJustificationType(juce::Justification::centred);
    reverserLabel.setColour(juce::Label::textColourId, accentColor);
    reverserLabel.setTooltip("Enable or disable the reverse effect");
    addAndMakeVisible(reverserLabel);
    
    reverserButton.setButtonText("Enable");
    reverserButton.setToggleState(false, juce::dontSendNotification);
    reverserButton.setColour(juce::ToggleButton::textColourId, juce::Colours::black);
    // Note: ToggleButton doesn't have textColourOnId
    reverserButton.setColour(juce::ToggleButton::tickColourId, juce::Colours::black);
    reverserButton.setColour(juce::ToggleButton::tickDisabledColourId, sectionGreen.darker());
    reverserButton.setTooltip("Enable or disable the reverse effect");
    addAndMakeVisible(reverserButton);
    
    // Time slider
    setupSlider(timeSlider, timeLabel, timeValueLabel, "Window Time", " s");
    timeSlider.setRange(0.03, 5.0, 0.001);  // min 30ms, max 5s
    timeSlider.setSkewFactorFromMidPoint(0.5);
    timeSlider.setDoubleClickReturnValue(true, 2.0);
    auto timeTooltip = "Size of the reverse window in seconds (30ms - 5s). Smaller values create granular effects, larger values create smoother reverses.";
    timeSlider.setTooltip(timeTooltip);
    timeLabel.setTooltip(timeTooltip);
    
    // Feedback slider
    setupSlider(feedbackSlider, feedbackLabel, feedbackValueLabel, "Feedback Depth", "%");
    feedbackSlider.setDoubleClickReturnValue(true, 0.0);
    auto feedbackTooltip = "Amount of feedback applied to the reversed signal. Creates echo-like effects.";
    feedbackSlider.setTooltip(feedbackTooltip);
    feedbackLabel.setTooltip(feedbackTooltip);
    
    // Wet mix slider
    setupSlider(wetMixSlider, wetMixLabel, wetMixValueLabel, "Wet Mix", "%");
    wetMixSlider.setDoubleClickReturnValue(true, 100.0);
    auto wetTooltip = "Level of the reversed signal. 100% = fully reversed, 0% = no reversed signal.";
    wetMixSlider.setTooltip(wetTooltip);
    wetMixLabel.setTooltip(wetTooltip);
    
    // Dry mix slider
    setupSlider(dryMixSlider, dryMixLabel, dryMixValueLabel, "Dry Mix", "%");
    dryMixSlider.setDoubleClickReturnValue(true, 0.0);
    auto dryTooltip = "Level of the original signal. Mix with wet signal for blended effects.";
    dryMixSlider.setTooltip(dryTooltip);
    dryMixLabel.setTooltip(dryTooltip);
    
    // Crossfade slider (hidden by default)
    setupSlider(crossfadeSlider, crossfadeLabel, crossfadeValueLabel, "Crossfade", "%");
    crossfadeSlider.setDoubleClickReturnValue(true, 20.0);
    auto crossfadeTooltip = "Crossfade time between forward and backward sections in Forward Backwards mode. Lower = sharper transitions, Higher = smoother blending.";
    crossfadeSlider.setTooltip(crossfadeTooltip);
    crossfadeLabel.setTooltip(crossfadeTooltip);
    crossfadeSlider.setVisible(false);
    crossfadeLabel.setVisible(false);
    crossfadeValueLabel.setVisible(false);
    
    // Envelope slider
    setupSlider(envelopeSlider, envelopeLabel, envelopeValueLabel, "Envelope", " ms");
    envelopeSlider.setRange(10.0, 100.0, 1.0);
    envelopeSlider.setDoubleClickReturnValue(true, 30.0);
    auto envelopeTooltip = "Fade in/out time for each reversed segment. Lower = sharper transitions, Higher = smoother transitions.";
    envelopeSlider.setTooltip(envelopeTooltip);
    envelopeLabel.setTooltip(envelopeTooltip);
    
    // About button
    aboutButton.setButtonText("About");
    aboutButton.setColour(juce::TextButton::buttonColourId, sectionGreen.darker(0.2f));
    aboutButton.setColour(juce::TextButton::textColourOnId, juce::Colours::black);
    aboutButton.setColour(juce::TextButton::textColourOffId, juce::Colours::black);
    aboutButton.onClick = [this] 
    {
        if (!aboutWindow)
            aboutWindow = std::make_unique<AboutWindow>();
        else
            aboutWindow->setVisible(true);
    };
    aboutButton.setTooltip("About SammyJs Reversinator");
    addAndMakeVisible(aboutButton);
    
    // Create attachments
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
    modeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        audioProcessor.getValueTreeState(), "mode", modeSelector);
    crossfadeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "crossfade", crossfadeSlider);
    envelopeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "envelope", envelopeSlider);
    
    // Add mode change listener
    modeSelector.onChange = [this]
    {
        bool isForwardBackwards = (modeSelector.getSelectedId() == 2);
        crossfadeSlider.setVisible(isForwardBackwards);
        crossfadeLabel.setVisible(isForwardBackwards);
        crossfadeValueLabel.setVisible(isForwardBackwards);
        resized();
    };
}

ReversinatorAudioProcessorEditor::~ReversinatorAudioProcessorEditor()
{
    setLookAndFeel(nullptr);
}

void ReversinatorAudioProcessorEditor::setupSlider(juce::Slider& slider, juce::Label& label, 
                                                  juce::Label& valueLabel, const juce::String& labelText, 
                                                  const juce::String& suffix)
{
    // Define colors
    juce::Colour accentColor = juce::Colours::black;
    juce::Colour backgroundGreen(0xff75fb87);
    juce::Colour sectionGreen(0xff65ad6b);
    
    slider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 80, 20);
    slider.setColour(juce::Slider::rotarySliderFillColourId, accentColor);
    slider.setColour(juce::Slider::textBoxTextColourId, juce::Colours::black);
    slider.setColour(juce::Slider::textBoxBackgroundColourId, backgroundGreen.darker(0.2f));
    slider.setColour(juce::Slider::textBoxOutlineColourId, accentColor.withAlpha(0.5f));
    slider.setTextValueSuffix(suffix);
    addAndMakeVisible(slider);
    
    label.setText(labelText, juce::dontSendNotification);
    label.setFont(getCustomFonts()->getFont(16.0f, juce::Font::bold));
    label.setJustificationType(juce::Justification::centred);
    label.setColour(juce::Label::textColourId, accentColor);
    addAndMakeVisible(label);
    
    valueLabel.setJustificationType(juce::Justification::centred);
    valueLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    addAndMakeVisible(valueLabel);
}

void ReversinatorAudioProcessorEditor::paint (juce::Graphics& g)
{
    // Define our color scheme
    juce::Colour backgroundGreen(0xff75fb87);
    juce::Colour sectionGreen(0xff65ad6b);
    juce::Colour accentColor = juce::Colours::black;
    
    // Fill the entire component with blue background first
    g.fillAll(backgroundGreen);
    
    // Calculate scale factor and center offset
    float scaledWidth = defaultWidth * currentScale;
    float scaledHeight = defaultHeight * currentScale;
    float xOffset = (getWidth() - scaledWidth) * 0.5f;
    float yOffset = (getHeight() - scaledHeight) * 0.5f;
    
    // Apply scaling and centering transform
    g.addTransform(juce::AffineTransform::scale(currentScale)
                  .translated(xOffset / currentScale, yOffset / currentScale));
    
    // Draw the main interface area with the same background
    g.setColour(backgroundGreen);
    g.fillRect(0, 0, defaultWidth, defaultHeight);
    
    // Draw sections with purple background
    g.setColour(sectionGreen);
    g.fillRoundedRectangle(10, 70, defaultWidth - 20, 55, 8.0f);  // Reverser section
    g.fillRoundedRectangle(10, 135, defaultWidth - 20, 55, 8.0f);  // Mode section
    g.fillRoundedRectangle(10, 200, defaultWidth - 20, 140, 8.0f); // Controls section
    
    // Draw subtle borders with accent color
    g.setColour(accentColor.withAlpha(0.3f));
    g.drawRoundedRectangle(10, 70, defaultWidth - 20, 55, 8.0f, 1.5f);
    g.drawRoundedRectangle(10, 135, defaultWidth - 20, 55, 8.0f, 1.5f);
    g.drawRoundedRectangle(10, 200, defaultWidth - 20, 140, 8.0f, 1.5f);
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
    {
        // Skip the ResizableCornerComponent which JUCE adds automatically
        if (dynamic_cast<juce::ResizableCornerComponent*>(child) == nullptr)
        {
            child->setTransform(transform);
        }
    }
    
    // Layout components at their default positions (unscaled)
    auto area = juce::Rectangle<int>(0, 0, defaultWidth, defaultHeight);
    
    // Top section
    websiteLink.setBounds(area.removeFromTop(18));
    titleLabel.setBounds(area.removeFromTop(35));
    area.removeFromTop(2);
    
    // Reverser section - properly centered
    auto reverserArea = area.removeFromTop(65).reduced(20, 5);  // Less vertical reduction
    reverserArea.removeFromTop(18);  // Add more space at top (was 8, now 18 for 10px lower)
    auto reverserRow = reverserArea.withSizeKeepingCentre(160, reverserArea.getHeight());  // Balanced width for natural text
    auto labelArea = reverserRow.removeFromLeft(75);  // Give text more room
    reverserLabel.setBounds(labelArea.withSizeKeepingCentre(labelArea.getWidth(), 20));
    reverserButton.setBounds(reverserRow.withSizeKeepingCentre(80, 25));
    
    area.removeFromTop(5);
    
    // Mode section - bigger
    auto modeArea = area.removeFromTop(65).reduced(20, 10);
    modeLabel.setBounds(modeArea.removeFromTop(20));
    auto modeWidth = 250;
    modeSelector.setBounds(modeArea.withSizeKeepingCentre(modeWidth, 28));
    
    area.removeFromTop(5);
    
    // Controls section
    auto controlsArea = area.removeFromTop(140).reduced(20, 10);
    auto sliderSize = 70;  // Smaller knobs
    
    // Check if crossfade is visible to adjust spacing
    bool isForwardBackwards = crossfadeSlider.isVisible();
    int numSliders = isForwardBackwards ? 6 : 5;  // Include envelope slider
    auto totalSliderWidth = numSliders * sliderSize;
    auto spacing = (controlsArea.getWidth() - totalSliderWidth) / (numSliders + 1);
    
    auto sliderY = controlsArea.getY() + 20;
    
    // Position sliders
    if (isForwardBackwards)
    {
        // 5 sliders layout - properly centered
        auto sliderX = controlsArea.getX() + spacing;
        
        timeLabel.setBounds(sliderX - 10, sliderY - 20, sliderSize + 20, 20);
        timeSlider.setBounds(sliderX, sliderY, sliderSize, sliderSize + 20);
        sliderX += sliderSize + spacing;
        
        crossfadeLabel.setBounds(sliderX - 10, sliderY - 20, sliderSize + 20, 20);
        crossfadeSlider.setBounds(sliderX, sliderY, sliderSize, sliderSize + 20);
        sliderX += sliderSize + spacing;
        
        feedbackLabel.setBounds(sliderX - 10, sliderY - 20, sliderSize + 20, 20);
        feedbackSlider.setBounds(sliderX, sliderY, sliderSize, sliderSize + 20);
        sliderX += sliderSize + spacing;
        
        wetMixLabel.setBounds(sliderX - 10, sliderY - 20, sliderSize + 20, 20);
        wetMixSlider.setBounds(sliderX, sliderY, sliderSize, sliderSize + 20);
        sliderX += sliderSize + spacing;
        
        dryMixLabel.setBounds(sliderX - 10, sliderY - 20, sliderSize + 20, 20);
        dryMixSlider.setBounds(sliderX, sliderY, sliderSize, sliderSize + 20);
        sliderX += sliderSize + spacing;
        
        envelopeLabel.setBounds(sliderX - 10, sliderY - 20, sliderSize + 20, 20);
        envelopeSlider.setBounds(sliderX, sliderY, sliderSize, sliderSize + 20);
    }
    else
    {
        // 5 sliders layout - properly centered
        auto startX = controlsArea.getX() + spacing;
        
        timeLabel.setBounds(startX - 10, sliderY - 20, sliderSize + 20, 20);
        timeSlider.setBounds(startX, sliderY, sliderSize, sliderSize + 20);
        startX += sliderSize + spacing;
        
        feedbackLabel.setBounds(startX - 10, sliderY - 20, sliderSize + 20, 20);
        feedbackSlider.setBounds(startX, sliderY, sliderSize, sliderSize + 20);
        startX += sliderSize + spacing;
        
        wetMixLabel.setBounds(startX - 10, sliderY - 20, sliderSize + 20, 20);
        wetMixSlider.setBounds(startX, sliderY, sliderSize, sliderSize + 20);
        startX += sliderSize + spacing;
        
        dryMixLabel.setBounds(startX - 10, sliderY - 20, sliderSize + 20, 20);
        dryMixSlider.setBounds(startX, sliderY, sliderSize, sliderSize + 20);
        startX += sliderSize + spacing;
        
        envelopeLabel.setBounds(startX - 10, sliderY - 20, sliderSize + 20, 20);
        envelopeSlider.setBounds(startX, sliderY, sliderSize, sliderSize + 20);
    }
    
    // About button at bottom - less bottom space
    auto bottomArea = area.removeFromBottom(20);
    aboutButton.setBounds(bottomArea.removeFromRight(60).withSizeKeepingCentre(50, 20));
}