#include "DoomOverlays.h"
#include "BinaryData.h"

#ifndef COPYRIGHT_YEAR
 #define COPYRIGHT_YEAR "2026"
#endif

namespace
{
    //==========================================================================
    // Shared metrics. The title row matches the main header's feel: big gold title, hazard strip under it.
    constexpr int c_hazardHeight = 8;

    // The dialogs
    constexpr int c_dialogPad = 22;
    constexpr int c_dialogTitleRow = 44;
    constexpr int c_dialogGap = 14;
    constexpr float c_messageScale = 1.5f;
    constexpr int c_messageLineHeight = 16;
    constexpr int c_buttonHeight = 34;
    constexpr int c_buttonGap = 10;
    constexpr int c_buttonMinWidth = 100;
    constexpr int c_buttonsGap = 18;
    constexpr int c_dialogBottomPad = 20;
    constexpr int c_dialogMinWidth = 460;
    constexpr int c_entryMinWidth = 520;
    constexpr int c_cursorBlinkMs = 400;

    // The About card (fits the editor's 1000 x 720 minimum with room to spare)
    constexpr int c_aboutWidth = 560;
    constexpr int c_aboutHeight = 640;
    constexpr int c_aboutPad = 24;
    constexpr int c_aboutTitleRow = 60;
    constexpr int c_skullFrameMs = 230; // DOOM's menu skull swaps every 8 tics of 35 Hz

    // The manual update check (the same request as the shared sammyj::UpdateCheck)
    const char* const c_releasesUrl = "https://api.github.com/repos/samueljustice/plugins/releases?per_page=100";
    const char* const c_tagPrefix = "doomeq-v";
    constexpr int c_timeoutMs = 5000;

    const juce::Colour c_insetColour { 0xff0d0b09 };

    juce::String currentVersion()
    {
        return JucePlugin_VersionString;
    }

    /** 0.0.x builds are test builds: published as GitHub pre-releases. */
    bool isTestBuild()
    {
        return currentVersion().startsWith ("0.0.");
    }

    /** The contact address for the About card's mail link, set by the build (SAMMYJ_CONTACT_EMAIL). */
    juce::String contactEmail()
    {
       #ifdef SAMMYJ_CONTACT_EMAIL
        return SAMMYJ_CONTACT_EMAIL;
       #else
        return {};
       #endif
    }

    //==========================================================================
    /** Word-wraps text for the pixel font at a scale. Newlines break; a word wider than a line is split. */
    juce::StringArray wrapText (const WadArt::PixelFont& font, const juce::String& text, float scale, float maxWidth)
    {
        juce::StringArray out;
        const auto fits = [&] (const juce::String& s) { return (float) font.widthOf (s) * scale <= maxWidth; };

        for (const auto& paragraph : juce::StringArray::fromLines (text))
        {
            juce::String line;
            for (auto word : juce::StringArray::fromTokens (paragraph, " ", ""))
            {
                if (word.isEmpty())
                    continue;

                const auto candidate = line.isEmpty() ? word : line + " " + word;
                if (fits (candidate))
                {
                    line = candidate;
                    continue;
                }

                if (line.isNotEmpty())
                    out.add (line);

                // A word wider than the whole line (a long file path): break it anywhere.
                while (word.length() > 1 && ! fits (word))
                {
                    int n = word.length() - 1;
                    while (n > 1 && ! fits (word.substring (0, n)))
                        --n;
                    out.add (word.substring (0, n));
                    word = word.substring (n);
                }
                line = word;
            }
            out.add (line);
        }
        return out;
    }

    /** The pixel font has plain ASCII only: swap the licence files' typographic characters for it. */
    juce::String toPixelAscii (const juce::String& text)
    {
        juce::String out;
        out.preallocateBytes ((size_t) text.length() + 16);

        for (auto c : text)
        {
            switch (c)
            {
                case 0xfeff: break;                                     // byte-order mark
                case 0x00a9: out << "(C)"; break;
                case 0x201c: case 0x201d: out << "\""; break;
                case 0x2018: case 0x2019: out << "'"; break;
                case 0x2013: case 0x2014: out << "-"; break;
                case '\t':   out << " "; break;
                case '\r': case '\f': break;
                default:     out += (c == '\n' || (c >= 32 && c < 127)) ? juce::String::charToString (c) : juce::String (" ");
            }
        }
        return out;
    }

    /**
     * Reflows a plain-text licence into paragraphs: its lines are hard-wrapped at about 72
     * columns, which the pixel font can't fit, so they're joined and wrapped again to the view.
     * A blank line, a "*" bullet or an "a)" item starts a new paragraph.
     */
    juce::StringArray paragraphsOf (const juce::String& raw)
    {
        juce::StringArray paragraphs;
        juce::String current;

        const auto flush = [&]
        {
            if (current.isNotEmpty())
                paragraphs.add (current);
            current = {};
        };

        for (auto line : juce::StringArray::fromLines (toPixelAscii (raw)))
        {
            line = line.trim();
            if (line.isEmpty())
            {
                flush();
                continue;
            }

            const bool startsItem = line.startsWithChar ('*') || line.startsWithChar ('-')
                                 || (line.length() > 2 && line[1] == ')' && line[2] == ' ');
            if (startsItem)
                flush();

            current = current.isEmpty() ? line : current + " " + line;
        }
        flush();
        return paragraphs;
    }

