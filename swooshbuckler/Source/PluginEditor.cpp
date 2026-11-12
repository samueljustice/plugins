#include "PluginEditor.h"
#include "Version.h"

namespace TC = Theme::Colours;

namespace
{

}

//==============================================================================
// SwellButton
void SwooshbucklerAudioProcessorEditor::SwellButton::timerCallback()
{
    constexpr float dt = 1.0f / 60.0f;
    const float omega = juce::MathConstants<float>::twoPi * 3.2f;
    m_velocity += (omega * omega * (m_target - m_scale) - 2.0f * 0.35f * omega * m_velocity) * dt;
    m_scale += m_velocity * dt;
    const float glow = m_glow + (m_processor.getEngine().getEnergy() - m_glow) * 0.2f;
    const bool changed = std::abs(m_velocity) > 1.0e-3f || std::abs(m_target - m_scale) > 1.0e-3f || std::abs(glow - m_glow) > 2.0e-3f;
    m_glow = glow;
    if (changed)
        repaint(); // only when something moved: idle repaints cost frame rate
}

void SwooshbucklerAudioProcessorEditor::SwellButton::paint(juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    const auto centre = b.getCentre();
    const float radius = juce::jmin(b.getWidth(), b.getHeight()) * 0.5f * 0.8f * m_scale;

    juce::ColourGradient halo(TC::flight.withAlpha(0.35f * m_glow), centre.x, centre.y,
                              TC::flight.withAlpha(0.0f), centre.x + radius * 1.25f, centre.y, true);
    g.setGradientFill(halo);
    g.fillEllipse(juce::Rectangle<float>(radius * 2.5f, radius * 2.5f).withCentre(centre));

    for (int i = 0; i < 8; ++i)
    {
        const float a = juce::MathConstants<float>::twoPi * static_cast<float>(i) / 8.0f + (1.0f - m_scale) * 2.0f;
        const juce::Point<float> dir { std::sin(a), -std::cos(a) };
        g.setColour(TC::brass.withAlpha(0.8f));
        g.drawLine(juce::Line<float>(centre + dir * radius * 0.96f, centre + dir * radius * 1.13f), 2.5f);
        g.fillEllipse(juce::Rectangle<float>(6.0f, 6.0f).withCentre(centre + dir * radius * 1.15f));
    }

    juce::ColourGradient body(TC::flight.brighter(0.2f), centre.x, centre.y - radius, TC::flight.darker(0.55f), centre.x, centre.y + radius, false);
    g.setGradientFill(body);
    g.fillEllipse(juce::Rectangle<float>(radius * 2.0f, radius * 2.0f).withCentre(centre));
    g.setColour(TC::brass.brighter(0.3f));
    g.drawEllipse(juce::Rectangle<float>(radius * 2.0f, radius * 2.0f).withCentre(centre), 2.5f);

    g.setColour(TC::background);
    g.setFont(m_fonts.display(radius * 0.5f));
    g.drawText("Swell", juce::Rectangle<float>(radius * 2.0f, radius * 0.6f).withCentre(centre.translated(0.0f, -radius * 0.08f)), juce::Justification::centred);
    g.setFont(m_fonts.label(radius * 0.13f));
    g.setColour(TC::background.withAlpha(0.7f));
    g.drawText("HOLD / SPACE / MIDI", juce::Rectangle<float>(radius * 2.0f, radius * 0.3f).withCentre(centre.translated(0.0f, radius * 0.36f)), juce::Justification::centred);
}

//==============================================================================
// TapeButton
void SwooshbucklerAudioProcessorEditor::TapeButton::timerCallback()
{
    m_blink += 1.0f / 30.0f;
    const bool capturing = m_processor.isCapturing(), armed = m_processor.isRecording();
    if (capturing)
        m_angle -= 0.18f; // anticlockwise: the reels' bottoms, and the tape between them, run left to right
    if (capturing || armed || m_wasArmed)
        repaint();
    m_wasArmed = armed;
}

void SwooshbucklerAudioProcessorEditor::TapeButton::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat().reduced(0.5f);
    const bool armed = m_processor.isRecording();
    const bool capturing = m_processor.isCapturing();

    g.setColour(armed ? TC::record.withAlpha(0.16f) : TC::panelRaised.brighter(isMouseOver() ? 0.1f : 0.0f));
    g.fillRoundedRectangle(b, 5.0f);
    g.setColour(armed ? TC::record.withAlpha(0.9f) : TC::outline);
    g.drawRoundedRectangle(b, 5.0f, 1.0f);

    // Cassette body with two reels and the tape between them
    auto icon = b.removeFromLeft(b.getHeight() * 1.6f).reduced(5.0f, 5.0f);
    g.setColour(TC::brass.withAlpha(0.85f));
    g.drawRoundedRectangle(icon, 3.0f, 1.2f);
    const float r = icon.getHeight() * 0.3f;
    const juce::Point<float> c1 { icon.getX() + icon.getWidth() * 0.3f, icon.getCentreY() - 1.0f };
    const juce::Point<float> c2 { icon.getX() + icon.getWidth() * 0.7f, icon.getCentreY() - 1.0f };
    for (const auto& c : { c1, c2 })
    {
        g.setColour(TC::scope);
        g.fillEllipse(juce::Rectangle<float>(r * 2.0f, r * 2.0f).withCentre(c));
        g.setColour(capturing ? TC::record : TC::brass);
        g.drawEllipse(juce::Rectangle<float>(r * 2.0f, r * 2.0f).withCentre(c), 1.2f);
        for (int k = 0; k < 3; ++k)
        {
            const float a = m_angle + juce::MathConstants<float>::twoPi * static_cast<float>(k) / 3.0f;
            g.drawLine(c.x, c.y, c.x + r * 0.85f * std::sin(a), c.y - r * 0.85f * std::cos(a), 1.2f);
        }
    }
    g.setColour(TC::brass.withAlpha(0.6f));
    g.drawLine(c1.x, c1.y + r, c2.x, c2.y + r, 1.0f);

    // State
    juce::String text = "ARM";
    if (armed && capturing)
        text = "REC " + juce::String::formatted("%02d:%02d", static_cast<int>(m_processor.getRecordedSeconds()) / 60,
                                                static_cast<int>(m_processor.getRecordedSeconds()) % 60);
    else if (armed)
        text = "ARMED";
    if (armed)
    {
        const bool on = capturing || std::fmod(m_blink, 1.0f) < 0.6f;
        g.setColour(TC::record.withAlpha(on ? 1.0f : 0.3f));
        g.fillEllipse(juce::Rectangle<float>(7.0f, 7.0f).withCentre({ b.getX() + 6.0f, b.getCentreY() }));
        b.removeFromLeft(12.0f);
    }
    g.setFont(m_fonts.label(12.0f));
    g.setColour(armed ? TC::text : TC::textDim);
    g.drawFittedText(text, b.toNearestInt(), juce::Justification::centredLeft, 1);
}

//==============================================================================
// Morph
bool SwooshbucklerAudioProcessorEditor::Morph::step()
{
    constexpr float dt = 1.0f / 60.0f;
    const float omega = juce::MathConstants<float>::twoPi * 2.2f;
    bool moving = false;
    for (auto& it : items)
    {
        if (it.param == nullptr)
            continue;
        it.vel += (omega * omega * (it.target - it.pos) - 2.0f * 0.55f * omega * it.vel) * dt;
        it.pos += it.vel * dt;
        if (std::abs(it.target - it.pos) < 1.0e-4f && std::abs(it.vel) < 1.0e-3f)
        {
            it.param->setValueNotifyingHost(it.target);
            it.param->endChangeGesture();
            it.param = nullptr;
            continue;
        }
        it.param->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, it.pos));
        moving = true;
    }
    if (! moving)
        items.clear();
    return moving;
}

void SwooshbucklerAudioProcessorEditor::Morph::cancel(const juce::String& id)
{
    for (auto& it : items)
        if (it.param != nullptr && it.param->getParameterID() == id)
        {
            it.param->endChangeGesture();
            it.param = nullptr;
        }
}

void SwooshbucklerAudioProcessorEditor::Morph::finish()
{
    for (auto& it : items)
        if (it.param != nullptr)
        {
            it.param->setValueNotifyingHost(it.target);
            it.param->endChangeGesture();
        }
    items.clear();
}

//==============================================================================
// Settings panel
namespace
{
    constexpr int c_rateValues[6] = { 0, 44100, 48000, 88200, 96000, 192000 };
    const char* c_rateNames[6] = { "SESSION", "44.1 kHz", "48 kHz", "88.2 kHz", "96 kHz", "192 kHz" };
}

