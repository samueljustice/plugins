#include "PluginEditor.h"
#include "Version.h"

namespace TC = Theme::Colours;

#if JUCE_MAC
/** The app's menu bar. Every item is an action, shown with the key the user has given it. */
struct SwooshbucklerAudioProcessorEditor::MenuBar final : public juce::MenuBarModel
{
    explicit MenuBar(SwooshbucklerAudioProcessorEditor& o) : owner(o) {}

    juce::StringArray getMenuBarNames() override { return { "File", "Edit", "Transport", "View" }; }

    juce::PopupMenu getMenuForIndex(int index, const juce::String&) override
    {
        using A = Shortcuts::Action;
        juce::PopupMenu menu;
        auto add = [&](A action) { owner.m_keys.addToMenu(menu, action, static_cast<int>(action) + 1, owner.canDoAction(action)); };
        switch (index)
        {
            case 0: add(A::NewSession); add(A::OpenSession); menu.addSeparator(); add(A::SaveSession); add(A::SaveSessionAs); break;
            case 1: add(A::Undo); add(A::Redo); menu.addSeparator(); add(A::Randomise); add(A::PreviousRoll); add(A::NextRoll);
                    add(A::KeepRoll); menu.addSeparator(); add(A::PreviousPreset); add(A::NextPreset); break;
            case 2: add(A::PlayStop); add(A::Throw); menu.addSeparator(); add(A::ArmTape); add(A::SaveLast); break;
            case 3: add(A::Takes); add(A::Variations); menu.addSeparator(); add(A::MorphPad); add(A::Cables);
                    menu.addSeparator(); add(A::Settings); break;
            default: break;
        }
        return menu;
    }

    void menuItemSelected(int itemId, int) override
    {
        if (itemId > 0 && itemId <= Shortcuts::numActions)
            owner.doAction(static_cast<Shortcuts::Action>(itemId - 1));
    }

    SwooshbucklerAudioProcessorEditor& owner;
};
#endif

namespace
{

}

//==============================================================================
// SwellButton
void SwooshbucklerAudioProcessorEditor::SwellButton::frame(float dt)
{
    // The one Swell spring (Motion::Springs), stepped with the real time of the frame.
    const bool moving = m_scale.stepSwell(dt);
    const float glow = m_glow + (m_processor.getEngine().getEnergy() - m_glow) * juce::jmin(1.0f, dt * 12.0f);
    const bool changed = moving || std::abs(glow - m_glow) > 2.0e-3f;
    m_glow = glow;
    if (changed)
        repaint(); // only when something moved: idle repaints cost frame rate
}

void SwooshbucklerAudioProcessorEditor::SwellButton::paint(juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    const auto centre = b.getCentre();
    const float radius = juce::jmin(b.getWidth(), b.getHeight()) * 0.5f * 0.8f * m_scale.value;

    auto disc = [&centre](float r) { return juce::Rectangle<float>(r * 2.0f, r * 2.0f).withCentre(centre); };

    // The glow: it follows the energy of the ride.
    if (m_glow > 0.01f)
    {
        juce::ColourGradient halo(TC::flight.withAlpha(0.35f * m_glow), centre.x, centre.y,
                                  TC::flight.withAlpha(0.0f), centre.x + radius * 1.25f, centre.y, true);
        g.setGradientFill(halo);
        g.fillEllipse(disc(radius * 1.25f));
    }

    // A big brass wheel boss, flat: eight block handles around it, a dark lip under it, a pressed line inside its edge.
    for (int i = 0; i < 8; ++i)
    {
        // Turned half a step, so none points straight up at the nameplate or down at Throw.
        const float a = juce::MathConstants<float>::twoPi * (static_cast<float>(i) + 0.5f) / 8.0f + (1.0f - m_scale.value) * 2.0f;
        const juce::Point<float> dir { std::sin(a), -std::cos(a) };
        g.setColour(TC::edgeDark);
        g.drawLine(juce::Line<float>(centre + dir * radius * 0.9f, centre + dir * (radius * 1.15f + 1.0f)), 8.0f);
        g.setColour(TC::brass);
        g.drawLine(juce::Line<float>(centre + dir * radius * 0.9f, centre + dir * radius * 1.15f), 6.0f);
    }
    g.setColour(TC::edgeDark);
    g.fillEllipse(disc(radius + 1.0f).translated(0.0f, 3.0f));
    g.setColour(TC::brassDark);
    g.fillEllipse(disc(radius).translated(0.0f, 2.5f));
    g.setColour(TC::edgeDark);
    g.fillEllipse(disc(radius + 1.0f));
    g.setColour(TC::brass);
    g.fillEllipse(disc(radius));
    g.setColour(TC::brassDark.withAlpha(0.7f));
    g.drawEllipse(disc(radius - 4.5f), 1.0f);
    g.setColour(juce::Colours::white.withAlpha(0.3f));
    g.drawEllipse(disc(radius - 5.5f), 1.0f);

    // The word is part of the drawing: it grows and shrinks with the button. The hint under it is ordinary text.
    Theme::stampText(g, "Swell", juce::Rectangle<float>(radius * 1.7f, radius * 0.7f).withCentre(centre.translated(0.0f, -radius * 0.1f)),
                     m_fonts.art(radius * 0.56f), juce::Justification::centred, TC::window);
    g.setColour(TC::window.withAlpha(0.85f));
    Theme::fitText(g, m_keyHint.isEmpty() ? juce::String("Hold") : "Hold or " + m_keyHint,
                   juce::Rectangle<float>(radius * 1.6f, 14.0f).withCentre(centre.translated(0.0f, radius * 0.42f)),
                   m_fonts.caption(), juce::Justification::centred);
}

//==============================================================================
// TapeButton
void SwooshbucklerAudioProcessorEditor::TapeButton::frame(float dt)
{
    const bool capturing = m_processor.isCapturing(), armed = m_processor.isRecording();
    // The reels spin up over a quarter of a second and coast down over six tenths, so starting and stopping the
    // tape reads as a machine rather than a switch.
    const float wanted = capturing ? 1.0f : 0.0f;
    const float tau = (wanted > m_speed ? Motion::Ms::spinUp : Motion::Ms::coastDown) * 0.001f;
    const float was = m_speed;
    m_speed += (wanted - m_speed) * juce::jmin(1.0f, dt / tau);
    // A click on Last flicks them back half a turn, then they settle.
    float flick = 0.0f;
    if (std::abs(m_rewind) > 1.0e-3f)
    {
        flick = m_rewind * juce::jmin(1.0f, dt / 0.3f);
        m_rewind -= flick;
    }
    m_angle -= m_speed * 10.8f * dt - flick; // anticlockwise: the reels' bottoms, and the tape, run left to right
    // Armed and waiting, the lamp breathes once a second between a third and full; capturing, it is solid.
    m_breath += dt;
    const float lamp = ! armed ? 0.0f
                               : (capturing ? 1.0f : 0.35f + 0.65f * (0.5f + 0.5f * std::sin(m_breath * juce::MathConstants<float>::twoPi)));
    const bool changed = armed != m_wasArmed || std::abs(lamp - m_lamp) > 0.01f || m_speed > 1.0e-3f || was > 1.0e-3f
                         || std::abs(m_rewind) > 1.0e-3f || (armed && capturing);
    m_lamp = lamp;
    m_wasArmed = armed;
    if (changed)
        repaint();
}

void SwooshbucklerAudioProcessorEditor::TapeButton::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat().reduced(0.5f);
    const bool armed = m_processor.isRecording();
    const bool capturing = m_processor.isCapturing();

    // A control plate like the buttons beside it; while it is armed it is tinted cherry, with a cherry edge
    // (not a whole cherry plate: the reels and the time have to read on it).
    SwooshLookAndFeel::drawControlPlate(g, getLocalBounds().toFloat(), TC::record, Theme::Look::Normal, false, isMouseOver() && ! armed, false);
    b = SwooshLookAndFeel::plateFace(getLocalBounds().toFloat(), false);
    if (armed)
    {
        g.setColour(TC::record.withAlpha(0.2f));
        Theme::Plate::fill(g, b, Theme::Cut::control);
        g.setColour(TC::record.withAlpha(0.9f));
        Theme::Plate::stroke(g, b, Theme::Cut::control);
    }

    // Cassette body with two reels and the tape between them
    auto icon = b.removeFromLeft(b.getHeight() * 1.6f).reduced(5.0f, 5.0f);
    g.setColour(TC::brass.withAlpha(0.85f));
    Theme::Plate::stroke(g, icon.expanded(0.6f), 0.0f, 1.2f);
    const float r = icon.getHeight() * 0.3f;
    const juce::Point<float> c1 { icon.getX() + icon.getWidth() * 0.3f, icon.getCentreY() - 1.0f };
    const juce::Point<float> c2 { icon.getX() + icon.getWidth() * 0.7f, icon.getCentreY() - 1.0f };
    for (const auto& c : { c1, c2 })
    {
        g.setColour(TC::scope);
        g.fillEllipse(juce::Rectangle<float>(r * 2.0f, r * 2.0f).withCentre(c));
        g.setColour(m_speed > 0.02f ? TC::brass.interpolatedWith(TC::record, juce::jlimit(0.0f, 1.0f, m_speed)) : TC::brass);
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
    juce::String text = "Arm";
    if (armed && capturing)
        text = "Rec " + juce::String::formatted("%02d:%02d", static_cast<int>(m_processor.getRecordedSeconds()) / 60,
                                                static_cast<int>(m_processor.getRecordedSeconds()) % 60);
    else if (armed)
        text = "Armed";
    if (armed)
    {
        // Round object: a lamp. It breathes while the tape waits and is solid while it writes.
        g.setColour(TC::record.withAlpha(juce::jlimit(0.25f, 1.0f, m_lamp)));
        g.fillEllipse(juce::Rectangle<float>(7.0f, 7.0f).withCentre({ b.getX() + 6.0f, b.getCentreY() }));
        b.removeFromLeft(12.0f);
    }
    Theme::stampText(g, text, b.withTrimmedRight(5.0f), m_fonts.control(), juce::Justification::centredLeft, TC::text);
}

