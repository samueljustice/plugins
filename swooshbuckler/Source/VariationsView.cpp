#include "VariationsView.h"
#include "LookAndFeel.h"

namespace TC = Theme::Colours;

namespace
{
    using Mode = VariationRenderer::Mode;
    using Each = VariationRenderer::Each;
    using Names = VariationRenderer::Names;
    using Files = VariationRenderer::Files;
    using State = VariationRenderer::State;

    /** A menu item with what it gives beside it, quietly (where a key would be in the other menus). */
    void addChoice(juce::PopupMenu& menu, int id, const juce::String& text, const juce::String& gives, bool enabled = true)
    {
        juce::PopupMenu::Item item(text);
        item.setID(id);
        item.setEnabled(enabled);
        item.shortcutKeyDescription = gives;
        menu.addItem(item);
    }

    juce::String oneDecimal(double value)
    {
        const juce::String text(value, 1);
        return text.endsWith(".0") ? text.dropLastCharacters(2) : text;
    }

    /** A path that fits: its end is what tells folders apart, so the front is what gives way. */
    juce::String fitPath(const juce::Font& font, const juce::String& path, int width)
    {
        if (juce::GlyphArrangement::getStringWidthInt(font, path) <= width)
            return path;
        for (int cut = 1; cut < path.length(); ++cut)
        {
            const auto shorter = "..." + path.substring(cut);
            if (juce::GlyphArrangement::getStringWidthInt(font, shorter) <= width)
                return shorter;
        }
        return "...";
    }

    juce::String timeLeft(double seconds)
    {
        if (seconds < 0.0)
            return "Working out how long is left";
        if (seconds < 1.5)
            return "Nearly done";
        if (seconds < 90.0)
            return "About " + juce::String(static_cast<int>(std::floor(seconds + 0.5))) + " s left";
        return "About " + juce::String(static_cast<int>(std::floor(seconds / 60.0 + 0.5))) + " min left"; // halves round up, as in the estimate
    }
}

juce::String VariationsView::showButtonText()
{
   #if JUCE_MAC
    return "Show in Finder";
   #elif JUCE_WINDOWS
    return "Show in Explorer";
   #else
    return "Show Folder";
   #endif
}

