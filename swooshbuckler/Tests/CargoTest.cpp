// Offline tests for the sample tracks and the things around them.
//
//   SwooshbucklerCargo <output folder>
//
// Region detection on a gappy sample, editing with undo, the CargoPlayer, the
// mixer (mute / solo), session save / restore, the tape recorder, presets.

#include <JuceHeader.h>
#include "Cargo.h"
#include "MasterProcessor.h"
#include "Presets.h"
#include "TapeRecorder.h"

namespace
{
    constexpr double c_rate = 48000.0;
    constexpr int c_block = 256;
    int g_failures = 0;

    void check(bool ok, const juce::String& what)
    {
        std::cout << (ok ? "  [ok]   " : "  [FAIL] ") << what << std::endl;
        if (! ok) ++g_failures;
    }

}

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init;
    const juce::File folder = argc > 1 ? juce::File(argv[1]) : juce::File::getCurrentWorkingDirectory();
    folder.createDirectory();

    // Cargo: a "pig squeal" track that is mostly silence, as the only source
    {
        CargoHold hold;
        juce::AudioBuffer<float> squeals(1, static_cast<int>(c_rate * 8.0));
        squeals.clear();
        float* d = squeals.getWritePointer(0);
        juce::Random rng(7);
        double phase = 0.0;
        for (int k = 0; k < 4; ++k)
        {
            const int start = static_cast<int>((0.5 + k * 2.0) * c_rate);
            const int len = static_cast<int>(0.35 * c_rate);
            for (int i = 0; i < len; ++i)
            {
                const double t = i / c_rate;
                const double f = 900.0 + 1400.0 * std::sin(juce::MathConstants<double>::pi * t / 0.35) + 60.0 * std::sin(40.0 * t);
                phase += f / c_rate;
                const float env = static_cast<float>(std::sin(juce::MathConstants<double>::pi * i / len));
                d[start + i] = 0.5f * env * static_cast<float>(std::sin(juce::MathConstants<double>::twoPi * phase)
                                                             + 0.3 * std::sin(2.0 * juce::MathConstants<double>::twoPi * phase))
                             + 0.01f * rng.nextFloat() * env;
            }
        }

        auto tmp = folder.getChildFile("squeals_source.wav");
        tmp.deleteFile();
        {
            juce::WavAudioFormat wav;
            auto* stream = new juce::FileOutputStream(tmp);
            std::unique_ptr<juce::AudioFormatWriter> writer(wav.createWriterFor(stream, c_rate, 1, 24, {}, 0));
            writer->writeFromAudioSampleBuffer(squeals, 0, squeals.getNumSamples());
        }

        juce::String error;
        check(hold.loadFile(0, tmp, error), "cargo: loads a WAV " + error);
        const auto data = hold.get(0);
        check(data != nullptr && data->regions.size() == 4, "cargo: detects the 4 squeals (found "
              + juce::String(data != nullptr ? static_cast<int>(data->regions.size()) : -1) + ")");

        // Editing round trip
        const int len = data->length();
        hold.duplicate(0, data->regions[0].start, data->regions[0].end);
        check(hold.get(0)->length() == len + (data->regions[0].end - data->regions[0].start), "cargo: duplicate inserts a copy");
        hold.undo(0);
        check(hold.get(0)->length() == len, "cargo: undo restores the length");
        hold.trim(0, data->regions[1].start, data->regions[2].end);
        check(hold.get(0)->regions.size() == 2, "cargo: trim keeps two squeals");
        hold.undo(0);

        auto tree = hold.toValueTree();
        CargoHold restored;
        restored.fromValueTree(tree);
        check(restored.get(0) != nullptr && std::abs(restored.get(0)->length() - len) < 16, "cargo: survives save/restore");

        // The player on its own: a Tonal one-shot is silent between cues and its focus lands on time.
        {
            CargoPlayer player;
            player.prepare(c_rate);
            sj::Random prng;
            const auto* live = hold.getLive(0);
            const double toPeak = 0.6;
            std::vector<float> mono(static_cast<size_t>(3.0 * c_rate), 0.0f);
            constexpr int chunk = 32;
            for (int pos = 0; pos + chunk <= static_cast<int>(mono.size()); pos += chunk)
            {
                CargoCue cue;
                if (pos == chunk * 10) { cue.active = true; cue.secondsToPeak = toPeak; cue.flightSeconds = 1.2; }
                player.render(live, CargoRole::Tonal, true, 1.0f, 0.0f, prng, 0.3f, cue, mono.data() + pos, chunk);
            }
            int loudest = 0;
            float maxStep = 0.0f;
            for (size_t i = 0; i < mono.size(); ++i)
            {
                if (std::abs(mono[i]) > std::abs(mono[static_cast<size_t>(loudest)])) loudest = static_cast<int>(i);
                if (i > 0) maxStep = juce::jmax(maxStep, std::abs(mono[i] - mono[i - 1]));
            }
            const double cueAt = chunk * 10 / c_rate;
            const double errorMs = 1000.0 * (loudest / c_rate - (cueAt + toPeak));
            check(std::abs(errorMs) < 80.0, "player: Tonal focus lands on secondsToPeak (" + juce::String(errorMs, 1) + " ms)");
            float before = 0.0f, after = 0.0f;
            for (size_t i = 0; i < static_cast<size_t>(cueAt * c_rate); ++i) before = juce::jmax(before, std::abs(mono[i]));
            for (size_t i = static_cast<size_t>(2.2 * c_rate); i < mono.size(); ++i) after = juce::jmax(after, std::abs(mono[i]));
            check(before == 0.0f && after == 0.0f, "player: Tonal is silent before and after its accent");
            check(maxStep < 0.5f, "player: Tonal accent has no clicks (max step " + juce::String(maxStep, 3) + ")");

            // Air: time-scaled to the flight length, still click-free.
            player.reset();
            std::fill(mono.begin(), mono.end(), 0.0f);
            for (int pos = 0; pos + chunk <= static_cast<int>(mono.size()); pos += chunk)
            {
                CargoCue cue;
                if (pos == 0 || pos == chunk * 1500) { cue.active = true; cue.secondsToPeak = 0.05; cue.flightSeconds = 0.4; }
                player.render(live, CargoRole::Air, true, 1.0f, 0.5f, prng, 0.3f, cue, mono.data() + pos, chunk);
            }
            maxStep = 0.0f;
            float airPeak = 0.0f;
            for (size_t i = 1; i < mono.size(); ++i)
            {
                maxStep = juce::jmax(maxStep, std::abs(mono[i] - mono[i - 1]));
                airPeak = juce::jmax(airPeak, std::abs(mono[i]));
            }
            check(airPeak > 0.01f && maxStep < 0.5f, "player: Air accents play and stay click-free (max step "
                  + juce::String(maxStep, 3) + ")");
        }

        // Mixer: mute and solo via laneGain.
        {
            check(hold.laneGain(0) == 1.0f && hold.laneGain(6) == 1.0f, "mixer: lanes start open");
            hold.setMute(6, true);
            check(hold.laneGain(6) == 0.0f && hold.laneGain(0) == 1.0f, "mixer: mute closes only its lane");
            hold.setSolo(7, true);
            check(hold.laneGain(7) == 1.0f && hold.laneGain(0) == 0.0f && hold.laneGain(6) == 0.0f,
                  "mixer: solo closes every other lane");
            hold.setSolo(0, true);
            check(hold.laneGain(0) == 1.0f && hold.laneGain(7) == 1.0f && hold.laneGain(1) == 0.0f, "mixer: two solos");
            hold.setSolo(0, false);
            hold.setSolo(7, false);
            check(hold.laneGain(0) == 1.0f && hold.laneGain(6) == 0.0f, "mixer: unsolo restores, mute stays");
            hold.setSolo(6, true);
            check(hold.laneGain(6) == 0.0f, "mixer: mute wins over solo");
            hold.setSolo(6, false);

            // Mixer, role and name survive save / restore.
            hold.setSolo(2, true);
            hold.setRoleOverride(0, static_cast<int>(CargoRole::Bed));
            hold.rename(0, "Squealer");
            CargoHold again;
            again.fromValueTree(hold.toValueTree());
            check(again.getMute(6) && again.getSolo(2) && ! again.getSolo(0) && again.laneGain(3) == 0.0f,
                  "mixer: mute / solo survive save/restore");
            check(again.getRoleOverride(0) == static_cast<int>(CargoRole::Bed), "cargo: role override survives save/restore");
            check(again.getDisplayName(0) == "Squealer", "cargo: user name survives save/restore");
            check(again.get(0) != nullptr && again.get(0)->detectedRole == hold.get(0)->detectedRole,
                  "cargo: detected role survives save/restore");
            hold.rename(0, {});
            check(hold.getDisplayName(0).isNotEmpty() && hold.getDisplayName(0) != "Squealer", "cargo: empty rename goes back to automatic");
            check(hold.getDisplayName(3) == "Track 4", "cargo: empty track is named Track N");
            check(hold.firstEmptyTrack() == 1, "cargo: first empty track");
        }

        // Dense cues (Energy mode at a high Rate, fast MIDI): every accent still plays, none is dropped because
        // the next whoosh arrived before it started.
        {
            CargoPlayer player;
            player.prepare(c_rate);
            sj::Random prng;
            const auto* live = hold.getLive(0);
            const double toPeak = 0.6, spacing = 0.3;
            std::vector<float> mono(static_cast<size_t>(4.0 * c_rate), 0.0f);
            constexpr int chunk = 32;
            std::vector<double> cues;
            double nextCue = 0.05;
            for (int pos = 0; pos + chunk <= static_cast<int>(mono.size()); pos += chunk)
            {
                CargoCue cue;
                const double t = pos / c_rate;
                if (t >= nextCue && nextCue < 2.5)
                {
                    cue.active = true; cue.secondsToPeak = toPeak; cue.flightSeconds = 1.2;
                    cues.push_back(t);
                    nextCue += spacing;
                }
                player.render(live, CargoRole::Tonal, true, 1.0f, 0.0f, prng, 0.3f, cue, mono.data() + pos, chunk);
            }
            int heard = 0;
            for (double c : cues)
            {
                const int a = static_cast<int>((c + toPeak - 0.04) * c_rate), b = static_cast<int>((c + toPeak + 0.04) * c_rate);
                float m = 0.0f;
                for (int i = juce::jmax(0, a); i < juce::jmin(static_cast<int>(mono.size()), b); ++i)
                    m = juce::jmax(m, std::abs(mono[static_cast<size_t>(i)]));
                if (m > 0.05f) ++heard;
            }
            check(heard >= static_cast<int>(cues.size()) - 1, "player: dense cues keep every accent (" + juce::String(heard)
                  + " of " + juce::String(static_cast<int>(cues.size())) + " heard on their peak)");
        }

        // Toggling Skip silence while a loop plays never cuts the loop in one sample.
        {
            auto runLoop = [&](bool toggle)
            {
                CargoPlayer player;
                player.prepare(c_rate);
                sj::Random prng(77);
                const auto* live = hold.getLive(0);
                std::vector<float> mono(static_cast<size_t>(8.0 * c_rate), 0.0f);
                constexpr int chunk = 32;
                bool skip = false;
                int n = 0;
                for (int pos = 0; pos + chunk <= static_cast<int>(mono.size()); pos += chunk, ++n)
                {
                    if (toggle && n % 547 == 0) skip = ! skip; // every ~0.36 s
                    CargoCue cue;
                    player.render(live, CargoRole::Generic, skip, 1.0f, 0.0f, prng, 0.4f, cue, mono.data() + pos, chunk);
                }
                float maxStep = 0.0f;
                for (size_t i = static_cast<size_t>(0.1 * c_rate); i < mono.size(); ++i)
                    maxStep = juce::jmax(maxStep, std::abs(mono[i] - mono[i - 1]));
                return maxStep;
            };
            const float base = runLoop(false), toggled = runLoop(true);
            check(toggled <= base * 1.5f + 0.02f, "player: Skip silence toggles are click-free (max step " + juce::String(toggled, 3)
                  + ", steady " + juce::String(base, 3) + ")");
        }

        // Sessions: v1 trees, older 4-track sessions, loading over existing state, audio over full scale,
        // and undo of Clear lane.
        {
            // v1 tree: no Mixer, no detected role -> classified on restore, mixer cleared.
            auto v1 = hold.toValueTree();
            v1.removeChild(v1.getChildWithName("Mixer"), nullptr);
            for (auto node : v1)
                for (auto* prop : { "detectedRole", "roleConfidence", "autoLabel" })
                    node.removeProperty(prop, nullptr);
            CargoHold old;
            old.setMute(3, true);
            old.fromValueTree(v1);
            check(old.get(0) != nullptr && ! old.get(0)->regions.empty() && old.get(0)->autoLabel.isNotEmpty(),
                  "session: a v1 tree is classified on restore");
            check(! old.getMute(3) && ! old.getMute(6), "session: a v1 tree clears the mixer");

            // Loading over existing state: tracks missing from the tree are cleared, undo history is dropped.
            CargoHold over;
            juce::String err;
            over.loadFile(0, tmp, err);
            over.loadFile(0, tmp, err);   // undo history on track 0
            over.loadFile(5, tmp, err);   // stale audio on track 6
            over.rename(5, "Stale");
            auto fourTracks = hold.toValueTree();
            for (int i = fourTracks.getNumChildren(); --i >= 0;)
                if (fourTracks.getChild(i).hasType("Track") && static_cast<int>(fourTracks.getChild(i).getProperty("index")) >= 4)
                    fourTracks.removeChild(i, nullptr);
            over.fromValueTree(fourTracks);
            check(over.get(5) == nullptr && over.getDisplayName(5) == "Track 6", "session: a 4-track session clears tracks 5 and 6");
            check(! over.canUndo(0) && ! over.canRedo(0), "session: loading a session drops the undo history");
            over.fromValueTree(juce::ValueTree("Cargo"));
            check(over.get(0) == nullptr, "session: a state without cargo clears the lanes");

            // Over 0 dBFS: a float source that peaks at +6 dB survives save / restore unclipped.
            auto hot = folder.getChildFile("hot_source.wav");
            hot.deleteFile();
            {
                juce::AudioBuffer<float> buf(1, static_cast<int>(c_rate));
                for (int i = 0; i < buf.getNumSamples(); ++i)
                    buf.setSample(0, i, 2.0f * static_cast<float>(std::sin(juce::MathConstants<double>::twoPi * 220.0 * i / c_rate))
                                        * std::sin(juce::MathConstants<float>::pi * static_cast<float>(i) / static_cast<float>(buf.getNumSamples())));
                juce::WavAudioFormat wav;
                std::unique_ptr<juce::AudioFormatWriter> writer(wav.createWriterFor(new juce::FileOutputStream(hot), c_rate, 1, 32, {}, 0));
                writer->writeFromAudioSampleBuffer(buf, 0, buf.getNumSamples());
            }
            CargoHold loud;
            check(loud.loadFile(0, hot, err), "session: loads a 32-bit float WAV");
            const float before = loud.get(0) != nullptr ? loud.get(0)->audio.getMagnitude(0, 0, loud.get(0)->length()) : 0.0f;
            CargoHold back;
            back.fromValueTree(loud.toValueTree());
            const float after = back.get(0) != nullptr ? back.get(0)->audio.getMagnitude(0, 0, back.get(0)->length()) : 0.0f;
            check(before > 1.5f && std::abs(after - before) < 0.01f * before,
                  "session: audio over 0 dBFS survives restore (" + juce::String(before, 3) + " -> " + juce::String(after, 3) + ")");

            // Clear lane, then undo: the name and role come back too; redo clears them again.
            CargoHold lane;
            lane.loadFile(1, tmp, err);
            lane.rename(1, "Mine");
            lane.setRoleOverride(1, static_cast<int>(CargoRole::Impact));
            lane.clear(1);
            lane.undo(1);
            check(lane.get(1) != nullptr && lane.getDisplayName(1) == "Mine" && lane.getRoleOverride(1) == static_cast<int>(CargoRole::Impact),
                  "cargo: undoing Clear lane restores the name and role");
            lane.redo(1);
            check(lane.get(1) == nullptr && lane.getRoleOverride(1) == -1 && lane.getDisplayName(1) == "Track 2",
                  "cargo: redo clears them again");
        }
    }


    // Delay line: the interpolation fraction keeps full precision high up the ring (no 1/64-sample steps).
    {
        sj::DelayLine line;
        line.prepare(static_cast<int>(4.6 * c_rate)); // 2^18 ring, as the layer lines
        const double f = 1000.0;
        const int pushes = (1 << 18) - 64;
        for (int n = 0; n < pushes; ++n)
            line.push(static_cast<float>(std::sin(juce::MathConstants<double>::twoPi * f * n / c_rate)));
        double worst = 0.0;
        for (int k = 0; k < 400; ++k)
        {
            const double d = 1000.0 + k * 0.01237;
            const double expected = std::sin(juce::MathConstants<double>::twoPi * f * ((pushes - 1) - d) / c_rate);
            worst = std::max(worst, std::abs(static_cast<double>(line.read(d)) - expected));
        }
        check(worst < 3.0e-4, "dsp: fractional delay is exact at large ring indices (error " + juce::String(worst, 6) + ")");
    }

    // Tape recorder: a take, a rate change while rolling, the retro buffer on silence.
    {
        auto tapeFolder = folder.getChildFile("tape");
        tapeFolder.deleteRecursively();
        tapeFolder.createDirectory();
        TapeRecorder tape;
        tape.prepare(c_rate, 2);
        juce::String error;
        check(tape.start(tapeFolder, 24, error), "tape: starts a take " + error);
        juce::AudioBuffer<float> block(2, c_block);
        auto feed = [&](double seconds, double rate)
        {
            for (int pos = 0; pos < static_cast<int>(seconds * rate); pos += c_block)
            {
                for (int i = 0; i < c_block; ++i)
                    for (int ch = 0; ch < 2; ++ch)
                        block.setSample(ch, i, 0.25f * static_cast<float>(std::sin(0.05 * (pos + i))));
                tape.process(block.getArrayOfReadPointers(), 2, c_block);
            }
        };
        feed(1.0, c_rate);
        const auto first = tape.getCurrentFile();
        tape.prepare(96000.0, 2); // the device / host changed rate mid-take
        const auto second = tape.getCurrentFile();
        check(tape.isRecording() && second != first, "tape: a rate change closes the take and rolls a new one");
        feed(1.0, 96000.0);
        tape.stop();
        check(tape.getDroppedSamples() == 0, "tape: no audio dropped");
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> a(formats.createReaderFor(first)), b(formats.createReaderFor(second));
        check(a != nullptr && juce::exactlyEqual(a->sampleRate, c_rate) && std::abs(static_cast<int>(a->lengthInSamples) - static_cast<int>(c_rate)) < 2 * c_block,
              "tape: the first take is 1 s at 48 kHz");
        check(b != nullptr && juce::exactlyEqual(b->sampleRate, 96000.0) && std::abs(static_cast<int>(b->lengthInSamples) - 96000) < 2 * c_block,
              "tape: the second take is 1 s at 96 kHz");

        TapeRecorder quiet;
        quiet.prepare(c_rate, 2);
        block.clear();
        for (int pos = 0; pos < static_cast<int>(2.0 * c_rate); pos += c_block)
            quiet.process(block.getArrayOfReadPointers(), 2, c_block);
        juce::File written;
        check(! quiet.saveRetro(tapeFolder, 24, written, error), "tape: saving a silent retro buffer is refused (" + error + ")");
        TapeRecorder empty;
        empty.prepare(c_rate, 2);
        check(! empty.saveRetro(tapeFolder, 24, written, error), "tape: saving an empty retro buffer is refused");
    }

    // Presets: every sample-track level is a session parameter; Cargo Run still exists.
    {
        bool allTracks = true;
        for (int i = 0; i < ParamIDs::numCargoTracks; ++i)
            allTracks = allTracks && Presets::isSessionParameter(ParamIDs::cargoTrack[i]);
        check(allTracks, "presets: all 6 track levels are session parameters");
        check(! Presets::isSessionParameter(ParamIDs::energy), "presets: the ride is not a session parameter");
        check(! Presets::factory().empty(), "presets: factory presets exist");
    }

    std::cout << (g_failures == 0 ? "ALL PASSED" : juce::String(g_failures) + " FAILED") << std::endl;
    return g_failures == 0 ? 0 : 1;
}
