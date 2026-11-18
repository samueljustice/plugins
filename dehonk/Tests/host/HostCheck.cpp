// A small console host for the plug-in's own processor (the JUCE side, not just the core). It
// checks what the offline harness cannot: which channel layouts the plug-in accepts, that the LFE
// it finds in each layout comes out untouched, how it behaves with odd buffers and with a layout
// change, and which stem formats the JUCE AAX wrapper would offer Pro Tools.
//
// Built from the plug-in's CMake project, not by default:
//   cmake --build dehonk/build --config Release --target DeHonkHostCheck
// then run dehonk/build/DeHonkHostCheck_artefacts/DeHonkHostCheck (under a Release/ folder there
// with Xcode or Visual Studio). The exit code is 1 if any check failed.
#include <JuceHeader.h>

#include "DeHonkCore.h"

#include <cstring>
#include <iostream>
#include <vector>

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();

namespace
{
using S = juce::AudioChannelSet;

int failures = 0;

void check (bool ok, const juce::String& what)
{
    std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
    if (! ok)
        ++failures;
}

/** Noise with two ringing resonances (151 and 247 Hz) per channel, different on every channel. */
const std::vector<std::vector<float>>& signal()
{
    static const std::vector<std::vector<float>> sig = []
    {
        const int n = 48000 * 5;
        std::vector<std::vector<float>> s (16, std::vector<float> ((size_t) n));
        for (int c = 0; c < 16; ++c)
        {
            juce::Random r (99 + c);
            double lp = 0, y1[2] {}, y2[2] {};
            const double f[2] = { 151.0, 247.0 };
            for (int i = 0; i < n; ++i)
            {
                const double w = r.nextDouble() * 2.0 - 1.0;
                lp += 0.02 * (w - lp);
                double v = 2.0 * lp;
                for (int k = 0; k < 2; ++k)
                {
                    const double rr = 0.9985, th = juce::MathConstants<double>::twoPi * f[k] / 48000.0;
                    const double y = 2 * rr * std::cos (th) * y1[k] - rr * rr * y2[k] + w * 0.004;
                    y2[k] = y1[k];
                    y1[k] = y;
                    v += y;
                }
                s[(size_t) c][(size_t) i] = (float) juce::jlimit (-1.0, 1.0, v);
            }
        }
        return s;
    }();
    return sig;
}

void fill (juce::AudioBuffer<float>& b, juce::int64 pos)
{
    const auto& s = signal();
    for (int c = 0; c < b.getNumChannels(); ++c)
        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample (c, i, s[(size_t) c][(size_t) ((pos + i) % (juce::int64) s[0].size())]);
}

struct RunResult
{
    std::vector<int> differing;     // per buffer channel: samples in the second half that differ from the input
    bool finite = true;
};

/** Runs `seconds` of the test signal through `p` in blocks of `block` with `bufChannels` buffer
    channels, and compares each output sample with its input over the second half. */
RunResult run (juce::AudioProcessor& p, int bufChannels, int block, double seconds, bool bypassed = false)
{
    RunResult r;
    r.differing.assign ((size_t) bufChannels, 0);
    juce::MidiBuffer midi;
    const auto total = (juce::int64) (seconds * 48000.0);
    juce::AudioBuffer<float> in (bufChannels, block), io (bufChannels, block);
    for (juce::int64 pos = 0; pos < total; pos += block)
    {
        fill (in, pos);
        io.makeCopyOf (in, true);
        if (bypassed)
            p.processBlockBypassed (io, midi);
        else
            p.processBlock (io, midi);
        for (int c = 0; c < bufChannels; ++c)
            for (int i = 0; i < block; ++i)
            {
                const float v = io.getSample (c, i);
                r.finite = r.finite && std::isfinite (v);
                if (pos > total / 2)
                    r.differing[(size_t) c] += std::memcmp (in.getReadPointer (c) + i, io.getReadPointer (c) + i, sizeof (float)) != 0 ? 1 : 0;
            }
    }
    return r;
}

/** One character per channel: '=' passed through bit for bit, 'T' treated. */
juce::String show (const std::vector<int>& d)
{
    juce::String s;
    for (auto v : d)
        s << (v == 0 ? "=" : "T");
    return s;
}

bool apply (juce::AudioProcessor& p, const S& set, bool prepare)
{
    juce::AudioProcessor::BusesLayout bl;
    bl.inputBuses.add (set);
    bl.outputBuses.add (set);
    if (prepare)
        p.releaseResources();
    if (! p.setBusesLayout (bl))
        return false;
    if (prepare)
    {
        p.setRateAndBufferSizeDetails (48000.0, 512);
        p.prepareToPlay (48000.0, 512);
    }
    return true;
}

void setParam (juce::AudioProcessor& p, const char* id, float normalised)
{
    for (auto* prm : p.getParameters())
        if (auto* w = dynamic_cast<juce::AudioProcessorParameterWithID*> (prm))
            if (w->paramID == id)
                w->setValueNotifyingHost (normalised);
}

/** A processor acting all the time (Threshold at its lowest), at full Depth, so that every
    treated channel is visibly changed. Lookahead stays at its default of 0. */
std::unique_ptr<juce::AudioProcessor> makeProcessor()
{
    std::unique_ptr<juce::AudioProcessor> p (createPluginFilter());
    setParam (*p, "threshold", 0.0f);
    setParam (*p, "depth", 1.0f);
    return p;
}

struct LayoutEntry
{
    const char* name;
    S set;
};
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    const LayoutEntry supported[] = {
        { "mono", S::mono() }, { "stereo", S::stereo() }, { "LCR", S::createLCR() }, { "LCRS", S::createLCRS() },
        { "quad", S::quadraphonic() }, { "5.0", S::create5point0() }, { "5.1", S::create5point1() },
        { "6.0", S::create6point0() }, { "6.1", S::create6point1() }, { "7.0", S::create7point0() },
        { "7.1", S::create7point1() }, { "7.0 SDDS", S::create7point0SDDS() }, { "7.1 SDDS", S::create7point1SDDS() },
        { "5.0.2", S::create5point0point2() }, { "5.1.2", S::create5point1point2() }, { "5.0.4", S::create5point0point4() },
        { "5.1.4", S::create5point1point4() }, { "7.0.2", S::create7point0point2() }, { "7.1.2", S::create7point1point2() },
        { "7.0.4", S::create7point0point4() }, { "7.1.4", S::create7point1point4() },
    };
    const LayoutEntry refused[] = {
        { "7.1.6", S::create7point1point6() }, { "9.1.4", S::create9point1point4() }, { "discrete 6", S::discreteChannels (6) },
        { "ambisonic 1", S::ambisonic (1) }, { "6.0 Music", S::create6point0Music() }, { "6.1 Music", S::create6point1Music() },
        { "hexagonal", S::hexagonal() }, { "octagonal", S::octagonal() }, { "pentagonal", S::pentagonal() },
    };

