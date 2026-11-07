// Renders the editor offscreen to PNGs: a handful of entries in paper and night, plus the
// longest and shortest entries, so layout changes can be checked without a host.
//   SoundAdvicePreview <out-dir> [count] [width height]
//   SoundAdvicePreview --hero <out.png> <paper-entry-id> <night-entry-id>
//     the README image: one page in paper and one in night, side by side

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

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
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