//==============================================================================
// Morph
bool SwooshbucklerAudioProcessorEditor::Morph::step(float dt)
{
    dt = juce::jlimit(0.0f, Motion::c_maxStep, dt);
    const float omega = juce::MathConstants<float>::twoPi * Motion::Springs::swellHz;
    bool moving = false;
    for (auto& it : items)
    {
        if (it.param == nullptr)
            continue;
        if (it.delay > 0.0f)
        {
            it.delay -= dt;
            moving = true;
            continue; // it has not set off yet: the wave has not reached it
        }
        it.vel += (omega * omega * (it.target - it.pos) - 2.0f * Motion::Springs::swellDamping * omega * it.vel) * dt;
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
// The keys, in Settings
SwooshbucklerAudioProcessorEditor::ShortcutsList::ShortcutsList(SwooshbucklerAudioProcessorEditor& owner) : m_owner(owner)
{
    setWantsKeyboardFocus(true);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

juce::Rectangle<int> SwooshbucklerAudioProcessorEditor::ShortcutsList::rowBounds(int index) const
{
    return { 0, index * c_row, getWidth(), c_row };
}

int SwooshbucklerAudioProcessorEditor::ShortcutsList::rowAt(juce::Point<int> p) const
{
    const int index = p.y / c_row;
    return juce::isPositiveAndBelow(index, Shortcuts::numActions) && getLocalBounds().contains(p) ? index : -1;
}

void SwooshbucklerAudioProcessorEditor::ShortcutsList::stopListening()
{
    if (m_listening < 0)
        return;
    m_listening = -1;
    repaint();
}

void SwooshbucklerAudioProcessorEditor::ShortcutsList::mouseMove(const juce::MouseEvent& e)
{
    if (const int row = rowAt(e.getPosition()); row != m_over)
    {
        m_over = row;
        repaint();
    }
}

void SwooshbucklerAudioProcessorEditor::ShortcutsList::mouseExit(const juce::MouseEvent&)
{
    if (m_over >= 0)
    {
        m_over = -1;
        repaint();
    }
}

void SwooshbucklerAudioProcessorEditor::ShortcutsList::mouseDown(const juce::MouseEvent& e)
{
    const int row = rowAt(e.getPosition());
    if (row < 0)
    {
        stopListening();
        return;
    }
    m_listening = row;
    grabKeyboardFocus();
    repaint();
}

void SwooshbucklerAudioProcessorEditor::ShortcutsList::focusLost(FocusChangeType)
{
    stopListening();
}

void SwooshbucklerAudioProcessorEditor::ShortcutsList::bind(Shortcuts::Action action, const juce::KeyPress& key)
{
    m_owner.m_keys.set(action, key);
    stopListening();
    m_owner.refreshShortcuts();
    m_owner.toast(key.isValid() ? juce::String(Shortcuts::info(action).name) + " is now " + m_owner.m_keys.keyText(action)
                                : juce::String(Shortcuts::info(action).name) + " has no key");
    repaint();
}

bool SwooshbucklerAudioProcessorEditor::ShortcutsList::keyPressed(const juce::KeyPress& key)
{
    if (m_listening < 0)
        return false;
    const auto action = static_cast<Shortcuts::Action>(m_listening);
    if (key == juce::KeyPress::escapeKey)
    {
        stopListening();
        return true;
    }
    if (key == juce::KeyPress::backspaceKey || key == juce::KeyPress::deleteKey)
    {
        bind(action, {}); // the key is taken away
        return true;
    }
    if (! Shortcuts::isBindable(key))
        return true; // a key that cannot be a shortcut: the row keeps waiting
    const auto taken = m_owner.m_keys.clash(action, key);
    if (taken == Shortcuts::Action::Count)
    {
        bind(action, key);
        return true;
    }
    // Another action has that key already: ask before taking it off it.
    const auto text = Shortcuts::describe(key);
    juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                                     .withIconType(juce::MessageBoxIconType::QuestionIcon)
                                     .withTitle("Move the Key?")
                                     .withMessage(Theme::localiseShortcuts(text) + " is " + Shortcuts::info(taken).name
                                                  + " at the moment. Give it to " + Shortcuts::info(action).name + " instead?")
                                     .withButton("Move It")
                                     .withButton("Cancel"),
                                 [this, action, key](int result)
                                 {
                                     if (result == 1)
                                         bind(action, key);
                                     else
                                         stopListening();
                                 });
    return true;
}

void SwooshbucklerAudioProcessorEditor::ShortcutsList::resized() {}

void SwooshbucklerAudioProcessorEditor::ShortcutsList::paint(juce::Graphics& g)
{
    const auto& f = *m_owner.m_fonts;
    for (int i = 0; i < Shortcuts::numActions; ++i)
    {
        const auto action = static_cast<Shortcuts::Action>(i);
        auto row = rowBounds(i);
        const bool listening = i == m_listening;
        if (listening || i == m_over)
        {
            g.setColour(listening ? TC::brass.withAlpha(0.18f) : TC::hover);
            g.fillRect(row.reduced(0, 1));
        }
        auto plate = row.reduced(0, 2).removeFromRight(104);
        auto name = row.withTrimmedRight(110);
        g.setColour(listening ? TC::brass : TC::text);
        Theme::fitText(g, Shortcuts::info(action).name, name.withTrimmedLeft(6), f.caption(), juce::Justification::centredLeft);
        SwooshLookAndFeel::drawWell(g, plate.toFloat());
        const auto key = m_owner.m_keys.keyText(action);
        if (listening)
        {
            g.setColour(TC::brass);
            Theme::fitText(g, "Press a key", plate.reduced(4, 0), f.caption(), juce::Justification::centred);
        }
        else
        {
            Theme::stampText(g, key.isEmpty() ? juce::String("-") : key, plate.reduced(4, 0).toFloat(), f.caption(),
                             juce::Justification::centred, key.isEmpty() ? TC::textFaint : TC::text);
        }
    }
}

//==============================================================================
// Settings panel
namespace
{
    constexpr int c_rateValues[6] = { 0, 44100, 48000, 88200, 96000, 192000 };
    const char* c_rateNames[6] = { "Session", "44.1 kHz", "48 kHz", "88.2 kHz", "96 kHz", "192 kHz" };
}

SwooshbucklerAudioProcessorEditor::SettingsPanel::SettingsPanel(SwooshbucklerAudioProcessorEditor& owner)
    : m_owner(owner), m_keysList(owner)
{
    addAndMakeVisible(m_keysList);
    m_resetKeys.setColour(juce::TextButton::buttonOnColourId, TC::flight);
    m_resetKeys.setWantsKeyboardFocus(false);
    m_resetKeys.setTooltip("Reset To Defaults\nPuts every key back to the one it started with.");
    m_resetKeys.onClick = [this]
    {
        m_keysList.stopListening();
        m_owner.m_keys.resetToDefaults();
        m_owner.refreshShortcuts();
        m_keysList.repaint();
        m_owner.toast("The keys are back to their defaults");
    };
    addAndMakeVisible(m_resetKeys);

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
    const char* bitNames[] = { "16-Bit", "24-Bit", "32-Bit Float" };
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
    const char* chNames[] = { "Stereo", "Mono" };
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
        m_auto.setTooltip("Auto-Arm on Opening\nArms the tape every time Swooshbuckler opens, so you never miss the first good take.");
        m_choose.setTooltip("Choose\nChoose the folder recordings are saved to.");
        m_desktop.setTooltip("Desktop\nSave recordings to the default folder again.");
        m_reveal.setTooltip("Show\nShow the recordings folder in the Finder / Explorer.");
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

    m_perWhoosh.setTooltip("One File per Whoosh\nOn: the armed tape closes its file each time a whoosh has ended and opens the next one for the next "
                           "whoosh, so a session of riding leaves a folder of separate, numbered files. A whoosh has ended when the level "
                           "has sat about 25 dB under its peak for most of a second, or when a thrown whoosh has rung out with the rig stopped. "
                           "Sounds shorter than a fifth of a second are not kept, and the quiet between whooshes is always left out. "
                           "Off: one file for the whole take.");
    m_perWhoosh.setColour(juce::ToggleButton::tickColourId, TC::flight);
    m_perWhoosh.onClick = [this] { m_owner.m_processor.setOneFilePerWhoosh(m_perWhoosh.getToggleState()); repaint(); };
    addAndMakeVisible(m_perWhoosh);

    for (auto* editor : { &m_name, &m_creator })
    {
        editor->setFont(m_owner.m_fonts->body());
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
        b.setTooltip(juce::String(lengthNames[i]) + "\nHow much the Last button can save after the fact. The sound is held in memory all the time: about "
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
    m_panel = getLocalBounds().withSizeKeepingCentre(1060, 700);
    auto r = m_panel.reduced(28, 0);
    r.removeFromTop(56);

    // The keys have a column of their own at the right: the tape's settings keep the left one.
    {
        auto keys = r.removeFromRight(334);
        r.removeFromRight(26);
        m_keysCaption = keys.removeFromTop(20);
        m_keysHint = keys.removeFromTop(30);
        m_keysList.setBounds(keys.removeFromTop(ShortcutsList::idealHeight()));
        keys.removeFromTop(10);
        m_resetKeys.setBounds(keys.removeFromTop(28).removeFromLeft(160));
    }

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

    auto bottom = m_panel.reduced(28, 18).removeFromBottom(30).withTrimmedRight(360); // the keys' column is to the right
    m_close.setBounds(bottom.removeFromRight(100));
    m_audio.setBounds(bottom.removeFromLeft(150));
}

void SwooshbucklerAudioProcessorEditor::SettingsPanel::paint(juce::Graphics& g)
{
    g.fillAll(TC::window.withAlpha(0.8f));
    SwooshLookAndFeel::drawPanel(g, m_panel.toFloat(), "Settings", "The tape, the audio device and the keys", TC::brass,
                                 *m_owner.m_fonts, Icons::Icon::Settings);

    const auto& f = *m_owner.m_fonts;
    // The keys column.
    g.setColour(TC::textDim);
    Theme::fitText(g, "Shortcuts", m_keysCaption, f.caption(), juce::Justification::centredLeft);
    Theme::fitLines(g, "Click a row and press the key you want. Esc leaves it as it was, Backspace takes the key away.",
                    m_keysHint, f.caption(), TC::textFaint);
    const char* captions[] = { "Recordings Folder", "File Type", "Bit Depth", "Sample Rate", "Channels" };
    g.setColour(TC::textDim);
    for (int i = 0; i < 5; ++i)
        Theme::fitText(g, captions[i], m_captions[static_cast<size_t>(i)], f.caption(), juce::Justification::centredLeft);
    const char* moreCaptions[] = { "Take Name", "Creator", "File Names", "Keep the Last" };
    for (size_t i = 0; i < m_moreCaptions.size(); ++i)
        Theme::fitText(g, moreCaptions[i], m_moreCaptions[i], f.caption(), juce::Justification::centredLeft);
    g.setColour(TC::text);
    Theme::fitText(g, m_owner.m_processor.getRecordFolder().getFullPathName(),
                   m_captions[0].withY(m_captions[0].getBottom()).withHeight(22), f.body(), juce::Justification::centredLeft, Theme::Fit::Name);

    // What the sample rate means in practice
    auto& p = m_owner.m_processor;
    const juce::String last = p.getRetroMinutes() == 1 ? juce::String("Last 60 s saves the last minute in the same format.")
                                                       : "Last " + juce::String(p.getRetroMinutes()) + " Min saves the last " + juce::String(p.getRetroMinutes()) + " minutes in the same format.";
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
    Theme::fitLines(g, note, m_captions[5], f.body(), TC::textFaint);
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
    m_website.setTooltip("Website\nOpens sweetjusticesound.com in your browser.");
    m_website.onClick = [] { juce::URL("https://www.sweetjusticesound.com").launchInDefaultBrowser(); };
    m_copy.setTooltip("Copy Version\nCopies the version and build to the clipboard, for a bug report.");
    m_copy.onClick = [this]
    {
        juce::SystemClipboard::copyTextToClipboard(m_owner.versionLine() + ", built " + Version::builtOn()
                                                   + (Version::commit().isNotEmpty() ? " (" + Version::commit() + ")" : juce::String())
                                                   + ", " + juce::SystemStats::getOperatingSystemName());
        m_owner.toast("Version copied");
    };
    m_logo = std::make_unique<LogoOutline>(BinaryData::sjlogo_svg, static_cast<size_t>(BinaryData::sjlogo_svgSize));

    // Licences: the third-party notices (THIRD-PARTY-NOTICES.md, built in), in place of the About text.
    m_licences.setColour(juce::TextButton::buttonOnColourId, TC::flight);
    m_licences.setWantsKeyboardFocus(false);
    m_licences.setTooltip("Licences\nShows the notices and licences of what Swooshbuckler is built with: JUCE, the FLAC and Ogg Vorbis codecs, "
                          "the plug-in format SDKs and the two typefaces. Click again to go back.");
    m_licences.onClick = [this] { showNotices(! m_notices.isVisible()); };
    addAndMakeVisible(m_licences);
    m_notices.setMultiLine(true, true);
    m_notices.setReadOnly(true);
    m_notices.setScrollbarsShown(true);
    m_notices.setCaretVisible(false);
    m_notices.setFont(m_owner.m_fonts->body());
    // The file is Markdown: shown as plain text, without the heading marks.
    auto lines = juce::StringArray::fromLines(juce::String::fromUTF8(BinaryData::THIRDPARTYNOTICES_md, BinaryData::THIRDPARTYNOTICES_mdSize));
    for (auto& line : lines)
        if (line.startsWithChar('#'))
            line = line.trimCharactersAtStart("#").trimStart();
    m_notices.setText(lines.joinIntoString("\n"), false);
    addChildComponent(m_notices);
}

void SwooshbucklerAudioProcessorEditor::AboutPanel::showNotices(bool show)
{
    m_notices.setVisible(show);
    m_licences.setButtonText(show ? "About" : "Licences");
    if (show)
        m_notices.setCaretPosition(0);
    repaint();
}

void SwooshbucklerAudioProcessorEditor::AboutPanel::visibilityChanged()
{
    if (isVisible())
    {
        showNotices(false);
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
    bottom.removeFromLeft(8);
    m_licences.setBounds(bottom.removeFromLeft(100));
    r.removeFromBottom(14);
    m_notices.setBounds(r.withTrimmedTop(4));
    m_logoArea = r.removeFromLeft(190).withSizeKeepingCentre(190, 190).withY(r.getY() + 6);
    r.removeFromLeft(24);
    m_textArea = r;
}

void SwooshbucklerAudioProcessorEditor::AboutPanel::paint(juce::Graphics& g)
{
    const auto& f = *m_owner.m_fonts;
    g.fillAll(TC::window.withAlpha(0.8f));
    SwooshLookAndFeel::drawPanel(g, m_panel.toFloat(), "About", "What it is, who made it and which version this is", TC::brass, f);
    if (m_notices.isVisible())
        return; // the licences cover the rest

    // The Sweet Justice logo, traced by its comets.
    if (m_logo != nullptr && m_logo->isLoaded())
    {
        SwooshLookAndFeel::drawWell(g, m_logoArea.toFloat(), Theme::Cut::panel);
        const float t = static_cast<float>(juce::Time::getMillisecondCounterHiRes() * 0.001 - m_shownAt);
        m_logo->renderPreview(g, m_logoArea.toFloat().reduced(18.0f), t, 1.0f);
    }

    auto r = m_textArea;
    SwooshLookAndFeel::drawCutlasses(g, juce::Rectangle<float>(static_cast<float>(r.getX()), static_cast<float>(r.getY()) + 8.0f, 34.0f, 30.0f), TC::master, TC::brass);
    g.setColour(TC::text);
    Theme::fitText(g, "Swooshbuckler", r.removeFromTop(46).withTrimmedLeft(44), f.wordmark(), juce::Justification::centredLeft);

    auto line = [&](const juce::String& text, const juce::Font& font, juce::Colour colour, int height)
    {
        Theme::fitLines(g, text, r.removeFromTop(height), font, colour);
    };
    // The format, when there is one to name: offline tools that make the editor without a wrapper say nothing.
    const auto wrapper = m_owner.m_processor.wrapperType;
    line("Version " + Version::number()
             + (wrapper == juce::AudioProcessor::wrapperType_Undefined
                    ? juce::String() : juce::String("   ") + juce::AudioProcessor::getWrapperTypeDescription(wrapper)),
         f.title(), TC::brass, 22);
    line("Built " + Version::builtOn() + (Version::commit().isNotEmpty() ? "   commit " + Version::commit() : juce::String()),
         f.body(), TC::textDim, 20);
    r.removeFromTop(8);
    line("A whoosh-making rig. Drop in up to ten sounds and it rides the faders across them, making doppler passes, "
         "swells, hits and tails. Shape them with two effect buses, a hit gate, a spectral morph and four modulators, "
         "and arm the tape to keep every one.", f.body(), TC::text, 54);
    r.removeFromTop(6);
    line("Made by Samuel Justice at Sweet Justice Sound.", f.title(), TC::text, 22);
    r.removeFromTop(4);
    line("Fonts: Pirata One by Rodrigo Fuenzalida and Nicolas Massi; IBM Plex Sans Condensed by IBM. "
         "Both under the SIL Open Font License 1.1. Built with JUCE " + Version::juce() + ". "
         "VST is a registered trademark of Steinberg Media Technologies GmbH.", f.body(), TC::textFaint, 50);
    // The build year, so the notice never needs editing.
    line(juce::String(juce::CharPointer_UTF8("\xc2\xa9 ")) + Version::year() + " Samuel Justice. MIT License.",
         f.body(), TC::textFaint, 18);
}

//==============================================================================
//==============================================================================
/** The ride of the last 8 seconds: where the ride has each bus's faders (the mean of its loaded tracks), newest at
    the right, bus A in coral and bus B in turquoise. The window's timer steps it thirty times a second (step);
    it repaints only itself. */
class SwooshbucklerAudioProcessorEditor::RideStrip : public juce::Component, public juce::SettableTooltipClient
{
public:
    RideStrip(SwooshbucklerAudioProcessor& p, const Theme::Fonts& f) : m_processor(p), m_fonts(f)
    {
        for (auto& line : m_history)
            line.fill(0.0f);
    }

    void paint(juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat();
        SwooshLookAndFeel::drawWell(g, r, Theme::Cut::small);
        const float labelWidth = 52.0f;
        g.setColour(TC::textFaint);
        Theme::fitText(g, "Last 8 s", r.withWidth(labelWidth).withTrimmedLeft(6.0f), m_fonts.caption(), juce::Justification::centredLeft);
        const auto area = r.withTrimmedLeft(labelWidth).reduced(4.0f, 3.0f);
        g.setColour(TC::grid);
        g.fillRect(juce::Rectangle<float>(area.getX(), area.getBottom() - 1.0f, area.getWidth(), 1.0f));
        const juce::Colour colours[2] = { TC::busA, TC::busB };
        for (size_t bus = 0; bus < 2; ++bus)
        {
            if (! m_used[bus])
                continue;
            auto& p = m_path; // kept between frames, so drawing it allocates nothing
            p.clear();
            for (int k = 0; k < c_points; ++k)
            {
                const float v = juce::jlimit(0.0f, 1.0f, m_history[bus][static_cast<size_t>((m_head + k) % c_points)]);
                const float x = area.getX() + area.getWidth() * static_cast<float>(k) / static_cast<float>(c_points - 1);
                const float y = area.getBottom() - 1.0f - (area.getHeight() - 2.0f) * v;
                if (k == 0) p.startNewSubPath(x, y); else p.lineTo(x, y);
            }
            g.setColour(colours[bus].withAlpha(0.9f));
            g.strokePath(p, juce::PathStrokeType(1.4f, juce::PathStrokeType::mitered, juce::PathStrokeType::butt));
        }
    }

    /** Takes the rides as they are now: one more point. */
    void step()
    {
        const auto& engine = m_processor.getEngine();
        const auto& hold = m_processor.getCargo();
        float sum[2] = { 0.0f, 0.0f };
        int count[2] = { 0, 0 };
        for (int t = 0; t < CargoHold::c_numTracks; ++t)
        {
            const int bus = hold.getBus(t);
            if (bus < 0 || bus > 1 || hold.get(t) == nullptr)
                continue;
            sum[bus] += engine.getRide(t);
            ++count[bus];
        }
        for (size_t bus = 0; bus < 2; ++bus)
        {
            m_used[bus] = count[bus] > 0;
            m_history[bus][static_cast<size_t>(m_head)] = count[bus] > 0 ? sum[bus] / static_cast<float>(count[bus]) : 0.0f;
        }
        m_head = (m_head + 1) % c_points;
        if (Theme::isShownInWindow(*this))
            repaint();
    }

private:
    static constexpr int c_points = 240; // 8 s at 30 Hz
    SwooshbucklerAudioProcessor& m_processor;
    const Theme::Fonts& m_fonts;
    std::array<std::array<float, c_points>, 2> m_history;
    std::array<bool, 2> m_used { false, false };
    int m_head = 0;
    juce::Path m_path;
};

namespace
{
    // The window's grid, in design pixels: one gutter between panels and around the window, rows from the top.
    namespace Lay
    {
        constexpr int gutter = Theme::Space::gutter;
        constexpr int left = gutter, right = SwooshbucklerAudioProcessorEditor::c_width - gutter;
        constexpr int headerRule = 50;                       // the rope under the header
        constexpr int topY = 58, topH = 360;                 // the tracks and the sea
        constexpr int seaW = 648;                            // 1.8 : 1
        constexpr int row2Y = topY + topH + gutter, rowH = 156; // Ride, Bus A, Bus B
        constexpr int row3Y = row2Y + rowH + gutter;         // Hits, Output
        constexpr int modY = row3Y + rowH + gutter, modH = 128; // Stowaway, Modulation
        constexpr int modCardsY = 34, modCardsH = 88;        // Modulation: its title bar, then the four modulators
        constexpr int footerY = modY + modH + 6, footerH = 18;
        // Knobs: one column pitch for bus A, bus B and the Hits; a panel's knobs start a pad in from its edge.
        constexpr int pitch = 64, pad = 12;
        constexpr int rideW = 400, busAW = 7 * pitch + 2 * pad, busBW = 9 * pitch + 2 * pad;
        constexpr int busAX = left + rideW + gutter, busBX = busAX + busAW + gutter;
        constexpr int knobsY = 42;                           // from a panel's top to its knob cells
        constexpr int stripY = 134, stripH = 16;             // the scope strip along a panel's bottom
        // Stowaway's plate, at the left of the bottom row: folded it is this wide (or as wide as its nameplate and
        // its chevron need), open it holds five wheels on the shared pitch. Modulation has the rest of the row.
        constexpr int stowFoldedW = 176, stowPad = 16, stowOpenW = 5 * pitch + 2 * stowPad;
        constexpr int stowKnobsY = 35, stowStripY = 108, stowStripH = 14;
        constexpr int stowSwitchW = 54;                      // its switch, and the word (On or Off) beside it
        static_assert(busBX + busBW == right, "row 2 fills the window's width");
        static_assert(footerY + footerH <= SwooshbucklerAudioProcessorEditor::c_height - 4, "the rows fit the window's height");
    }
}

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
    , m_variations(p, *m_fonts)
    , m_about(*this)
{
    setLookAndFeel(&m_lnf);
    juce::LookAndFeel::setDefaultLookAndFeel(&m_lnf); // popup menus, alerts and file choosers match too
    m_tooltips.setLookAndFeel(&m_lnf);
    setWantsKeyboardFocus(true);

    // The keys as the user has them, and the one clock the moving parts run on.
    m_keys.attach(p.getSettingsFile());
    m_swell.listenTo(&m_clock);
    m_tape.listenTo(&m_clock);
    m_wires.listenTo(&m_clock);

    addAndMakeVisible(m_content);
    m_content.setBounds(0, 0, c_width, c_height);
    m_content.onPaint = [this](juce::Graphics& g) { paintContent(g); };
    m_content.onMouseDown = [this](const juce::MouseEvent& e)
    {
        if (m_titleArea.contains(e.getPosition()))
        {
            m_about.setVisible(true);
            m_about.toFront(false);
        }
        else if (m_stowNameplate.contains(e.getPosition()))
            setStowawayOpen(m_processor.getStowawayPanel() != 1); // the nameplate folds and opens the plate, like its chevron
    };

    // Header ------------------------------------------------------------------
    for (auto* b : { &m_presetName, &m_dice, &m_retroButton, &m_settingsButton, &m_playButton, &m_newButton, &m_takesButton,
                     &m_variationsButton, &m_abButton })
    {
        b->setColour(juce::TextButton::buttonOnColourId, TC::brass);
        b->setWantsKeyboardFocus(false);
        m_content.addAndMakeVisible(*b);
    }
    // One look for each kind of thing: Play is the one filled button, the preset box is a menu, A | B is a chip,
    // and the buttons that are best known by their picture carry it.
    SwooshLookAndFeel::setStyle(m_playButton, SwooshLookAndFeel::ButtonStyle::Primary);
    Icons::set(m_playButton, Icons::Icon::Play);
    SwooshLookAndFeel::setStyle(m_presetName, SwooshLookAndFeel::ButtonStyle::Menu);
    SwooshLookAndFeel::setShowsName(m_presetName);
    m_abButton.getProperties().set("plain", true); // its letter says which set: no lamp
    Icons::set(m_dice, Icons::Icon::Die);
    Icons::set(m_settingsButton, Icons::Icon::Settings);
    Icons::set(m_newButton, Icons::Icon::New);
    m_presetName.setButtonText(p.currentPresetName);
    m_presetName.setTooltip("Presets\nClick for the list: the factory presets, then your own. Picking one glides every control to its new setting over a moment, "
                            "so you can switch while it plays. A preset is a complete look: every knob it doesn't set goes back to its default. "
                            "Your tracks, their levels, the output section and the modulators are left alone (a preset of your own can carry its "
                            "modulators and wires, if you saved it with them). The list also saves, renames and deletes your presets, switches "
                            "A / B and saves and opens sessions. The arrows either side step through the presets.");
    m_presetName.onClick = [this] { showPresetMenu(); };
    m_presetPrev.setTooltip("Previous Preset\nLoads the preset before this one in the list.");
    m_presetNext.setTooltip("Next Preset\nLoads the preset after this one in the list.");
    m_presetPrev.onClick = [this] { stepPreset(-1); };
    m_presetNext.onClick = [this] { stepPreset(1); };
    m_dice.setTooltip("Randomise\nRolls a random but musical setting for the ride, the buses, the hits and the space, and glides to it. "
                      "Only the knobs it rolls move: every other control stays where you set it, and so do your tracks, their levels and the output. "
                      "A section with its lock on is left out. Keep rolling until you hear something you like; the arrows beside it go back "
                      "through the last 20 rolls, and the star keeps one as a preset.");
    m_dice.onClick = [this] { rollDice(-1); };

    for (auto* b : { &m_undoButton, &m_redoButton, &m_rollBack, &m_rollForward, &m_keepRoll, &m_presetPrev, &m_presetNext })
        m_content.addAndMakeVisible(*b);
    m_undoButton.setTooltip("Undo\nTakes back the last thing you changed: a knob move, a preset, a Randomise roll, a section's die, A / B, New Session "
                            "or a track edit, in the order they happened. A preset or a roll is one step, however many knobs it moved.");
    m_undoButton.onClick = [this] { stepHistory(false); };
    m_redoButton.setTooltip("Redo\nDoes again what Undo took back.");
    m_redoButton.onClick = [this] { stepHistory(true); };
    m_rollBack.setTooltip("Previous Roll\nGoes back through the last 20 Randomise rolls (the header's and the section dice), and at the end to the "
                          "settings you had before the first of them. Each one comes back as you left it.");
    m_rollBack.onClick = [this] { stepRoll(-1); };
    m_rollForward.setTooltip("Next Roll\nGoes forward again through the rolls you stepped back over.");
    m_rollForward.onClick = [this] { stepRoll(1); };
    m_keepRoll.setTooltip("Keep This Roll\nSaves the settings as they are as a preset of your own, called Roll, Roll 2 and so on. "
                          "It joins your presets in the preset list, where you can rename it.");
    m_keepRoll.onClick = [this] { keepRoll(); };
    m_abButton.setTooltip("A / B\nTwo sets of the settings a preset holds, to compare two versions of a sound. Click to go to the other set; "
                          "each waits as you left it. The other set starts as a copy of this one. Copy to Other, in the preset list, "
                          "copies the settings you have now across. Both sets are saved with the session.");
    m_abButton.onClick = [this]
    {
        m_morph.finish();
        m_processor.switchAB();
        toast(m_processor.getABSlot() == 0 ? "Set A" : "Set B");
    };

    // A die and a lock for each section, in its title bar.
    {
        const juce::Colour colours[Presets::numSections] = { TC::brass, TC::busA, TC::busB, TC::hits, TC::output, TC::stowaway };
        const char* rolls[Presets::numSections] = {
            "Energy, Rate, Fill, Chaos and Pitch",
            "bus A's Doppler, Width, Bend, Low End, Swirl, Filter and Drive",
            "bus B's Doppler, Width, Bend, Low End, Swirl, Filter, Drive, Ducked and Filtered",
            "Sensitivity, Boost, Release and Duck",
            "Space, and gently Inflate and Push",
            "Stowaway's Follow, Low End, Filter, Drive and Width, never its Mode or its switch",
        };
        static_assert(Presets::Stowaway == Presets::numSections - 1, "the die and lock of each section, in Presets' order");
        for (int section = 0; section < Presets::numSections; ++section)
        {
            const juce::String name = Presets::sectionName(section);
            auto& die = m_sectionDice[static_cast<size_t>(section)];
            die = std::make_unique<IconButton>(IconButton::Icon::Die, colours[section], true);
            die->setTooltip("Roll " + name + "\nRolls just this section: " + rolls[section] + ". Everything else stays where it is. "
                            "The roll joins the Randomise history, so the arrows by Randomise step back over it.");
            die->onClick = [this, section] { rollDice(section); };
            m_content.addAndMakeVisible(*die);
            auto& lock = m_sectionLocks[static_cast<size_t>(section)];
            lock = std::make_unique<IconButton>(IconButton::Icon::Lock, colours[section], true);
            lock->setClickingTogglesState(true);
            lock->setToggleState(m_processor.isSectionLocked(section), juce::dontSendNotification);
            lock->setTooltip("Lock " + name + "\nOn: the header's Randomise (and Randomise Everything in Render Variations) leaves this section "
                             "as it is. The section's own die still rolls it. The locks are saved with the session.");
            lock->onClick = [this, section] { m_processor.setSectionLocked(section, m_sectionLocks[static_cast<size_t>(section)]->getToggleState()); };
            m_content.addAndMakeVisible(*lock);
        }
    }
    m_content.addAndMakeVisible(m_tape);
    m_tape.setTooltip("Tape\nArm it and it records whenever there is sound, skipping the quiet stretches, into one long file of usable whooshes. Click again to stop and close the file. Pass peaks and hits are marked in WAV files, so a take arrives ready to spot. Where it saves, in which format, Skip Silence (off for one continuous take) and One File per Whoosh (a separate file for each) are in Settings; what it has saved is in Takes.");
    m_tape.onClick = [this] { toggleLog(); };
    m_retroButton.setTooltip("Last 60 Seconds\nThe rig always remembers the last minute. Click to save it to a file, even if the tape wasn't armed: for the happy accident you didn't record. Keep the Last in Settings makes it 3 or 5 minutes.");
    m_takesButton.setTooltip("Takes\nThe recordings of this run, newest first, with how many there are: tape takes, whoosh files and Last 60 s saves. "
                             "Open it to hear them, drag them out to the Finder / Explorer or into your DAW, keep or delete them.");
    m_takesButton.onClick = [this] { showTakes(); };
    m_variationsButton.setTooltip("Render Variations\nRenders a batch of whooshes straight to files, in the background and faster than real time: "
                                  "choose how many (100 unless you say otherwise), how long, and what changes from one to the next. "
                                  "While a batch renders, this button shows how far it is.");
    m_variationsButton.onClick = [this] { showVariations(); };
    m_playButton.setTooltip("Play / Stop\nStarts and stops the whole rig. Stop lets the tail ring out. The app always opens stopped, so nothing blasts out when you load a session. In a DAW it can be automated, and with Follow Host on the transport starts and stops it too.");
    m_playButton.onClick = [this] { m_processor.setPlaying(! m_processor.isPlaying()); };
    m_newButton.setTooltip("New Session\nStops, empties every track and puts every control back to its default. Undo brings all of it back in one step: the tracks, the settings and the wires.");
    m_newButton.onClick = [this]
    {
        m_morph.finish();
        m_processor.newSession();
        const auto undoKey = m_keys.keyText(Shortcuts::Action::Undo);
        toast(undoKey.isEmpty() ? juce::String("New Session. Undo brings everything back.")
                                : "New Session. " + undoKey + " brings everything back.");
    };
    m_retroButton.onClick = [this] { saveRetro(); };
    m_settingsButton.setTooltip("Settings\nWhere recordings go, their file type, bit depth, sample rate and channels, auto-arming the tape, and (in the app) the audio device.");
    m_settingsButton.onClick = [this] { m_settings.refresh(); m_settings.setVisible(true); m_settings.toFront(false); };

    // Tracks + output view + ride --------------------------------------------
    m_content.addAndMakeVisible(m_hold);
    m_hold.onHistory = [this](bool redo) { stepHistory(redo); };
    m_hold.onSwell = [this] { m_processor.setSwell(true); juce::Timer::callAfterDelay(250, [this] { if (! m_spaceHeld) m_processor.setSwell(false); }); };
    m_hold.onStatus = [this](const juce::String& s) { toast(s); };
    m_hold.onTakeNoteClicked = [this] { showTakes(); };
    m_morphPad.laneColourFor = [this](int track) { return m_hold.laneColour(track); };
    m_content.addAndMakeVisible(m_sea);
    m_sea.onClick = [this] { m_processor.setSwell(true); juce::Timer::callAfterDelay(400, [this] { if (! m_spaceHeld) m_processor.setSwell(false); }); };
    m_sea.setTooltip("The Sea\nWhat the rig sends out: the sound rolls away as waves, and each doppler pass is a comet across the sky. Click it (or press space) to swell every track up.");

    // Morph: the pad sits over the output view (the waves show through) while it's switched on.
    m_content.addChildComponent(m_morphPad);
    // Each side of the morph is its bus until a rope is dragged from its handle onto a track.
    m_morphPad.setBusLabels("Bus A", TC::movement, "Bus B", TC::sources);
    m_morphPad.setRopeOverlayParent(&m_content);
    m_morphPad.laneAtScreenPosition = [this](juce::Point<int> screenPos) { return m_hold.trackAtScreenPosition(screenPos); };
    m_morphPad.laneAnchor = [this](int track, juce::Point<int>& screenPos) { return m_hold.laneAnchorScreen(track, screenPos); };
    m_morphPad.onSourceAssigned = [this](int side, int track)
    {
        m_processor.setMorphSource(side, track);
        const juce::String name = side == 0 ? "A" : "B";
        toast(track >= 0 ? "Morph side " + name + " is the track \"" + m_processor.getCargo().getDisplayName(track) + "\"."
                         : "Morph side " + name + " is bus " + name + " again.");
        syncMorphPad();
    };
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
    m_morphButton.getProperties().set("plain", true); // its text says the state: no lamp
    m_morphButton.setTooltip("Morph\nShows the morph pad over the sea. It blends bus A into bus B spectrally, not just a crossfade: the two sounds trade their tone and pitch. To morph two single sounds, drag the rope from the A or B handle under the pad onto a track. Switch it on with the On / Off button on the pad, then drag the dot or press its Play button to let it wander.");
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
                         "The gate isn't needed, so Gate, Release, Hold, Floor and Sync Hits rest.\n"
                         "Off: the hit gate fires them (and Sync Hits lines them up, if it's on).");
    m_content.addAndMakeVisible(m_aimHits);
    m_aimAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(p.getState(), ParamIDs::hitAim, m_aimHits);

    m_rideStrip = std::make_unique<RideStrip>(p, *m_fonts);
    m_rideStrip->setTooltip("Ride, the Last 8 Seconds\nWhere the ride has had the faders: bus A's tracks in coral, bus B's in turquoise, the newest "
                            "moment at the right. A swell is a hump; Energy sets how deep the dips between them go and Rate how long they are.");
    m_content.addAndMakeVisible(*m_rideStrip);
    m_content.addAndMakeVisible(m_swell);
    m_swell.setTooltip("Swell\nHold it to push every fader up at once, for a big whoosh on demand. Held for longer than a couple of seconds, the mix eases back to its usual level with every fader still up. Any MIDI note does the same while Keys is set to Swell.");
    m_rideButton.setClickingTogglesState(true);
    m_rideButton.setColour(juce::TextButton::buttonOnColourId, TC::flight);
    m_rideButton.setTooltip("Auto Ride\nThe heart of the rig: every track's fader moves by itself, each at its own speed, so overlapping swells make whooshes for you. Energy, Rate, Fill, Chaos and Pitch shape how it moves. Off: the faders stay where you set them, the doppler passes do the moving, and the mix is eased down to about the level it has with the ride on.");
    m_rideButton.setWantsKeyboardFocus(false);
    m_content.addAndMakeVisible(m_rideButton);
    m_rideAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(p.getState(), ParamIDs::rideOn, m_rideButton);

    m_followMasterButton.setClickingTogglesState(true);
    m_followMasterButton.setColour(juce::TextButton::buttonOnColourId, TC::flight);
    m_followMasterButton.setTooltip("Follow Master\nOn: every track's ride swells with the Master track's loudness instead of its own slow wave, so all the layers "
                                    "move like that one sound. The Master itself plays as it is. Energy still sets how deep the faders dip. "
                                    "It needs Auto Ride on and a Master track (the star on a lane); until then it waits.");
    m_followMasterButton.setWantsKeyboardFocus(false);
    m_content.addAndMakeVisible(m_followMasterButton);
    m_followMasterAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(p.getState(), ParamIDs::rideFollowMaster, m_followMasterButton);

    m_throwButton.setColour(juce::TextButton::buttonColourId, TC::brass.withAlpha(0.25f));
    m_throwButton.setWantsKeyboardFocus(false);
    m_throwButton.setTooltip("Throw\nThrows one whoosh now: a single pass on both buses, with a long approach, the peak as it goes by and a shorter tail. "
                             "With Aim on, the hits land on its peak. While the rig plays it goes on top of the ride; with the rig stopped you get "
                             "just that one whoosh and then silence, so single whooshes can be made one at a time. The peak is heard half a Throw "
                             "Length after you throw, and the Ride panel counts down to it. A MIDI note does the same with Keys set to Throw.");
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
                    "time from pressing Throw to the peak. Fully left (Rate): a throw is as long as the ride's own passes, set by Rate.");

    // Buses: one set of controls per bus, shown one bus at a time (tabs) --------
    {
        const juce::Colour busColours[ParamIDs::numBuses] = { TC::movement, TC::sources };
        const char* names[] = { "Doppler", "Width", "Bend", "Low End", "Swirl", "Filter", "Drive", "Ducked", "Filtered" };
        const char* tips[] = {
            "How dramatic each pass of the moving source is: how close it comes, how much it swells and brightens on the way by. Speed, distance and direction change by themselves with every pass.",
            "How far each pass travels across the stereo field. 0: in the middle. High: a full side-to-side fly-by, and the ear turned away from the pass hears it a little duller, as a head in the way would make it. With Link on, bus B uses bus A's Width.",
            "How much the pitch bends as a pass goes by (the doppler bend), together with Doppler: it rises on the way in and drops as the pass goes by. It changes the pitch only, not how the pass swells. Low: passes go by without the bend. High: a big race-car scream and drop.",
            "Weight: a sub octave, low overtones, and an extra low lift at the loudest moments. With the faders still (Auto Ride off, or Swell held) the lift follows each pass: up as it comes close, down as it leaves.",
            "Movement inside the sound: a phaser, then (from about a third of the way) a flanger that sweeps along with each pass. The level stays put; only the colour moves. High settings narrow and darken a bed. With the faders still (Auto Ride off, or Swell held) the passes alone drive the sweep.",
            "A filter that opens as each pass swells and closes in its tail, so the tails are dark and the peaks bright, like air rushing past. More: more of it, closing further at the peak, and over the top 40% a resonant edge. With the faders still (Auto Ride off, or Swell held) it follows how close each pass is.",
            "Saturation: grit and density, from a gentle thickening to a hard, flattened growl. The level is kept roughly steady, so it adds character rather than just loudness.",
            "How much bus A pushes this bus down at A's peaks (side-chain ducking, up to -20 dB in Volume mode), so the two buses don't pile up. Duck sets how (Volume, Darken or Mids). With Take Turns on, also how far the quieter bus goes down: at 0, Take Turns does nothing.",
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

    // Stowaway: the extra bus. Its switch, its Mode, the chevron that opens and folds its plate, five wheels that
    // keep their names in every mode, and its scope. What each wheel does (and whether it rests) follows the
    // switch and the Mode: see showStowaway.
    {
        m_stowOn.setColour(juce::ToggleButton::tickColourId, TC::stowaway);
        m_stowOn.setWantsKeyboardFocus(false);
        m_stowOn.setTitle("Stowaway On / Off");
        m_stowOn.setTooltip("Stowaway On / Off\nSwitches the extra bus on or off. Off (how a new session starts): Stowaway is silent and does no work, "
                            "and a track sent to it waits unplayed, with a hollow S on its tag. On: it fades in, and off it fades out, so "
                            "neither clicks. Sending a track to Stowaway from its lane's bus tag switches it on for you. Rumble sounds on Auto "
                            "play here only while it is on and its Mode is Sub; otherwise they play on Bus A. Presets and Randomise never "
                            "switch it off.");
        m_content.addAndMakeVisible(m_stowOn);
        m_stowOnAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(p.getState(), ParamIDs::stowOn, m_stowOn);

        m_stowMode.addItemList(juce::StringArray(ParamIDs::stowModeNames, ParamIDs::numStowModes), 1);
        m_stowMode.setWantsKeyboardFocus(false);
        m_stowMode.setTooltip("Stowaway Mode\nWhat the extra bus is. Sub: only the low end of its tracks, in the middle, swelling with Bus A. "
                              "Wake: a trail that opens behind each of Bus A's passes. Fly: a third mover, with doppler passes of its own. "
                              "Dry: riding only, past Space and straight to the output. The five knobs keep their names in every mode, and "
                              "one that does nothing in a mode rests. Changing the Mode never clicks. Randomise leaves it alone.");
        m_content.addAndMakeVisible(m_stowMode);
        m_stowModeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(p.getState(), ParamIDs::stowMode, m_stowMode);

        m_stowFold.onClick = [this] { setStowawayOpen(m_processor.getStowawayPanel() != 1); };
        m_content.addAndMakeVisible(m_stowFold);

        for (int k = 0; k < ParamIDs::numStowControls; ++k)
            addKnob(ParamIDs::stow[k], ParamIDs::stowNames[k], TC::stowaway, stowawayKnobTip(k, p.getStowawayMode(), p.isStowawayOn()))
                .setSize(WheelKnob::Size::Secondary);

        m_stowScope = std::make_unique<SectionScope>(p.getEngine().getScope(RigEngine::SectionStow), TC::stowaway);
        m_stowScope->setTooltip("Stowaway, the Last 3 Seconds\nWhat Stowaway sends on, after its Mode has done its work. The newest sound is at the right.");
        m_content.addAndMakeVisible(*m_stowScope);

        // A session that comes back with Stowaway on and its plate never opened opens it; otherwise as it was left.
        if (p.isStowawayOn() && p.getStowawayPanel() < 0)
            p.setStowawayPanel(1);
        m_stowWasOn = p.isStowawayOn();
        m_stowOpen.snap(p.getStowawayPanel() == 1 ? 1.0f : 0.0f);
    }

    // Hits + output --------------------------------------------------------------
    addKnob(ParamIDs::hitSense, "Sensitivity", TC::record, "How easily the hits fire. With Aim on: which pass peaks get a hit (closer passes on a louder bus score higher). With Aim off: how easily the hit gate opens, by the Gate mode. Low: only the biggest moments fire the hits. High: they fire often.");
    addKnob(ParamIDs::hitBoost, "Boost", TC::record, "How loud the hit tracks come in when they fire, from 3 dB under their own level to 12 dB over it. More makes the hits punch out of the whoosh (the top end leans on the limiter).");
    addKnob(ParamIDs::hitRelease, "Release", TC::record, "How long the hits tail out after the gate opens. Short: tight, dry impacts. Long: the hits ring on into the next swell.");
    addKnob(ParamIDs::hitHold, "Hold", TC::record, "How long the gate stays open once it opens. Longer stops it chattering open and shut, so each hit reads as one event.");
    addKnob(ParamIDs::hitFloor, "Floor", TC::record, "How far the gate closes between hits. Closed (fully left): the hits vanish completely between peaks. Turn it up and they stay under the mix instead, from -22 dB to just under their full level.");
    addKnob(ParamIDs::hitDuck, "Duck", TC::record, "How far buses A and B get out of the way when the hits fire (up to -12 dB at the hits' peaks), so the impact punches through the wall of whoosh. Duck sets how they get out of the way.");
    addKnob(ParamIDs::duckReturn, "Return", TC::record, "How fast ducked buses come back up afterwards, for the hits' duck and for bus B ducked by A. Fast (fully left) follows straight away. Slower is smoother; at the top it pumps.");

    // Gate and duck modes: the first entry of each is the default.
    m_gateMode.addItemList({ "Peaks", "Level", "Jumps", "Open" }, 1);
    m_gateMode.setTooltip("Gate Mode\nWhat opens the hit gate. Peaks: only at the loudest moments of buses A and B together. Level: whenever the mix is louder than a level, set by Sensitivity. Jumps: on sudden rises, however loud or quiet. Open: no gate, the hits always play.");
    m_duckMode.addItemList({ "Volume", "Darken", "Mids" }, 1);
    m_duckMode.setTooltip("Duck Mode\nHow a ducked bus gets out of the way, both when the hits duck A and B and when A ducks B. Volume: it is turned down. Darken: it is low-passed instead, so it stays but gets out of the way. Mids: only the crowded middle (250 Hz to 4 kHz) goes down, so the sub and air stay.");
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
    m_takeTurns.setTooltip("Take Turns\nOn: only the louder of bus A and bus B speaks at a time and the other ducks under it (by as much as Ducked), so they trade places instead of piling up. Off: A always ducks B. Good for taming the everything-at-once wall of sound.");
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
                                     "The passes peak at the same moments either way, so Aim and Link work the same.")
                        + (b == 1 ? "\nWhile Link is on, bus B uses bus A's Pass Mode." : ""));
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

    // Peak Tremolo: a tinted group at the left of Output (before Space in the signal): its switch and its wave and
    // Shakes menus over its Depth, Rate and Threshold knobs.
    addKnob(ParamIDs::tremDepth, "Depth", TC::movement, "Tremolo Depth\nHow far the level dips at the very peak (the readout), up to a full chop at 100%. It only shakes the loudest moments: past Threshold it eases in, "
            "deepest at the peak, and lets go as the level falls, so the dynamics flutter where the whoosh is biggest.");
    m_tremOn.setColour(juce::ToggleButton::tickColourId, TC::busA);
    m_tremOn.setWantsKeyboardFocus(false);
    m_tremOn.setTooltip("Peak Tremolo\nOn: a tremolo that shakes only the loudest moments. As the level climbs past Threshold towards its recent "
                        "peak, the tremolo deepens (up to Depth, at Rate, in the chosen wave) and it lets go as the level falls, so the "
                        "dynamics flutter where the whoosh is biggest. Off (the default): no tremolo. Switching fades it in or out.");
    m_content.addAndMakeVisible(m_tremOn);
    m_tremOnAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(p.getState(), ParamIDs::tremOn, m_tremOn);
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
                      "The tape and Last 60 s record before it, at full level, so turning it down never makes a quieter take.");
    m_trim.setTypedParameter(p.getState().getParameter(ParamIDs::masterTrim), *m_fonts);
    m_trim.onValueChange = [this]
    {
        m_trimReadout = juce::String(m_trim.getValue(), 1);
        m_content.repaint(m_trimValue);
    };
    m_trim.minTypingWidth = 54; // the fader is narrower than "-12.0 dB"
    updateMeterReadouts();
    m_content.addAndMakeVisible(m_trim);
    m_trimAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(p.getState(), ParamIDs::masterTrim, m_trim);
    m_trimReadout = juce::String(m_trim.getValue(), 1);

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
    m_hold.onLanesMoved = [this] { m_wires.rebuildSoon(); m_morphPad.ropesMoved(); }; // scrolling the tracks moves the plugs on their pitch boxes, and the morph's ropes
    m_wires.onStatus = [this](const juce::String& text) { toast(text); };
    m_wires.isSourceBipolar = [&p](int source)
    {
        return p.getState().getRawParameterValue(ParamIDs::modType[source])->load() < 1.5f; // LFO, Random
    };
    p.getMods().onChange = [safe = juce::Component::SafePointer<ModWires>(&m_wires)]
    {
        if (safe != nullptr) safe->rebuild();
    };

    // Over the panels and the sea, and under the overlays, which dim everything behind them.
    m_content.addAndMakeVisible(m_toastPlate);
    m_content.addChildComponent(m_settings);
    m_content.addChildComponent(m_takes);
    m_content.addChildComponent(m_variations);
    m_variations.onOpenTakes = [this] { showTakes(); };
    m_variations.onKeyboardDone = [this] { grabKeyboardFocus(); };
    m_takes.onStatus = [this](const juce::String& text) { toast(text); };
    m_content.addChildComponent(m_about);

    // Version: next to the website, and the title itself, both open About.
    m_versionLink.setButtonText(versionLine());
    m_versionLink.setURL({});
    m_versionLink.setFont(m_fonts->caption(), false, juce::Justification::centredLeft);
    m_versionLink.setColour(juce::HyperlinkButton::textColourId, TC::textFaint);
    m_versionLink.setTooltip("About Swooshbuckler\nThe version, the build and the credits.");
    m_versionLink.onClick = [this] { m_about.setVisible(true); m_about.toFront(false); };
    m_content.addAndMakeVisible(m_versionLink);

    m_content.addAndMakeVisible(m_websiteLink);
    m_websiteLink.setFont(m_fonts->caption(), false, juce::Justification::centredLeft);
    m_websiteLink.setColour(juce::HyperlinkButton::textColourId, TC::textFaint); // demoted: the rig is the thing, not the address
    m_websiteLink.setTooltip("Website\nOpens sweetjusticesound.com in your browser.");

    setResizable(true, true);
    const int minWidth = juce::roundToInt(c_width * c_minScale), maxWidth = juce::roundToInt(c_width * c_maxScale);
    setResizeLimits(minWidth, minWidth * c_height / c_width, maxWidth, maxWidth * c_height / c_width);
    if (auto* c = getConstrainer())
        c->setFixedAspectRatio(static_cast<double>(c_width) / c_height);

    layoutContent();
    showStowaway(0.0f);
    const int saved = p.savedEditorWidth;
    // The first time, the design size or the largest that is whole on the screen; after that, the size it was left at.
    const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
    const int width = saved > 0 ? juce::jlimit(minWidth, maxWidth, saved)
                                : firstOpenWidth(display != nullptr ? display->userArea : juce::Rectangle<int>());
    setSize(width, width * c_height / c_width);
    m_sizeRestored = true;

    // A newer release: the popup sits on the editor itself, above the scaled content, and scales with it.
    addChildComponent(m_updatePopup);
    m_updatePopup.setBounds(0, 0, c_width, c_height);
    m_updatePopup.setTransform(m_content.getTransform());

    // Which control does the same thing as which key: its tooltip then names the key, whatever it has been
    // changed to, and the menu bar and the menus take their keys from the same place.
    {
        using A = Shortcuts::Action;
        const std::pair<juce::Component*, A> tagged[] = {
            { &m_playButton, A::PlayStop },       { &m_throwButton, A::Throw },        { &m_swell, A::Swell },
            { &m_tape, A::ArmTape },              { &m_retroButton, A::SaveLast },     { &m_dice, A::Randomise },
            { &m_rollBack, A::PreviousRoll },     { &m_rollForward, A::NextRoll },     { &m_keepRoll, A::KeepRoll },
            { &m_presetPrev, A::PreviousPreset }, { &m_presetNext, A::NextPreset },    { &m_undoButton, A::Undo },
            { &m_redoButton, A::Redo },           { &m_newButton, A::NewSession },     { &m_takesButton, A::Takes },
            { &m_variationsButton, A::Variations },{ &m_settingsButton, A::Settings }, { &m_morphButton, A::MorphPad },
            { &m_cablesButton, A::Cables },
        };
        for (const auto& [component, action] : tagged)
            Shortcuts::tag(*component, action);
    }

    // Whatever the keys are, the places that draw one read it now.
    refreshShortcuts();

   #if JUCE_MAC
    // The app gets a menu bar: the same actions, showing the same keys.
    if (juce::PluginHostType::getPluginLoadedAs() == juce::AudioProcessor::wrapperType_Standalone)
    {
        m_menuBar = std::make_unique<MenuBar>(*this);
        juce::MenuBarModel::setMacMainMenu(m_menuBar.get());
    }
   #endif

    startTimerHz(60);

    // Checks GitHub at most once a day in the background, silently; only an editor ever asks.
    if (checkForUpdates)
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
   #if JUCE_MAC
    if (m_menuBar != nullptr)
    {
        juce::MenuBarModel::setMacMainMenu(nullptr);
        m_menuBar = nullptr;
    }
   #endif
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
    // Heading (the caption, unless the tip brings its own), then what it does. The how-to line that is the same
    // on every knob is added by the tooltip window (SwooshTooltips), in its second tier, so it is written once.
    const juce::String id(paramId);
    // Energy can follow a MIDI controller, so its right-click is a menu (with typing a value in it).
    const bool learnable = id == ParamIDs::energy;
    knob->setTooltip((tip.containsChar('\n') ? tip : caption + "\n" + tip)
                     + (learnable ? "\nRight-click for Learn MIDI CC, so a knob on a MIDI controller moves it." : ""));
    m_content.addAndMakeVisible(*knob);
    m_attachments.push_back(std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(m_processor.getState(), paramId, *knob));
    if (auto* param = m_processor.getState().getParameter(paramId))
    {
        knob->setDoubleClickReturnValue(true, param->convertFrom0to1(param->getDefaultValue()));
        knob->setTypedParameter(param, *m_fonts);
    }
    knob->onDragStart = [this, id] { m_morph.cancel(id); };
    knob->onTypedValue = [this, id] { m_morph.cancel(id); };
    if (learnable)
        knob->onContextMenu = [this, k = knob.get()] { m_hold.showControlMenu(SwooshbucklerAudioProcessor::c_ccEnergy, *k); };
    m_controls[id] = knob.get();
    m_knobs.push_back(std::move(knob));
    return *m_knobs.back();
}

//==============================================================================
void SwooshbucklerAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(TC::window);
}

void SwooshbucklerAudioProcessorEditor::resized()
{
    const float scale = static_cast<float>(getWidth()) / static_cast<float>(c_width);
    m_content.setTransform(juce::AffineTransform::scale(scale));
    m_updatePopup.setTransform(juce::AffineTransform::scale(scale));
    if (m_sizeRestored)
        m_processor.savedEditorWidth = getWidth();
}

int SwooshbucklerAudioProcessorEditor::firstOpenWidth(juce::Rectangle<int> usableArea)
{
    // Whole on the screen, with room for a title bar and a little air; never under the smallest size.
    const int minWidth = juce::roundToInt(c_width * c_minScale);
    if (usableArea.isEmpty())
        return c_width;
    const int byWidth = usableArea.getWidth() - 16;
    const int byHeight = (usableArea.getHeight() - 48) * c_width / c_height;
    return juce::jlimit(minWidth, c_width, juce::jmin(byWidth, byHeight));
}

std::vector<std::pair<juce::String, juce::Rectangle<int>>> SwooshbucklerAudioProcessorEditor::panelBounds() const
{
    return { { "Tracks", m_hold.getBounds() },         { "Sea", m_sea.getBounds() },
             { "Ride", m_performPanel },               { "Bus A", m_sections[0].bounds },
             { "Bus B", m_sections[1].bounds },        { "Hits", m_sections[2].bounds },
             { "Output", m_sections[3].bounds },       { "Stowaway", m_stowPanel },
             { "Modulation", m_modSection },           { "Footer", m_footer } };
}

void SwooshbucklerAudioProcessorEditor::layoutContent()
{
    // Header: the name at the left, then three groups with a rule between them: transport | presets | tape, and
    // Settings at the far right.
    {
        const int linkWidth = juce::GlyphArrangement::getStringWidthInt(m_fonts->caption(), m_websiteLink.getButtonText()) + 8;
        m_websiteLink.setBounds(58, 33, linkWidth, 15);
        m_versionLink.setBounds(m_websiteLink.getRight() + 4, 33,
                                juce::GlyphArrangement::getStringWidthInt(m_fonts->caption(), m_versionLink.getButtonText()) + 8, 15);
        m_titleArea = { Lay::left, 2, 236, 32 };
    }
    {
        static constexpr int y = 10, h = 30, groupGap = 21; // static: MSVC will not take a local as a default argument
        int x = 262;
        auto place = [&x](juce::Component& c, int width, int gap, int top = y, int height = h)
        {
            c.setBounds(x, top, width, height);
            x += width + gap;
        };
        // Transport: Follow Host (in a host only), Play, New.
        const bool host = m_followHost.isVisible();
        if (host)
            place(m_followHost, 96, 4);
        else
            m_followHost.setBounds(x, y, 96, h);
        place(m_playButton, 76, 4, y - 1, h + 2);
        place(m_newButton, 62, 0);
        m_headerRules[0] = x + groupGap / 2;
        x += groupGap;
        // Presets: Undo, Redo | < preset > A / B | Randomise, its history, Keep. The preset box takes what is left.
        constexpr int tapeWidth = 124 + 4 + 78 + 4 + 88 + 4 + 72, settingsWidth = 94;
        constexpr int afterPreset = 2 + 24 + 4 + 26 + 12 + 106 + 2 + 24 + 2 + 24 + 2 + 26;
        place(m_undoButton, 26, 2);
        place(m_redoButton, 26, 10);
        place(m_presetPrev, 24, 2);
        const int presetWidth = Lay::right - settingsWidth - groupGap - tapeWidth - groupGap - afterPreset - x;
        place(m_presetName, presetWidth, 2);
        place(m_presetNext, 24, 4);
        place(m_abButton, 26, 12);
        place(m_dice, 106, 2);
        place(m_rollBack, 24, 2);
        place(m_rollForward, 24, 2);
        place(m_keepRoll, 26, 0);
        m_headerRules[1] = x + groupGap / 2;
        x += groupGap;
        // Tape: the tape itself, Last 60 s, Render Variations and the Takes.
        place(m_tape, 124, 4, y - 2, h + 4);
        place(m_retroButton, 78, 4);
        place(m_variationsButton, 88, 4);
        place(m_takesButton, 72, groupGap);
        place(m_settingsButton, Lay::right - x, 0);
    }

    // Row 1: the tracks and the sea, side by side.
    m_hold.setBounds(Lay::left, Lay::topY, Lay::right - Lay::left - Lay::gutter - Lay::seaW, Lay::topH);
    m_sea.setBounds(Lay::right - Lay::seaW, Lay::topY, Lay::seaW, Lay::topH);
    // The morph pad is a dock over the sea's lower right, so the sky, the moon and the comets stay in view; the
    // Morph button that shows it sits in the sea's top right, directly over it and clear of the corner plate.
    constexpr int morphW = 380, morphH = 214;
    m_morphPad.setBounds(m_sea.getRight() - 10 - morphW, m_sea.getBottom() - 10 - morphH, morphW, morphH);
    m_morphButton.setBounds(m_sea.getRight() - 20 - 100, m_sea.getY() + 12, 100, 22);
    m_sea.setReservedArea(m_sea.getLocalArea(&m_morphButton, m_morphButton.getLocalBounds()));

    // A knob in its cell: the wheel of its size, whole, in the middle of the room it is given.
    auto placeKnob = [this](const char* id, juce::Rectangle<int> cell, WheelKnob::Size size)
    {
        auto* control = m_controls[id];
        if (auto* knob = dynamic_cast<WheelKnob*>(control))
            knob->setSize(size);
        control->setBounds(cell.withSizeKeepingCentre(cell.getWidth(), juce::jmin(cell.getHeight(), WheelKnob::cellHeight(size))));
    };
    constexpr int primaryH = WheelKnob::cellHeight(WheelKnob::Size::Primary), secondaryH = WheelKnob::cellHeight(WheelKnob::Size::Secondary);

    // Row 2: Ride, Bus A, Bus B. Row 3: Hits (under Ride and bus A, its knobs on bus A's columns), Output (under bus B).
    m_performPanel = { Lay::left, Lay::row2Y, Lay::rideW, Lay::rowH };
    m_sections[0] = { 1, "Bus A", "Growls and tones, and the key for B", TC::busA, { Lay::busAX, Lay::row2Y, Lay::busAW, Lay::rowH }, Icons::Icon::LetterA };
    m_sections[1] = { 2, "Bus B", "Air and beds", TC::busB, { Lay::busBX, Lay::row2Y, Lay::busBW, Lay::rowH }, Icons::Icon::LetterB };
    m_sections[2] = { 3, "Hits", "At the loudest moments", TC::hits, { Lay::left, Lay::row3Y, Lay::busAX + Lay::busAW - Lay::left, Lay::rowH }, Icons::Icon::Hits };
    m_sections[3] = { 4, "Output", "Space, loudness, limiter", TC::output, { Lay::busBX, Lay::row3Y, Lay::busBW, Lay::rowH }, Icons::Icon::Output };
    const auto& busA = m_sections[0].bounds;
    const auto& busB = m_sections[1].bounds;
    const auto& hits = m_sections[2].bounds;
    const auto& out = m_sections[3].bounds;

    // Ride: Swell at the left; its knobs in a row with Throw Length at the end, over Auto Ride, Follow Master and
    // Throw; the ride of the last 8 seconds along the bottom. Keys is in the title bar.
    {
        const auto& ride = m_performPanel;
        const int bodyY = ride.getY() + 37, bodyBottom = ride.getBottom() - 6;
        m_swell.setBounds(ride.getX() + 10, bodyY + 3 + (bodyBottom - bodyY - 3 - 92) / 2, 92, 92);
        const int kx = ride.getX() + 106, kRight = ride.getRight() - Lay::pad + 4;
        constexpr int lengthW = 62;
        const int cw = (kRight - lengthW - kx) / 5;
        const char* ids[] = { ParamIDs::energy, ParamIDs::rate, ParamIDs::fill, ParamIDs::chaos, ParamIDs::pitch };
        for (int i = 0; i < 5; ++i)
            placeKnob(ids[i], { kx + i * cw, bodyY, cw, secondaryH }, WheelKnob::Size::Secondary);
        placeKnob(ParamIDs::throwLength, { kRight - lengthW, bodyY, lengthW, secondaryH }, WheelKnob::Size::Secondary);
        const int by = bodyY + secondaryH + 3;
        m_rideButton.setBounds(kx, by, 84, 18);
        m_followMasterButton.setBounds(kx + 88, by, 106, 18);
        m_throwButton.setBounds(kRight - 80, by, 80, 18);
        m_rideStripArea = { kx, ride.getY() + Lay::stripY, kRight - kx, Lay::stripH };
        m_rideStrip->setBounds(m_rideStripArea);

        const int keysWidth = 66, keysLabel = 28;
        m_keysMode.setBounds(ride.getRight() - Lay::pad - keysWidth, ride.getY() + 9, keysWidth, 22);
        m_keysLabel = { m_keysMode.getX() - keysLabel - 4, m_keysMode.getY(), keysLabel, 22 };
        const int titleEnd = ride.getX() + SwooshLookAndFeel::titleEnd(*m_fonts, "Ride") + 12;
        m_rideStatusArea = { titleEnd, ride.getY() + 9, m_keysLabel.getX() - 4 - 44 - 6 - titleEnd, 22 }; // the die and lock sit between it and Keys
    }

    // A row of primary knobs on the shared column pitch, from x.
    auto row = [&placeKnob](int x, int y, std::vector<const char*> ids)
    {
        for (const auto* id : ids)
        {
            placeKnob(id, { x, y, Lay::pitch, primaryH }, WheelKnob::Size::Primary);
            x += Lay::pitch;
        }
    };
    for (int b = 0; b < ParamIDs::numBuses; ++b)
    {
        std::vector<const char*> ids;
        for (int k = 0; k < ParamIDs::numBusControls; ++k)
            if (ParamIDs::bus[b][k] != nullptr)
                ids.push_back(ParamIDs::bus[b][k]);
        const auto& panel = m_sections[static_cast<size_t>(b)].bounds;
        row(panel.getX() + Lay::pad, panel.getY() + Lay::knobsY, ids);
    }
    // The Hits' seven knobs sit on bus A's seven columns.
    row(busA.getX() + Lay::pad, hits.getY() + Lay::knobsY,
        { ParamIDs::hitSense, ParamIDs::hitBoost, ParamIDs::hitRelease, ParamIDs::hitHold, ParamIDs::hitFloor, ParamIDs::hitDuck, ParamIDs::duckReturn });
    // Bus B's last two knobs (what bus A does to it) are in a tinted group of their own.
    m_fromBusAGroup = { busB.getRight() - Lay::pad - 2 * Lay::pitch - 2, busB.getY() + 33, 2 * Lay::pitch + 6, Lay::stripY - 33 - 3 };
    m_fromBusACaption = m_fromBusAGroup.reduced(6, 0).withTrimmedTop(1).withHeight(13);

    {
        // Title bars. Bus A: Pass. Bus B: Pass, Link, Take Turns. Each section's die and lock sit left of them.
        const int y = 9, passWidth = 78, passLabel = 30;
        m_takeTurns.setBounds(busB.getRight() - Lay::pad - 98, busB.getY() + y, 98, 22);
        m_linkBuses.setBounds(m_takeTurns.getX() - 4 - 58, busB.getY() + y, 58, 22);
        m_passMode[0].setBounds(busA.getRight() - Lay::pad - passWidth, busA.getY() + y, passWidth, 22);
        m_passMode[1].setBounds(m_linkBuses.getX() - 8 - passWidth, busB.getY() + y, passWidth, 22);
        for (size_t b = 0; b < m_passMode.size(); ++b)
            m_passModeLabel[b] = { m_passMode[b].getX() - passLabel - 4, m_passMode[b].getY(), passLabel, 22 };

        auto diceAt = [this](int section, int right, int top)
        {
            m_sectionLocks[static_cast<size_t>(section)]->setBounds(right - 20, top + 1, 20, 20);
            m_sectionDice[static_cast<size_t>(section)]->setBounds(right - 42, top + 1, 20, 20);
        };
        diceAt(Presets::Ride, m_keysLabel.getX() - 4, m_performPanel.getY() + y);
        diceAt(Presets::BusA, m_passModeLabel[0].getX() - 4, busA.getY() + y);
        diceAt(Presets::BusB, m_passModeLabel[1].getX() - 4, busB.getY() + y);
        diceAt(Presets::Hits, hits.getRight() - Lay::pad, hits.getY() + y);
        diceAt(Presets::Output, out.getRight() - Lay::pad, out.getY() + y);
        // Each subtitle stops short of its title bar's first control.
        for (size_t i = 0; i < m_sections.size(); ++i)
            m_sections[i].subtitleRight = m_sectionDice[i + 1]->getX() - 8;

        // The scope strips: what each section sends on, along the bottom of its panel.
        for (size_t i = 0; i < m_sectionScopes.size(); ++i)
        {
            const auto& panel = m_sections[i].bounds;
            m_sectionScopes[i]->setBounds(panel.getX() + Lay::pad, panel.getY() + Lay::stripY, panel.getWidth() - 2 * Lay::pad, Lay::stripH);
        }
    }

    // Hits, left of its knobs (under the Ride panel): the gate lamp, Aim and Sync Hits, and the Gate and Duck menus.
    {
        const int x = hits.getX() + Lay::pad, top = hits.getY() + Lay::knobsY + 6;
        m_gateLamp = { x, top, 84, 76 };
        m_aimHits.setBounds(x + 96, top + 10, 104, 24);
        m_syncHits.setBounds(x + 96, top + 42, 104, 24);
        const int labelX = x + 216, label = 34, menuX = labelX + label + 4, menuW = busA.getX() + Lay::pad - 16 - menuX;
        m_gateModeLabel = { labelX, top + 11, label, 22 };
        m_gateMode.setBounds(menuX, top + 11, menuW, 22);
        m_duckModeLabel = { labelX, top + 43, label, 22 };
        m_duckMode.setBounds(menuX, top + 43, menuW, 22);
    }

    // Output, left to right: the Peak Tremolo group (it comes before Space in the signal) with its switch and name
    // over its two menus and its three knobs; then Space, Inflate, Push and Ceiling; then the meters and the Trim fader.
    {
        const int top = out.getY() + 34, bottom = out.getY() + Lay::stripY - 3;
        constexpr int menuW = 84, tremKnobW = 52;
        m_tremGroup = { out.getX() + Lay::pad - 2, top, 8 + menuW + 4 + 3 * tremKnobW + 4, bottom - top };
        auto head = m_tremGroup.reduced(8, 0).withTrimmedTop(3).withHeight(16);
        m_tremOn.setBounds(head.removeFromLeft(30));
        head.removeFromLeft(6);
        m_tremCaption = head.withWidth(menuW + 40);
        const int menuY = m_tremGroup.getY() + 28;
        m_tremShape.setBounds(m_tremGroup.getX() + 8, menuY, menuW, 22);
        m_tremTarget.setBounds(m_tremGroup.getX() + 8, menuY + 28, menuW, 22);
        int kx = m_tremGroup.getX() + 8 + menuW + 4;
        const int ky = m_tremGroup.getBottom() - 2 - secondaryH;
        for (const auto* id : { ParamIDs::tremDepth, ParamIDs::tremRate, ParamIDs::tremThreshold })
        {
            placeKnob(id, { kx, ky, tremKnobW, secondaryH }, WheelKnob::Size::Secondary);
            kx += tremKnobW;
        }

        // The meters and the Trim fader: a name over each, the bars and the fader's slot, a number under each.
        constexpr int textRow = 13, trimW = 40, meterW = 88;
        m_meterArea = { out.getRight() - Lay::pad - trimW - 2 - meterW, top + 2, meterW, bottom - top - 2 };
        m_trimLabel = { m_meterArea.getRight() + 2, m_meterArea.getY(), trimW, textRow };
        m_trimValue = { m_trimLabel.getX(), m_meterArea.getBottom() - textRow, trimW, textRow };
        m_trim.setBounds(m_trimLabel.getCentreX() - 14, m_trimLabel.getBottom() + 1, 28, m_trimValue.getY() - m_trimLabel.getBottom() - 2);
        const int restX = m_tremGroup.getRight() + 6, restW = m_meterArea.getX() - 4 - restX, ow = restW / 4;
        int ox = restX + (restW - 4 * ow) / 2;
        for (const auto* id : { ParamIDs::space, ParamIDs::inflate, ParamIDs::push, ParamIDs::ceiling })
        {
            placeKnob(id, { ox, ky - 8, ow, secondaryH }, WheelKnob::Size::Secondary);
            ox += ow;
        }
    }

    // Row 4: Stowaway's plate at the left, as far open as it is, and Modulation in the rest of the row.
    m_wires.setBounds(0, 0, c_width, c_height);
    layoutBottomRow();
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

    m_footer = { Lay::left, Lay::footerY, Lay::right - Lay::left, Lay::footerH };
    m_toastArea = m_footer.withTrimmedLeft(m_footer.getWidth() / 2);
    for (auto* overlay : std::initializer_list<juce::Component*> { &m_settings, &m_takes, &m_variations, &m_about })
    {
        overlay->setBounds(0, 0, c_width, c_height);
        overlay->setVisible(false);
    }
}

void SwooshbucklerAudioProcessorEditor::layoutBottomRow()
{
    // Stowaway's plate eases between its two widths. The first half of the way it wears the folded layout (its
    // switch and Mode under the nameplate, a line saying what it is), fading out; the second half the open one
    // (switch and Mode in the title bar, the five wheels, dice and lock), fading in. A wheel shows once the plate
    // has opened past it, so nothing ever stands outside the plate or on Modulation.
    const float open = juce::jlimit(0.0f, 1.0f, m_stowOpen.value);
    const bool openLayout = open >= 0.5f;
    const float alpha = openLayout ? 2.0f * open - 1.0f : 1.0f - 2.0f * open;
    const int titleEnd = SwooshLookAndFeel::titleEnd(*m_fonts, "Stowaway");
    const int foldedW = juce::jmax(Lay::stowFoldedW, titleEnd + 8 + 20 + Lay::pad);
    const int width = juce::roundToInt(static_cast<float>(foldedW) + static_cast<float>(Lay::stowOpenW - foldedW) * open);
    m_stowPanel = { Lay::left, Lay::modY, width, Lay::modH };
    const auto& s = m_stowPanel;
    const int inside = s.getRight() - 6;

    m_stowNameplate = { s.getX() + 18, s.getY() + 8, titleEnd - 18, 24 };
    const bool opening = m_stowOpen.target() > 0.5f;
    m_stowFold.setBounds(s.getRight() - Lay::pad - 20, s.getY() + 10, 20, 20);
    m_stowFold.setIcon(opening ? IconButton::Icon::Previous : IconButton::Icon::Next);
    m_stowFold.setTooltip(opening ? "Collapse Stowaway\nFolds the plate down to its switch, its Mode and its scope, and gives the room back to "
                                    "Modulation. Its settings stay as they are, and it goes on playing. Clicking the nameplate does the same."
                                  : "Expand Stowaway\nOpens the plate to show its five knobs, its die and its lock. It opens by itself the first "
                                    "time Stowaway is switched on. Clicking the nameplate does the same.");
    m_stowStrip = { s.getX() + Lay::pad, s.getY() + Lay::stowStripY, width - 2 * Lay::pad, Lay::stowStripH };
    m_stowScope->setBounds(m_stowStrip);

    auto& die = *m_sectionDice[Presets::Stowaway];
    auto& lock = *m_sectionLocks[Presets::Stowaway];
    lock.setBounds(m_stowFold.getX() - 4 - 20, s.getY() + 10, 20, 20);
    die.setBounds(lock.getX() - 2 - 20, s.getY() + 10, 20, 20);
    if (openLayout)
    {
        m_stowOn.setBounds(s.getX() + titleEnd + 8, s.getY() + 12, Lay::stowSwitchW, 16);
        m_stowMode.setBounds(m_stowOn.getRight() + 6, s.getY() + 9, 72, 22);
        m_stowNote = {};
    }
    else
    {
        m_stowOn.setBounds(s.getX() + Lay::stowPad, s.getY() + 43, Lay::stowSwitchW, 16);
        m_stowMode.setBounds(m_stowOn.getRight() + 6, s.getY() + 40, width - (Lay::stowPad + Lay::stowSwitchW + 6) - Lay::pad, 22);
        m_stowNote = { s.getX() + Lay::stowPad, s.getY() + 68, width - 2 * Lay::stowPad, 36 };
    }
    auto show = [alpha](juce::Component& c, bool visible)
    {
        c.setVisible(visible);
        c.setAlpha(visible ? alpha : 1.0f);
    };
    show(m_stowOn, true);
    show(m_stowMode, m_stowMode.getRight() <= die.getX() - 4 || ! openLayout);
    show(die, openLayout && die.getX() >= m_stowMode.getRight() + 4);
    show(lock, openLayout && die.isVisible());
    for (int k = 0; k < ParamIDs::numStowControls; ++k)
    {
        auto* knob = m_controls[ParamIDs::stow[k]];
        knob->setBounds(s.getX() + Lay::stowPad + k * Lay::pitch, s.getY() + Lay::stowKnobsY, Lay::pitch, WheelKnob::cellHeight(WheelKnob::Size::Secondary));
        show(*knob, openLayout && knob->getRight() <= inside);
    }

    // Modulation: its title bar (the name, the hint, Cables at its right end), then the four modulators.
    m_modSection = { s.getRight() + Lay::gutter, Lay::modY, Lay::right - (s.getRight() + Lay::gutter), Lay::modH };
    m_cablesButton.setBounds(m_modSection.getRight() - Lay::pad - 88, m_modSection.getY() + 9, 88, 22);
    m_modPanel.setBounds(m_modSection.getX() + Lay::pad, m_modSection.getY() + Lay::modCardsY, m_modSection.getWidth() - 2 * Lay::pad, Lay::modCardsH);
    m_wires.rebuildSoon();
    m_content.repaint(juce::Rectangle<int>(0, Lay::modY - 8, c_width, Lay::modH + 16));
}

void SwooshbucklerAudioProcessorEditor::setStowawayOpen(bool open)
{
    m_processor.setStowawayPanel(open ? 1 : 0);
}

juce::String SwooshbucklerAudioProcessorEditor::stowawayNote(int mode, bool on)
{
    if (! on)
        return "Off. Switch it on for an extra bus.";
    switch (mode)
    {
        case ParamIDs::StowWake: return "A trail behind each of Bus A's passes.";
        case ParamIDs::StowFly:  return "A third mover, with passes of its own.";
        case ParamIDs::StowDry:  return "Riding only, straight to the output.";
        default:                 return "Low end only, swelling with Bus A.";
    }
}

juce::String SwooshbucklerAudioProcessorEditor::stowawayKnobTip(int control, int mode, bool on)
{
    using namespace ParamIDs;
    const juce::String modeName = stowModeNames[juce::jlimit(0, numStowModes - 1, mode)];
    juce::String what;
    if (! Params::stowControlInUse(mode, control))
        what = mode == StowSub ? juce::String("Not used in Sub: a sub is mono, in the middle. It rests, and keeps its setting for the other modes.")
                               : juce::String("Not used in Dry: its tracks are only ridden, with nothing added. It rests, and keeps its setting for the other modes.");
    else if (control == StowFollow)
        what = mode == StowSub  ? "How much the sub swells with Bus A's passes. 0%: a steady sub. 100%: it rises and falls as far as Bus A does. "
                                  "With no track on Bus A it follows Bus B, and with none on either its own tracks are ridden instead."
             : mode == StowWake ? "How far the trail shuts between Bus A's passes. 0%: a steady layer. 100%: silent until Bus A falls away."
             : mode == StowFly  ? "How closely its passes keep time with Bus A's. 0%: they keep their own time. 100%: each one is at its closest "
                                  "at the same moment as one of Bus A's."
                                : "How much of the ride its tracks keep. 0%: they stay at their faders. 100%: they are ridden like any other track.";
    else if (control == StowLow)
        what = mode == StowSub ? "Weight, after the crossover: a note an octave down and overtones just above the crossover. A second low-pass keeps "
                                 "what it adds within about an octave above the crossover."
                               : "Weight: a note an octave down and a lift of the low end.";
    else if (control == StowFilter)
        what = mode == StowSub ? "The crossover, from 40 to 250 Hz. Only what is under it comes through, whatever is sent here: above it the sound "
                                 "falls away at 48 dB per octave."
                               : "A tilt. Open in the middle; turn it down to close a low-pass over the sound (as far as 150 Hz), or up to raise "
                                 "a high-pass under it (as far as 2.5 kHz).";
    else if (control == StowDrive)
        what = mode == StowSub ? "Saturation, after the crossover: it thickens the sub and flattens its swell without turning it up."
                               : "Saturation: grit and density, with the level kept steady.";
    else
        what = mode == StowWake ? "How far out to the side the trail sits, behind Bus A's pass. 0%: in the middle."
             : mode == StowFly  ? "How far each of its passes travels across the stereo field. 0%: in the middle."
                                : "How wide a stereo sound stays. 0%: mono. 100%: its own left and right.";
    return juce::String("Stowaway: ") + stowNames[control] + "\nIn " + modeName + ": " + what
           + (on ? "" : "\nStowaway is off, so this does nothing until it is switched on. It can still be set.");
}

void SwooshbucklerAudioProcessorEditor::showStowaway(float dt)
{
    const bool on = m_processor.isStowawayOn();
    const int mode = m_processor.getStowawayMode();
    // Switched on with a plate that has never been opened: it opens by itself, this once.
    if (on && ! m_stowWasOn && m_processor.getStowawayPanel() < 0)
        m_processor.setStowawayPanel(1);
    m_stowWasOn = on;
    m_stowOpen.to(m_processor.getStowawayPanel() == 1 ? 1.0f : 0.0f, Motion::Ms::overlayIn);
    if (m_stowOpen.step(dt))
        layoutBottomRow();

    // The looks follow the switch and the Mode: off, everything on the plate rests (it can still be set up); on,
    // a wheel that does nothing in this Mode rests, and each wheel's tooltip says what it does here.
    const int shown = (on ? 1 : 0) + 2 * mode;
    if (shown == m_stowShown)
        return;
    m_stowShown = shown;
    m_stowOn.setButtonText(on ? "On" : "Off"); // the switch says which it is, in a word
    Theme::setLook(m_stowMode, on ? Theme::Look::Normal : Theme::Look::Resting);
    for (int k = 0; k < ParamIDs::numStowControls; ++k)
        if (auto* knob = m_controls[ParamIDs::stow[k]])
        {
            Theme::setLook(*knob, on && Params::stowControlInUse(mode, k) ? Theme::Look::Normal : Theme::Look::Resting);
            knob->setTooltip(stowawayKnobTip(k, mode, on));
            if (auto* wheel = dynamic_cast<WheelKnob*>(knob))
                wheel->valueTextChanged(); // its readout follows the Mode too (Filter: a crossover in Sub, a tilt otherwise)
        }
    m_stowScope->setVisible(on);
    m_content.repaint(m_stowPanel);
}

void SwooshbucklerAudioProcessorEditor::paintContent(juce::Graphics& g)
{
    g.fillAll(TC::window);
    const auto& f = *m_fonts;

    // The header: the mark, the name, and the one rope rule in the window.
    if (g.clipRegionIntersects({ 0, 0, c_width, Lay::topY }))
    {
        SwooshLookAndFeel::drawCutlasses(g, { 14.0f, 7.0f, 38.0f, 34.0f }, TC::output, TC::brass);
        g.setColour(TC::text);
        Theme::fitText(g, "Swooshbuckler", juce::Rectangle<float>(58.0f, -2.0f, 184.0f, 38.0f), f.wordmark(), juce::Justification::centredLeft);
        SwooshLookAndFeel::drawRope(g, { static_cast<float>(Lay::left), static_cast<float>(Lay::headerRule) },
                                    { static_cast<float>(Lay::right), static_cast<float>(Lay::headerRule) }, TC::brass);
        // A rule between the header's groups: transport | presets | tape.
        for (const int x : m_headerRules)
            SwooshLookAndFeel::drawRule(g, { static_cast<float>(x), 13.0f }, { static_cast<float>(x), 37.0f });
    }

    SwooshLookAndFeel::drawPanel(g, m_performPanel.toFloat(), "Ride", {}, TC::brass, f, Icons::Icon::Ride);
    {
        // The subtitle line doubles as the readout: the time to a throw's peak, or the keys that are held.
        const bool readout = m_rideStatus.isNotEmpty();
        g.setColour(readout ? TC::brass : TC::textFaint);
        Theme::fitText(g, readout ? m_rideStatus : juce::String("Rides the faders for you"), m_rideStatusArea, readout ? f.control() : f.body(),
                       juce::Justification::centredLeft, readout ? Theme::Fit::Message : Theme::Fit::Sentence);
    }
    for (const auto& sec : m_sections)
        SwooshLookAndFeel::drawPanel(g, sec.bounds.toFloat(), sec.title, sec.subtitle, sec.accent, f, sec.mark, static_cast<float>(sec.subtitleRight));
    SwooshLookAndFeel::drawPanel(g, m_modSection.toFloat(), "Modulation", "Drag a jack onto any knob to wire it.", TC::brass, f,
                                 Icons::Icon::Modulation, static_cast<float>(m_cablesButton.getX() - 8));

    // Stowaway's plate. Off, the whole plate rests: darker, its tally hollow, its name dim, and its scope's window
    // says Off. On, its tally is in its colour and a lit line runs along its top edge.
    if (g.clipRegionIntersects(m_stowPanel.expanded(4, 6)))
    {
        const bool on = m_processor.isStowawayOn();
        SwooshLookAndFeel::drawPanel(g, m_stowPanel.toFloat(), "Stowaway", {}, TC::stowaway, f, Icons::Icon::LetterS, 0.0f, ! on);
        if (on)
        {
            g.setColour(TC::stowaway.withAlpha(0.85f));
            g.fillRect(juce::Rectangle<float>(static_cast<float>(m_stowPanel.getX()) + 18.0f, static_cast<float>(m_stowPanel.getY()) + 1.0f,
                                              static_cast<float>(m_stowPanel.getWidth()) - 36.0f, 2.0f));
        }
        else
        {
            SwooshLookAndFeel::drawWell(g, m_stowStrip.toFloat(), Theme::Cut::small);
            g.setColour(TC::textFaint);
            Theme::fitText(g, "Off", m_stowStrip, f.caption(), juce::Justification::centred);
        }
        // Folded: a line saying what it is now, under its switch and Mode.
        if (! m_stowNote.isEmpty())
        {
            const float alpha = juce::jlimit(0.0f, 1.0f, 1.0f - 2.0f * m_stowOpen.value);
            if (alpha > 0.01f)
                Theme::fitLines(g, stowawayNote(m_processor.getStowawayMode(), on), m_stowNote, f.body(),
                                (on ? TC::textDim : TC::textFaint).withMultipliedAlpha(alpha));
        }
    }
    g.setColour(TC::textDim);
    Theme::fitText(g, "Keys", m_keysLabel, f.caption(), juce::Justification::centredRight);
    Theme::fitText(g, "Gate", m_gateModeLabel, f.caption(), juce::Justification::centredLeft);
    Theme::fitText(g, "Duck", m_duckModeLabel, f.caption(), juce::Justification::centredLeft);
    for (const auto& label : m_passModeLabel)
        Theme::fitText(g, "Pass", label, f.caption(), juce::Justification::centredRight);

    // The signal's way through the panels: small brass arrows in the gutters.
    auto arrow = [&g](juce::Point<float> c, bool down)
    {
        juce::Path p;
        if (down) { p.startNewSubPath(c.x - 5.0f, c.y - 2.5f); p.lineTo(c.x, c.y + 2.5f); p.lineTo(c.x + 5.0f, c.y - 2.5f); }
        else      { p.startNewSubPath(c.x - 2.5f, c.y - 5.0f); p.lineTo(c.x + 2.5f, c.y); p.lineTo(c.x - 2.5f, c.y + 5.0f); }
        g.setColour(TC::brass.withAlpha(0.8f));
        g.strokePath(p, juce::PathStrokeType(2.0f, juce::PathStrokeType::mitered, juce::PathStrokeType::butt));
    };
    {
        const float gutter = static_cast<float>(Lay::gutter) * 0.5f;
        const auto a = m_sections[0].bounds.toFloat(), b = m_sections[1].bounds.toFloat(), h = m_sections[2].bounds.toFloat(), o = m_sections[3].bounds.toFloat();
        arrow({ a.getCentreX(), a.getY() - gutter }, true);        // tracks -> buses
        arrow({ b.getX() - gutter, a.getCentreY() }, false);       // A -> B
        arrow({ a.getCentreX(), h.getY() - gutter }, true);        // buses -> hits
        arrow({ o.getX() - gutter, h.getCentreY() }, false);       // hits -> output
    }

    // Bus B's Ducked and Filtered are what bus A does to it: a tinted group of their own, in A's colour.
    if (g.clipRegionIntersects(m_fromBusAGroup))
    {
        const auto r = m_fromBusAGroup.toFloat();
        g.setColour(TC::busA.withAlpha(0.07f));
        g.fillRect(r);
        g.setColour(TC::busA.withAlpha(0.45f));
        Theme::Plate::stroke(g, r, 0.0f);
        g.setColour(TC::busA.brighter(0.25f));
        Theme::fitText(g, "From Bus A", m_fromBusACaption, f.caption(), juce::Justification::centredLeft);
    }

    // Hit gate lamp: a cherry light in a dark socket.
    if (g.clipRegionIntersects(m_gateLamp.expanded(10)))
    {
        const auto lamp = juce::Rectangle<float>(30.0f, 30.0f).withCentre(m_gateLamp.toFloat().getCentre().translated(0.0f, -10.0f));
        g.setColour(TC::well);
        g.fillEllipse(lamp.expanded(3.0f));
        g.setColour(TC::hits.withAlpha(0.15f + 0.35f * m_gate));
        g.fillEllipse(lamp.expanded(8.0f * m_gate));
        g.setColour(TC::hits.withAlpha(0.18f + 0.82f * m_gate));
        g.fillEllipse(lamp);
        g.setColour(TC::textDim);
        Theme::fitText(g, m_aimHits.getToggleState() ? "Aimed Hits" : "Hit Gate", m_gateLamp.withTop(m_gateLamp.getBottom() - 16), f.caption(),
                       juce::Justification::centred);
    }

    // The Peak Tremolo group: a tinted inset with its name at the top, over its switch, menus and knobs.
    if (g.clipRegionIntersects(m_tremGroup))
    {
        g.setColour(TC::window.withAlpha(0.45f));
        Theme::Plate::fill(g, m_tremGroup.toFloat(), Theme::Cut::control);
        Theme::Plate::wellEdge(g, m_tremGroup.toFloat(), Theme::Cut::control);
        g.setColour(m_tremShown == 1 ? TC::busA : TC::textDim);
        Theme::fitText(g, "Peak Tremolo", m_tremCaption, f.caption(), juce::Justification::centredLeft);
    }

    // Output meters: two slotted windows cut in the plate, with their scales, a held peak and a number under each.
    if (g.clipRegionIntersects(m_meterArea.getUnion(m_trimLabel).getUnion(m_trimValue)))
    {
        auto m = m_meterArea.toFloat();
        const auto names = m.removeFromTop(13.0f), numbers = m.removeFromBottom(13.0f);
        // Left to right: the GR scale, the GR bar, the Out bar, the Out scale.
        auto bars = m.reduced(0.0f, 3.0f);
        const auto grScale = bars.removeFromLeft(16.0f);
        const auto grBar = bars.removeFromLeft(18.0f);
        bars.removeFromLeft(12.0f);
        const auto outBar = bars.removeFromLeft(18.0f);
        const auto outScale = bars.removeFromLeft(24.0f);
        for (auto bar : { grBar, outBar })
            SwooshLookAndFeel::drawWell(g, bar, 0.0f);

        // GR grows down from the top, 12 dB at the bottom; Out grows up from the bottom, 0 dBFS at the top.
        const auto grIn = grBar.reduced(2.0f), outIn = outBar.reduced(2.0f);
        auto grY = [&grIn](float db) { return grIn.getY() + grIn.getHeight() * juce::jlimit(0.0f, 1.0f, -db / 12.0f); };
        auto outY = [&outIn](float db) { return outIn.getBottom() - outIn.getHeight() * outMeterPosition(db); };

        if (m_grDb < -0.05f)
        {
            g.setColour(TC::busA);
            g.fillRect(grIn.withBottom(grY(m_grDb)));
        }
        const float outDb = juce::Decibels::gainToDecibels(m_outLevel, -60.0f);
        if (outDb > -59.5f)
        {
            // Driftwood up to -6, brass above it, cherry for the last decibel.
            const float top = outY(outDb), y6 = outY(-6.0f), y1 = outY(-1.0f);
            g.setColour(TC::output);
            g.fillRect(outIn.withTop(juce::jmax(top, y6)));
            if (top < y6)
            {
                g.setColour(TC::brass);
                g.fillRect(outIn.withTop(juce::jmax(top, y1)).withBottom(y6));
            }
            if (top < y1)
            {
                g.setColour(TC::hits);
                g.fillRect(outIn.withTop(top).withBottom(y1));
            }
        }
        // The slots: a dark line across each bar every third pixel.
        g.setColour(TC::well);
        for (const auto& in : { grIn, outIn })
            for (float y = in.getBottom() - 3.0f; y > in.getY(); y -= 3.0f)
                g.fillRect(juce::Rectangle<float>(in.getX(), y, in.getWidth(), 1.0f));

        // Peak holds: the deepest reduction and the highest level of the last moments stay as a line.
        if (m_grHoldDb < -0.05f)
        {
            g.setColour(TC::text);
            g.fillRect(juce::Rectangle<float>(grIn.getX(), juce::jmin(grY(m_grHoldDb), grIn.getBottom() - 2.0f), grIn.getWidth(), 2.0f));
        }
        if (m_outHold > 0.001f)
        {
            const float holdDb = juce::Decibels::gainToDecibels(m_outHold, -60.0f);
            g.setColour(holdDb > -1.0f ? TC::hits : TC::text);
            g.fillRect(juce::Rectangle<float>(outIn.getX(), juce::jlimit(outIn.getY(), outIn.getBottom() - 2.0f, outY(holdDb) - 1.0f), outIn.getWidth(), 2.0f));
        }

        // Scales in dB: a tick against each bar, and a number beside the ticks that have room for one.
        const float row = 12.0f;
        for (const int db : { 0, 3, 6, 12 })
        {
            const float y = juce::jlimit(grIn.getY() + 0.5f, grIn.getBottom() - 0.5f, grY(static_cast<float>(-db)));
            g.setColour(TC::stroke);
            g.fillRect(juce::Rectangle<float>(grBar.getX() - 4.0f, y - 0.5f, 3.0f, 1.0f));
            if (db == 3)
                continue; // a tick only: its number would sit on its neighbours
            g.setColour(TC::textFaint);
            Theme::fitText(g, juce::String(db), juce::Rectangle<float>(grScale.getX(), juce::jlimit(grBar.getY() - 1.0f, grBar.getBottom() - row + 1.0f, y - row * 0.5f), grScale.getWidth() - 5.0f, row),
                           f.caption(), juce::Justification::centredRight);
        }
        for (const int db : { 0, 6, 12, 24, 48 })
        {
            const float y = juce::jlimit(outIn.getY() + 0.5f, outIn.getBottom() - 0.5f, outY(static_cast<float>(-db)));
            g.setColour(TC::stroke);
            g.fillRect(juce::Rectangle<float>(outBar.getRight() + 1.0f, y - 0.5f, 3.0f, 1.0f));
            if (db == 6)
                continue; // a tick only: its number would sit on the 0 above it
            g.setColour(TC::textFaint);
            Theme::fitText(g, db == 0 ? juce::String("0") : "-" + juce::String(db),
                           juce::Rectangle<float>(outScale.getX() + 6.0f, juce::jlimit(outBar.getY() - 1.0f, outBar.getBottom() - row + 1.0f, y - row * 0.5f), outScale.getWidth() - 6.0f, row),
                           f.caption(), juce::Justification::centredLeft);
        }

        // The Trim fader's scale: a tick at 0, -6, -12 and -24 dB, left of its slot.
        g.setColour(TC::stroke);
        for (const double db : { 0.0, -6.0, -12.0, -24.0 })
            g.fillRect(juce::Rectangle<float>(static_cast<float>(m_trim.getX()) + 2.0f, static_cast<float>(m_trim.getY()) + m_trim.getPositionOfValue(db) - 0.5f,
                                              db == 0.0 ? 6.0f : 4.0f, 1.0f));

        const auto grName = juce::Rectangle<float>(grBar.getX() - 9.0f, names.getY(), grBar.getWidth() + 18.0f, names.getHeight());
        const auto outName = grName.withX(outBar.getX() - 9.0f);
        g.setColour(TC::textDim);
        Theme::fitText(g, "GR", grName, f.caption(), juce::Justification::centred);
        Theme::fitText(g, "Out", outName, f.caption(), juce::Justification::centred);
        Theme::fitText(g, "Trim", m_trimLabel, f.caption(), juce::Justification::centred);

        // The numbers: the deepest recent reduction, the highest recent level, and where the fader is. In dB.
        g.setColour(m_grHoldDb < -0.05f ? TC::busA.brighter(0.2f) : TC::textFaint);
        Theme::fitText(g, m_grReadout, grName.withY(numbers.getY()), f.caption(), juce::Justification::centred);
        g.setColour(m_outHold > 0.001f ? TC::text : TC::textFaint);
        Theme::fitText(g, m_outReadout, outName.withY(numbers.getY()), f.caption(), juce::Justification::centred);
        g.setColour(TC::text);
        Theme::fitText(g, m_trimReadout, m_trimValue, f.caption(), juce::Justification::centred);
    }

    if (g.clipRegionIntersects(m_footer))
    {
        g.setColour(TC::textFaint);
        // The key named here is the one Swell has now: the line is written in refreshShortcuts, not here.
        Theme::fitText(g, m_footerHint,
                       m_footer.withWidth(m_footer.getWidth() / 2 - 12), f.body(), juce::Justification::centredLeft, Theme::Fit::Sentence);
        if (m_toast.isNotEmpty())
        {
            const float age = static_cast<float>(juce::Time::getMillisecondCounterHiRes() * 0.001 - m_toastTime);
            g.setColour(TC::brass.withAlpha(juce::jlimit(0.0f, 1.0f, 8.0f - age)));
            Theme::fitText(g, m_toast, m_toastArea, f.body(), juce::Justification::centredRight, Theme::Fit::Message);
        }
    }

}

//==============================================================================

void SwooshbucklerAudioProcessorEditor::refreshShortcuts()
{
   #if JUCE_MAC
    if (m_menuBar != nullptr)
        m_menuBar->menuItemsChanged();
   #endif
    // The two places a key is written into the drawing rather than asked for as it is needed: the hint under the
    // Swell button and the sea's own tip. Every other tooltip gets its key from the registry as it is shown.
    const auto swellKey = m_keys.keyText(Shortcuts::Action::Swell);
    m_swell.setKeyHint(swellKey);
    m_footerHint = "Drop sounds into Tracks, press Play, arm the tape and let it ride. Hold Swell"
                   + (swellKey.isEmpty() ? juce::String() : " (or " + swellKey + ")") + " to push.";
    m_sea.setTooltip(swellKey.isEmpty() ? juce::String("The Sea\nClick it to swell every track up.")
                                        : "The Sea\nClick it (or press " + swellKey + ") to swell every track up.");
    m_content.repaint();
}

//==============================================================================
bool SwooshbucklerAudioProcessorEditor::isTyping()
{
    return dynamic_cast<juce::TextEditor*>(juce::Component::getCurrentlyFocusedComponent()) != nullptr;
}

bool SwooshbucklerAudioProcessorEditor::canDoAction(Shortcuts::Action action) const
{
    using A = Shortcuts::Action;
    switch (action)
    {
        // The session files are the app's own: in a host, the host owns those keys and that menu.
        case A::NewSession: case A::OpenSession: case A::SaveSession: case A::SaveSessionAs:
            return ! m_processor.canFollowHost();
        case A::Undo:         return m_processor.getHistory().canUndo() || ! m_morph.items.empty();
        case A::Redo:         return m_processor.getHistory().canRedo();
        case A::PreviousRoll: return m_processor.canStepRoll(-1);
        case A::NextRoll:     return m_processor.canStepRoll(1);
        default:              return true;
    }
}

void SwooshbucklerAudioProcessorEditor::doAction(Shortcuts::Action action)
{
    using A = Shortcuts::Action;
    if (! canDoAction(action))
        return;
    switch (action)
    {
        case A::PlayStop:       m_processor.setPlaying(! m_processor.isPlaying()); break;
        case A::Throw:          m_processor.throwWhoosh(); break;
        case A::Swell:          break; // held, not pressed: keyPressed and keyStateChanged do it
        case A::ArmTape:        toggleLog(); break;
        case A::SaveLast:       saveRetro(); break;
        case A::Randomise:      rollDice(-1); break;
        case A::PreviousRoll:   stepRoll(-1); break;
        case A::NextRoll:       stepRoll(1); break;
        case A::KeepRoll:       keepRoll(); break;
        case A::PreviousPreset: stepPreset(-1); break;
        case A::NextPreset:     stepPreset(1); break;
        case A::Undo:           stepHistory(false); break;
        case A::Redo:           stepHistory(true); break;
        case A::NewSession:     if (m_newButton.onClick) m_newButton.onClick(); break;
        case A::OpenSession:    openSession(); break;
        case A::SaveSession:    saveSession(false); break;
        case A::SaveSessionAs:  saveSession(true); break;
        case A::Takes:          showTakes(); break;
        case A::Variations:     showVariations(); break;
        case A::Settings:       m_settings.refresh(); m_settings.setVisible(true); m_settings.toFront(false); break;
        case A::MorphPad:       m_morphButton.setToggleState(! m_morphButton.getToggleState(), juce::sendNotification); break;
        case A::Cables:         m_cablesButton.setToggleState(! m_cablesButton.getToggleState(), juce::sendNotification); break;
        case A::Count:          break;
    }
}

bool SwooshbucklerAudioProcessorEditor::keyPressed(const juce::KeyPress& key)
{
    // Nothing fires while a value is being typed, while a key is being given a new action, or through a panel
    // that is open over the rig (its own keys belong to it).
    const bool panelOpen = m_settings.isVisible() || m_about.isVisible() || m_takes.isVisible()
                           || m_variations.isVisible() || m_updatePopup.isVisible();
    if (! isTyping() && ! m_settings.keys().isListening())
    {
        const auto action = m_keys.find(key);
        if (action == Shortcuts::Action::Swell)
        {
            if (! m_spaceHeld)
            {
                m_spaceHeld = true;
                m_processor.setSwell(true);
                m_swell.press(true);
            }
            return true;
        }
        if (action != Shortcuts::Action::Count)
        {
            // A key with a modifier works wherever the focus is; a bare one only over the rig itself, so a
            // single letter cannot act through an open panel.
            const bool bare = ! Shortcuts::normalise(key).getModifiers().isAnyModifierKeyDown();
            if (! bare || ! panelOpen)
            {
                doAction(action);
                return true;
            }
        }
    }
    if (key == juce::KeyPress::escapeKey && m_updatePopup.isVisible()) // when the popup didn't get the focus
    {
        m_updatePopup.setVisible(false);
        return true;
    }
    if (key == juce::KeyPress::escapeKey && (m_settings.isVisible() || m_about.isVisible() || m_takes.isVisible() || m_variations.isVisible()))
    {
        m_settings.keys().stopListening();
        m_settings.setVisible(false);
        m_about.setVisible(false);
        m_takes.setVisible(false);
        m_variations.setVisible(false); // a batch that is rendering carries on
        return true;
    }
    return false;
}

bool SwooshbucklerAudioProcessorEditor::keyStateChanged(bool)
{
    const auto swellKey = m_keys.key(Shortcuts::Action::Swell);
    if (m_spaceHeld && (! swellKey.isValid() || ! juce::KeyPress::isKeyCurrentlyDown(swellKey.getKeyCode())))
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
        if (CargoHoldView::isAudioFile(f) || f.endsWithIgnoreCase(UserPresets::extension)
            || f.endsWithIgnoreCase(SwooshbucklerAudioProcessor::c_sessionExtension))
            return true;
    return false;
}

void SwooshbucklerAudioProcessorEditor::filesDropped(const juce::StringArray& files, int, int)
{
    // A session file opens; a preset file loads (and joins your presets); sounds go into Tracks.
    for (const auto& f : files)
        if (f.endsWithIgnoreCase(SwooshbucklerAudioProcessor::c_sessionExtension))
        {
            openSessionFile(juce::File(f));
            return;
        }
    for (const auto& f : files)
        if (f.endsWithIgnoreCase(UserPresets::extension))
        {
            loadPresetFile(juce::File(f), true);
            return;
        }
    const auto text = m_hold.loadFiles(files);
    if (text.isNotEmpty())
        toast(text);
}

void SwooshbucklerAudioProcessorEditor::toast(const juce::String& text, juce::File file)
{
    // A saved take is said in the Tracks title bar, beside the lanes it came from; a click there opens Takes.
    if (file != juce::File())
    {
        m_hold.showTakeNote(text);
        return;
    }
    m_toast = Theme::localiseShortcuts(text);
    m_toastTime = juce::Time::getMillisecondCounterHiRes() * 0.001;
    // The plate rises into place and fades in; the footer keeps the same line as a log.
    m_toastRise.snap(0.0f);
    m_toastRise.to(1.0f, Motion::Ms::overlayIn);
    placeToast(m_sea.getBounds());
    m_content.repaint(m_footer);
    m_toastPlate.repaint();
}

void SwooshbucklerAudioProcessorEditor::ToastPlate::paint(juce::Graphics& g)
{
    // It rises eight pixels into place and fades, and it lies over everything the content draws.
    if (owner.m_toast.isEmpty() || owner.m_toastRise.value <= 0.004f)
        return;
    const float a = juce::jlimit(0.0f, 1.0f, owner.m_toastRise.value);
    const auto plate = getLocalBounds().toFloat().reduced(2.0f, 10.0f).translated(0.0f, 8.0f * (1.0f - a));
    g.setColour(TC::shadow.withAlpha(0.45f * a));
    Theme::Plate::fill(g, plate.translated(0.0f, 2.0f), Theme::Cut::control);
    g.setColour(TC::raised.withAlpha(0.97f * a));
    Theme::Plate::fill(g, plate, Theme::Cut::control);
    g.setColour(TC::brass.withAlpha(0.8f * a));
    Theme::Plate::stroke(g, plate, Theme::Cut::control);
    Theme::stampText(g, owner.m_toast, plate.reduced(13.0f, 2.0f), owner.m_fonts->body(), juce::Justification::centred,
                     TC::text.withAlpha(a), Theme::Fit::Message);
}

void SwooshbucklerAudioProcessorEditor::placeToast(juce::Rectangle<int> near)
{
    // A plate at the top of the open water: one place for everything the rig says back, in sight of the header
    // the action came from and clear of every panel's own lettering.
    constexpr int height = 26, margin = 14;
    if (near.isEmpty())
        near = { 0, 0, c_width, c_height }; // before the window has been laid out
    const int widest = juce::jmax(160, juce::jmin(560, near.getWidth() - 2 * margin));
    const int width = juce::jlimit(140, widest, juce::roundToInt(Theme::textWidth(m_fonts->body(), m_toast)) + 30);
    m_toastPill = juce::Rectangle<int>(near.getCentreX() - width / 2, near.getY() + margin, width, height);
    m_toastPlate.setBounds(m_toastPill.expanded(2, 10)); // room for the rise and the shadow under it
}

void SwooshbucklerAudioProcessorEditor::showTakes()
{
    m_settings.setVisible(false);
    m_about.setVisible(false);
    m_variations.setVisible(false);
    m_processor.refreshTakes();
    m_takes.setVisible(true);
    m_takes.toFront(false);
}

void SwooshbucklerAudioProcessorEditor::showVariations()
{
    m_settings.setVisible(false);
    m_about.setVisible(false);
    m_takes.setVisible(false);
    m_variations.setVisible(true);
    m_variations.toFront(false);
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
    m_tape.rewind(); // the reels flick back: the rig is going back over what has just been played
    juce::File written;
    juce::String error;
    if (m_processor.saveRetro(written, error))
        toast("Saved " + written.getFileName(), written);
    else
        toast(error);
}

//==============================================================================
std::vector<SwooshbucklerAudioProcessorEditor::PresetItem> SwooshbucklerAudioProcessorEditor::presetList() const
{
    std::vector<PresetItem> items;
    const auto& factory = Presets::factory();
    for (size_t i = 0; i < factory.size(); ++i)
        items.push_back({ factory[i].name, static_cast<int>(i), {} });
    for (const auto& file : m_processor.getUserPresets())
        items.push_back({ file.getFileNameWithoutExtension(), -1, file });
    return items;
}

namespace
{
    enum PresetMenuItem { itemDefaults = 1000, itemSave, itemRename, itemRemove, itemShowFolder, itemSwitchAB, itemCopyAB, itemSaveSession, itemSaveSessionAs, itemOpenSession, itemClearRecent, itemFirstRecent = 1100 };
}

juce::PopupMenu SwooshbucklerAudioProcessorEditor::buildPresetMenu(const std::vector<PresetItem>& items, juce::File& currentFile,
                                                                   juce::Array<juce::File>& recent) const
{
    const auto current = m_processor.currentPresetName;
    currentFile = juce::File(); // the preset of yours that is loaded, if one is
    juce::PopupMenu menu;
    menu.addSectionHeader("Factory Presets");
    bool userHeader = false;
    for (size_t i = 0; i < items.size(); ++i)
    {
        if (items[i].factory < 0 && ! userHeader)
        {
            menu.addSectionHeader("Your Presets");
            userHeader = true;
        }
        const bool ticked = items[i].name.equalsIgnoreCase(current);
        if (ticked && items[i].factory < 0)
            currentFile = items[i].file;
        menu.addItem(static_cast<int>(i) + 1, items[i].name, true, ticked);
    }
    menu.addSeparator();
    menu.addItem(itemSave, "Save Preset...");
    menu.addItem(itemRename, "Rename Preset...", currentFile != juce::File());
    menu.addItem(itemRemove, "Delete Preset", currentFile != juce::File());
    menu.addItem(itemShowFolder, "Show Preset Folder");
    menu.addItem(itemDefaults, "Back to Defaults");
    menu.addSectionHeader("A / B");
    menu.addItem(itemSwitchAB, m_processor.getABSlot() == 0 ? "Switch to B" : "Switch to A");
    menu.addItem(itemCopyAB, "Copy to Other");
    menu.addSectionHeader("Session");
    const auto session = m_processor.getSessionFile();
    // Each one shows its key at the right of the row, from the registry, so a key that has been changed reads
    // the same here as it does in Settings and in the menu bar.
    {
        juce::PopupMenu::Item save(session != juce::File() ? "Save Session \"" + session.getFileNameWithoutExtension() + "\"" : juce::String("Save Session"));
        save.itemID = itemSaveSession;
        save.shortcutKeyDescription = m_keys.keyText(Shortcuts::Action::SaveSession);
        menu.addItem(save);
    }
    m_keys.addToMenu(menu, Shortcuts::Action::SaveSessionAs, itemSaveSessionAs);
    m_keys.addToMenu(menu, Shortcuts::Action::OpenSession, itemOpenSession);
    recent = m_processor.getRecentSessions();
    {
        juce::PopupMenu sub;
        for (int i = 0; i < recent.size(); ++i)
            sub.addItem(itemFirstRecent + i, recent[i].getFileNameWithoutExtension(), true, recent[i] == session);
        sub.addSeparator();
        sub.addItem(itemClearRecent, "Clear Recent Sessions");
        menu.addSubMenu("Recent Sessions", sub, ! recent.isEmpty());
    }
    return menu;
}

void SwooshbucklerAudioProcessorEditor::showPresetMenu()
{
    const auto items = presetList();
    juce::File currentFile;
    juce::Array<juce::File> recent;
    auto menu = buildPresetMenu(items, currentFile, recent);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(m_presetName),
                       [safe = juce::Component::SafePointer<SwooshbucklerAudioProcessorEditor>(this), items, currentFile, recent](int result)
    {
        if (safe == nullptr || result <= 0)
            return;
        auto& self = *safe;
        if (result >= itemFirstRecent)
        {
            if (result - itemFirstRecent < recent.size())
                self.openSessionFile(recent[result - itemFirstRecent]);
            return;
        }
        switch (result)
        {
            case itemDefaults:        self.applyLook({ "Default", {} }, Presets::Unlisted::Default, "Back to Defaults", "Default"); break;
            case itemSave:            self.savePresetAs(); break;
            case itemRename:          self.renamePreset(currentFile); break;
            case itemRemove:          self.deletePreset(currentFile); break;
            case itemShowFolder:
            {
                const auto folder = self.m_processor.getPresetFolder();
                folder.createDirectory();
                folder.revealToUser();
                break;
            }
            case itemSwitchAB:        self.m_abButton.onClick(); break;
            case itemCopyAB:
                self.m_morph.finish();
                self.m_processor.copyABToOther();
                self.toast(self.m_processor.getABSlot() == 0 ? "Copied A to B" : "Copied B to A");
                break;
            case itemSaveSession: self.saveSession(false); break;
            case itemSaveSessionAs:   self.saveSession(true); break;
            case itemOpenSession: self.openSession(); break;
            case itemClearRecent:     self.m_processor.clearRecentSessions(); break;
            default:
                if (result - 1 < static_cast<int>(items.size()))
                    self.choosePreset(items[static_cast<size_t>(result - 1)]);
                break;
        }
    });
}

