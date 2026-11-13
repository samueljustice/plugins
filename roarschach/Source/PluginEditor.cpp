#include "PluginEditor.h"
#include "dsp/KnobLaws.h"

#include "AboutWindow.h"
#include "Params.h"
#include "UpdatePopup.h"

#include "SammyJUpdateCheck.h"

namespace
{
    // Design-size layout (everything is scaled uniformly from here).
    constexpr int c_margin = 16;
    constexpr int c_headerHeight = 56;
    constexpr int c_stripHeight = 336;
    constexpr int c_sectionGap = 12;
    constexpr float c_viewRadius = 8.0f;
    constexpr int c_displayReserve = 40;      // the least a display keeps for menus and a title bar
    constexpr int c_hostFrameReserve = 110;   // a host's plug-in window: title bar and its own header
    constexpr float c_hostWidthShare = 0.95f; // ... and the share of the display's width a first window takes
    constexpr double c_rememberAfterMs = 800.0;

    const juce::Identifier c_viewEditorScale { "viewEditorScale" };
    const juce::Identifier c_viewHintDone { "viewHintDone" };
    const char* const c_settingsScaleKey = "editorScale";
    const juce::Identifier c_undoFocus { "undoFocus" };   // a control Cmd+Z works from while it has the focus

    const char* const c_hintText = "Play a voice through it to wake the creature. Pick a preset or roll a die to change it.";

    juce::InterProcessLock& settingsLock()
    {
        static juce::InterProcessLock lock("SammyJRoarschachSettings");
        return lock;
    }

    juce::PropertiesFile::Options settingsOptions()
    {
        juce::PropertiesFile::Options options;
        options.applicationName = "Roarschach";
        options.folderName = "Sammy J";
        options.filenameSuffix = "settings";
        options.osxLibrarySubFolder = "Application Support";
        options.commonToAllUsers = false;
        options.millisecondsBeforeSaving = -1; // saved explicitly
        options.processLock = &settingsLock();
        return options;
    }

    bool hasUndoFocus(const juce::Component* c)
    {
        return c != nullptr && (dynamic_cast<const DiceButton*>(c) != nullptr || c->getProperties().contains(c_undoFocus));
    }

    const juce::String c_title = "ROARSCHACH";
    const juce::String c_taglineText = juce::String(Theme::tagline) + "  /";

    // Preset menu item IDs: factory presets are 1..getNumFactory() (they lead the preset list in a
    // fixed order, so their index is stable), user presets count up from c_firstUserItemId.
    constexpr int c_saveAsItemId = 1001, c_deleteItemId = 1002, c_revealItemId = 1003, c_undoItemId = 1004, c_redoItemId = 1005;
    constexpr int c_firstUserItemId = 100000;
}

