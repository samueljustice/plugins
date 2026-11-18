// A small console host for DOOM EQ's processor. It runs the real plug-in code offline and checks
// what's easy to get wrong and hard to hear in a quick test: a flat EQ is transparent, type
// changes and bypass don't click, blocks bigger than the prepared size work (and give the same
// result), mono-in/stereo-out, state and preset recall, host programs, and no NaNs at extreme
// settings.
//
// Built from the plug-in's CMake project, not by default:
//   cmake --build toys/doomeq/build --config Release --target DoomEQHostCheck
// then run toys/doomeq/build/DoomEQHostCheck_artefacts/DoomEQHostCheck. Exit code 1 if any check failed.
#include <JuceHeader.h>

#include "PluginProcessor.h"

#include <cmath>
#include <iostream>
#include <memory>
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

std::unique_ptr<DoomEQProcessor> make (const S& in = S::stereo(), const S& out = S::stereo(), double sr = 48000.0, int block = 512)
{
    std::unique_ptr<DoomEQProcessor> p (dynamic_cast<DoomEQProcessor*> (createPluginFilter()));
    juce::AudioProcessor::BusesLayout layout;
    layout.inputBuses.add (in);
    layout.outputBuses.add (out);
    p->setBusesLayout (layout);
    p->setRateAndBufferSizeDetails (sr, block);
    p->prepareToPlay (sr, block);
    return p;
}

void set (DoomEQProcessor& p, const juce::String& id, float value)
{
    auto* prm = p.apvts.getParameter (id);
    jassert (prm != nullptr);
    prm->setValueNotifyingHost (prm->convertTo0to1 (value));
}

float get (DoomEQProcessor& p, const juce::String& id)
{
    return p.apvts.getRawParameterValue (id)->load();
}

/** Runs `input` (one channel, copied to every input channel) through p in blocks of `block`. */
juce::AudioBuffer<float> run (DoomEQProcessor& p, const std::vector<float>& input, int block,
                              const std::function<void (int sampleIndex)>& beforeBlock = {})
{
    const int numOut = p.getTotalNumOutputChannels(), numIn = p.getTotalNumInputChannels();
    const int chans = juce::jmax (numIn, numOut);
    juce::AudioBuffer<float> out (numOut, (int) input.size());
    juce::MidiBuffer midi;

    for (int start = 0; start < (int) input.size(); start += block)
    {
        const int n = juce::jmin (block, (int) input.size() - start);
        if (beforeBlock)
            beforeBlock (start);

        juce::AudioBuffer<float> b (chans, n);
        b.clear();
        for (int ch = 0; ch < numIn; ++ch)
            b.copyFrom (ch, 0, input.data() + start, n);
        p.processBlock (b, midi);
        for (int ch = 0; ch < numOut; ++ch)
            out.copyFrom (ch, start, b, ch, 0, n);
    }
    return out;
}

std::vector<float> sine (double freq, double amp, int n, double sr = 48000.0)
{
    std::vector<float> s ((size_t) n);
    for (int i = 0; i < n; ++i)
        s[(size_t) i] = (float) (amp * std::sin (juce::MathConstants<double>::twoPi * freq * i / sr));
    return s;
}

std::vector<float> noise (int n, int seed = 7)
{
    juce::Random r (seed);
    std::vector<float> s ((size_t) n);
    for (auto& v : s)
        v = r.nextFloat() * 0.5f - 0.25f;
    return s;
}

bool allFinite (const juce::AudioBuffer<float>& b)
{
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
        for (int i = 0; i < b.getNumSamples(); ++i)
            if (! std::isfinite (b.getSample (ch, i)))
                return false;
    return true;
}