VariationsView::VariationsView(SwooshbucklerAudioProcessor& processor, const Theme::Fonts& fonts)
    : m_processor(processor), m_fonts(fonts)
{
    m_show.setButtonText(showButtonText());
    for (auto* b : { &m_choose, &m_dated, &m_render, &m_cancel, &m_show, &m_openTakes, &m_close })
    {
        b->setColour(juce::TextButton::buttonOnColourId, TC::flight);
        b->setWantsKeyboardFocus(false);
        addAndMakeVisible(*b);
    }
    SwooshLookAndFeel::setStyle(m_render, SwooshLookAndFeel::ButtonStyle::Primary); // the main action

    const char* restrictions[] = { "0123456789", "0123456789.", "0123456789.", "" };
    const int lengths[] = { 4, 5, 4, 40 };
    juce::TextEditor* editors[] = { &m_count, &m_length, &m_gap, &m_batchName };
    for (int i = 0; i < 4; ++i)
    {
        auto& e = *editors[i];
        e.setFont(m_fonts.body());
        e.setInputRestrictions(lengths[i], restrictions[i]);
        e.setSelectAllWhenFocused(true);
        e.setJustification(juce::Justification::centredLeft);
        // Return and Escape hand the keyboard back to the rig (space for Swell, F for Throw).
        e.onReturnKey = [this] { if (onKeyboardDone) onKeyboardDone(); };
        e.onEscapeKey = [this] { if (onKeyboardDone) onKeyboardDone(); };
        addAndMakeVisible(e);
    }
    m_count.setTooltip("How Many\nHow many variations to make, from 1 to 1000.");
    m_count.onTextChange = [this]
    {
        if (const int count = m_count.getText().getIntValue(); count >= 1)
            m_processor.setVariationCount(count);
        syncControls();
    };
    m_count.onFocusLost = [this] { m_count.setText(juce::String(job().count), false); syncControls(); };
    m_length.onTextChange = [this]
    {
        if (const double seconds = m_length.getText().getDoubleValue(); seconds > 0.0)
            (m_each.getSelectedId() == 2 ? m_rideSeconds : m_whooshSeconds) = seconds;
        syncControls();
    };
    m_length.onFocusLost = [this]
    {
        const auto j = job();
        m_whooshSeconds = j.seconds;
        m_rideSeconds = j.rideSeconds;
        m_length.setText(oneDecimal(j.each == Each::WholeRide ? j.rideSeconds : j.seconds), false);
        keepChoices();
        syncControls();
    };
    m_gap.setTooltip("Gap\nThe silence between one variation and the next in the one long file, from 0 to 10 seconds.");
    m_gap.onTextChange = [this] { syncControls(); };
    m_gap.onFocusLost = [this] { m_gap.setText(oneDecimal(job().gapSeconds), false); keepChoices(); syncControls(); };
    m_batchName.setTooltip("Batch Name\nThe name your files start with. For example, Dragon Wings gives Dragon Wings_001, Dragon Wings_002 and so on.");
    m_batchName.setTextToShowWhenEmpty("Type a name", TC::textFaint);
    m_batchName.onTextChange = [this] { syncControls(); };
    m_batchName.onFocusLost = [this] { keepChoices(); syncControls(); };

    for (auto* box : { &m_each, &m_mode, &m_batch, &m_names, &m_files })
    {
        box->setWantsKeyboardFocus(false);
        addAndMakeVisible(*box);
    }
    m_each.onChange = [this]
    {
        const bool ride = m_each.getSelectedId() == 2;
        m_processor.setVariationValue("Each", ride ? "1" : "0");
        m_length.setText(oneDecimal(ride ? m_rideSeconds : m_whooshSeconds), false);
        syncControls();
    };
    m_mode.onChange = [this] { m_processor.setVariationMode(m_mode.getSelectedId() - 1); syncControls(); };
    m_names.onChange = [this] { m_processor.setVariationValue("Names", juce::String(m_names.getSelectedId() - 1)); syncControls(); };
    m_files.onChange = [this] { m_processor.setVariationValue("Files", juce::String(m_files.getSelectedId() - 1)); syncControls(); };
    m_files.setTooltip("Files\nA file for every variation, one long file that holds them all with a named marker at the start of each, or both.");
    m_names.setTooltip("File Names\nWhat the files are called. The First File line below shows the name the first one will get.");
    m_batch.onChange = [this]
    {
        const int id = m_batch.getSelectedId();
        const auto recent = m_processor.getVariations().recentBatches();
        m_repeat = id == 2 ? Repeat::Last : id >= 10 ? Repeat::Chosen : Repeat::New;
        if (m_repeat == Repeat::Chosen && id - 10 < static_cast<int>(recent.size()))
            m_repeatNumber = recent[static_cast<size_t>(id - 10)].number;
        if (VariationRenderer::Batch batch; repeatedBatch(batch))
            takeFrom(batch);
        else
            m_repeat = Repeat::New;
        syncControls();
    };

    m_amount.setSliderStyle(juce::Slider::LinearHorizontal);
    m_amount.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    m_amount.setRange(0.0, 1.0, 0.01);
    m_amount.setColour(juce::Slider::trackColourId, TC::flight);
    m_amount.setDoubleClickReturnValue(true, 0.3);
    m_amount.setWantsKeyboardFocus(false);
    m_amount.textFromValueFunction = [](double v) { return juce::String(juce::roundToInt(100.0 * v)) + "%"; };
    m_amount.setTooltip("How Different\nHow far each file may stray from your sound. Low: close cousins of what you dialled in. High: each knob can "
                        "move up to half its travel, so the files go further afield. Used only with My Settings, Small Changes.");
    // The batch reads the slider itself; the settings file is written once the drag is over, not at every step of it.
    m_amount.onValueChange = [this] { syncControls(); };
    m_amount.onDragEnd = [this] { m_processor.setVariationAmount(static_cast<float>(m_amount.getValue())); };
    addAndMakeVisible(m_amount);

    m_choose.setTooltip("Choose Folder\nPick the folder the files go to. Files that are already there are never replaced: the numbers carry on after them.");
    m_choose.onClick = [this]
    {
        m_chooser = std::make_unique<juce::FileChooser>("Where should the variations go?", m_processor.getRecordFolder());
        m_chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                               [this](const juce::FileChooser& fc)
                               {
                                   if (const auto chosen = fc.getResult(); chosen.isDirectory())
                                   {
                                       m_processor.setVariationFolder(chosen.getFullPathName());
                                       syncControls();
                                   }
                               });
    };
    m_dated.setTooltip("Dated Folder\nEvery batch gets a new folder of its own inside the recordings folder, named with the date and time.");
    m_dated.onClick = [this] { m_processor.setVariationFolder({}); syncControls(); };

    m_render.setTooltip("Render\nMakes the files in the background, from your settings as they are right now. You can keep playing and changing "
                        "things meanwhile; the files will not pick up those changes.");
    m_render.onClick = [this] { startBatch(); };
    m_cancel.setTooltip("Cancel\nStops making files. The ones already made are kept.");
    m_cancel.onClick = [this] { m_processor.getVariations().cancel(); };
    m_show.setTooltip(showButtonText() + "\nOpens the folder the files are in.");
    m_show.onClick = [this]
    {
        auto target = m_shown.state != State::Idle ? m_shown.folder : folder();
        while (! target.isDirectory() && target.getParentDirectory() != target)
            target = target.getParentDirectory();
        target.revealToUser();
    };
    m_openTakes.setTooltip("Open Takes\nOpens Takes, where you can hear the newest files, star them and drag them out.");
    m_openTakes.onClick = [this] { if (onOpenTakes) onOpenTakes(); };
    m_close.onClick = [this] { setVisible(false); };
    setMouseCursor(juce::MouseCursor::NormalCursor);
    fillMenus();
}