//==============================================================================
RoarschachAudioProcessorEditor::RoarschachAudioProcessorEditor(RoarschachAudioProcessor& audioProcessor)
    : AudioProcessorEditor(&audioProcessor)
    , m_audioProcessor(audioProcessor)
    , m_lookAndFeel(*m_fonts)
    , m_monsterView(audioProcessor.getMonsterFeatures(), &audioProcessor.getCreatureShared())
    , m_strip(audioProcessor.getValueTreeState(), audioProcessor.getMonsterFeatures(), *m_fonts, &audioProcessor.getPositionWrites())
{
    setLookAndFeel(&m_lookAndFeel);
    m_tooltipWindow.setLookAndFeel(&m_lookAndFeel);
    RoarschachLookAndFeel::showFocusRingsAfterTab(*this); // keyboard focus rings after Tab, hidden by a click

    m_content.onPaint = [this](juce::Graphics& g) { paintContent(g); };
    m_content.onResized = [this] { layoutContent(); };
    addAndMakeVisible(m_content);

    m_monsterView.setCorner(c_viewRadius, Theme::Colours::background);
    m_monsterView.isCreatureFrozen = [&audioProcessor] { return audioProcessor.isCreatureFrozen(); };
    m_monsterView.setCreatureFrozen = [&audioProcessor](bool frozen) { audioProcessor.setCreatureFrozen(frozen); };
    m_monsterView.resetCreature = [&audioProcessor] { audioProcessor.resetCreature(); };
    m_monsterView.keepCreatureColour = [&audioProcessor](std::optional<roar::CreatureMood::Axes> colour) { audioProcessor.setCreatureColour(colour); };
    // One line of help until this instance has heard audio once (kept with the session, so it stays away).
    if (!static_cast<bool>(audioProcessor.getViewValue(c_viewHintDone, false)))
    {
        m_monsterView.setHint(c_hintText);
        m_monsterView.onAudioHeard = [&audioProcessor] { audioProcessor.setViewValue(c_viewHintDone, true); };
    }
    updateMonsterParams();
    m_content.addAndMakeVisible(m_monsterView);
    m_content.addAndMakeVisible(m_strip);

    m_aboutButton.setButtonText("?");
    m_aboutButton.setTooltip("About Roarschach, and a quick start");
    m_aboutButton.onClick = [this] { showAboutWindow(); };
    m_content.addAndMakeVisible(m_aboutButton);

    setupPresetBar();
    setupDice();

    m_websiteLink.setFont(m_fonts->label(10.5f), false, juce::Justification::centredLeft);
    m_websiteLink.setColour(juce::HyperlinkButton::textColourId, Theme::Colours::textDim); // amber on hover (ThemeLink)
    m_websiteLink.setTooltip("Open sweetjusticesound.com");
    m_content.addAndMakeVisible(m_websiteLink);

    // Resizable, uniformly scaled, fixed aspect ratio; the size is part of the saved state.
    // Read the saved scale first: setting the limits below resizes the editor, which
    // would otherwise overwrite it. A damaged value (NaN, inf) must not give a 0 x 0 editor.
    // The largest scale also stays within the largest display's work area, so the window can never
    // be wider or taller than the screen (the design is 1760 px wide; 125 % would be 2200 px).
    float maxScale = c_maxScale;
    {
        float fits = 0.0f;
        for (const auto& display : juce::Desktop::getInstance().getDisplays().displays)
        {
            const auto area = display.userArea;
            if (area.getWidth() > 0 && area.getHeight() > c_displayReserve)
                fits = std::max(fits, std::min(static_cast<float>(area.getWidth()) / static_cast<float>(c_baseWidth),
                                               static_cast<float>(area.getHeight() - c_displayReserve) / static_cast<float>(c_baseHeight)));
        }
        if (fits > 0.0f)
            maxScale = juce::jlimit(c_minScale, c_maxScale, fits);
    }
    // Only in a host or the standalone app: tests and tools build the editor around a bare processor
    // and must neither read nor change the size this computer remembers.
    m_useMachineSettings = audioProcessor.wrapperType != juce::AudioProcessor::wrapperType_Undefined;

    // This instance's own size (a session), else the size last chosen on this computer, else the
    // largest default that leaves room on the main display.
    const auto usable = [](double v) { return std::isfinite(v) && v > 0.0; };
    double wanted = static_cast<double>(m_audioProcessor.getViewValue(c_viewEditorScale, 0.0));
    if (!usable(wanted) && m_useMachineSettings)
        wanted = static_cast<double>(readRememberedScale(getSettingsFile()));
    if (!usable(wanted))
    {
        wanted = c_smallestDefaultScale;
        if (const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
            wanted = defaultScaleForDisplay(display->userArea.getWidth(), display->userArea.getHeight());
    }
    const float scale = static_cast<float>(juce::jlimit<double>(c_minScale, maxScale, wanted));
    m_openedScale = scale;
    setResizable(true, true);
    setResizeLimits(juce::roundToInt(c_baseWidth * c_minScale), juce::roundToInt(c_baseHeight * c_minScale),
                    juce::roundToInt(c_baseWidth * maxScale), juce::roundToInt(c_baseHeight * maxScale));
    if (auto* sizeConstrainer = getConstrainer())
        sizeConstrainer->setFixedAspectRatio(static_cast<double>(c_baseWidth) / c_baseHeight);

    setSize(juce::roundToInt(c_baseWidth * scale), juce::roundToInt(c_baseHeight * scale));
    m_sizeRestored = true;

    startTimerHz(30);

    // Look for a newer release once the UI is up (a background thread; silent if it can't connect).
    // Only in a host or the standalone app: not when tests or tools build the editor around a bare processor.
    if (audioProcessor.wrapperType != juce::AudioProcessor::wrapperType_Undefined)
        m_updateCheck = std::make_unique<sammyj::UpdateCheck>("roarschach", "roarschach-v", JucePlugin_VersionString,
                                                              [this](const sammyj::UpdateInfo& info) { showUpdatePopup(info); });
}

RoarschachAudioProcessorEditor::~RoarschachAudioProcessorEditor()
{
    stopTimer();
    removeMouseListener(&m_diceFocusRelease);
    rememberSizeNow();

    // The update check first (its thread stops, its callback never runs), then its popup.
    m_updateCheck.reset();
    m_updatePopup.reset();

    // Nothing the editor opened may outlive it: the About window (and its update check), any
    // popup menu (it would keep using m_lookAndFeel), alerts and the tooltip window.
    m_aboutWindow.reset();
    juce::PopupMenu::dismissAllActiveMenus();
    m_alert.reset();
    m_tooltipWindow.hideTip();
    m_tooltipWindow.setLookAndFeel(nullptr);
    setLookAndFeel(nullptr);
}

//==============================================================================
RoarschachAudioProcessorEditor::EditorTooltipWindow::EditorTooltipWindow(juce::Component& editor)
    : juce::TooltipWindow(nullptr, 700) // no parent: a desktop window of its own, above the GL view
    , m_editor(editor)
{
    setOpaque(false); // the LookAndFeel draws a rounded panel; the corners show what is behind the tip
}

juce::String RoarschachAudioProcessorEditor::EditorTooltipWindow::getTipFor(juce::Component& component)
{
    return m_editor.isParentOf(&component) ? juce::TooltipWindow::getTipFor(component) : juce::String();
}

float RoarschachAudioProcessorEditor::EditorTooltipWindow::getDesktopScaleFactor() const
{
    return juce::Component::getApproximateScaleFactorForComponent(&m_editor);
}

//==============================================================================
void RoarschachAudioProcessorEditor::resized()
{
    // Uniform scale of the design-size content to the editor's current size.
    const float scale = static_cast<float>(getWidth()) / static_cast<float>(c_baseWidth);
    m_content.setBounds(0, 0, c_baseWidth, c_baseHeight);
    m_content.setTransform(juce::AffineTransform::scale(scale));

    if (m_sizeRestored)
    {
        m_audioProcessor.setViewValue(c_viewEditorScale, scale);

        // A size the user chose is also the size the next new instance opens at (written once the
        // drag has come to rest, not on every step of it).
        if (std::abs(scale - m_openedScale) > 0.004f)
        {
            m_scaleToRemember = scale;
            m_rememberAtMs = juce::Time::getMillisecondCounterHiRes() + c_rememberAfterMs;
        }
    }

    if (m_updatePopup != nullptr)
        m_updatePopup->setBounds(getLocalBounds()); // the card stays centred
}

void RoarschachAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(Theme::Colours::background);
}

