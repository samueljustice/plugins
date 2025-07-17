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
ReversinatorAudioProcessorEditor::ReversinatorAudioProcessorEditor (ReversinatorAudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p)
{
    // Set up look and feel
    lookAndFeel.setColour(juce::Slider::textBoxTextColourId, juce::Colours::black);
    lookAndFeel.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::lightgrey);
    lookAndFeel.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::grey);
    setLookAndFeel(&lookAndFeel);
    
    // Set size and resizing at the end
    setSize (defaultWidth, defaultHeight);
    setResizable(true, true);
    setResizeLimits(400, 320, 1000, 800);
    
    // Website link
    websiteLink.setButtonText("www.sweetjusticesound.com");
    websiteLink.setURL(juce::URL("https://www.sweetjusticesound.com"));
    websiteLink.setJustificationType(juce::Justification::centred);
    websiteLink.setColour(juce::HyperlinkButton::textColourId, juce::Colours::lightblue);
    websiteLink.setTooltip("Visit Sweet Justice Sound website for more plugins and music");
    addAndMakeVisible(websiteLink);
    
    // Title
    titleLabel.setText("SammyJs Reversinator", juce::dontSendNotification);
    titleLabel.setFont(juce::Font(24.0f, juce::Font::bold));
    titleLabel.setJustificationType(juce::Justification::centred);
    titleLabel.setTooltip("Real-time audio reversing effect");
    addAndMakeVisible(titleLabel);
    
    // Mode selector
    modeLabel.setText("Effect Mode", juce::dontSendNotification);
    modeLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(modeLabel);
    
    modeSelector.addItem("Reverse Playback", 1);
    modeSelector.addItem("Forward Backwards", 2);
    modeSelector.addItem("Reverse Repeat", 3);
    modeSelector.setSelectedId(1);
    modeSelector.setJustificationType(juce::Justification::centred);
    modeSelector.setTooltip("Select the reverse effect mode: Reverse Playback (continuous reverse), Forward Backwards (smooth crossfade), or Reverse Repeat (double playback with vibrato)");
    addAndMakeVisible(modeSelector);
    
    // Reverser toggle
    reverserLabel.setText("Reverser", juce::dontSendNotification);
    reverserLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(reverserLabel);
    
    reverserButton.setButtonText("Enable");
    reverserButton.setToggleState(false, juce::dontSendNotification);
    reverserButton.setTooltip("Enable or disable the reverse effect");
    addAndMakeVisible(reverserButton);
    
    // Time slider
    setupSlider(timeSlider, timeLabel, timeValueLabel, "Window Time", " s");
    timeSlider.setRange(0.03, 2.0, 0.001);  // min 30ms to prevent crackling
    timeSlider.setSkewFactorFromMidPoint(0.5);
    timeSlider.setDoubleClickReturnValue(true, 0.5);
    timeSlider.setTooltip("Size of the reverse window in seconds (30ms - 2s). Smaller values create granular effects, larger values create smoother reverses.");
    
    // Feedback slider
    setupSlider(feedbackSlider, feedbackLabel, feedbackValueLabel, "Feedback Depth", "%");
    feedbackSlider.setDoubleClickReturnValue(true, 0.0);
    feedbackSlider.setTooltip("Amount of feedback applied to the reversed signal. Creates echo-like effects.");
    
    // Wet mix slider
    setupSlider(wetMixSlider, wetMixLabel, wetMixValueLabel, "Wet Mix", "%");
    wetMixSlider.setDoubleClickReturnValue(true, 100.0);
    wetMixSlider.setTooltip("Level of the reversed signal. 100% = fully reversed, 0% = no reversed signal.");
    
    // Dry mix slider
    setupSlider(dryMixSlider, dryMixLabel, dryMixValueLabel, "Dry Mix", "%");
    dryMixSlider.setDoubleClickReturnValue(true, 0.0);
    dryMixSlider.setTooltip("Level of the original signal. Mix with wet signal for blended effects.");
    
    // Crossfade slider (hidden by default)
    setupSlider(crossfadeSlider, crossfadeLabel, crossfadeValueLabel, "Crossfade", "%");
    crossfadeSlider.setDoubleClickReturnValue(true, 20.0);
    crossfadeSlider.setTooltip("Crossfade time between forward and backward sections in Forward Backwards mode. Lower = sharper transitions, Higher = smoother blending.");
    crossfadeSlider.setVisible(false);
    crossfadeLabel.setVisible(false);
    crossfadeValueLabel.setVisible(false);
    
    // About button
    aboutButton.setColour(juce::TextButton::buttonColourId, juce::Colours::darkgrey.darker());
    aboutButton.setColour(juce::TextButton::textColourOnId, juce::Colours::lightgrey);
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
    slider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 80, 20);
    slider.setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(0xff4a90e2));
    slider.setColour(juce::Slider::textBoxTextColourId, juce::Colours::black);
    slider.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::white);
    slider.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::grey);
    slider.setTextValueSuffix(suffix);
    addAndMakeVisible(slider);
    
    label.setText(labelText, juce::dontSendNotification);
    label.setJustificationType(juce::Justification::centred);
    label.setColour(juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible(label);
    
    valueLabel.setJustificationType(juce::Justification::centred);
    valueLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    addAndMakeVisible(valueLabel);
}

void ReversinatorAudioProcessorEditor::paint (juce::Graphics& g)
{
    // Fill the entire component with dark background first
    g.fillAll(juce::Colour(0xff1a1a1a));
    
    // Calculate scale factor and center offset
    float scaledWidth = defaultWidth * currentScale;
    float scaledHeight = defaultHeight * currentScale;
    float xOffset = (getWidth() - scaledWidth) * 0.5f;
    float yOffset = (getHeight() - scaledHeight) * 0.5f;
    
    // Apply scaling and centering transform
    g.addTransform(juce::AffineTransform::scale(currentScale)
                  .translated(xOffset / currentScale, yOffset / currentScale));
    
    // Draw background gradient
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff2a2a2a), 0, 0,
                                           juce::Colour(0xff1a1a1a), 0, defaultHeight, false));
    g.fillRect(0, 0, defaultWidth, defaultHeight);
    
    // Draw sections with more space for top controls
    g.setColour(juce::Colour(0xff3a3a3a));
    g.fillRoundedRectangle(10, 70, defaultWidth - 20, 55, 5.0f);  // Reverser section - bigger
    g.fillRoundedRectangle(10, 135, defaultWidth - 20, 55, 5.0f);  // Mode section - bigger
    g.fillRoundedRectangle(10, 200, defaultWidth - 20, 140, 5.0f); // Controls section
    
    g.setColour(juce::Colours::grey);
    g.drawRoundedRectangle(10, 70, defaultWidth - 20, 55, 5.0f, 1.0f);
    g.drawRoundedRectangle(10, 135, defaultWidth - 20, 55, 5.0f, 1.0f);
    g.drawRoundedRectangle(10, 200, defaultWidth - 20, 140, 5.0f, 1.0f);
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
    auto reverserArea = area.removeFromTop(65).reduced(20, 10);
    auto reverserRow = reverserArea.withSizeKeepingCentre(200, reverserArea.getHeight());
    auto labelArea = reverserRow.removeFromLeft(80);
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
    int numSliders = isForwardBackwards ? 5 : 4;
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
    }
    else
    {
        // 4 sliders layout - properly centered
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
    }
    
    // About button at bottom - less bottom space
    auto bottomArea = area.removeFromBottom(20);
    aboutButton.setBounds(bottomArea.removeFromRight(60).withSizeKeepingCentre(50, 20));
}