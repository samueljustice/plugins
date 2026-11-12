// Keep test: runs the real plug-in (no window) and checks everything about keeping work.
//
//   - Presets: a factory preset is a complete look; Randomise only moves what it rolls, with Width and Bend in safe ranges.
//   - Section dice and locks; the locks are saved with the session.
//   - The Randomise history: back and forward through the last 20 rolls and the settings before the first.
//   - Undo: one step per knob move, one per preset / roll however many knobs glide, the order across track edits and
//     settings, New Session in one step.
//   - Your own presets: files that round-trip, with and without the modulators and wires; rename; names.
//   - A/B.
//   - Session files: the whole state in one file, smaller than the old form, which is still read.
//
// Usage: SwooshbucklerKeep [scratch folder] (a folder in the temp directory if none is given). Nothing is read from
// or written to the user's settings or presets.

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "Parameters.h"
#include "Presets.h"
#include "UserPresets.h"

#include <cmath>
#include <iostream>

namespace
{
    constexpr double c_fs = 48000.0;
    int g_failures = 0;

    void check(bool ok, const juce::String& what)
    {
        std::cout << (ok ? "  [ok]   " : "  [FAIL] ") << what << std::endl;
        if (! ok)
            ++g_failures;
    }

    void writeWav(const juce::File& file, const juce::AudioBuffer<float>& audio)
    {
        file.deleteFile();
        juce::WavAudioFormat wav;
        auto* stream = new juce::FileOutputStream(file);
        std::unique_ptr<juce::AudioFormatWriter> writer(wav.createWriterFor(stream, c_fs, 1, 24, {}, 0));
        writer->writeFromAudioSampleBuffer(audio, 0, audio.getNumSamples());
    }

    void makeSources(const juce::File& folder)
    {
        juce::Random r(11);
        for (int k = 0; k < 2; ++k)
        {
            juce::AudioBuffer<float> audio(1, static_cast<int>(c_fs * (3.0 + k)));
            float y = 0.0f;
            for (int i = 0; i < audio.getNumSamples(); ++i)
            {
                y += 0.1f * ((r.nextFloat() * 2.0f - 1.0f) - y);
                audio.setSample(0, i, 2.0f * y * static_cast<float>(0.6 + 0.4 * std::sin((2.0 + k) * i / c_fs)));
            }
            writeWav(folder.getChildFile("sound" + juce::String(k) + ".wav"), audio);
        }
    }

    using Proc = SwooshbucklerAudioProcessor;

    struct Rig
    {
        std::unique_ptr<juce::AudioProcessor> base { createPluginFilter() };
        Proc& proc { *dynamic_cast<Proc*>(base.get()) };
        Rig() { proc.setPlayConfigDetails(0, 2, c_fs, 512); }

        juce::RangedAudioParameter* param(const juce::String& id) const
        {
            auto* p = proc.getState().getParameter(id);
            if (p == nullptr)
                check(false, "parameter " + id + " exists");
            return p;
        }
        float plain(const juce::String& id) const { auto* p = param(id); return p->convertFrom0to1(p->getValue()); }
        /** A knob move as the window makes it: one gesture. */
        void move(const juce::String& id, float plainValue)
        {
            auto* p = param(id);
            p->beginChangeGesture();
            p->setValueNotifyingHost(p->convertTo0to1(plainValue));
            p->endChangeGesture();
        }
        /** The header's Randomise or a section's die, landing at once (the window glides instead). */
        Presets::Preset roll(int section = -1)
        {
            EditHistory::Group group(proc.getHistory(), "Randomise");
            const auto rolled = proc.rollLook(section);
            proc.applyLook(rolled, Presets::Unlisted::Keep, "Randomise", "Randomised", true);
            return rolled;
        }
        bool step(int delta)
        {
            EditHistory::Group group(proc.getHistory(), "Roll History");
            if (const auto* look = proc.stepRoll(delta))
            {
                proc.applyLook(*look, Presets::Unlisted::Keep, "Roll History", look->name, true);
                return true;
            }
            return false;
        }
        bool load(int track, const juce::File& file)
        {
            juce::String error;
            const bool ok = proc.getCargo().loadFile(track, file, error);
            if (! ok)
                check(false, "loads " + file.getFileName() + ": " + error);
            return ok;
        }
        int length(int track) const { const auto d = proc.getCargo().get(track); return d != nullptr ? d->length() : 0; }
    };

    bool near(float a, float b, float tolerance = 1.0e-4f) { return std::abs(a - b) <= tolerance; }

