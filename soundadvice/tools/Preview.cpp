// Renders the editor offscreen to PNGs: a handful of entries in paper and night, plus the
// longest and shortest entries, so layout changes can be checked without a host.
//   SoundAdvicePreview <out-dir> [count] [width height]
//   SoundAdvicePreview --hero <out.png> <paper-entry-id> <night-entry-id>
//     the README image: one page in paper and one in night, side by side
//   SoundAdvicePreview --icon <out.png>
//     the app icon: <out.png> at 1024 x 1024 and <out.ico> for Windows. Commit both.

#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    void render (SoundAdviceProcessor& proc, int width, int height, const juce::File& file)
    {
        std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
        editor->setSize (width, height);
        const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);
        file.deleteFile();
        juce::FileOutputStream out (file);
        juce::PNGImageFormat().writeImageToStream (image, out);
    }
}

namespace
{
    void showEntry (SoundAdviceProcessor& proc, const juce::String& category, const juce::String& entry, bool night)
    {
        juce::ValueTree tree ("SoundAdvice");
        tree.setProperty ("category", category, nullptr);
        tree.setProperty ("entry", entry, nullptr);
        tree.setProperty ("night", night, nullptr);
        juce::MemoryBlock state;
        if (auto xml = tree.createXml())
            juce::AudioProcessor::copyXmlToBinary (*xml, state);
        proc.setStateInformation (state.getData(), (int) state.getSize());
    }

    juce::Image snapshot (SoundAdviceProcessor& proc, int width, int height, float scale)
    {
        std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
        editor->setSize (width, height);
        return editor->createComponentSnapshot (editor->getLocalBounds(), true, scale);
    }

    int hero (const juce::File& file, const juce::String& paperId, const juce::String& nightId)
    {
        SoundAdviceProcessor proc;
        const auto& lib = proc.getLibrary();
        auto categoryOf = [&lib] (const juce::String& id)
        {
            const auto e = lib.findEntry (id);
            return e >= 0 ? lib.getEntries()[(size_t) e].categories[0] : juce::String ("all");
        };
        if (lib.findEntry (paperId) < 0 || lib.findEntry (nightId) < 0)
        {
            std::cout << "unknown entry id\n";
            return 1;
        }

        constexpr float scale = 2.0f;
        showEntry (proc, categoryOf (paperId), paperId, false);
        const auto paper = snapshot (proc, 560, 360, scale);
        showEntry (proc, categoryOf (nightId), nightId, true);
        const auto night = snapshot (proc, 560, 360, scale);

        const int pad = 72, w = paper.getWidth(), h = paper.getHeight();
        juce::Image canvas (juce::Image::ARGB, w * 2 + pad * 3, h + pad * 2, true);
        juce::Graphics g (canvas);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xffd9d1c4), 0.0f, 0.0f,
                                                 juce::Colour (0xffb9b0a3), 0.0f, (float) canvas.getHeight(), false));
        g.fillAll();

        const juce::DropShadow shadow (juce::Colours::black.withAlpha (0.28f), 48, { 0, 18 });
        for (int i = 0; i < 2; ++i)
        {
            const juce::Rectangle<int> r (pad + i * (w + pad), pad, w, h);
            shadow.drawForRectangle (g, r);
            g.setOpacity (1.0f);   // the shadow leaves its alpha on the context, and images draw with it
            g.drawImageAt (i == 0 ? paper : night, r.getX(), r.getY());
        }

        file.deleteFile();
        juce::FileOutputStream out (file);
        juce::PNGImageFormat().writeImageToStream (canvas, out);
        std::cout << "wrote " << file.getFullPathName() << "\n";
        return 0;
    }
}

namespace
{
    /** The icon: the plug-in's paper card on the macOS icon grid, a large opening quote mark in the
        accent colour, and the die tumbling in the corner. */
    juce::Image renderIcon (int size)
    {
        const auto paper = Theme::palette (false);
        juce::Image image (juce::Image::ARGB, size, size, true);
        juce::Graphics g (image);
        const float s = (float) size;

        // macOS icon grid: an 824 / 1024 rounded square, centred, with room for the drop shadow.
        const auto body = juce::Rectangle<float> (s, s).reduced (s * 100.0f / 1024.0f);
        const float corner = body.getWidth() * 0.225f;
        juce::Path shape;
        shape.addRoundedRectangle (body, corner);

        for (int i = 6; i > 0; --i)
        {
            g.setColour (juce::Colours::black.withAlpha (0.035f));
            g.fillRoundedRectangle (body.translated (0.0f, s * 0.012f).expanded (s * 0.004f * (float) i), corner);
        }

        g.saveState();
        g.reduceClipRegion (shape);
        g.setGradientFill (juce::ColourGradient (paper.background.brighter (0.02f), body.getCentreX(), body.getY() + body.getHeight() * 0.35f,
                                                 paper.backgroundEdge.darker (0.04f), body.getX(), body.getBottom(), true));
        g.fillRect (body);

        // The quote mark, set in the plug-in's italic, a little above and left of centre.
        const auto quoteMark = juce::String::fromUTF8 ("\xe2\x80\x9c");
        const auto font = Theme::italic (body.getHeight() * 0.95f);
        juce::GlyphArrangement glyphs;
        glyphs.addLineOfText (font, quoteMark, 0.0f, 0.0f);
        juce::Path mark;
        glyphs.createPath (mark);
        const auto target = body.withSizeKeepingCentre (body.getWidth() * 0.48f, body.getHeight() * 0.38f)
                                .translated (-body.getWidth() * 0.07f, -body.getHeight() * 0.08f);
        mark.applyTransform (mark.getTransformToScaleToFit (target, true));
        g.setColour (paper.accent);
        g.fillPath (mark);

        // The plug-in's hairline frame, inset, like a printed card.
        g.setColour (paper.hairline);
        g.drawRoundedRectangle (body.reduced (body.getWidth() * 0.06f), corner * 0.72f, s * 0.004f);

        // The die, tilted, bottom right: the same drawing as the button, scaled up.
        const auto die = juce::Rectangle<float> (body.getWidth() * 0.2f, body.getWidth() * 0.2f)
                             .withCentre ({ body.getX() + body.getWidth() * 0.74f, body.getY() + body.getHeight() * 0.75f });
        juce::Graphics::ScopedSaveState tilt (g);
        g.addTransform (juce::AffineTransform::rotation (0.26f, die.getCentreX(), die.getCentreY()));
        g.setColour (paper.ink.withAlpha (0.85f));
        g.drawRoundedRectangle (die, die.getWidth() * 0.2f, die.getWidth() * 0.065f);
        const auto inset = die.reduced (die.getWidth() * 0.27f);
        const float pip = die.getWidth() * 0.095f;
        for (auto [fx, fy] : { std::pair { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 0.5f, 0.5f }, { 0.0f, 1.0f }, { 1.0f, 1.0f } })
            g.fillEllipse (juce::Rectangle<float> (pip * 2.0f, pip * 2.0f)
                               .withCentre ({ inset.getX() + inset.getWidth() * fx, inset.getY() + inset.getHeight() * fy }));
        return image;
    }