void RoarschachAudioProcessorEditor::layoutContent()
{
    auto bounds = m_content.getLocalBounds();

    auto header = bounds.removeFromTop(c_headerHeight).reduced(c_margin, 0);
    m_aboutButton.setBounds(header.removeFromRight(26).withSizeKeepingCentre(26, 26));

    // Website link sits after the title and tagline (widths match paintContent()).
    {
        const float titleWidth = juce::GlyphArrangement::getStringWidth(m_fonts->heading(20.0f), c_title);
        const float taglineWidth = juce::GlyphArrangement::getStringWidth(m_fonts->label(10.5f), c_taglineText);
        const int linkX = c_margin + 30 + 12 + juce::roundToInt(titleWidth) + 4 + 14 + juce::roundToInt(taglineWidth) + 8;
        const int linkWidth = juce::roundToInt(juce::GlyphArrangement::getStringWidth(m_fonts->label(10.5f), "SWEETJUSTICESOUND.COM")) + 6;
        m_websiteLink.setBounds(linkX, 2 + (c_headerHeight - 20) / 2, linkWidth, 20);
    }

    // Preset bar, right-aligned next to the About button: < name > die SAVE.
    header.removeFromRight(14);
    auto presetRow = header.removeFromRight(430).withSizeKeepingCentre(430, 26);
    m_presetSaveButton.setBounds(presetRow.removeFromRight(64));
    presetRow.removeFromRight(8);
    m_presetDice.setBounds(presetRow.removeFromRight(26));
    presetRow.removeFromRight(4);
    m_presetNextButton.setBounds(presetRow.removeFromRight(26));
    presetRow.removeFromRight(4);
    m_presetPrevButton.setBounds(presetRow.removeFromLeft(26));
    presetRow.removeFromLeft(4);
    m_presetNameButton.setBounds(presetRow);

    bounds.reduce(c_margin, 0);
    bounds.removeFromBottom(c_margin);

    m_strip.setBounds(bounds.removeFromBottom(c_stripHeight));
    bounds.removeFromBottom(c_sectionGap);
    m_monsterView.setBounds(bounds);
}

void RoarschachAudioProcessorEditor::paintContent(juce::Graphics& g)
{
    using namespace Theme::Colours;

    g.fillAll(background);

    // Header: the mark, the name and the tagline (the website link and the preset bar are components).
    auto header = m_content.getLocalBounds().removeFromTop(c_headerHeight).reduced(c_margin, 0).toFloat();

    RoarschachLookAndFeel::drawMark(g, header.removeFromLeft(30.0f).withSizeKeepingCentre(30.0f, 22.0f));
    header.removeFromLeft(12.0f);

    const auto titleFont = m_fonts->heading(20.0f);
    g.setFont(titleFont);
    g.setColour(text);
    const float titleWidth = juce::GlyphArrangement::getStringWidth(titleFont, c_title);
    g.drawText(c_title, header.removeFromLeft(titleWidth + 4.0f), juce::Justification::centredLeft, false);

    header.removeFromLeft(14.0f);
    g.setFont(m_fonts->label(10.5f));
    g.setColour(textFaint);
    g.drawText(c_taglineText, header.withTrimmedTop(4.0f), juce::Justification::centredLeft, false);
}

void RoarschachAudioProcessorEditor::timerCallback()
{
    updatePresetDisplay();
    updateMonsterParams();

    if (m_scaleToRemember > 0.0f && juce::Time::getMillisecondCounterHiRes() >= m_rememberAtMs)
        rememberSizeNow();
}

//==============================================================================
// The window size
//==============================================================================
float RoarschachAudioProcessorEditor::defaultScaleForDisplay(int workAreaWidth, int workAreaHeight) noexcept
{
    if (workAreaWidth <= 0 || workAreaHeight <= c_hostFrameReserve)
        return c_smallestDefaultScale;

    const float fits = std::min(c_hostWidthShare * static_cast<float>(workAreaWidth) / static_cast<float>(c_baseWidth),
                                static_cast<float>(workAreaHeight - c_hostFrameReserve) / static_cast<float>(c_baseHeight));
    // In steps of 5 %, so the type lands on the same sizes from one display to the next.
    const float stepped = std::floor(fits * 20.0f + 0.001f) / 20.0f;
    return juce::jlimit(c_smallestDefaultScale, c_largestDefaultScale, stepped);
}