void VariationsView::fillMenus()
{
    const auto defaults = m_processor.getVariations().defaultJob();
    auto fill = [](juce::ComboBox& box, int selected, std::initializer_list<std::pair<juce::String, juce::String>> items)
    {
        box.clear(juce::dontSendNotification);
        int id = 1;
        for (const auto& [text, gives] : items)
            addChoice(*box.getRootMenu(), id++, text, gives);
        box.setSelectedId(selected + 1, juce::dontSendNotification);
    };
    fill(m_each, static_cast<int>(defaults.each),
         { { VariationRenderer::eachName(Each::OneWhoosh), "A single pass, from silence until it has rung out" },
           { VariationRenderer::eachName(Each::WholeRide), "A stretch of the rig playing, as you hear it with Play on" } });
    fill(m_mode, static_cast<int>(defaults.mode),
         { { VariationRenderer::modeName(Mode::NewPasses), "Your sound exactly as you set it; only the passes differ" },
           { VariationRenderer::modeName(Mode::SmallChanges), "Your sound, with every knob moved a little" },
           { VariationRenderer::modeName(Mode::RandomiseEverything), "A different sound in every file" } });
    // The names as they would be for this session, so each item shows what it gives.
    auto example = [&defaults](Names names)
    {
        auto j = defaults;
        j.names = names;
        j.folder = defaults.folder.getChildFile("Example"); // not there: the numbers start at 001
        j.count = 1;
        if (names == Names::BatchNameAndCounter)
            return juce::String("A name you type, then 001");
        return VariationRenderer::planOf(j).firstName;
    };
    fill(m_names, static_cast<int>(defaults.names),
         { { VariationRenderer::namesName(Names::Counter), example(Names::Counter) },
           { VariationRenderer::namesName(Names::DateAndTime), example(Names::DateAndTime) },
           { VariationRenderer::namesName(Names::PresetAndCounter), example(Names::PresetAndCounter) },
           { VariationRenderer::namesName(Names::BatchNameAndCounter), example(Names::BatchNameAndCounter) } });
    fill(m_files, static_cast<int>(defaults.files),
         { { VariationRenderer::filesName(Files::PerVariation), "Every variation is a file of its own" },
           { VariationRenderer::filesName(Files::OneWithMarkers), "One long file, with a named marker at each variation" },
           { VariationRenderer::filesName(Files::Both), "The separate files, and the one long file as well" } });
    fillBatchMenu();
}

void VariationsView::fillBatchMenu()
{
    const auto recent = m_processor.getVariations().recentBatches();
    m_batch.clear(juce::dontSendNotification);
    auto& menu = *m_batch.getRootMenu();
    addChoice(menu, 1, "New Batch Every Time", "Variations you have not had before");
    addChoice(menu, 2, "Repeat Last Batch", recent.empty() ? juce::String("Nothing rendered yet") : juce::String("The same variations as last time"),
              ! recent.empty());
    juce::PopupMenu older;
    for (size_t i = 0; i < recent.size(); ++i)
        older.addItem(10 + static_cast<int>(i), VariationRenderer::batchTitle(recent[i]));
    menu.addSubMenu("Repeat Batch...", older, ! recent.empty());

    int selected = 1;
    if (m_repeat == Repeat::Last && ! recent.empty())
        selected = 2;
    else if (m_repeat == Repeat::Chosen)
        for (size_t i = 0; i < recent.size(); ++i)
            if (recent[i].number == m_repeatNumber)
                selected = 10 + static_cast<int>(i);
    if (selected == 1)
        m_repeat = Repeat::New;
    m_batch.setSelectedId(selected, juce::dontSendNotification);
}

