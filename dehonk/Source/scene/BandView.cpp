#include "BandView.h"
#include "PondScene.h"
#include "ToyPalette.h"

namespace dehonk::scene
{

namespace
{
    constexpr float floorDb = -72.0f;       // bottom of the level scale
    constexpr float cutScaleDb = 12.0f;     // depth of the cut lane

    juce::Font label (float size) { return juce::Font (juce::FontOptions (size)).withExtraKerningFactor (0.08f); }

    /** A chip of text: a rounded pill with an ink outline. */
    void chip (juce::Graphics& g, const juce::String& text, juce::Point<float> rightTop, juce::Colour fill, float size = 11.0f)
    {
        const auto font = label (size);
        const float w = juce::GlyphArrangement::getStringWidth (font, text) + 16.0f, h = size + 9.0f;
        const juce::Rectangle<float> r (rightTop.x - w, rightTop.y, w, h);
        g.setColour (fill);
        g.fillRoundedRectangle (r, h * 0.5f);
        g.setColour (toy::ink);
        g.drawRoundedRectangle (r, h * 0.5f, toy::outline);
        g.setFont (font);
        g.drawText (text, r, juce::Justification::centred, false);
    }
}

BandView::BandView()
{
    for (auto& l : level)
        l = floorDb;
    setOpaque (false);
    setTooltip ("What De-Honk is doing in each band. Blue bars: the sound coming in. Red: what is being taken out. "
                "Drag the handles to move the edges between modules.");
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
juce::Rectangle<float> BandView::plot() const noexcept
{
    // Inside the card: a column on the left for the level meter, a row along the bottom for the
    // frequency marks.
    return getLocalBounds().toFloat().reduced (toy::outline + 6.0f).withTrimmedBottom (toy::dropY + 16.0f).withTrimmedLeft (34.0f);
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

int BandView::edgeAt (juce::Point<float> at) const noexcept
{
    int best = -1;
    float bestD = 9.0f;
    for (int e = 0; e < kNumEdges; ++e)
    {
        const float d = std::abs (at.x - xForHz (state.edgeHz[e]));
        if (d < bestD) { bestD = d; best = e; }
    }
    return plot().expanded (0.0f, 8.0f).contains (at) ? best : -1;
}

//==============================================================================
void BandView::paint (juce::Graphics& g)
{
    const auto card = getLocalBounds().toFloat().reduced (toy::outline * 0.5f).withTrimmedBottom (toy::dropY);
    const auto p = plot();
    const bool asleep = state.bypass;

    // The card, with its flat shadow.
    g.setColour (toy::paper);
    g.fillRoundedRectangle (card, toy::cardCorner);

    // The cut lane across the top (how much is taken out, on a big scale) and the levels below it.
    const float laneH = juce::jmax (22.0f, p.getHeight() * 0.36f);
    const auto lane = p.withHeight (laneH);
    const auto levels = p.withTrimmedTop (laneH + 4.0f);

    {
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (p.toNearestInt());

        // Each module's stretch, in its colour, and the Click band.
        for (int m = 0; m < kNumModules; ++m)
        {
            const float x0 = xForHz (state.edgeHz[m]), x1 = xForHz (state.edgeHz[m + 1]);
            if (x1 <= x0)
                continue;
            const bool on = state.moduleOn[m] && ! asleep;
            g.setColour ((on ? toy::module[m] : toy::grey).withAlpha (on ? 0.38f : 0.16f));
            g.fillRect (juce::Rectangle<float> (x0, p.getY(), x1 - x0, p.getHeight()));
        }
        {
            const float x0 = xForHz (1800.0f), x1 = xForHz (4000.0f);
            const bool on = state.snapOn && ! asleep;
            g.setColour ((on ? toy::click : toy::grey).withAlpha (on ? 0.38f : 0.10f));
            g.fillRect (juce::Rectangle<float> (x0, p.getY(), x1 - x0, p.getHeight()));
        }

        // Faint rules in the cut lane at 3, 6 and 9 dB.
        g.setColour (toy::ink.withAlpha (0.10f));
        for (float db : { 3.0f, 6.0f, 9.0f })
            g.fillRect (juce::Rectangle<float> (p.getX(), lane.getY() + lane.getHeight() * db / cutScaleDb, p.getWidth(), 1.0f));

        // The bars. Pale blue: coming in. Blue: going out. Red: taken out.
        const int n = juce::jlimit (0, kMaxBands, state.frame.numBands);
        const float step = n > 1 ? (xForHz (state.frame.bandHz[n - 1]) - xForHz (state.frame.bandHz[0])) / (float) (n - 1) : 6.0f;
        const float barW = juce::jmax (2.0f, step * 0.74f);
        auto yFor = [&levels] (float db) { return levels.getBottom() - levels.getHeight() * juce::jlimit (0.0f, 1.0f, (db - floorDb) / -floorDb); };
        for (int k = 0; k < n; ++k)
        {
            const float x = xForHz (state.frame.bandHz[k]) - barW * 0.5f;
            const float yIn = yFor (level[k]), yOut = yFor (level[k] - cut[k]);
            if (levels.getBottom() - yIn < 1.0f)
                continue;
            g.setColour (asleep ? toy::grey : toy::water);
            g.fillRoundedRectangle (x, yOut, barW, levels.getBottom() - yOut + barW, barW * 0.5f);
            if (cut[k] > 0.05f && ! asleep)
            {
                // What was there before the cut: a red cap on the bar, and the cut itself, magnified,
                // hanging in the lane above.
                g.setColour (toy::jacketRed);
                g.fillRoundedRectangle (x, yIn, barW, juce::jmax (2.5f, yOut - yIn), juce::jmin (barW * 0.5f, 2.0f));
                const float drop = lane.getHeight() * juce::jlimit (0.0f, 1.0f, cut[k] / cutScaleDb);
                g.fillRoundedRectangle (x - 1.0f, lane.getY() - barW, barW + 2.0f, drop + barW, (barW + 2.0f) * 0.5f);
            }
        }

        // Click: one fat bar across its band, falling back after each click.
        if (clickHeld > 0.05f && ! asleep)
        {
            const float x0 = xForHz (1800.0f) + 4.0f, x1 = xForHz (4000.0f) - 4.0f;
            const float drop = lane.getHeight() * juce::jlimit (0.0f, 1.0f, clickHeld / cutScaleDb);
            g.setColour (toy::jacketRed);
            g.fillRoundedRectangle (x0, lane.getY() - 8.0f, x1 - x0, drop + 8.0f, 6.0f);
        }
    }

    // Names along the top of each stretch.
    const auto nameFont = label (10.0f);
    g.setFont (nameFont);
    juce::RectangleList<int> nameAreas;     // where the names are, so the Max Cut line can step round them
    auto drawName = [&] (const char* text, juce::Rectangle<float> area, bool on)
    {
        g.setColour (toy::ink.withAlpha (on && ! asleep ? 0.85f : 0.35f));
        g.drawText (text, area, juce::Justification::centredLeft, false);
        const float width = juce::jmin (area.getWidth(), juce::GlyphArrangement::getStringWidth (nameFont, text));
        nameAreas.add (area.withWidth (width).expanded (4.0f, 2.0f).getSmallestIntegerContainer());
    };
    static const char* const names[kNumModules] = { "woof", "tub", "fog", "bark" };
    for (int m = 0; m < kNumModules; ++m)
    {
        const float x0 = xForHz (state.edgeHz[m]), x1 = xForHz (state.edgeHz[m + 1]);
        if (x1 - x0 < 30.0f)
            continue;
        drawName (names[m], juce::Rectangle<float> (x0 + 12.0f, p.getY() + 3.0f, x1 - x0 - 14.0f, 12.0f), state.moduleOn[m]);
    }
    drawName ("snap", juce::Rectangle<float> (xForHz (1800.0f) + 8.0f, p.getY() + 3.0f, xForHz (4000.0f) - xForHz (1800.0f) - 10.0f, 12.0f), state.snapOn);

    // Max Cut: a dashed line across the cut lane at the depth no cut passes.
    if (! asleep && state.maxCutDb > 0.0f && state.maxCutDb < cutScaleDb)
    {
        const float yCap = lane.getY() + lane.getHeight() * state.maxCutDb / cutScaleDb;
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (p.toNearestInt());
        for (const auto& area : nameAreas)
            g.excludeClipRegion (area);
        g.setColour (toy::ink.withAlpha (0.55f));
        const float dashes[] = { 4.0f, 3.0f };
        g.drawDashedLine ({ p.getX(), yCap, p.getRight(), yCap }, dashes, 2, 1.0f);
    }

    // Frequency marks under the plot.
    g.setFont (label (9.5f));
    for (const auto& mark : { std::pair<float, const char*> { 50.0f, "50" }, { 100.0f, "100" }, { 200.0f, "200" }, { 500.0f, "500" },
                              { 1000.0f, "1k" }, { 2000.0f, "2k" }, { 4000.0f, "4k" } })
    {
        const float x = xForHz (mark.first);
        g.setColour (toy::ink.withAlpha (0.25f));
        g.fillRect (juce::Rectangle<float> (x - 0.5f, p.getBottom(), 1.0f, 4.0f));
        g.setColour (toy::ink.withAlpha (0.7f));
        g.drawText (mark.second, juce::Rectangle<float> (x - 20.0f, p.getBottom() + 4.0f, 40.0f, 11.0f), juce::Justification::centred, false);
    }

    // The edges: a line with a round handle at the top.
    for (int e = 0; e < kNumEdges; ++e)
    {
        const float x = xForHz (state.edgeHz[e]);
        const bool hot = e == hovered || e == dragging;
        g.setColour (toy::ink);
        g.fillRect (juce::Rectangle<float> (x - 0.6f, p.getY(), 1.2f, p.getHeight()));
        const float r = hot ? 6.0f : 4.0f;
        g.setColour (hot ? toy::sunYellow : toy::paper);
        g.fillEllipse (x - r, p.getY() - r + 2.0f, r * 2.0f, r * 2.0f);
        g.setColour (toy::ink);
        g.drawEllipse (x - r, p.getY() - r + 2.0f, r * 2.0f, r * 2.0f, toy::outline);
        if (hot)
        {
            const float hz = state.edgeHz[e];
            const auto text = hz >= 1000.0f ? juce::String (hz / 1000.0f, 2) + " kHz" : juce::String (juce::roundToInt (hz)) + " Hz";
            chip (g, text, { juce::jlimit (p.getX() + 60.0f, p.getRight(), x + 34.0f), p.getY() + 12.0f }, toy::sunYellow);
        }
    }

    // The programme level, with the threshold marked. Yellow below it, red once De-Honk is acting.
    {
        const juce::Rectangle<float> col (card.getX() + 10.0f, p.getY(), 14.0f, p.getHeight());
        auto yFor = [&col] (float db) { return col.getBottom() - col.getHeight() * juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f); };
        g.setColour (toy::skyPale);
        g.fillRoundedRectangle (col, 7.0f);
        const float top = yFor (programme);
        if (col.getBottom() - top > 1.0f)
        {
            const bool acting = state.hasFrame && state.frame.activity > 0.5f && ! asleep;
            g.setColour (asleep ? toy::grey : (acting ? toy::jacketRed : toy::sunYellow));
            g.fillRoundedRectangle (col.withTop (juce::jmin (top, col.getBottom() - 14.0f)), 7.0f);
        }
        g.setColour (toy::ink);
        g.drawRoundedRectangle (col, 7.0f, toy::outline);
        if (state.thresholdDb > -59.9f)
        {
            const float y = yFor (state.thresholdDb);
            juce::Path tick;
            tick.addTriangle (col.getRight() + 9.0f, y - 5.0f, col.getRight() + 9.0f, y + 5.0f, col.getRight() + 1.0f, y);
            g.fillPath (tick);
        }
    }

    // The plot's frame, and the card's outline over everything.
    g.setColour (toy::ink);
    g.drawRect (p.expanded (0.5f), toy::outline);
    g.drawRoundedRectangle (card, toy::cardCorner, toy::outline);

    // What is being heard, and the deepest cut.
    float deepest = 0.0f;
    for (int k = 0; k < kMaxBands; ++k)
        deepest = juce::jmax (deepest, cut[k]);
    // In the corner under the cut lane, beyond the last band, where the plot is otherwise empty.
    juce::Point<float> at (p.getRight() - 6.0f, levels.getBottom() - 26.0f);
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
    const int now = edgeAt (e.position);
    if (now != hovered)
    {
        hovered = now;
        setMouseCursor (hovered >= 0 ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void BandView::mouseExit (const juce::MouseEvent&)
{
    if (hovered >= 0 && dragging < 0)
    {
        hovered = -1;
        setMouseCursor (juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void BandView::mouseDown (const juce::MouseEvent& e)
{
    dragging = edgeAt (e.position);
    if (dragging >= 0 && onEdgeDragStarted != nullptr)
        onEdgeDragStarted (dragging);
    repaint();
}

void BandView::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging >= 0 && onEdgeDragged != nullptr)
        onEdgeDragged (dragging, hzForX (e.position.x));
}

void BandView::mouseUp (const juce::MouseEvent& e)
{
    if (dragging >= 0 && onEdgeDragEnded != nullptr)
        onEdgeDragEnded (dragging);
    dragging = -1;
    hovered = edgeAt (e.position);
    repaint();
}

} // namespace dehonk::scene