void SwooshbucklerAudioProcessorEditor::choosePreset(const PresetItem& item)
{
    if (item.factory >= 0)
    {
        const auto& preset = Presets::factory()[static_cast<size_t>(item.factory)];
        applyLook(preset, Presets::Unlisted::Default, preset.name, preset.name);
    }
    else
        loadPresetFile(item.file, false);
}

void SwooshbucklerAudioProcessorEditor::stepPreset(int delta)
{
    const auto items = presetList();
    const int count = static_cast<int>(items.size());
    int at = -1;
    for (int i = 0; i < count; ++i)
        if (items[static_cast<size_t>(i)].name.equalsIgnoreCase(m_processor.currentPresetName))
            at = i;
    choosePreset(items[static_cast<size_t>(((at < 0 ? (delta > 0 ? -1 : 0) : at) + delta + count) % count)]);
}

void SwooshbucklerAudioProcessorEditor::loadPresetFile(const juce::File& file, bool addToPresets)
{
    UserPresets::Loaded loaded;
    juce::String error;
    if (! UserPresets::read(file, loaded, error))
    {
        toast(error);
        return;
    }
    // A file dropped on the window joins your presets (unless one of that name is there already).
    juce::String added;
    if (addToPresets && file.getParentDirectory() != m_processor.getPresetFolder())
    {
        const auto copy = UserPresets::fileFor(m_processor.getPresetFolder(), loaded.preset.name);
        if (! copy.existsAsFile() && ! UserPresets::isFactoryName(loaded.preset.name)
            && m_processor.getPresetFolder().createDirectory() && file.copyFileTo(copy))
            added = " and added it to your presets";
    }
    m_morph.finish();
    {
        EditHistory::Group group(m_processor.getHistory(), loaded.preset.name);
        m_processor.applyPresetWires(loaded);
        applyLook(loaded.preset, Presets::Unlisted::Default, loaded.preset.name, loaded.preset.name);
    }
    if (addToPresets)
        toast("Loaded " + loaded.preset.name + added);
}

