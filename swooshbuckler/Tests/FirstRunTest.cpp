// First-run test: the real plug-in (no window) and the pieces behind the first-run features.
//
//   - Demo sounds: each of the five is typed correctly from its audio alone (no file name to help), has the
//     regions it should, is the same every time, and the bed and the rumble loop without a step. Load Demo Sounds
//     fills five tracks on the right buses as ONE undo step, and Play then makes sound. The rumble plays on bus A
//     while Stowaway is off, and is Stowaway's sub (low end only) from the moment it is switched on.
//   - Typed values: for every knob and fader, typing a readout back gives a position that reads the same; the
//     units ("-12 dB", "4.2 s", "35%", a bare number, ms against s), the words ("Rate", "Off") and the ends.
//   - MIDI control: Learn takes the next controller, the fader follows it (playing or stopped) without making undo
//     steps, the mapping is kept in the settings file and read by the next rig, and Forget removes it.
//   - File names cut in the middle keep their start and their end.
//
// Usage: SwooshbucklerFirstRun [scratch folder] (a folder in the temp directory if none is given). Nothing is read
// from or written to the user's settings.

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "Parameters.h"
#include "DemoSounds.h"
#include "SampleClassifier.h"
#include "Theme.h"
#include "TypedValue.h"

#include <cmath>
#include <iostream>

namespace
{
    constexpr double c_fs = 48000.0;
    int g_failures = 0;

    bool same(float a, float b) { return std::abs(a - b) < 1.0e-6f; }

    void check(bool ok, const juce::String& what)
    {
        std::cout << (ok ? "  [ok]   " : "  [FAIL] ") << what << std::endl;
        if (! ok)
            ++g_failures;
    }

    using Proc = SwooshbucklerAudioProcessor;

    struct Rig
    {
        std::unique_ptr<juce::AudioProcessor> base { createPluginFilter() };
        Proc& proc { *dynamic_cast<Proc*>(base.get()) };
        juce::AudioBuffer<float> block { 2, 512 };
        Rig()
        {
            proc.setPlayConfigDetails(0, 2, c_fs, 512);
            proc.prepareToPlay(c_fs, 512);
        }
        juce::RangedAudioParameter* param(const juce::String& id) const { return proc.getState().getParameter(id); }
        /** One block, with these MIDI messages at its start. Returns the block's peak. */
        float run(std::initializer_list<juce::MidiMessage> messages = {})
        {
            juce::MidiBuffer midi;
            for (const auto& m : messages)
                midi.addEvent(m, 0);
            block.clear();
            proc.processBlock(block, midi);
            return block.getMagnitude(0, block.getNumSamples());
        }
        float runSeconds(double seconds)
        {
            float peak = 0.0f;
            for (int i = 0; i < static_cast<int>(seconds * c_fs / 512.0); ++i)
                peak = juce::jmax(peak, run());
            return peak;
        }
        int tracksLoaded() const
        {
            int n = 0;
            for (int t = 0; t < CargoHold::c_numTracks; ++t)
                n += proc.getCargo().get(t) != nullptr ? 1 : 0;
            return n;
        }
    };

    /** The readout of a parameter at a position, as the typing box sees it. */
    std::function<juce::String(float)> readout(juce::RangedAudioParameter* p)
    {
        return [p](float n) { return p->getText(n, 0); };
    }