bool VariationsView::repeatedBatch(VariationRenderer::Batch& batch) const
{
    if (m_repeat == Repeat::New)
        return false;
    for (const auto& b : m_processor.getVariations().recentBatches())
        if (m_repeat == Repeat::Last || b.number == m_repeatNumber)
        {
            batch = b;
            return true;
        }
    return false;
}

void VariationsView::takeFrom(const VariationRenderer::Batch& batch)
{
    // The same variations need the same count, lengths and What Varies; they can still be changed afterwards.
    m_processor.setVariationCount(batch.count);
    m_processor.setVariationMode(static_cast<int>(batch.mode));
    m_processor.setVariationAmount(batch.amount);
    m_processor.setVariationValue("Each", juce::String(static_cast<int>(batch.each)));
    m_whooshSeconds = batch.seconds;
    m_rideSeconds = batch.rideSeconds;
    m_count.setText(juce::String(batch.count), false);
    m_mode.setSelectedId(static_cast<int>(batch.mode) + 1, juce::dontSendNotification);
    m_amount.setValue(batch.amount, juce::dontSendNotification);
    m_each.setSelectedId(static_cast<int>(batch.each) + 1, juce::dontSendNotification);
    m_length.setText(oneDecimal(batch.each == Each::WholeRide ? batch.rideSeconds : batch.seconds), false);
    keepChoices();
}

void VariationsView::keepChoices()
{
    const auto j = job();
    m_processor.setVariationValue("RideSeconds", juce::String(j.rideSeconds, 1));
    m_processor.setVariationValue("Gap", juce::String(j.gapSeconds, 1));
    m_processor.setVariationValue("BatchName", j.batchName);
}

void VariationsView::visibilityChanged()
{
    if (isVisible())
    {
        readSettings();
        startTimerHz(15);
    }
    else
    {
        stopTimer();
        m_processor.setVariationAmount(static_cast<float>(m_amount.getValue())); // a wheel or key move has no drag end
        keepChoices();
    }
}

void VariationsView::readSettings()
{
    const auto defaults = m_processor.getVariations().defaultJob();
    m_count.setText(juce::String(defaults.count), false);
    if (! m_processor.getVariations().isRendering())
        m_whooshSeconds = defaults.seconds; // Throw Length, as it is now
    m_rideSeconds = defaults.rideSeconds;
    m_gap.setText(oneDecimal(defaults.gapSeconds), false);
    m_batchName.setText(defaults.batchName, false);
    m_amount.setValue(defaults.amount, juce::dontSendNotification);
    fillMenus(); // the choices as they were left, and names that show this session's
    if (VariationRenderer::Batch batch; repeatedBatch(batch))
        m_whooshSeconds = batch.seconds; // a batch to repeat keeps its whooshes' length while the panel is shut
    m_length.setText(oneDecimal(defaults.each == Each::WholeRide ? m_rideSeconds : m_whooshSeconds), false);
    m_error.clear();
    m_shown = m_processor.getVariations().getProgress();
    syncControls();
}

juce::File VariationsView::folder() const
{
    const auto chosen = m_processor.getVariationFolder();
    return chosen.isNotEmpty() ? juce::File(chosen)
                               : VariationRenderer::datedFolder(m_processor.getRecordFolder(), m_processor.getEffectiveTakeName(), juce::Time::getCurrentTime());
}

VariationRenderer::Job VariationsView::job() const
{
    auto j = m_processor.getVariations().defaultJob(); // the tape's format and name, the folder, the time, a new batch
    j.count = juce::jlimit(1, VariationRenderer::c_maxCount, m_count.getText().getIntValue());
    j.each = m_each.getSelectedId() == 2 ? Each::WholeRide : Each::OneWhoosh;
    j.seconds = juce::jlimit(VariationRenderer::c_minSeconds, VariationRenderer::c_maxSeconds, m_whooshSeconds);
    j.rideSeconds = juce::jlimit(VariationRenderer::c_minRideSeconds, VariationRenderer::c_maxRideSeconds, m_rideSeconds);
    j.mode = static_cast<Mode>(juce::jlimit(0, 2, m_mode.getSelectedId() - 1));
    j.amount = static_cast<float>(m_amount.getValue());
    j.names = static_cast<Names>(juce::jlimit(0, 3, m_names.getSelectedId() - 1));
    j.batchName = m_batchName.getText().trim();
    j.files = static_cast<Files>(juce::jlimit(0, 2, m_files.getSelectedId() - 1));
    j.gapSeconds = juce::jlimit(0.0, VariationRenderer::c_maxGapSeconds, m_gap.getText().getDoubleValue());
    if (VariationRenderer::Batch batch; repeatedBatch(batch))
        j.batchSeed = batch.number;
    return j;
}