juce::AlertWindow* SwooshbucklerAudioProcessorEditor::makeNameDialog(const juce::String& title, const juce::String& message,
                                                                     const juce::String& initial, const juce::String& okText, bool modsOption)
{
    auto* window = new juce::AlertWindow(title, message, juce::MessageBoxIconType::NoIcon, this);
    window->addTextEditor("name", initial);
    if (auto* editor = window->getTextEditor("name"))
        editor->setInputRestrictions(UserPresets::maxNameLength);
    m_dialogToggle.reset();
    if (modsOption)
    {
        m_dialogToggle = std::make_unique<juce::ToggleButton>("Include Modulators and Wires");
        m_dialogToggle->setName({}); // or the window prints the name above it as a caption
        m_dialogToggle->setColour(juce::ToggleButton::tickColourId, TC::brass);
        m_dialogToggle->setTooltip("Include Modulators and Wires\nOn: the preset also holds the four modulators' settings and every wire, and loading it "
                                   "replaces yours. Off: the preset leaves your modulators and wires alone.");
        m_dialogToggle->setSize(280, 26);
        window->addCustomComponent(m_dialogToggle.get());
    }
    window->addButton(okText, 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    return window;
}

void SwooshbucklerAudioProcessorEditor::askForName(const juce::String& title, const juce::String& message, const juce::String& initial,
                                                   const juce::String& okText, bool modsOption,
                                                   std::function<void(const juce::String&, bool)> done)
{
    auto* window = makeNameDialog(title, message, initial, okText, modsOption);
    window->enterModalState(true, juce::ModalCallbackFunction::create(
        [safe = juce::Component::SafePointer<SwooshbucklerAudioProcessorEditor>(this), window, done = std::move(done)](int result)
        {
            if (safe == nullptr)
                return;
            const auto name = window->getTextEditorContents("name").trim();
            const bool mods = safe->m_dialogToggle != nullptr && safe->m_dialogToggle->getToggleState();
            window->removeCustomComponent(0);
            safe->m_dialogToggle.reset();
            safe->grabKeyboardFocus(); // the keys go back to the rig
            if (result == 1 && name.isNotEmpty() && done)
                done(name, mods);
        }), true);
}

void SwooshbucklerAudioProcessorEditor::savePresetAs()
{
    const auto current = m_processor.currentPresetName;
    const auto suggestion = UserPresets::isFactoryName(current) ? UserPresets::uniqueName(m_processor.getPresetFolder(), "My Preset") : current;
    askForName("Save Preset", "The settings as they are now are saved under this name. A preset of yours with the same name is replaced.",
               suggestion, "Save", true, [this](const juce::String& name, bool withMods)
    {
        m_morph.finish(); // what is saved is where a glide was heading
        juce::File written;
        juce::String error;
        if (m_processor.saveUserPreset(name, withMods, written, error))
            toast("Saved the preset " + written.getFileNameWithoutExtension() + (withMods ? " with its modulators and wires" : juce::String()));
        else
            toast(error);
    });
}

void SwooshbucklerAudioProcessorEditor::renamePreset(const juce::File& file)
{
    if (! file.existsAsFile())
        return;
    askForName("Rename Preset", "A new name for \"" + file.getFileNameWithoutExtension() + "\".", file.getFileNameWithoutExtension(), "Rename", false,
               [this, file](const juce::String& name, bool)
    {
        juce::File renamed;
        juce::String error;
        if (! UserPresets::rename(file, name, renamed, error))
        {
            toast(error);
            return;
        }
        if (m_processor.currentPresetName.equalsIgnoreCase(file.getFileNameWithoutExtension()))
            m_processor.currentPresetName = renamed.getFileNameWithoutExtension();
        toast("Renamed to " + renamed.getFileNameWithoutExtension());
    });
}

void SwooshbucklerAudioProcessorEditor::deletePreset(const juce::File& file)
{
    if (! file.existsAsFile())
        return;
    // To the system trash, so a slip can be put right there.
    if (file.moveToTrash())
        toast("Moved the preset " + file.getFileNameWithoutExtension() + " to the trash");
    else
        toast("Couldn't delete " + file.getFileName());
}

void SwooshbucklerAudioProcessorEditor::applyLook(const Presets::Preset& look, Presets::Unlisted unlisted, const juce::String& undoName,
                                                  const juce::String& presetName, bool outputStage)
{
    m_morph.finish();
    // One undo step: the gestures begun here stay open while the knobs glide, and the step is closed when the
    // last of them lands.
    EditHistory::Group group(m_processor.getHistory(), undoName);
    if (presetName.isNotEmpty())
        m_processor.setPresetName(presetName);
    for (const auto& [param, target] : Presets::targets(m_processor.getParameters(), look, unlisted, outputStage))
    {
        if (std::abs(param->getValue() - target) < 1.0e-5f)
            continue;
        param->beginChangeGesture();
        m_morph.items.push_back({ param, param->getValue(), 0.0f, target, 0.0f });
    }
    stagger();

    // A preset never moves a track. One that switches Stowaway on with nothing there to play says so, or it would
    // look like a preset that does nothing: Stowaway only sounds once a track is sent to it.
    if (const auto on = look.values.find(ParamIDs::stowOn); presetName.isNotEmpty() && on != look.values.end() && on->second > 0.5f)
    {
        const auto mode = look.values.find(ParamIDs::stowMode);
        const bool sub = mode == look.values.end() || juce::roundToInt(mode->second) == ParamIDs::StowSub;
        const auto& hold = m_processor.getCargo();
        bool any = false;
        for (int t = 0; t < ParamIDs::numCargoTracks; ++t)
            if (hold.get(t) != nullptr)
                any = any || hold.getBusOverride(t) == CargoHold::c_stowBus
                      || (hold.getBusOverride(t) < 0 && sub && hold.getRole(t) == CargoRole::Rumble); // (a rumble goes to a sub by itself)
        if (! any)
            toast(sub ? "Stowaway is on with nothing on it: send it a track, or load a Rumble"
                      : "Stowaway is on with nothing on it: send it a track from a lane's bus tag");
    }
}

void SwooshbucklerAudioProcessorEditor::stagger()
{
    // The wave runs the way the eye reads the window: down the rows, and left to right along each. A control
    // that is not on screen (or has no knob) sets off at once.
    std::vector<std::pair<int, size_t>> order; // where the knob is, and which item it is
    order.reserve(m_morph.items.size());
    for (size_t i = 0; i < m_morph.items.size(); ++i)
    {
        const auto* param = m_morph.items[i].param;
        int place = -1;
        if (param != nullptr)
            if (const auto found = m_controls.find(param->getParameterID()); found != m_controls.end() && found->second != nullptr)
            {
                const auto bounds = found->second->getBounds();
                // A row is 80 design pixels deep: knobs side by side are in the same one.
                place = (bounds.getCentreY() / 80) * 10000 + bounds.getCentreX();
            }
        order.push_back({ place, i });
    }
    std::sort(order.begin(), order.end());
    int step = 0;
    for (const auto& [place, index] : order)
        m_morph.items[index].delay = place < 0 ? 0.0f
                                               : juce::jmin(Morph::c_wave, static_cast<float>(step++) * Morph::c_waveStep);
}

void SwooshbucklerAudioProcessorEditor::rollDice(int section)
{
    m_morph.finish();
    // The die tumbles: three faces, a tenth of a second each, then it settles on five again. The label does
    // not move - only the face the die shows changes.
    m_diceTime = juce::Time::getMillisecondCounterHiRes() * 0.001;
    m_diceStep = 0;
    m_diceRolling = section < 0 ? static_cast<juce::Component*>(&m_dice) : m_sectionDice[static_cast<size_t>(section)].get();
    Icons::setPips(*m_diceRolling, 1 + juce::Random::getSystemRandom().nextInt(6));
    const juce::String name = section < 0 ? juce::String("Randomise") : "Roll " + juce::String(Presets::sectionName(section));
    EditHistory::Group group(m_processor.getHistory(), name);
    const auto roll = m_processor.rollLook(section);
    applyLook(roll, Presets::Unlisted::Keep, name, "Randomised", true);
    if (roll.values.empty())
        toast("Every section is locked: nothing to roll");
}

void SwooshbucklerAudioProcessorEditor::stepRoll(int delta)
{
    m_morph.finish();
    if (! m_processor.canStepRoll(delta))
        return;
    const juce::String name = delta < 0 ? "Previous Roll" : "Next Roll";
    EditHistory::Group group(m_processor.getHistory(), name);
    if (const auto* look = m_processor.stepRoll(delta))
    {
        applyLook(*look, Presets::Unlisted::Keep, name, look->name, true);
        const int at = m_processor.getRollPosition();
        toast(at == 0 ? juce::String("Before the first roll") : "Roll " + juce::String(at) + " of " + juce::String(m_processor.getRollCount()));
    }
}

void SwooshbucklerAudioProcessorEditor::keepRoll()
{
    m_morph.finish();
    juce::File written;
    juce::String error;
    if (m_processor.saveUserPreset(UserPresets::uniqueName(m_processor.getPresetFolder(), "Roll"), false, written, error))
        toast("Kept as the preset " + written.getFileNameWithoutExtension() + ". Rename it in the preset list.");
    else
        toast(error);
}

void SwooshbucklerAudioProcessorEditor::stepHistory(bool redo)
{
    m_morph.finish(); // a glide still moving is the step to take back
    juce::String what;
    const bool done = redo ? m_processor.redoEdit(&what) : m_processor.undoEdit(&what);
    m_hold.historyStepped();
    if (done)
        toast((redo ? "Redo: " : "Undo: ") + what);
    else
        toast(redo ? "Nothing to redo" : "Nothing to undo");
}

//==============================================================================
void SwooshbucklerAudioProcessorEditor::saveSession(bool saveAs)
{
    const auto current = m_processor.getSessionFile();
    auto write = [this](const juce::File& file)
    {
        m_morph.finish();
        juce::String error;
        if (m_processor.saveSession(file, error))
            toast("Saved the session " + m_processor.getSessionFile().getFileNameWithoutExtension());
        else
            toast(error);
    };
    if (! saveAs && current != juce::File())
    {
        write(current);
        return;
    }
    const auto start = current != juce::File() ? current
                                               : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Swooshbuckler Session");
    m_sessionChooser = std::make_unique<juce::FileChooser>("Save Session As", start, juce::String("*") + SwooshbucklerAudioProcessor::c_sessionExtension);
    m_sessionChooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                                  [write](const juce::FileChooser& chooser)
                                  {
                                      if (chooser.getResult() != juce::File())
                                          write(chooser.getResult());
                                  });
}

