#include "BandView.h"
#include "PondScene.h"
#include "ToyPalette.h"

namespace dehonk::scene
{

namespace
{
    // One scale for the bars, the level bar at the left and the Threshold: the Threshold's own
    // range, so its line across the bands is at the height of that level on every one of them.
    constexpr float floorDb = -60.0f;
    constexpr float alwaysAtOrBelowDb = -59.9f;   // the Threshold at the bottom: always acting
    constexpr float cutScaleDb = 12.0f;           // depth of the cut lane
    constexpr float maxCutMinDb = 3.0f, maxCutMaxDb = 12.0f;
    constexpr float snapLowHz = 1800.0f, snapHighHz = 4000.0f;
    constexpr float gutter = 42.0f;               // left of the plot: the level bar and the Threshold's grip

    int rangeOfHz (float hz, const float* edges) noexcept
    {
        for (int m = 0; m < kNumModules; ++m)
            if (hz >= edges[m] && hz < edges[m + 1])
                return m;
        return hz >= snapLowHz && hz <= snapHighHz ? kNumModules : -1;
    }
}

BandView::BandView()
{
    for (auto& l : level)
        l = floorDb;
    // Opaque: what lies behind it (the window's own background) is drawn into the backdrop once,
    // so that a repaint of the view at every picture never repaints the window under it.
    setOpaque (true);
    setTooltip ("What De-Honk is doing in each band. Blue bars: the sound coming in. Red: what is being taken out. "
                "Three things here can be dragged: the handles between the ranges, the Threshold (the grip on the level bar "
                "at the left, and its line across the bands) and Max Cut (the dashed line across the top). "
                "Hold Shift to move one finely. A double-click puts any of them back.");
}

void BandView::setFocus (int range)
{
    range = juce::jlimit (-1, (int) kNumModules, range);
    if (range != focus)
    {
        focus = range;
        repaint();
    }
}

juce::String BandView::getTooltip()
{
    const int at = dragging != noHandle ? dragging : hovered;
    if (at == thresholdHandle)
        return "Threshold: De-Honk acts only while the mix is louder than this. Drag it up or down. "
               "The bar at the left is the level of the whole mix, which is what the Threshold is compared with: "
               "it turns red once the mix is over the line. At the bottom De-Honk always acts. Shift moves it finely; a double-click puts it back.";
    if (at == maxCutHandle)
        return "Max Cut: the deepest any one cut may go. Drag the dashed line up or down, from 3 to 12 dB. A double-click puts it back.";
    if (at >= 0 && at < kNumEdges)
        return "The edge between two ranges. Drag it left or right. A double-click puts it back.";
    return juce::SettableTooltipClient::getTooltip();
}

void BandView::update (const BandViewState& s, double seconds)
{
    state = s;
    const float dt = (float) juce::jlimit (0.0, 0.25, seconds);
    const int n = juce::jlimit (0, kMaxBands, s.frame.numBands);
    for (int k = 0; k < n; ++k)
    {
        const float in = s.hasFrame ? juce::jmax (floorDb, s.frame.levelDb[k]) : floorDb;
        level[k] = in > level[k] ? in : juce::jmax (in, level[k] - 36.0f * dt);
        const float c = s.hasFrame ? s.frame.cutDb[k] : 0.0f;
        cut[k] = c > cut[k] ? c : juce::jmax (c, cut[k] - 30.0f * dt);
    }
    const float click = s.hasFrame && s.snapOn ? s.frame.snapCutDb : 0.0f;
    clickHeld = click > clickHeld ? click : juce::jmax (0.0f, clickHeld - 40.0f * dt);
    const float prog = s.hasFrame ? s.frame.programDb : -120.0f;
    programme = prog > programme ? prog : juce::jmax (prog, programme - 40.0f * dt);
    repaint();
}

//==============================================================================
const BandView::Shaped& BandView::shaped (const juce::String& text, Face face, float size) const
{
    for (const auto& s : shapedTexts)
        if (s.face == face && juce::approximatelyEqual (s.size, size) && s.text == text)
            return s;

    // The readings ("cut 3.4 dB") change all the time: keep the store small by dropping the oldest.
    if (shapedTexts.size() >= 48)
        shapedTexts.erase (shapedTexts.begin(), shapedTexts.begin() + 16);

    Shaped s;
    s.text = text;
    s.face = face;
    s.size = size;
    const auto font = face == nameFace ? fonts->name (size) : face == valueFace ? fonts->value (size) : fonts->body (size);
    s.glyphs.addLineOfText (font, text, 0.0f, 0.0f);
    s.width = juce::GlyphArrangement::getStringWidth (font, text);
    s.centreToBaseline = (font.getAscent() - font.getDescent()) * 0.5f;
    s.height = font.getHeight();
    shapedTexts.push_back (std::move (s));
    return shapedTexts.back();
}

void BandView::drawShaped (juce::Graphics& g, const Shaped& s, juce::Rectangle<float> area, bool centred)
{
    const float x = centred ? area.getCentreX() - s.width * 0.5f : area.getX();
    s.glyphs.draw (g, juce::AffineTransform::translation (x, area.getCentreY() + s.centreToBaseline));
}

void BandView::chip (juce::Graphics& g, const juce::String& text, juce::Point<float> anchor, juce::Colour fill, bool fromLeft) const
{
    // A chip of text: a rounded pill with an ink outline. `anchor` is its top right corner, or its
    // top left one with `fromLeft`.
    const auto& s = shaped (text, valueFace, 10.5f);
    const float w = s.width + 16.0f, h = s.height + 8.0f;
    const juce::Rectangle<float> r (fromLeft ? anchor.x : anchor.x - w, anchor.y, w, h);
    g.setColour (fill);
    g.fillRoundedRectangle (r, h * 0.5f);
    g.setColour (toy::ink);
    g.drawRoundedRectangle (r, h * 0.5f, toy::outline);
    drawShaped (g, s, r, true);
}

//==============================================================================
juce::Rectangle<float> BandView::plot() const noexcept
{
    // Inside the card: a column on the left for the level bar and the Threshold's grip, a row
    // along the bottom for the frequency marks.
    return getLocalBounds().toFloat().reduced (toy::outline + 6.0f).withTrimmedBottom (toy::dropY + 16.0f).withTrimmedLeft (gutter);
}

juce::Rectangle<float> BandView::lane() const noexcept
{
    const auto p = plot();
    return p.withHeight (juce::jmax (22.0f, p.getHeight() * 0.36f));
}

juce::Rectangle<float> BandView::levels() const noexcept
{
    return plot().withTrimmedTop (lane().getHeight() + 4.0f);
}

juce::Rectangle<float> BandView::meter() const noexcept
{
    const auto l = levels();
    return { plot().getX() - gutter + 4.0f, l.getY(), 13.0f, l.getHeight() };
}

float BandView::xForHz (float hz) const noexcept
{
    const auto p = plot();
    return p.getX() + PondScene::xForHz (hz) * p.getWidth();
}

float BandView::hzForX (float x) const noexcept
{
    const auto p = plot();
    return PondScene::hzForX ((x - p.getX()) / juce::jmax (1.0f, p.getWidth()));
}

float BandView::yForLevel (float db) const noexcept
{
    const auto l = levels();
    return l.getBottom() - l.getHeight() * juce::jlimit (0.0f, 1.0f, (db - floorDb) / -floorDb);
}

float BandView::yForMaxCut() const noexcept
{
    const auto l = lane();
    return l.getY() + l.getHeight() * juce::jlimit (0.0f, 1.0f, state.maxCutDb / cutScaleDb);
}

int BandView::handleAt (juce::Point<float> at) const noexcept
{
    const auto p = plot();
    const auto l = levels();
    const float yThreshold = yForLevel (state.thresholdDb);

    // 1. An edge's round handle at the top of its line.
    for (int e = 0; e < kNumEdges; ++e)
        if (at.getDistanceFrom ({ xForHz (state.edgeHz[e]), p.getY() + 2.0f }) < 9.0f)
            return e;

    // 2. The Threshold's grip beside the level bar (anywhere in the gutter near its height).
    if (at.x < p.getX() && at.x >= meter().getX() - 3.0f && std::abs (at.y - yThreshold) < 9.0f)
        return thresholdHandle;

    if (! p.expanded (0.0f, 6.0f).contains (at))
        return noHandle;

    // 3. The two lines that run across: Max Cut in the cut lane, the Threshold over the bands.
    if (std::abs (at.y - yForMaxCut()) < 5.0f)
        return maxCutHandle;
    if (state.thresholdDb > alwaysAtOrBelowDb && l.expanded (0.0f, 4.0f).contains (at) && std::abs (at.y - yThreshold) < 5.0f)
        return thresholdHandle;

    // 4. An edge anywhere along its line.
    int best = noHandle;
    float bestD = 7.0f;
    for (int e = 0; e < kNumEdges; ++e)
    {
        const float d = std::abs (at.x - xForHz (state.edgeHz[e]));
        if (d < bestD) { bestD = d; best = e; }
    }
    return best;
}

void BandView::setHovered (int handle)
{
    if (handle == hovered)
        return;
    hovered = handle;
    setMouseCursor (handle == noHandle ? juce::MouseCursor::NormalCursor
                    : handle < kNumEdges ? juce::MouseCursor::LeftRightResizeCursor
                                         : juce::MouseCursor::UpDownResizeCursor);
    repaint();
}

//==============================================================================
void BandView::renderBackdrop (float scale)
{
    // Everything that stays put between pictures: the window's background behind the view (its
    // parent's own paint, without the parent's other children), the card, each range's stretch
    // and its name, the rules, the frequency marks and the level bar's well. Drawn again only when
    // an edge, a switch, the focus, the size or the place in the window changes.
    const int w = juce::jmax (1, juce::roundToInt ((float) getWidth() * scale)), h = juce::jmax (1, juce::roundToInt ((float) getHeight() * scale));
    if (backdrop.getWidth() != w || backdrop.getHeight() != h)
        backdrop = juce::Image (juce::Image::ARGB, w, h, true, juce::SoftwareImageType());   // copied into each picture as it is
    else
        backdrop.clear (backdrop.getBounds());

    juce::Graphics g (backdrop);
    g.addTransform (juce::AffineTransform::scale (scale));

    if (auto* parent = getParentComponent())
    {
        juce::Graphics::ScopedSaveState behind (g);
        g.setOrigin (-getPosition());
        parent->paint (g);
    }
    else
        g.fillAll (toy::cream);

    const auto card = getLocalBounds().toFloat().reduced (toy::outline * 0.5f).withTrimmedBottom (toy::dropY);
    const auto p = plot();
    const auto cutLane = lane();
    const bool asleep = state.bypass;

    g.setColour (toy::paper);
    g.fillRoundedRectangle (card, toy::cardCorner);

    {
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (p.toNearestInt());

        // Each range's stretch, in its colour, and Snap's. The ranges out of focus are paler.
        for (int m = 0; m < kNumModules; ++m)
        {
            const float x0 = xForHz (state.edgeHz[m]), x1 = xForHz (state.edgeHz[m + 1]);
            if (x1 <= x0)
                continue;
            const bool on = state.moduleOn[m] && ! asleep;
            const float alpha = (on ? 0.38f : 0.16f) * (inFocus (m) ? 1.0f : 0.35f);
            g.setColour ((on ? toy::module[m] : toy::grey).withAlpha (alpha));
            g.fillRect (juce::Rectangle<float> (x0, p.getY(), x1 - x0, p.getHeight()));
        }
        {
            const float x0 = xForHz (snapLowHz), x1 = xForHz (snapHighHz);
            const bool on = state.snapOn && ! asleep;
            g.setColour ((on ? toy::click : toy::grey).withAlpha ((on ? 0.38f : 0.10f) * (inFocus (kNumModules) ? 1.0f : 0.35f)));
            g.fillRect (juce::Rectangle<float> (x0, p.getY(), x1 - x0, p.getHeight()));
        }

        // Faint rules in the cut lane at 3, 6 and 9 dB.
        g.setColour (toy::ink.withAlpha (0.10f));
        for (float db : { 3.0f, 6.0f, 9.0f })
            g.fillRect (juce::Rectangle<float> (p.getX(), cutLane.getY() + cutLane.getHeight() * db / cutScaleDb, p.getWidth(), 1.0f));
    }

    // Names along the top of each stretch. Where they go is worked out here; they are drawn
    // over the bars at every picture (see paint), so that a cut hanging in the lane never covers one.
    nameAreas.clear();
    names.clear();
    auto drawName = [&] (const char* text, juce::Rectangle<float> area, bool on, bool focused)
    {
        const auto& name = shaped (text, nameFace, 9.5f);
        if (name.width > area.getWidth())
            return;
        names.push_back ({ text, area, (on && ! asleep ? 0.85f : 0.35f) * (focused ? 1.0f : 0.5f) });
        nameAreas.add (area.withWidth (name.width).expanded (4.0f, 2.0f).getSmallestIntegerContainer());
    };
    static const char* const rangeNames[kNumModules] = { "woof", "tub", "fog", "bark" };
    for (int m = 0; m < kNumModules; ++m)
    {
        const float x0 = xForHz (state.edgeHz[m]), x1 = xForHz (state.edgeHz[m + 1]);
        drawName (rangeNames[m], juce::Rectangle<float> (x0 + 12.0f, p.getY() + 3.0f, x1 - x0 - 14.0f, 12.0f), state.moduleOn[m], inFocus (m));
    }
    drawName ("snap", juce::Rectangle<float> (xForHz (snapLowHz) + 8.0f, p.getY() + 3.0f, xForHz (snapHighHz) - xForHz (snapLowHz) - 10.0f, 12.0f),
              state.snapOn, inFocus (kNumModules));

    // Frequency marks under the plot.
    for (const auto& mark : { std::pair<float, const char*> { 50.0f, "50" }, { 100.0f, "100" }, { 200.0f, "200" }, { 500.0f, "500" },
                              { 1000.0f, "1k" }, { 2000.0f, "2k" }, { 4000.0f, "4k" } })
    {
        const float x = xForHz (mark.first);
        g.setColour (toy::ink.withAlpha (0.25f));
        g.fillRect (juce::Rectangle<float> (x - 0.5f, p.getBottom(), 1.0f, 4.0f));
        g.setColour (toy::ink.withAlpha (0.7f));
        drawShaped (g, shaped (mark.second, bodyFace, 9.5f), juce::Rectangle<float> (x - 20.0f, p.getBottom() + 4.0f, 40.0f, 11.0f), true);
    }

    // The level bar's well and its name.
    {
        const auto col = meter();
        g.setColour (toy::skyPale);
        g.fillRoundedRectangle (col, 6.5f);
        g.setColour (toy::ink.withAlpha (0.6f));
        drawShaped (g, shaped ("mix", nameFace, 8.5f), juce::Rectangle<float> (col.getX() - 6.0f, cutLane.getCentreY() - 6.0f, col.getWidth() + 12.0f, 12.0f), true);
    }

    g.setColour (toy::ink);
    g.drawRoundedRectangle (card, toy::cardCorner, toy::outline);
}

void BandView::paint (juce::Graphics& g)
{
    // The whole view is drawn here, by JUCE's own renderer, into an image at the screen's own
    // scale, and that image is handed to the window as one picture. Left to the window, each of
    // the bars, cuts and handles would be a separate shape for CoreGraphics to fill on the message
    // thread as the window is put on the screen, at every picture; drawn here it costs a fraction
    // of that. The parts that stay put are copied in from the backdrop, made again only when
    // something in it has changed.
    const float scale = juce::jlimit (1.0f, 4.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
    BackdropKey key {};
    size_t i = 0;
    for (int e = 0; e < kNumEdges; ++e)   key[i++] = state.edgeHz[e];
    for (int m = 0; m < kNumModules; ++m) key[i++] = state.moduleOn[m] ? 1.0f : 0.0f;
    key[i++] = state.snapOn ? 1.0f : 0.0f;
    key[i++] = state.bypass ? 1.0f : 0.0f;
    key[i++] = (float) focus;
    key[i++] = (float) getWidth();
    key[i++] = (float) getHeight();
    key[i++] = scale;
    key[i++] = (float) getX();
    key[i++] = (float) getY();
    key[i++] = getParentComponent() != nullptr ? (float) getParentWidth() : 0.0f;
    key[i++] = getParentComponent() != nullptr ? (float) getParentHeight() : 0.0f;
    if (! backdrop.isValid() || key != backdropKey)
    {
        backdropKey = key;
        renderBackdrop (scale);
    }

    if (picture.getWidth() != backdrop.getWidth() || picture.getHeight() != backdrop.getHeight())
        picture = juce::Image (juce::Image::ARGB, backdrop.getWidth(), backdrop.getHeight(), false, juce::SoftwareImageType());
    {
        const juce::Image::BitmapData from (backdrop, juce::Image::BitmapData::readOnly);
        juce::Image::BitmapData to (picture, juce::Image::BitmapData::writeOnly);
        const size_t row = (size_t) to.width * (size_t) to.pixelStride;
        for (int y = 0; y < to.height; ++y)
            std::memcpy (to.getLinePointer (y), from.getLinePointer (y), row);
    }
    {
        juce::Graphics pg (picture);
        pg.addTransform (juce::AffineTransform::scale (scale));
        paintMoving (pg);
    }

    g.setOpacity (1.0f);
    g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
    g.drawImageTransformed (picture, juce::AffineTransform::scale ((float) getWidth() / (float) picture.getWidth(),
                                                                   (float) getHeight() / (float) picture.getHeight()));
}

void BandView::paintMoving (juce::Graphics& g)
{
    const auto p = plot();
    const auto cutLane = lane();
    const auto lev = levels();
    const bool asleep = state.bypass;
    const int hot = dragging != noHandle ? dragging : hovered;

    {
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (p.toNearestInt());

        // The bars. Blue: going out. Red: taken out. Each colour is gathered into one shape and
        // filled once, which costs far less than a hundred small fills.
        const int n = juce::jlimit (0, kMaxBands, state.frame.numBands);
        const float step = n > 1 ? (xForHz (state.frame.bandHz[n - 1]) - xForHz (state.frame.bandHz[0])) / (float) (n - 1) : 6.0f;
        const float barW = juce::jmax (2.0f, step * 0.74f);
        juce::Path blue, pale, red, faintRed, voice;
        for (int k = 0; k < n; ++k)
        {
            const float x = xForHz (state.frame.bandHz[k]) - barW * 0.5f;
            const float yIn = yForLevel (level[k]), yOut = yForLevel (level[k] - cut[k]);
            if (lev.getBottom() - yIn < 1.0f)
                continue;
            const bool mine = focus < 0 || rangeOfHz (state.frame.bandHz[k], state.edgeHz) == focus;
            (mine ? blue : pale).addRoundedRectangle (x, yOut, barW, lev.getBottom() - yOut + barW, barW * 0.5f);
            if (cut[k] > 0.05f && ! asleep)
            {
                // What was there before the cut: a red cap on the bar, and the cut itself, magnified,
                // hanging in the lane above.
                auto& caps = mine ? red : faintRed;
                caps.addRoundedRectangle (x, yIn, barW, juce::jmax (2.5f, yOut - yIn), juce::jmin (barW * 0.5f, 2.0f));
                const float drop = cutLane.getHeight() * juce::jlimit (0.0f, 1.0f, cut[k] / cutScaleDb);
                caps.addRoundedRectangle (x - 1.0f, cutLane.getY() - barW, barW + 2.0f, drop + barW, (barW + 2.0f) * 0.5f);
            }
            if (state.hasFrame && state.frame.keyed && state.frame.dialogue[k] > 0.3f && ! asleep)
            {
                // A band that is being left to the dialogue: a lavender dot under its bar.
                const float r = juce::jmin (barW * 0.5f, 2.6f);
                voice.addEllipse (x + barW * 0.5f - r, lev.getBottom() - 2.0f * r - 1.0f, 2.0f * r, 2.0f * r);
            }
        }
        g.setColour (asleep ? toy::grey : toy::water);
        g.fillPath (blue);
        if (! pale.isEmpty())
        {
            g.setColour (asleep ? toy::grey.withAlpha (0.6f) : toy::waterPale);
            g.fillPath (pale);
        }
        g.setColour (toy::jacketRed);
        g.fillPath (red);
        if (! faintRed.isEmpty())
        {
            g.setColour (toy::jacketRed.withAlpha (0.45f));
            g.fillPath (faintRed);
        }
        if (! voice.isEmpty())
        {
            g.setColour (toy::lavender.darker (0.25f));
            g.fillPath (voice);
        }

        // Snap: one fat bar across its band, falling back after each click.
        if (clickHeld > 0.05f && ! asleep)
        {
            const float x0 = xForHz (snapLowHz) + 4.0f, x1 = xForHz (snapHighHz) - 4.0f;
            const float drop = cutLane.getHeight() * juce::jlimit (0.0f, 1.0f, clickHeld / cutScaleDb);
            g.setColour (toy::jacketRed.withAlpha (inFocus (kNumModules) ? 0.88f : 0.45f));
            g.fillRoundedRectangle (x0, cutLane.getY() - 8.0f, x1 - x0, drop + 8.0f, 6.0f);
        }
    }

    // The ranges' names, over the bars and the cuts: each on a small patch of its stretch's own
    // colour where a cut hangs behind it, so it reads whatever is going on there.
    for (const auto& n : names)
    {
        const auto& text = shaped (n.text, nameFace, 9.5f);
        const auto patch = n.area.withWidth (text.width).expanded (3.0f, 1.0f);
        bool covered = false;
        const int nb = juce::jlimit (0, kMaxBands, state.frame.numBands);
        for (int k = 0; k < nb && ! covered; ++k)
        {
            const float x = xForHz (state.frame.bandHz[k]);
            covered = cut[k] > 0.05f && ! asleep && x > patch.getX() - 4.0f && x < patch.getRight() + 4.0f;
        }
        if (clickHeld > 0.05f && ! asleep && patch.getX() >= xForHz (snapLowHz))
            covered = true;
        if (covered)
        {
            g.setColour (toy::paper.withAlpha (0.86f));
            g.fillRoundedRectangle (patch, 4.0f);
        }
        g.setColour (toy::ink.withAlpha (covered ? juce::jmax (0.7f, n.alpha) : n.alpha));
        drawShaped (g, text, n.area, false);
    }

    // Max Cut: a dashed line across the cut lane at the depth no cut passes, with a grip at its
    // right-hand end. It is dragged up and down.
    const float yCap = yForMaxCut();
    {
        const bool hotCap = hot == maxCutHandle;
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (p.toNearestInt().expanded (0, 3));
        {
            juce::Graphics::ScopedSaveState inner (g);
            for (const auto& area : nameAreas)
                g.excludeClipRegion (area);
            g.setColour (toy::ink.withAlpha (asleep ? 0.3f : (hotCap ? 1.0f : 0.6f)));
            // Dashes of 4 with gaps of 3, as rectangles filled together.
            const float thick = hotCap ? 1.6f : 1.0f;
            juce::RectangleList<float> dashes;
            for (float x = p.getX(); x < p.getRight() - 20.0f; x += 7.0f)
                dashes.addWithoutMerging ({ x, yCap - thick * 0.5f, juce::jmin (4.0f, p.getRight() - 20.0f - x), thick });
            g.fillRectList (dashes);
        }
        const auto grip = juce::Rectangle<float> (18.0f, hotCap ? 11.0f : 9.0f).withCentre ({ p.getRight() - 11.0f, yCap });
        g.setColour (hotCap ? toy::sunYellow : toy::paper);
        g.fillRoundedRectangle (grip, grip.getHeight() * 0.5f);
        g.setColour (toy::ink);
        g.drawRoundedRectangle (grip, grip.getHeight() * 0.5f, toy::outline);
        for (float dx : { -3.0f, 0.0f, 3.0f })
            g.fillRect (juce::Rectangle<float> (grip.getCentreX() + dx - 0.5f, grip.getCentreY() - 2.0f, 1.0f, 4.0f));
    }

    // The Threshold's line across the bands, on the bars' own scale.
    const bool always = state.thresholdDb <= alwaysAtOrBelowDb;
    // Red from the moment the mix is over the line (with a little slack, so that it does not
    // flicker as the level sits on it).
    over = state.hasFrame && state.frame.activity > (over ? 0.005f : 0.03f);
    const bool acting = over && ! asleep;
    const float yThreshold = yForLevel (state.thresholdDb);
    const bool hotThreshold = hot == thresholdHandle;
    if (! always)
    {
        g.setColour ((acting ? toy::jacketRed : toy::ink).withAlpha (asleep ? 0.25f : (hotThreshold ? 1.0f : 0.6f)));
        g.fillRect (juce::Rectangle<float> (p.getX(), yThreshold - (hotThreshold ? 0.9f : 0.6f), p.getWidth(), hotThreshold ? 1.8f : 1.2f));
    }

    // The edges: a line with a round handle at the top.
    for (int e = 0; e < kNumEdges; ++e)
    {
        const float x = xForHz (state.edgeHz[e]);
        const bool hotEdge = e == hot;
        g.setColour (toy::ink);
        g.fillRect (juce::Rectangle<float> (x - 0.6f, p.getY(), 1.2f, p.getHeight()));
        const float r = hotEdge ? 6.0f : 4.5f;
        g.setColour (hotEdge ? toy::sunYellow : toy::paper);
        g.fillEllipse (x - r, p.getY() - r + 2.0f, r * 2.0f, r * 2.0f);
        g.setColour (toy::ink);
        g.drawEllipse (x - r, p.getY() - r + 2.0f, r * 2.0f, r * 2.0f, toy::outline);
    }

    // The level of the whole mix, with the Threshold's grip on it. Yellow below the Threshold,
    // red once De-Honk is acting.
    {
        const auto col = meter();       // its well is in the backdrop
        const float top = yForLevel (programme);
        if (col.getBottom() - top > 1.0f)
        {
            g.setColour (asleep ? toy::grey : (acting ? toy::jacketRed : toy::sunYellow));
            g.fillRoundedRectangle (col.withTop (juce::jmin (top, col.getBottom() - 13.0f)), 6.5f);
        }
        g.setColour (toy::ink);
        g.drawRoundedRectangle (col, 6.5f, toy::outline);

        // The grip: a small flag pointing at the bar, joined to the line across the bands.
        const float gy = juce::jlimit (col.getY() + 1.0f, col.getBottom() - 1.0f, yThreshold);
        const float x0 = col.getRight() + 1.5f, x1 = p.getX() - 1.0f, half = hotThreshold ? 7.5f : 6.0f;
        juce::Path flag;
        flag.startNewSubPath (x0, gy);
        flag.lineTo (x0 + half, gy - half);
        flag.lineTo (x1 - 2.5f, gy - half);
        flag.quadraticTo (x1, gy - half, x1, gy - half + 2.5f);
        flag.lineTo (x1, gy + half - 2.5f);
        flag.quadraticTo (x1, gy + half, x1 - 2.5f, gy + half);
        flag.lineTo (x0 + half, gy + half);
        flag.closeSubPath();
        g.setColour (hotThreshold ? toy::sunYellow : (acting ? toy::jacketRed : toy::orange));
        g.fillPath (flag);
        g.setColour (toy::ink);
        g.strokePath (flag, juce::PathStrokeType (toy::outline, juce::PathStrokeType::curved));
        g.fillRect (juce::Rectangle<float> (col.getX(), gy - 0.6f, col.getWidth(), 1.2f));
    }

    // The plot's frame.
    g.setColour (toy::ink);
    g.drawRect (p.expanded (0.5f), toy::outline);

    // What a hot handle is set to.
    if (hot == thresholdHandle)
    {
        const auto text = always ? juce::String ("threshold: always") : "threshold " + juce::String (state.thresholdDb, 1) + " dBFS";
        const float y = yThreshold - 24.0f < lev.getY() ? yThreshold + 5.0f : yThreshold - 24.0f;
        chip (g, text, { p.getX() + 8.0f, y }, toy::sunYellow, true);
    }
    else if (hot == maxCutHandle)
    {
        chip (g, "max cut " + juce::String (state.maxCutDb, 1) + " dB", { p.getRight() - 26.0f, juce::jmin (yCap + 5.0f, lev.getY() - 2.0f) }, toy::sunYellow);
    }
    else if (hot >= 0 && hot < kNumEdges)
    {
        const float hz = state.edgeHz[hot], x = xForHz (hz);
        const auto text = hz >= 1000.0f ? juce::String (hz / 1000.0f, 2) + " kHz" : juce::String (juce::roundToInt (hz)) + " Hz";
        const float w = shaped (text, valueFace, 10.5f).width + 16.0f;
        chip (g, text, { juce::jlimit (p.getX() + w + 2.0f, p.getRight() - 2.0f, x + w + 8.0f), p.getY() + 14.0f }, toy::sunYellow);
    }

    // What is being heard, and the deepest cut: in the corner under the cut lane, beyond the last
    // band, where the plot is otherwise empty.
    float deepest = 0.0f;
    for (int k = 0; k < kMaxBands; ++k)
        deepest = juce::jmax (deepest, cut[k]);
    const juce::Point<float> at (p.getRight() - 6.0f, lev.getBottom() - 25.0f);
    if (asleep)
        chip (g, "bypassed", at, toy::grey);
    else if (state.diff)
        chip (g, "hearing: untreated", at, toy::sunYellow);
    else if (state.soloHonk)
        chip (g, "hearing: removed only", at, toy::sunYellow);
    else
        chip (g, "cut " + juce::String (deepest, 1) + " dB", at, deepest > 0.5f ? toy::pink : toy::paper);
}

//==============================================================================
void BandView::mouseMove (const juce::MouseEvent& e)
{
    setHovered (handleAt (e.position));
}

void BandView::mouseExit (const juce::MouseEvent&)
{
    if (dragging == noHandle)
        setHovered (noHandle);
}

void BandView::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
        return;
    draggedSinceDown = false;
    dragging = handleAt (e.position);
    // A handle is moved from where it was: by how far the pointer has gone since the press, not
    // to the pointer. With Shift (or cmd/ctrl) held it moves a tenth as far.
    dragFrom = e.position;
    dragValue = dragging == thresholdHandle ? state.thresholdDb : dragging == maxCutHandle ? state.maxCutDb
              : dragging >= 0 && dragging < kNumEdges ? state.edgeHz[dragging] : 0.0f;
    if (dragging != noHandle && onHandleDragStarted != nullptr)
        onHandleDragStarted (dragging);
    repaint();
}

void BandView::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging == noHandle || onHandleDragged == nullptr)
        return;
    // A press that has not moved changes nothing (a double-click must not nudge the handle first).
    if (! draggedSinceDown && e.getDistanceFromDragStart() < 2)
        return;
    draggedSinceDown = true;

    // Each stretch of the drag is added to where the handle had got to, at the rate in force
    // for that stretch, so pressing or letting go of the modifier half-way does not make it jump.
    const bool fine = e.mods.isShiftDown() || e.mods.isCommandDown();
    const auto moved = (e.position - dragFrom) * (fine ? 0.1f : 1.0f);
    dragFrom = e.position;
    if (dragging < kNumEdges)
    {
        // Along a logarithmic axis: the same distance is the same ratio wherever it starts.
        const float x = xForHz (dragValue) + moved.x;
        dragValue = juce::jlimit (kEdgeMinHz[dragging], kEdgeMaxHz[dragging], hzForX (x));
        onHandleDragged (dragging, dragValue);
    }
    else if (dragging == thresholdHandle)
    {
        const auto l = levels();
        dragValue = juce::jlimit (floorDb, 0.0f, dragValue + floorDb * moved.y / juce::jmax (1.0f, l.getHeight()));
        onHandleDragged (dragging, std::round (dragValue * 10.0f) / 10.0f);
    }
    else if (dragging == maxCutHandle)
    {
        const auto l = lane();
        dragValue = juce::jlimit (maxCutMinDb, maxCutMaxDb, dragValue + cutScaleDb * moved.y / juce::jmax (1.0f, l.getHeight()));
        onHandleDragged (dragging, std::round (dragValue * 10.0f) / 10.0f);
    }
}

void BandView::mouseUp (const juce::MouseEvent& e)
{
    if (dragging != noHandle && onHandleDragEnded != nullptr)
        onHandleDragEnded (dragging);
    dragging = noHandle;
    hovered = -2;       // so the cursor and the highlight are set again for where the pointer is now
    setHovered (getLocalBounds().toFloat().contains (e.position) ? handleAt (e.position) : (int) noHandle);
}

void BandView::mouseDoubleClick (const juce::MouseEvent& e)
{
    const int handle = handleAt (e.position);
    if (handle != noHandle && onHandleReset != nullptr)
        onHandleReset (handle);
}

} // namespace dehonk::scene