    /** Several colours on one line ("VERSION x" gold, the rest dim), at the largest scale that fits. */
    void drawSegments (juce::Graphics& g, const WadArt::PixelFont& font, juce::Rectangle<int> area,
                       std::initializer_list<std::pair<juce::String, juce::Colour>> segments, float maxScale)
    {
        juce::String all;
        int width = 0;
        for (const auto& s : segments)
        {
            all << s.first;
            width += font.widthOf (s.first);
        }

        if (! font.valid)
        {
            font.drawFitted (g, all, area.toFloat(), juce::Justification::centredLeft, segments.begin()->second, maxScale);
            return;
        }

        float scale = 1.0f;
        for (float s : { 2.0f, 1.5f, 1.0f })
            if (s <= maxScale && (float) width * s <= (float) area.getWidth())
            {
                scale = s;
                break;
            }

        float x = (float) area.getX();
        const float y = (float) area.getCentreY() - (float) font.height * scale * 0.5f;
        for (const auto& s : segments)
        {
            font.draw (g, s.first, x, y, scale, s.second);
            x += (float) font.widthOf (s.first) * scale;
        }
    }

    /** A section heading, like the menus' section headers: red bolt, dim caption, a rule to the edge. */
    void drawHeading (juce::Graphics& g, const WadArt::PixelFont& font, juce::Rectangle<int> r, const juce::String& text)
    {
        const int cy = r.getCentreY();
        g.setColour (Palette::doomRed);
        g.fillRect (r.getX(), cy - 2, 4, 4);

        font.drawFitted (g, text, r.withTrimmedLeft (10).toFloat(), juce::Justification::centredLeft, Palette::textDim, 1.0f);

        const int end = r.getX() + 10 + font.widthOf (text) + 8;
        if (end < r.getRight())
        {
            g.setColour (Palette::bevelDark);
            g.fillRect (end, cy, r.getRight() - end, 1);
            g.setColour (Palette::bevelLight.withAlpha (0.4f));
            g.fillRect (end, cy + 1, r.getRight() - end, 1);
        }
    }

    /** A WAD patch at a whole-number scale (crisp pixels), centred in an area, never larger than it. */
    void drawPatchCentred (juce::Graphics& g, const juce::Image& img, juce::Rectangle<int> area, int maxScale)
    {
        if (! img.isValid())
            return;

        int scale = maxScale;
        while (scale > 1 && (img.getWidth() * scale > area.getWidth() || img.getHeight() * scale > area.getHeight()))
            --scale;

        const auto dest = juce::Rectangle<int> (img.getWidth() * scale, img.getHeight() * scale).withCentre (area.getCentre());
        juce::Graphics::ScopedSaveState ss (g);
        g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
        g.drawImage (img, dest.toFloat());
    }
}

//==============================================================================
// DoomOverlay
//==============================================================================
DoomOverlay::DoomOverlay (const WadArt& art, const Textures& tex) : m_art (art), m_tex (tex)
{
    setWantsKeyboardFocus (true);
    setOpaque (false);

    // The DOOM screen under the overlay repaints at 35 fps, and every repaint there repaints the
    // overlay too. Caching the overlay's pixels keeps that to one image draw, not hundreds of glyphs.
    setBufferedToImage (true);
}

DoomOverlay::~DoomOverlay() = default;

void DoomOverlay::setUpButton (juce::TextButton& b, bool primary)
{
    b.setWantsKeyboardFocus (false);
    b.setMouseClickGrabsKeyboardFocus (false);
    b.setClickingTogglesState (false);

    // The LookAndFeel lights a button that's "on" (red fill, yellow text): that marks the Return action.
    b.setToggleState (primary, juce::dontSendNotification);
}

void DoomOverlay::paintBackdropAndCard (juce::Graphics& g, int titleRowHeight) const
{
    g.fillAll (Palette::background.withAlpha (0.8f));

    if (m_card.isEmpty())
        return;

    juce::DropShadow (juce::Colours::black.withAlpha (0.6f), 24, { 0, 8 }).drawForRectangle (g, m_card);

    DoomStyle::fillTextured (g, m_card.toFloat(), m_tex.panel, 2.0f, 0.4f, m_card.getPosition().toFloat());

    const auto strip = m_card.reduced (3, 0).withY (m_card.getY() + titleRowHeight).withHeight (c_hazardHeight);
    DoomStyle::fillTextured (g, strip.toFloat(), m_tex.hazard, 1.0f, 0.75f, strip.getPosition().toFloat());
    g.setColour (Palette::bevelDark);
    g.fillRect (strip.withHeight (1));
    g.fillRect (strip.withY (strip.getBottom()).withHeight (1));

    DoomStyle::bevel (g, m_card, 3, Palette::bevelLight.withAlpha (0.75f), Palette::bevelDark);
    DoomStyle::rivets (g, m_card, 6, 5);
}

void DoomOverlay::mouseDown (const juce::MouseEvent& e)
{
    if (! m_card.contains (e.getPosition()))
        cancel();
    else
        grabFocusIfShowing();
}

void DoomOverlay::parentHierarchyChanged()
{
    grabFocusIfShowing();

    // The editor may move focus itself while it puts the overlay up (leaving command mode, say),
    // so take it again once that's done.
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<DoomOverlay> (this)]
    {
        if (safe != nullptr && ! safe->hasKeyboardFocus (true))
            safe->grabFocusIfShowing();
    });
}

void DoomOverlay::visibilityChanged()
{
    grabFocusIfShowing();
}

void DoomOverlay::grabFocusIfShowing()
{
    if (isShowing())
        grabKeyboardFocus();
}

std::unique_ptr<juce::AccessibilityHandler> DoomOverlay::createAccessibilityHandler()
{
    return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::dialogWindow);
}