juce::File RoarschachAudioProcessorEditor::getSettingsFile()
{
    return settingsOptions().getDefaultFile();
}

float RoarschachAudioProcessorEditor::readRememberedScale(const juce::File& settingsFile)
{
    if (!settingsFile.existsAsFile())
        return 0.0f;

    juce::PropertiesFile settings(settingsFile, settingsOptions());
    const double scale = settings.getDoubleValue(c_settingsScaleKey, 0.0);
    return std::isfinite(scale) && scale > 0.0 ? juce::jlimit(c_minScale, c_maxScale, static_cast<float>(scale)) : 0.0f;
}

bool RoarschachAudioProcessorEditor::rememberScale(const juce::File& settingsFile, float scale)
{
    if (!std::isfinite(scale) || scale <= 0.0f)
        return false;

    juce::PropertiesFile settings(settingsFile, settingsOptions());
    settings.setValue(c_settingsScaleKey, static_cast<double>(juce::jlimit(c_minScale, c_maxScale, scale)));
    return settings.save();
}

void RoarschachAudioProcessorEditor::rememberSizeNow()
{
    const float scale = std::exchange(m_scaleToRemember, 0.0f);
    if (scale > 0.0f && m_useMachineSettings)
        rememberScale(getSettingsFile(), scale);
}

juce::String RoarschachAudioProcessorEditor::latencyNote(int latencySamples, double sampleRate)
{
    if (latencySamples <= 0 || !std::isfinite(sampleRate) || sampleRate <= 0.0)
        return {};

    const double kHz = sampleRate / 1000.0;
    return juce::String(1000.0 * latencySamples / sampleRate, 1) + " ms (" + juce::String(latencySamples) + " samples at "
         + (std::abs(kHz - std::round(kHz)) < 0.001 ? juce::String(juce::roundToInt(kHz)) : juce::String(kHz, 1)) + " kHz)";
}

void RoarschachAudioProcessorEditor::updateMonsterParams()
{
    auto& parameters = m_audioProcessor.getValueTreeState();
    const auto value = [&parameters](const char* id)
    {
        const auto* p = parameters.getRawParameterValue(id);
        const float v = p != nullptr ? p->load(std::memory_order_relaxed) : Params::defaultOf(id);
        return std::isfinite(v) ? v : Params::defaultOf(id);
    };
    const auto on = [&value](const char* id) { return value(id) >= 0.5f ? 1.0f : 0.0f; };
    const auto percent = [&value](const char* id) { return juce::jlimit(0.0f, 1.0f, value(id) / 100.0f); };

    // The creature reads the knobs of the modules that are On (an Off module reads 0), as the
    // effect they have (roar::laws::view), so it follows the sound rather than the knob travel.
    namespace view = roar::laws::view;
    const auto effect = [&percent](double (*law)(double), const char* id) { return static_cast<float>(law(static_cast<double>(percent(id)))); };
    MonsterView::Params params;
    params.pitchSt = on(Params::pitchOn) * juce::jlimit(-24.0f, 12.0f, value(Params::pitch));
    params.size = on(Params::pitchOn) * percent(Params::pitchSize);
    params.growl = on(Params::growlOn) * effect(view::growl, Params::growl);
    params.liquid = on(Params::shimmerOn) * effect(view::shimmer, Params::shimmer);   // skin ripple and drips
    params.swirl = on(Params::swirlOn) * effect(view::swirl, Params::swirl);
    params.ensemble = on(Params::ensembleOn) * effect(view::ensemble, Params::ensemble);
    params.overtone = on(Params::overtoneOn) * effect(view::overtone, Params::overtone);
    params.ring = on(Params::ringOn) * percent(Params::ring);
    params.cleanOn = on(Params::cleanOn);
    // (the edge is tidied only by containment beyond the usual fifth: the usual setting leaves the ink as it is)
    params.clean = on(Params::cleanOn) * juce::jlimit(0.0f, 1.0f, (percent(Params::cleanContain) - 0.22f) / 0.78f);
    params.roar = on(Params::roarOn) * effect(view::roar, Params::roar);
    params.sub = on(Params::subOn) * effect(view::sub, Params::sub);
    params.gravel = on(Params::vocoderOn) * effect(view::gravel, Params::gravel);
    params.detailLow = on(Params::detailOn) * effect(view::detail, Params::detailLow);
    params.detailHigh = on(Params::detailOn) * effect(view::detail, Params::detailHigh);
    // The clouds' colour mood follows the settings (the view drifts to it slowly) and holds while
    // the creature is Frozen.
    params.mood = roar::CreatureMood::evaluate(roar::CreatureMood::Inputs::read(value));
    params.moodFrozen = m_audioProcessor.isCreatureFrozen();
    if (params.moodFrozen)
        params.moodKept = m_audioProcessor.getCreatureColour();
    m_monsterView.setParams(params);
}