SwooshbucklerAudioProcessorEditor::SettingsPanel::SettingsPanel(SwooshbucklerAudioProcessorEditor& owner) : m_owner(owner)
{
    auto setup = [this](juce::TextButton& b)
    {
        b.setColour(juce::TextButton::buttonOnColourId, TC::flight);
        b.setWantsKeyboardFocus(false);
        addAndMakeVisible(b);
    };
    for (auto* b : { &m_choose, &m_reveal, &m_close, &m_desktop, &m_audio })
        setup(*b);

    const char* types[] = { "WAV", "AIFF", "FLAC" };
    for (int i = 0; i < 3; ++i)
    {
        setup(m_types[static_cast<size_t>(i)]);
        m_types[static_cast<size_t>(i)].setButtonText(types[i]);
        m_types[static_cast<size_t>(i)].onClick = [this, i] { m_owner.m_processor.setFileType(i); refresh(); };
    }
    const int bits[] = { 16, 24, 32 };
    const char* bitNames[] = { "16-BIT", "24-BIT", "32-BIT FLOAT" };
    for (int i = 0; i < 3; ++i)
    {
        setup(m_bits[static_cast<size_t>(i)]);
        m_bits[static_cast<size_t>(i)].setButtonText(bitNames[i]);
        const int value = bits[i];
        m_bits[static_cast<size_t>(i)].onClick = [this, value] { m_owner.m_processor.setBitDepth(value); refresh(); };
    }
    for (int i = 0; i < 6; ++i)
    {
        setup(m_rates[static_cast<size_t>(i)]);
        m_rates[static_cast<size_t>(i)].setButtonText(c_rateNames[i]);
        const int value = c_rateValues[i];
        m_rates[static_cast<size_t>(i)].onClick = [this, value]
        {
            if (m_owner.m_processor.isRecording())
            {
                m_owner.toast("Stop the tape to change its sample rate");
                return;
            }
            m_owner.m_processor.setRecordRate(value);
            refresh();
        };
    }
    const char* chNames[] = { "STEREO", "MONO" };
    for (int i = 0; i < 2; ++i)
    {
        setup(m_channels[static_cast<size_t>(i)]);
        m_channels[static_cast<size_t>(i)].setButtonText(chNames[i]);
        m_channels[static_cast<size_t>(i)].onClick = [this, i] { m_owner.m_processor.setMono(i == 1); refresh(); };
    }

    {
        const char* typeTips[] = { "WAV\nThe most widely read format. The only one that can hold 32-bit float.",
                                   "AIFF\nUncompressed, like WAV; the usual choice on a Mac and in Pro Tools.",
                                   "FLAC\nLossless compression: about half the size of WAV, exactly the same sound." };
        const char* bitTips[] = { "16-Bit\nCD quality: the smallest files, fine for finished sounds.",
                                  "24-Bit\nThe studio standard: plenty of headroom for editing afterwards.",
                                  "32-Bit Float\nCan't clip in the file, so you can fix the level later. WAV only." };
        for (size_t i = 0; i < 3; ++i)
        {
            m_types[i].setTooltip(typeTips[i]);
            m_bits[i].setTooltip(bitTips[i]);
        }
        m_rates[0].setTooltip("Session Rate\nRecords at the audio device's (or the host's) sample rate.");
        for (size_t i = 1; i < m_rates.size(); ++i)
            m_rates[i].setTooltip(juce::String(c_rateNames[i]) + "\nRecords at this rate. Above the device rate the rig itself runs faster, "
                                  "so the recording keeps the extra high end (good for pitching it down later).");
        m_channels[0].setTooltip("Stereo\nRecords both channels, with all the movement across the field.");
        m_channels[1].setTooltip("Mono\nFolds the output to one channel: half the size, for sounds you'll pan yourself.");
        m_auto.setTooltip("Auto-Arm\nArms the tape every time Swooshbuckler opens, so you never miss the first good take.");
        m_choose.setTooltip("Choose the folder recordings are saved to");
        m_desktop.setTooltip("Save recordings to the default folder again");
        m_reveal.setTooltip("Show the recordings folder in the Finder / Explorer");
        m_audio.setTooltip("Audio Device\nPick the audio output, its sample rate and buffer size.");
    }

    m_auto.setColour(juce::ToggleButton::tickColourId, TC::flight);
    m_auto.onClick = [this] { m_owner.m_processor.setAutoRecord(m_auto.getToggleState()); };
    addAndMakeVisible(m_auto);
    m_skip.setTooltip("Skip Silence\nOn: the armed tape leaves the quiet stretches out. It stops rolling once the level has sat about 25 dB under "
                      "the recent peaks for most of a second, and starts again, a quarter of a second early, when the sound comes back, "
                      "so the gaps between whooshes are closed up. After Stop it keeps the whole tail. "
                      "Off: the tape rolls all the time, for one continuous take.");
    m_skip.setColour(juce::ToggleButton::tickColourId, TC::flight);
    m_skip.onClick = [this] { m_owner.m_processor.setSkipSilence(m_skip.getToggleState()); repaint(); };
    addAndMakeVisible(m_skip);

    m_perWhoosh.setTooltip("One File Per Whoosh\nOn: the armed tape closes its file each time a whoosh has ended and opens the next one for the next "
                           "whoosh, so a session of riding leaves a folder of separate, numbered files. A whoosh has ended when the level "
                           "has sat about 25 dB under its peak for most of a second, or when a thrown whoosh has rung out with the rig stopped. "
                           "Sounds shorter than a fifth of a second are not kept, and the quiet between whooshes is always left out. "
                           "Off: one file for the whole take.");
    m_perWhoosh.setColour(juce::ToggleButton::tickColourId, TC::flight);
    m_perWhoosh.onClick = [this] { m_owner.m_processor.setOneFilePerWhoosh(m_perWhoosh.getToggleState()); repaint(); };
    addAndMakeVisible(m_perWhoosh);

    for (auto* editor : { &m_name, &m_creator })
    {
        editor->setFont(m_owner.m_fonts->body(13.0f));
        editor->setColour(juce::TextEditor::backgroundColourId, TC::scope);
        editor->setInputRestrictions(60);
        editor->setSelectAllWhenFocused(true);
        // Return and Escape hand the keyboard back to the rig (space for Swell, F for Throw).
        editor->onReturnKey = [this] { m_owner.grabKeyboardFocus(); };
        editor->onEscapeKey = [this] { m_owner.grabKeyboardFocus(); };
        addAndMakeVisible(*editor);
    }
    m_name.setTooltip("Take Name\nThe start of every recorded file's name; a number, or the date and time, follows it. "
                      "Leave it empty to use the preset's name.");
    m_name.onTextChange = [this] { m_owner.m_processor.setTakeName(m_name.getText()); };
    m_creator.setTooltip("Creator\nWho made the recording. It is written into every file, with a description of the preset and the main settings "
                         "(as the originator of a WAV, a comment in an AIFF, the artist of a FLAC). Leave it empty for Samuel Justice.");
    m_creator.onTextChange = [this] { m_owner.m_processor.setCreator(m_creator.getText()); };

    const char* styleNames[] = { "Counter", "Date and Time" };
    const char* styleTips[] = { "Counter\nFiles are named Name_0001, Name_0002 and so on. The number carries on for as long as Swooshbuckler is open, "
                                "and never takes one that a file in the folder already has.",
                                "Date and Time\nFiles are named after the moment they were started, like Name_2026-11-07_16-32-10. Good for single long takes." };
    for (int i = 0; i < 2; ++i)
    {
        auto& b = m_nameStyles[static_cast<size_t>(i)];
        setup(b);
        b.setButtonText(styleNames[i]);
        b.setTooltip(styleTips[i]);
        b.onClick = [this, i] { m_owner.m_processor.setNameStyle(i); refresh(); };
    }
    const int minutes[] = { 1, 3, 5 };
    const char* lengthNames[] = { "1 Minute", "3 Minutes", "5 Minutes" };
    for (int i = 0; i < 3; ++i)
    {
        auto& b = m_retroLengths[static_cast<size_t>(i)];
        setup(b);
        b.setButtonText(lengthNames[i]);
        const int value = minutes[i];
        // Two channels of 32-bit float: 23 MB a minute at 48 kHz, four times that at 192 kHz.
        b.setTooltip(juce::String(lengthNames[i]) + "\nHow much the LAST button can save after the fact. The sound is held in memory all the time: about "
                     + juce::String(23 * value) + " MB at a 48 kHz recording rate, " + juce::String(46 * value) + " MB at 96 kHz and "
                     + juce::String(92 * value) + " MB at 192 kHz. Changing it empties what is held.");
        b.onClick = [this, value] { m_owner.m_processor.setRetroMinutes(value); refresh(); };
    }

    m_audio.setVisible(static_cast<bool>(SwooshbucklerAudioProcessor::showAudioSettings));
    m_audio.onClick = [] { if (SwooshbucklerAudioProcessor::showAudioSettings) SwooshbucklerAudioProcessor::showAudioSettings(); };
    m_close.onClick = [this] { setVisible(false); };
    m_reveal.onClick = [this] { m_owner.m_processor.getRecordFolder().revealToUser(); };
    m_desktop.onClick = [this] { m_owner.m_processor.setRecordFolder(TapeRecorder::defaultFolder()); refresh(); };
    m_choose.onClick = [this]
    {
        m_chooser = std::make_unique<juce::FileChooser>("Where should recordings go?", m_owner.m_processor.getRecordFolder());
        m_chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                               [this](const juce::FileChooser& fc)
                               {
                                   const auto folder = fc.getResult();
                                   if (folder.isDirectory())
                                   {
                                       m_owner.m_processor.setRecordFolder(folder);
                                       refresh();
                                   }
                               });
    };
}

void SwooshbucklerAudioProcessorEditor::SettingsPanel::refresh()
{
    auto& p = m_owner.m_processor;
    const int type = p.getFileType(), bits = p.getBitDepth(), rate = p.getRecordRate();
    for (int i = 0; i < 3; ++i)
        m_types[static_cast<size_t>(i)].setToggleState(i == type, juce::dontSendNotification);
    m_bits[0].setToggleState(bits == 16, juce::dontSendNotification);
    m_bits[1].setToggleState(bits == 24 || (bits == 32 && type != 0), juce::dontSendNotification);
    m_bits[2].setToggleState(bits == 32 && type == 0, juce::dontSendNotification);
    m_bits[2].setEnabled(type == 0); // 32-bit float is WAV only
    for (int i = 0; i < 6; ++i)
        m_rates[static_cast<size_t>(i)].setToggleState(c_rateValues[i] == rate, juce::dontSendNotification);
    m_channels[0].setToggleState(! p.getMono(), juce::dontSendNotification);
    m_channels[1].setToggleState(p.getMono(), juce::dontSendNotification);
    m_auto.setToggleState(p.getAutoRecord(), juce::dontSendNotification);
    m_skip.setToggleState(p.getSkipSilence(), juce::dontSendNotification);
    m_perWhoosh.setToggleState(p.getOneFilePerWhoosh(), juce::dontSendNotification);
    if (! m_name.hasKeyboardFocus(true))
        m_name.setText(p.getTakeName(), false);
    if (! m_creator.hasKeyboardFocus(true))
        m_creator.setText(p.getCreator(), false);
    m_name.setTextToShowWhenEmpty(p.getEffectiveTakeName(), TC::textFaint);
    m_creator.setTextToShowWhenEmpty("Samuel Justice", TC::textFaint);
    for (int i = 0; i < 2; ++i)
        m_nameStyles[static_cast<size_t>(i)].setToggleState(i == p.getNameStyle(), juce::dontSendNotification);
    m_retroLengths[0].setToggleState(p.getRetroMinutes() == 1, juce::dontSendNotification);
    m_retroLengths[1].setToggleState(p.getRetroMinutes() == 3, juce::dontSendNotification);
    m_retroLengths[2].setToggleState(p.getRetroMinutes() == 5, juce::dontSendNotification);
    m_audio.setVisible(static_cast<bool>(SwooshbucklerAudioProcessor::showAudioSettings)); // standalone only
    repaint();
}

void SwooshbucklerAudioProcessorEditor::SettingsPanel::mouseDown(const juce::MouseEvent& e)
{
    if (! m_panel.contains(e.getPosition()))
        setVisible(false);
}

void SwooshbucklerAudioProcessorEditor::SettingsPanel::resized()
{
    m_panel = getLocalBounds().withSizeKeepingCentre(700, 646);
    auto r = m_panel.reduced(28, 0);
    r.removeFromTop(56);

    auto section = [&](int index, int height)
    {
        m_captions[static_cast<size_t>(index)] = r.removeFromTop(20);
        auto row = r.removeFromTop(height);
        r.removeFromTop(12);
        return row;
    };
    auto buttons = [](juce::Rectangle<int> row, juce::TextButton* const* list, int count, int width)
    {
        for (int i = 0; i < count; ++i)
        {
            list[i]->setBounds(row.removeFromLeft(width));
            row.removeFromLeft(6);
        }
    };

    auto folder = section(0, 52);
    folder.removeFromTop(22);
    juce::TextButton* folderButtons[] = { &m_choose, &m_desktop, &m_reveal };
    buttons(folder.removeFromTop(28), folderButtons, 3, 104);

    juce::TextButton* types[] = { &m_types[0], &m_types[1], &m_types[2] };
    buttons(section(1, 28), types, 3, 90);
    juce::TextButton* bits[] = { &m_bits[0], &m_bits[1], &m_bits[2] };
    buttons(section(2, 28), bits, 3, 120);
    juce::TextButton* rates[] = { &m_rates[0], &m_rates[1], &m_rates[2], &m_rates[3], &m_rates[4], &m_rates[5] };
    buttons(section(3, 28), rates, 6, 98);
    juce::TextButton* channels[] = { &m_channels[0], &m_channels[1] };
    buttons(section(4, 28), channels, 2, 100);

    {
        // Take Name and Creator side by side, then File Names and Keep the Last.
        auto captions = r.removeFromTop(20);
        auto row = r.removeFromTop(26);
        r.removeFromTop(12);
        m_moreCaptions[0] = captions.removeFromLeft(300);
        m_name.setBounds(row.removeFromLeft(300));
        captions.removeFromLeft(24);
        row.removeFromLeft(24);
        m_moreCaptions[1] = captions.removeFromLeft(300);
        m_creator.setBounds(row.removeFromLeft(300));

        captions = r.removeFromTop(20);
        row = r.removeFromTop(28);
        r.removeFromTop(12);
        m_moreCaptions[2] = captions.removeFromLeft(300);
        auto styles = row.removeFromLeft(300);
        juce::TextButton* styleButtons[] = { &m_nameStyles[0], &m_nameStyles[1] };
        buttons(styles, styleButtons, 2, 120);
        captions.removeFromLeft(24);
        row.removeFromLeft(24);
        m_moreCaptions[3] = captions.removeFromLeft(300);
        juce::TextButton* lengthButtons[] = { &m_retroLengths[0], &m_retroLengths[1], &m_retroLengths[2] };
        buttons(row, lengthButtons, 3, 96);
    }
    {
        auto toggles = r.removeFromTop(26);
        m_skip.setBounds(toggles.removeFromLeft(150));
        toggles.removeFromLeft(24);
        m_perWhoosh.setBounds(toggles.removeFromLeft(210));
        m_auto.setBounds(r.removeFromTop(26));
    }
    r.removeFromTop(6);
    m_captions[5] = r.removeFromTop(44);

    auto bottom = m_panel.reduced(28, 18).removeFromBottom(30);
    m_close.setBounds(bottom.removeFromRight(100));
    m_audio.setBounds(bottom.removeFromLeft(150));
}

void SwooshbucklerAudioProcessorEditor::SettingsPanel::paint(juce::Graphics& g)
{
    g.fillAll(TC::background.withAlpha(0.8f));
    SwooshLookAndFeel::drawPanel(g, m_panel.toFloat(), "Settings", "the tape and the audio device", TC::flight, *m_owner.m_fonts);

    const auto& f = *m_owner.m_fonts;
    const char* captions[] = { "RECORDINGS ARE SAVED TO", "FILE TYPE", "BIT DEPTH", "SAMPLE RATE", "CHANNELS" };
    for (int i = 0; i < 5; ++i)
    {
        g.setFont(f.label(10.5f));
        g.setColour(TC::textDim);
        g.drawText(captions[i], m_captions[static_cast<size_t>(i)], juce::Justification::centredLeft);
    }
    const char* moreCaptions[] = { "Take Name", "Creator", "File Names", "Keep the Last" };
    for (size_t i = 0; i < m_moreCaptions.size(); ++i)
        g.drawText(moreCaptions[i], m_moreCaptions[i], juce::Justification::centredLeft);
    g.setFont(f.body(12.0f));
    g.setColour(TC::text);
    g.drawText(m_owner.m_processor.getRecordFolder().getFullPathName(),
               m_captions[0].withY(m_captions[0].getBottom()).withHeight(22), juce::Justification::centredLeft, true);

    // What the sample rate means in practice
    auto& p = m_owner.m_processor;
    const juce::String last = p.getRetroMinutes() == 1 ? juce::String("LAST 60s saves the last minute in the same format.")
                                                       : "LAST " + juce::String(p.getRetroMinutes()) + " MIN saves the last " + juce::String(p.getRetroMinutes()) + " minutes in the same format.";
    juce::String note = (p.getOneFilePerWhoosh() ? "Armed, the tape writes a new file for every whoosh and leaves the quiet between them out. "
                         : p.getSkipSilence() ? "Armed, the tape writes whenever there is sound and skips the quiet stretches. "
                                              : "Armed, the tape rolls all the time. ") + last;
    if (p.getRecordRate() > 0 && p.getDeviceRate() > 0.0)
    {
        if (p.getRecordRate() > p.getDeviceRate() + 0.5)
            note << "  The rig is running at " << juce::String(p.getRenderRate() / 1000.0, 1) << " kHz, so the recording keeps everything up to "
                 << juce::String(p.getRenderRate() / 2000.0, 1) << " kHz (you hear it at the device's "
                 << juce::String(p.getDeviceRate() / 1000.0, 1) << " kHz).";
        else if (p.getRecordRate() < p.getDeviceRate() - 0.5)
            note << "  Recordings are converted down from the device's " << juce::String(p.getDeviceRate() / 1000.0, 1) << " kHz.";
    }
    g.setFont(f.body(11.5f));
    g.setColour(TC::textFaint);
    g.drawFittedText(note, m_captions[5], juce::Justification::topLeft, 3);
}