    bool sameLook(const Proc::Look& a, const Proc::Look& b, juce::String* firstDifference = nullptr)
    {
        if (a.size() != b.size())
        {
            if (firstDifference != nullptr)
                *firstDifference = "sizes " + juce::String(static_cast<int>(a.size())) + " / " + juce::String(static_cast<int>(b.size()));
            return false;
        }
        for (const auto& [id, value] : a)
        {
            const auto it = b.find(id);
            if (it == b.end() || ! near(value, it->second, 2.0e-4f * juce::jmax(1.0f, std::abs(value))))
            {
                if (firstDifference != nullptr)
                    *firstDifference = id + " " + juce::String(value) + " / " + (it == b.end() ? juce::String("missing") : juce::String(it->second));
                return false;
            }
        }
        return true;
    }
}

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init;
    const juce::File folder = argc > 1 ? juce::File(argv[1]) : juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("SwooshbucklerKeepTest");
    folder.createDirectory();
    const auto sources = folder.getChildFile("sources");
    sources.createDirectory();
    makeSources(sources);

    // A scratch settings file (the presets folder sits beside it), shared with every processor made below.
    juce::PropertiesFile::Options options;
    options.applicationName = "SwooshbucklerKeepTest";
    options.filenameSuffix = ".settings";
    const auto settingsFile = folder.getChildFile("keep.settings");
    settingsFile.deleteFile();
    juce::PropertiesFile settings(settingsFile, options);
    Proc::useSettings(&settings);
    folder.getChildFile("Swooshbuckler Presets").deleteRecursively();

    // Presets and Randomise (the value maps themselves) ------------------------------------------------------
    {
        Rig rig;
        const auto& params = rig.proc.getParameters();

        // A factory preset is a complete look: what it doesn't name goes back to its default.
        rig.move(ParamIDs::hitHold, 0.8f);
        rig.move(ParamIDs::duckReturn, 0.6f);
        rig.move(ParamIDs::inflate, 0.9f); // the session's: no preset touches it
        const auto& factory = Presets::factory().front();
        rig.proc.applyLook(factory, Presets::Unlisted::Default, factory.name, factory.name);
        check(near(rig.plain(ParamIDs::hitHold), 0.12f) && near(rig.plain(ParamIDs::duckReturn), 0.0f) && near(rig.plain(ParamIDs::energy), 0.2f),
              "presets: a factory preset sets what it names and puts everything else of its own back to default");
        check(near(rig.plain(ParamIDs::inflate), 0.9f) && rig.proc.currentPresetName == factory.name,
              "presets: the output stage is left alone, and the preset's name is shown");

        // Randomise: only what it rolls moves.
        rig.move(ParamIDs::hitHold, 0.8f);
        rig.move(ParamIDs::duckReturn, 0.6f);
        rig.move(ParamIDs::hitFloor, 0.5f);
        rig.move(ParamIDs::passMode[0], 1.0f);
        rig.move(ParamIDs::throwLength, 2.0f);
        rig.move(ParamIDs::morphX, 0.9f);
        const auto before = rig.proc.captureLook();
        rig.proc.setRollSeed(1234);
        const auto rolled = rig.roll();
        bool keptUnlisted = true, movedListed = false;
        for (const auto& [id, value] : rig.proc.captureLook())
        {
            if (rolled.values.count(id) == 0)
                keptUnlisted = keptUnlisted && near(value, before.at(id));
            else if (! near(value, before.at(id)))
                movedListed = true;
        }
        check(keptUnlisted && movedListed && near(rig.plain(ParamIDs::hitHold), 0.8f) && near(rig.plain(ParamIDs::duckReturn), 0.6f)
                  && near(rig.plain(ParamIDs::passMode[0]), 1.0f) && near(rig.plain(ParamIDs::throwLength), 2.0f, 1.0e-3f),
              "randomise: every knob the roll doesn't name stays where it was (Hold " + juce::String(rig.plain(ParamIDs::hitHold), 2)
                  + ", Return " + juce::String(rig.plain(ParamIDs::duckReturn), 2) + ", Pass Mode, Throw Length)");
        check(near(rig.plain(ParamIDs::inflate), 0.9f) && rolled.values.count(ParamIDs::inflate) == 0,
              "randomise: the header's roll leaves the output stage alone");

        // Width and Bend are in the roll, inside safe ranges; the old targets() reset them to 0.7 / 0.6 every time.
        juce::Random r(99);
        float widthLo = 2.0f, widthHi = -1.0f, bendLo = 2.0f, bendHi = -1.0f;
        bool allThere = true;
        for (int i = 0; i < 400; ++i)
        {
            const auto p = Presets::roll(r);
            for (int b = 0; b < ParamIDs::numBuses; ++b)
            {
                const auto w = p.values.find(ParamIDs::bus[b][ParamIDs::Width]), bend = p.values.find(ParamIDs::bus[b][ParamIDs::Bend]);
                allThere = allThere && w != p.values.end() && bend != p.values.end();
                if (! allThere)
                    break;
                widthLo = juce::jmin(widthLo, w->second);
                widthHi = juce::jmax(widthHi, w->second);
                bendLo = juce::jmin(bendLo, bend->second);
                bendHi = juce::jmax(bendHi, bend->second);
            }
        }
        check(allThere && widthLo >= 0.35f && widthHi <= 1.0f && widthHi - widthLo > 0.5f && bendLo >= 0.3f && bendHi <= 0.9f && bendHi - bendLo > 0.45f,
              "randomise: Width rolls within " + juce::String(widthLo, 2) + " .. " + juce::String(widthHi, 2) + " and Bend within "
                  + juce::String(bendLo, 2) + " .. " + juce::String(bendHi, 2) + " (400 rolls)");

        // Every rolled value is a legal value of a real parameter that Randomise may move.
        bool legal = true;
        const auto sample = Presets::roll(r);
        for (const auto& [id, value] : sample.values)
        {
            auto* p = rig.proc.getState().getParameter(id);
            legal = legal && p != nullptr && ! Presets::isSessionParameter(id) && value >= p->getNormalisableRange().start && value <= p->getNormalisableRange().end;
        }
        check(legal && Presets::targets(params, Presets::roll(r), Presets::Unlisted::Keep).size() == Presets::roll(r).values.size(),
              "randomise: every rolled value belongs to a parameter and is inside its range");
    }

    // Section dice and locks ---------------------------------------------------------------------------------
    {
        Rig rig;
        rig.proc.setRollSeed(7);
        auto sectionOf = [](const juce::String& id)
        {
            juce::Random r(1);
            for (int s = 0; s < Presets::numSections; ++s)
                if (Presets::rollSection(r, s).values.count(id) != 0)
                    return s;
            return -1;
        };
        // A section's die moves that section only.
        for (int section = 0; section < Presets::numSections; ++section)
        {
            const auto before = rig.proc.captureLook(false, true);
            const auto rolled = rig.roll(section);
            bool onlyThat = ! rolled.values.empty(), moved = false;
            for (const auto& [id, value] : rig.proc.captureLook(false, true))
            {
                if (near(value, before.at(id)))
                    continue;
                moved = true;
                onlyThat = onlyThat && sectionOf(id) == section;
            }
            check(onlyThat && moved, juce::String("dice: the ") + Presets::sectionName(section) + " die moves only its own section ("
                                         + juce::String(static_cast<int>(rolled.values.size())) + " controls)");
        }
        check(rig.roll(Presets::Output).values.count(ParamIDs::inflate) == 1 && rig.plain(ParamIDs::inflate) <= 0.5f && rig.plain(ParamIDs::push) <= 6.0f,
              "dice: the Output die also rolls Inflate and Push, gently (Inflate " + juce::String(rig.plain(ParamIDs::inflate), 2)
                  + ", Push " + juce::String(rig.plain(ParamIDs::push), 1) + " dB)");

        // A locked section is left out of the header's roll; its own die still rolls it.
        rig.proc.setSectionLocked(Presets::BusA, true);
        rig.proc.setSectionLocked(Presets::Hits, true);
        const auto before = rig.proc.captureLook();
        bool lockedStayed = true, othersMoved = false;
        for (int i = 0; i < 5; ++i)
        {
            rig.roll();
            for (const auto& [id, value] : rig.proc.captureLook())
            {
                const int section = sectionOf(id);
                if (section == Presets::BusA || section == Presets::Hits)
                    lockedStayed = lockedStayed && near(value, before.at(id));
                else if (! near(value, before.at(id)))
                    othersMoved = true;
            }
        }
        check(lockedStayed && othersMoved, "locks: Bus A and Hits locked stay put through five Randomise rolls; the other sections roll");
        const float doppler = rig.plain("aDoppler");
        rig.roll(Presets::BusA);
        check(! near(rig.plain("aDoppler"), doppler), "locks: a locked section's own die still rolls it");

        // The locks are part of the session.
        juce::MemoryBlock state;
        rig.proc.getStateInformation(state);
        Rig other;
        other.proc.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        check(other.proc.isSectionLocked(Presets::BusA) && other.proc.isSectionLocked(Presets::Hits) && ! other.proc.isSectionLocked(Presets::Ride)
                  && other.proc.getLockedSections() == rig.proc.getLockedSections(),
              "locks: saved with the session and restored");
        other.proc.newSession();
        check(other.proc.getLockedSections() == 0, "locks: a new session has none");
    }

    // The Randomise history --------------------------------------------------------------------------------------
    {
        Rig rig;
        rig.proc.setRollSeed(42);
        rig.move(ParamIDs::energy, 0.5f);
        const auto start = rig.proc.captureLook(false, true);
        check(! rig.proc.canStepRoll(-1) && ! rig.proc.canStepRoll(1), "history: nothing to step to before the first roll");

        std::vector<Proc::Look> looks;
        for (int i = 0; i < 3; ++i)
        {
            rig.roll();
            looks.push_back(rig.proc.captureLook(false, true));
        }
        check(rig.proc.getRollCount() == 3 && rig.proc.canStepRoll(-1) && ! rig.proc.canStepRoll(1), "history: three rolls, a way back, nothing forward");
        rig.step(-1);
        const bool second = sameLook(rig.proc.captureLook(false, true), looks[1]);
        rig.step(-1);
        const bool first = sameLook(rig.proc.captureLook(false, true), looks[0]);
        rig.step(-1);
        juce::String difference;
        const bool atStart = sameLook(rig.proc.captureLook(false, true), start, &difference);
        check(second && first && atStart && ! rig.proc.canStepRoll(-1) && rig.proc.currentPresetName == "Default",
              "history: back steps through the rolls to the settings before the first one " + difference);
        rig.step(1);
        rig.step(1);
        rig.step(1);
        check(sameLook(rig.proc.captureLook(false, true), looks[2]) && ! rig.proc.canStepRoll(1) && rig.proc.currentPresetName == "Randomised",
              "history: forward comes back to the latest roll");

        // How you left a roll is kept when you step away from it.
        rig.step(-1);
        rig.move(ParamIDs::chaos, 0.11f);
        rig.step(1);
        rig.step(-1);
        check(near(rig.plain(ParamIDs::chaos), 0.11f), "history: a roll comes back as you left it (a tweak made on it is kept)");

        // A new roll from the middle forgets what was ahead.
        rig.roll();
        check(rig.proc.getRollCount() == 3 && ! rig.proc.canStepRoll(1), "history: rolling from the middle drops the rolls that were ahead");

        // Twenty rolls are kept, plus where you started.
        Rig long_;
        long_.proc.setRollSeed(5);
        std::vector<Proc::Look> all;
        all.push_back(long_.proc.captureLook(false, true));
        for (int i = 0; i < 26; ++i)
        {
            long_.roll();
            all.push_back(long_.proc.captureLook(false, true));
        }
        int steps = 0;
        while (long_.step(-1))
            ++steps;
        check(long_.proc.getRollCount() == Proc::c_maxRolls && steps == Proc::c_maxRolls
                  && sameLook(long_.proc.captureLook(false, true), all[all.size() - 1 - static_cast<size_t>(Proc::c_maxRolls)]),
              "history: after 26 rolls, back goes " + juce::String(steps) + " steps (the last 20 rolls and the one before them)");

        // Undo takes the history's place back with the settings.
        Rig undo;
        undo.proc.setRollSeed(3);
        undo.roll();
        undo.roll();
        undo.proc.undoEdit();
        check(undo.proc.getRollPosition() == 1 && undo.proc.canStepRoll(1), "history: undoing a roll steps the history back with it");
    }

    // Undo ---------------------------------------------------------------------------------------------------------
    {
        Rig rig;
        auto& history = rig.proc.getHistory();
        check(! history.canUndo() && ! history.canRedo(), "undo: a new rig has nothing to undo");

        // One knob move is one step.
        rig.move(ParamIDs::energy, 0.8f);
        juce::Thread::sleep(450); // slower than the quick-moves window: the next move is its own step
        rig.move(ParamIDs::rate, 0.9f);
        juce::String what;
        check(rig.proc.undoEdit(&what) && near(rig.plain(ParamIDs::rate), 0.4f) && near(rig.plain(ParamIDs::energy), 0.8f) && what == "Rate",
              "undo: takes back the last knob move only, and names it (" + what + ")");
        check(rig.proc.undoEdit(&what) && near(rig.plain(ParamIDs::energy), 0.35f) && ! history.canUndo(), "undo: then the one before it");
        check(rig.proc.redoEdit() && rig.proc.redoEdit() && near(rig.plain(ParamIDs::energy), 0.8f) && near(rig.plain(ParamIDs::rate), 0.9f) && ! history.canRedo(),
              "redo: brings both back");

        // Quick moves in a row (the mouse wheel, the pad's X then Y) are one step.
        juce::Thread::sleep(450);
        rig.move(ParamIDs::morphX, 0.2f);
        rig.move(ParamIDs::morphY, 0.3f);
        rig.move(ParamIDs::morphX, 0.25f);
        rig.proc.undoEdit();
        check(near(rig.plain(ParamIDs::morphX), 0.5f) && near(rig.plain(ParamIDs::morphY), 0.5f) && near(rig.plain(ParamIDs::rate), 0.9f),
              "undo: quick moves in a row are one step");

        // The transport is not an edit; a change without a gesture (automation) is not recorded.
        juce::Thread::sleep(450);
        rig.proc.setPlaying(true);
        rig.proc.setPlaying(false);
        rig.param(ParamIDs::fill)->setValueNotifyingHost(0.77f);
        rig.proc.undoEdit(&what);
        check(what == "Rate" && near(rig.plain(ParamIDs::fill), 0.77f), "undo: Play and automation are not steps (the step taken back was " + what + ")");
        history.clear();

        // A preset that glides: the gestures open together and land at different times; that is one step.
        rig.move(ParamIDs::energy, 0.5f);
        juce::Thread::sleep(450);
        const auto before = rig.proc.captureLook();
        const auto& preset = Presets::factory()[1];
        std::vector<std::pair<juce::RangedAudioParameter*, float>> glide;
        {
            EditHistory::Group group(history, preset.name);
            rig.proc.setPresetName(preset.name);
            for (const auto& [param, target] : Presets::targets(rig.proc.getParameters(), preset, Presets::Unlisted::Default))
                if (! near(param->getValue(), target, 1.0e-6f))
                {
                    param->beginChangeGesture();
                    glide.emplace_back(param, target);
                }
        }
        for (int frame = 1; frame <= 4; ++frame) // the springs: every knob part of the way, some landing early
        {
            for (size_t i = 0; i < glide.size(); ++i)
            {
                auto* param = glide[i].first;
                if (param == nullptr)
                    continue;
                const bool lands = frame == 4 || (i % 3 == 0 && frame == 2);
                param->setValueNotifyingHost(lands ? glide[i].second : param->getValue() + 0.25f * (glide[i].second - param->getValue()));
                if (lands)
                {
                    param->endChangeGesture();
                    glide[i].first = nullptr;
                }
            }
            juce::Thread::sleep(120);
        }
        check(glide.size() > 6 && rig.proc.currentPresetName == preset.name && near(rig.plain(ParamIDs::energy), 0.36f),
              "undo: a preset glided " + juce::String(static_cast<int>(glide.size())) + " controls into place");
        check(rig.proc.undoEdit(&what) && sameLook(rig.proc.captureLook(), before) && what == preset.name && rig.proc.currentPresetName == "Default",
              "undo: the whole glide is one step, and the preset's name goes back with it (" + what + ")");
        check(rig.proc.undoEdit() && near(rig.plain(ParamIDs::energy), 0.8f) && ! history.canUndo(), "undo: the step before the preset is the knob move before it");
        rig.proc.redoEdit();
        rig.proc.redoEdit();
        check(near(rig.plain(ParamIDs::energy), 0.36f) && near(rig.plain("aDoppler"), 0.9f) && rig.proc.currentPresetName == preset.name,
              "redo: the preset comes back in one step");

        // Randomise is one step too.
        const auto beforeRoll = rig.proc.captureLook();
        rig.roll();
        check(! sameLook(rig.proc.captureLook(), beforeRoll) && rig.proc.undoEdit() && sameLook(rig.proc.captureLook(), beforeRoll)
                  && rig.proc.currentPresetName == preset.name,
              "undo: a Randomise roll is one step");
    }
    {
        // Track edits and settings, in the order they happened.
        Rig rig;
        auto& history = rig.proc.getHistory();
        rig.load(0, sources.getChildFile("sound0.wav"));
        const int full = rig.length(0);
        rig.move(ParamIDs::energy, 0.7f);
        rig.proc.getCargo().erase(0, 0, full / 2);
        const int half = rig.length(0);
        juce::Thread::sleep(450);
        rig.move(ParamIDs::chaos, 0.9f);
        check(full > 0 && half < full, "undo: a track loaded, a knob moved, half the track deleted, another knob moved");
        rig.proc.undoEdit();
        const bool a = near(rig.plain(ParamIDs::chaos), 0.4f) && rig.length(0) == half;
        rig.proc.undoEdit();
        const bool b = rig.length(0) == full && near(rig.plain(ParamIDs::energy), 0.7f) && rig.proc.takeRestoredTrack() == 0;
        rig.proc.undoEdit();
        const bool c = near(rig.plain(ParamIDs::energy), 0.35f) && rig.length(0) == full;
        rig.proc.undoEdit();
        const bool d = rig.length(0) == 0 && ! history.canUndo();
        check(a && b && c && d, juce::String("undo: steps back through knob, track edit, knob, track load in that order (")
                                    + (a ? "1" : "-") + (b ? "2" : "-") + (c ? "3" : "-") + (d ? "4" : "-") + ")");
        rig.proc.redoEdit();
        rig.proc.redoEdit();
        rig.proc.redoEdit();
        rig.proc.redoEdit();
        check(rig.length(0) == half && near(rig.plain(ParamIDs::chaos), 0.9f) && near(rig.plain(ParamIDs::energy), 0.7f) && ! history.canRedo(),
              "redo: and forward again in the same order");

        // New Session is one step: tracks, settings, wires, the preset's name and the locks all come back.
        rig.load(1, sources.getChildFile("sound1.wav"));
        rig.proc.getCargo().rename(1, "My Wind");
        rig.proc.getCargo().setMute(CargoHold::c_numBuiltIns + 1, true);
        rig.proc.getMods().connect(0, ParamIDs::energy, 0.4f);
        rig.proc.setSectionLocked(Presets::Ride, true);
        const auto& preset = Presets::factory()[2];
        rig.proc.applyLook(preset, Presets::Unlisted::Default, preset.name, preset.name);
        rig.move(ParamIDs::inflate, 0.66f);
        const int len0 = rig.length(0), len1 = rig.length(1);
        const auto look = rig.proc.captureLook(true, true);
        rig.proc.newSession();
        check(rig.length(0) == 0 && rig.length(1) == 0 && near(rig.plain(ParamIDs::inflate), 0.3f) && rig.proc.getMods().getRoutes().empty()
                  && rig.proc.currentPresetName == "Default" && rig.proc.getLockedSections() == 0,
              "new session: tracks, settings, wires and locks are cleared");
        juce::String what, difference;
        const bool undone = rig.proc.undoEdit(&what);
        check(undone && what == "New Session" && rig.length(0) == len0 && rig.length(1) == len1 && sameLook(rig.proc.captureLook(true, true), look, &difference),
              "new session: one undo brings the tracks and every setting back " + difference);
        check(rig.proc.getCargo().getDisplayName(1) == "My Wind" && rig.proc.getCargo().getMute(CargoHold::c_numBuiltIns + 1)
                  && rig.proc.getMods().getRoutes().size() == 1 && rig.proc.currentPresetName == preset.name && rig.proc.isSectionLocked(Presets::Ride),
              "new session: and the track's name and mute, the wire, the preset's name and the lock");
        check(rig.proc.redoEdit() && rig.length(0) == 0 && rig.length(1) == 0 && rig.proc.getMods().getRoutes().empty(), "new session: redo clears it again");
        rig.proc.undoEdit();
        check(rig.length(1) == len1, "new session: and undo brings it back once more");
    }

    // Your own presets ---------------------------------------------------------------------------------------------
    {
        Rig rig;
        const auto presets = rig.proc.getPresetFolder();
        check(presets == settingsFile.getSiblingFile("Swooshbuckler Presets") && rig.proc.getUserPresets().empty(),
              "user presets: the folder sits beside the settings and starts empty");

        rig.proc.setRollSeed(77);
        rig.roll();
        rig.move(ParamIDs::hitHold, 0.44f);
        rig.move(ParamIDs::passMode[1], 1.0f);
        rig.move(ParamIDs::modKnob[0], 0.91f);
        rig.move(ParamIDs::inflate, 0.8f);
        rig.proc.getMods().connect(1, ParamIDs::rate, -0.3f);
        const auto saved = rig.proc.captureLook();
        juce::File plainFile, modsFile;
        juce::String error;
        check(rig.proc.saveUserPreset("Dry Jets", false, plainFile, error) && plainFile.existsAsFile() && plainFile.getParentDirectory() == presets
                  && plainFile.getFileName() == "Dry Jets.swooshpreset" && rig.proc.currentPresetName == "Dry Jets",
              "user presets: Save Preset writes Dry Jets.swooshpreset into the folder " + error);
        check(rig.proc.saveUserPreset("Wired: Jets?", true, modsFile, error) && modsFile.existsAsFile(), "user presets: a name with odd characters is made a legal file name ("
                  + modsFile.getFileNameWithoutExtension() + ")");
        check(! rig.proc.saveUserPreset("Jet Passes", false, plainFile, error) && error.isNotEmpty(), "user presets: a factory preset's name is refused (" + error + ")");
        check(plainFile.getSize() < 6000, "user presets: a preset file is small (" + juce::String(plainFile.getSize()) + " bytes)");

        // Another rig (as on another machine): the file alone brings the look back.
        Rig other;
        other.move(ParamIDs::modKnob[0], 0.2f);
        other.proc.getMods().connect(2, ParamIDs::chaos, 0.5f);
        UserPresets::Loaded loaded;
        error.clear();
        check(UserPresets::read(plainFile, loaded, error) && ! loaded.hasMods && loaded.preset.name == "Dry Jets", "user presets: the file reads back " + error);
        {
            EditHistory::Group group(other.proc.getHistory(), loaded.preset.name);
            other.proc.applyPresetWires(loaded);
            other.proc.applyLook(loaded.preset, Presets::Unlisted::Default, loaded.preset.name, loaded.preset.name);
        }
        juce::String difference;
        check(sameLook(other.proc.captureLook(), saved, &difference) && near(other.plain(ParamIDs::hitHold), 0.44f) && near(other.plain(ParamIDs::passMode[1]), 1.0f),
              "user presets: every control a preset owns round-trips through the file " + difference);
        check(near(other.plain(ParamIDs::inflate), 0.3f) && near(other.plain(ParamIDs::modKnob[0]), 0.2f) && other.proc.getMods().getRoutes().size() == 1
                  && other.proc.getMods().getRoutes()[0].param == ParamIDs::chaos,
              "user presets: saved without them, a preset leaves the output stage, the modulators and the wires alone");

        // With the modulators and wires.
        check(UserPresets::read(modsFile, loaded, error) && loaded.hasMods, "user presets: the second file carries the modulators and wires");
        {
            EditHistory::Group group(other.proc.getHistory(), loaded.preset.name);
            other.proc.applyPresetWires(loaded);
            other.proc.applyLook(loaded.preset, Presets::Unlisted::Default, loaded.preset.name, loaded.preset.name);
        }
        const auto routes = other.proc.getMods().getRoutes();
        check(near(other.plain(ParamIDs::modKnob[0]), 0.91f) && routes.size() == 1 && routes[0].param == ParamIDs::rate && routes[0].source == 1 && near(routes[0].depth, -0.3f)
                  && near(other.plain(ParamIDs::inflate), 0.3f),
              "user presets: saved with them, the modulator's knob and the wire come back (and the output stage still stays)");
        other.proc.undoEdit();
        const auto undone = other.proc.getMods().getRoutes();
        check(near(other.plain(ParamIDs::modKnob[0]), 0.2f) && undone.size() == 1 && undone[0].param == ParamIDs::chaos && other.proc.currentPresetName == "Dry Jets",
              "user presets: loading one is one undo step, wires included");

        // The list, renaming, unique names.
        auto names = [&rig]
        {
            juce::StringArray list;
            for (const auto& f : rig.proc.getUserPresets())
                list.add(f.getFileNameWithoutExtension());
            return list.joinIntoString(", ");
        };
        check(rig.proc.getUserPresets().size() == 2 && names().startsWith("Dry Jets, Wired"), "user presets: the list is by name (" + names() + ")");
        juce::File renamed;
        check(UserPresets::rename(plainFile, "Wet Jets", renamed, error) && renamed.existsAsFile() && ! plainFile.existsAsFile()
                  && UserPresets::read(renamed, loaded, error) && loaded.preset.name == "Wet Jets" && sameLook(loaded.preset.values, saved),
              "user presets: Rename moves the file and keeps its settings");
        check(! UserPresets::rename(renamed, modsFile.getFileNameWithoutExtension(), plainFile, error) && renamed.existsAsFile(),
              "user presets: renaming onto another preset's name is refused (" + error + ")");
        check(UserPresets::uniqueName(presets, "Roll") == "Roll", "user presets: the first kept roll is called Roll");
        rig.proc.saveUserPreset("Roll", false, plainFile, error);
        check(UserPresets::uniqueName(presets, "Roll") == "Roll 2" && UserPresets::uniqueName(presets, "Jet Passes") == "Jet Passes 2",
              "user presets: the next is Roll 2, and a factory name is never handed out");

        // A file that isn't a preset.
        const auto junk = folder.getChildFile("junk.swooshpreset");
        junk.replaceWithText("not a preset");
        check(! UserPresets::read(junk, loaded, error) && error.isNotEmpty(), "user presets: a file that isn't one is refused (" + error + ")");
        // A hand-edited file can't reach the session's parameters.
        const auto sly = folder.getChildFile("sly.swooshpreset");
        sly.replaceWithText("<SwooshbucklerPreset name=\"sly\" version=\"1\"><Value id=\"ceiling\" value=\"0\"/><Value id=\"cargo1\" value=\"0\"/>"
                            "<Value id=\"energy\" value=\"7\"/></SwooshbucklerPreset>");
        check(UserPresets::read(sly, loaded, error), "user presets: a hand-made file reads");
        other.proc.applyLook(loaded.preset, Presets::Unlisted::Default, "sly", "sly");
        check(near(other.plain(ParamIDs::ceiling), -0.3f) && near(other.plain(ParamIDs::cargoTrack[0]), 0.93f) && near(other.plain(ParamIDs::energy), 1.0f),
              "user presets: a file can't move the tracks or the output stage, and a value out of range is brought into it");
    }

    // A/B ------------------------------------------------------------------------------------------------------------
    {
        Rig rig;
        rig.move(ParamIDs::energy, 0.2f);
        rig.move(ParamIDs::inflate, 0.7f);
        check(rig.proc.getABSlot() == 0, "a/b: starts on A");
        rig.proc.switchAB();
        check(rig.proc.getABSlot() == 1 && near(rig.plain(ParamIDs::energy), 0.2f), "a/b: B starts as a copy of A");
        rig.move(ParamIDs::energy, 0.9f);
        rig.move("aDrive", 0.4f);
        rig.move(ParamIDs::inflate, 0.1f); // the session's: A/B leaves it where it is
        rig.proc.switchAB();
        check(rig.proc.getABSlot() == 0 && near(rig.plain(ParamIDs::energy), 0.2f) && near(rig.plain("aDrive"), 0.12f) && near(rig.plain(ParamIDs::inflate), 0.1f),
              "a/b: back on A its settings return; the output stage is not part of a set");
        rig.proc.switchAB();
        check(rig.proc.getABSlot() == 1 && near(rig.plain(ParamIDs::energy), 0.9f) && near(rig.plain("aDrive"), 0.4f), "a/b: and B's are as they were left");
        rig.proc.undoEdit();
        check(rig.proc.getABSlot() == 0 && near(rig.plain(ParamIDs::energy), 0.2f), "a/b: a switch is one undo step");
        rig.proc.redoEdit();

        // Saved with the session.
        juce::MemoryBlock state;
        rig.proc.getStateInformation(state);
        Rig other;
        other.proc.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        const bool onB = other.proc.getABSlot() == 1 && near(other.plain(ParamIDs::energy), 0.9f);
        other.proc.switchAB();
        check(onB && other.proc.getABSlot() == 0 && near(other.plain(ParamIDs::energy), 0.2f) && near(other.plain("aDrive"), 0.12f),
              "a/b: both sets and the one you are on are saved with the session");

        // Copy To Other.
        other.move(ParamIDs::rate, 0.05f);
        other.proc.copyABToOther();
        other.move(ParamIDs::rate, 0.6f);
        other.proc.switchAB();
        check(near(other.plain(ParamIDs::rate), 0.05f) && near(other.plain(ParamIDs::energy), 0.2f), "a/b: Copy To Other puts these settings in the other set");
    }

    // Session files ------------------------------------------------------------------------------------------------
    {
        const auto sessions = folder.getChildFile("sessions");
        sessions.deleteRecursively();
        sessions.createDirectory();
        Rig rig;
        rig.load(0, sources.getChildFile("sound0.wav"));
        rig.load(1, sources.getChildFile("sound1.wav"));
        rig.proc.getCargo().rename(1, "Wind Bed");
        rig.proc.setRollSeed(9);
        rig.roll();
        rig.move(ParamIDs::inflate, 0.55f);
        rig.move(ParamIDs::cargoTrack[1], 0.5f);
        rig.proc.getMods().connect(3, ParamIDs::space, 0.25f);
        rig.proc.setSectionLocked(Presets::Output, true);
        rig.proc.currentPresetName = "My Look";
        const auto look = rig.proc.captureLook(true, true);
        check(rig.proc.getSessionFile() == juce::File(), "sessions: a new session has no file");

        juce::String error;
        const auto file = sessions.getChildFile("Dragon Pass"); // no extension given: it gets one
        check(rig.proc.saveSession(file, error) && rig.proc.getSessionFile() == sessions.getChildFile("Dragon Pass.swooshsession")
                  && rig.proc.getSessionFile().existsAsFile(),
              "sessions: Save Session As writes Dragon Pass.swooshsession " + error);
        const auto written = rig.proc.getSessionFile();

        // The file is the binary form; the same state as XML (how sessions were stored before) is bigger.
        juce::MemoryBlock data;
        written.loadFileAsData(data);
        juce::ValueTree tree;
        const bool reads = Proc::readState(data.getData(), static_cast<int>(data.getSize()), tree);
        juce::MemoryBlock xmlForm;
        if (auto xml = tree.createXml())
            juce::AudioProcessor::copyXmlToBinary(*xml, xmlForm);
        check(reads && juce::AudioProcessor::getXmlFromBinary(data.getData(), static_cast<int>(data.getSize())) == nullptr
                  && data.getSize() < xmlForm.getSize() * 85 / 100,
              "sessions: the file is " + juce::String(static_cast<int>(data.getSize() / 1024)) + " KB; the same session in the old XML form is "
                  + juce::String(static_cast<int>(xmlForm.getSize() / 1024)) + " KB");

        // Opened in another rig: everything is there, and the session is named after the file.
        Rig other;
        other.move(ParamIDs::energy, 0.99f);
        check(other.proc.openSession(written, error), "sessions: Open Session reads it " + error);
        juce::String difference;
        check(sameLook(other.proc.captureLook(true, true), look, &difference) && near(other.plain(ParamIDs::cargoTrack[1]), 0.5f)
                  && other.length(0) == rig.length(0) && other.length(1) == rig.length(1) && other.length(0) > 0
                  && other.proc.getCargo().getDisplayName(1) == "Wind Bed",
              "sessions: the settings, both tracks' audio and the track's name come back " + difference);
        const auto wires = other.proc.getMods().getRoutes();
        check(wires.size() == 1 && wires[0].param == ParamIDs::space && other.proc.isSectionLocked(Presets::Output) && other.proc.currentPresetName == "My Look"
                  && other.proc.getSessionFile() == written && ! other.proc.getHistory().canUndo(),
              "sessions: and the wire, the lock, the preset's name and the session's file (nothing of the old session is left to undo)");

        // A moved file is known by where it is now.
        const auto moved = sessions.getChildFile("Renamed Pass.swooshsession");
        written.copyFileTo(moved);
        check(other.proc.openSession(moved, error) && other.proc.getSessionFile() == moved, "sessions: a copied file opens under its own name");

        // Save (to the same file) after a change.
        other.move(ParamIDs::energy, 0.123f);
        check(other.proc.saveSession(other.proc.getSessionFile(), error), "sessions: Save writes to the session's file " + error);
        Rig third;
        third.proc.openSession(moved, error);
        check(near(third.plain(ParamIDs::energy), 0.123f), "sessions: the saved change is in the file");

        // The session's file is part of the state (the app reopens where you left off, name included); New forgets it.
        juce::MemoryBlock state;
        other.proc.getStateInformation(state);
        Rig reopened;
        reopened.proc.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        check(reopened.proc.getSessionFile() == moved, "sessions: the state remembers which file it is");
        reopened.proc.newSession();
        check(reopened.proc.getSessionFile() == juce::File(), "sessions: a new session has no file again");
        reopened.proc.undoEdit();
        check(reopened.proc.getSessionFile() == moved, "sessions: undoing New Session brings the file's name back too");

        // The old form (XML, audio in base64) still opens: as a state, and as a file.
        Rig old;
        old.proc.setStateInformation(xmlForm.getData(), static_cast<int>(xmlForm.getSize()));
        check(sameLook(old.proc.captureLook(true, true), look, &difference) && old.length(0) == rig.length(0) && old.length(1) == rig.length(1),
              "sessions: a state in the old XML form is still read " + difference);
        const auto oldFile = sessions.getChildFile("Old Form.swooshsession");
        oldFile.replaceWithData(xmlForm.getData(), xmlForm.getSize());
        Rig oldToo;
        check(oldToo.proc.openSession(oldFile, error) && oldToo.length(1) == rig.length(1), "sessions: and so is a file in it");

        // What isn't a session is refused and changes nothing.
        const auto junk = sessions.getChildFile("junk.swooshsession");
        junk.replaceWithText("This is not a session at all, just some text that is long enough to look like one.");
        const float energyBefore = third.plain(ParamIDs::energy);
        check(! third.proc.openSession(junk, error) && error.isNotEmpty() && near(third.plain(ParamIDs::energy), energyBefore)
                  && third.length(0) == rig.length(0) && third.proc.getSessionFile() == moved,
              "sessions: a file that isn't a session is refused and nothing changes (" + error + ")");
        check(! third.proc.openSession(sessions.getChildFile("missing.swooshsession"), error), "sessions: a missing file is refused (" + error + ")");

        // Recent sessions: newest first, only files that still exist.
        const auto recent = rig.proc.getRecentSessions();
        check(recent.size() == 3 && recent[0] == oldFile && recent[1] == moved && recent[2] == written, "sessions: the recent list is newest first ("
                  + juce::String(recent.size()) + ")");
        oldFile.deleteFile();
        check(rig.proc.getRecentSessions().size() == 2, "sessions: a file that has gone drops off the recent list");
        juce::PropertiesFile reread(settingsFile, options);
        check(reread.getValue("recentSessions").contains("Renamed Pass"), "sessions: the recent list is kept in the settings file");
        rig.proc.clearRecentSessions();
        check(rig.proc.getRecentSessions().isEmpty(), "sessions: Clear Recent Sessions empties it");
    }

    Proc::useSettings(nullptr);
    std::cout << (g_failures == 0 ? "\nALL PASSED" : "\nFAILED: " + juce::String(g_failures)) << std::endl;
    return g_failures == 0 ? 0 : 1;
}