//==============================================================================
// DoomDialog
//==============================================================================
DoomDialog::DoomDialog (const WadArt& art, const Textures& tex, DoomDialogOptions options)
    : DoomOverlay (art, tex), m_options (std::move (options))
{
    setTitle (m_options.title);
    setDescription (m_options.message);

    auto names = m_options.buttons;
    if (names.isEmpty())
        names.add ("OK");

    for (int i = 0; i < names.size(); ++i)
    {
        auto* b = m_buttons.add (new juce::TextButton (names[i]));
        setUpButton (*b, i == 0);
        b->onClick = [this, i] { finish (i); };
        addAndMakeVisible (b);
    }
    m_cancelIndex = m_buttons.size() - 1;

    if (m_options.withTextEntry)
    {
        // The initial text goes through the same filter as typing.
        for (auto c : m_options.initialText)
        {
            const auto upper = juce::CharacterFunctions::toUpperCase (c);
            if (isAllowedCharacter (upper) && m_text.length() < juce::jmax (1, m_options.maxTextLength))
                m_text += juce::String::charToString (upper);
        }

        // DOOM's save-game slot, straight from the WAD (Freedoom has its own).
        m_slotLeft = art.patch ("LSLEFT");
        m_slotCentre = art.patch ("LSCNTR");
        m_slotRight = art.patch ("LSRGHT");

        startTimer (c_cursorBlinkMs);
    }
}

DoomDialog::~DoomDialog()
{
    stopTimer();
}

void DoomDialog::setCancelButton (int index)
{
    m_cancelIndex = juce::jlimit (0, m_buttons.size() - 1, index);
}

bool DoomDialog::isAllowedCharacter (juce::juce_wchar c)
{
    // Space and the font's glyphs (! .. _), less what a file name can't hold.
    if (c == ' ')
        return true;
    if (c < 33 || c > 95)
        return false;
    return ! juce::String ("/\\:*?\"<>|").containsChar (c);
}

void DoomDialog::resized()
{
    if (getWidth() <= 0 || getHeight() <= 0)
        return;

    const auto& font = m_art.font();

    // Each button as wide as its label at scale 2; shrunk together if the card can't hold them.
    juce::Array<int> widths;
    int total = 0;
    for (auto* b : m_buttons)
    {
        const int w = juce::jmax (c_buttonMinWidth, font.widthOf (b->getButtonText()) * 2 + 28);
        widths.add (w);
        total += w;
    }
    total += c_buttonGap * (m_buttons.size() - 1);

    int width = juce::jmax (c_dialogMinWidth, total + 2 * c_dialogPad, font.widthOf (m_options.title) * 2 + 2 * c_dialogPad + 8);
    if (m_options.withTextEntry)
        width = juce::jmax (width, c_entryMinWidth);
    width = juce::jmax (200, juce::jmin (width, getWidth() - 40));

    const int textWidth = width - 2 * c_dialogPad;
    m_messageLines = m_options.message.isEmpty() ? juce::StringArray()
                                                 : wrapText (font, m_options.message, c_messageScale, (float) textWidth);

    int slotHeight = 0;
    if (m_options.withTextEntry)
    {
        const int patchHeight = juce::jmax (m_slotLeft.getHeight(), m_slotCentre.getHeight(), m_slotRight.getHeight());
        slotHeight = patchHeight > 0 ? juce::jmax (24, patchHeight * 2) : 32;
    }

    const int messageHeight = m_messageLines.size() * c_messageLineHeight;
    int height = c_dialogTitleRow + c_hazardHeight + c_dialogGap + messageHeight;
    if (m_options.withTextEntry)
        height += (messageHeight > 0 ? c_dialogGap : 0) + slotHeight;
    height += c_buttonsGap + c_buttonHeight + c_dialogBottomPad;
    height = juce::jmax (120, juce::jmin (height, getHeight() - 40));

    m_card = juce::Rectangle<int> (width, height).withCentre (getLocalBounds().getCentre());

    auto area = m_card.reduced (c_dialogPad, 0);
    m_titleArea = area.removeFromTop (c_dialogTitleRow).withTrimmedTop (3);
    area.removeFromTop (c_hazardHeight + c_dialogGap);
    m_messageArea = area.removeFromTop (messageHeight);
    if (m_options.withTextEntry)
    {
        if (messageHeight > 0)
            area.removeFromTop (c_dialogGap);
        m_slotArea = area.removeFromTop (slotHeight);
    }

    auto row = m_card.withTrimmedBottom (c_dialogBottomPad).removeFromBottom (c_buttonHeight).reduced (c_dialogPad, 0);
    const float shrink = total > row.getWidth() ? (float) row.getWidth() / (float) total : 1.0f;
    float x = (float) row.getCentreX() - (float) total * shrink * 0.5f;
    for (int i = 0; i < m_buttons.size(); ++i)
    {
        const float w = (float) widths[i] * shrink;
        m_buttons[i]->setBounds (juce::roundToInt (x), row.getY(), juce::roundToInt (w), row.getHeight());
        x += w + (float) c_buttonGap * shrink;
    }
}

void DoomDialog::paint (juce::Graphics& g)
{
    paintBackdropAndCard (g, c_dialogTitleRow);

    const auto& font = m_art.font();
    font.drawFitted (g, m_options.title, m_titleArea.toFloat(), juce::Justification::centredLeft, Palette::hudYellow, 2.0f);

    for (int i = 0; i < m_messageLines.size(); ++i)
    {
        const auto line = m_messageArea.withY (m_messageArea.getY() + i * c_messageLineHeight).withHeight (c_messageLineHeight);
        font.drawFitted (g, m_messageLines[i], line.toFloat(), juce::Justification::centredLeft, Palette::textDim, c_messageScale);
    }

    if (m_options.withTextEntry)
        paintTextSlot (g);
}