juce::String VariationsView::formatLine() const
{
    const auto j = job();
    const char* types[] = { "WAV", "AIFF", "FLAC" };
    const int bits = j.options.type != TapeRecorder::FileType::Wav && j.options.bitDepth == 32 ? 24 : j.options.bitDepth;
    juce::String line;
    line << types[juce::jlimit(0, 2, static_cast<int>(j.options.type))] << ", " << bits << "-bit" << (bits == 32 ? " float" : "") << ", "
         << oneDecimal(j.rate / 1000.0) << " kHz, " << (j.options.mono ? "mono" : "stereo");
    return line;
}

juce::String VariationsView::whatVariesSentence(Mode mode, float amount)
{
    switch (mode)
    {
        case Mode::SmallChanges:
            return "Each one uses your settings with every knob moved a little (How Different is at " + juce::String(juce::roundToInt(100.0f * amount)) + "%).";
        case Mode::RandomiseEverything:
            return "Each one is a fresh Randomise: a different sound every time.";
        case Mode::NewPasses:
        default:
            return "Each one uses your settings exactly as they are. Only the passes differ.";
    }
}

void VariationsView::syncControls()
{
    const bool rendering = m_processor.getVariations().isRendering();
    const auto j = job();
    for (auto* c : std::initializer_list<juce::Component*> { &m_count, &m_length, &m_each, &m_mode, &m_batch, &m_names, &m_files, &m_choose, &m_dated,
                                                              &m_render })
        c->setEnabled(! rendering);
    auto dim = [rendering](juce::Component& c, bool used)
    {
        c.setEnabled(used && ! rendering);
        c.setAlpha(used ? 1.0f : 0.35f);
    };
    dim(m_amount, j.mode == Mode::SmallChanges);
    dim(m_batchName, j.names == Names::BatchNameAndCounter);
    dim(m_gap, j.files != Files::PerVariation);
    m_dated.setToggleState(m_processor.getVariationFolder().isEmpty(), juce::dontSendNotification);
    m_cancel.setEnabled(rendering);
    m_openTakes.setEnabled(m_shown.done > 0);

    // The tips say what the choice in hand gives.
    const bool ride = j.each == Each::WholeRide;
    m_each.setTooltip(ride ? "What Each File Is\nWhole Ride: a stretch of the rig playing, with its passes, hits and modulation, as you hear it with Play on."
                           : "What Each File Is\nOne Whoosh: a single pass that starts from silence and rings out, like pressing Throw.");
    m_length.setTooltip(ride ? "Length\nHow many seconds of the rig playing go into each file, from 1 to 120."
                             : "Length\nHow long each whoosh takes to go by, from 0.3 to 6 seconds. The file runs a little longer, until the sound has rung out.");
    m_mode.setTooltip(j.mode == Mode::SmallChanges
                          ? "What Varies\nYour settings, with each knob moved a little either way, by up to How Different. A family of sounds around the one you dialled in."
                          : j.mode == Mode::RandomiseEverything
                                ? "What Varies\nA fresh Randomise for every file, so each one is a different sound. Your tracks, their levels and any locked section stay as they are."
                                : "What Varies\nEvery knob, wire and modulator stays exactly as you set it. Only the passes differ: how each one flies, how close it comes "
                                  "and which part of each sound it plays.");
    m_batch.setTooltip(m_repeat == Repeat::New
                           ? "Batch\nEvery press of Render makes variations you have not had before."
                           : "Batch\nRender makes the same variations as that batch again, as long as your settings and sounds are the same. Handy for the same "
                             "whooshes in another file type, or with one knob changed.");

    // What Render will make: the first file's name, then how much it comes to. While a batch renders these keep
    // saying what it is making (its own files would move the next free number along under it).
    if (rendering)
    {
        repaint();
        return;
    }
    const auto plan = VariationRenderer::planOf(j);
    const auto extension = TapeRecorder::extensionOf(j.options.type);
    const auto format = " (" + formatLine() + ", as chosen in Settings)";
    switch (j.files)
    {
        case Files::OneWithMarkers: m_firstLine = "The file: " + plan.combined.getFileName() + format; break;
        case Files::Both:           m_firstLine = "First file: " + plan.firstName + extension + ", and all of them in " + plan.combined.getFileName() + format; break;
        case Files::PerVariation:
        default:                    m_firstLine = "First file: " + plan.firstName + extension + format; break;
    }
    const auto estimate = m_processor.getVariations().estimate(j);
    m_getLine = VariationRenderer::oneFileTooBig(j, estimate)
                    ? juce::String("That one file would be over 4 GB, too big for markers. Make fewer or shorter ones, or use FLAC.")
                    : VariationRenderer::estimateText(j, estimate);
    m_syncedMinute = juce::Time::getCurrentTime().getMinutes();
    repaint();
}

