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
#include "RigEngine.h"
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

    juce::File writeMono(const juce::File& folder, const juce::String& name, const juce::AudioBuffer<float>& buffer,
                         double rate, int bits = 24)
    {
        auto file = folder.getChildFile(name);
        file.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> writer(wav.createWriterFor(new juce::FileOutputStream(file), rate, 1, bits, {}, 0));
        if (writer != nullptr)
            writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
        return file;
    }

    juce::AudioBuffer<float> sines(double rate, double seconds, std::initializer_list<double> freqs, float amp)
    {
        juce::AudioBuffer<float> b(1, static_cast<int>(rate * seconds));
        b.clear();
        float* d = b.getWritePointer(0);
        for (const double f : freqs)
            for (int i = 0; i < b.getNumSamples(); ++i)
                d[i] += amp * static_cast<float>(std::sin(juce::MathConstants<double>::twoPi * f * i / rate));
        return b;
    }

    /** Amplitude of the component at f (Hann-windowed single-bin transform). */
    double toneLevel(const float* x, int n, double f, double rate)
    {
        double re = 0.0, im = 0.0, sum = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double w = 0.5 - 0.5 * std::cos(juce::MathConstants<double>::twoPi * i / (n - 1));
            const double a = juce::MathConstants<double>::twoPi * f * i / rate;
            re += w * x[i] * std::cos(a);
            im += w * x[i] * std::sin(a);
            sum += w;
        }
        return 2.0 * std::sqrt(re * re + im * im) / sum;
    }

    double rmsOf(const float* x, int n)
    {
        double s = 0.0;
        for (int i = 0; i < n; ++i)
            s += static_cast<double>(x[i]) * x[i];
        return std::sqrt(s / juce::jmax(1, n));
    }

    /** One second of a sine track through a CargoPlayer (looping, 48 kHz session) at a steady speed. */
    std::vector<float> playSine(double fileRate, double freq, float rate, const CargoData** dataOut = nullptr)
    {
        static std::vector<std::unique_ptr<CargoData>> keep;
        keep.push_back(std::make_unique<CargoData>());
        auto& d = *keep.back();
        d.sampleRate = fileRate;
        d.audio = sines(fileRate, 6.0, { freq }, 0.5f);
        d.analyse();
        if (dataOut != nullptr)
            *dataOut = &d;
        CargoPlayer player;
        player.prepare(c_rate);
        sj::Random rng;
        std::vector<float> out(static_cast<size_t>(c_rate), 0.0f);
        for (int pos = 0; pos + c_block <= static_cast<int>(out.size()); pos += c_block)
            player.render(&d, CargoRole::Bed, false, rate, 0.0f, rng, 0.1f, CargoCue {}, out.data() + pos, c_block);
        return out;
    }

    double dbOf(double ratio) { return 20.0 * std::log10(juce::jmax(1.0e-12, ratio)); }
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

            // Cmd+Z / Shift+Cmd+Z follow the order of the edits across tracks (remove two, load one, then undo).
            CargoHold order;
            order.loadFile(0, tmp, err);
            order.loadFile(1, tmp, err);
            order.clear(0);
            order.clear(1);
            order.loadFile(2, tmp, err);
            const int u1 = order.undoLatest(), u2 = order.undoLatest(), u3 = order.undoLatest();
            check(u1 == 2 && order.get(2) == nullptr, "undo order: the latest edit (a load on track 3) goes first");
            check(u2 == 1 && order.get(1) != nullptr && u3 == 0 && order.get(0) != nullptr,
                  "undo order: then the removals, newest first, so both removed tracks come back");
            const int r1 = order.redoLatest();
            check(r1 == 0 && order.get(0) == nullptr && order.get(1) != nullptr, "undo order: redo replays the most recently undone edit first");
            order.loadFile(3, tmp, err);
            check(! order.canRedoLatest() && order.redoLatest() == -1, "undo order: a new edit ends the redo chain");
        }
    }


    // Start markers: saved with the session, reset by a new file, and part of undo.
    {
        const auto wavA = writeMono(folder, "marker_a.wav", sines(c_rate, 2.0, { 440.0 }, 0.4f), c_rate);
        juce::String err;
        CargoHold hold;
        hold.loadFile(0, wavA, err);
        hold.loadFile(3, wavA, err);
        hold.setStart(0, 0.4f);
        hold.setStart(3, 0.25f);
        CargoHold back;
        back.fromValueTree(hold.toValueTree());
        check(std::abs(back.getStart(0) - 0.4f) < 1.0e-4f && std::abs(back.getStart(3) - 0.25f) < 1.0e-4f && back.getStart(1) == 0.0f,
              "start marker: survives save/restore (" + juce::String(back.getStart(0), 3) + ", " + juce::String(back.getStart(3), 3) + ")");

        hold.loadFile(0, wavA, err);
        check(hold.getStart(0) == 0.0f && std::abs(hold.getStart(3) - 0.25f) < 1.0e-4f,
              "start marker: a new file starts from its beginning (other tracks keep theirs)");
        hold.undo(0);
        check(std::abs(hold.getStart(0) - 0.4f) < 1.0e-4f, "start marker: undoing the load brings the marker back");
        hold.redo(0);
        check(hold.getStart(0) == 0.0f, "start marker: redo resets it again");
        hold.clear(3);
        const float cleared = hold.getStart(3);
        hold.undo(3);
        check(cleared == 0.0f && std::abs(hold.getStart(3) - 0.25f) < 1.0e-4f, "start marker: Remove resets it and undo restores it");
    }

    // Lifetime: data replaced while a player still reads it is kept until the engine has let go of it.
    {
        const auto wav = writeMono(folder, "lifetime.wav", sines(c_rate, 2.0, { 330.0 }, 0.4f), c_rate);
        juce::String err;
        CargoHold hold;
        hold.loadFile(0, wav, err);
        RigEngine engine;
        engine.setCargoHold(&hold);
        RigEngine::Params p;
        engine.setParams(p);
        engine.prepare(c_rate, c_block);
        engine.setParams(p);
        std::vector<float> l(c_block), r(c_block);
        for (int i = 0; i < 40; ++i)
            engine.process(nullptr, nullptr, l.data(), r.data(), c_block);

        std::weak_ptr<const CargoData> old = hold.get(0);
        hold.fromValueTree(juce::ValueTree("Cargo")); // a new session: no undo step holds the old audio
        // The audio thread is paused (a bypassed plug-in, a stalled device): however old the retired data gets,
        // the engine's player still points at it.
        const int kept = hold.purgeRetired(0);
        check(! old.expired() && kept >= 1, "lifetime: retired data survives while the engine still reads it");
        for (int i = 0; i < 200; ++i)
            engine.process(nullptr, nullptr, l.data(), r.data(), c_block);
        hold.purgeRetired(0);
        check(old.expired(), "lifetime: it is freed once the engine has moved on");
    }

    // Lifetime: an engine that is reset (or whose audio has stopped) lets go of the data it was reading, so it is
    // freed without the engine having to run again.
    {
        const auto wav = writeMono(folder, "lifetime2.wav", sines(c_rate, 2.0, { 330.0 }, 0.4f), c_rate);
        juce::String err;
        CargoHold hold;
        hold.loadFile(0, wav, err);
        RigEngine engine;
        engine.setCargoHold(&hold);
        RigEngine::Params p;
        engine.setParams(p);
        engine.prepare(c_rate, c_block);
        std::vector<float> l(c_block), r(c_block);
        for (int k = 0; k < 2; ++k)
        {
            for (int i = 0; i < 40; ++i)
                engine.process(nullptr, nullptr, l.data(), r.data(), c_block);
            std::weak_ptr<const CargoData> old = hold.get(0);
            hold.fromValueTree(juce::ValueTree("Cargo"));
            hold.purgeRetired(0);
            const bool heldWhileRead = ! old.expired();
            if (k == 0) engine.reset(); else engine.letGoOfCargo();
            hold.purgeRetired(0);
            check(heldWhileRead && old.expired(), k == 0 ? "lifetime: a reset engine lets go of the data it was reading"
                                                         : "lifetime: so does an engine whose audio has stopped");
            hold.loadFile(0, wav, err);
        }
    }

    // Undo steps keep the audio but not the octave copies (nearly as much memory again), once the engine can no
    // longer be reading them; undo builds the copies again. And the undo history is capped by how much it holds.
    {
        const auto wav = writeMono(folder, "undomem.wav", sines(c_rate, 4.0, { 330.0, 5000.0 }, 0.3f), c_rate);
        juce::String err;
        CargoHold hold;
        hold.loadFile(0, wav, err);
        const auto original = hold.get(0);
        const std::vector<float> mip0 = original->mips[0];
        const std::int64_t audioBytes = static_cast<std::int64_t>(original->length()) * 4;

        RigEngine engine;
        engine.setCargoHold(&hold);
        RigEngine::Params p;
        engine.setParams(p);
        engine.prepare(c_rate, c_block);
        std::vector<float> l(c_block), r(c_block);
        for (int i = 0; i < 40; ++i)
            engine.process(nullptr, nullptr, l.data(), r.data(), c_block);

        hold.reverse(0, 0, original->length());
        const std::int64_t withCopies = hold.getUndoBytes();
        hold.purgeRetired(0); // the engine hasn't run since the edit: it still reads the old data
        check(! original->mips[0].empty() && hold.getUndoBytes() == withCopies,
              "undo memory: a step the engine may still be reading keeps its octave copies");
        for (int i = 0; i < 200; ++i)
            engine.process(nullptr, nullptr, l.data(), r.data(), c_block);
        hold.purgeRetired(0);
        const std::int64_t without = hold.getUndoBytes();
        std::cout << "         one undo step of a 4 s sound: " << withCopies / 1024 << " KB with its octave copies, " << without / 1024
                  << " KB once the engine has let go (the audio alone is " << audioBytes / 1024 << " KB)" << std::endl;
        check(original->mips[0].empty() && without == audioBytes && withCopies > audioBytes * 18 / 10,
              "undo memory: once the engine has let go, the step keeps only the audio");
        check(! hold.get(0)->mips[0].empty(), "undo memory: the sound in use keeps its octave copies");

        hold.undo(0);
        check(hold.get(0) == original && original->mips[0] == mip0, "undo memory: undo brings the octave copies back, the same as before");
        for (int i = 0; i < 200; ++i)
            engine.process(nullptr, nullptr, l.data(), r.data(), c_block);
        hold.purgeRetired(0);
        check(! original->mips[0].empty(), "undo memory: they stay while the sound is the one in use");
        hold.redo(0);
        check(! hold.get(0)->mips[0].empty(), "undo memory: redo brings them back too");

        // The cap: about two and a half steps' worth. The oldest edits are forgotten, the newest always stays.
        hold.setUndoByteLimit(audioBytes * 5);
        for (int k = 0; k < 8; ++k)
        {
            hold.reverse(0, 0, hold.get(0)->length());
            hold.purgeRetired(0);
        }
        int steps = 0;
        const std::int64_t capped = hold.getUndoBytes();
        while (hold.canUndoLatest() && steps < 100)
        {
            hold.undoLatest();
            ++steps;
        }
        std::cout << "         undo cap: " << capped / 1024 << " KB held under a " << audioBytes * 5 / 1024 << " KB limit, " << steps << " steps left to undo" << std::endl;
        check(capped <= audioBytes * 5 && steps >= 2 && steps <= 5, "undo memory: the history is capped by size, oldest edits first");
        hold.setUndoByteLimit(1);
        hold.reverse(0, 0, hold.get(0)->length());
        check(hold.canUndoLatest() && hold.undoLatest() == 0, "undo memory: however small the limit, the latest edit can be undone");
    }

    // Band-limited playback: what a faster read pushes over Nyquist is filtered out, not folded back.
    {
        const auto level = [](const std::vector<float>& v) { return rmsOf(v.data() + 9600, static_cast<int>(v.size()) - 9600 - c_block); };

        // Unpitched playback is the file itself, sample for sample (times the auto gain).
        {
            const CargoData* d = nullptr;
            const auto out = playSine(c_rate, 1234.5, 1.0f, &d);
            double worst = 0.0;
            for (int i = 4800; i < static_cast<int>(out.size()) - c_block; ++i) // (the last part block isn't rendered)
                worst = std::max(worst, std::abs(static_cast<double>(out[static_cast<size_t>(i)])
                                                 - static_cast<double>(d->autoGain) * d->audio.getSample(0, i)));
            check(worst < 1.0e-6, "playback: speed 1 is sample exact (worst error " + juce::String(dbOf(worst / 0.5), 1) + " dB)");
        }

        const double ref = level(playSine(c_rate, 1000.0, 1.0f));
        struct Case { double fileRate, freq; float rate; const char* what; };
        const Case folding[] = {
            { 48000.0, 15000.0, 2.0f, "15 kHz up an octave (30 kHz)" },
            { 48000.0, 19000.0, 1.4983f, "19 kHz up 7 semitones (28.5 kHz)" },
            { 48000.0, 10000.0, 2.8284f, "10 kHz up 18 semitones (28.3 kHz)" },
            { 48000.0, 7000.0, 4.0f, "7 kHz up two octaves (28 kHz)" },
            { 48000.0, 13000.0, 4.0f, "13 kHz up two octaves (52 kHz)" },
            { 96000.0, 30000.0, 1.0f, "30 kHz in a 96 kHz file, unpitched" },
            { 96000.0, 30000.0, 4.0f, "30 kHz in a 96 kHz file, up two octaves" },
        };
        for (const auto& c : folding)
        {
            const double db = dbOf(level(playSine(c.fileRate, c.freq, c.rate)) / ref);
            check(db < -60.0, juce::String("playback: no fold-back from ") + c.what + " (" + juce::String(db, 1) + " dB)");
        }
        const Case kept[] = {
            { 48000.0, 5000.0, 2.0f, "5 kHz up an octave" },
            { 48000.0, 8000.0, 1.4983f, "8 kHz up 7 semitones" },
            { 48000.0, 3000.0, 4.0f, "3 kHz up two octaves" },
            { 48000.0, 9000.0, 0.5f, "9 kHz down an octave" },
            { 96000.0, 8000.0, 1.0f, "8 kHz in a 96 kHz file" },
            { 44100.0, 12000.0, 1.0f, "12 kHz in a 44.1 kHz file" },
        };
        for (const auto& c : kept)
        {
            const double db = dbOf(level(playSine(c.fileRate, c.freq, c.rate)) / ref);
            check(std::abs(db) < 0.5, juce::String("playback: keeps ") + c.what + " at its level (" + juce::String(db, 2) + " dB)");
        }

        // The octave copies line up with the audio: where the speed crosses from one to the next, both give the
        // same signal, so the hand-over (a short crossfade as well) cannot click.
        {
            CargoData d;
            d.sampleRate = c_rate;
            d.audio = sines(c_rate, 2.0, { 300.0, 1700.0, 4100.0, 6000.0 }, 0.2f);
            d.analyse();
            check(! d.mips[0].empty() && ! d.mips[3].empty() && CargoPlayer::levelFor(d, 1.0) == 0 && CargoPlayer::levelFor(d, 1.99) == 0
                      && CargoPlayer::levelFor(d, 2.0) == 1 && CargoPlayer::levelFor(d, 5.0) == 2 && CargoPlayer::levelFor(d, 40.0) == 4,
                  "playback: four octave copies, picked by speed");
            for (int lv = 0; lv < 4; ++lv)
            {
                // Tones that are still under the output's Nyquist at the speed where copy lv hands over to lv + 1.
                const double inc = static_cast<double>(2 << lv);
                const double down = 1.0 / static_cast<double>(1 << lv);
                CargoData t;
                t.sampleRate = c_rate;
                t.audio = sines(c_rate, 2.0, { 300.0 * down, 1700.0 * down, 4100.0 * down, 6000.0 * down }, 0.2f);
                t.analyse();
                double diff = 0.0, sum = 0.0;
                for (int i = 0; i < 4000; ++i)
                {
                    const double pos = 2000.37 + i * 20.731;
                    const double a = CargoPlayer::readBandLimited(t, lv, pos, inc);
                    const double b = CargoPlayer::readBandLimited(t, lv + 1, pos, inc);
                    diff += (a - b) * (a - b);
                    sum += a * a;
                }
                const double db = 10.0 * std::log10(juce::jmax(1.0e-20, diff / juce::jmax(1.0e-20, sum)));
                check(db < -50.0, "playback: copies " + juce::String(lv) + " and " + juce::String(lv + 1)
                                                      + " agree where they hand over (" + juce::String(db, 1) + " dB apart)");
            }

            // A pitch sweep through two hand-overs is as smooth as a steady read.
            CargoPlayer player;
            player.prepare(c_rate);
            sj::Random rng;
            std::vector<float> out(static_cast<size_t>(c_rate * 0.5), 0.0f);
            const int chunks = static_cast<int>(out.size()) / 64;
            for (int k = 0; k < chunks; ++k)
            {
                const float rate = 1.5f * std::pow(2.0f, 1.7f * static_cast<float>(k) / static_cast<float>(chunks)); // 1.5 .. 4.9
                player.render(&d, CargoRole::Bed, false, rate, 0.0f, rng, 0.05f, CargoCue {}, out.data() + k * 64, 64);
            }
            bool finite = true;
            float peak = 0.0f;
            for (const float v : out) { finite = finite && std::isfinite(v); peak = std::max(peak, std::abs(v)); }
            check(finite && peak > 0.05f && peak < 1.2f, "playback: a pitch sweep across the octave copies stays clean (peak "
                                                             + juce::String(peak, 3) + ")");
        }
    }

    // A hiss pitched up loses exactly the part that no longer fits under Nyquist, no more and no less: up an octave
    // half of white noise is left (-3 dB and a little for the filter's slope), up two octaves a quarter. Folding
    // back would keep all of it (0 dB); filtering too hard would lose more.
    {
        CargoData d;
        d.sampleRate = c_rate;
        d.audio.setSize(1, static_cast<int>(6.0 * c_rate));
        juce::uint32 seed = 12345u;
        for (int i = 0; i < d.audio.getNumSamples(); ++i)
        {
            seed = seed * 1664525u + 1013904223u;
            d.audio.setSample(0, i, 0.3f * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f));
        }
        d.analyse();
        const auto play = [&d](float rate)
        {
            CargoPlayer player;
            player.prepare(c_rate);
            sj::Random rng;
            std::vector<float> out(static_cast<size_t>(c_rate), 0.0f);
            for (int pos = 0; pos + c_block <= static_cast<int>(out.size()); pos += c_block)
                player.render(&d, CargoRole::Bed, false, rate, 0.0f, rng, 0.1f, CargoCue {}, out.data() + pos, c_block);
            return rmsOf(out.data() + 9600, static_cast<int>(out.size()) - 9600 - c_block);
        };
        const double unity = play(1.0f);
        const double up1 = dbOf(play(2.0f) / unity), up2 = dbOf(play(4.0f) / unity), up19 = dbOf(play(2.9966f) / unity);
        check(up1 < -2.6 && up1 > -4.6, "playback: white noise up an octave keeps the half that fits (" + juce::String(up1, 2) + " dB)");
        check(up19 < -4.4 && up19 > -6.4, "playback: white noise up 19 semitones keeps the third that fits (" + juce::String(up19, 2) + " dB)");
        check(up2 < -5.6 && up2 > -7.6, "playback: white noise up two octaves keeps the quarter that fits (" + juce::String(up2, 2) + " dB)");
    }

    // The band-limited read stays cheap: ten tracks pitched up 21 semitones (the longest kernel in normal use).
    {
        CargoData d;
        d.sampleRate = c_rate;
        d.audio = sines(c_rate, 8.0, { 220.0, 3100.0 }, 0.3f);
        d.analyse();
        std::vector<float> out(c_block);
        sj::Random rng;
        float sum = 0.0f;
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        for (int track = 0; track < 10; ++track)
        {
            CargoPlayer player;
            player.prepare(c_rate);
            for (int pos = 0; pos < static_cast<int>(10.0 * c_rate); pos += c_block)
            {
                player.render(&d, CargoRole::Bed, false, 3.364f, 0.0f, rng, 0.5f, CargoCue {}, out.data(), c_block);
                sum += out[0];
            }
        }
        const double seconds = (juce::Time::getMillisecondCounterHiRes() - t0) * 0.001;
        check(std::isfinite(sum) && seconds < 1.25, "playback: ten pitched tracks x 10 s took " + juce::String(seconds, 3) + " s ("
                                                        + juce::String(static_cast<int>(10.0 / seconds)) + "x real time, need 8x)");
    }

    // Paste between files of different rates goes through the same band-limited kernel.
    {
        juce::String err;
        CargoHold hold;
        hold.loadFile(0, writeMono(folder, "paste_96k.wav", sines(96000.0, 0.5, { 1000.0, 30000.0 }, 0.4f), 96000.0), err);
        hold.loadFile(1, writeMono(folder, "paste_48k.wav", sines(c_rate, 0.5, { 200.0 }, 0.1f), c_rate), err);
        hold.copy(0, 0, hold.get(0)->length());
        hold.paste(1, 0);
        const auto pasted = hold.get(1);
        const int n = 20000;
        const double wanted = toneLevel(pasted->audio.getReadPointer(0) + 2000, n, 1000.0, c_rate);
        const double folded = toneLevel(pasted->audio.getReadPointer(0) + 2000, n, 18000.0, c_rate);
        check(pasted->length() == 24000 + 24000 && std::abs(wanted - 0.4) < 0.01 && dbOf(folded / wanted) < -60.0,
              "paste: a 96 kHz clip pasted into a 48 kHz track keeps its 1 kHz (" + juce::String(wanted, 3) + ") and its 30 kHz does not fold ("
                  + juce::String(dbOf(folded / wanted), 1) + " dB)");
    }

    // Auto gain: levels by RMS, but never lifts a peaky sound over full scale.
    {
        CargoData clicks;
        clicks.sampleRate = c_rate;
        clicks.audio.setSize(1, static_cast<int>(c_rate * 2.0));
        clicks.audio.clear();
        for (int k = 0; k < 4; ++k)
            for (int i = 0; i < 4800; ++i) // a 0.9 spike, then a quiet 100 ms tail
                clicks.audio.setSample(0, 4800 + k * 20000 + i,
                                       (i < 8 ? 0.9f : 0.02f * std::exp(-static_cast<float>(i) / 1500.0f))
                                           * static_cast<float>(std::sin(0.9 * i)));
        clicks.analyse();
        float peak = 0.0f;
        for (const auto& reg : clicks.regions)
            peak = std::max(peak, clicks.audio.getMagnitude(0, reg.start, reg.end - reg.start));
        check(! clicks.regions.empty() && clicks.autoGain > 1.0f && peak * clicks.autoGain <= 1.0005f,
              "auto gain: a peaky sound is lifted to full scale, not past it (peak after gain "
                  + juce::String(peak * clicks.autoGain, 3) + ", gain " + juce::String(clicks.autoGain, 2) + ")");

        CargoData steady;
        steady.sampleRate = c_rate;
        steady.audio = sines(c_rate, 1.0, { 500.0 }, 0.05f);
        steady.analyse();
        const double rms = rmsOf(steady.audio.getReadPointer(0), steady.length());
        check(std::abs(steady.autoGain * rms - 0.22) < 0.01, "auto gain: a steady sound still reaches the RMS target ("
                                                                 + juce::String(steady.autoGain * rms, 3) + ")");
    }

    // Loading: a long file says it was cut short, and a batch accounts for every file.
    {
        juce::String err;
        // 10.5 minutes at 8 kHz (8-bit, so the file stays small): only the first 10 minutes are loaded.
        juce::AudioBuffer<float> longOne(1, static_cast<int>(8000.0 * 630.0));
        for (int i = 0; i < longOne.getNumSamples(); ++i)
            longOne.setSample(0, i, 0.3f * static_cast<float>(std::sin(0.2 * i)));
        const auto longFile = writeMono(folder, "long_file.wav", longOne, 8000.0, 8);
        const auto shortFile = writeMono(folder, "short_file.wav", sines(c_rate, 0.5, { 440.0 }, 0.3f), c_rate);
        CargoHold hold;
        check(hold.loadFile(0, longFile, err) && hold.get(0)->length() == 8000 * 600
                  && hold.getLoadNote(0) == "Loaded the first 10:00 of 10:30",
              "loading: a file over ten minutes is cut and says so (\"" + hold.getLoadNote(0) + "\")");
        hold.loadFile(1, shortFile, err);
        check(hold.getLoadNote(1).isEmpty(), "loading: a file that fits has no note");
        hold.trim(0, 0, 8000 * 60);
        check(hold.getLoadNote(0).isEmpty(), "loading: the note goes once the track is edited");

        auto bogus = folder.getChildFile("x.mp4");
        bogus.replaceWithText("not audio");
        juce::StringArray paths;
        paths.add(bogus.getFullPathName());
        for (int i = 0; i < 12; ++i)
            paths.add(shortFile.getFullPathName());
        CargoHold batch;
        const auto report = batch.loadFiles(paths);
        check(report.tracks.size() == 10 && report.noRoom == 2 && report.failed.size() == 1 && report.failed[0] == "x.mp4"
                  && batch.firstEmptyTrack() == -1 && report.hasProblems(),
              "loading: a batch of 13 adds 10, counts 2 with no track left and 1 unreadable");
        check(report.summary() == "Added 10. 2 didn't fit (ten tracks at most). Couldn't read x.mp4 (try WAV, AIFF, FLAC, OGG or MP3).",
              "loading: the status line says all of it (\"" + report.summary() + "\")");

        CargoHold one;
        juce::StringArray two;
        two.add(longFile.getFullPathName());
        two.add(shortFile.getFullPathName());
        const auto r2 = one.loadFiles(two, 4);
        check(r2.tracks.size() == 2 && r2.tracks[0] == 4 && r2.tracks[1] == 0 && ! r2.hasProblems()
                  && r2.summary().startsWith("Added ") && r2.summary().endsWith("Loaded the first 10:00 of 10:30 (long_file.wav)."),
              "loading: an explicit first track, then the empty ones, with the note for the long file (\"" + r2.summary() + "\")");
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

        error.clear();
        // Skip Silence: whooshes over a bed that never falls silent (1 s loud, 4 s of bed 40 dB under it, four
        // times). The gate measures against the recent peak, so the bed between the whooshes is left out; each
        // whoosh keeps three quarters of a second behind it and (after the first) a quarter second in front.
        auto readAll = [&formats](const juce::File& f, juce::AudioBuffer<float>& into)
        {
            std::unique_ptr<juce::AudioFormatReader> r(formats.createReaderFor(f));
            if (r == nullptr)
                return 0.0;
            into.setSize(static_cast<int>(r->numChannels), static_cast<int>(r->lengthInSamples));
            r->read(&into, 0, static_cast<int>(r->lengthInSamples), 0, true, true);
            return r->sampleRate;
        };
        auto whooshesOverBed = [&](TapeRecorder& t)
        {
            for (int pos = 0; pos < static_cast<int>(20.0 * c_rate); pos += c_block)
            {
                for (int i = 0; i < c_block; ++i)
                {
                    const int at = pos + i;
                    const bool loud = (at % static_cast<int>(5.0 * c_rate)) < static_cast<int>(c_rate);
                    const float v = (loud ? 0.5f : 0.005f) * static_cast<float>(std::sin(0.03 * at));
                    block.setSample(0, i, v);
                    block.setSample(1, i, v);
                }
                t.process(block.getArrayOfReadPointers(), 2, c_block);
                if ((pos / c_block) % 64 == 63)
                    juce::Thread::sleep(2); // let the writer thread keep up
            }
        };
        {
            TapeRecorder gated;
            gated.prepare(c_rate, 2);
            gated.setSignalGate(true);
            check(gated.start(tapeFolder, 24, error), "tape: starts a take with Skip Silence " + error);
            whooshesOverBed(gated);
            gated.stop();
            juce::AudioBuffer<float> audio;
            readAll(gated.getLastFile(), audio);
            const double seconds = audio.getNumSamples() / c_rate;
            check(seconds > 7.0 && seconds < 8.6,
                  "tape: Skip Silence leaves out the bed between whooshes (" + juce::String(seconds, 2) + " s kept of 20 s)");
            // The first join (1.75 s in: 1 s of whoosh and 0.75 s behind it) is faded, not a cut in the middle of the bed.
            float quietest = 1.0f;
            const int window = static_cast<int>(0.002 * c_rate);
            for (int start = static_cast<int>(1.65 * c_rate); start + window < juce::jmin(audio.getNumSamples(), static_cast<int>(1.85 * c_rate)); start += window / 2)
                quietest = std::min(quietest, audio.getMagnitude(0, start, window));
            check(quietest < 0.0006f, "tape: the tape fades out and in around a skipped stretch (level at the join "
                                          + juce::String(juce::Decibels::gainToDecibels(quietest), 1) + " dB, the bed is -46 dB)");

            // Held open (the rig ringing out after Stop), the gate keeps rolling through the quiet.
            TapeRecorder held;
            held.prepare(c_rate, 2);
            held.setSignalGate(true);
            held.setGateHold(true);
            check(held.start(tapeFolder, 24, error), "tape: starts a held take " + error);
            whooshesOverBed(held);
            held.stop();
            readAll(held.getLastFile(), audio);
            check(std::abs(audio.getNumSamples() / c_rate - 20.0) < 0.1, "tape: a held gate keeps the quiet tail ("
                                                                             + juce::String(audio.getNumSamples() / c_rate, 2) + " s of 20 s)");

            // A sound that comes back while the gate is fading out: the tape comes back up from where the fade had
            // got to, without a step. A steady level shows it: 1 s loud, then a bed 34 dB under it, and one loud
            // sample at the end of the 10 ms block that starts two thirds of the way through the 30 ms fade.
            {
                constexpr int c_piece = 480;
                const int lastLoud = static_cast<int>(c_rate) / c_piece - 1;                      // the last loud block
                const int closes = lastLoud * c_piece + static_cast<int>(0.75 * c_rate);          // where the fade would end
                const int back = (closes - static_cast<int>(0.02 * c_rate)) / c_piece * c_piece; // start of the block that reopens
                TapeRecorder reopened;
                reopened.prepare(c_rate, 2);
                reopened.setSignalGate(true);
                TapeRecorder::Options floatFile;
                floatFile.bitDepth = 32;
                check(reopened.start(tapeFolder, floatFile, error), "tape: starts a take for the reopening gate " + error);
                juce::AudioBuffer<float> piece(2, c_piece);
                for (int pos = 0; pos < back + 4 * c_piece; pos += c_piece)
                {
                    for (int i = 0; i < c_piece; ++i)
                    {
                        const int at = pos + i;
                        const float v = (at < static_cast<int>(c_rate) || at == back + c_piece - 1) ? 0.5f : 0.01f;
                        piece.setSample(0, i, v);
                        piece.setSample(1, i, v);
                    }
                    reopened.process(piece.getArrayOfReadPointers(), 2, c_piece);
                }
                reopened.stop();
                readAll(reopened.getLastFile(), audio);
                float biggestStep = 0.0f, lowest = 1.0f;
                for (int i = closes - static_cast<int>(0.03 * c_rate); i < back + c_piece - 2 && i + 1 < audio.getNumSamples(); ++i)
                {
                    biggestStep = std::max(biggestStep, std::abs(audio.getSample(0, i + 1) - audio.getSample(0, i)));
                    lowest = std::min(lowest, audio.getSample(0, i));
                }
                check(audio.getNumSamples() == back + 4 * c_piece && lowest < 0.008f && lowest > 0.001f,
                      "tape: a sound that returns during the fade reopens the gate (the bed had faded to "
                          + juce::String(lowest / 0.01f, 2) + " of its level)");
                check(biggestStep < 3.0f * 0.01f / static_cast<float>(0.03 * c_rate),
                      "tape: the reopened gate comes back up without a step (biggest step in the bed "
                          + juce::String(biggestStep / 0.01f, 4) + " of its level)");
            }

            TapeRecorder continuous;
            continuous.prepare(c_rate, 2);
            continuous.setSignalGate(false);
            check(continuous.start(tapeFolder, 24, error), "tape: starts a continuous take " + error);
            whooshesOverBed(continuous);
            continuous.stop();
            std::unique_ptr<juce::AudioFormatReader> all(formats.createReaderFor(continuous.getLastFile()));
            check(all != nullptr && std::abs(static_cast<double>(all->lengthInSamples) - 20.0 * c_rate) < 2.0 * c_block,
                  "tape: with Skip Silence off the take is continuous (20 s)");
            check(all != nullptr && all->metadataValues[juce::WavAudioFormat::bwavOriginatorRef] == "Samuel Justice",
                  "tape: the file names its author (" + (all != nullptr ? all->metadataValues[juce::WavAudioFormat::bwavOriginatorRef] : juce::String()) + ")");
        }

        // 16-bit takes are dithered: a tone of 0.4 of a 16-bit step survives (without dither the file is flat),
        // and the dither adds no offset.
        {
            TapeRecorder dithered;
            dithered.prepare(c_rate, 2);
            check(dithered.start(tapeFolder, 16, error), "tape: starts a 16-bit take " + error);
            const double amp = 0.4 / 32768.0, w = juce::MathConstants<double>::twoPi * 1000.0 / c_rate;
            for (int pos = 0; pos < static_cast<int>(4.0 * c_rate); pos += c_block)
            {
                for (int i = 0; i < c_block; ++i)
                    for (int ch = 0; ch < 2; ++ch)
                        block.setSample(ch, i, static_cast<float>(amp * std::sin(w * (pos + i))));
                dithered.process(block.getArrayOfReadPointers(), 2, c_block);
                if ((pos / c_block) % 64 == 63)
                    juce::Thread::sleep(2); // let the writer thread keep up
            }
            dithered.stop();
            juce::AudioBuffer<float> audio;
            readAll(dithered.getLastFile(), audio);
            double inPhase = 0.0, mean = 0.0;
            for (int i = 0; i < audio.getNumSamples(); ++i)
            {
                inPhase += audio.getSample(0, i) * std::sin(w * i);
                mean += audio.getSample(0, i);
            }
            const double found = 2.0 * inPhase / juce::jmax(1, audio.getNumSamples()) * 32768.0;
            mean = mean / juce::jmax(1, audio.getNumSamples()) * 32768.0;
            check(dithered.getDroppedSamples() == 0 && found > 0.3 && found < 0.5,
                  "tape: 16-bit dither keeps a tone smaller than one step (" + juce::String(found, 3) + " of a step, 0.4 sent)");
            check(std::abs(mean) < 0.05, "tape: 16-bit dither adds no offset (" + juce::String(mean, 3) + " of a step)");
        }

        // The writer's FIFO is sized by time (3 s at any rate), and lost audio is counted in seconds of the take.
        {
            TapeRecorder fast;
            fast.prepare(192000.0, 2);
            check(fast.getFifoSamples() >= 3 * 192000, "tape: the writer FIFO holds 3 s at 192 kHz (" + juce::String(fast.getFifoSamples()) + " samples)");
            check(fast.start(tapeFolder, 24, error), "tape: starts a 192 kHz take " + error);
            for (int i = 0; i < c_block; ++i)
                for (int ch = 0; ch < 2; ++ch)
                    block.setSample(ch, i, 0.1f * static_cast<float>(std::sin(0.01 * i)));
            // 2.5 s arriving all at once (a stalled disk catching up): it must all fit.
            for (int pos = 0; pos < static_cast<int>(2.5 * 192000.0); pos += c_block)
                fast.process(block.getArrayOfReadPointers(), 2, c_block);
            check(fast.getDroppedSamples() == 0, "tape: 2.5 s of 192 kHz audio in one burst is not dropped ("
                                                     + juce::String(fast.getDroppedSamples()) + " samples lost)");
            // 30 s more at once does overflow: the loss is reported in seconds at the take's own rate.
            for (int pos = 0; pos < static_cast<int>(30.0 * 192000.0); pos += c_block)
                fast.process(block.getArrayOfReadPointers(), 2, c_block);
            const auto dropped = fast.getDroppedSamples();
            check(dropped > 0 && std::abs(fast.getDroppedSeconds() - static_cast<double>(dropped) / 192000.0) < 1.0e-6,
                  "tape: dropped audio is counted in seconds at the take's rate (" + juce::String(fast.getDroppedSeconds(), 2) + " s)");
            fast.stop();
        }

        // LAST 60s is written with the options it is given (file type, bit depth, mono).
        {
            TapeRecorder retro;
            retro.prepare(c_rate, 2);
            for (int pos = 0; pos < static_cast<int>(3.0 * c_rate); pos += c_block)
            {
                for (int i = 0; i < c_block; ++i)
                    for (int ch = 0; ch < 2; ++ch)
                        block.setSample(ch, i, 0.25f * static_cast<float>(std::sin(0.05 * (pos + i))));
                retro.process(block.getArrayOfReadPointers(), 2, c_block);
            }
            TapeRecorder::Options o;
            o.bitDepth = 16;
            o.type = TapeRecorder::FileType::Flac;
            o.mono = true;
            check(retro.saveRetro(tapeFolder, o, written, error), "tape: LAST 60s saves " + error);
            std::unique_ptr<juce::AudioFormatReader> r(formats.createReaderFor(written));
            check(written.hasFileExtension("flac") && r != nullptr && r->bitsPerSample == 16 && r->numChannels == 1,
                  "tape: LAST 60s uses the chosen file type, bit depth and channels");
        }
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