void SwooshbucklerAudioProcessorEditor::openSession()
{
    const auto current = m_processor.getSessionFile();
    const auto start = current != juce::File() ? current.getParentDirectory() : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    m_sessionChooser = std::make_unique<juce::FileChooser>("Open Session", start, juce::String("*") + SwooshbucklerAudioProcessor::c_sessionExtension);
    m_sessionChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this](const juce::FileChooser& chooser)
                                  {
                                      if (chooser.getResult() != juce::File())
                                          openSessionFile(chooser.getResult());
                                  });
}

void SwooshbucklerAudioProcessorEditor::openSessionFile(const juce::File& file)
{
    m_morph.finish();
    juce::String error;
    if (m_processor.openSession(file, error))
        toast("Opened the session " + file.getFileNameWithoutExtension());
    else
        toast(error);
}

//==============================================================================
void SwooshbucklerAudioProcessorEditor::syncMorphPad()
{
    auto& st = m_processor.getState();
    auto value = [&st](const char* id) { return st.getRawParameterValue(id)->load(); };
    // The MORPH button shows the pad; ON on the pad switches the morph itself.
    const bool shown = m_morphButton.getToggleState();
    const bool on = value(ParamIDs::morphOn) > 0.5f;
    m_morphButton.setButtonText(on ? "Morph On" : "Morph");
    m_morphButton.setColour(juce::TextButton::buttonOnColourId, on ? TC::go : TC::filter);
    if (on) m_morphButton.setColour(juce::TextButton::textColourOffId, TC::go);
    else    m_morphButton.removeColour(juce::TextButton::textColourOffId);
    m_sea.setLabelsShown(! shown);
    // The pad is docked over the sea's lower right, not spread over all of it: the sea behind is dimmed so the
    // pad reads, and the sky, the moon and the comets stay in view.
    m_sea.setDimmedUnder(shown ? m_sea.getLocalArea(&m_morphPad, m_morphPad.getLocalBounds()) : juce::Rectangle<int>());
    if (m_morphPad.isVisible() != shown)
    {
        m_morphPad.setVisible(shown);
        if (shown)
            m_morphPad.toFront(false);
        m_morphButton.toFront(false);
    }
    m_morphPad.setMorphEnabled(on);
    {
        // The ropes: each side's track (or its bus).
        const auto& cargo = m_processor.getCargo();
        const int a = cargo.getMorphSource(0), b = cargo.getMorphSource(1);
        m_morphPad.setSources(a, b, a >= 0 ? cargo.getDisplayName(a) : juce::String(), b >= 0 ? cargo.getDisplayName(b) : juce::String());
    }
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
        if (c != nullptr)
            Theme::setLook(*c, off ? Theme::Look::Disabled : Theme::Look::Normal);
    };
    dim(&m_gateMode, aim);
    dim(&m_syncHits, aim);
    for (const auto* id : { ParamIDs::hitRelease, ParamIDs::hitHold, ParamIDs::hitFloor })
        dim(control(id), aim || open);
    dim(control(ParamIDs::hitSense), open);
    if (auto* energy = control(ParamIDs::energy))
        Theme::setLook(*energy, ride ? Theme::Look::Normal : Theme::Look::Resting); // still usable, so it can be set before switching the ride on
    m_content.repaint(m_gateLamp.expanded(10));
}