//==============================================================================
// About panel
SwooshbucklerAudioProcessorEditor::AboutPanel::AboutPanel(SwooshbucklerAudioProcessorEditor& owner) : m_owner(owner)
{
    for (auto* b : { &m_close, &m_website, &m_copy })
    {
        b->setColour(juce::TextButton::buttonOnColourId, TC::flight);
        b->setWantsKeyboardFocus(false);
        addAndMakeVisible(*b);
    }
    m_close.onClick = [this] { setVisible(false); };
    m_website.setTooltip("Open sweetjusticesound.com");
    m_website.onClick = [] { juce::URL("https://www.sweetjusticesound.com").launchInDefaultBrowser(); };
    m_copy.setTooltip("Copy the version and build to the clipboard, for a bug report");
    m_copy.onClick = [this]
    {
        juce::SystemClipboard::copyTextToClipboard(m_owner.versionLine() + ", built " + Version::builtOn()
                                                   + (Version::commit().isNotEmpty() ? " (" + Version::commit() + ")" : juce::String())
                                                   + ", " + juce::SystemStats::getOperatingSystemName());
        m_owner.toast("Version copied");
    };
    m_logo = std::make_unique<LogoOutline>(BinaryData::sjlogo_svg, static_cast<size_t>(BinaryData::sjlogo_svgSize));
}

void SwooshbucklerAudioProcessorEditor::AboutPanel::visibilityChanged()
{
    if (isVisible())
    {
        m_shownAt = juce::Time::getMillisecondCounterHiRes() * 0.001;
        startTimerHz(30);
    }
    else
    {
        stopTimer();
    }
}

void SwooshbucklerAudioProcessorEditor::AboutPanel::mouseDown(const juce::MouseEvent& e)
{
    if (! m_panel.contains(e.getPosition()))
        setVisible(false);
}

void SwooshbucklerAudioProcessorEditor::AboutPanel::resized()
{
    m_panel = getLocalBounds().withSizeKeepingCentre(680, 410);
    auto r = m_panel.reduced(28, 0).withTrimmedTop(58).withTrimmedBottom(18);
    auto bottom = r.removeFromBottom(30);
    m_close.setBounds(bottom.removeFromRight(100));
    bottom.removeFromRight(8);
    m_website.setBounds(bottom.removeFromRight(100));
    m_copy.setBounds(bottom.removeFromLeft(130));
    r.removeFromBottom(14);
    m_logoArea = r.removeFromLeft(190).withSizeKeepingCentre(190, 190).withY(r.getY() + 6);
    r.removeFromLeft(24);
    m_textArea = r;
}

void SwooshbucklerAudioProcessorEditor::AboutPanel::paint(juce::Graphics& g)
{
    const auto& f = *m_owner.m_fonts;
    g.fillAll(TC::background.withAlpha(0.8f));
    SwooshLookAndFeel::drawPanel(g, m_panel.toFloat(), "About", "the rig, its makers and its version", TC::flight, f);

    // The Sweet Justice logo, traced by its comets.
    if (m_logo != nullptr && m_logo->isLoaded())
    {
        g.setColour(TC::scope);
        g.fillRoundedRectangle(m_logoArea.toFloat(), 8.0f);
        g.setColour(TC::outline);
        g.drawRoundedRectangle(m_logoArea.toFloat().reduced(0.5f), 8.0f, 1.0f);
        const float t = static_cast<float>(juce::Time::getMillisecondCounterHiRes() * 0.001 - m_shownAt);
        m_logo->renderPreview(g, m_logoArea.toFloat().reduced(18.0f), t, 1.0f);
    }

    auto r = m_textArea;
    SwooshLookAndFeel::drawCutlasses(g, juce::Rectangle<float>(static_cast<float>(r.getX()), static_cast<float>(r.getY()) + 8.0f, 34.0f, 30.0f), TC::master, TC::brass);
    g.setFont(f.display(40.0f));
    g.setColour(TC::text);
    g.drawText("Swooshbuckler", r.removeFromTop(46).withTrimmedLeft(44), juce::Justification::centredLeft);

    auto line = [&](const juce::String& text, const juce::Font& font, juce::Colour colour, int height)
    {
        g.setFont(font);
        g.setColour(colour);
        g.drawFittedText(text, r.removeFromTop(height), juce::Justification::topLeft, juce::jmax(1, height / 16));
    };
    line("Version " + Version::number() + "   " + juce::AudioProcessor::getWrapperTypeDescription(m_owner.m_processor.wrapperType),
         f.heading(14.0f), TC::flight, 20);
    line("Built " + Version::builtOn() + (Version::commit().isNotEmpty() ? "   commit " + Version::commit() : juce::String()),
         f.body(12.0f), TC::textDim, 22);
    r.removeFromTop(8);
    line("A whoosh-making rig. Drop in up to ten sounds and it rides the faders across them, making doppler passes, "
         "swells, hits and tails. Shape them with two effect buses, a hit gate, a spectral morph and four modulators, "
         "and arm the tape to keep every one.", f.body(13.0f), TC::text, 58);
    r.removeFromTop(8);
    line("Made by Samuel Justice at Sweet Justice Sound.", f.heading(13.0f), TC::text, 22);
    r.removeFromTop(6);
    line("Fonts: Pirata One by Rodrigo Fuenzalida and Nicolas Massi; IBM Plex Sans Condensed by IBM. "
         "Both under the SIL Open Font License 1.1.  Built with JUCE " + Version::juce() + ".  "
         "VST is a registered trademark of Steinberg Media Technologies GmbH.", f.body(11.5f), TC::textFaint, 50);
    // The build year, so the notice never needs editing.
    line(juce::String(juce::CharPointer_UTF8("\xc2\xa9 ")) + Version::year() + " Samuel Justice. MIT License.",
         f.body(11.5f), TC::textFaint, 18);
}