void VariationsView::startBatch()
{
    const auto j = job();
    m_count.setText(juce::String(j.count), false);
    m_length.setText(oneDecimal(j.each == Each::WholeRide ? j.rideSeconds : j.seconds), false);
    m_gap.setText(oneDecimal(j.gapSeconds), false);
    m_error.clear();
    m_processor.setVariationAmount(j.amount);
    keepChoices();
    juce::String error;
    if (! m_processor.getVariations().start(j, error))
        m_error = error;
    m_shown = m_processor.getVariations().getProgress();
    fillBatchMenu(); // this batch is now the last one
    syncControls();
}

void VariationsView::timerCallback()
{
    const auto progress = m_processor.getVariations().getProgress();
    const bool ended = m_shown.state == State::Rendering && progress.state != State::Rendering;
    const bool changed = progress.state != m_shown.state || progress.done != m_shown.done || progress.silent != m_shown.silent
                      || progress.currentFile != m_shown.currentFile
                      || juce::roundToInt(progress.secondsLeft) != juce::roundToInt(m_shown.secondsLeft);
    m_shown = progress;
    if (ended)
        m_processor.refreshTakes(); // the last files are in Takes by the time Open Takes can be pressed
    // Date And Time names, and a dated folder, follow the clock: the First File line moves on with the minute.
    const bool newMinute = progress.state != State::Rendering && juce::Time::getCurrentTime().getMinutes() != m_syncedMinute;
    if (changed || ended || newMinute)
        syncControls();
}

void VariationsView::mouseDown(const juce::MouseEvent& e)
{
    if (! m_panel.contains(e.getPosition()))
        setVisible(false);
}

void VariationsView::resized()
{
    m_panel = getLocalBounds().withSizeKeepingCentre(c_panelWidth, c_panelHeight);
    auto r = m_panel.reduced(28, 0);
    r.removeFromTop(56);
    size_t caption = 0;
    // One row of captions and, under it, the controls they name.
    struct Row
    {
        juce::Rectangle<int> captions, controls;
        std::array<juce::Rectangle<int>, 11>& names;
        size_t& next;
        juce::Rectangle<int> take(int width, int gapAfter = 20)
        {
            names[next++] = captions.removeFromLeft(width);
            const auto box = controls.removeFromLeft(width);
            captions.removeFromLeft(gapAfter);
            controls.removeFromLeft(gapAfter);
            return box;
        }
    };
    auto row = [&]() -> Row
    {
        const auto captions = r.removeFromTop(20);
        const auto controls = r.removeFromTop(28);
        r.removeFromTop(12);
        return { captions, controls, m_captions, caption };
    };

    // How Many, What Each File Is, Length, What Varies
    {
        auto line = row();
        m_count.setBounds(line.take(80));
        m_each.setBounds(line.take(170));
        auto length = line.take(88);
        m_length.setBounds(length.removeFromLeft(64));
        m_lengthUnit = length.withTrimmedLeft(6);
        m_mode.setBounds(line.take(line.controls.getWidth(), 0));
    }
    // How Different, Batch
    {
        auto line = row();
        auto amount = line.take(246);
        m_amountValue = amount.removeFromRight(46);
        m_amount.setBounds(amount.reduced(0, 4));
        m_batch.setBounds(line.take(line.controls.getWidth(), 0));
    }
    // File Names, Batch Name, Files, Gap
    {
        auto line = row();
        m_names.setBounds(line.take(210));
        m_batchName.setBounds(line.take(190));
        m_files.setBounds(line.take(190, 12));
        auto gap = line.take(line.controls.getWidth(), 0);
        m_gap.setBounds(gap.removeFromLeft(44));
        m_gapUnit = gap.withTrimmedLeft(6);
    }
    // Folder: where it is, and the two ways to say where
    {
        m_captions[caption++] = r.removeFromTop(20);
        auto line = r.removeFromTop(28);
        r.removeFromTop(10);
        m_dated.setBounds(line.removeFromRight(116));
        line.removeFromRight(6);
        m_choose.setBounds(line.removeFromRight(104));
        m_folderText = line.withTrimmedRight(12);
    }
    jassert(caption == m_captions.size());
    m_firstText = r.removeFromTop(20);
    r.removeFromTop(8);
    m_getText = r.removeFromTop(18);
    m_variesText = r.removeFromTop(18);
    r.removeFromTop(10);
    m_bar = r.removeFromTop(22);
    r.removeFromTop(6);
    m_status = r.removeFromTop(36);

    auto bottom = m_panel.reduced(28, 18).removeFromBottom(30);
    m_close.setBounds(bottom.removeFromRight(90));
    m_render.setBounds(bottom.removeFromLeft(110));
    bottom.removeFromLeft(6);
    m_cancel.setBounds(bottom.removeFromLeft(90));
    bottom.removeFromLeft(28);
    m_show.setBounds(bottom.removeFromLeft(136));
    bottom.removeFromLeft(6);
    m_openTakes.setBounds(bottom.removeFromLeft(110));
}