/** Largest |y[n] - 2y[n-1] + y[n-2]| in [from, to): a click shows up as a spike here. */
float maxCurvature (const juce::AudioBuffer<float>& b, int ch, int from, int to)
{
    float worst = 0.0f;
    for (int i = juce::jmax (2, from); i < juce::jmin (to, b.getNumSamples()); ++i)
        worst = juce::jmax (worst, std::abs (b.getSample (ch, i) - 2.0f * b.getSample (ch, i - 1) + b.getSample (ch, i - 2)));
    return worst;
}
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;
    constexpr int sr = 48000;

    std::cout << "Layouts" << std::endl;
    {
        auto p = make();
        auto ok = [&p] (const S& in, const S& out)
        {
            juce::AudioProcessor::BusesLayout l;
            l.inputBuses.add (in);
            l.outputBuses.add (out);
            return p->checkBusesLayoutSupported (l);
        };
        check (ok (S::stereo(), S::stereo()), "stereo -> stereo");
        check (ok (S::mono(), S::mono()), "mono -> mono");
        check (ok (S::mono(), S::stereo()), "mono -> stereo");
        check (! ok (S::stereo(), S::mono()), "stereo -> mono refused");
        check (std::abs (p->getTailLengthSeconds() - 1.0) < 1e-9, "tail length 1 s");
    }

    std::cout << "Transparency" << std::endl;
    {
        auto p = make();
        const auto in = noise (sr);
        const auto out = run (*p, in, 512);
        float worst = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < sr; ++i)
                worst = juce::jmax (worst, std::abs (out.getSample (ch, i) - in[(size_t) i]));
        check (worst < 1.0e-4f, "default (flat) settings pass audio unchanged (max diff " + juce::String (worst, 7) + ")");
    }

    std::cout << "Clicks" << std::endl;
    {
        // A 60 Hz tone under a +12 dB low shelf; then the band becomes a low cut mid-tone.
        auto measure = [] (std::function<void (DoomEQProcessor&)> setup, std::function<void (DoomEQProcessor&)> change)
        {
            auto p = make();
            setup (*p);
            const auto in = sine (60.0, 0.25, sr);
            const int at = sr / 2;
            const auto out = run (*p, in, 256, [&] (int start) { if (start == at) change (*p); });
            const float baseline = maxCurvature (out, 0, at - 4000, at - 100);
            const float around = maxCurvature (out, 0, at - 100, at + 4000);
            return std::make_pair (baseline, around);
        };

        auto boost = [] (DoomEQProcessor& p) { set (p, "b1_type", 1.0f); set (p, "b1_freq", 80.0f); set (p, "b1_gain", 12.0f); };

        auto [b1, a1] = measure (boost, [] (DoomEQProcessor& p) { set (p, "b1_type", 3.0f); }); // Low Shelf -> Low Cut 12
        check (a1 < juce::jmax (0.002f, b1 * 20.0f), "type change (shelf -> cut) doesn't click (curvature " + juce::String (a1, 5) + " vs " + juce::String (b1, 5) + ")");

        auto [b2, a2] = measure (boost, [] (DoomEQProcessor& p) { set (p, "eq_bypass", 1.0f); });
        check (a2 < juce::jmax (0.002f, b2 * 20.0f), "EQ bypass fades instead of clicking (curvature " + juce::String (a2, 5) + ")");

        auto [b3, a3] = measure (boost, [] (DoomEQProcessor& p) { set (p, "b1_on", 0.0f); });
        check (a3 < juce::jmax (0.002f, b3 * 20.0f), "band off fades instead of clicking (curvature " + juce::String (a3, 5) + ")");

        auto [b4, a4] = measure ([] (DoomEQProcessor& p) { set (p, "b1_type", 3.0f); set (p, "b1_freq", 30.0f); },
                                 [] (DoomEQProcessor& p) { set (p, "b1_type", 5.0f); }); // 12 -> 48 dB/oct
        check (a4 < juce::jmax (0.002f, b4 * 20.0f), "cut slope change doesn't click (curvature " + juce::String (a4, 5) + ")");
    }

    std::cout << "Host bypass" << std::endl;
    {
        auto p = make();
        set (*p, "b3_gain", 12.0f);
        set (*p, "bypass", 1.0f);
        const auto in = noise (sr / 2);
        const auto out = run (*p, in, 512);
        float worst = 0.0f;
        for (int i = sr / 10; i < sr / 2; ++i) // after the fade
            worst = juce::jmax (worst, std::abs (out.getSample (0, i) - in[(size_t) i]));
        check (worst < 1.0e-6f, "host bypass passes the input through (max diff " + juce::String (worst, 8) + ")");
    }

    std::cout << "Block sizes" << std::endl;
    {
        auto setup = [] (DoomEQProcessor& p)
        {
            set (p, "b2_gain", 6.0f); set (p, "b2_q", 3.0f);
            set (p, "b5_type", 7.0f); set (p, "b5_freq", 9000.0f);
        };
        auto small = make (S::stereo(), S::stereo(), sr, 512);
        auto large = make (S::stereo(), S::stereo(), sr, 512);
        setup (*small);
        setup (*large);
        const auto in = noise (sr);
        const auto a = run (*small, in, 512);
        const auto b = run (*large, in, 8192); // bigger than prepared: split inside, nothing resized
        float worst = 0.0f;
        for (int i = 0; i < sr; ++i)
            worst = juce::jmax (worst, std::abs (a.getSample (0, i) - b.getSample (0, i)));
        check (allFinite (b), "8192-sample blocks after preparing for 512: finite");
        check (worst < 1.0e-5f, "8192-sample blocks give the same result as 512 (max diff " + juce::String (worst, 8) + ")");
    }

    std::cout << "Mono in, stereo out" << std::endl;
    {
        auto p = make (S::mono(), S::stereo());
        check (p->getTotalNumInputChannels() == 1 && p->getTotalNumOutputChannels() == 2, "layout applied");
        const auto in = noise (sr / 4);
        const auto out = run (*p, in, 512);
        float worst = 0.0f;
        for (int i = 0; i < sr / 4; ++i)
            worst = juce::jmax (worst, std::abs (out.getSample (1, i) - in[(size_t) i]));
        check (worst < 1.0e-4f, "right channel carries the mono input (max diff " + juce::String (worst, 7) + ")");
    }

    std::cout << "Extremes" << std::endl;
    for (double rate : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        auto p = make (S::stereo(), S::stereo(), rate, 256);
        set (*p, "b1_type", 0.0f); set (*p, "b1_freq", 20.0f); set (*p, "b1_gain", 18.0f); set (*p, "b1_q", 10.0f);
        set (*p, "b2_type", 8.0f); set (*p, "b2_freq", 20000.0f); set (*p, "b2_q", 10.0f);
        set (*p, "b3_type", 2.0f); set (*p, "b3_freq", 20000.0f); set (*p, "b3_gain", -18.0f); set (*p, "b3_q", 10.0f);
        set (*p, "b4_type", 5.0f); set (*p, "b4_freq", 20.0f); set (*p, "b4_q", 0.1f);
        set (*p, "b5_type", 0.0f); set (*p, "b5_freq", 19000.0f); set (*p, "b5_gain", 18.0f); set (*p, "b5_q", 0.1f);
        const auto out = run (*p, noise ((int) rate / 2), 256, [&] (int start)
        {
            if (start % 4096 == 0) // sweep while running
                set (*p, "b5_freq", (start / 4096) % 2 == 0 ? 20.0f : 20000.0f);
        });
        check (allFinite (out), "extreme settings with sweeps at " + juce::String ((int) rate) + " Hz stay finite");
    }

    std::cout << "Presets" << std::endl;
    {
        auto p = make();
        auto& pm = p->getPresetManager();
        check (p->getNumPrograms() == PresetManager::getNumFactory() + 1, "host programs = factory presets + User Preset");
        check (p->getProgramName (p->getNumPrograms() - 1) == "User Preset", "last program is \"User Preset\"");

        set (*p, "doom_sound", 1.0f);
        set (*p, "doom_level", -20.0f);
        bool allLoaded = true, rangesOk = true;
        for (int i = 0; i < PresetManager::getNumFactory(); ++i)
        {
            allLoaded = allLoaded && pm.loadPreset (i);
            const auto out = run (*p, noise (4096, i), 512);
            rangesOk = rangesOk && allFinite (out);
        }
        check (allLoaded, juce::String (PresetManager::getNumFactory()) + " factory presets load");
        check (rangesOk, "every factory preset processes finite audio");
        check (get (*p, "doom_sound") > 0.5f && std::abs (get (*p, "doom_level") + 20.0f) < 0.01f,
               "loading presets leaves DOOM SOUND and its level alone");

        p->setCurrentProgram (3);
        check (p->getCurrentProgram() == 3, "setCurrentProgram (3) -> getCurrentProgram () == 3");
    }

    std::cout << "State" << std::endl;
    {
        auto a = make();
        a->getPresetManager().loadPreset (4);
        set (*a, "b2_gain", -7.5f);
        a->setOverlay (35.0f);
        a->setEditorSize (1111, 777);
        juce::MemoryBlock state;
        a->getStateInformation (state);

        auto b = make();
        set (*b, "eq_bypass", 1.0f);
        b->setStateInformation (state.getData(), (int) state.getSize());

        bool same = true;
        for (auto* prm : a->getParameters())
            if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (prm))
                same = same && std::abs (get (*a, r->paramID) - get (*b, r->paramID)) < 1.0e-4f;
        check (same, "every parameter recalls");
        check (b->getPresetManager().getCurrentName() == a->getPresetManager().getCurrentName()
               && b->getPresetManager().isModified(), "preset name and edited flag recall (" + b->getPresetManager().getCurrentName() + " *)");
        check (std::abs (b->getOverlay() - 35.0f) < 0.01f && b->getEditorSize() == juce::Point<int> (1111, 777), "overlay and window size recall");

        // A 0.0.1 session has no bypass parameters: loading it must not keep this instance's.
        auto xml = juce::AudioProcessor::getXmlFromBinary (state.getData(), (int) state.getSize());
        for (auto* child = xml->getFirstChildElement(); child != nullptr;)
        {
            auto* next = child->getNextElement();
            const auto id = child->getStringAttribute ("id");
            if (id == "bypass" || id == "eq_bypass")
                xml->removeChildElement (child, true);
            child = next;
        }
        juce::MemoryBlock oldState;
        juce::AudioProcessor::copyXmlToBinary (*xml, oldState);
        auto c = make();
        set (*c, "eq_bypass", 1.0f);
        c->setStateInformation (oldState.getData(), (int) oldState.getSize());
        check (get (*c, "eq_bypass") < 0.5f, "a session without bypass loads with the EQ in, whatever the instance had");
    }

    std::cout << (failures == 0 ? "All checks passed" : juce::String (failures) + " check(s) failed") << std::endl;
    return failures == 0 ? 0 : 1;
}
