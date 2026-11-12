#include "VariationsView.h"
#include "LookAndFeel.h"

namespace TC = Theme::Colours;

namespace
{
    using Mode = VariationRenderer::Mode;
    using State = VariationRenderer::State;

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
            return "About " + juce::String(juce::roundToInt(seconds)) + " s left";
        return "About " + juce::String(juce::roundToInt(seconds / 60.0)) + " min left";
    }
}

juce::String VariationsView::showButtonText()
{
   #if JUCE_MAC
    return "Show In Finder";
   #elif JUCE_WINDOWS
    return "Show In Explorer";
   #else
    return "Show Folder";
   #endif
}

VariationsView::VariationsView(SwooshbucklerAudioProcessor& processor, const Theme::Fonts& fonts)
    : m_processor(processor), m_fonts(fonts)
{
    m_show.setButtonText(showButtonText());
    for (auto* b : { &m_newSeed, &m_choose, &m_dated, &m_render, &m_cancel, &m_show, &m_openTakes, &m_close, &m_modes[0], &m_modes[1], &m_modes[2] })
    {
        b->setColour(juce::TextButton::buttonOnColourId, TC::flight);
        b->setWantsKeyboardFocus(false);
        addAndMakeVisible(*b);
    }
    m_render.setColour(juce::TextButton::buttonOnColourId, TC::go);

    const char* restrictions[] = { "0123456789", "0123456789.", "0123456789" };
    const int lengths[] = { 4, 4, 10 };
    juce::TextEditor* editors[] = { &m_count, &m_length, &m_seed };
    for (int i = 0; i < 3; ++i)
    {
        auto& e = *editors[i];
        e.setFont(m_fonts.body(13.0f));
        e.setColour(juce::TextEditor::backgroundColourId, TC::scope);
        e.setInputRestrictions(lengths[i], restrictions[i]);
        e.setSelectAllWhenFocused(true);
        e.setJustification(juce::Justification::centredLeft);
        // Return and Escape hand the keyboard back to the rig (space for Swell, F for Throw).
        e.onReturnKey = [this] { if (onKeyboardDone) onKeyboardDone(); };
        e.onEscapeKey = [this] { if (onKeyboardDone) onKeyboardDone(); };
        addAndMakeVisible(e);
    }
    m_count.setTooltip("Count\nHow many whooshes to render, from 1 to 1000. Each one becomes a file of its own.");
    m_count.onTextChange = [this]
    {
        if (const int count = m_count.getText().getIntValue(); count >= 1)
            m_processor.setVariationCount(count);
        repaint();
    };
    m_count.onFocusLost = [this] { m_count.setText(juce::String(job().count), false); repaint(); };
    m_length.setTooltip("Length\nHow long each thrown whoosh is, from 0.3 to 6 seconds; it peaks half-way through. It starts out as the Ride panel's Throw Length. "
                        "Every file runs on after its whoosh until the echoes and Space have rung out (six seconds at the most), so the files are longer than this.");
    m_length.onFocusLost = [this] { m_length.setText(juce::String(job().seconds, 1), false); };
    m_seed.setTooltip("Batch Seed\nThe number every file's random draws come from. The same seed, with the same settings and samples, renders the same batch again; "
                      "another seed renders another batch. It is written into every file, with that file's settings.");
    m_seed.onTextChange = [this] { m_seedIsOurs = false; };
    m_seed.onFocusLost = [this] { m_seed.setText(juce::String(job().batchSeed), false); };
    m_newSeed.setTooltip("New Seed\nDraws a new batch seed. One is drawn after every batch anyway, unless you typed your own.");
    m_newSeed.onClick = [this]
    {
        m_seed.setText(juce::String(m_processor.getVariations().defaultJob().batchSeed), false);
        m_seedIsOurs = true;
    };

    const char* modeTips[] = {
        "Randomise Everything\nA fresh Randomise roll for every file: the ride, the buses, the hits and the space all change, the way the RANDOMISE button "
        "changes them. Your tracks, their levels and the output stay as they are.",
        "Small Changes\nEvery file starts from the settings as they are now, nudges each of those controls by up to Amount, and starts each track a little "
        "further into its sample. For a family of whooshes around one you like.",
        "Seed Only\nThe settings stay exactly as they are. Only the rig's own dice change: how each pass flies, how close it comes and which sound of each "
        "track it picks." };
    for (int i = 0; i < 3; ++i)
    {
        auto& b = m_modes[static_cast<size_t>(i)];
        b.setButtonText(VariationRenderer::modeName(static_cast<Mode>(i)));
        b.setTooltip(modeTips[i]);
        b.onClick = [this, i] { m_processor.setVariationMode(i); syncControls(); };
    }

    m_amount.setSliderStyle(juce::Slider::LinearHorizontal);
    m_amount.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    m_amount.setRange(0.0, 1.0, 0.01);
    m_amount.setColour(juce::Slider::trackColourId, TC::flight);
    m_amount.setDoubleClickReturnValue(true, 0.3);
    m_amount.setWantsKeyboardFocus(false);
    m_amount.setTooltip("Amount\nHow far Small Changes may move each control: at 100%, up to half of its travel either way, and up to four semitones of Pitch.");
    // The batch reads the slider itself; the settings file is written once the drag is over, not at every step of it.
    m_amount.onValueChange = [this] { repaint(); };
    m_amount.onDragEnd = [this] { m_processor.setVariationAmount(static_cast<float>(m_amount.getValue())); };
    addAndMakeVisible(m_amount);

    m_choose.setTooltip("Choose Folder\nPick the folder the files go to. Numbers carry on after the files already there, so nothing is ever written over.");
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
    m_dated.setTooltip("Dated Folder\nThe usual place: every batch gets a new folder of its own, named after the date and time, inside the recordings folder.");
    m_dated.onClick = [this] { m_processor.setVariationFolder({}); syncControls(); };

    m_render.setTooltip("Render\nRenders the batch in the background, faster than real time, from the session as it is at this moment. The rig keeps playing "
                        "and you can keep working; nothing you change afterwards reaches the batch.");
    m_render.onClick = [this] { startBatch(); };
    m_cancel.setTooltip("Cancel\nStops the batch. The files already rendered are kept; the one in hand is dropped.");
    m_cancel.onClick = [this] { m_processor.getVariations().cancel(); };
    m_show.setTooltip(Theme::localiseShortcuts(showButtonText() + "\nShows the batch's folder in the Finder / Explorer."));
    m_show.onClick = [this]
    {
        auto target = m_shown.state != State::Idle ? m_shown.folder : folder();
        while (! target.isDirectory() && target.getParentDirectory() != target)
            target = target.getParentDirectory();
        target.revealToUser();
    };
    m_openTakes.setTooltip("Open Takes\nOpens Takes, where the newest rendered files can be heard, starred and dragged out.");
    m_openTakes.onClick = [this] { if (onOpenTakes) onOpenTakes(); };
    m_close.onClick = [this] { setVisible(false); };
    setMouseCursor(juce::MouseCursor::NormalCursor);
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
    }
}

