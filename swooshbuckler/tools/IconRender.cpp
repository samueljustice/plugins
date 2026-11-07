// Renders Swooshbuckler's app icon (Resources/icon.png, 1024 x 1024, and Resources/icon.ico for the Windows installer) with the app's own
// palette and cutlasses: crossed cutlasses over a doppler comet sweeping across a night sea.
// Not part of the release build: run it after changing the artwork, and commit the PNG.
//
//   cmake -B build -DSWOOSHBUCKLER_TOOLS=ON && cmake --build build --target SwooshbucklerIcon
//   ./build/SwooshbucklerIcon_artefacts/Debug/SwooshbucklerIcon Resources/icon.png

#include <JuceHeader.h>

namespace
{
    const juce::Colour deep       { 0xff06131a };
    const juce::Colour panel      { 0xff15323e };
    const juce::Colour lagoon     { 0xff3ccbc2 };
    const juce::Colour coral      { 0xffff8466 };
    const juce::Colour gold       { 0xfff2b84b };
    const juce::Colour sand       { 0xfff2e8d2 };
    const juce::Colour driftwood  { 0xffd3c9b4 };
    const juce::Colour brass      { 0xffdcae5c };
    const juce::Colour brassDark  { 0xff7d5a26 };

    /** One cutlass: a curved blade with a lit edge, a basket guard and a wrapped grip (same shape as the header's). */
    void drawCutlass(juce::Graphics& g, juce::Rectangle<float> area, bool mirror)
    {
        const auto t = mirror ? juce::AffineTransform::scale(-1.0f, 1.0f, area.getCentreX(), 0.0f) : juce::AffineTransform();
        const float x0 = area.getX(), y0 = area.getY(), w = area.getWidth(), h = area.getHeight();
        auto P = [&](float fx, float fy) { return juce::Point<float>(x0 + fx * w, y0 + fy * h); };

        juce::Path blade;
        blade.startNewSubPath(P(0.18f, 0.84f));
        blade.quadraticTo(P(0.55f, 0.55f), P(0.95f, 0.02f));
        blade.quadraticTo(P(0.72f, 0.50f), P(0.25f, 0.90f));
        blade.closeSubPath();

        // Shadow, steel, then a bright cutting edge along the outer curve.
        g.setColour(juce::Colours::black.withAlpha(0.45f));
        g.fillPath(blade, t.translated(mirror ? -10.0f : 10.0f, 14.0f));
        g.setGradientFill(juce::ColourGradient(sand, P(0.9f, 0.05f).transformedBy(t), driftwood.darker(0.35f), P(0.2f, 0.88f).transformedBy(t), false));
        g.fillPath(blade, t);
        juce::Path edge;
        edge.startNewSubPath(P(0.20f, 0.84f));
        edge.quadraticTo(P(0.56f, 0.56f), P(0.94f, 0.04f));
        g.setColour(juce::Colours::white.withAlpha(0.85f));
        g.strokePath(edge, juce::PathStrokeType(w * 0.012f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded), t);

        // Guard: a brass bow.
        juce::Path guard;
        guard.startNewSubPath(P(0.06f, 0.73f));
        guard.quadraticTo(P(0.25f, 0.79f), P(0.33f, 0.98f));
        const juce::PathStrokeType guardStroke(w * 0.075f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
        g.setColour(brassDark);
        g.strokePath(guard, guardStroke, t.translated(0.0f, w * 0.012f));
        g.setColour(brass);
        g.strokePath(guard, guardStroke, t);

        // Grip, with a pommel.
        juce::Path grip;
        grip.startNewSubPath(P(0.20f, 0.87f));
        grip.lineTo(P(0.05f, 0.99f));
        g.setColour(brassDark);
        g.strokePath(grip, juce::PathStrokeType(w * 0.065f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded), t);
        const auto pommel = P(0.04f, 1.0f).transformedBy(t);
        g.setColour(brass);
        g.fillEllipse(juce::Rectangle<float>(w * 0.085f, w * 0.085f).withCentre(pommel));
    }

    /** The doppler pass: a comet sweeping up across the icon, thick and hot at the head, fading to a thin tail. */
    void drawComet(juce::Graphics& g, juce::Rectangle<float> body)
    {
        const auto at = [&](float fx, float fy) { return juce::Point<float>(body.getX() + fx * body.getWidth(), body.getY() + fy * body.getHeight()); };
        constexpr int n = 120;
        // Head at the right, tail at the left, along a gentle arc.
        auto pointOn = [&](float u)
        {
            const auto a = at(0.00f, 0.78f), c = at(0.40f, 0.30f), b = at(0.84f, 0.27f);
            const float v = 1.0f - u;
            return a * (v * v) + c * (2.0f * u * v) + b * (u * u);
        };
        for (int pass = 0; pass < 3; ++pass)
        {
            // Wide soft glow, then the body, then a hot core.
            const float widthScale[] = { 3.2f, 1.0f, 0.35f };
            const float alphaScale[] = { 0.12f, 0.9f, 1.0f };
            for (int i = 0; i < n; ++i)
            {
                const float u0 = static_cast<float>(i) / n, u1 = static_cast<float>(i + 1) / n;
                const float taper = std::pow(u1, 1.6f);
                juce::Path seg;
                seg.startNewSubPath(pointOn(u0));
                seg.lineTo(pointOn(u1));
                const auto colour = pass == 2 ? sand : coral.interpolatedWith(gold, std::pow(u1, 0.7f));
                g.setColour(colour.withAlpha(alphaScale[pass] * (0.08f + 0.92f * taper)));
                g.strokePath(seg, juce::PathStrokeType(body.getWidth() * 0.075f * widthScale[pass] * (0.06f + taper),
                                                       juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }
        }
        // The head: a bright ball with a halo.
        const auto head = pointOn(1.0f);
        const float r = body.getWidth() * 0.05f;
        g.setGradientFill(juce::ColourGradient(gold.withAlpha(0.55f), head, gold.withAlpha(0.0f), head.translated(r * 3.0f, 0.0f), true));
        g.fillEllipse(juce::Rectangle<float>(r * 6.0f, r * 6.0f).withCentre(head));
        g.setColour(sand);
        g.fillEllipse(juce::Rectangle<float>(r * 1.3f, r * 1.3f).withCentre(head));
    }

    void drawSea(juce::Graphics& g, juce::Rectangle<float> body)
    {
        for (int layer = 0; layer < 3; ++layer)
        {
            const float top = body.getY() + body.getHeight() * (0.72f + 0.08f * static_cast<float>(layer));
            const float amp = body.getHeight() * (0.025f - 0.005f * static_cast<float>(layer));
            const float waves = 2.5f + static_cast<float>(layer);
            juce::Path p;
            p.startNewSubPath(body.getX(), body.getBottom());
            for (int i = 0; i <= 64; ++i)
            {
                const float x = body.getX() + body.getWidth() * static_cast<float>(i) / 64.0f;
                const float y = top + amp * std::sin(juce::MathConstants<float>::twoPi * waves * static_cast<float>(i) / 64.0f + static_cast<float>(layer));
                p.lineTo(x, y);
            }
            p.lineTo(body.getRight(), body.getBottom());
            p.closeSubPath();
            g.setColour(lagoon.withMultipliedSaturation(0.85f).withBrightness(0.36f - 0.07f * static_cast<float>(layer)));
            g.fillPath(p);
            // A lit crest on each wave
            juce::Path crest;
            for (int i = 0; i <= 64; ++i)
            {
                const float x = body.getX() + body.getWidth() * static_cast<float>(i) / 64.0f;
                const float y = top + amp * std::sin(juce::MathConstants<float>::twoPi * waves * static_cast<float>(i) / 64.0f + static_cast<float>(layer));
                if (i == 0) crest.startNewSubPath(x, y); else crest.lineTo(x, y);
            }
            g.setColour(lagoon.withAlpha(0.35f + 0.15f * static_cast<float>(layer)));
            g.strokePath(crest, juce::PathStrokeType(body.getWidth() * 0.006f));
        }
    }

    juce::Image render(int size)
    {
        juce::Image image(juce::Image::ARGB, size, size, true);
        juce::Graphics g(image);
        const float s = static_cast<float>(size);
        // macOS icon grid: an 824 / 1024 rounded square, centred, with room for the drop shadow.
        const auto body = juce::Rectangle<float>(s, s).reduced(s * 100.0f / 1024.0f);
        const float corner = body.getWidth() * 0.225f;
        juce::Path shape;
        shape.addRoundedRectangle(body, corner);

        // Drop shadow
        for (int i = 6; i > 0; --i)
        {
            g.setColour(juce::Colours::black.withAlpha(0.05f));
            g.fillRoundedRectangle(body.translated(0.0f, s * 0.012f).expanded(s * 0.004f * static_cast<float>(i)), corner);
        }

        g.saveState();
        g.reduceClipRegion(shape);
        g.setGradientFill(juce::ColourGradient(panel.brighter(0.15f), body.getTopLeft(), deep, body.getBottomLeft(), false));
        g.fillRect(body);
        // Sunset glow behind the head of the comet
        g.setGradientFill(juce::ColourGradient(coral.withAlpha(0.18f), body.getX() + body.getWidth() * 0.84f, body.getY() + body.getHeight() * 0.27f,
                                               coral.withAlpha(0.0f), body.getX() + body.getWidth() * 0.35f, body.getY() + body.getHeight() * 0.8f, true));
        g.fillRect(body);
        // Stars
        juce::Random rng(7);
        for (int i = 0; i < 40; ++i)
        {
            const float x = body.getX() + rng.nextFloat() * body.getWidth();
            const float y = body.getY() + rng.nextFloat() * body.getHeight() * 0.55f;
            const float r = s * (0.002f + 0.003f * rng.nextFloat());
            g.setColour(sand.withAlpha(0.25f + 0.5f * rng.nextFloat()));
            g.fillEllipse(x - r, y - r, 2.0f * r, 2.0f * r);
        }
        drawSea(g, body);
        drawComet(g, body);

        const auto swords = body.withSizeKeepingCentre(body.getWidth() * 0.62f, body.getHeight() * 0.62f).translated(0.0f, body.getHeight() * 0.02f);
        drawCutlass(g, swords, false);
        drawCutlass(g, swords, true);

        // A thin inner rim, like the panels' outline
        g.restoreState();
        g.setColour(gold.withAlpha(0.35f));
        g.drawRoundedRectangle(body.reduced(s * 0.004f), corner, s * 0.006f);
        return image;
    }
}

/** A Windows .ico holding PNG images (allowed since Vista): header, directory, then each PNG. */
bool writeIco(const juce::File& file, std::initializer_list<int> sizes)
{
    std::vector<juce::MemoryBlock> pngs;
    for (int size : sizes)
    {
        juce::MemoryOutputStream png;
        juce::PNGImageFormat().writeImageToStream(render(1024).rescaled(size, size, juce::Graphics::highResamplingQuality), png);
        pngs.push_back(png.getMemoryBlock());
    }
    juce::MemoryOutputStream out;
    out.writeShort(0);
    out.writeShort(1);
    out.writeShort(static_cast<short>(pngs.size()));
    int offset = 6 + 16 * static_cast<int>(pngs.size());
    size_t i = 0;
    for (int size : sizes)
    {
        out.writeByte(static_cast<char>(size >= 256 ? 0 : size));
        out.writeByte(static_cast<char>(size >= 256 ? 0 : size));
        out.writeByte(0);
        out.writeByte(0);
        out.writeShort(1);
        out.writeShort(32);
        out.writeInt(static_cast<int>(pngs[i].getSize()));
        out.writeInt(offset);
        offset += static_cast<int>(pngs[i].getSize());
        ++i;
    }
    for (const auto& png : pngs)
        out.write(png.getData(), png.getSize());
    return file.replaceWithData(out.getData(), out.getDataSize());
}

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init;
    const juce::File out = argc > 1 ? juce::File::getCurrentWorkingDirectory().getChildFile(argv[1])
                                    : juce::File::getCurrentWorkingDirectory().getChildFile("icon.png");
    const auto image = render(1024);
    out.deleteFile();
    juce::FileOutputStream stream(out);
    juce::PNGImageFormat png;
    if (! stream.openedOk() || ! png.writeImageToStream(image, stream))
    {
        std::cerr << "Couldn't write " << out.getFullPathName() << "\n";
        return 1;
    }
    std::cout << "Wrote " << out.getFullPathName() << "\n";
    const auto ico = out.withFileExtension("ico");
    if (! writeIco(ico, { 16, 24, 32, 48, 64, 128, 256 }))
    {
        std::cerr << "Couldn't write " << ico.getFullPathName() << "\n";
        return 1;
    }
    std::cout << "Wrote " << ico.getFullPathName() << "\n";
    return 0;
}