//==============================================================================
SwooshbucklerAudioProcessorEditor::SwooshbucklerAudioProcessorEditor(SwooshbucklerAudioProcessor& p)
    : AudioProcessorEditor(&p)
    , m_processor(p)
    , m_lnf(*m_fonts)
    , m_tape(p, *m_fonts)
    , m_sea(p, *m_fonts)
    , m_morphPad(*m_fonts)
    , m_swell(p, *m_fonts)
    , m_hold(p, *m_fonts)
    , m_wires(p.getMods(), *m_fonts)
    , m_modPanel(p, *m_fonts, m_wires)
    , m_settings(*this)
    , m_takes(p, *m_fonts)
    , m_about(*this)
{
    setLookAndFeel(&m_lnf);
    juce::LookAndFeel::setDefaultLookAndFeel(&m_lnf); // popup menus, alerts and file choosers match too
    m_tooltips.setLookAndFeel(&m_lnf);
    setWantsKeyboardFocus(true);

    addAndMakeVisible(m_content);
    m_content.setBounds(0, 0, c_width, c_height);
    m_content.onPaint = [this](juce::Graphics& g) { paintContent(g); };
    m_content.onMouseDown = [this](const juce::MouseEvent& e)
    {
        if (m_toastFile != juce::File() && m_toastArea.contains(e.getPosition()))
            showTakes(); // a "Saved ..." message: the file is at the top of the takes
        else if (m_titleArea.contains(e.getPosition()))
        {
            m_about.setVisible(true);
            m_about.toFront(false);
        }
    };

    // Header ------------------------------------------------------------------
    for (auto* b : { &m_presetPrev, &m_presetName, &m_presetNext, &m_dice, &m_retroButton, &m_settingsButton, &m_playButton, &m_newButton, &m_takesButton })
    {
        b->setColour(juce::TextButton::buttonOnColourId, TC::flight);
        b->setWantsKeyboardFocus(false);
        m_content.addAndMakeVisible(*b);
    }
    m_presetName.setButtonText(p.currentPresetName);
    m_presetName.setTooltip("Presets\nClick for the list of factory presets. Picking one glides every control to its new setting over a moment, so you can switch while it plays. Your tracks, their levels and the output section are left alone. Use < and > to step through them.");
    m_presetName.onClick = [this] { showPresetMenu(); };
    m_presetPrev.setTooltip("Previous Preset");
    m_presetNext.setTooltip("Next Preset");
    m_presetPrev.onClick = [this] { stepPreset(-1); };
    m_presetNext.onClick = [this] { stepPreset(1); };
    m_dice.setColour(juce::TextButton::buttonOnColourId, TC::movement);
    m_dice.setTooltip("Randomise\nRolls a random but musical setting for the ride, the buses, the hits and the space, and glides to it. Your tracks, their levels and the output stay put. Keep rolling until you hear something you like.");
    m_dice.onClick = [this]
    {
        applyPreset(Presets::roll(m_random), false);
        m_processor.currentPresetName = "Randomised";
    };
    m_content.addAndMakeVisible(m_tape);
    m_tape.setTooltip("Tape\nArm it and it records whenever there is sound, skipping the quiet stretches, into one long file of usable whooshes. Click again to stop and close the file. Pass peaks and hits are marked in WAV files, so a take arrives ready to spot. Where it saves, in which format, Skip Silence (off for one continuous take) and One File Per Whoosh (a separate file for each) are in SETTINGS; what it has saved is in Takes.");
    m_tape.onClick = [this] { toggleLog(); };
    m_retroButton.setTooltip("Last 60 Seconds\nThe rig always remembers the last minute. Click to save it to a file, even if the tape wasn't armed: for the happy accident you didn't record. Keep the Last in SETTINGS makes it 3 or 5 minutes.");
    m_takesButton.setTooltip("Takes\nThe recordings of this run, newest first, with how many there are: tape takes, whoosh files and LAST 60s saves. "
                             "Open it to hear them, drag them out to the Finder / Explorer or into your DAW, keep or delete them.");
    m_takesButton.onClick = [this] { showTakes(); };
    m_playButton.setColour(juce::TextButton::buttonOnColourId, TC::good);
    m_playButton.setTooltip("Play / Stop\nStarts and stops the whole rig. Stop lets the tail ring out. The app always opens stopped, so nothing blasts out when you load a session. In a DAW it can be automated, and with Follow Host on the transport starts and stops it too.");
    m_playButton.onClick = [this] { m_processor.setPlaying(! m_processor.isPlaying()); };
    m_newButton.setTooltip("New Session\nStops, empties every track and puts every control back to its default. Cmd+Z can bring removed tracks back.");
    m_newButton.onClick = [this]
    {
        m_morph.finish();
        m_processor.newSession();
        toast("New Session");
    };
    m_retroButton.onClick = [this] { saveRetro(); };
    m_settingsButton.setTooltip("Settings\nWhere recordings go, their file type, bit depth, sample rate and channels, auto-arming the tape, and (in the app) the audio device.");
    m_settingsButton.onClick = [this] { m_settings.refresh(); m_settings.setVisible(true); m_settings.toFront(false); };

    // Tracks + output view + ride --------------------------------------------
    m_content.addAndMakeVisible(m_hold);
    m_hold.onSwell = [this] { m_processor.setSwell(true); juce::Timer::callAfterDelay(250, [this] { if (! m_spaceHeld) m_processor.setSwell(false); }); };
    m_hold.onStatus = [this](const juce::String& s) { toast(s); };
    m_content.addAndMakeVisible(m_sea);
    m_sea.onClick = [this] { m_processor.setSwell(true); juce::Timer::callAfterDelay(400, [this] { if (! m_spaceHeld) m_processor.setSwell(false); }); };
    m_sea.setTooltip("The Sea\nWhat the rig sends out: the sound rolls away as waves, and each doppler pass is a comet across the sky. Click it (or press space) to swell every track up.");

    // Morph: the pad sits over the output view (the waves show through) while it's switched on.
    m_content.addChildComponent(m_morphPad);
    m_morphPad.setFixedSources("Bus A", TC::movement, "Bus B", TC::sources);
    auto setParam = [this](const char* id, float plain)
    {
        if (auto* param = m_processor.getState().getParameter(id))
        {
            param->beginChangeGesture();
            param->setValueNotifyingHost(param->convertTo0to1(plain));
            param->endChangeGesture();
        }
    };
    m_morphPad.onXYChanged = [setParam](float x, float y) { setParam(ParamIDs::morphX, x); setParam(ParamIDs::morphY, y); };
    m_morphPad.onModeChanged = [setParam](int mode) { setParam(ParamIDs::morphMode, static_cast<float>(mode)); };
    m_morphPad.onShapeChanged = [setParam](int shape) { setParam(ParamIDs::morphShape, static_cast<float>(shape)); };
    m_morphPad.onWanderChanged = [this, setParam](bool on, float speed)
    {
        const bool was = m_processor.getState().getRawParameterValue(ParamIDs::morphWander)->load() > 0.5f;
        if (was && ! on)
        {
            // Stopped: the dot stays where the wander left it.
            float x = 0.5f, y = 0.5f, rotation = 0.0f;
            m_processor.getMorphPosition(x, y, rotation);
            setParam(ParamIDs::morphX, x);
            setParam(ParamIDs::morphY, y);
        }
        if (was != on)
            setParam(ParamIDs::morphWander, on ? 1.0f : 0.0f);
        if (std::abs(m_processor.getState().getRawParameterValue(ParamIDs::morphSpeed)->load() - speed) > 1.0e-6f)
            setParam(ParamIDs::morphSpeed, speed);
    };
    // The processor flies the dot (so it wanders with the window closed and in a bounce); the pad shows where it is.
    m_morphPad.wanderPosition = [this](float& x, float& y, float& rotation) { m_processor.getMorphPosition(x, y, rotation); };
    m_morphPad.onEnableChanged = [setParam](bool on) { setParam(ParamIDs::morphOn, on ? 1.0f : 0.0f); };
    m_morphButton.setClickingTogglesState(true);
    m_morphButton.setColour(juce::TextButton::buttonOnColourId, TC::filter);
    m_morphButton.setWantsKeyboardFocus(false);
    m_morphButton.setTooltip("Morph\nShows the morph pad over the sea. It blends bus A into bus B spectrally, not just a crossfade: the two sounds trade their tone and pitch. Switch it on with ON on the pad, then drag the dot or press play to let it wander.");
    m_morphButton.onClick = [this] { syncMorphPad(); };
    m_content.addAndMakeVisible(m_morphButton);

    m_syncHits.setClickingTogglesState(true);
    m_syncHits.setColour(juce::TextButton::buttonOnColourId, TC::record);
    m_syncHits.setWantsKeyboardFocus(false);
    m_syncHits.setTooltip("Sync Hits\nOn: every Impact track fires its next hit on one shared beat, so all their transients land together as one big hit (the waveforms slide into line to show it). Off: each hit track loops on its own timing.");
    m_content.addAndMakeVisible(m_syncHits);
    m_syncAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(p.getState(), ParamIDs::hitSync, m_syncHits);
    m_aimHits.setClickingTogglesState(true);
    m_aimHits.setColour(juce::TextButton::buttonOnColourId, TC::record);
    m_aimHits.setWantsKeyboardFocus(false);
    m_aimHits.setTooltip("Aim\nOn (the default): the hits are aimed at the peaks of the doppler passes on buses A and B. Each pass is loudest "
                         "at its middle, which is known as soon as it starts, so Swooshbuckler looks ahead: when a peak is coming, it "
                         "picks every hit track's next sound and starts it early by that sound's own lead-in, so all the transients land "
                         "right on the peak. Sensitivity sets which peaks get a hit (closer passes on a louder bus win). "
                         "The gate isn't needed, so GATE, Release, Hold, Floor and SYNC HITS rest.\n"
                         "Off: the classic hit gate (and SYNC HITS if it's on).");
    m_content.addAndMakeVisible(m_aimHits);
    m_aimAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(p.getState(), ParamIDs::hitAim, m_aimHits);

    m_content.addAndMakeVisible(m_swell);
    m_swell.setTooltip("Swell\nHold it to push every fader up at once, for a big whoosh on demand. Held for longer than a couple of seconds, the mix eases back to its usual level with every fader still up. The space bar does the same, and so does any MIDI note while Keys is set to Swell.");
    m_rideButton.setClickingTogglesState(true);
    m_rideButton.setColour(juce::TextButton::buttonOnColourId, TC::flight);
    m_rideButton.setTooltip("Auto Ride\nThe heart of the rig: every track's fader moves by itself, each at its own speed, so overlapping swells make whooshes for you. Energy, Rate, Fill, Chaos and Pitch shape how it moves. Off: the faders stay where you set them, the doppler passes do the moving, and the mix is eased down to about the level it has with the ride on.");
    m_rideButton.setWantsKeyboardFocus(false);
    m_content.addAndMakeVisible(m_rideButton);
    m_rideAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(p.getState(), ParamIDs::rideOn, m_rideButton);

    m_throwButton.setColour(juce::TextButton::buttonColourId, TC::flight.withAlpha(0.25f));
    m_throwButton.setWantsKeyboardFocus(false);
    m_throwButton.setTooltip("Throw\nThrows one whoosh now: a single pass on both buses, with a long approach, the peak as it goes by and a shorter tail. "
                             "With AIM on, the hits land on its peak. While the rig plays it goes on top of the ride; with the rig stopped you get "
                             "just that one whoosh and then silence, so single whooshes can be made one at a time. The peak is heard half a Throw "
                             "Length after you throw, and the Ride panel counts down to it. The F key does the same, and so does a MIDI note with Keys set to Throw.");
    m_throwButton.onClick = [this] { m_processor.throwWhoosh(); };
    m_content.addAndMakeVisible(m_throwButton);

    m_keysMode.addItemList({ "Swell", "Pitch", "Throw" }, 1);
    m_keysMode.setWantsKeyboardFocus(false);
    m_keysMode.setTooltip("Keys\nWhat MIDI notes do. Swell: any held note holds Swell. "
                          "Pitch: the held notes set the pitch. Each note is its distance from middle C (C4) in semitones, added to the Pitch knob and "
                          "each track's own pitch. Hold several and they are dealt out over the loaded tracks in order, the lowest note to the first "
                          "track, going round again when there are more tracks than notes, so a chord gives each layer its own pitch. Let go and the "
                          "pitch glides back. Throw: every note throws one whoosh at that note's pitch; how hard you play sets how close and loud it "
                          "passes, and notes played together make one whoosh with their pitches dealt out the same way. The held notes are shown in "
                          "the Ride panel. Pitch never goes past two octaves either way.");
    m_content.addAndMakeVisible(m_keysMode);
    m_keysModeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(p.getState(), ParamIDs::keysMode, m_keysMode);

    m_followHost.setClickingTogglesState(true);
    m_followHost.setColour(juce::TextButton::buttonOnColourId, TC::good);
    m_followHost.setWantsKeyboardFocus(false);
    m_followHost.setTooltip("Follow Host\nOn: the rig plays while your DAW's transport plays and stops when it stops, letting the tail ring out. "
                            "Every start begins the ride again from the same place, so the same settings bounce the same way each time. "
                            "Play still starts and stops it by hand while the transport is stopped. Off: only Play (or its automation) starts and stops the rig.");
    m_followHost.setToggleState(p.getFollowHost(), juce::dontSendNotification);
    m_followHost.onClick = [this] { m_processor.setFollowHost(m_followHost.getToggleState()); };
    m_content.addChildComponent(m_followHost);
    m_followHost.setVisible(p.canFollowHost());

    addKnob(ParamIDs::energy, "Energy", TC::flight, "How deep the auto ride dips the faders between swells, evenly in dB: 4 dB more for every 10%, to -36 dB at 90% and right down to silence at 100%, so each whoosh rises out of nothing and falls back. Low: gentle movement that stays fairly level. Does nothing with Auto Ride off.");
    addKnob(ParamIDs::rate, "Rate", TC::flight, "How fast the faders move, and so how long each whoosh and each doppler pass lasts. The readout is the time of one swell (each track rides a little faster or slower than it). Low: slow, long passes (25 s swells, 6 s passes). High: short ones (under 2 s swells, 1.6 s passes).");
    addKnob(ParamIDs::fill, "Fill", TC::flight, "Long echoes (about 0.5, 1 and 2 s, a little different on every track) after each track's fader, so they hang on into the dips of the ride and the gaps of unsteady sounds and keep the result going. Gappy sounds get more of it.");
    addKnob(ParamIDs::chaos, "Chaos", TC::flight, "How unpredictable it is. 0 is a steady pattern that repeats (every track plays in order from its start). Higher: every swell has a different size and speed, every pass a different speed, length and closeness, and crackle tracks jump around.");
    addKnob(ParamIDs::pitch, "Pitch", TC::flight, "Plays every track higher or lower, in semitones (up to two octaves either way), finer near the middle. Down makes everything bigger, heavier and slower; up makes it smaller and faster (the hits too).");
    addKnob(ParamIDs::throwLength, "Throw Length", TC::flight, "")
        .setTooltip("Throw Length\nHow long a thrown whoosh lasts, from 0.3 to 6 seconds. Its peak is heard halfway through, so half of this is the "
                    "time from pressing Throw to the peak. Fully left (Rate): a throw is as long as the ride's own passes, set by Rate.\n"
                    "Double-click to reset it.");

    // Buses: one set of controls per bus, shown one bus at a time (tabs) --------
    {
        const juce::Colour busColours[ParamIDs::numBuses] = { TC::movement, TC::sources };
        const char* names[] = { "Doppler", "Width", "Bend", "Low End", "Swirl", "Filter", "Drive", "Ducked", "Filtered" };
        const char* tips[] = {
            "How dramatic each pass of the moving source is: how close it comes, how much it swells and brightens on the way by. Speed, distance and direction change by themselves with every pass.",
            "How far each pass travels across the stereo field. 0: in the middle. High: a full side-to-side fly-by, and the ear turned away from the pass hears it a little duller, as a head in the way would make it. With LINK on, bus B uses bus A's Width.",
            "How much the pitch bends as a pass goes by (the doppler bend), together with Doppler: it rises on the way in and drops as the pass goes by. It changes the pitch only, not how the pass swells. Low: passes go by without the bend. High: a big race-car scream and drop.",
            "Weight: a sub octave, low overtones, and an extra low lift at the loudest moments. With the faders still (Auto Ride off, or Swell held) the lift follows each pass: up as it comes close, down as it leaves.",
            "Movement inside the sound: a phaser, then (from about a third of the way) a flanger that sweeps along with each pass. The level stays put; only the colour moves. High settings narrow and darken a bed. With the faders still (Auto Ride off, or Swell held) the passes alone drive the sweep.",
            "A filter that opens as each pass swells and closes in its tail, so the tails are dark and the peaks bright, like air rushing past. More: more of it, closing further at the peak, and over the top 40% a resonant edge. With the faders still (Auto Ride off, or Swell held) it follows how close each pass is.",
            "Saturation: grit and density, from a gentle thickening to a hard, flattened growl. The level is kept roughly steady, so it adds character rather than just loudness.",
            "How much bus A pushes this bus down at A's peaks (side-chain ducking, up to -20 dB in Volume mode), so the two buses don't pile up. DUCK sets how (Volume, Darken or Mids). With TAKE TURNS on, also how far the quieter bus goes down: at 0, Take Turns does nothing.",
            "How far bus A closes a low-pass filter on this bus at A's peaks (down to about 280 Hz at 100%): B goes dark under A instead of quieter.",
        };
        for (int b = 0; b < ParamIDs::numBuses; ++b)
        {
            for (int k = 0; k < ParamIDs::numBusControls; ++k)
                if (ParamIDs::bus[b][k] != nullptr)
                    addKnob(ParamIDs::bus[b][k], names[k], busColours[b],
                            juce::String("Bus ") + ParamIDs::busNames[b] + ": " + names[k] + "\n" + tips[k]);

        }
    }

    // Hits + output --------------------------------------------------------------
    addKnob(ParamIDs::hitSense, "Sensitivity", TC::record, "How easily the hits fire. With AIM on: which pass peaks get a hit (closer passes on a louder bus score higher). With AIM off: how easily the hit gate opens, by the GATE mode. Low: only the biggest moments fire the hits. High: they fire often.");
    addKnob(ParamIDs::hitBoost, "Boost", TC::record, "How loud the hit tracks come in when they fire, from 3 dB under their own level to 12 dB over it. More makes the hits punch out of the whoosh (the top end leans on the limiter).");
    addKnob(ParamIDs::hitRelease, "Release", TC::record, "How long the hits tail out after the gate opens. Short: tight, dry impacts. Long: the hits ring on into the next swell.");
    addKnob(ParamIDs::hitHold, "Hold", TC::record, "How long the gate stays open once it opens. Longer stops it chattering open and shut, so each hit reads as one event.");
    addKnob(ParamIDs::hitFloor, "Floor", TC::record, "How far the gate closes between hits. Closed (fully left): the hits vanish completely between peaks. Turn it up and they stay under the mix instead, from -22 dB to just under their full level.");
    addKnob(ParamIDs::hitDuck, "Duck", TC::record, "How far buses A and B get out of the way when the hits fire (up to -12 dB at the hits' peaks), so the impact punches through the wall of whoosh. DUCK sets how they get out of the way.");
    addKnob(ParamIDs::duckReturn, "Return", TC::record, "How fast ducked buses come back up afterwards, for the hits' duck and for bus B ducked by A. Fast (fully left) follows straight away. Slower is smoother; at the top it pumps.");

    // Gate and duck modes: the first entry of each is the original rig.
    m_gateMode.addItemList({ "Peaks", "Level", "Jumps", "Open" }, 1);
    m_gateMode.setTooltip("Gate Mode\nWhat opens the hit gate. Peaks: only at the loudest moments of buses A and B together (the original rig). Level: whenever the mix is louder than a level, set by Sensitivity. Jumps: on sudden rises, however loud or quiet. Open: no gate, the hits always play.");
    m_duckMode.addItemList({ "Volume", "Darken", "Mids" }, 1);
    m_duckMode.setTooltip("Duck Mode\nHow a ducked bus gets out of the way, both when the hits duck A and B and when A ducks B. Volume: it is turned down (the original rig). Darken: it is low-passed instead, so it stays but gets out of the way. Mids: only the crowded middle (250 Hz to 4 kHz) goes down, so the sub and air stay.");
    for (auto* c : { &m_gateMode, &m_duckMode })
    {
        c->setWantsKeyboardFocus(false);
        m_content.addAndMakeVisible(*c);
    }
    m_gateModeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(p.getState(), ParamIDs::gateMode, m_gateMode);
    m_duckModeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(p.getState(), ParamIDs::duckMode, m_duckMode);
    m_takeTurns.setClickingTogglesState(true);
    m_takeTurns.setColour(juce::TextButton::buttonOnColourId, TC::sources);
    m_takeTurns.setWantsKeyboardFocus(false);
    m_takeTurns.setTooltip("Take Turns\nOn: only the louder of bus A and bus B speaks at a time and the other ducks under it (by as much as Ducked), so they trade places instead of piling up. Off: A always ducks B (the original rig). Good for taming the everything-at-once wall of sound.");
    m_content.addAndMakeVisible(m_takeTurns);
    m_linkBuses.setClickingTogglesState(true);
    m_linkBuses.setColour(juce::TextButton::buttonOnColourId, TC::movement);
    m_linkBuses.setWantsKeyboardFocus(false);
    m_linkBuses.setTooltip("Link\nOn: bus B flies the same doppler passes as bus A, at the same moment, as close and in the same direction, "
                           "with A's Doppler, Width and Pass Mode, so the two buses move as one source: one consistent whoosh. "
                           "B keeps its own Bend, so its pitch can bend more or less.\n"
                           "Off (the default): each bus does its own passes, which keeps energy beds on B moving independently. "
                           "B's own Doppler, Width and Pass Mode, and any modulator wires on its knobs, come back when you unlink.");
    m_content.addAndMakeVisible(m_linkBuses);
    for (size_t b = 0; b < m_passMode.size(); ++b)
    {
        auto& menu = m_passMode[b];
        menu.addItemList({ "Bed", "Fly-By" }, 1);
        menu.setTooltip(juce::String("Pass Mode\nHow this bus's doppler passes follow each other. "
                                     "Bed: two passes overlap, one coming in as the other goes out, so the level stays steady. Good for beds, air and long energy files. "
                                     "Fly-By: one pass at a time, with a long approach, the peak as it goes by and a shorter tail, so each sound is heard once, at one pitch. "
                                     "Good for a single clear whoosh, tones and anything with a sharp start. "
                                     "The passes peak at the same moments either way, so AIM and LINK work the same.")
                        + (b == 1 ? "\nWhile LINK is on, bus B uses bus A's Pass Mode." : ""));
        menu.setWantsKeyboardFocus(false);
        m_content.addAndMakeVisible(menu);
        m_passModeAttachment[b] = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(p.getState(), ParamIDs::passMode[b], menu);
    }

    {
        const auto& engine = p.getEngine();
        const std::array<const ScopeRing*, 4> rings { &engine.getScope(0), &engine.getScope(1), &engine.getScope(2), &p.getOutputScope() };
        const std::array<juce::Colour, 4> colours { TC::movement, TC::sources, TC::record, TC::master };
        const char* tips[] = {
            "Bus A, the Last 3 Seconds\nWhat bus A sends on, after its effects. The newest sound is at the right.",
            "Bus B, the Last 3 Seconds\nWhat bus B sends on, after being ducked and filtered by bus A: a thin line under a busy A is the ducking working.",
            "Hits, the Last 3 Seconds\nWhat gets through the hit gate. Each spike is a hit; the gaps are the gate closed.",
            "Output, the Last 3 Seconds\nWhat you hear and what the tape records, after the space and the limiter.",
        };
        for (size_t i = 0; i < m_sectionScopes.size(); ++i)
        {
            m_sectionScopes[i] = std::make_unique<SectionScope>(*rings[i], colours[i]);
            m_sectionScopes[i]->setTooltip(tips[i]);
            m_content.addAndMakeVisible(*m_sectionScopes[i]);
        }
    }
    m_takeTurnsAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(p.getState(), ParamIDs::takeTurns, m_takeTurns);
    m_linkBusesAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(p.getState(), ParamIDs::linkBuses, m_linkBuses);

    addKnob(ParamIDs::space, "Space", TC::space, "Echoes and reverb around the whole mix. Low: close and dry. High: a big open wash for the whooshes to fly through (it smooths out the swells).");
    addKnob(ParamIDs::inflate, "Inflate", TC::master, "Makes the mix louder and denser without adding new peaks, so quieter detail comes forward. Up to 70% it blends in; the last 30% makes its curve harder and grittier. Good for thin sources. It comes before Push, so it does the same wherever Push is.");
    addKnob(ParamIDs::push, "Push", TC::master, "How hard the mix drives into the limiter, after Inflate (up to +15 dB). More is louder and denser, then squashed; the ceiling stays where it is, so it can't clip. A hit that goes over is let go at once; loud stretches are held steadily, so a swell keeps its shape.");
    addKnob(ParamIDs::ceiling, "Ceiling", TC::master, "The limiter's ceiling: nothing gets out louder than this (0 to -6 dBFS, finer near the top). It is a true-peak ceiling: the limiter also reads the waveform between the samples, so a take stays under it when it is converted or played back. The default, -0.3 dB, leaves a little headroom for file conversion.");

    // Peak tremolo: a framed group at the left of Output (before Space in the signal): On/Off, wave and Shakes menus
    // over its Depth, Rate and Threshold knobs.
    addKnob(ParamIDs::tremDepth, "Depth", TC::movement, "Tremolo Depth\nHow far the level dips at the very peak (the readout), up to a full chop at 100%. It only shakes the loudest moments: past Threshold it eases in, "
            "deepest at the peak, and lets go as the level falls, so the dynamics flutter where the whoosh is biggest.");
    m_tremOn.addItemList({ "Off", "On" }, 1);
    m_tremOn.setWantsKeyboardFocus(false);
    m_tremOn.setTooltip("Peak Tremolo\nOn: a tremolo that shakes only the loudest moments. As the level climbs past Threshold towards its recent "
                        "peak, the tremolo deepens (up to Depth, at Rate, in the chosen wave) and it lets go as the level falls, so the "
                        "dynamics flutter where the whoosh is biggest. Off (the default): no tremolo. Switching fades it in or out.");
    m_content.addAndMakeVisible(m_tremOn);
    m_tremOnAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(p.getState(), ParamIDs::tremOn, m_tremOn);
    addKnob(ParamIDs::tremRate, "Rate", TC::movement, "Tremolo Rate\nHow fast it shakes, from 1.5 Hz (a slow throb) to 30 Hz (a buzzing flutter).");
    addKnob(ParamIDs::tremThreshold, "Threshold", TC::movement, "Tremolo Threshold\nHow near its recent peak the level has to be before the tremolo starts (the readout, in dB under the peak). "
            "Low: it shakes most of the time. High: only right at the top of each swell. A steady bed sits at its own peak, so it shakes all the time.");
    m_tremShape.addItemList({ "Sine", "Square", "Flutter" }, 1);
    m_tremShape.setTooltip("Tremolo Shape\nSine: a smooth throb. Square: hard chops, like a helicopter. Flutter: a random wobble, like wings or a loose flap.");
    m_tremTarget.addItemList({ "Everything", "Buses A + B", "Hits" }, 1);
    m_tremTarget.setTooltip("Tremolo Shakes\nWhat the tremolo shakes, and whose level drives it. Everything (the default): the whole mix, by its level. "
                            "Buses A + B: each bus by its own level, dipping together; the hits stay solid. Hits: only the hits.");
    for (auto* box : { &m_tremShape, &m_tremTarget })
    {
        box->setWantsKeyboardFocus(false);
        m_content.addAndMakeVisible(*box);
    }
    m_tremShapeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(p.getState(), ParamIDs::tremShape, m_tremShape);
    m_tremTargetAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(p.getState(), ParamIDs::tremTarget, m_tremTarget);

    // Master Trim: how loud you hear it. After the limiter; the tape records before it, at full level.
    m_trim.setSliderStyle(juce::Slider::LinearVertical);
    m_trim.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    m_trim.setColour(juce::Slider::trackColourId, TC::master);
    m_trim.setDoubleClickReturnValue(true, 0.0);
    m_trim.setWantsKeyboardFocus(false);
    m_trim.setTooltip("Master Trim\nHow loud you hear it (and what your DAW gets from the plug-in), from silence up to the limiter's level, after the limiter. "
                      "The tape and LAST 60s record before it, at full level, so turning it down never makes a quieter take.\n"
                      "Drag it. Double-click to reset it to 0 dB.");
    m_content.addAndMakeVisible(m_trim);
    m_trimAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(p.getState(), ParamIDs::masterTrim, m_trim);

    // Modulation: the panel at the bottom, and the wires over everything (only their plugs take clicks).
    m_content.addAndMakeVisible(m_modPanel);
    m_cablesButton.setClickingTogglesState(true);
    m_cablesButton.setToggleState(true, juce::dontSendNotification);
    m_cablesButton.setColour(juce::TextButton::buttonOnColourId, TC::brass);
    m_cablesButton.setWantsKeyboardFocus(false);
    m_cablesButton.setTooltip("Cables\nShows or hides the cables. The plugs on the knobs stay either way, so you can still change a wire's depth or pull it out.");
    m_cablesButton.onClick = [this] { m_wires.setShowCables(m_cablesButton.getToggleState()); };
    m_content.addAndMakeVisible(m_cablesButton);
    m_content.addAndMakeVisible(m_wires);
    m_wires.setPanel(&m_modPanel);
    m_hold.onLanesMoved = [this] { m_wires.rebuildSoon(); }; // scrolling the tracks moves the plugs on their pitch boxes
    m_wires.onStatus = [this](const juce::String& text) { toast(text); };
    m_wires.isSourceBipolar = [&p](int source)
    {
        return p.getState().getRawParameterValue(ParamIDs::modType[source])->load() < 1.5f; // LFO, Random
    };
    p.getMods().onChange = [safe = juce::Component::SafePointer<ModWires>(&m_wires)]
    {
        if (safe != nullptr) safe->rebuild();
    };

    m_content.addChildComponent(m_settings);
    m_content.addChildComponent(m_takes);
    m_takes.onStatus = [this](const juce::String& text) { toast(text); };
    m_content.addChildComponent(m_about);

    // Version: next to the website, and the title itself, both open About.
    m_versionLink.setButtonText(versionLine());
    m_versionLink.setURL({});
    m_versionLink.setFont(m_fonts->label(10.0f), false, juce::Justification::centredLeft);
    m_versionLink.setColour(juce::HyperlinkButton::textColourId, TC::textDim);
    m_versionLink.setTooltip("About Swooshbuckler: version, build and credits");
    m_versionLink.onClick = [this] { m_about.setVisible(true); m_about.toFront(false); };
    m_content.addAndMakeVisible(m_versionLink);

    m_content.addAndMakeVisible(m_websiteLink);
    m_websiteLink.setFont(m_fonts->label(10.0f), false, juce::Justification::centredLeft);
    m_websiteLink.setColour(juce::HyperlinkButton::textColourId, TC::flight);

    setResizable(true, true);
    setResizeLimits(c_width * 6 / 10, c_height * 6 / 10, c_width * 2, c_height * 2);
    if (auto* c = getConstrainer())
        c->setFixedAspectRatio(static_cast<double>(c_width) / c_height);

    layoutContent();
    const int saved = p.savedEditorWidth;
    const int width = saved > 0 ? juce::jlimit(c_width * 6 / 10, c_width * 2, saved) : c_width;
    setSize(width, width * c_height / c_width);
    m_sizeRestored = true;

    for (size_t i = 0; i < Presets::factory().size(); ++i)
        if (Presets::factory()[i].name.equalsIgnoreCase(p.currentPresetName)) // sessions saved before the names were Title Case
            m_presetIndex = static_cast<int>(i);

    // A newer release: the popup sits on the editor itself, above the scaled content, and scales with it.
    addChildComponent(m_updatePopup);
    m_updatePopup.setBounds(0, 0, c_width, c_height);
    m_updatePopup.setTransform(m_content.getTransform());

    startTimerHz(60);

    // Checks GitHub at most once a day in the background, silently; only an editor ever asks.
    m_updateCheck = std::make_unique<sammyj::UpdateCheck>("swooshbuckler", "swooshbuckler-v", JucePlugin_VersionString,
                                                          [this](const sammyj::UpdateInfo& info) { showUpdatePopup(info); });
}