void VariationsView::paint(juce::Graphics& g)
{
    g.fillAll(TC::window.withAlpha(0.8f));
    SwooshLookAndFeel::drawPanel(g, m_panel.toFloat(), "Render Variations", "Lots of versions of your sound, straight to files", TC::brass, m_fonts,
                                 Icons::Icon::Variations);

    const char* captions[] = { "How Many", "What Each File Is", "Length", "What Varies", "How Different", "Batch",
                               "File Names", "Batch Name", "Files", "Gap", "Folder" };
    const auto mode = static_cast<Mode>(juce::jlimit(0, 2, m_mode.getSelectedId() - 1));
    const bool small = mode == Mode::SmallChanges;
    const bool named = m_names.getSelectedId() - 1 == static_cast<int>(Names::BatchNameAndCounter);
    const bool gapped = m_files.getSelectedId() - 1 != static_cast<int>(Files::PerVariation);
    for (size_t i = 0; i < m_captions.size(); ++i)
    {
        const bool used = (i != 4 || small) && (i != 7 || named) && (i != 9 || gapped);
        g.setColour(used ? TC::textDim : TC::textFaint);
        Theme::fitText(g, captions[i], m_captions[i], m_fonts.caption(), juce::Justification::centredLeft);
    }

    const auto body = m_fonts.body();
    g.setColour(TC::textDim);
    Theme::fitText(g, "s", m_lengthUnit, body, juce::Justification::centredLeft);
    g.setColour(gapped ? TC::textDim : TC::textFaint);
    Theme::fitText(g, "s", m_gapUnit, body, juce::Justification::centredLeft);
    g.setColour(small ? TC::text : TC::textFaint);
    Theme::fitText(g, juce::String(juce::roundToInt(100.0 * m_amount.getValue())) + "%", m_amountValue, body, juce::Justification::centredRight);

    // Where the files go, what the first is called, and what Render will make.
    {
        const bool dated = m_processor.getVariationFolder().isEmpty();
        g.setColour(TC::text);
        const auto path = dated ? folder().getParentDirectory().getFullPathName() + juce::File::getSeparatorString()
                                      + m_processor.getEffectiveTakeName() + " Variations, with the date and time"
                                : folder().getFullPathName();
        Theme::fitText(g, fitPath(body, path, m_folderText.getWidth()), m_folderText, body, juce::Justification::centredLeft, Theme::Fit::Name);
        g.setColour(TC::textDim);
        Theme::fitText(g, m_firstLine, m_firstText, body, juce::Justification::centredLeft, Theme::Fit::Name);
        g.setColour(TC::text);
        Theme::fitText(g, m_getLine, m_getText, body, juce::Justification::centredLeft, Theme::Fit::Sentence);
        g.setColour(TC::textDim);
        VariationRenderer::Batch batch;
        Theme::fitText(g, repeatedBatch(batch) ? "The same variations as " + VariationRenderer::batchTitle(batch) + ", if your settings and sounds are the same."
                                               : whatVariesSentence(mode, static_cast<float>(m_amount.getValue())),
                       m_variesText, body, juce::Justification::centredLeft, Theme::Fit::Message);
    }

    // Progress: the bar with its percentage, then what is happening in words.
    const auto& p = m_shown;
    const bool rendering = p.state == State::Rendering;
    const int handled = p.done + p.silent;
    const float fraction = p.total > 0 ? juce::jlimit(0.0f, 1.0f, static_cast<float>(handled) / static_cast<float>(p.total)) : 0.0f;
    {
        const auto bar = m_bar.toFloat();
        SwooshLookAndFeel::drawWell(g, bar);
        if (p.state != State::Idle && fraction > 0.0f)
        {
            g.saveState();
            g.reduceClipRegion(bar.withWidth(bar.getWidth() * fraction).toNearestInt());
            g.setColour(p.state == State::Failed ? TC::hits.withAlpha(0.75f)
                        : p.state == State::Cancelled ? TC::textFaint
                        : p.state == State::Finished ? TC::go.withAlpha(0.85f) : TC::brass.withAlpha(0.9f));
            Theme::Plate::fill(g, bar.reduced(1.0f), Theme::Cut::control - 1.0f);
            g.restoreState();
        }
        if (p.state != State::Idle)
        {
            const auto text = juce::String(juce::roundToInt(100.0f * fraction)) + "%";
            // Dark on the filled part, light on the empty part, so the number reads wherever the edge is.
            g.setColour(TC::text);
            Theme::fitText(g, text, m_bar, m_fonts.control(), juce::Justification::centred);
            g.saveState();
            g.reduceClipRegion(bar.withWidth(bar.getWidth() * fraction).toNearestInt());
            g.setColour(TC::window);
            Theme::fitText(g, text, m_bar, m_fonts.control(), juce::Justification::centred);
            g.restoreState();
        }
    }

    auto status = m_status;
    auto top = status.removeFromTop(18);
    const auto folderName = p.folder.getFileName();
    const bool ride = p.each == Each::WholeRide;
    auto things = [ride](int n) { return juce::String(n) + (ride ? (n == 1 ? " ride" : " rides") : (n == 1 ? " whoosh" : " whooshes")); };
    // What was made, by how the files were asked for.
    juce::String made;
    switch (p.filing)
    {
        case Files::OneWithMarkers: made = p.combinedFile + ", with " + things(p.done) + " in it, in " + folderName; break;
        case Files::Both:           made = juce::String(p.done) + (p.done == 1 ? " file" : " files") + " and " + p.combinedFile + " in " + folderName; break;
        case Files::PerVariation:
        default:                    made = juce::String(p.done) + (p.done == 1 ? " file" : " files") + " in " + folderName; break;
    }
    const juce::String silentNote = p.silent > 0 ? " " + juce::String(p.silent) + (p.silent == 1 ? " had no sound and was left out." : " had no sound and were left out.")
                                                 : juce::String();
    const auto left = juce::Justification::centredLeft;
    if (m_error.isNotEmpty())
    {
        Theme::fitLines(g, m_error, m_status, body, TC::hits.brighter(0.2f));
    }
    else if (rendering)
    {
        g.setColour(TC::text);
        Theme::fitText(g, juce::String(juce::jmin(p.total, handled + 1)) + " of " + juce::String(p.total), top.removeFromLeft(110), body, left);
        g.setColour(TC::textDim);
        Theme::fitText(g, timeLeft(p.secondsLeft), top.removeFromRight(190), body, juce::Justification::centredRight, Theme::Fit::Sentence);
        Theme::fitText(g, p.currentFile, top, body, juce::Justification::centred, Theme::Fit::Name);
        g.setColour(TC::textFaint);
        Theme::fitText(g, "You can close this panel and keep playing. The Variations button in the header shows how far it is.", status, body, left,
                       Theme::Fit::Sentence);
    }
    else if (p.state == State::Finished)
    {
        g.setColour(TC::text);
        Theme::fitText(g, fitPath(body, "Done. " + made, top.getWidth()), top, body, left, Theme::Fit::Name);
        g.setColour(TC::textFaint);
        Theme::fitText(g, "Batch " + juce::String(p.batchSeed) + ", made in " + juce::String(p.elapsedSeconds, 1) + " s." + silentNote, status,
                       m_fonts.caption(), left, Theme::Fit::Message);
    }
    else if (p.state == State::Cancelled)
    {
        g.setColour(TC::text);
        Theme::fitText(g, fitPath(body, p.done > 0 ? "Cancelled. Kept: " + made : juce::String("Cancelled before the first one was made."), top.getWidth()), top,
                       body, left, Theme::Fit::Name);
        g.setColour(TC::textFaint);
        Theme::fitText(g, "Batch " + juce::String(p.batchSeed) + "." + silentNote, status, m_fonts.caption(), left, Theme::Fit::Message);
    }
    else if (p.state == State::Failed)
    {
        Theme::fitLines(g, p.error + (p.done > 0 ? " Kept: " + made + "." : juce::String()), m_status, body, TC::hits.brighter(0.2f));
    }
    else
    {
        Theme::fitLines(g, "Press Render to make them. It works in the background, so you can keep playing and changing things meanwhile.", m_status, body,
                        TC::textFaint);
    }
}
