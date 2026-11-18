// A small console host for the plug-in's own processor (the JUCE side, not just the core). It
// checks what the offline harness cannot: which channel layouts the plug-in accepts, that the LFE
// it finds in each layout comes out untouched, how it behaves with odd buffers and with a layout
// change, which stem formats the JUCE AAX wrapper would offer Pro Tools, the dialogue sidechain
// bus (which layouts it takes, that it is only read, that Pro Tools would be offered a key input)
// and that a state from the first test build is read (Link as a percentage becomes a mode).
//
// Built from the plug-in's CMake project, not by default:
//   cmake --build dehonk/build --config Release --target DeHonkHostCheck
// then run dehonk/build/DeHonkHostCheck_artefacts/DeHonkHostCheck (under a Release/ folder there
// with Xcode or Visual Studio). The exit code is 1 if any check failed.
#include "FirstBuildStates.h"
#include <JuceHeader.h>

#include "DeHonkCore.h"
#include "../../Source/PluginProcessor.h"

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

/** The main input and output as `set`, and the dialogue sidechain as `key` (off by default). */
juce::AudioProcessor::BusesLayout layoutFor (const S& in, const S& out, const S& key = S::disabled())
{
    juce::AudioProcessor::BusesLayout bl;
    bl.inputBuses.add (in);
    bl.inputBuses.add (key);
    bl.outputBuses.add (out);
    return bl;
}