void VariationsView::readSettings()
{
    const auto defaults = m_processor.getVariations().defaultJob();
    m_count.setText(juce::String(defaults.count), false);
    if (! m_processor.getVariations().isRendering())
        m_length.setText(juce::String(defaults.seconds, 1), false);
    if (m_seed.isEmpty())
    {
        m_seed.setText(juce::String(defaults.batchSeed), false);
        m_seedIsOurs = true;
    }
    m_amount.setValue(defaults.amount, juce::dontSendNotification);
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
    auto j = m_processor.getVariations().defaultJob(); // the tape's format and name, the folder
    j.count = juce::jlimit(1, VariationRenderer::c_maxCount, m_count.getText().getIntValue());
    const double seconds = m_length.getText().getDoubleValue();
    j.seconds = juce::jlimit(VariationRenderer::c_minSeconds, VariationRenderer::c_maxSeconds, seconds > 0.0 ? seconds : j.seconds);
    j.mode = static_cast<Mode>(m_processor.getVariationMode());
    j.amount = static_cast<float>(m_amount.getValue());
    if (const auto seed = m_seed.getText().getLargeIntValue(); seed > 0)
        j.batchSeed = static_cast<std::uint32_t>(juce::jmin<juce::int64>(seed, 0xffffffffll));
    return j;
}