void DoomDialog::paintTextSlot (juce::Graphics& g) const
{
    const auto r = m_slotArea;
    const auto& font = m_art.font();
    int textLeft = r.getX() + 10, textRight = r.getRight() - 10;

    if (m_slotLeft.isValid() && m_slotCentre.isValid() && m_slotRight.isValid() && m_slotCentre.getWidth() > 0)
    {
        // As DOOM draws it: the left cap, the middle tiled, the right cap, at twice the size.
        juce::Graphics::ScopedSaveState ss (g);
        g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);

        const auto drawPatch = [&] (const juce::Image& img, int x)
        {
            const int h = img.getHeight() * 2;
            g.drawImage (img, juce::Rectangle<float> ((float) x, (float) (r.getCentreY() - h / 2), (float) img.getWidth() * 2.0f, (float) h));
        };

        const int lw = m_slotLeft.getWidth() * 2, rw = m_slotRight.getWidth() * 2, cw = m_slotCentre.getWidth() * 2;
        drawPatch (m_slotLeft, r.getX());
        {
            juce::Graphics::ScopedSaveState clip (g);
            g.reduceClipRegion (r.getX() + lw, r.getY(), r.getWidth() - lw - rw, r.getHeight());
            for (int x = r.getX() + lw; x < r.getRight() - rw; x += cw)
                drawPatch (m_slotCentre, x);
        }
        drawPatch (m_slotRight, r.getRight() - rw);

        textLeft = r.getX() + lw + 4;
        textRight = r.getRight() - rw - 4;
    }
    else
    {
        g.setColour (c_insetColour);
        g.fillRect (r);
        DoomStyle::bevel (g, r, 2, Palette::bevelDark, Palette::bevelLight.withAlpha (0.55f));
    }

    if (! font.valid)
    {
        font.drawFitted (g, m_text + (m_cursorOn ? "_" : ""), juce::Rectangle<int> (textLeft, r.getY(), textRight - textLeft, r.getHeight()).toFloat(),
                         juce::Justification::centredLeft, Palette::hudYellow);
        return;
    }

    float scale = 1.0f;
    for (float s : { 2.0f, 1.5f })
        if ((float) font.widthOf (m_text + "_") * s <= (float) (textRight - textLeft) && (float) font.height * s <= (float) r.getHeight())
        {
            scale = s;
            break;
        }

    const float y = (float) r.getCentreY() - (float) font.height * scale * 0.5f;
    font.draw (g, m_text, (float) textLeft, y, scale, Palette::hudYellow);
    if (m_cursorOn)
        font.draw (g, "_", (float) textLeft + (float) font.widthOf (m_text) * scale, y, scale, Palette::hudYellow);
}

bool DoomDialog::keyPressed (const juce::KeyPress& key)
{
    if (m_done)
        return true;

    if (key.isKeyCode (juce::KeyPress::returnKey))
    {
        finish (0);
        return true;
    }

    if (key.isKeyCode (juce::KeyPress::escapeKey))
    {
        finish (m_cancelIndex);
        return true;
    }

    if (! m_options.withTextEntry)
        return false;

    if (key.isKeyCode (juce::KeyPress::backspaceKey) || key.isKeyCode (juce::KeyPress::deleteKey))
    {
        if (m_text.isNotEmpty())
            m_text = m_text.dropLastCharacters (1);
        m_cursorOn = true;
        repaint (m_slotArea);
        return true;
    }

    // Leave shortcuts (Cmd+S and so on) to the host.
    const auto mods = key.getModifiers();
    if (mods.isCommandDown() || mods.isCtrlDown())
        return false;

    const auto c = juce::CharacterFunctions::toUpperCase (key.getTextCharacter());
    if (! isAllowedCharacter (c))
        return false;

    // A typed character is used up here even when the slot is full, so it never reaches DOOM.
    if (m_text.length() < juce::jmax (1, m_options.maxTextLength))
    {
        m_text += juce::String::charToString (c);
        m_cursorOn = true;
        repaint (m_slotArea);
    }
    return true;
}

void DoomDialog::cancel()
{
    finish (m_cancelIndex);
}

void DoomDialog::timerCallback()
{
    m_cursorOn = ! m_cursorOn;
    repaint (m_slotArea);
}

void DoomDialog::finish (int buttonIndex)
{
    if (m_done)
        return;

    m_done = true;
    stopTimer();
    setVisible (false);

    // The callback usually deletes this dialog, so it's posted, never run from inside our own handler.
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<DoomDialog> (this),
                                      callback = m_options.onResult, buttonIndex, text = m_text]
    {
        if (safe != nullptr && callback != nullptr)
            callback (buttonIndex, text);
    });
}

//==============================================================================
// UpdatePopup
//==============================================================================
DoomDialogOptions UpdatePopup::makeOptions (const sammyj::UpdateInfo& info, std::function<void()> onClose)
{
    DoomDialogOptions o;
    o.title = "DOOM EQ " + info.latestVersion + " IS AVAILABLE";
    o.message = "YOU HAVE " + info.currentVersion + ".";
    o.buttons = { "DOWNLOAD", "LATER", "SKIP THIS VERSION" };
    o.onResult = [info, onClose = std::move (onClose)] (int button, const juce::String&)
    {
        if (button == 0)
            info.openDownloadPage();
        else if (button == 2)
            sammyj::UpdateCheck::skipVersion (info);

        if (onClose != nullptr)
            onClose();
    };
    return o;
}