    std::cout << "[ RUN  ] layouts accepted and refused, the LFE untouched" << std::endl;
    {
        auto p = makeProcessor();
        for (const auto& l : supported)
        {
            juce::AudioProcessor::BusesLayout bl;
            bl.inputBuses.add (l.set);
            bl.outputBuses.add (l.set);
            if (! p->checkBusesLayoutSupported (bl))
            {
                check (false, juce::String (l.name) + ": refused");
                continue;
            }
            apply (*p, l.set, true);
            const auto r = run (*p, l.set.size(), 512, 3.0);
            const int lfe = l.set.getChannelIndexForType (S::LFE);
            bool ok = r.finite && p->getLatencySamples() == 0;
            for (int c = 0; c < l.set.size(); ++c)
                ok = ok && ((c == lfe) == (r.differing[(size_t) c] == 0));
            check (ok, juce::String (l.name) + " (" + l.set.getSpeakerArrangementAsString() + "): accepted, " + show (r.differing)
                           + (lfe >= 0 ? ", LFE at " + juce::String (lfe) + " bit-identical to its input" : ", no LFE")
                           + ", every other channel treated, latency " + juce::String (p->getLatencySamples()));
        }
        for (const auto& l : refused)
        {
            juce::AudioProcessor::BusesLayout bl;
            bl.inputBuses.add (l.set);
            bl.outputBuses.add (l.set);
            check (! p->checkBusesLayoutSupported (bl), juce::String (l.name) + ": refused");
        }
        juce::AudioProcessor::BusesLayout mixed;
        mixed.inputBuses.add (S::stereo());
        mixed.outputBuses.add (S::create5point1());
        check (! p->checkBusesLayoutSupported (mixed), "stereo in, 5.1 out: refused");
    }