void RoarschachAudioProcessorEditor::showAboutWindow()
{
    // One About window per editor: another click brings it back to the front. The editor owns it,
    // so it is closed (and its update check stopped) when the editor goes.
    if (m_aboutWindow != nullptr)
    {
        m_aboutWindow->setMinimised(false);
        m_aboutWindow->toFront(true);
        return;
    }

    m_aboutWindow = std::make_unique<AboutWindow>(m_lookAndFeel, [this] { m_aboutWindow.reset(); },
                                                  latencyNote(m_audioProcessor.getLatencySamples(), m_audioProcessor.getSampleRate()));
}

void RoarschachAudioProcessorEditor::showUpdatePopup(const sammyj::UpdateInfo& info)
{
    if (m_updatePopup != nullptr)
        return;

    m_updatePopup = std::make_unique<UpdatePopup>(m_lookAndFeel, info, [this] { closeUpdatePopup(); });

    // The creature's OpenGL view is a native view on top of the editor's window, so nothing the editor
    // paints can cover it: it is hidden while the popup is up (its context comes back when it is shown).
    m_monsterView.setVisible(false);

    addAndMakeVisible(*m_updatePopup); // the last child: above the content (and the corner resizer)
    m_updatePopup->setBounds(getLocalBounds());
    m_updatePopup->toFront(false);

    if (isShowing())
        m_updatePopup->grabKeyboardFocus(); // Escape and Return work at once; Tab moves through the buttons
}

void RoarschachAudioProcessorEditor::closeUpdatePopup()
{
    m_updatePopup.reset();
    m_monsterView.setVisible(true);
}

//==============================================================================
// Presets
//==============================================================================
void RoarschachAudioProcessorEditor::setupPresetBar()
{
    auto& presets = m_audioProcessor.getPresetManager();

    m_presetPrevButton.setButtonText("<");
    m_presetPrevButton.setTooltip("Previous preset");
    m_presetPrevButton.onClick = [this] { m_audioProcessor.getDice().loadPrevious(); afterPresetLoad(false); };

    m_presetNextButton.setButtonText(">");
    m_presetNextButton.setTooltip("Next preset");
    m_presetNextButton.onClick = [this] { m_audioProcessor.getDice().loadNext(); afterPresetLoad(false); };

   #if JUCE_MAC
    const juce::String undoKey = "Cmd+Z";
   #else
    const juce::String undoKey = "Ctrl+Z";
   #endif
    m_presetNameButton.setTooltip("Choose a preset, or save, delete and find your presets. Undo Load in this menu (or " + undoKey
                                  + " straight after loading) brings back the sound you had, edits and all.");
    m_presetNameButton.onClick = [this] { showPresetMenu(); };

    m_presetSaveButton.setButtonText("SAVE");
    m_presetSaveButton.setTooltip("Save the current settings as a preset");
    m_presetSaveButton.onClick = [this]
    {
        auto& pm = m_audioProcessor.getPresetManager();
        pm.refresh(); // a preset renamed or removed outside the plugin is not written back blindly
        const auto current = pm.getSnapshot();

        // Editing a user preset that is still in the folder: Save writes back to its file.
        // Otherwise ask for a name (and before replacing anything).
        if (current.currentIsUser && current.currentIndex >= 0)
            savePresetAs(current.currentName);
        else
            promptSavePreset(current.currentName == "Init" ? juce::String() : current.currentName);
    };

    for (auto* b : { &m_presetPrevButton, &m_presetNameButton, &m_presetNextButton, &m_presetSaveButton })
        m_content.addAndMakeVisible(b);
    for (auto* b : { &m_presetPrevButton, &m_presetNameButton, &m_presetNextButton })
        b->getProperties().set(c_undoFocus, true);

    presets.refresh();
    updatePresetDisplay();
}

//==============================================================================
// Dice
//==============================================================================
void RoarschachAudioProcessorEditor::setupDice()
{
    const auto undoHint = SignalFlowStrip::diceUndoHint();
    m_presetDice.setTooltip("Loads a random factory preset, never the one showing. Every factory preset comes up once "
                            "before any comes up again. " + undoHint);
    m_presetDice.setTitle("Random Preset");
    m_presetDice.setAccent(Theme::Colours::output);
    m_presetDice.onClick = [this] { m_audioProcessor.getDice().rollPreset(); afterRoll(); };
    m_presetDice.onSecondaryClick = [this] { showDiceMenu(m_presetDice); };
    m_presetDice.onUndoKey = [this](bool redo) { return diceUndoKey(redo); };
    m_content.addAndMakeVisible(m_presetDice);

    m_strip.onRollAll = [this] { m_audioProcessor.getDice().rollAll(); afterRoll(); };
    m_strip.onRollModule = [this](int module) { m_audioProcessor.getDice().rollModule(module); afterRoll(); };
    m_strip.onDiceMenu = [this](juce::Component& die) { showDiceMenu(die); };
    m_strip.onDiceUndoKey = [this](bool redo) { return diceUndoKey(redo); };
    addMouseListener(&m_diceFocusRelease, true);
    afterRoll();
}

void RoarschachAudioProcessorEditor::afterRoll()
{
    const int face = m_audioProcessor.getDice().getFace();
    m_presetDice.setFace(face);
    m_strip.setDiceFace(face);
    updatePresetDisplay();
}