UpdatePopup::UpdatePopup (const WadArt& art, const Textures& tex, const sammyj::UpdateInfo& info, std::function<void()> onClose)
    : DoomDialog (art, tex, makeOptions (info, std::move (onClose)))
{
    setCancelButton (1); // Esc and a click outside are LATER, not SKIP

    if (auto* b = getButton (0))
        b->setTooltip ("Opens the download page for the Sammy J's Plugins installer in your browser.");
    if (auto* b = getButton (1))
        b->setTooltip ("Close this. You'll be reminded next time.");
    if (auto* b = getButton (2))
        b->setTooltip ("Don't show this again until a newer version comes out.");
}

UpdatePopup::~UpdatePopup() = default;

//==============================================================================
// DoomOverlayParts
//==============================================================================
namespace DoomOverlayParts
{
    constexpr float c_linkScale = 1.5f;

    PixelLink::PixelLink (const WadArt& art, const juce::String& text, juce::URL target)
        : juce::Button (text), m_art (art), m_target (std::move (target))
    {
        setWantsKeyboardFocus (false);
        setMouseClickGrabsKeyboardFocus (false);
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        onClick = [this] { m_target.launchInDefaultBrowser(); };
    }

    int PixelLink::getIdealWidth() const
    {
        return juce::roundToInt ((float) m_art.font().widthOf (getButtonText()) * c_linkScale) + 8;
    }

    void PixelLink::paintButton (juce::Graphics& g, bool highlighted, bool down)
    {
        const auto& font = m_art.font();
        const auto colour = (highlighted || down) ? Palette::gold : Palette::textDim;
        const auto r = getLocalBounds().toFloat();

        font.drawFitted (g, getButtonText(), r, juce::Justification::centredLeft, colour, c_linkScale);

        if (highlighted || down)
        {
            const float scale = (float) font.widthOf (getButtonText()) * c_linkScale <= r.getWidth() ? c_linkScale : 1.0f;
            const int y = juce::roundToInt (r.getCentreY() + (float) font.height * scale * 0.5f) + 2;
            g.setColour (colour.withAlpha (0.7f));
            g.fillRect (0, juce::jmin (y, getHeight() - 1), juce::roundToInt ((float) font.widthOf (getButtonText()) * scale), 1);
        }
    }

    //==========================================================================
    PixelTextView::PixelTextView (const WadArt& art) : m_art (art)
    {
        setMouseClickGrabsKeyboardFocus (false);
        setWantsKeyboardFocus (false);
    }

    void PixelTextView::addParagraph (const juce::String& text, juce::Colour colour, float scale, int gapAfter)
    {
        m_paragraphs.push_back ({ text, colour, scale, gapAfter });
    }

    void PixelTextView::layout (int width)
    {
        width = juce::jmax (40, width);
        m_lines.clear();

        const auto& font = m_art.font();
        int y = 0;
        for (const auto& p : m_paragraphs)
        {
            const int lineHeight = juce::roundToInt ((float) font.height * p.scale + 4.0f * p.scale);
            for (const auto& line : wrapText (font, p.text, p.scale, (float) width))
            {
                m_lines.push_back ({ line, p.colour, p.scale, y });
                y += lineHeight;
            }
            y += p.gapAfter;
        }

        setSize (width, juce::jmax (1, y));
    }

    void PixelTextView::paint (juce::Graphics& g)
    {
        // Only the lines in view: the licences run to a few hundred.
        const auto clip = g.getClipBounds();
        const auto& font = m_art.font();

        for (const auto& line : m_lines)
        {
            const int lineHeight = juce::roundToInt ((float) font.height * line.scale + 4.0f * line.scale);
            if (line.y + lineHeight < clip.getY())
                continue;
            if (line.y > clip.getBottom())
                break;

            font.drawFitted (g, line.text, juce::Rectangle<float> (0.0f, (float) line.y, (float) getWidth(), (float) lineHeight),
                             juce::Justification::centredLeft, line.colour, line.scale);
        }
    }
}

//==============================================================================
// AboutPanel: the manual CHECK FOR UPDATES
//==============================================================================
struct AboutPanel::CheckResult
{
    enum class Tone { neutral, newer, latest, warning };

    juce::String text;
    Tone tone = Tone::neutral;
    bool newer = false;
    bool prerelease = false;
    juce::String version, url;
};

/**
 * One check, on demand. Unlike the daily sammyj::UpdateCheck it always asks GitHub, says what it
 * found (including "you're up to date" and failures), and lets a test build see newer test builds
 * (pre-releases), since every 0.0.x DOOM EQ is published as one.
 */
class AboutPanel::ManualCheck : public juce::Thread
{
public:
    ManualCheck (juce::String version, std::function<void (const CheckResult&)> onResult)
        : juce::Thread ("DOOM EQ update check"), m_currentVersion (std::move (version)), m_onResult (std::move (onResult))
    {
    }

    ~ManualCheck() override
    {
        m_alive->store (false);

        // As sammyj::UpdateCheck does: a cancel can land before the request exists and be lost,
        // so keep cancelling until the thread has gone. The force-kill in stopThread() is the last resort.
        for (int i = 0; i < 200 && isThreadRunning(); ++i)
        {
            cancelRequest();
            waitForThreadToExit (10);
        }
        stopThread (1000);
    }