juce::String VariationsView::formatLine() const
{
    const auto j = job();
    const auto extension = TapeRecorder::extensionOf(j.options.type);
    const char* types[] = { "WAV", "AIFF", "FLAC" };
    const int bits = j.options.type != TapeRecorder::FileType::Wav && j.options.bitDepth == 32 ? 24 : j.options.bitDepth;
    juce::String rate(j.rate / 1000.0, 1);
    if (rate.endsWith(".0"))
        rate = rate.dropLastCharacters(2);
    juce::String line;
    line << VariationRenderer::fileNameOf(j.name, 1, j.count) << extension;
    if (j.count > 1)
        line << " to " << VariationRenderer::fileNameOf(j.name, j.count, j.count) << extension;
    line << ": " << types[juce::jlimit(0, 2, static_cast<int>(j.options.type))] << ", " << bits << "-bit" << (bits == 32 ? " float" : "") << ", " << rate
         << " kHz, " << (j.options.mono ? "mono" : "stereo") << ". The name and the format are the tape's, from SETTINGS.";
    return line;
}

void VariationsView::syncControls()
{
    const bool rendering = m_processor.getVariations().isRendering();
    const int mode = m_processor.getVariationMode();
    for (int i = 0; i < 3; ++i)
    {
        m_modes[static_cast<size_t>(i)].setToggleState(i == mode, juce::dontSendNotification);
        m_modes[static_cast<size_t>(i)].setEnabled(! rendering);
    }
    for (auto* c : std::initializer_list<juce::Component*> { &m_count, &m_length, &m_seed, &m_newSeed, &m_choose, &m_dated, &m_render })
        c->setEnabled(! rendering);
    m_amount.setEnabled(! rendering && mode == static_cast<int>(Mode::SmallChanges));
    m_amount.setAlpha(m_amount.isEnabled() ? 1.0f : 0.35f);
    m_dated.setToggleState(m_processor.getVariationFolder().isEmpty(), juce::dontSendNotification);
    m_cancel.setEnabled(rendering);
    m_openTakes.setEnabled(m_shown.done > 0);
    repaint();
}

void VariationsView::startBatch()
{
    const auto j = job();
    m_count.setText(juce::String(j.count), false);
    m_length.setText(juce::String(j.seconds, 1), false);
    m_seed.setText(juce::String(j.batchSeed), false);
    m_error.clear();
    m_processor.setVariationAmount(j.amount);
    juce::String error;
    if (! m_processor.getVariations().start(j, error))
        m_error = error;
    m_shown = m_processor.getVariations().getProgress();
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
    {
        m_processor.refreshTakes(); // the last files are in Takes by the time Open Takes can be pressed
        if (m_seedIsOurs) // the next batch is a different one unless a seed was typed
            m_seed.setText(juce::String(m_processor.getVariations().defaultJob().batchSeed), false);
    }
    if (changed || ended)
        syncControls();
}

void VariationsView::mouseDown(const juce::MouseEvent& e)
{
    if (! m_panel.contains(e.getPosition()))
        setVisible(false);
}