void RoarschachAudioProcessorEditor::showDiceMenu(juce::Component& die)
{
    auto& dice = m_audioProcessor.getDice();
    auto& undo = dice.getUndoManager();
    juce::PopupMenu menu;
    menu.setLookAndFeel(&m_lookAndFeel);
    menu.addItem(1, dice.canUndo() ? "Undo " + undo.getUndoDescription() : juce::String("Undo Roll"), dice.canUndo());
    menu.addItem(2, dice.canRedo() ? "Redo " + undo.getRedoDescription() : juce::String("Redo Roll"), dice.canRedo());
    menu.showMenuAsync(RoarschachLookAndFeel::menuOptionsFor(die),
                       [safe = juce::Component::SafePointer<RoarschachAudioProcessorEditor>(this)](int result)
                       {
                           if (safe == nullptr || result == 0)
                               return;
                           auto& d = safe->m_audioProcessor.getDice();
                           if (result == 1)
                               d.undo();
                           else if (result == 2)
                               d.redo();
                           safe->afterRoll();
                       });
}

bool RoarschachAudioProcessorEditor::diceUndoKey(bool redo)
{
    // Only the plug-in's own history (rolls and preset loads): anything else is the host's to undo.
    auto& dice = m_audioProcessor.getDice();
    const bool done = redo ? dice.redo() : dice.undo();
    afterRoll();
    return done;
}