    void start()
    {
        startThread (juce::Thread::Priority::low);
    }

private:
    void cancelRequest()
    {
        signalThreadShouldExit();

        // WebInputStream::cancel() unblocks connect() and read() from another thread.
        const juce::ScopedLock sl (m_streamLock);
        if (m_stream != nullptr)
            m_stream->cancel();
    }

    void run() override
    {
        juce::String body;
        bool ok = false;

        {
            juce::WebInputStream stream (juce::URL (c_releasesUrl), false);
            stream.withConnectionTimeout (c_timeoutMs)
                  .withExtraHeaders ("User-Agent: SammyJ-Plugins-UpdateCheck\r\nAccept: application/vnd.github+json");

            {
                // Checked under the lock, so cancelRequest() either sees the stream or stops it starting.
                const juce::ScopedLock sl (m_streamLock);
                if (threadShouldExit())
                    return;
                m_stream = &stream;
            }

            // No listener: one that aborts connect() makes JUCE (macOS) destroy the connection on
            // this thread while cancelRequest() may be using it.
            if (stream.connect (nullptr) && ! stream.isError() && stream.getStatusCode() == 200 && ! threadShouldExit())
            {
                body = stream.readEntireStreamAsString();
                ok = true;
            }

            const juce::ScopedLock sl (m_streamLock);
            m_stream = nullptr;
        }

        if (threadShouldExit())
            return;

        const auto result = ok ? interpret (body, m_currentVersion)
                               : CheckResult { "FAILED TO CHECK FOR UPDATES", CheckResult::Tone::warning };

        // The panel is only touched on the message thread, and only while it (and this check) exists.
        juce::MessageManager::callAsync ([alive = m_alive, callback = m_onResult, result]
        {
            if (alive->load() && callback != nullptr)
                callback (result);
        });
    }

    static CheckResult interpret (const juce::String& body, const juce::String& current)
    {
        // Keep the parsed tree in a named local: getArray() points into it.
        const auto releases = juce::JSON::parse (body);
        const auto* list = releases.getArray();
        if (list == nullptr)
            return { "INVALID RESPONSE FROM SERVER", CheckResult::Tone::warning };

        const bool allowPrereleases = current.startsWith ("0.0.");
        const juce::String prefix (c_tagPrefix);

        CheckResult best;
        for (const auto& release : *list)
        {
            const auto tag = release.getProperty ("tag_name", {}).toString();
            const bool prerelease = static_cast<bool> (release.getProperty ("prerelease", false));
            if (! tag.startsWith (prefix)
                || static_cast<bool> (release.getProperty ("draft", false))
                || (prerelease && ! allowPrereleases))
                continue;

            const auto candidate = tag.substring (prefix.length());
            if (candidate.isEmpty() || ! candidate.containsOnly ("0123456789."))
                continue;

            if (best.version.isEmpty() || sammyj::UpdateCheck::isNewer (candidate, best.version))
            {
                best.version = candidate;
                best.url = release.getProperty ("html_url", {}).toString();
                best.prerelease = prerelease;
            }
        }

        if (best.version.isEmpty())
            return { "NO RELEASES FOUND", CheckResult::Tone::neutral };

        if (sammyj::UpdateCheck::isNewer (best.version, current))
        {
            best.text = best.version + " AVAILABLE!"; // short enough to sit beside DOWNLOAD
            best.tone = CheckResult::Tone::newer;
            best.newer = true;
            return best;
        }

        return { "YOU HAVE THE LATEST VERSION", CheckResult::Tone::latest };
    }

    const juce::String m_currentVersion;
    const std::function<void (const CheckResult&)> m_onResult;

    // Read on the message thread by the posted result, cleared by the destructor (message thread).
    std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>> (true);

    juce::CriticalSection m_streamLock;
    juce::WebInputStream* m_stream = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ManualCheck)
};