void SwooshbucklerAudioProcessorEditor::showUpdatePopup(const sammyj::UpdateInfo& info)
{
    m_updatePopup.show(info);
}

SwooshbucklerAudioProcessorEditor::~SwooshbucklerAudioProcessorEditor()
{
    stopTimer();
    m_processor.getMods().onChange = nullptr;
    m_processor.setSwell(false);
    m_morph.finish();
    m_tooltips.setLookAndFeel(nullptr);
    if (&juce::LookAndFeel::getDefaultLookAndFeel() == &m_lnf)
        juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
    setLookAndFeel(nullptr);
}

juce::String SwooshbucklerAudioProcessorEditor::versionLine() const
{
    return "v" + Version::number();
}

WheelKnob& SwooshbucklerAudioProcessorEditor::addKnob(const char* paramId, const juce::String& caption, juce::Colour accent, const juce::String& tip)
{
    auto knob = std::make_unique<WheelKnob>(*m_fonts, accent);
    knob->caption = caption;
    // Heading (the caption, unless the tip brings its own), what it does, then how to handle it.
    knob->setTooltip((tip.containsChar('\n') ? tip : caption + "\n" + tip)
                     + "\nDouble-click to reset it. Drag a MOD jack onto it to modulate it.");
    m_content.addAndMakeVisible(*knob);
    m_attachments.push_back(std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(m_processor.getState(), paramId, *knob));
    if (auto* param = m_processor.getState().getParameter(paramId))
        knob->setDoubleClickReturnValue(true, param->convertFrom0to1(param->getDefaultValue()));
    const juce::String id(paramId);
    knob->onDragStart = [this, id] { m_morph.cancel(id); };
    m_controls[id] = knob.get();
    m_knobs.push_back(std::move(knob));
    return *m_knobs.back();
}