    /** A Windows .ico holding PNG images (allowed since Vista): header, directory, then each PNG. */
    bool writeIco (const juce::File& file, std::initializer_list<int> sizes)
    {
        const auto master = renderIcon (1024);
        std::vector<juce::MemoryBlock> pngs;
        for (int size : sizes)
        {
            juce::MemoryOutputStream png;
            juce::PNGImageFormat().writeImageToStream (master.rescaled (size, size, juce::Graphics::highResamplingQuality), png);
            pngs.push_back (png.getMemoryBlock());
        }
        juce::MemoryOutputStream out;
        out.writeShort (0);
        out.writeShort (1);
        out.writeShort ((short) pngs.size());
        int offset = 6 + 16 * (int) pngs.size();
        size_t i = 0;
        for (int size : sizes)
        {
            out.writeByte ((char) (size >= 256 ? 0 : size));
            out.writeByte ((char) (size >= 256 ? 0 : size));
            out.writeByte (0);
            out.writeByte (0);
            out.writeShort (1);
            out.writeShort (32);
            out.writeInt ((int) pngs[i].getSize());
            out.writeInt (offset);
            offset += (int) pngs[i].getSize();
            ++i;
        }
        for (const auto& png : pngs)
            out.write (png.getData(), png.getSize());
        return file.replaceWithData (out.getData(), out.getDataSize());
    }

    int icon (const juce::File& file)
    {
        file.deleteFile();
        {
            juce::FileOutputStream out (file);
            if (! out.openedOk() || ! juce::PNGImageFormat().writeImageToStream (renderIcon (1024), out))
                return 1;
        }
        if (! writeIco (file.withFileExtension ("ico"), { 16, 24, 32, 48, 64, 128, 256 }))
            return 1;
        std::cout << "wrote " << file.getFullPathName() << " and its .ico\n";
        return 0;
    }
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    if (argc == 3 && juce::String (argv[1]) == "--icon")
        return icon (juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]));
    if (argc == 5 && juce::String (argv[1]) == "--hero")
        return hero (juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]), argv[3], argv[4]);
    const juce::File outDir (argc > 1 ? juce::File::getCurrentWorkingDirectory().getChildFile (argv[1])
                                      : juce::File::getCurrentWorkingDirectory());
    const int count = argc > 2 ? juce::String (argv[2]).getIntValue() : 4;
    const int width = argc > 4 ? juce::String (argv[3]).getIntValue() : 560;
    const int height = argc > 4 ? juce::String (argv[4]).getIntValue() : 360;
    outDir.createDirectory();

    SoundAdviceProcessor proc;
    const auto& cats = proc.getLibrary().getCategories();
    for (int i = 0; i < count; ++i)
    {
        proc.selectCategory (1 + i % ((int) cats.size() - 1));
        proc.setNightMode (i % 2 == 1);
        render (proc, width, height, outDir.getChildFile ("preview-" + juce::String (i) + ".png"));
    }

    // Extremes: the longest entry decides the smallest type size.
    const auto& entries = proc.getLibrary().getEntries();
    size_t longest = 0;
    for (size_t i = 0; i < entries.size(); ++i)
        if (entries[i].text.length() > entries[longest].text.length())
            longest = i;
    juce::MemoryBlock state;
    juce::ValueTree tree ("SoundAdvice");
    tree.setProperty ("category", "all", nullptr);
    tree.setProperty ("entry", entries[longest].id, nullptr);
    if (auto xml = tree.createXml())
        juce::AudioProcessor::copyXmlToBinary (*xml, state);
    proc.setStateInformation (state.getData(), (int) state.getSize());
    render (proc, width, height, outDir.getChildFile ("preview-longest.png"));
    std::cout << "longest: " << entries[longest].text.length() << " chars\n";
    return 0;
}