//==============================================================================
// AboutPanel
//==============================================================================
AboutPanel::AboutPanel (const WadArt& art, const Textures& tex, std::function<void()> onClose)
    : DoomOverlay (art, tex),
      m_onClose (std::move (onClose)),
      m_webLink (art, "SWEETJUSTICESOUND.COM", juce::URL ("https://sweetjusticesound.com")),
      m_mailLink (art, contactEmail().toUpperCase(), juce::URL ("mailto:" + contactEmail())),
      m_credits (art)
{
    setTitle ("About SammyJs DOOM EQ");

    m_skull[0] = art.patch ("M_SKULL1");
    m_skull[1] = art.patch ("M_SKULL2");

    m_webLink.setTooltip ("Open sweetjusticesound.com");
    addAndMakeVisible (m_webLink);

    // The mail link only when the build names an address (SAMMYJ_CONTACT_EMAIL).
    if (contactEmail().isNotEmpty())
    {
        m_mailLink.setTooltip ("Email " + contactEmail());
        addAndMakeVisible (m_mailLink);
    }

    setUpButton (m_checkButton, false);
    setUpButton (m_downloadButton, true);
    setUpButton (m_closeButton, false);
    m_checkButton.setTooltip ("Check GitHub for a newer version of DOOM EQ now.");
    m_checkButton.onClick = [this] { startCheck(); };
    m_downloadButton.onClick = [this] { download(); };
    m_closeButton.onClick = [this] { close(); };
    addAndMakeVisible (m_checkButton);
    addChildComponent (m_downloadButton); // shown when a check finds a newer version
    addAndMakeVisible (m_closeButton);

    // Credits, then both licences in full: DOOM EQ's own (GPL v2, because it links doomgeneric)
    // and Freedoom's, whose game data, textures, font and music it ships.
    m_credits.addParagraph ("COPYRIGHT (C) " COPYRIGHT_YEAR " SAMUEL JUSTICE", Palette::text, 1.5f, 6);
    m_credits.addParagraph ("DOOM EQ IS LICENSED UNDER THE GNU GPL V2, BECAUSE IT LINKS DOOMGENERIC (GPL-2.0).", Palette::text, 1.5f, 6);
    m_credits.addParagraph ("DOOM ENGINE: DOOMGENERIC BY OZKL, BASED ON ID SOFTWARE'S DOOM SOURCE.", Palette::text, 1.5f, 6);
    m_credits.addParagraph ("GAME DATA, TEXTURES, FONT AND MUSIC: FREEDOOM (BSD LICENCE).", Palette::text, 1.5f, 6);
    m_credits.addParagraph ("BUILT WITH JUCE. VST IS A REGISTERED TRADEMARK OF STEINBERG MEDIA TECHNOLOGIES GMBH.", Palette::text, 1.5f, 16);

    const auto addLicence = [this] (const juce::String& heading, const char* data, int size)
    {
        m_credits.addParagraph (heading, Palette::hudYellow, 1.5f, 8);
        for (const auto& p : paragraphsOf (juce::String::fromUTF8 (data, size)))
            m_credits.addParagraph (p.toUpperCase(), Palette::textDim, 1.0f, 6);
        m_credits.addParagraph ({}, Palette::textDim, 1.0f, 10);
    };
    addLicence ("LICENSE: DOOM EQ (GNU GPL V2)", BinaryData::LICENSE, BinaryData::LICENSESize);
    addLicence ("LICENSE: FREEDOOM", BinaryData::FREEDOOMCOPYING_txt, BinaryData::FREEDOOMCOPYING_txtSize);

    m_viewport.setViewedComponent (&m_credits, false);
    m_viewport.setScrollBarsShown (true, false);
    m_viewport.setScrollBarThickness (10);
    m_viewport.setSingleStepSizes (0, 22);
    m_viewport.setWantsKeyboardFocus (false); // the panel keeps the keys (it forwards the scrolling ones)
    m_viewport.getVerticalScrollBar().setColour (juce::ScrollBar::thumbColourId, Palette::gold.withAlpha (0.6f));
    m_viewport.getVerticalScrollBar().setColour (juce::ScrollBar::trackColourId, juce::Colours::transparentBlack);
    addAndMakeVisible (m_viewport);

    startTimer (c_skullFrameMs);
}

AboutPanel::~AboutPanel()
{
    stopTimer();
    m_check.reset(); // cancels a check still running; its result is never delivered
}

void AboutPanel::resized()
{
    if (getWidth() <= 0 || getHeight() <= 0)
        return;

    const auto bounds = getLocalBounds();
    const int width = juce::jmax (320, juce::jmin (c_aboutWidth, bounds.getWidth() - 40));
    const int height = juce::jmax (420, juce::jmin (c_aboutHeight, bounds.getHeight() - 40));
    m_card = juce::Rectangle<int> (width, height).withCentre (bounds.getCentre());

    auto area = m_card.reduced (c_aboutPad, 0);

    m_titleRow = area.removeFromTop (c_aboutTitleRow).withTrimmedTop (3);
    m_skullArea = m_titleRow.withWidth (48);
    area.removeFromTop (c_hazardHeight + 12);

    m_taglineRow = area.removeFromTop (14);
    area.removeFromTop (4);
    m_versionRow = area.removeFromTop (14);
    area.removeFromTop (14);

    m_createdHeading = area.removeFromTop (12);
    area.removeFromTop (6);
    m_nameRow = area.removeFromTop (16);
    area.removeFromTop (6);
    {
        auto links = area.removeFromTop (18);
        m_webLink.setBounds (links.removeFromLeft (juce::jmin (links.getWidth(), m_webLink.getIdealWidth())));
        links.removeFromLeft (24);
        m_mailLink.setBounds (links.removeFromLeft (juce::jmin (juce::jmax (0, links.getWidth()), m_mailLink.getIdealWidth())));
    }
    area.removeFromTop (14);

    m_updatesHeading = area.removeFromTop (12);
    area.removeFromTop (6);
    {
        auto row = area.removeFromTop (30);
        m_checkButton.setBounds (row.removeFromRight (210));
        if (m_downloadButton.isVisible())
        {
            row.removeFromRight (8);
            m_downloadButton.setBounds (row.removeFromRight (116));
        }
        row.removeFromRight (10);
        m_statusArea = row;
    }
    area.removeFromTop (14);

    m_creditsHeading = area.removeFromTop (12);
    area.removeFromTop (6);

    area.removeFromBottom (18);
    m_closeButton.setBounds (area.removeFromBottom (32).withSizeKeepingCentre (130, 32));
    area.removeFromBottom (12);

    m_creditsBox = area;
    m_viewport.setBounds (m_creditsBox.reduced (6, 4));
    m_credits.layout (m_viewport.getWidth() - m_viewport.getScrollBarThickness() - 6);
}