//==============================================================================
void SwooshbucklerAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(TC::background);
}

void SwooshbucklerAudioProcessorEditor::resized()
{
    const float scale = static_cast<float>(getWidth()) / static_cast<float>(c_width);
    m_content.setTransform(juce::AffineTransform::scale(scale));
    m_updatePopup.setTransform(juce::AffineTransform::scale(scale));
    if (m_sizeRestored)
        m_processor.savedEditorWidth = getWidth();
}

void SwooshbucklerAudioProcessorEditor::layoutContent()
{
    // Header: transport and session on the left, presets in the middle, tape and settings on the right
    {
        const int linkWidth = juce::GlyphArrangement::getStringWidthInt(m_fonts->label(10.0f), m_websiteLink.getButtonText()) + 8;
        m_websiteLink.setBounds(66, 38, linkWidth, 16);
        m_versionLink.setBounds(m_websiteLink.getRight() + 6, 38,
                                juce::GlyphArrangement::getStringWidthInt(m_fonts->label(10.0f), m_versionLink.getButtonText()) + 8, 16);
        m_titleArea = { 16, 4, 290, 34 };
    }
    m_playButton.setBounds(372, 15, 84, 30);
    m_newButton.setBounds(462, 15, 60, 30);
    m_presetPrev.setBounds(536, 15, 26, 30);
    m_presetName.setBounds(566, 15, 172, 30);
    m_presetNext.setBounds(742, 15, 26, 30);
    m_dice.setBounds(774, 15, 100, 30);
    m_tape.setBounds(888, 13, 124, 34);
    m_retroButton.setBounds(1018, 15, 86, 30);
    m_takesButton.setBounds(1110, 15, 70, 30);
    m_settingsButton.setBounds(1186, 15, 78, 30);

    // Row 1: the tracks, the output view, and the ride
    m_hold.setBounds(16, 62, 884, 384);
    m_sea.setBounds(912, 62, 352, 196);
    m_morphPad.setBounds(m_sea.getBounds().reduced(6).withTrimmedTop(18));
    m_morphButton.setBounds(m_sea.getRight() - 86, m_sea.getY() + 4, 80, 20);
    m_performPanel = { 912, 270, 352, 176 };
    m_swell.setBounds(m_performPanel.getX() + 8, m_performPanel.getY() + 28, 100, 100);
    m_throwButton.setBounds(m_performPanel.getX() + 14, m_performPanel.getY() + 126, 88, 22);
    m_rideButton.setBounds(m_performPanel.getX() + 14, m_performPanel.getY() + 150, 88, 22);
    {
        constexpr int cw = 78, ch = 70;
        const int x0 = m_performPanel.getX() + 108, y0 = m_performPanel.getY() + 34;
        const char* ids[] = { ParamIDs::energy, ParamIDs::rate, ParamIDs::fill, ParamIDs::chaos, ParamIDs::pitch, ParamIDs::throwLength };
        for (int i = 0; i < 6; ++i)
            m_controls[ids[i]]->setBounds(x0 + (i % 3) * cw, y0 + (i / 3) * ch, cw, ch);
    }
    {
        // Keys (what MIDI notes do) at the right of the Ride title bar; the panel's subtitle line is between the two.
        const int keysWidth = 70, keysLabel = 30;
        m_keysMode.setBounds(m_performPanel.getRight() - 14 - keysWidth, m_performPanel.getY() + 9, keysWidth, 22);
        m_keysLabel = { m_keysMode.getX() - keysLabel - 2, m_keysMode.getY(), keysLabel, 22 };
        const int titleEnd = m_performPanel.getX() + 30 + juce::GlyphArrangement::getStringWidthInt(m_fonts->display(21.0f), "Ride") + 12;
        m_rideStatusArea = { titleEnd, m_performPanel.getY() + 9, m_keysLabel.getX() - 4 - titleEnd, 22 };
    }
    m_followHost.setBounds(286, 15, 80, 30); // left of Play (in a host only)

    // Rows 2 and 3, in signal order
    m_sections[0] = { 1, "Bus A", "growls, tones, crackle, rumble - and the key for B", TC::movement, { 16, 458, 616, 168 } };
    m_sections[1] = { 2, "Bus B", "air and beds, ducked and filtered by A", TC::sources, { 648, 458, 616, 168 } };
    m_sections[2] = { 3, "Hits", "at the loudest moments", TC::record, { 16, 638, 616, 158 } };
    m_sections[3] = { 4, "Output", "space, loudness, limiter", TC::master, { 648, 638, 616, 158 } };

    auto row = [this](juce::Rectangle<int> panel, std::vector<const char*> ids, int rightReserve)
    {
        auto r = panel.reduced(14, 0).withTrimmedTop(40).withTrimmedBottom(8).withTrimmedRight(rightReserve);
        const int w = r.getWidth() / static_cast<int>(ids.size());
        for (const auto* id : ids)
            m_controls[id]->setBounds(r.removeFromLeft(w).reduced(3, 0));
    };
    for (int b = 0; b < ParamIDs::numBuses; ++b)
    {
        std::vector<const char*> ids;
        for (int k = 0; k < ParamIDs::numBusControls; ++k)
            if (ParamIDs::bus[b][k] != nullptr)
                ids.push_back(ParamIDs::bus[b][k]);
        row(m_sections[static_cast<size_t>(b)].bounds, ids, 0);
    }
    row(m_sections[2].bounds, { ParamIDs::hitSense, ParamIDs::hitBoost, ParamIDs::hitRelease, ParamIDs::hitHold, ParamIDs::hitFloor,
                                ParamIDs::hitDuck, ParamIDs::duckReturn }, 120);
    {
        // Modes in the Hits title bar, TAKE TURNS in bus B's.
        const auto& hits = m_sections[2].bounds;
        const int y = hits.getY() + 9;
        m_duckMode.setBounds(hits.getRight() - 14 - 92, y, 92, 22);
        m_duckModeLabel = { m_duckMode.getX() - 40, y, 38, 22 };
        m_gateMode.setBounds(m_duckModeLabel.getX() - 10 - 92, y, 92, 22);
        m_gateModeLabel = { m_gateMode.getX() - 40, y, 38, 22 };
        const auto& busB = m_sections[1].bounds;
        m_takeTurns.setBounds(busB.getRight() - 14 - 96, busB.getY() + 9, 96, 22);
        m_linkBuses.setBounds(m_takeTurns.getX() - 6 - 56, busB.getY() + 9, 56, 22);

        // PASS (the Pass Mode menu) in each bus's title bar: at the right end of A's, left of LINK in B's.
        const auto& busA = m_sections[0].bounds;
        const int passWidth = 76, passLabel = 32;
        m_passMode[0].setBounds(busA.getRight() - 14 - passWidth, busA.getY() + 9, passWidth, 22);
        m_passMode[1].setBounds(m_linkBuses.getX() - 6 - passWidth, busB.getY() + 9, passWidth, 22);
        for (size_t b = 0; b < m_passMode.size(); ++b)
            m_passModeLabel[b] = { m_passMode[b].getX() - passLabel - 2, m_passMode[b].getY(), passLabel, 22 };

        // The little waveforms, at the right of each title bar (left of any buttons and menus there).
        const auto& out = m_sections[3].bounds;
        m_sectionScopes[0]->setBounds(m_passModeLabel[0].getX() - 6 - 120, busA.getY() + 9, 120, 22);
        m_sectionScopes[1]->setBounds(m_passModeLabel[1].getX() - 6 - 64, busB.getY() + 9, 64, 22);
        m_sectionScopes[2]->setBounds(m_gateModeLabel.getX() - 6 - 120, hits.getY() + 9, 120, 22);
        m_sectionScopes[3]->setBounds(out.getRight() - 14 - 120, out.getY() + 9, 120, 22);
    }
    m_gateLamp = { m_sections[2].bounds.getRight() - 120, m_sections[2].bounds.getY() + 38, 100, 52 };
    m_aimHits.setBounds(m_gateLamp.getX() + 4, m_gateLamp.getBottom() + 2, 92, 24);
    m_syncHits.setBounds(m_gateLamp.getX() + 4, m_aimHits.getBottom() + 4, 92, 24);
    // Output, left to right: the framed PEAK TREMOLO group (it comes before Space in the signal) with its three menus
    // over its three knobs; then Space, Inflate, Push and Ceiling; then the meters and the trim fader.
    {
        const auto& out = m_sections[3].bounds;
        m_tremGroup = { out.getX() + 12, out.getY() + 38, 240, out.getHeight() - 44 };
        auto menus = m_tremGroup.reduced(6, 0).withTrimmedTop(12).withHeight(20);
        m_tremOn.setBounds(menus.removeFromLeft(58));
        menus.removeFromLeft(4);
        m_tremShape.setBounds(menus.removeFromLeft(72));
        menus.removeFromLeft(4);
        m_tremTarget.setBounds(menus);
        auto knobs = m_tremGroup.reduced(4, 0).withTrimmedTop(36).withTrimmedBottom(2);
        const int tw = knobs.getWidth() / 3;
        for (const auto* id : { ParamIDs::tremDepth, ParamIDs::tremRate, ParamIDs::tremThreshold })
            m_controls[id]->setBounds(knobs.removeFromLeft(tw).reduced(2, 0));

        m_meterArea = { out.getRight() - 150, out.getY() + 44, 100, 100 };
        m_trim.setBounds(m_meterArea.getRight() + 6, m_meterArea.getY() - 2, 30, m_meterArea.getHeight() - 16);
        m_trimLabel = { m_trim.getX() - 6, m_trim.getBottom() + 1, 42, 14 };
        auto rest = juce::Rectangle<int>(m_tremGroup.getRight() + 8, out.getY() + 40,
                                         m_meterArea.getX() - 8 - (m_tremGroup.getRight() + 8), out.getHeight() - 48);
        const int ow = rest.getWidth() / 4;
        for (const auto* id : { ParamIDs::space, ParamIDs::inflate, ParamIDs::push, ParamIDs::ceiling })
            m_controls[id]->setBounds(rest.removeFromLeft(ow).reduced(2, 0));
    }

    // Row 4: modulation (MegaMod style), across the full width
    m_modSection = { 16, 808, 1248, 190 };
    m_modPanel.setBounds(m_modSection.reduced(14, 0).withTrimmedTop(40).withTrimmedBottom(10));
    m_cablesButton.setBounds(m_modSection.getRight() - 96, m_modSection.getY() + 10, 82, 22);
    m_wires.setBounds(0, 0, c_width, c_height);
    {
        // Every knob a wire can reach, and each track's Semitones box.
        std::map<juce::String, ModTarget*> targets;
        for (const auto& [id, control] : m_controls)
            if (ModMatrix::targetIndex(id) >= 0)
                if (auto* target = dynamic_cast<ModTarget*>(control))
                    targets[id] = target;
        for (const auto& [id, target] : m_hold.getModTargets())
            targets[id] = target;
        m_wires.setTargets(std::move(targets));
    }

    m_footer = { 16, 1002, 1248, 18 };
    m_toastArea = { 640, 1002, 624, 18 };
    m_settings.setBounds(0, 0, c_width, c_height);
    m_settings.setVisible(false);
    m_takes.setBounds(0, 0, c_width, c_height);
    m_takes.setVisible(false);
    m_about.setBounds(0, 0, c_width, c_height);
    m_about.setVisible(false);

}