void VariationsView::resized()
{
    m_panel = getLocalBounds().withSizeKeepingCentre(700, 440);
    auto r = m_panel.reduced(28, 0);
    r.removeFromTop(56);

    // Count, Length, Batch Seed
    {
        auto captions = r.removeFromTop(20);
        auto row = r.removeFromTop(28);
        r.removeFromTop(14);
        m_captions[0] = captions.removeFromLeft(90);
        m_count.setBounds(row.removeFromLeft(90));
        captions.removeFromLeft(28);
        row.removeFromLeft(28);
        m_captions[1] = captions.removeFromLeft(110);
        m_length.setBounds(row.removeFromLeft(70));
        m_lengthUnit = row.removeFromLeft(40).withTrimmedLeft(6);
        captions.removeFromLeft(28);
        row.removeFromLeft(28);
        m_captions[2] = captions.removeFromLeft(120);
        m_seed.setBounds(row.removeFromLeft(120));
        row.removeFromLeft(6);
        m_newSeed.setBounds(row.removeFromLeft(96));
    }
    // What Varies, Amount
    {
        auto captions = r.removeFromTop(20);
        auto row = r.removeFromTop(28);
        r.removeFromTop(14);
        const int widths[] = { 176, 124, 96 };
        m_captions[3] = captions.removeFromLeft(widths[0] + widths[1] + widths[2] + 12);
        for (int i = 0; i < 3; ++i)
        {
            m_modes[static_cast<size_t>(i)].setBounds(row.removeFromLeft(widths[i]));
            row.removeFromLeft(6);
        }
        captions.removeFromLeft(22);
        row.removeFromLeft(16);
        m_captions[4] = captions;
        m_amountValue = row.removeFromRight(46);
        m_amount.setBounds(row.reduced(0, 4));
    }
    // Folder
    {
        m_captions[5] = r.removeFromTop(20);
        m_folderText = r.removeFromTop(22);
        r.removeFromTop(2);
        auto row = r.removeFromTop(28);
        r.removeFromTop(10);
        m_choose.setBounds(row.removeFromLeft(104));
        row.removeFromLeft(6);
        m_dated.setBounds(row.removeFromLeft(116));
    }
    m_formatText = r.removeFromTop(20);
    r.removeFromTop(16);
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
    g.fillAll(TC::background.withAlpha(0.8f));
    SwooshLookAndFeel::drawPanel(g, m_panel.toFloat(), "Render Variations", "a batch of thrown whooshes, straight to files", TC::flight, m_fonts);

    const char* captions[] = { "Count", "Length", "Batch Seed", "What Varies", "Amount", "Folder" };
    g.setFont(m_fonts.label(10.5f));
    g.setColour(TC::textDim);
    for (size_t i = 0; i < m_captions.size(); ++i)
        g.drawText(captions[i], m_captions[i], juce::Justification::centredLeft);

    const bool small = m_processor.getVariationMode() == static_cast<int>(Mode::SmallChanges);
    g.setFont(m_fonts.body(12.5f));
    g.setColour(TC::textDim);
    g.drawText("s", m_lengthUnit, juce::Justification::centredLeft);
    g.setColour(small ? TC::text : TC::textFaint);
    g.drawText(juce::String(juce::roundToInt(100.0 * m_amount.getValue())) + "%", m_amountValue, juce::Justification::centredRight);

    // Where the files go, and what they are.
    {
        const auto font = m_fonts.body(12.0f);
        const bool dated = m_processor.getVariationFolder().isEmpty();
        g.setFont(font);
        g.setColour(TC::text);
        const auto path = dated ? folder().getParentDirectory().getFullPathName() + juce::File::getSeparatorString()
                                      + m_processor.getEffectiveTakeName() + " Variations, with the date and time"
                                : folder().getFullPathName();
        g.drawText(fitPath(font, path, m_folderText.getWidth()), m_folderText, juce::Justification::centredLeft, false);
        g.setFont(m_fonts.body(11.5f));
        g.setColour(TC::textFaint);
        g.drawFittedText(formatLine(), m_formatText, juce::Justification::centredLeft, 1);
    }

    // Progress: the bar with its percentage, then what is happening in words.
    const auto& p = m_shown;
    const bool rendering = p.state == State::Rendering;
    const int handled = p.done + p.silent;
    const float fraction = p.total > 0 ? juce::jlimit(0.0f, 1.0f, static_cast<float>(handled) / static_cast<float>(p.total)) : 0.0f;
    {
        const auto bar = m_bar.toFloat();
        g.setColour(TC::scope);
        g.fillRoundedRectangle(bar, 4.0f);
        if (p.state != State::Idle && fraction > 0.0f)
        {
            g.saveState();
            g.reduceClipRegion(bar.withWidth(bar.getWidth() * fraction).toNearestInt());
            g.setColour(p.state == State::Failed ? TC::record.withAlpha(0.75f)
                        : p.state == State::Cancelled ? TC::textFaint
                        : p.state == State::Finished ? TC::go.withAlpha(0.85f) : TC::flight.withAlpha(0.9f));
            g.fillRoundedRectangle(bar, 4.0f);
            g.restoreState();
        }
        g.setColour(TC::outline);
        g.drawRoundedRectangle(bar.reduced(0.5f), 4.0f, 1.0f);
        if (p.state != State::Idle)
        {
            const auto text = juce::String(juce::roundToInt(100.0f * fraction)) + "%";
            g.setFont(m_fonts.heading(11.5f));
            // Dark on the filled part, light on the empty part, so the number reads wherever the edge is.
            g.setColour(TC::text);
            g.drawText(text, m_bar, juce::Justification::centred, false);
            g.saveState();
            g.reduceClipRegion(bar.withWidth(bar.getWidth() * fraction).toNearestInt());
            g.setColour(TC::background);
            g.drawText(text, m_bar, juce::Justification::centred, false);
            g.restoreState();
        }
    }

    auto status = m_status;
    auto top = status.removeFromTop(18);
    g.setFont(m_fonts.body(12.5f));
    const auto folderName = p.folder.getFileName();
    const juce::String filesWord = p.done == 1 ? " file" : " files";
    const juce::String silentNote = p.silent > 0 ? " (" + juce::String(p.silent) + (p.silent == 1 ? " render" : " renders") + " had no sound and left no file)" : juce::String();
    if (m_error.isNotEmpty())
    {
        g.setColour(TC::record);
        g.drawFittedText(m_error, m_status, juce::Justification::topLeft, 2);
    }
    else if (rendering)
    {
        g.setColour(TC::text);
        g.drawText(juce::String(juce::jmin(p.total, handled + 1)) + " of " + juce::String(p.total), top.removeFromLeft(110), juce::Justification::centredLeft, false);
        g.setColour(TC::textDim);
        g.drawText(timeLeft(p.secondsLeft), top.removeFromRight(190), juce::Justification::centredRight, false);
        g.drawText(p.currentFile, top, juce::Justification::centred, true);
        g.setFont(m_fonts.body(11.5f));
        g.setColour(TC::textFaint);
        g.drawText("The rig keeps playing. You can close this panel: the button in the header shows how far the batch is.", status, juce::Justification::centredLeft, true);
    }
    else if (p.state == State::Finished)
    {
        g.setColour(TC::text);
        g.drawText(fitPath(g.getCurrentFont(), juce::String(p.done) + filesWord + " in " + folderName, top.getWidth()), top, juce::Justification::centredLeft, false);
        g.setFont(m_fonts.body(11.5f));
        g.setColour(TC::textFaint);
        g.drawText("Batch Seed " + juce::String(p.batchSeed) + ", rendered in " + juce::String(p.elapsedSeconds, 1) + " s" + silentNote + ".", status,
                   juce::Justification::centredLeft, true);
    }
    else if (p.state == State::Cancelled)
    {
        g.setColour(TC::text);
        g.drawText(fitPath(g.getCurrentFont(), "Cancelled. " + juce::String(p.done) + filesWord + " kept in " + folderName, top.getWidth()), top,
                   juce::Justification::centredLeft, false);
        g.setFont(m_fonts.body(11.5f));
        g.setColour(TC::textFaint);
        g.drawText("Batch Seed " + juce::String(p.batchSeed) + silentNote + ".", status, juce::Justification::centredLeft, true);
    }
    else if (p.state == State::Failed)
    {
        g.setColour(TC::record);
        g.drawFittedText(p.error + (p.done > 0 ? " " + juce::String(p.done) + filesWord + " kept in " + folderName + "." : juce::String()), m_status,
                         juce::Justification::topLeft, 2);
    }
    else
    {
        g.setFont(m_fonts.body(11.5f));
        g.setColour(TC::textFaint);
        g.drawFittedText("Each file is one thrown whoosh, rendered from silence until its echoes and Space have rung out, with the same output stage "
                         "as the tape. Rendering runs in the background, faster than real time.", m_status, juce::Justification::topLeft, 2);
    }
}