void SwooshbucklerAudioProcessorEditor::updateMeterReadouts()
{
    // The numbers under the meters, written when they change (not at every repaint): one decimal, in dB.
    const int gr = juce::roundToInt(m_grHoldDb * 10.0f);
    const int out = juce::roundToInt(juce::Decibels::gainToDecibels(m_outHold, -60.0f) * 10.0f);
    if (gr != m_grReadoutTenths || m_grReadout.isEmpty())
    {
        m_grReadoutTenths = gr;
        m_grReadout = juce::String(static_cast<float>(gr) * 0.1f, 1);
    }
    if (out != m_outReadoutTenths || m_outReadout.isEmpty())
    {
        m_outReadoutTenths = out;
        m_outReadout = out <= -600 ? juce::String("-60") : juce::String(static_cast<float>(out) * 0.1f, 1);
    }
}

void SwooshbucklerAudioProcessorEditor::showLinkedKnobs()
{
    // Linked, bus B's Doppler and Width follow bus A: they show a chain link in A's colour (and keep their own
    // values for when it's unlinked).
    const bool linked = m_processor.getState().getRawParameterValue(ParamIDs::linkBuses)->load() > 0.5f;
    if (m_linkShown == (linked ? 1 : 0))
        return;
    m_linkShown = linked ? 1 : 0;
    for (const int k : { static_cast<int>(ParamIDs::Doppler), static_cast<int>(ParamIDs::Width) })
    {
        const auto it = m_controls.find(juce::String(ParamIDs::bus[1][k]));
        if (it == m_controls.end() || it->second == nullptr)
            continue;
        if (auto* knob = dynamic_cast<WheelKnob*>(it->second))
        {
            knob->setLinkColour(TC::busA);
            knob->setLinkText("Linked To A");
        }
        Theme::setLook(*it->second, linked ? Theme::Look::Linked : Theme::Look::Normal);
    }
    // And B flies A's passes in A's Pass Mode.
    Theme::setLook(m_passMode[1], linked ? Theme::Look::Linked : Theme::Look::Normal);
}