void SwooshbucklerAudioProcessorEditor::paintContent(juce::Graphics& g)
{
    g.fillAll(TC::background);
    const auto& f = *m_fonts;

    SwooshLookAndFeel::drawCutlasses(g, { 18.0f, 12.0f, 38.0f, 34.0f }, TC::master, TC::brass);
    g.setFont(f.display(36.0f));
    g.setColour(TC::text);
    g.drawText("Swooshbuckler", juce::Rectangle<float>(66.0f, 2.0f, 300.0f, 38.0f), juce::Justification::centredLeft);

    SwooshLookAndFeel::drawRope(g, { 16.0f, 56.0f }, { 1264.0f, 56.0f }, TC::outlineSoft);

    SwooshLookAndFeel::drawPanel(g, m_performPanel.toFloat(), "Ride", {}, TC::flight, f);
    {
        // The subtitle line doubles as the readout: the time to a throw's peak, or the keys that are held.
        const bool readout = m_rideStatus.isNotEmpty();
        g.setFont(readout ? f.label(12.0f) : f.body(11.0f));
        g.setColour(readout ? TC::flight : TC::textDim);
        g.drawText(readout ? m_rideStatus : juce::String("the faders move by themselves"), m_rideStatusArea, juce::Justification::centredLeft, false);
    }
    for (const auto& sec : m_sections)
        SwooshLookAndFeel::drawPanel(g, sec.bounds.toFloat(), sec.title, sec.subtitle, sec.accent, f);
    SwooshLookAndFeel::drawPanel(g, m_modSection.toFloat(), "Modulation", "drag a jack onto any knob to wire it", TC::brass, f);
    g.setFont(f.label(10.5f));
    g.setColour(TC::textDim);
    g.drawText("Keys", m_keysLabel, juce::Justification::centredRight);
    g.drawText("GATE", m_gateModeLabel, juce::Justification::centredRight);
    g.drawText("DUCK", m_duckModeLabel, juce::Justification::centredRight);
    for (const auto& label : m_passModeLabel)
        g.drawText("PASS", label, juce::Justification::centredRight);

    auto arrow = [&g](juce::Point<float> c, bool down)
    {
        juce::Path p;
        if (down) { p.startNewSubPath(c.x - 6.0f, c.y - 3.0f); p.lineTo(c.x, c.y + 3.0f); p.lineTo(c.x + 6.0f, c.y - 3.0f); }
        else      { p.startNewSubPath(c.x - 3.0f, c.y - 6.0f); p.lineTo(c.x + 3.0f, c.y); p.lineTo(c.x - 3.0f, c.y + 6.0f); }
        g.setColour(TC::brass.withAlpha(0.8f));
        g.strokePath(p, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    };
    arrow({ 458.0f, 452.0f }, true);   // tracks -> buses
    arrow({ 640.0f, 542.0f }, false);  // A -> B
    arrow({ 324.0f, 632.0f }, true);   // buses -> hits
    arrow({ 640.0f, 716.0f }, false);  // hits -> output

    // Hit gate lamp
    {
        const auto lamp = juce::Rectangle<float>(26.0f, 26.0f).withCentre(m_gateLamp.toFloat().getCentre().translated(0.0f, -8.0f));
        g.setColour(TC::record.withAlpha(0.15f + 0.85f * m_gate));
        g.fillEllipse(lamp);
        g.setColour(TC::record.withAlpha(0.15f + 0.35f * m_gate));
        g.fillEllipse(lamp.expanded(8.0f * m_gate));
        g.setColour(TC::outline);
        g.drawEllipse(lamp, 1.0f);
        g.setFont(f.label(10.0f));
        g.setColour(TC::textDim);
        g.drawText(m_aimHits.getToggleState() ? "AIMED HITS" : "HIT GATE", m_gateLamp.withTop(m_gateLamp.getBottom() - 16), juce::Justification::centred);
    }

    // Output meters
    {
        auto m = m_meterArea.toFloat();
        const auto readout = m.removeFromTop(16.0f); // the deepest recent gain reduction in dB, across the top
        // Left to right: the GR scale, the GR bar, the OUT bar, the OUT scale.
        auto bars = m.withTrimmedBottom(20.0f).reduced(0.0f, 2.0f);
        const auto grScale = bars.removeFromLeft(15.0f);
        const auto grBar = bars.removeFromLeft(22.0f);
        bars.removeFromLeft(10.0f);
        const auto outBar = bars.removeFromLeft(22.0f);
        const auto outScale = bars.removeFromLeft(24.0f);
        for (auto bar : { grBar, outBar })
        {
            g.setColour(TC::scope);
            g.fillRoundedRectangle(bar, 3.0f);
            g.setColour(TC::outline);
            g.drawRoundedRectangle(bar, 3.0f, 1.0f);
        }

        // GR grows down from the top, 12 dB at the bottom; OUT grows up from the bottom, 0 dBFS at the top.
        auto grY = [&grBar](float db) { return grBar.getY() + grBar.getHeight() * juce::jlimit(0.0f, 1.0f, -db / 12.0f); };
        auto outY = [&outBar](float db) { return outBar.getBottom() - outBar.getHeight() * outMeterPosition(db); };

        g.setColour(TC::movement);
        g.fillRoundedRectangle(grBar.withBottom(grY(m_grDb)), 3.0f);
        const float outDb = juce::Decibels::gainToDecibels(m_outLevel, -60.0f);
        g.setColour(TC::master);
        g.fillRoundedRectangle(outBar.withTop(outY(outDb)), 3.0f);

        // Peak holds: the deepest reduction and the highest level of the last moments stay as a line.
        if (m_grHoldDb < -0.05f)
        {
            g.setColour(TC::movement.brighter(0.5f));
            g.fillRect(juce::Rectangle<float>(grBar.getX() + 1.0f, juce::jmin(grY(m_grHoldDb), grBar.getBottom() - 1.5f), grBar.getWidth() - 2.0f, 1.5f));
        }
        if (m_outHold > 0.001f)
        {
            const float holdDb = juce::Decibels::gainToDecibels(m_outHold, -60.0f);
            g.setColour(TC::text);
            g.fillRect(juce::Rectangle<float>(outBar.getX() + 1.0f, juce::jmax(outY(holdDb) - 0.75f, outBar.getY()), outBar.getWidth() - 2.0f, 1.5f));
        }

        // Scales in dB: a tick against each bar and its number beside it.
        g.setFont(f.label(8.0f));
        for (const int db : { 3, 6, 12 })
        {
            const float y = juce::jmin(grY(static_cast<float>(-db)), grBar.getBottom() - 0.5f);
            g.setColour(TC::outline);
            g.fillRect(juce::Rectangle<float>(grBar.getX() - 3.0f, y - 0.5f, 3.0f, 1.0f));
            g.setColour(TC::textFaint);
            g.drawText(juce::String(db), juce::Rectangle<float>(grScale.getX(), juce::jmin(y - 5.0f, grBar.getBottom() - 9.0f), grScale.getWidth() - 4.0f, 10.0f),
                       juce::Justification::centredRight);
        }
        for (const int db : { 0, 6, 12, 24, 48 })
        {
            const float y = juce::jmax(outY(static_cast<float>(-db)), outBar.getY() + 0.5f);
            g.setColour(TC::outline);
            g.fillRect(juce::Rectangle<float>(outBar.getRight(), y - 0.5f, 3.0f, 1.0f));
            g.setColour(TC::textFaint);
            g.drawText(db == 0 ? juce::String("0") : "-" + juce::String(db),
                       juce::Rectangle<float>(outScale.getX() + 4.0f, juce::jmax(y - 5.0f, outBar.getY() - 1.0f), outScale.getWidth() - 4.0f, 10.0f),
                       juce::Justification::centredLeft);
        }

        g.setFont(f.label(10.0f));
        g.setColour(TC::textDim);
        g.drawText("GR", grBar.withY(grBar.getBottom() + 3.0f).withHeight(14.0f).expanded(10.0f, 0.0f), juce::Justification::centred);
        g.drawText("OUT", outBar.withY(outBar.getBottom() + 3.0f).withHeight(14.0f).expanded(10.0f, 0.0f), juce::Justification::centred);
        g.drawText("TRIM", m_trimLabel, juce::Justification::centred);

        // The PEAK TREMOLO group: a thin frame with its name in the top edge.
        const auto frame = m_tremGroup.toFloat().reduced(0.5f);
        g.setColour(TC::movement.withAlpha(0.45f));
        g.drawRoundedRectangle(frame, 5.0f, 1.0f);
        g.setFont(f.label(9.5f));
        const juce::String name = "PEAK TREMOLO";
        const float nw = juce::GlyphArrangement::getStringWidth(g.getCurrentFont(), name) + 10.0f;
        const auto tag = juce::Rectangle<float>(frame.getX() + 10.0f, frame.getY() - 6.0f, nw, 12.0f);
        g.setColour(TC::panel);
        g.fillRect(tag);
        g.setColour(TC::movement);
        g.drawText(name, tag, juce::Justification::centred);

        g.setFont(f.label(10.0f));
        g.setColour(m_grHoldDb < -0.05f ? TC::movement : TC::textFaint);
        g.drawText("GR " + juce::String(m_grHoldDb, 1) + " dB", readout.withWidth(84.0f), juce::Justification::centred);
    }

    g.setFont(f.body(11.0f));
    g.setColour(TC::textFaint);
    g.drawText("Drop sounds into Tracks, press PLAY, ARM the tape and let it ride.  Hold SWELL (or space) to push.",
               m_footer.withWidth(620), juce::Justification::centredLeft, true);
    if (m_toast.isNotEmpty())
    {
        const float age = static_cast<float>(juce::Time::getMillisecondCounterHiRes() * 0.001 - m_toastTime);
        g.setColour(TC::flight.withAlpha(juce::jlimit(0.0f, 1.0f, 8.0f - age)));
        g.drawText(m_toast + (m_toastFile != juce::File() ? "  (click for Takes)" : ""), m_toastArea, juce::Justification::centredRight, true);
    }
}

//==============================================================================
bool SwooshbucklerAudioProcessorEditor::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::spaceKey)
    {
        if (! m_spaceHeld)
        {
            m_spaceHeld = true;
            m_processor.setSwell(true);
            m_swell.press(true);
        }
        return true;
    }
    if ((key.getKeyCode() == 'f' || key.getKeyCode() == 'F') && ! key.getModifiers().isAnyModifierKeyDown())
    {
        m_processor.throwWhoosh(); // F: throw one whoosh
        return true;
    }
    if (key == juce::KeyPress::escapeKey && m_updatePopup.isVisible()) // when the popup didn't get the focus
    {
        m_updatePopup.setVisible(false);
        return true;
    }
    if (key == juce::KeyPress::escapeKey && (m_settings.isVisible() || m_about.isVisible() || m_takes.isVisible()))
    {
        m_settings.setVisible(false);
        m_about.setVisible(false);
        m_takes.setVisible(false);
        return true;
    }
    return false;
}

bool SwooshbucklerAudioProcessorEditor::keyStateChanged(bool)
{
    if (m_spaceHeld && ! juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::spaceKey))
    {
        m_spaceHeld = false;
        m_processor.setSwell(false);
        m_swell.press(false);
        return true;
    }
    return false;
}

bool SwooshbucklerAudioProcessorEditor::isInterestedInFileDrag(const juce::StringArray& files)
{
    for (const auto& f : files)
        if (CargoHoldView::isAudioFile(f))
            return true;
    return false;
}

void SwooshbucklerAudioProcessorEditor::filesDropped(const juce::StringArray& files, int, int)
{
    const auto text = m_hold.loadFiles(files);
    if (text.isNotEmpty())
        toast(text);
}

void SwooshbucklerAudioProcessorEditor::toast(const juce::String& text, juce::File file)
{
    m_toast = Theme::localiseShortcuts(text);
    m_toastFile = file;
    m_toastTime = juce::Time::getMillisecondCounterHiRes() * 0.001;
    m_content.repaint(m_footer);
}

void SwooshbucklerAudioProcessorEditor::showTakes()
{
    m_settings.setVisible(false);
    m_about.setVisible(false);
    m_takes.setVisible(true);
    m_takes.toFront(false);
}

void SwooshbucklerAudioProcessorEditor::toggleLog()
{
    if (m_processor.isRecording())
    {
        const double dropped = m_processor.getRecordingDroppedSeconds(); // read before the take closes
        m_processor.stopRecording();
        const auto file = m_processor.getLastRecording();
        const int files = m_processor.getFilesThisTake();
        m_takesSeen = m_processor.getTakesGeneration(); // this message covers the files just closed
        if (files <= 0 || file == juce::File())
            toast("Nothing was recorded: there was no sound while the tape was armed");
        else
            toast((files > 1 ? "Saved " + juce::String(files) + " files, the last is " : juce::String("Saved ")) + file.getFileName()
                      + (dropped > 0.005 ? " (the disk fell behind: " + juce::String(dropped, 2) + " s are missing)" : juce::String()),
                  file);
    }
    else
    {
        juce::String error;
        if (! m_processor.arm(error))
            toast("Couldn't arm the tape: " + error);
        else if (m_processor.getOneFilePerWhoosh())
            toast("Armed: a new file for every whoosh, into " + m_processor.getRecordFolder().getFileName());
        else
            toast((m_processor.getSkipSilence() ? "Armed: recording whenever there is sound, into " : "Armed: recording all the time, into ")
                  + m_processor.getCurrentRecording().getFileName());
    }
}