    std::cout << "[ RUN  ] odd buffers" << std::endl;
    {
        auto p = makeProcessor();
        apply (*p, S::create7point1point4(), true);
        juce::MidiBuffer midi;
        juce::AudioBuffer<float> empty (12, 0);
        p->processBlock (empty, midi);
        check (true, "a buffer of zero samples on 7.1.4: no crash");

        const auto wide = run (*p, 14, 512, 2.0);
        check (wide.finite && wide.differing[3] == 0 && wide.differing[12] == 0 && wide.differing[13] == 0 && wide.differing[0] > 0,
               "14 buffer channels on a 12-channel bus: " + show (wide.differing) + " (the LFE and the two extra untouched)");

        const auto narrow = run (*p, 8, 512, 2.0);
        bool allThrough = narrow.finite;
        for (auto d : narrow.differing)
            allThrough = allThrough && d == 0;
        check (allThrough, "8 buffer channels on a 12-channel bus: " + show (narrow.differing) + " (passed through)");

        for (int block : { 1, 4096 })
        {
            const auto r = run (*p, 12, block, 2.0);
            check (r.finite && r.differing[3] == 0 && r.differing[0] > 0, "block " + juce::String (block) + " against a prepared 512: " + show (r.differing));
        }

        const auto byp = run (*p, 12, 512, 2.0, true);
        bool through = byp.finite;
        for (auto d : byp.differing)
            through = through && d == 0;
        check (through, "processBlockBypassed at Lookahead 0: " + show (byp.differing) + " (the input, once the switch has faded)");
    }

    std::cout << "[ RUN  ] layout change" << std::endl;
    {
        auto p = makeProcessor();
        apply (*p, S::create5point1(), true);
        apply (*p, S::create7point1point4(), false);
        const auto before = run (*p, 12, 512, 1.0);
        bool through = before.finite;
        for (auto d : before.differing)
            through = through && d == 0;
        check (through, "5.1 -> 7.1.4 without prepareToPlay: " + show (before.differing) + " (passed through)");
        apply (*p, S::create7point1point4(), true);
        const auto after = run (*p, 12, 512, 2.0);
        check (after.finite && after.differing[3] == 0 && after.differing[0] > 0, "then prepareToPlay: " + show (after.differing) + " (treated)");

        // The same channel count, a different layout: 7.0 has no LFE, 6.1 has one.
        apply (*p, S::create7point0(), true);
        apply (*p, S::create6point1(), false);
        const auto same = run (*p, 7, 512, 1.0);
        bool same0 = same.finite;
        for (auto d : same.differing)
            same0 = same0 && d == 0;
        check (same0, "7.0 -> 6.1 (both 7 channels) without prepareToPlay: " + show (same.differing) + " (passed through)");
        apply (*p, S::create6point1(), true);
        const int lfe = S::create6point1().getChannelIndexForType (S::LFE);
        const auto sameAfter = run (*p, 7, 512, 2.0);
        bool ok = sameAfter.finite;
        for (int c = 0; c < 7; ++c)
            ok = ok && ((c == lfe) == (sameAfter.differing[(size_t) c] == 0));
        check (ok, "then prepareToPlay: " + show (sameAfter.differing) + ", LFE at " + juce::String (lfe) + " untouched");
    }

    // The JUCE AAX wrapper's own probe (its fullBusesLayoutFromMainLayout): from the default
    // layout, Bus::isLayoutSupported for the input then the output, then checkBusesLayoutSupported.
    // What passes is what Pro Tools is offered as stem formats.
    std::cout << "[ RUN  ] AAX stem formats" << std::endl;
    {
        std::unique_ptr<juce::AudioProcessor> q (createPluginFilter());
        std::vector<LayoutEntry> all (std::begin (supported), std::end (supported));
        all.insert (all.end(), std::begin (refused), std::end (refused));
        int offered = 0, mixed = 0;
        juce::String names;
        for (const auto& lin : all)
            for (const auto& lout : all)
            {
                juce::AudioProcessor::BusesLayout cur;
                cur.inputBuses.add (S::stereo());
                cur.outputBuses.add (S::stereo());
                if (! q->getBus (true, 0)->isLayoutSupported (lin.set, &cur)) continue;
                if (! q->getBus (false, 0)->isLayoutSupported (lout.set, &cur)) continue;
                if (cur.getMainInputChannelSet() != lin.set || cur.getMainOutputChannelSet() != lout.set) continue;
                if (! q->checkBusesLayoutSupported (cur)) continue;
                ++offered;
                mixed += lin.set == lout.set ? 0 : 1;
                names << lin.name << " ";
            }
        check (offered == (int) std::size (supported) && mixed == 0,
               "offers " + juce::String (offered) + " (need " + juce::String ((int) std::size (supported)) + "), " + juce::String (mixed) + " with input and output different: " + names);
    }

    std::cout << (failures == 0 ? "all host checks passed" : juce::String (failures) + " host checks failed") << std::endl;
    return failures == 0 ? 0 : 1;
}