void AboutPanel::paint (juce::Graphics& g)
{
    paintBackdropAndCard (g, c_aboutTitleRow);

    const auto& font = m_art.font();

    // Title row: the menu skull, then the wordmark as in the main header.
    drawPatchCentred (g, m_skull[m_skullFrame], m_skullArea, 2);
    {
        const auto words = m_titleRow.withTrimmedLeft (m_skullArea.getWidth() + 12);
        if (font.valid)
        {
            const int wd = font.widthOf ("DOOM"), gap = 4, we = font.widthOf ("EQ");
            float sc = 3.0f;
            while (sc > 1.0f && (float) (wd + gap + we) * sc > (float) words.getWidth())
                sc -= 1.0f;
            const float x = (float) words.getX(), y = (float) words.getCentreY() - (float) font.height * sc * 0.5f;
            font.draw (g, "DOOM", x, y, sc, Palette::doomRed);
            font.draw (g, "EQ", x + (float) (wd + gap) * sc, y, sc, Palette::hudYellow);
        }
        else
        {
            font.drawFitted (g, "DOOM EQ", words.toFloat(), juce::Justification::centredLeft, Palette::hudYellow, 3.0f);
        }
    }

    font.drawFitted (g, "SAMMYJS  /  6 BAND PARAMETRIC EQ  /  RUNS DOOM", m_taglineRow.toFloat(),
                     juce::Justification::centredLeft, Palette::textDim, 1.5f);

    {
       #ifdef BUILD_TIMESTAMP
        juce::String built (BUILD_TIMESTAMP);
       #else
        juce::String built (__DATE__);
       #endif
        built = built.toUpperCase().replace ("  ", " ").trim(); // __DATE__ pads single-digit days

        juce::String rest = "  /  BUILT " + built;
        if (isTestBuild())
            rest << "  /  TEST BUILD";

        drawSegments (g, font, m_versionRow, { { "VERSION " + currentVersion(), Palette::hudYellow }, { rest, Palette::textDim } }, 1.5f);
    }

    drawHeading (g, font, m_createdHeading, "CREATED BY");
    font.drawFitted (g, "SAMUEL JUSTICE", m_nameRow.toFloat(), juce::Justification::centredLeft, Palette::text, 2.0f);

    drawHeading (g, font, m_updatesHeading, "UPDATES");
    if (m_status.isNotEmpty())
    {
        juce::Graphics::ScopedSaveState ss (g);
        g.reduceClipRegion (m_statusArea); // never under the buttons, whatever the text
        font.drawFitted (g, m_status, m_statusArea.toFloat(), juce::Justification::centredLeft, m_statusColour, 1.5f);
    }

    drawHeading (g, font, m_creditsHeading, "CREDITS / LICENSE");
    g.setColour (c_insetColour);
    g.fillRect (m_creditsBox);
    DoomStyle::bevel (g, m_creditsBox, 2, Palette::bevelDark, Palette::bevelLight.withAlpha (0.55f));
}

bool AboutPanel::keyPressed (const juce::KeyPress& key)
{
    // The lit button is the Return action: DOWNLOAD when a newer version was found.
    if (key.isKeyCode (juce::KeyPress::returnKey) && m_downloadButton.isVisible())
    {
        m_downloadButton.triggerClick();
        return true;
    }
    if (key.isKeyCode (juce::KeyPress::escapeKey) || key.isKeyCode (juce::KeyPress::returnKey))
    {
        close();
        return true;
    }

    // Up/down, page up/down, home/end scroll the credits.
    return m_viewport.keyPressed (key);
}

void AboutPanel::cancel()
{
    close();
}

void AboutPanel::timerCallback()
{
    m_skullFrame = 1 - m_skullFrame;
    repaint (m_skullArea);
}

void AboutPanel::close()
{
    if (m_closing)
        return;

    m_closing = true;
    stopTimer();
    setVisible (false);

    // The owner deletes the panel; not from inside the button's (or key's) own callback.
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<AboutPanel> (this)]
    {
        if (safe != nullptr && safe->m_onClose != nullptr)
            safe->m_onClose();
    });
}

void AboutPanel::startCheck()
{
    if (m_check != nullptr && m_check->isThreadRunning())
        return;

    m_check.reset(); // a finished check; its result has been shown

    m_status = "CHECKING FOR UPDATES...";
    m_statusColour = Palette::textDim;
    m_downloadButton.setVisible (false);
    m_checkButton.setEnabled (false);
    m_checkButton.setAlpha (0.5f);
    resized();
    repaint();

    m_check = std::make_unique<ManualCheck> (currentVersion(), [this] (const CheckResult& r) { showCheckResult (r); });
    m_check->start();
}

void AboutPanel::showCheckResult (const CheckResult& r)
{
    m_status = r.text;
    switch (r.tone)
    {
        case CheckResult::Tone::newer:   m_statusColour = Palette::hudYellow; break;
        case CheckResult::Tone::latest:  m_statusColour = Palette::bands[3]; break;
        case CheckResult::Tone::warning: m_statusColour = Palette::doomRed; break;
        case CheckResult::Tone::neutral:
        default:                         m_statusColour = Palette::textDim; break;
    }

    m_newVersion = r.version;
    m_newUrl = r.url;
    m_newIsPrerelease = r.prerelease;

    // A test build's DOWNLOAD goes to that pre-release's own page (the installer leaves pre-releases out).
    const bool canDownload = r.newer && (! r.prerelease || r.url.startsWith ("https://github.com/"));
    m_downloadButton.setTooltip (r.prerelease ? "Opens this test build's release page on GitHub in your browser."
                                              : "Opens the download page for the Sammy J's Plugins installer in your browser.");
    m_downloadButton.setVisible (canDownload);

    m_checkButton.setEnabled (true);
    m_checkButton.setAlpha (1.0f);
    resized();
    repaint();
}

void AboutPanel::download()
{
    if (m_newIsPrerelease && m_newUrl.startsWith ("https://github.com/"))
        juce::URL (m_newUrl).launchInDefaultBrowser();
    else
        sammyj::UpdateInfo { "doomeq", currentVersion(), m_newVersion, m_newUrl }.openDownloadPage();
}