    /** Types `text` into a parameter that sits at `current`, and returns what it then reads. */
    juce::String typed(juce::RangedAudioParameter* p, const juce::String& text, float current = 0.5f)
    {
        const float n = TypedValue::toNormalised(readout(p), text, current);
        return n < 0.0f ? juce::String("(not understood)") : p->getText(n, 0);
    }
}

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init;
    const juce::File folder = argc > 1 ? juce::File(argv[1]) : juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("SwooshbucklerFirstRunTest");
    folder.createDirectory();

    juce::PropertiesFile::Options options;
    options.applicationName = "SwooshbucklerFirstRunTest";
    options.filenameSuffix = ".settings";
    const auto settingsFile = folder.getChildFile("firstrun.settings");
    settingsFile.deleteFile();
    juce::PropertiesFile settings(settingsFile, options);
    Proc::useSettings(&settings);

    // Demo sounds: the audio itself ------------------------------------------------------------------------------
    std::cout << "Demo sounds" << std::endl;
    {
        const auto started = juce::Time::getMillisecondCounterHiRes();
        const auto sounds = DemoSounds::all();
        const double madeIn = juce::Time::getMillisecondCounterHiRes() - started;
        check(static_cast<int>(sounds.size()) == DemoSounds::c_count, "demo: five sounds");
        std::cout << "         made in " << juce::String(madeIn, 1) << " ms" << std::endl;
        check(madeIn < 1500.0, "demo: making the five takes well under a second and a half (" + juce::String(madeIn, 0) + " ms)");

        struct Expect { CargoRole role; int regions; double minSeconds; int channels; };
        const Expect expected[] = { { CargoRole::Tonal, 5, 6.0, 1 }, { CargoRole::Bed, 1, 7.5, 2 }, { CargoRole::Air, 6, 5.0, 1 }, { CargoRole::Impact, 4, 4.0, 1 },
                                    { CargoRole::Rumble, 1, 7.5, 1 } };
        for (size_t i = 0; i < sounds.size() && i < 5; ++i)
        {
            const auto& sound = sounds[i];
            const auto& want = expected[i];
            const int length = sound.audio.getNumSamples();
            // What the classifier hears: the mono sum, with no file name to lean on.
            juce::AudioBuffer<float> mono(1, length);
            mono.copyFrom(0, 0, sound.audio, 0, 0, length);
            if (sound.audio.getNumChannels() > 1)
            {
                mono.addFrom(0, 0, sound.audio, 1, 0, length);
                mono.applyGain(0.5f);
            }
            auto regions = CargoData::detectRegions(mono.getReadPointer(0), length, c_fs);
            const auto analysis = SampleClassifier::analyse(mono.getReadPointer(0), length, c_fs, regions, {});
            std::cout << "         " << sound.name << ": " << juce::String(length / c_fs, 2) << " s, " << regions.size() << " regions, heard as "
                      << SampleClassifier::roleName(analysis.role) << " (confidence " << juce::String(analysis.confidence, 2) << "), peak "
                      << juce::String(sound.audio.getMagnitude(0, length), 2) << std::endl
                      << "           low " << juce::String(analysis.lowRatio, 2) << "  centroid " << juce::String(analysis.centroidHz, 0) << " Hz  flatness "
                      << juce::String(analysis.flatness, 3) << "  pitch clarity " << juce::String(analysis.pitchClarity, 2) << "  attack "
                      << juce::String(analysis.attackMs, 1) << " ms  crest " << juce::String(analysis.crest, 1) << " dB  onsets/s "
                      << juce::String(analysis.onsetRate, 1) << "  sustain " << juce::String(analysis.sustainRatio, 2) << std::endl;
            check(analysis.role == want.role, "demo: " + sound.name + " is heard as " + SampleClassifier::roleName(want.role) + " from its audio alone");
            check(static_cast<int>(regions.size()) == want.regions, "demo: " + sound.name + " has " + juce::String(want.regions) + " sound region(s)");
            check(length / c_fs >= want.minSeconds && sound.audio.getNumChannels() == want.channels,
                  "demo: " + sound.name + " is several seconds long, in " + juce::String(want.channels) + " channel(s)");
            const float peak = sound.audio.getMagnitude(0, length);
            check(peak > 0.5f && peak <= 1.0f, "demo: " + sound.name + " peaks under full scale");
        }

        // The same samples every time.
        const auto again = DemoSounds::all();
        bool identical = true;
        for (size_t i = 0; i < sounds.size(); ++i)
            for (int ch = 0; ch < sounds[i].audio.getNumChannels(); ++ch)
                identical = identical && std::equal(sounds[i].audio.getReadPointer(ch), sounds[i].audio.getReadPointer(ch) + sounds[i].audio.getNumSamples(),
                                          again[i].audio.getReadPointer(ch));
        check(identical, "demo: the sounds are the same every time they are made");

        // The bed loops: the step from its last sample to its first is no bigger than its steps anywhere else.
        const auto& bed = sounds[1].audio;
        const float* l = bed.getReadPointer(0);
        const int n = bed.getNumSamples();
        float largest = 0.0f;
        for (int i = 1; i < n; ++i)
            largest = juce::jmax(largest, std::abs(l[i] - l[i - 1]));
        check(std::abs(l[0] - l[n - 1]) <= largest, "demo: the bed's end runs into its start (seam step "
                                                    + juce::String(std::abs(l[0] - l[n - 1]), 4) + ", largest step inside " + juce::String(largest, 4) + ")");
        // So does the rumble, and nearly all of it is low: what is above 500 Hz is more than 20 dB under the whole.
        {
            const auto& rumble = sounds[4].audio;
            const float* x = rumble.getReadPointer(0);
            const int len = rumble.getNumSamples();
            float biggest = 0.0f;
            for (int i = 1; i < len; ++i)
                biggest = juce::jmax(biggest, std::abs(x[i] - x[i - 1]));
            check(std::abs(x[0] - x[len - 1]) <= biggest, "demo: the rumble's end runs into its start (seam step "
                                                          + juce::String(std::abs(x[0] - x[len - 1]), 4) + ", largest step inside " + juce::String(biggest, 4) + ")");
            // Two one-pole high-passes at 500 Hz in a row (12 dB an octave).
            const float a = static_cast<float>(1.0 - std::exp(-2.0 * juce::MathConstants<double>::pi * 500.0 / c_fs));
            float z1 = 0.0f, z2 = 0.0f;
            double all = 0.0, high = 0.0;
            for (int i = 0; i < len; ++i)
            {
                z1 += a * (x[i] - z1);
                const float h1 = x[i] - z1;
                z2 += a * (h1 - z2);
                const float h2 = h1 - z2;
                all += static_cast<double>(x[i]) * x[i];
                high += static_cast<double>(h2) * h2;
            }
            const double highDb = 10.0 * std::log10(juce::jmax(1.0e-12, high) / all);
            check(highDb < -20.0, "demo: the rumble is low end (above 500 Hz it is " + juce::String(highDb, 1) + " dB under the whole)");
        }

        // It is wide: the two sides differ, and the mono sum keeps most of the level.
        double sumL = 0.0, sumR = 0.0, sumM = 0.0, sumS = 0.0;
        const float* r = bed.getReadPointer(1);
        for (int i = 0; i < n; ++i)
        {
            sumL += l[i] * l[i];
            sumR += r[i] * r[i];
            sumM += 0.25 * (l[i] + r[i]) * (l[i] + r[i]);
            sumS += 0.25 * (l[i] - r[i]) * (l[i] - r[i]);
        }
        const double sideDb = 10.0 * std::log10(sumS / sumM);
        check(sideDb > -12.0 && sideDb < -3.0 && std::abs(10.0 * std::log10(sumL / sumR)) < 1.5,
              "demo: the bed is wide but holds in mono (side " + juce::String(sideDb, 1) + " dB against the middle) and balanced");
    }

    // Load Demo Sounds through the rig -------------------------------------------------------------------------
    {
        Rig rig;
        auto& hold = rig.proc.getCargo();
        check(rig.tracksLoaded() == 0 && ! rig.proc.getHistory().canUndo(), "load: the rig starts empty");
        check(rig.proc.loadDemoSounds() == 5 && rig.tracksLoaded() == 5, "load: Load Demo Sounds fills five tracks");
        check(hold.getRole(0) == CargoRole::Tonal && hold.getRole(1) == CargoRole::Bed && hold.getRole(2) == CargoRole::Air && hold.getRole(3) == CargoRole::Impact,
              "load: the first four are typed Tonal Content, Energy Bed, Air / Swish and Impact");
        check(hold.getRole(4) == CargoRole::Rumble && hold.get(4) != nullptr && hold.get(4)->name == "Demo Rumble" && ! hold.get(4)->hasSide(),
              "load: the fifth is Demo Rumble, typed Rumble, in mono");
        check(hold.getBus(4) == 0 && ! rig.param(ParamIDs::stowOn)->getValue(),
              "load: with Stowaway off, as in a new session, the rumble plays on bus A");
        check(hold.getBus(0) == 0 && hold.getBus(1) == 1 && hold.getBus(2) == 1 && hold.getBus(3) == CargoHold::c_hitsBus,
              "load: the growl plays on bus A, the bed and the swishes on bus B, the hits on the hits");
        check(hold.get(1) != nullptr && hold.get(1)->hasSide() && hold.get(0) != nullptr && ! hold.get(0)->hasSide(), "load: the bed keeps its width, the growl is mono");
        check(hold.get(0)->name == "Demo Growl" && hold.get(3)->name == "Demo Hit", "load: each carries its name, as a file would");

        // Play makes a whoosh at once.
        rig.proc.setPlaying(true);
        const float peak = rig.runSeconds(4.0);
        rig.proc.setPlaying(false);
        check(peak > 0.05f, "load: Play then makes sound (peak " + juce::String(peak, 2) + ")");

        // One undo step for the five, and redo brings them back.
        juce::String what;
        check(rig.proc.undoEdit(&what) && rig.tracksLoaded() == 0, "load: one Undo takes all five out (\"" + what + "\")");
        check(! rig.proc.getHistory().canUndo(), "load: and nothing else is left to undo");
        check(rig.proc.redoEdit() && rig.tracksLoaded() == 5, "load: Redo brings all five back");

        // Stowaway has something to play: switched on (it starts as a Sub), it takes the rumble by itself and
        // sends its low end on; switched off, the rumble is back on bus A.
        rig.proc.setPlaying(true);
        rig.runSeconds(3.0);
        float offEnv = 0.0f;
        for (int i = 0; i < 100; ++i)
        {
            rig.run();
            offEnv = juce::jmax(offEnv, rig.proc.getEngine().getStowEnv());
        }
        rig.param(ParamIDs::stowOn)->setValueNotifyingHost(1.0f);
        rig.runSeconds(3.0);
        const int busOn = hold.getBus(4);
        float onEnv = 0.0f;
        for (int i = 0; i < 400; ++i)
        {
            rig.run();
            onEnv = juce::jmax(onEnv, rig.proc.getEngine().getStowEnv());
        }
        std::cout << "         Stowaway's level with the demo sounds: off " << juce::String(offEnv, 4) << ", on " << juce::String(onEnv, 4) << std::endl;
        check(busOn == CargoHold::c_stowBus && hold.getBus(0) == 0 && hold.getBus(1) == 1, "stowaway: switched on, it takes Demo Rumble and nothing else");
        check(offEnv < 1.0e-5f && onEnv > 0.02f, "stowaway: silent while off, and it has a sub to play as soon as it is on (level "
                                                 + juce::String(juce::Decibels::gainToDecibels(onEnv), 1) + " dB)");
        rig.param(ParamIDs::stowOn)->setValueNotifyingHost(0.0f);
        rig.runSeconds(1.0);
        check(hold.getBus(4) == 0, "stowaway: switched off again, the rumble is back on bus A");
        rig.proc.setPlaying(false);

        // With only two tracks free, two are loaded; with none, none.
        for (int t = 5; t < 8; ++t)
            hold.loadBuffer(t, DemoSounds::hit(), c_fs, "Filler");
        check(rig.proc.loadDemoSounds() == 2 && rig.tracksLoaded() == 10, "load: with two tracks free, two demo sounds go in");
        check(rig.proc.loadDemoSounds() == 0, "load: with every track in use, nothing is loaded and nothing is replaced");
    }

    // The rumble under the other four, with Stowaway off (it flies on bus A): it adds weight and does not take over.
    {
        auto measure = [](bool withRumble, double& rmsDb, double& lowDb)
        {
            Rig rig;
            rig.proc.loadDemoSounds();
            if (! withRumble)
                rig.proc.getCargo().clear(4);
            rig.proc.setPlaying(true);
            rig.runSeconds(2.0);
            double sum = 0.0, low = 0.0;
            float z = 0.0f;
            const float a = static_cast<float>(1.0 - std::exp(-2.0 * juce::MathConstants<double>::pi * 120.0 / c_fs));
            int count = 0;
            for (int i = 0; i < static_cast<int>(20.0 * c_fs / 512.0); ++i)
            {
                rig.run();
                const float* l = rig.block.getReadPointer(0);
                const float* r = rig.block.getReadPointer(1);
                for (int n = 0; n < 512; ++n)
                {
                    const float m = 0.5f * (l[n] + r[n]);
                    z += a * (m - z);
                    sum += 0.5 * (static_cast<double>(l[n]) * l[n] + static_cast<double>(r[n]) * r[n]);
                    low += static_cast<double>(z) * z;
                    ++count;
                }
            }
            rmsDb = 10.0 * std::log10(juce::jmax(1.0e-12, sum / count));
            lowDb = 10.0 * std::log10(juce::jmax(1.0e-12, low / count));
        };
        double rms4 = 0.0, low4 = 0.0, rms5 = 0.0, low5 = 0.0;
        measure(false, rms4, low4);
        measure(true, rms5, low5);
        std::cout << "         20 s of the demo sounds playing: without the rumble " << juce::String(rms4, 1) << " dB RMS (" << juce::String(low4, 1)
                  << " dB under 120 Hz), with it " << juce::String(rms5, 1) << " dB RMS (" << juce::String(low5, 1) << " dB under 120 Hz)" << std::endl;
        check(std::abs(rms5 - rms4) < 2.0 && low5 > low4 + 3.0,
              "load: the rumble adds low end (" + juce::String(low5 - low4, 1) + " dB under 120 Hz) and leaves the level about where it was ("
              + juce::String(rms5 - rms4, 1) + " dB)");
    }

    // Typed values ---------------------------------------------------------------------------------------------
    std::cout << "Typed values" << std::endl;
    {
        Rig rig;
        // Every knob and fader: a readout typed back lands on a position that reads the same.
        int floats = 0, wrong = 0;
        juce::String firstWrong;
        for (auto* p : rig.proc.getParameters())
        {
            if (dynamic_cast<juce::AudioParameterFloat*>(p) == nullptr)
                continue;
            auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(p);
            ++floats;
            for (const float n : { 0.0f, 0.02f, 0.1f, 0.25f, 0.4f, 0.5f, 0.61f, 0.73f, 0.9f, 0.98f, 1.0f })
            {
                const auto text = ranged->getText(n, 0);
                const auto back = typed(ranged, text, 0.5f);
                if (back != text)
                {
                    ++wrong;
                    if (firstWrong.isEmpty())
                        firstWrong = ranged->getName(40) + ": \"" + text + "\" came back as \"" + back + "\"";
                }
            }
        }
        check(floats > 40 && wrong == 0, "typed: every readout of all " + juce::String(floats) + " knobs and faders, typed back, reads the same"
                                          + (wrong > 0 ? " (" + juce::String(wrong) + " wrong, first: " + firstWrong + ")" : juce::String()));

        auto* energy = rig.param(ParamIDs::energy);
        auto* fader = rig.param(ParamIDs::cargoTrack[0]);
        auto* rate = rig.param(ParamIDs::rate);
        auto* release = rig.param(ParamIDs::hitRelease);
        auto* throwLength = rig.param(ParamIDs::throwLength);
        auto* duck = rig.param(ParamIDs::hitDuck);
        auto* push = rig.param(ParamIDs::push);
        auto* semis = rig.param(ParamIDs::trackSemis[0]);
        auto* pitch = rig.param(ParamIDs::pitch);

        const float n35 = TypedValue::toNormalised(readout(energy), "35%", 0.5f);
        check(std::abs(n35 - 0.35f) < 2.0e-4f, "typed: \"35%\" is exactly 0.35 (the middle of what reads 35%), got " + juce::String(n35, 5));
        check(typed(energy, "35") == "35%" && typed(energy, " 80 % ") == "80%", "typed: a bare number and stray spaces work (\"35\", \" 80 % \")");
        check(typed(fader, "-12 dB") == "-12.0 dB" && typed(fader, "-7.5") == "-7.5 dB", "typed: \"-12 dB\" and \"-7.5\" on a track fader");
        check(typed(fader, "-inf") == "-inf dB" && same(TypedValue::toNormalised(readout(fader), "-inf dB", 0.9f), 0.0f), "typed: \"-inf\" puts a fader at the bottom");
        check(typed(rate, "4.2 s") == "4.2 s", "typed: \"4.2 s\" on Rate (a readout that falls as the knob rises)");
        check(typed(release, "300", 0.5f) == "300 ms", "typed: a bare \"300\" on a control showing ms is 300 ms");
        check(typed(release, "0.3 s") == "300 ms" && typed(release, "1.2s") == "1200 ms", "typed: seconds typed into a control that shows ms are converted");
        check(typed(throwLength, "Rate") == "Rate" && same(TypedValue::toNormalised(readout(throwLength), "rate", 0.8f), 0.0f), "typed: a word the control shows (\"Rate\", any case)");
        check(typed(throwLength, "2.5") == "2.5 s" && typed(throwLength, "450 ms", 0.8f) == "0.45 s", "typed: \"2.5\" and \"450 ms\" on Throw Length");
        check(typed(duck, "4.8") == "-4.8 dB" && typed(duck, "-4.8 dB") == "-4.8 dB" && typed(duck, "off") == "Off", "typed: Hit Duck takes the amount with or without its minus, and \"off\"");
        check(typed(push, "99 dB") == "+15.0 dB" && typed(push, "-20") == "+0.0 dB", "typed: a value out of reach goes to the nearest end");
        check(typed(semis, "+7") == "+7 st" && typed(semis, "-12 st") == "-12 st" && typed(semis, "3.4") == "+3 st", "typed: semitones, stepped (3.4 is 3)");
        check(typed(pitch, "-3,5") == "-3.5 st", "typed: a decimal comma is a decimal point");
        check(TypedValue::toNormalised(readout(energy), "loud", 0.5f) < 0.0f && TypedValue::toNormalised(readout(energy), "", 0.5f) < 0.0f,
              "typed: text that is neither a number nor a readout is refused (the control stays put)");
    }

    // MIDI control ---------------------------------------------------------------------------------------------
    std::cout << "MIDI control" << std::endl;
    {
        Rig rig;
        auto* fader3 = rig.param(ParamIDs::cargoTrack[2]);
        auto* energy = rig.param(ParamIDs::energy);
        const float faderBefore = fader3->getValue();
        bool none = true;
        for (int t = 0; t < Proc::c_numCcTargets; ++t)
            none = none && rig.proc.getMidiControl(t) < 0;
        check(none, "midi: nothing is mapped until you learn something");

        // A controller that nothing has learned does nothing.
        rig.run({ juce::MidiMessage::controllerEvent(1, 21, 100) });
        rig.proc.applyMidiControls();
        check(same(fader3->getValue(), faderBefore), "midi: an unlearned controller moves nothing");

        // Learn: the next controller that moves is taken, and the fader goes to it (the rig is stopped here).
        rig.proc.learnMidiControl(2);
        check(rig.proc.getMidiLearnTarget() == 2, "midi: Learn waits for a controller");
        rig.run({ juce::MidiMessage::controllerEvent(1, 21, 64) });
        check(rig.proc.getMidiLearnTarget() == -1 && rig.proc.getMidiControl(2) == 21, "midi: the first controller that moves is learned (CC 21)");
        check(rig.proc.applyMidiControls() == 2, "midi: the rig reports what was just learned");
        check(std::abs(fader3->getValue() - 64.0f / 127.0f) < 1.0e-4f, "midi: the fader goes to the controller's position, with the rig stopped");
        check(settings.getValue("midiControls") == "2:21", "midi: the mapping is in the settings file (\"" + settings.getValue("midiControls") + "\")");

        // While playing too, on any channel; other controllers are ignored.
        rig.proc.setPlaying(true);
        rig.run();
        rig.run({ juce::MidiMessage::controllerEvent(5, 21, 127), juce::MidiMessage::controllerEvent(1, 22, 3) });
        rig.proc.applyMidiControls();
        check(std::abs(fader3->getValue() - 1.0f) < 1.0e-6f, "midi: it follows while the rig plays, on any channel, and other controllers do nothing");
        rig.run({ juce::MidiMessage::controllerEvent(1, 21, 0) });
        rig.proc.applyMidiControls();
        check(same(fader3->getValue(), 0.0f), "midi: the bottom of the controller is the bottom of the fader");
        rig.proc.setPlaying(false);
        check(! rig.proc.getHistory().canUndo(), "midi: controller moves are not undo steps");

        // Energy, and one controller for two targets.
        rig.proc.learnMidiControl(Proc::c_ccEnergy);
        rig.run({ juce::MidiMessage::controllerEvent(1, 21, 32) });
        rig.proc.applyMidiControls();
        check(rig.proc.getMidiControl(Proc::c_ccEnergy) == 21 && std::abs(energy->getValue() - 32.0f / 127.0f) < 1.0e-4f
                  && std::abs(fader3->getValue() - 32.0f / 127.0f) < 1.0e-4f,
              "midi: Energy can be learned, and one controller can move two things");
        check(Proc::midiTargetName(2) == "Track 3 Fader" && Proc::midiTargetName(Proc::c_ccEnergy) == "Energy", "midi: the targets have plain names");

        // The next rig (the same settings file) has the mappings.
        {
            Rig second;
            check(second.proc.getMidiControl(2) == 21 && second.proc.getMidiControl(Proc::c_ccEnergy) == 21 && second.proc.getMidiControl(0) == -1,
                  "midi: a rig opened afterwards has the same mappings");
            second.run({ juce::MidiMessage::controllerEvent(1, 21, 127) });
            second.proc.applyMidiControls();
            check(same(second.param(ParamIDs::cargoTrack[2])->getValue(), 1.0f), "midi: and its fader follows straight away");
        }

        // Forget.
        const int generation = rig.proc.getMidiControlGeneration();
        rig.proc.setMidiControl(2, -1);
        const float held = fader3->getValue();
        rig.run({ juce::MidiMessage::controllerEvent(1, 21, 90) });
        rig.proc.applyMidiControls();
        check(rig.proc.getMidiControl(2) == -1 && same(fader3->getValue(), held) && rig.proc.getMidiControlGeneration() != generation,
              "midi: Forget lets the fader go");
        check(settings.getValue("midiControls") == juce::String(Proc::c_ccEnergy) + ":21", "midi: and the settings file says so (\"" + settings.getValue("midiControls") + "\")");

        // Cancelling a Learn leaves everything as it was.
        rig.proc.learnMidiControl(4);
        rig.proc.learnMidiControl(-1);
        rig.run({ juce::MidiMessage::controllerEvent(1, 40, 90) });
        check(rig.proc.applyMidiControls() == -1 && rig.proc.getMidiControl(4) == -1, "midi: a cancelled Learn learns nothing");

        // Channel mode messages (Reset All Controllers and the like, which hosts send by themselves) are not controls.
        rig.proc.learnMidiControl(4);
        rig.run({ juce::MidiMessage::controllerEvent(1, 121, 0), juce::MidiMessage::controllerEvent(1, 126, 0) });
        check(rig.proc.getMidiLearnTarget() == 4 && rig.proc.getMidiControl(4) == -1, "midi: a channel mode message (CC 120 and up) is never learned");
        rig.proc.learnMidiControl(-1);
        rig.proc.setMidiControl(Proc::c_ccEnergy, -1);
    }

    // File names cut in the middle -------------------------------------------------------------------------------
    std::cout << "File names" << std::endl;
    {
        const juce::String name = "ANMLFarm_Pig Squeal Angry Close_Take 03_v2";
        const auto cut = Theme::middleTruncated(name, 24);
        std::cout << "         \"" << cut << "\"" << std::endl;
        check(cut.length() <= 24 && cut.startsWith("ANMLFarm_P") && cut.endsWith("03_v2") && cut.contains("..."),
              "names: a long name keeps its start and its end around three dots, within the length asked for");
        check(Theme::middleTruncated("Demo Growl", 24) == "Demo Growl", "names: a short name is left alone");
        check(Theme::middleTruncated(name, 4).isEmpty() || Theme::middleTruncated(name, 4).length() <= 4, "names: never longer than asked, however short");

        // Fitting a width (what the lane does), with a measure of 7 units a character and a count of the measurements.
        int measured = 0;
        const auto measure = [&measured](const juce::String& t) { ++measured; return 7.0f * static_cast<float>(t.length()); };
        check(Theme::fitMiddleBy(juce::String("wind"), measure, 100.0f) == "wind" && Theme::fitMiddleBy(juce::String("ab"), measure, 14.0f) == "ab",
              "names: a name shorter than six characters is shown whole when it fits");
        check(Theme::fitMiddleBy(juce::String("wind"), measure, 20.0f).isEmpty(), "names: and not at all when it does not");
        check(Theme::fitMiddleBy(name, measure, 1000.0f) == name, "names: a long name with room is shown whole");
        const auto fitted = Theme::fitMiddleBy(name, measure, 7.0f * 20.0f);
        check(fitted.length() <= 20 && fitted.length() >= 18 && fitted.contains("...") && fitted.startsWith("ANMLFarm") && fitted.endsWith("v2"),
              "names: a long name is cut to the longest that fits (\"" + fitted + "\")");
        check(Theme::fitMiddleBy(name, measure, 7.0f * 5.0f).isEmpty(), "names: no room for six characters shows nothing");
        measured = 0;
        const juce::String veryLong = juce::String::repeatedString("Long_File_Name_", 20);
        const auto cutLong = Theme::fitMiddleBy(veryLong, measure, 7.0f * 30.0f);
        check(cutLong.length() <= 30 && cutLong.length() >= 28 && measured <= 12,
              "names: fitting a 300-character name takes a handful of measurements (" + juce::String(measured) + ")");
    }

    Proc::useSettings(nullptr);
    std::cout << (g_failures == 0 ? "ALL PASSED" : juce::String(g_failures) + " FAILED") << std::endl;
    return g_failures == 0 ? 0 : 1;
}