bool apply (juce::AudioProcessor& p, const S& set, bool prepare, const S& key = S::disabled())
{
    const auto bl = layoutFor (set, set, key);
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
            const auto bl = layoutFor (l.set, l.set);
            if (! p->checkBusesLayoutSupported (bl) || ! p->checkBusesLayoutSupported (layoutFor (l.set, l.set, S::mono()))
                || ! p->checkBusesLayoutSupported (layoutFor (l.set, l.set, S::stereo())) || p->checkBusesLayoutSupported (layoutFor (l.set, l.set, S::createLCR())))
            {
                check (false, juce::String (l.name) + ": refused, or not with the sidechain off, mono and stereo and nothing else");
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
            check (! p->checkBusesLayoutSupported (layoutFor (l.set, l.set)) && ! p->checkBusesLayoutSupported (layoutFor (l.set, l.set, S::mono())),
                   juce::String (l.name) + ": refused");
        }
        check (! p->checkBusesLayoutSupported (layoutFor (S::stereo(), S::create5point1())), "stereo in, 5.1 out: refused");
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
                auto cur = q->getBusesLayout();
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

    // The dialogue sidechain.
    std::cout << "[ RUN  ] dialogue sidechain" << std::endl;
    {
        // The AAX wrapper offers a key input when the second input bus is mono in the layout it
        // settles on (from the default layout with every bus on), and lets Pro Tools switch it
        // off if the plug-in also takes the bus disabled.
        std::unique_ptr<juce::AudioProcessor> q (createPluginFilter());
        const bool twoIns = q->getBusCount (true) == 2 && q->getBusCount (false) == 1;
        const auto def = q->getBusesLayout();
        const bool monoKey = twoIns && q->getBus (true, 1)->getDefaultLayout() == S::mono();
        auto cur = def;
        const bool canMono = twoIns && q->getBus (true, 1)->isLayoutSupported (S::mono(), &cur) && cur.getChannelSet (true, 1) == S::mono();
        check (twoIns && monoKey && canMono && q->checkBusesLayoutSupported (layoutFor (S::create7point1point4(), S::create7point1point4(), S::mono()))
                   && q->checkBusesLayoutSupported (layoutFor (S::create7point1point4(), S::create7point1point4())),
               "two input buses, the second (\"" + (twoIns ? q->getBus (true, 1)->getName() : juce::String()) + "\") mono by default and may be off: Pro Tools is offered a mono key input, in every stem format");

        // The key's channels come after the main input's in the buffer and are only read.
        for (const auto& key : { S::mono(), S::stereo() })
            for (const auto& set : { S::mono(), S::stereo(), S::create5point1(), S::create7point1point4() })
            {
                auto p = makeProcessor();
                const bool applied = apply (*p, set, true, key);
                const int total = set.size() + key.size();
                const auto off = applied ? run (*p, total, 512, 2.0) : RunResult {};
                setParam (*p, "dialogue", 1.0f);
                const auto on = applied ? run (*p, total, 512, 3.0) : RunResult {};
                const int lfe = set.getChannelIndexForType (S::LFE);
                bool ok = applied && off.finite && on.finite;
                // Off: every main channel but the LFE is treated. On, with a key that is the first
                // channels of the mix itself: the first channel's bands are all the dialogue's.
                for (int c = 0; ok && c < total; ++c)
                {
                    const bool keyChannel = c >= set.size();
                    ok = ok && ((c == lfe || keyChannel) == (off.differing[(size_t) c] == 0));
                    ok = ok && (! keyChannel || on.differing[(size_t) c] == 0);
                }
                check (ok, juce::String (set.getDescription()) + " with a " + key.getDescription() + " sidechain: Dialogue off " + show (off.differing)
                               + ", on " + show (on.differing) + " (the key's channels, last, are never written)");
            }

        // Dialogue on with the mix itself as the key: everything is the dialogue's, nothing is cut.
        {
            auto p = makeProcessor();
            apply (*p, S::mono(), true, S::mono());
            setParam (*p, "dialogue", 1.0f);
            juce::MidiBuffer midi;
            juce::AudioBuffer<float> io (2, 512);
            int differing = 0;
            for (juce::int64 pos = 0; pos < 48000 * 4; pos += 512)
            {
                fill (io, pos);
                io.copyFrom (1, 0, io, 0, 0, 512);
                juce::AudioBuffer<float> in;
                in.makeCopyOf (io);
                p->processBlock (io, midi);
                if (pos > 48000 * 2)
                    for (int i = 0; i < 512; ++i)
                        differing += juce::exactlyEqual (io.getSample (0, i), in.getSample (0, i)) ? 0 : 1;
            }
            check (differing == 0, "mono, Dialogue on, the key the mix itself: nothing is cut once settled (" + juce::String (differing) + " samples differ from the input)");
        }
    }

    // A state saved by the first test build: Link was a percentage, and the parameters added
    // since are not in it.
    std::cout << "[ RUN  ] state from the first test build" << std::endl;
    {
        const auto valueOf = [] (juce::AudioProcessor& p, const char* id)
        {
            for (auto* prm : p.getParameters())
                if (auto* w = dynamic_cast<juce::RangedAudioParameter*> (prm))
                    if (w->paramID == id)
                        return w->convertFrom0to1 (w->getValue());
            return -1.0f;
        };
        for (float link : { 100.0f, 50.0f, 49.0f, 0.0f })
        {
            std::unique_ptr<juce::AudioProcessor> p (createPluginFilter());
            // Anything but the defaults first, so that what is restored is seen to be restored.
            setParam (*p, "linkMode", 0.5f);
            setParam (*p, "dialogue", 1.0f);
            setParam (*p, "fogTimingLocked", 0.0f);
            setParam (*p, "dialogueAmount", 0.2f);
            setParam (*p, "detection", 1.0f);
            juce::XmlElement old ("DeHonk");
            old.setAttribute ("stateVersion", 1);
            const auto add = [&old] (const char* id, double v)
            {
                auto* c = old.createNewChildElement ("PARAM");
                c->setAttribute ("id", id);
                c->setAttribute ("value", v);
            };
            add ("threshold", -22.0);
            add ("link", (double) link);
            add ("mix", 80.0);
            juce::MemoryBlock data;
            juce::AudioProcessor::copyXmlToBinary (old, data);
            p->setStateInformation (data.getData(), (int) data.getSize());
            const int mode = juce::roundToInt (valueOf (*p, "linkMode"));
            const bool ok = mode == (link >= 50.0f ? 0 : 2) && valueOf (*p, "dialogue") == 0.0f && valueOf (*p, "fogTimingLocked") == 1.0f
                         && valueOf (*p, "dialogueAmount") == 100.0f && valueOf (*p, "detection") == 0.0f && std::abs (valueOf (*p, "threshold") + 22.0f) < 0.01f && std::abs (valueOf (*p, "mix") - 80.0f) < 0.01f;
            check (ok, "Link " + juce::String (link, 0) + " % is read as " + (mode == 0 ? "Linked" : mode == 1 ? "Grouped" : "Unlinked")
                           + "; Threshold and Mix restored; what the state does not have (Dialogue, its Amount, the ranges' timing, Detection) at its default");

            // And what is saved now comes back as it was.
            setParam (*p, "linkMode", 0.5f);
            setParam (*p, "dialogue", 1.0f);
            setParam (*p, "tubTimingLocked", 0.0f);
            setParam (*p, "detection", 1.0f);
            juce::MemoryBlock now;
            p->getStateInformation (now);
            std::unique_ptr<juce::AudioProcessor> q (createPluginFilter());
            q->setStateInformation (now.getData(), (int) now.getSize());
            check (juce::roundToInt (valueOf (*q, "linkMode")) == 1 && valueOf (*q, "dialogue") == 1.0f && valueOf (*q, "tubTimingLocked") == 0.0f
                       && valueOf (*q, "detection") == 1.0f,
                   "saved with Link Grouped, Dialogue on, Tub's timing unlocked and Detection Steady: restored the same in a new instance");
        }
        // States the first test build really saved (Tests/host/FirstBuildStates.h): one on each
        // of its factory presets, one on its "User preset" entry, one with settings moved.
        {
            struct Expect { const char* program; float threshold, depth, sharpness, release, maxCut, mix; int linkMode, lookahead; bool woof; };
            // In the order of FirstBuildStates.h. "After RBass" has a new name since.
            const Expect expected[] = {
                { "Master bus",              -16.0f, 50.0f,  20.0f, 400.0f, 6.0f, 100.0f, 0, 0, false },
                { "After a bass enhancer",   -24.0f, 50.0f,  20.0f, 400.0f, 6.0f, 100.0f, 0, 0, false },
                { "Printed stem",            -30.0f, 60.0f,  20.0f, 400.0f, 6.0f, 100.0f, 0, 2, false },
                { "Narrow resonances only",  -16.0f, 50.0f, 100.0f, 700.0f, 6.0f, 100.0f, 0, 0, false },
                { "Heavy",                   -16.0f, 80.0f,  20.0f, 400.0f, 9.0f, 100.0f, 0, 0, true },
                { "Master bus",              -16.0f, 50.0f,  20.0f, 400.0f, 6.0f, 100.0f, 0, 0, false },
                { "Master bus",              -22.0f, 50.0f,  20.0f, 400.0f, 6.0f,  80.0f, 2, 0, false },
            };
            const int numStates = (int) (sizeof (firstBuild::states) / sizeof (firstBuild::states[0]));
            check (numStates == (int) (sizeof (expected) / sizeof (expected[0])), "seven states saved by the first test build are kept with the check");
            for (int i = 0; i < juce::jmin (numStates, (int) (sizeof (expected) / sizeof (expected[0]))); ++i)
            {
                const auto& e = expected[i];
                juce::MemoryBlock data;
                const bool decoded = data.fromBase64Encoding (firstBuild::states[i].base64);
                std::unique_ptr<juce::AudioProcessor> p (createPluginFilter());
                setParam (*p, "linkMode", 0.5f);
                setParam (*p, "dialogue", 1.0f);
                setParam (*p, "barkTimingLocked", 0.0f);
                setParam (*p, "link", 0.2f);
                setParam (*p, "detection", 1.0f);
                p->setStateInformation (data.getData(), (int) data.getSize());
                const auto near = [&] (const char* id, float want) { return std::abs (valueOf (*p, id) - want) < 0.01f; };
                const juce::String program = p->getProgramName (p->getCurrentProgram());
                const bool values = near ("threshold", e.threshold) && near ("depth", e.depth) && near ("sharpness", e.sharpness) && near ("release", e.release)
                                 && near ("maxCut", e.maxCut) && near ("mix", e.mix) && juce::roundToInt (valueOf (*p, "linkMode")) == e.linkMode
                                 && juce::roundToInt (valueOf (*p, "lookahead")) == e.lookahead && (valueOf (*p, "woofOn") >= 0.5f) == e.woof;
                const bool added = valueOf (*p, "dialogue") == 0.0f && valueOf (*p, "dialogueAmount") == 100.0f && valueOf (*p, "barkTimingLocked") == 1.0f
                                && near ("barkAttack", 14.0f) && near ("barkRelease", 400.0f) && near ("link", 100.0f)
                                && valueOf (*p, "detection") == 0.0f;
                const bool named = e.program == nullptr ? program == "User preset" : program == e.program;
                check (decoded && values && added && named,
                       "a state the first test build saved (" + juce::String (firstBuild::states[i].what) + "): its values come back, Link as "
                           + (e.linkMode == 0 ? "Linked" : "Unlinked") + ", what it does not have at its default, and the host is shown \"" + program + "\"");
            }
        }
        // A state saved by an early 0.0.2 (stateVersion 2, Link as a mode, Dialogue there) from
        // before Detection existed, restored over an instance at Steady: it comes back at Fast.
        {
            std::unique_ptr<juce::AudioProcessor> p (createPluginFilter());
            setParam (*p, "detection", 1.0f);
            setParam (*p, "dialogue", 0.0f);
            juce::XmlElement early ("DeHonk");
            early.setAttribute ("stateVersion", 2);
            const auto add = [&early] (const char* id, double v)
            {
                auto* c = early.createNewChildElement ("PARAM");
                c->setAttribute ("id", id);
                c->setAttribute ("value", v);
            };
            add ("threshold", -20.0);
            add ("linkMode", 2.0);
            add ("dialogue", 1.0);
            add ("dialogueAmount", 60.0);
            juce::MemoryBlock data;
            juce::AudioProcessor::copyXmlToBinary (early, data);
            p->setStateInformation (data.getData(), (int) data.getSize());
            check (valueOf (*p, "detection") == 0.0f && juce::roundToInt (valueOf (*p, "linkMode")) == 2 && valueOf (*p, "dialogue") == 1.0f
                       && std::abs (valueOf (*p, "dialogueAmount") - 60.0f) < 0.01f && std::abs (valueOf (*p, "threshold") + 20.0f) < 0.01f,
                   "an early 0.0.2 state without Detection, restored over Steady: Detection back at Fast, Link Unlinked, Dialogue on at 60 %, Threshold -20");
        }
        // A user preset file saved before Detection existed, loaded while at Steady: Fast. The file
        // is written to a temporary folder, never the user's preset folder.
        {
            std::unique_ptr<juce::AudioProcessor> p (createPluginFilter());
            auto* dh = dynamic_cast<DeHonkAudioProcessor*> (p.get());
            setParam (*p, "detection", 1.0f);
            const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("dehonk-hostcheck", "");
            dir.createDirectory();
            const auto file = dir.getChildFile ("Before detection.dehonkpreset");
            file.replaceWithText ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                                  "<DeHonkPreset name=\"Before detection\" pluginVersion=\"0.0.2\">\n"
                                  "  <Param id=\"threshold\" value=\"-18\"/>\n"
                                  "  <Param id=\"linkMode\" value=\"0\"/>\n"
                                  "  <Param id=\"dialogue\" value=\"0\"/>\n"
                                  "</DeHonkPreset>\n");
            const bool loaded = dh != nullptr && dh->getPresetManager().loadPresetFile (file);
            check (loaded && valueOf (*p, "detection") == 0.0f && std::abs (valueOf (*p, "threshold") + 18.0f) < 0.01f,
                   "a user preset file from before Detection, loaded while at Steady: Detection back at Fast, Threshold -18");
            dir.deleteRecursively();
        }
        // The first test build's parameters keep their places in the order an AU host numbers them
        // by: all of them, "link" included, carry version hint 1 and everything added carries a higher one.
        {
            std::unique_ptr<juce::AudioProcessor> p (createPluginFilter());
            const juce::StringArray first { "threshold", "depth", "sharpness", "speed", "woofOn", "tubOn", "fogOn", "barkOn", "snapOn", "edge0", "edge1", "edge2",
                                            "edge3", "edge4", "link", "lookahead", "mix", "trim", "bypass", "maxCut", "release", "impactGuard" };
            int firstSeen = 0, wrongHints = 0;
            bool linkAutomatable = true;
            for (auto* q : p->getParameters())
                if (auto* w = dynamic_cast<juce::RangedAudioParameter*> (q))
                {
                    const bool isFirst = first.contains (w->getParameterID());
                    firstSeen += isFirst ? 1 : 0;
                    wrongHints += (isFirst ? w->getVersionHint() == 1 : w->getVersionHint() > 1) ? 0 : 1;
                    if (w->getParameterID() == "link")
                        linkAutomatable = w->isAutomatable();
                }
            check (firstSeen == first.size() && wrongHints == 0 && ! linkAutomatable,
                   "all 22 parameters of the first test build are still there with version hint 1 (the old Link not automatable), and the "
                       + juce::String (p->getParameters().size() - first.size()) + " added since have a higher one");
        }
        // Rubbish does not crash it.
        std::unique_ptr<juce::AudioProcessor> p (createPluginFilter());
        const char junk[] = "not a state";
        p->setStateInformation (junk, (int) sizeof (junk));
        p->setStateInformation (nullptr, 0);
        check (true, "a state that is not one, and an empty one: no crash");
    }

    std::cout << (failures == 0 ? "all host checks passed" : juce::String (failures) + " host checks failed") << std::endl;
    return failures == 0 ? 0 : 1;
}