void SwooshbucklerAudioProcessorEditor::timerCallback()
{
    // One frame for everything that moves: the real time since the last one, clamped, so a late frame catches
    // the interface up in one step instead of running it in slow motion.
    const double frameNow = juce::Time::getMillisecondCounterHiRes() * 0.001;
    const float dt = m_frameTime > 0.0 ? juce::jlimit(0.0f, Motion::c_maxStep, static_cast<float>(frameNow - m_frameTime))
                                       : 1.0f / 60.0f;
    m_frameTime = frameNow;
    m_clock.tick(dt);

    showLinkedKnobs();
    showAimControls();
    showStowaway(dt);
    {
        // Peak Tremolo off: its knobs and menus rest (hollow), but stay usable so it can be set up before switching it on.
        const bool on = m_processor.getState().getRawParameterValue(ParamIDs::tremOn)->load() > 0.5f;
        if (m_tremShown != (on ? 1 : 0))
        {
            m_tremShown = on ? 1 : 0;
            std::vector<juce::Component*> parts { &m_tremShape, &m_tremTarget };
            for (const auto* id : { ParamIDs::tremDepth, ParamIDs::tremRate, ParamIDs::tremThreshold })
                parts.push_back(m_controls[id]);
            for (auto* c : parts)
                Theme::setLook(*c, on ? Theme::Look::Normal : Theme::Look::Resting);
            m_content.repaint(m_tremGroup);
        }
    }
    if ((++m_morphSyncTick % 6) == 0)
        syncMorphPad();
    if ((m_morphSyncTick & 1) == 0)
        m_rideStrip->step();
    {
        // Follow Master rests (hollow, still usable) until Auto Ride is on and a track is the Master.
        const bool ready = m_processor.getState().getRawParameterValue(ParamIDs::rideOn)->load() > 0.5f && m_processor.getCargo().getMaster() >= 0;
        if (m_followMasterShown != (ready ? 1 : 0))
        {
            m_followMasterShown = ready ? 1 : 0;
            Theme::setLook(m_followMasterButton, ready ? Theme::Look::Normal : Theme::Look::Resting);
        }
    }

    if (! m_morph.items.empty())
        m_morph.step(dt);

    // The rolling die: a new face every tenth of a second for three tenths, then it settles on five.
    if (m_diceTime > 0.0 && m_diceRolling != nullptr)
    {
        const int step = static_cast<int>((juce::Time::getMillisecondCounterHiRes() * 0.001 - m_diceTime) / 0.1);
        if (step >= 3)
        {
            Icons::setPips(*m_diceRolling, 5);
            m_diceTime = 0.0;
            m_diceStep = -1;
            m_diceRolling = nullptr;
        }
        else if (step != m_diceStep)
        {
            m_diceStep = step;
            Icons::setPips(*m_diceRolling, 1 + juce::Random::getSystemRandom().nextInt(6));
        }
    }


    const bool playing = m_processor.isPlaying();
    if (m_playButton.getToggleState() != playing)
    {
        m_playButton.setToggleState(playing, juce::dontSendNotification);
        m_playButton.setButtonText(playing ? "Stop" : "Play");
        Icons::set(m_playButton, playing ? Icons::Icon::Stop : Icons::Icon::Play);
    }

    if (m_presetName.getButtonText() != m_processor.currentPresetName)
        m_presetName.setButtonText(m_processor.currentPresetName);

    // Undo, redo and the roll history light up when there is somewhere to go; A / B says which set you are on;
    // the locks follow the session.
    {
        auto& history = m_processor.getHistory();
        m_undoButton.setEnabled(history.canUndo() || ! m_morph.items.empty());
        m_redoButton.setEnabled(history.canRedo());
        m_rollBack.setEnabled(m_processor.canStepRoll(-1));
        m_rollForward.setEnabled(m_processor.canStepRoll(1));
        const char* slot = m_processor.getABSlot() == 0 ? "A" : "B";
        if (m_abButton.getButtonText() != slot)
            m_abButton.setButtonText(slot);
        m_abButton.setToggleState(m_processor.getABSlot() == 1, juce::dontSendNotification);
        for (int section = 0; section < Presets::numSections; ++section)
            m_sectionLocks[static_cast<size_t>(section)]->setToggleState(m_processor.isSectionLocked(section), juce::dontSendNotification);
    }

    // The LAST button says how much it keeps, Takes how many there are; a whoosh file closed while the tape rolls
    // is announced, and so is a file the tape could not open.
    if (const int minutes = m_processor.getRetroMinutes(); minutes != m_retroShown)
    {
        m_retroShown = minutes;
        m_retroButton.setButtonText(minutes == 1 ? juce::String("Last 60 s") : "Last " + juce::String(minutes) + " Min");
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
    // Render Variations: the header button shows how far a batch is, and says so when it has ended with the panel closed.
    {
        const auto progress = m_processor.getVariations().getProgress();
        const bool rendering = progress.state == VariationRenderer::State::Rendering;
        const int percent = rendering ? juce::roundToInt(100.0f * static_cast<float>(progress.done + progress.silent) / static_cast<float>(juce::jmax(1, progress.total))) : -1;
        if (percent != m_variationsShown)
        {
            if (m_variationsShown >= 0 && ! rendering && ! m_variations.isVisible())
            {
                if (progress.state == VariationRenderer::State::Finished)
                    toast("Rendered " + juce::String(progress.done) + (progress.done == 1 ? " variation into " : " variations into ") + progress.folder.getFileName());
                else if (progress.state == VariationRenderer::State::Failed)
                    toast("Render Variations stopped: " + progress.error);
            }
            m_variationsShown = percent;
            m_variationsButton.setButtonText(rendering ? "Render " + juce::String(percent) + "%" : juce::String("Variations"));
            m_variationsButton.setToggleState(rendering, juce::dontSendNotification);
        }
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
            updateMeterReadouts();
            m_content.repaint(m_meterArea);
        }
    }

    if (m_toast.isNotEmpty())
    {
        const double age = juce::Time::getMillisecondCounterHiRes() * 0.001 - m_toastTime;
        // The plate holds for two and a half seconds and fades; the footer keeps the line for eight.
        if (age > 2.5 && m_toastRise.target() > 0.5f)
            m_toastRise.to(0.0f, Motion::Ms::overlayOut);
        if (m_toastRise.step(dt))
            m_toastPlate.repaint();
        if (age > 8.0)
            m_toast.clear();
        m_content.repaint(m_footer);
    }
}