void SwooshbucklerAudioProcessorEditor::saveRetro()
{
    juce::File written;
    juce::String error;
    if (m_processor.saveRetro(written, error))
        toast("Saved " + written.getFileName(), written);
    else
        toast(error);
}

//==============================================================================
void SwooshbucklerAudioProcessorEditor::showPresetMenu()
{
    juce::PopupMenu menu;
    menu.addSectionHeader("PRESETS");
    const auto& presets = Presets::factory();
    for (size_t i = 0; i < presets.size(); ++i)
        menu.addItem(static_cast<int>(i) + 1, presets[i].name, true, static_cast<int>(i) == m_presetIndex);
    menu.addSeparator();
    menu.addItem(1000, "Back to Defaults");
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(m_presetName), [this](int result)
    {
        if (result == 1000)
        {
            m_presetIndex = -1;
            applyPreset({ "Default", {} }, true);
            m_processor.currentPresetName = "Default";
        }
        else if (result > 0)
        {
            m_presetIndex = result - 1;
            const auto& preset = Presets::factory()[static_cast<size_t>(m_presetIndex)];
            applyPreset(preset, true);
            m_processor.currentPresetName = preset.name;
        }
    });
}

void SwooshbucklerAudioProcessorEditor::stepPreset(int delta)
{
    const int count = static_cast<int>(Presets::factory().size());
    m_presetIndex = ((m_presetIndex < 0 ? (delta > 0 ? -1 : 0) : m_presetIndex) + delta + count) % count;
    const auto& preset = Presets::factory()[static_cast<size_t>(m_presetIndex)];
    applyPreset(preset, true);
    m_processor.currentPresetName = preset.name;
}

void SwooshbucklerAudioProcessorEditor::applyPreset(const Presets::Preset& preset, bool includeMode)
{
    m_morph.finish();
    for (auto* p : m_processor.getParameters())
    {
        auto* param = dynamic_cast<juce::RangedAudioParameter*>(p);
        if (param == nullptr)
            continue;
        const auto id = param->getParameterID();
        if (Presets::isSessionParameter(id))
            continue;
        const auto it = preset.values.find(id);
        // Randomise leaves each bus's Pass Mode as it is set (a factory preset picks it).
        if (! includeMode && it == preset.values.end() && (id == ParamIDs::passMode[0] || id == ParamIDs::passMode[1]))
            continue;
        const float target = it != preset.values.end() ? param->convertTo0to1(it->second) : param->getDefaultValue();
        if (std::abs(param->getValue() - target) < 1.0e-5f)
            continue;
        param->beginChangeGesture();
        m_morph.items.push_back({ param, param->getValue(), 0.0f, target });
    }
}

//==============================================================================
void SwooshbucklerAudioProcessorEditor::syncMorphPad()
{
    auto& st = m_processor.getState();
    auto value = [&st](const char* id) { return st.getRawParameterValue(id)->load(); };
    // The MORPH button shows the pad; ON on the pad switches the morph itself.
    const bool shown = m_morphButton.getToggleState();
    const bool on = value(ParamIDs::morphOn) > 0.5f;
    m_morphButton.setButtonText(on ? "MORPH ON" : "MORPH");
    m_morphButton.setColour(juce::TextButton::buttonOnColourId, on ? TC::go : TC::filter);
    if (on) m_morphButton.setColour(juce::TextButton::textColourOffId, TC::go);
    else    m_morphButton.removeColour(juce::TextButton::textColourOffId);
    if (m_morphPad.isVisible() != shown)
    {
        m_morphPad.setVisible(shown);
        if (shown)
            m_morphPad.toFront(false);
        m_morphButton.toFront(false);
    }
    m_morphPad.setMorphEnabled(on);
    if (! shown)
        return;
    m_morphPad.setMode(juce::roundToInt(value(ParamIDs::morphMode)));
    m_morphPad.setShape(juce::roundToInt(value(ParamIDs::morphShape)));
    m_morphPad.setWander(value(ParamIDs::morphWander) > 0.5f, value(ParamIDs::morphSpeed));
    m_morphPad.setXY(value(ParamIDs::morphX), value(ParamIDs::morphY));
}

juce::String SwooshbucklerAudioProcessorEditor::rideStatus() const
{
    // A throw on its way in: the time until its peak is heard, to place it against picture.
    if (const float peakIn = m_processor.getEngine().getThrowPeakIn(); peakIn >= 0.0f)
        return "Peak in " + juce::String(peakIn, 1) + " s";
    // The held keys (the first few, then how many more).
    juce::String names;
    int shown = 0, more = 0;
    for (int note = 0; note < 128; ++note)
        if ((m_processor.getHeldKeys(note >> 6) >> (note & 63)) & 1u)
        {
            if (shown < 4)
                names << (shown++ > 0 ? " " : "") << juce::MidiMessage::getMidiNoteName(note, true, true, 4);
            else
                ++more;
        }
    return more > 0 ? names + " +" + juce::String(more) : names;
}

void SwooshbucklerAudioProcessorEditor::showAimControls()
{
    // AIM times the hits itself and holds the gate open: dim what only the gate (or the sync beat) uses. GATE Open
    // holds the gate open too, so the knobs that shape the gate do nothing there either. Energy does nothing with
    // Auto Ride off.
    auto& state = m_processor.getState();
    const bool aim = state.getRawParameterValue(ParamIDs::hitAim)->load() > 0.5f;
    const bool open = ! aim && juce::roundToInt(state.getRawParameterValue(ParamIDs::gateMode)->load()) == 3;
    const bool ride = state.getRawParameterValue(ParamIDs::rideOn)->load() > 0.5f;
    const int shown = (aim ? 1 : 0) + (open ? 2 : 0) + (ride ? 4 : 0);
    if (m_aimShown == shown)
        return;
    m_aimShown = shown;
    auto control = [this](const char* id) -> juce::Component*
    {
        const auto it = m_controls.find(juce::String(id));
        return it != m_controls.end() ? it->second : nullptr;
    };
    auto dim = [](juce::Component* c, bool off)
    {
        if (c == nullptr)
            return;
        c->setEnabled(! off);
        c->setAlpha(off ? 0.35f : 1.0f);
    };
    dim(&m_gateMode, aim);
    dim(&m_syncHits, aim);
    for (const auto* id : { ParamIDs::hitRelease, ParamIDs::hitHold, ParamIDs::hitFloor })
        dim(control(id), aim || open);
    dim(control(ParamIDs::hitSense), open);
    if (auto* energy = control(ParamIDs::energy))
        energy->setAlpha(ride ? 1.0f : 0.45f); // still usable, so it can be set before switching the ride on
    m_content.repaint(m_gateLamp.expanded(10));
}

void SwooshbucklerAudioProcessorEditor::showLinkedKnobs()
{
    // Linked, bus B's Doppler and Width follow bus A: dim them (they keep their own values for when it's unlinked).
    const bool linked = m_processor.getState().getRawParameterValue(ParamIDs::linkBuses)->load() > 0.5f;
    if (m_linkShown == (linked ? 1 : 0))
        return;
    m_linkShown = linked ? 1 : 0;
    for (const int k : { static_cast<int>(ParamIDs::Doppler), static_cast<int>(ParamIDs::Width) })
    {
        const auto it = m_controls.find(juce::String(ParamIDs::bus[1][k]));
        if (it == m_controls.end() || it->second == nullptr)
            continue;
        it->second->setEnabled(! linked);
        it->second->setAlpha(linked ? 0.35f : 1.0f);
    }
    // And B flies A's passes in A's Pass Mode.
    m_passMode[1].setEnabled(! linked);
    m_passMode[1].setAlpha(linked ? 0.35f : 1.0f);
}

void SwooshbucklerAudioProcessorEditor::timerCallback()
{
    showLinkedKnobs();
    showAimControls();
    {
        // Peak tremolo off: its knobs and menus dim, but stay usable so it can be set up before switching it on.
        const bool on = m_processor.getState().getRawParameterValue(ParamIDs::tremOn)->load() > 0.5f;
        if (m_tremShown != (on ? 1 : 0))
        {
            m_tremShown = on ? 1 : 0;
            std::vector<juce::Component*> parts { &m_tremShape, &m_tremTarget };
            for (const auto* id : { ParamIDs::tremDepth, ParamIDs::tremRate, ParamIDs::tremThreshold })
                parts.push_back(m_controls[id]);
            for (auto* c : parts)
                c->setAlpha(on ? 1.0f : 0.45f);
        }
    }
    if ((++m_morphSyncTick % 6) == 0)
        syncMorphPad();

    if (! m_morph.items.empty())
        m_morph.step();


    const bool playing = m_processor.isPlaying();
    if (m_playButton.getToggleState() != playing)
    {
        m_playButton.setToggleState(playing, juce::dontSendNotification);
        m_playButton.setButtonText(playing ? "STOP" : "PLAY");
    }

    if (m_presetName.getButtonText() != m_processor.currentPresetName)
        m_presetName.setButtonText(m_processor.currentPresetName);

    // The LAST button says how much it keeps, Takes how many there are; a whoosh file closed while the tape rolls
    // is announced, and so is a file the tape could not open.
    if (const int minutes = m_processor.getRetroMinutes(); minutes != m_retroShown)
    {
        m_retroShown = minutes;
        m_retroButton.setButtonText(minutes == 1 ? juce::String("LAST 60s") : "LAST " + juce::String(minutes) + " MIN");
    }
    if (const int generation = m_processor.getTakesGeneration(); generation != m_takesShown)
    {
        m_takesShown = generation;
        const auto& takes = m_processor.getTakes();
        m_takesButton.setButtonText(takes.empty() ? juce::String("Takes") : "Takes " + juce::String(static_cast<int>(takes.size())));
        if (m_takesSeen >= 0 && generation != m_takesSeen && m_processor.isRecording() && ! takes.empty() && takes.front().kind == "Whoosh")
            toast("Saved " + takes.front().file.getFileName(), takes.front().file);
        m_takesSeen = generation;
    }
    if ((++m_slowTick % 30) == 0)
        if (const auto error = m_processor.takeRecordingError(); error.isNotEmpty())
            toast("The tape couldn't write: " + error);
    if (m_followHost.getToggleState() != m_processor.getFollowHost())
        m_followHost.setToggleState(m_processor.getFollowHost(), juce::dontSendNotification);
    if (const auto status = rideStatus(); status != m_rideStatus)
    {
        m_rideStatus = status;
        m_content.repaint(m_rideStatusArea);
    }

    const float gate = m_processor.getEngine().getHitGate();
    if (std::abs(gate - m_gate) > 0.01f)
    {
        m_gate = gate;
        m_content.repaint(m_gateLamp.expanded(10));
    }


    // Meters. The processor hands over the deepest reduction and the highest level since the last look, so nothing
    // between two frames is missed; the fall-back and the peak holds run on the clock, not on frames or buffers.
    {
        const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
        const float dt = static_cast<float>(juce::jlimit(0.0, 0.25, now - m_meterTime));
        m_meterTime = now;

        const float grNow = juce::jmin(0.0f, m_processor.takeGainReductionDb());
        const float gr = juce::jmin(grNow, juce::jmin(0.0f, m_grDb + c_grFallDbPerSecond * dt));
        float grHold = m_grHoldDb;
        if (grNow <= grHold)
        {
            grHold = grNow;
            m_grHoldTime = now;
        }
        else if (now - m_grHoldTime > c_meterHoldSeconds)
            grHold = juce::jmin(gr, grHold + 2.0f * c_grFallDbPerSecond * dt);

        const float outNow = m_processor.takeOutputPeak();
        const float out = juce::jmax(outNow, m_processor.getOutputPeak());
        float outHold = m_outHold;
        if (outNow >= outHold)
        {
            outHold = outNow;
            m_outHoldTime = now;
        }
        else if (now - m_outHoldTime > c_meterHoldSeconds)
            outHold = juce::jmax(out, outHold * std::exp(-dt / 0.15f));

        if (std::abs(gr - m_grDb) > 0.05f || std::abs(out - m_outLevel) > 0.002f
            || std::abs(grHold - m_grHoldDb) > 0.05f || std::abs(outHold - m_outHold) > 0.002f)
        {
            m_grDb = gr;
            m_outLevel = out;
            m_grHoldDb = grHold;
            m_outHold = outHold;
            m_content.repaint(m_meterArea);
        }
    }

    if (m_toast.isNotEmpty())
    {
        const double age = juce::Time::getMillisecondCounterHiRes() * 0.001 - m_toastTime;
        if (age > 8.0)
        {
            m_toast.clear();
            m_toastFile = juce::File();
        }
        m_content.repaint(m_footer);
    }
}