bool RoarschachAudioProcessorEditor::undoKeyPressed(const juce::KeyPress& key)
{
    const bool isUndo = key == juce::KeyPress('z', juce::ModifierKeys::commandModifier, 0);
    const bool isRedo = key == juce::KeyPress('z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0);
    return (isUndo || isRedo) && diceUndoKey(isRedo);
}

bool RoarschachAudioProcessorEditor::keyPressed(const juce::KeyPress& key)
{
    // Keys reach the editor from a focused control that did not use them. A text field uses Cmd+Z
    // itself (and never passes it on); from a knob's value box or any other control it is the host's.
    // Only straight after a preset load (the preset bar still has the focus) is it the load's.
    auto* focused = juce::Component::getCurrentlyFocusedComponent();
    if (dynamic_cast<juce::TextEditor*>(focused) != nullptr || !(focused == this || hasUndoFocus(focused)))
        return false;
    return undoKeyPressed(key);
}

void RoarschachAudioProcessorEditor::afterPresetLoad(bool fromMenu)
{
    afterRoll();
    // The menu took the focus while it was up: back to the preset name, so Cmd+Z takes the load back.
    if (fromMenu && m_presetNameButton.isShowing())
        m_presetNameButton.grabKeyboardFocus();
}

void RoarschachAudioProcessorEditor::DiceFocusRelease::release(const juce::MouseEvent& e)
{
    auto* focused = juce::Component::getCurrentlyFocusedComponent();
    if (!hasUndoFocus(focused) || e.eventComponent == nullptr)
        return;
    for (auto* c = e.eventComponent; c != nullptr; c = c->getParentComponent())
        if (hasUndoFocus(c))
            return;   // a die or the preset bar itself (it takes the focus)
    focused->giveAwayKeyboardFocus();
}

void RoarschachAudioProcessorEditor::updatePresetDisplay()
{
    const auto& pm = m_audioProcessor.getPresetManager();
    const auto text = pm.getCurrentName() + (pm.isModified() ? " *" : "");

    if (m_presetNameButton.getButtonText() != text)
        m_presetNameButton.setButtonText(text);
}

void RoarschachAudioProcessorEditor::showPresetMenu()
{
    auto& pm = m_audioProcessor.getPresetManager();
    pm.refresh(); // pick up presets added or removed outside the plugin

    // One consistent view of the list and the current preset (a host may change either meanwhile).
    const auto snapshot = pm.getSnapshot();
    const auto& presets = snapshot.presets;
    const auto isCurrentFactory = [&snapshot](int i) { return snapshot.currentIsFactory && i == snapshot.currentIndex; };

    juce::PopupMenu menu;
    menu.setLookAndFeel(&m_lookAndFeel); // submenus take it from their parent menu
    menu.addSectionHeader("FACTORY");

    // Top-level factory presets (Init), then one submenu per category, in a fixed order.
    for (int i = 0; i < presets.size(); ++i)
        if (presets[i].isFactory && presets[i].category.isEmpty())
            menu.addItem(i + 1, presets[i].name, true, isCurrentFactory(i));

    // The categories in the order the factory list first names them (every category gets its submenu).
    juce::StringArray categories;
    for (const auto& preset : presets)
        if (preset.isFactory && preset.category.isNotEmpty())
            categories.addIfNotAlreadyThere(preset.category);

    for (const auto& category : categories)
    {
        juce::PopupMenu sub;
        bool containsCurrent = false;

        for (int i = 0; i < presets.size(); ++i)
        {
            if (presets[i].isFactory && presets[i].category == category)
            {
                sub.addItem(i + 1, presets[i].name, true, isCurrentFactory(i));
                containsCurrent = containsCurrent || isCurrentFactory(i);
            }
        }

        if (sub.getNumItems() > 0)
            menu.addSubMenu(category, sub, true, nullptr, containsCurrent);
    }

    // User presets are told apart by their file: two can carry the same name, in which case
    // menuName adds the file name. Items load (and Delete removes) exactly that file.
    juce::Array<juce::File> userFiles;
    juce::PopupMenu user;
    bool userContainsCurrent = false;
    juce::String currentMenuName = snapshot.currentName;

    for (const auto& preset : presets)
    {
        if (preset.isFactory)
            continue;

        const bool isCurrent = snapshot.currentIsUser && preset.file == snapshot.currentFile;
        user.addItem(c_firstUserItemId + userFiles.size(), preset.menuName, true, isCurrent);
        userFiles.add(preset.file);

        if (isCurrent)
        {
            userContainsCurrent = true;
            currentMenuName = preset.menuName;
        }
    }

    menu.addSectionHeader("USER");
    if (!userFiles.isEmpty())
        menu.addSubMenu("User Presets", user, true, nullptr, userContainsCurrent); // keeps long lists tidy
    else
        menu.addItem(-1, "(none saved yet)", false);

    // Delete is bound to the preset named here, even if another one is current by the time it is confirmed.
    const auto fileToDelete = snapshot.currentIsUser ? snapshot.currentFile : juce::File();

    // The last roll or preset load, and back again (the same history as the dice's menu).
    {
        auto& dice = m_audioProcessor.getDice();
        auto& undo = dice.getUndoManager();
        menu.addSeparator();
        menu.addItem(c_undoItemId, dice.canUndo() ? "Undo " + undo.getUndoDescription() : juce::String("Undo Load"), dice.canUndo());
        menu.addItem(c_redoItemId, dice.canRedo() ? "Redo " + undo.getRedoDescription() : juce::String("Redo Load"), dice.canRedo());
    }

    menu.addSeparator();
    menu.addItem(c_saveAsItemId, "Save As...");
    menu.addItem(c_deleteItemId, "Delete \"" + currentMenuName + "\"", fileToDelete != juce::File());
    menu.addItem(c_revealItemId, "Show Presets Folder");

    menu.showMenuAsync(RoarschachLookAndFeel::menuOptionsFor(m_presetNameButton, m_presetNameButton.getWidth()),
                       [safe = juce::Component::SafePointer<RoarschachAudioProcessorEditor>(this), userFiles, fileToDelete,
                        nameToDelete = currentMenuName](int result)
    {
        if (safe == nullptr || result == 0)
            return;

        auto& presetManager = safe->m_audioProcessor.getPresetManager();

        if (result == c_undoItemId || result == c_redoItemId)
        {
            safe->diceUndoKey(result == c_redoItemId);
            safe->afterPresetLoad(true);
        }
        else if (result == c_saveAsItemId)
        {
            safe->promptSavePreset(presetManager.isCurrentUserPreset() ? presetManager.getCurrentName() : juce::String());
        }
        else if (result == c_deleteItemId)
        {
            safe->confirmDeletePreset(fileToDelete, nameToDelete);
        }
        else if (result == c_revealItemId)
        {
            const auto dir = PresetManager::getUserPresetDirectory();
            dir.createDirectory();
            dir.startAsProcess(); // opens the folder in Finder / Explorer
        }
        else if (result >= c_firstUserItemId)
        {
            if (const int i = result - c_firstUserItemId; i < userFiles.size())
                safe->m_audioProcessor.getDice().loadUserPreset(userFiles[i]);

            safe->afterPresetLoad(true);
        }
        else if (result >= 1 && result <= PresetManager::getNumFactory())
        {
            safe->m_audioProcessor.getDice().loadPreset(result - 1); // factory presets lead the list, in a fixed order
            safe->afterPresetLoad(true);
        }
    });
}

void RoarschachAudioProcessorEditor::confirmDeletePreset(const juce::File& file, const juce::String& displayName)
{
    auto alert = makeAlert("Delete Preset", "Delete \"" + displayName + "\"? This removes the preset file.",
                           juce::MessageBoxIconType::WarningIcon);
    alert->addButton("Delete", 1, juce::KeyPress(juce::KeyPress::returnKey));
    alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    showAlert(std::move(alert), [safe = juce::Component::SafePointer<RoarschachAudioProcessorEditor>(this), file](int result, juce::AlertWindow&)
    {
        if (safe == nullptr || result != 1)
            return;

        // Exactly the file the dialog named, whatever the host has recalled since. Nothing
        // happens if it has gone (deleteUserPreset() returns false).
        safe->m_audioProcessor.getPresetManager().deleteUserPreset(file);
        safe->updatePresetDisplay();
    });
}

void RoarschachAudioProcessorEditor::promptSavePreset(const juce::String& suggestedName)
{
    auto alert = makeAlert("Save Preset", "Name this preset:", juce::MessageBoxIconType::NoIcon);
    alert->addTextEditor("name", suggestedName);
    if (auto* field = alert->getTextEditor("name"))
    {
        field->setJustification(juce::Justification::centredLeft); // vertically centred in the field
        field->setIndents(8, 0);
    }
    alert->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    showAlert(std::move(alert), [safe = juce::Component::SafePointer<RoarschachAudioProcessorEditor>(this)](int result, juce::AlertWindow& window)
    {
        if (safe == nullptr || result != 1)
            return;

        const auto name = window.getTextEditorContents("name").trim(); // read before another alert replaces this one
        if (name.isNotEmpty())
            safe->checkNameAndSave(name);
    });
}

void RoarschachAudioProcessorEditor::checkNameAndSave(const juce::String& name)
{
    auto& pm = m_audioProcessor.getPresetManager();

    if (!PresetManager::isValidPresetName(name))
    {
        // E.g. "???": nothing is left once the characters a file name can't hold are dropped.
        auto alert = makeAlert("Can't Use This Name",
                               "\"" + name + "\" can't be used as a preset name, as it has no characters "
                               "a file name can hold. Please use letters or numbers.",
                               juce::MessageBoxIconType::WarningIcon);
        alert->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
        alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        showAlert(std::move(alert), [safe = juce::Component::SafePointer<RoarschachAudioProcessorEditor>(this), name](int result, juce::AlertWindow&)
        {
            if (safe != nullptr && result == 1)
                safe->promptSavePreset(name); // try again, with the name as typed
        });
        return;
    }

    // Ask before replacing ANY existing file: a preset with this name, or a file this name maps to
    // (file names ignore case, and characters such as '/' are dropped, so "AB" replaces "A/B").
    if (pm.userPresetExists(name))
        confirmOverwrite(name, pm.getNameOfPresetSavedAs(name));
    else
        savePresetAs(name);
}

void RoarschachAudioProcessorEditor::confirmOverwrite(const juce::String& name, const juce::String& replacedName)
{
    const auto message = replacedName.isEmpty() || replacedName == name
                       ? "A preset called \"" + name + "\" already exists. Replace it?"
                       : "Saving as \"" + name + "\" replaces the preset \"" + replacedName + "\" (the two names give the same "
                         "file name). Replace \"" + replacedName + "\"?";

    auto alert = makeAlert("Replace Preset", message, juce::MessageBoxIconType::QuestionIcon);
    alert->addButton("Replace", 1, juce::KeyPress(juce::KeyPress::returnKey));
    alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    showAlert(std::move(alert), [safe = juce::Component::SafePointer<RoarschachAudioProcessorEditor>(this), name, replacedName](int result, juce::AlertWindow&)
    {
        if (safe == nullptr || result != 1)
            return;

        // The folder may have changed while the question was up: ask again if the save would now
        // replace a different preset.
        const auto nowReplaced = safe->m_audioProcessor.getPresetManager().getNameOfPresetSavedAs(name);
        if (nowReplaced.isNotEmpty() && nowReplaced != replacedName)
            safe->confirmOverwrite(name, nowReplaced);
        else
            safe->savePresetAs(name);
    });
}

void RoarschachAudioProcessorEditor::savePresetAs(const juce::String& name)
{
    if (!m_audioProcessor.getPresetManager().saveUserPreset(name))
    {
        auto alert = makeAlert("Couldn't Save Preset",
                               "The preset could not be written to:\n" + PresetManager::getUserPresetDirectory().getFullPathName(),
                               juce::MessageBoxIconType::WarningIcon);
        alert->addButton("OK", 0, juce::KeyPress(juce::KeyPress::returnKey));
        showAlert(std::move(alert), {});
    }

    updatePresetDisplay();
}

std::unique_ptr<juce::AlertWindow> RoarschachAudioProcessorEditor::makeAlert(const juce::String& title, const juce::String& message,
                                                                            juce::MessageBoxIconType icon)
{
    // Centred over the plugin window and drawn at the editor's own scale (not the design-size content's).
    auto alert = std::make_unique<juce::AlertWindow>(title, message, icon, this);

    // Before addButton() / addTextEditor(): they take button sizes and the field's font and outline from the
    // alert's LookAndFeel as they are called.
    alert->setLookAndFeel(&m_lookAndFeel);
    alert->setOpaque(false); // a rounded panel: the corners show what is behind the dialog
    RoarschachLookAndFeel::showFocusRingsAfterTab(*alert);
    return alert;
}

void RoarschachAudioProcessorEditor::showAlert(std::unique_ptr<juce::AlertWindow> alert,
                                               std::function<void(int, juce::AlertWindow&)> onResult)
{
    // Plugins can't run modal loops, so alerts are async. The editor owns the
    // window so it is always destroyed before the LookAndFeel it uses.
    m_alert = std::move(alert);
    m_alert->setLookAndFeel(&m_lookAndFeel); // already set by makeAlert(); kept for any alert made elsewhere

    m_alert->enterModalState(true, juce::ModalCallbackFunction::create(
        [safe = juce::Component::SafePointer<RoarschachAudioProcessorEditor>(this), onResult](int result)
        {
            if (safe == nullptr || safe->m_alert == nullptr)
                return;

            if (onResult)
                onResult(result, *safe->m_alert);

            // Defer the delete: we're inside the window's own callback.
            juce::MessageManager::callAsync([safe]
            {
                if (safe != nullptr && safe->m_alert != nullptr && !safe->m_alert->isCurrentlyModal())
                    safe->m_alert.reset();
            });
        }), false);
}
