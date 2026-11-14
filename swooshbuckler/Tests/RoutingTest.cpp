// Routing test: the Master track, choosing a track's bus, and the morph's ropes, through the real engine and the
// real plug-in (no window).
//
//   - Contours: a sound's loudness, pitch and brightness over its length are found as they were made.
//   - Master: one track at a time; the Master follower reads the Master's loudness where it is playing; the Master
//     Pitch and Master Brightness inputs move with the sound; a Follower set to Master takes the value as it is.
//   - Follow Master: the other tracks' rides swell with the Master's loudness and the Master plays unridden.
//   - Match Pitch to Master: lands within a few cents on tones, counts the Master's own tuning, leaves a sound
//     with no pitch alone, and is one undo step.
//   - Bus: a track sent to a bus by hand plays there (and only there), keeps its type and name, and the choice
//     survives undo, save and restore.
//   - Stowaway (its plumbing, in Dry; Sub mode itself is StowawayTest's): a track sent to it reaches the output at
//     unity and feeds neither bus A, bus B nor the hits; throws reach it, from rest too; rumbles go to it by
//     themselves only while its Mode is Sub; the hits duck it in Wake, Fly and Dry; its Mode and knobs reach the
//     engine (modulated), read by the mode, and are saved; Randomise and the dice never change the Mode.
//   - Morph ropes: a roped side morphs that one track and the rest of its bus plays on; with the morph off a
//     roped track sounds exactly as without the rope; changing a rope mid-morph doesn't click; ropes let go when
//     their track is removed, come back with undo, and are saved with the session.
//
// Usage: SwooshbucklerRouting [scratch folder] (a folder in the temp directory if none is given).

#include <JuceHeader.h>
#include "TestClock.h"
#include "PluginProcessor.h"
#include "Parameters.h"
#include "RigEngine.h"
#include "SampleClassifier.h"
#include "Presets.h"
#include "Modulators.h"
#include "TapeRecorder.h"

#include <cmath>
#include <iostream>

namespace
{
    constexpr double c_fs = 48000.0;
    constexpr int c_block = 256;
    constexpr double c_twoPi = juce::MathConstants<double>::twoPi;
    int g_failures = 0;

    void check(bool ok, const juce::String& what)
    {
        std::cout << (ok ? "  [ok]   " : "  [FAIL] ") << what << std::endl;
        if (! ok)
            ++g_failures;
    }

    juce::File writeWav(const juce::File& folder, const juce::String& name, const juce::AudioBuffer<float>& audio, double rate = c_fs)
    {
        const auto file = folder.getChildFile(name + ".wav");
        file.deleteFile();
        juce::WavAudioFormat wav;
        auto* stream = new juce::FileOutputStream(file);
        std::unique_ptr<juce::AudioFormatWriter> writer(wav.createWriterFor(stream, rate, 1, 24, {}, 0));
        writer->writeFromAudioSampleBuffer(audio, 0, audio.getNumSamples());
        return file;
    }

    juce::AudioBuffer<float> tone(double hz, double seconds, float gain = 0.5f, double rate = c_fs)
    {
        juce::AudioBuffer<float> b(1, static_cast<int>(seconds * rate));
        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample(0, i, gain * static_cast<float>(std::sin(c_twoPi * hz * i / rate)));
        return b;
    }

    /** The swell the Master test sounds are given: a raised cosine every 2 s, 30 dB from trough to peak. */
    float swellGain(double seconds)
    {
        const double u = 0.5 - 0.5 * std::cos(c_twoPi * seconds / 2.0);
        return static_cast<float>(std::pow(10.0, (-30.0 + 30.0 * u) / 20.0));
    }

    /** A harmonic tone that swells (see swellGain), for the Master. */
    juce::AudioBuffer<float> swellingTone(double hz, double seconds)
    {
        juce::AudioBuffer<float> b(1, static_cast<int>(seconds * c_fs));
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const double t = i / c_fs;
            double v = 0.0;
            for (int h = 1; h <= 4; ++h)
                v += std::sin(c_twoPi * hz * h * t) / h;
            b.setSample(0, i, 0.4f * swellGain(t) * static_cast<float>(v));
        }
        return b;
    }

    juce::AudioBuffer<float> noise(double seconds, int seed, float smooth = 0.3f)
    {
        juce::AudioBuffer<float> b(1, static_cast<int>(seconds * c_fs));
        juce::Random r(seed);
        float y = 0.0f;
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            y += smooth * ((r.nextFloat() * 2.0f - 1.0f) - y);
            b.setSample(0, i, 0.8f * y);
        }
        return b;
    }

    double correlation(const std::vector<float>& a, const std::vector<float>& b)
    {
        const size_t n = std::min(a.size(), b.size());
        if (n < 2)
            return 0.0;
        double ma = 0.0, mb = 0.0;
        for (size_t i = 0; i < n; ++i) { ma += a[i]; mb += b[i]; }
        ma /= static_cast<double>(n);
        mb /= static_cast<double>(n);
        double sab = 0.0, saa = 0.0, sbb = 0.0;
        for (size_t i = 0; i < n; ++i)
        {
            sab += (a[i] - ma) * (b[i] - mb);
            saa += (a[i] - ma) * (a[i] - ma);
            sbb += (b[i] - mb) * (b[i] - mb);
        }
        return saa > 0.0 && sbb > 0.0 ? sab / std::sqrt(saa * sbb) : 0.0;
    }

    /** The level of the band round `hz` (a sixth of an octave either side), in dB, over from..to of both channels. */
    float bandDb(const juce::AudioBuffer<float>& audio, int from, int to, double hz)
    {
        constexpr int order = 13, size = 1 << order;
        juce::dsp::FFT fft(order);
        std::vector<float> buf(static_cast<size_t>(2 * size));
        double power = 0.0;
        int frames = 0;
        const int lo = static_cast<int>(hz / 1.122 * size / c_fs), hi = static_cast<int>(hz * 1.122 * size / c_fs) + 1;
        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
            for (int start = from; start + size <= to; start += size / 2)
            {
                std::fill(buf.begin(), buf.end(), 0.0f);
                for (int i = 0; i < size; ++i)
                    buf[static_cast<size_t>(i)] = audio.getSample(ch, start + i) * (0.5f - 0.5f * std::cos(static_cast<float>(c_twoPi) * static_cast<float>(i) / static_cast<float>(size)));
                fft.performFrequencyOnlyForwardTransform(buf.data(), true);
                for (int b = lo; b <= hi; ++b)
                    power += static_cast<double>(buf[static_cast<size_t>(b)]) * buf[static_cast<size_t>(b)];
                ++frames;
            }
        // A full-scale sine reads about 0 dB.
        const double scale = static_cast<double>(size) * static_cast<double>(size) * 0.25 * 0.375 / 0.25;
        return static_cast<float>(10.0 * std::log10(power / std::max(1, frames) / scale + 1.0e-20));
    }

    /** A plain rig: steady faders, no colour, no echoes, no hits, so what goes in is what comes out of each bus. */
    RigEngine::Params plainParams()
    {
        RigEngine::Params p;
        p.rideOn = false;
        p.fill = 0.0f;
        p.space = 0.0f;
        p.chaos = 0.0f;
        p.hitAim = false;
        p.gateMode = 3; // Open: the hits path lets everything through
        p.hitDuck = 0.0f;
        for (auto& b : p.buses)
        {
            b.doppler = 0.0f;
            b.bend = 0.0f;
            b.width = 0.0f;
            b.low = b.swirl = b.filter = b.drive = b.duck = b.key = 0.0f;
        }
        return p;
    }

    juce::AudioBuffer<float> render(RigEngine& rig, double seconds, const std::function<void(int)>& beforeBlock = {})
    {
        juce::AudioBuffer<float> out(2, static_cast<int>(seconds * c_fs));
        for (int pos = 0; pos < out.getNumSamples(); pos += c_block)
        {
            if (beforeBlock)
                beforeBlock(pos);
            rig.process(nullptr, nullptr, out.getWritePointer(0, pos), out.getWritePointer(1, pos), std::min(c_block, out.getNumSamples() - pos));
        }
        return out;
    }

    bool allFinite(const juce::AudioBuffer<float>& audio)
    {
        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
            for (int i = 0; i < audio.getNumSamples(); ++i)
                if (! std::isfinite(audio.getSample(ch, i)))
                    return false;
        return true;
    }

    using Proc = SwooshbucklerAudioProcessor;
    struct Plug
    {
        std::unique_ptr<juce::AudioProcessor> base { createPluginFilter() };
        Proc& proc { *dynamic_cast<Proc*>(base.get()) };
        Plug() { proc.setPlayConfigDetails(0, 2, c_fs, 512); }
        float plain(const juce::String& id) const
        {
            auto* p = proc.getState().getParameter(id);
            return p->convertFrom0to1(p->getValue());
        }
        void set(const juce::String& id, float value)
        {
            auto* p = proc.getState().getParameter(id);
            p->beginChangeGesture();
            p->setValueNotifyingHost(p->convertTo0to1(value));
            p->endChangeGesture();
        }
        bool load(int track, const juce::File& file)
        {
            juce::String error;
            return proc.getCargo().loadFile(track, file, error);
        }
    };
}

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init;
    const juce::File folder = argc > 1 ? juce::File(argv[1])
                                       : juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("SwooshbucklerRoutingTest");
    folder.createDirectory();
    juce::String error;

    //==========================================================================
    std::cout << "Contours" << std::endl;
    {
        // Loudness: the swell is found as it was made (30 dB of it, inside the contour's 34 dB).
        const auto swell = swellingTone(220.0, 4.0);
        const auto regions = CargoData::detectRegions(swell.getReadPointer(0), swell.getNumSamples(), c_fs);
        const auto c = SampleClassifier::extractContours(swell.getReadPointer(0), swell.getNumSamples(), c_fs, regions);
        check(c.hop == 480 && c.size() == 400, "contours: one value every 10 ms (" + juce::String(c.size()) + " for 4 s)");
        double worst = 0.0;
        for (int f = 20; f < c.size() - 20; ++f)
        {
            const double t = (f + 0.5) * c.hop / c_fs;
            const double expected = 1.0 + 20.0 * std::log10(swellGain(t)) / 34.0;
            worst = std::max(worst, std::abs(expected - static_cast<double>(c.level[static_cast<size_t>(f)])));
        }
        std::cout << "  loudness contour: worst error " << worst << " of its 0..1" << std::endl;
        check(worst < 0.03, "contours: the loudness follows the sound's swell (within 1 dB)");

        const double cents = 1200.0 * std::log2(static_cast<double>(c.medianPitchHz) / 220.0);
        std::cout << "  median pitch " << c.medianPitchHz << " Hz (" << cents << " cents from 220), pitched " << c.voiced << std::endl;
        check(std::abs(cents) < 3.0 && c.voiced > 0.8f, "contours: a 220 Hz tone's pitch is found within 3 cents");

        // Pitch at other heights and file rates.
        struct Case { double hz, rate; };
        double worstCents = 0.0;
        for (const auto& k : { Case { 82.4, 48000.0 }, Case { 440.0, 44100.0 }, Case { 987.8, 96000.0 }, Case { 1567.0, 48000.0 } })
        {
            const auto t = tone(k.hz, 2.0, 0.5f, k.rate);
            const auto r = CargoData::detectRegions(t.getReadPointer(0), t.getNumSamples(), k.rate);
            const auto pc = SampleClassifier::extractContours(t.getReadPointer(0), t.getNumSamples(), k.rate, r);
            const double off = pc.medianPitchHz > 0.0f ? 1200.0 * std::log2(static_cast<double>(pc.medianPitchHz) / k.hz) : 9999.0;
            std::cout << "  " << k.hz << " Hz at " << k.rate << ": " << pc.medianPitchHz << " Hz (" << off << " cents)" << std::endl;
            worstCents = std::max(worstCents, std::abs(off));
        }
        check(worstCents < 5.0, "contours: tones from 82 Hz to 1.5 kHz at 44.1, 48 and 96 kHz are found within 5 cents");

        // A glide up: the pitch contour rises from its bottom to its top. Noise that opens up: so does the brightness.
        juce::AudioBuffer<float> glide(1, static_cast<int>(3.0 * c_fs));
        double phase = 0.0;
        for (int i = 0; i < glide.getNumSamples(); ++i)
        {
            phase += 200.0 * std::pow(2.0, i / (3.0 * c_fs)) / c_fs;
            glide.setSample(0, i, 0.5f * static_cast<float>(std::sin(c_twoPi * phase)));
        }
        const auto gc = SampleClassifier::extractContours(glide.getReadPointer(0), glide.getNumSamples(), c_fs,
                                                         CargoData::detectRegions(glide.getReadPointer(0), glide.getNumSamples(), c_fs));
        const float early = gc.pitch[30], mid = gc.pitch[150], late = gc.pitch[270];
        std::cout << "  glide: pitch contour " << early << " -> " << mid << " -> " << late << " (" << gc.pitchLowHz << " .. " << gc.pitchHighHz << " Hz)" << std::endl;
        check(early < 0.1f && std::abs(mid - 0.5f) < 0.1f && late > 0.9f, "contours: a glide's pitch contour rises across its range");

        juce::AudioBuffer<float> opening(1, static_cast<int>(3.0 * c_fs));
        {
            juce::Random r(21);
            float y = 0.0f;
            for (int i = 0; i < opening.getNumSamples(); ++i)
            {
                const float a = 0.01f + 0.5f * static_cast<float>(i) / static_cast<float>(opening.getNumSamples());
                y += a * ((r.nextFloat() * 2.0f - 1.0f) - y);
                opening.setSample(0, i, y / std::sqrt(a));
            }
        }
        const auto oc = SampleClassifier::extractContours(opening.getReadPointer(0), opening.getNumSamples(), c_fs,
                                                         CargoData::detectRegions(opening.getReadPointer(0), opening.getNumSamples(), c_fs));
        auto meanOf = [](const std::vector<float>& v, int from, int to)
        {
            double s = 0.0;
            for (int i = from; i < to; ++i) s += v[static_cast<size_t>(i)];
            return static_cast<float>(s / std::max(1, to - from));
        };
        const float dull = meanOf(oc.bright, 5, 45), bright = meanOf(oc.bright, 250, 290);
        std::cout << "  opening noise: brightness " << dull << " -> " << bright << " (" << oc.brightLowHz << " .. " << oc.brightHighHz << " Hz), pitched " << oc.voiced << std::endl;
        check(dull < 0.2f && bright > 0.8f, "contours: noise that opens up gets brighter across its range");
        check(oc.voiced < 0.2f, "contours: noise has no clear pitch");

        // Speed: a minute of sound (it runs on the message thread when a track is made the Master).
        {
            const auto minute = swellingTone(220.0, 60.0);
            const auto r = CargoData::detectRegions(minute.getReadPointer(0), minute.getNumSamples(), c_fs);
            const double started = TestClock::cpuMs();
            const auto mc = SampleClassifier::extractContours(minute.getReadPointer(0), minute.getNumSamples(), c_fs, r);
            const double ms = TestClock::cpuMs() - started;
            std::cout << "  contours of 60 s of sound: " << ms << " ms (" << mc.size() << " values)" << std::endl;
            check(ms < 3000.0, "contours: a minute of sound takes well under 3 seconds");
        }

        const auto silent = SampleClassifier::extractContours(nullptr, 0, c_fs, {});
        juce::AudioBuffer<float> zeros(1, 4800);
        zeros.clear();
        const auto quiet = SampleClassifier::extractContours(zeros.getReadPointer(0), 4800, c_fs, {});
        check(silent.size() == 0 && quiet.size() == 10 && quiet.level[5] <= 0.0f && quiet.medianPitchHz <= 0.0f, "contours: nothing and silence give empty, flat contours");
    }

    //==========================================================================
    std::cout << "Master track" << std::endl;
    const auto masterFile = writeWav(folder, "routing_master", swellingTone(220.0, 4.0));
    const auto bedFile = writeWav(folder, "routing_bed", noise(5.0, 5));
    {
        CargoHold hold;
        hold.loadFile(0, masterFile, error);
        hold.loadFile(1, bedFile, error);
        hold.setSkipSilence(0, false); // the whole file loops, so where it is playing is known
        hold.setSkipSilence(1, false);
        check(hold.getMaster() == -1, "master: none to begin with");
        hold.setMaster(5);
        check(hold.getMaster() == -1, "master: an empty track can't be it");
        hold.setMaster(1);
        hold.setMaster(0);
        check(hold.getMaster() == 0, "master: one at a time (choosing another replaces it)");
        check(hold.getLive(0)->getContours() != nullptr && hold.getLive(1)->getContours() != nullptr, "master: its contours are ready when it is chosen");

        RigEngine rig;
        rig.setCargoHold(&hold);
        auto p = plainParams();
        rig.setParams(p);
        rig.prepare(c_fs, c_block);
        rig.setParams(p);

        // The whole file plays from its start at its own speed: at time t the Master is at t.
        std::vector<float> followed, expected, heard, pitchSeen, brightSeen;
        const auto out = render(rig, 3.6, [&](int pos)
        {
            if (pos < c_block)
                return;
            const double t = (pos - 0.5 * c_block) / c_fs; // the middle of the block just rendered
            if (t > 0.3 && t < 3.3)
            {
                followed.push_back(rig.getMasterFollow()[0]);
                expected.push_back(juce::jlimit(0.0f, 1.0f, 1.0f + 20.0f * std::log10(swellGain(t)) / 34.0f));
                pitchSeen.push_back(rig.getMasterFollow()[1]);
                brightSeen.push_back(rig.getMasterFollow()[2]);
            }
        });
        for (int pos = c_block; pos + c_block <= out.getNumSamples(); pos += c_block)
        {
            const double t = (pos - 0.5 * c_block) / c_fs;
            if (t > 0.3 && t < 3.3)
                heard.push_back(juce::Decibels::gainToDecibels(out.getRMSLevel(0, pos - c_block, c_block) + 1.0e-6f));
        }
        float worst = 0.0f;
        for (size_t i = 0; i < followed.size(); ++i)
            worst = std::max(worst, std::abs(followed[i] - expected[i]));
        const double corr = correlation(followed, heard);
        std::cout << "  master follower against the swell: worst error " << worst << ", correlation with the output level " << corr << std::endl;
        check(! followed.empty() && worst < 0.05f, "master: the follower reads the Master's loudness where it is playing");
        check(corr > 0.9, "master: the follower moves with what is heard");

        // Stopped: the loudness rests. No Master: everything rests.
        rig.setReleased(true);
        render(rig, 0.3);
        check(rig.getMasterFollow()[0] <= 0.0f, "master: its loudness rests while the rig is stopped");
        rig.setReleased(false);
        render(rig, 0.3);
        check(rig.getMasterFollow()[0] > 0.0f, "master: and comes back with Play");
        hold.setMaster(-1);
        render(rig, 0.1);
        check(rig.getMasterFollow()[0] <= 0.0f && rig.getMasterFollow()[1] <= 0.0f && rig.getMasterFollow()[2] <= 0.0f, "master: with no Master the three inputs rest");

        // An edit to the Master: the new sound has its contours before it plays.
        hold.setMaster(0);
        hold.reverse(0, 0, hold.get(0)->length());
        check(hold.getLive(0)->getContours() != nullptr, "master: an edited Master has its contours at once");
        hold.undo(0);
        check(hold.getLive(0)->getContours() != nullptr, "master: and so has the sound an undo brings back");

        // Removing the Master's lane: nothing leads; undo brings the lane back as the Master.
        hold.clear(0);
        check(hold.getMaster() == -1, "master: removing its track leaves no Master");
        hold.undo(0);
        check(hold.getMaster() == 0, "master: undo brings the track back as the Master");
        // A new sound dropped on the lane: it is still the Master.
        hold.loadFile(0, bedFile, error);
        check(hold.getMaster() == 0 && hold.getLive(0)->getContours() != nullptr, "master: a new sound on the Master's lane leads in its place");
    }

    // Master Pitch and Master Brightness move with the sound, and a Follower takes the Master inputs as they are.
    {
        juce::AudioBuffer<float> glide(1, static_cast<int>(4.0 * c_fs));
        double phase = 0.0;
        for (int i = 0; i < glide.getNumSamples(); ++i)
        {
            const double u = static_cast<double>(i) / glide.getNumSamples();
            phase += 150.0 * std::pow(2.0, u) / c_fs;
            // Brighter as it rises: more of the upper harmonics.
            const double v = std::sin(c_twoPi * phase) + u * (0.7 * std::sin(c_twoPi * 3.0 * phase) + 0.6 * std::sin(c_twoPi * 5.0 * phase));
            glide.setSample(0, i, 0.25f * static_cast<float>(v));
        }
        CargoHold hold;
        hold.loadFile(0, writeWav(folder, "routing_glide", glide), error);
        hold.setSkipSilence(0, false);
        hold.setMaster(0);
        RigEngine rig;
        rig.setCargoHold(&hold);
        const auto p = plainParams();
        rig.setParams(p);
        rig.prepare(c_fs, c_block);
        rig.setParams(p);
        std::array<float, 3> early {}, late {};
        render(rig, 3.6, [&](int pos)
        {
            if (pos == static_cast<int>(0.5 * c_fs) / c_block * c_block) early = rig.getMasterFollow();
            if (pos == static_cast<int>(3.4 * c_fs) / c_block * c_block) late = rig.getMasterFollow();
        });
        std::cout << "  rising sound: Master Pitch " << early[1] << " -> " << late[1] << ", Master Brightness " << early[2] << " -> " << late[2] << std::endl;
        check(early[1] < 0.25f && late[1] > 0.75f, "master: Master Pitch rises with the sound's pitch");
        check(early[2] < 0.3f && late[2] > 0.7f, "master: Master Brightness rises as the sound gets brighter");

        ModMatrix mods;
        mods.prepare(c_fs);
        std::array<ModMatrix::SourceSettings, ModMatrix::c_numSources> set {};
        set[0] = { ModMatrix::Type::Follower, ModMatrix::Shape::Sine, 1.0f, ModMatrix::FollowMaster };
        set[1] = { ModMatrix::Type::Follower, ModMatrix::Shape::Sine, 1.0f, ModMatrix::FollowMasterPitch };
        set[2] = { ModMatrix::Type::Follower, ModMatrix::Shape::Sine, 1.0f, ModMatrix::FollowMasterBright };
        set[3] = { ModMatrix::Type::Follower, ModMatrix::Shape::Sine, 1.0f, ModMatrix::FollowOutput };
        ModMatrix::FollowerInputs in {};
        in[ModMatrix::FollowMaster] = 0.3f;
        in[ModMatrix::FollowMasterPitch] = 0.6f;
        in[ModMatrix::FollowMasterBright] = 0.9f;
        in[ModMatrix::FollowOutput] = 0.3f; // an RMS: -10.5 dBFS, most of the way up the follower's range
        for (int k = 0; k < 400; ++k)
            mods.process(set, 256, in);
        std::cout << "  followers: Master " << mods.getSourceValue(0) << ", Master Pitch " << mods.getSourceValue(1)
                  << ", Master Brightness " << mods.getSourceValue(2) << ", Output at the same number " << mods.getSourceValue(3) << std::endl;
        check(std::abs(mods.getSourceValue(0) - 0.3f) < 0.01f && std::abs(mods.getSourceValue(1) - 0.6f) < 0.01f
                  && std::abs(mods.getSourceValue(2) - 0.9f) < 0.01f,
              "master: a Follower set to a Master input takes the contour as it is");
        check(mods.getSourceValue(3) > 0.8f, "master: the loudness inputs still read in dB");
        check(ModMatrix::followerNames().size() == ModMatrix::c_numFollowerInputs, "master: every Follower input has a name");
    }

    //==========================================================================
    std::cout << "Follow Master" << std::endl;
    {
        CargoHold hold;
        hold.loadFile(0, masterFile, error);
        hold.loadFile(1, bedFile, error);
        hold.setSkipSilence(0, false);
        hold.setSkipSilence(1, false);
        hold.setMaster(0);

        auto run = [&](bool follow, float energy, std::vector<float>& ride, std::vector<float>& masterRide, std::vector<float>& swell, float& rms)
        {
            RigEngine rig;
            rig.setCargoHold(&hold);
            auto p = plainParams();
            p.rideOn = true;
            p.energy = energy;
            p.followMaster = follow;
            rig.setParams(p);
            rig.prepare(c_fs, c_block);
            rig.setParams(p);
            const auto out = render(rig, 3.6, [&](int pos)
            {
                const double t = (pos - 0.5 * c_block) / c_fs;
                if (t > 0.3 && t < 3.3)
                {
                    ride.push_back(rig.getRide(1));
                    masterRide.push_back(rig.getRide(0));
                    swell.push_back(juce::jlimit(0.0f, 1.0f, 1.0f + 20.0f * std::log10(swellGain(t)) / 34.0f));
                }
            });
            rms = out.getRMSLevel(0, static_cast<int>(0.5 * c_fs), static_cast<int>(3.0 * c_fs));
            return allFinite(out);
        };
        std::vector<float> ride, own, swell, rideOff, ownOff, swellOff;
        float rms = 0.0f, rmsOff = 0.0f;
        const bool fine = run(true, 1.0f, ride, own, swell, rms);
        run(false, 1.0f, rideOff, ownOff, swellOff, rmsOff);
        const double corr = correlation(ride, swell), corrOff = correlation(rideOff, swellOff);
        const float low = *std::min_element(ride.begin(), ride.end()), high = *std::max_element(ride.begin(), ride.end());
        const float ownLow = *std::min_element(own.begin(), own.end());
        std::cout << "  ride of the other track against the Master's swell: correlation " << corr << " (" << corrOff << " with Follow Master off), "
                  << low << " .. " << high << "; the Master's own ride never under " << ownLow << std::endl;
        check(fine && corr > 0.9, "follow master: the other track's ride swells with the Master's loudness");
        check(corrOff < 0.6, "follow master: off, the ride is its own LFO");
        check(low < 0.1f && high > 0.9f, "follow master: at full Energy the ride goes from silence to the fader");
        check(ownLow > 0.99f, "follow master: the Master itself plays unridden");

        // Energy still sets how deep the ride goes; with no Master the option does nothing.
        std::vector<float> shallow, o2, s2;
        float r2 = 0.0f;
        run(true, 0.3f, shallow, o2, s2, r2);
        const float shallowLow = *std::min_element(shallow.begin(), shallow.end());
        check(shallowLow > low + 0.2f, "follow master: Energy sets how deep the ride dips (" + juce::String(shallowLow, 2) + " at 30%)");
        hold.setMaster(-1);
        std::vector<float> none, o3, s3;
        float r3 = 0.0f;
        run(true, 1.0f, none, o3, s3, r3);
        check(none == rideOff, "follow master: with no Master the rides are their own LFOs, exactly as with it off");
    }

    //==========================================================================
    std::cout << "Match Pitch to Master" << std::endl;
    {
        Plug plug;
        auto& cargo = plug.proc.getCargo();
        plug.load(0, writeWav(folder, "routing_tone220", swellingTone(220.0, 3.0)));
        plug.load(1, writeWav(folder, "routing_tone300", tone(300.0, 3.0)));
        plug.load(2, bedFile);
        plug.load(3, writeWav(folder, "routing_tone1000", tone(1000.0, 2.0)));

        bool done = true;
        auto line = plug.proc.matchPitchToMaster(1, &done);
        check(! done && line.containsIgnoreCase("Master"), "match pitch: asks for a Master first (\"" + line + "\")");

        plug.proc.setMasterTrack(0);
        line = plug.proc.matchPitchToMaster(1, &done);
        const double wanted = 12.0 * std::log2(220.0 / 300.0);
        const double got = plug.plain(ParamIDs::trackSemis[1]) + plug.plain(ParamIDs::trackCents[1]) / 100.0;
        std::cout << "  300 Hz to a 220 Hz Master: " << plug.plain(ParamIDs::trackSemis[1]) << " st " << plug.plain(ParamIDs::trackCents[1])
                  << " c (wanted " << wanted << " st). \"" << line << "\"" << std::endl;
        check(done && std::abs(got - wanted) < 0.03, "match pitch: a 300 Hz tone lands within 3 cents of the 220 Hz Master");

        juce::String what;
        plug.proc.undoEdit(&what);
        check(std::abs(plug.plain(ParamIDs::trackSemis[1])) < 0.01f && std::abs(plug.plain(ParamIDs::trackCents[1])) < 0.01f && what == "Match Pitch",
              "match pitch: one undo step takes Semitones and Cents back (\"" + what + "\")");

        // The Master's own tuning counts: tuned up 2 st and 30 cents, the matched track goes up with it.
        plug.set(ParamIDs::trackSemis[0], 2.0f);
        plug.set(ParamIDs::trackCents[0], 30.0f);
        plug.proc.matchPitchToMaster(1, &done);
        const double got2 = plug.plain(ParamIDs::trackSemis[1]) + plug.plain(ParamIDs::trackCents[1]) / 100.0;
        check(done && std::abs(got2 - (wanted + 2.3)) < 0.03, "match pitch: the Master's own Semitones and Cents count (" + juce::String(got2, 2) + " st)");
        plug.set(ParamIDs::trackSemis[0], 0.0f);
        plug.set(ParamIDs::trackCents[0], 0.0f);

        // More than two octaves away: the nearest octave in reach. 1 kHz down to 220 Hz is -26.2 st: -14.2 is used.
        plug.proc.matchPitchToMaster(3, &done);
        const double got3 = plug.plain(ParamIDs::trackSemis[3]) + plug.plain(ParamIDs::trackCents[3]) / 100.0;
        const double wanted3 = 12.0 * std::log2(220.0 / 1000.0) + 12.0;
        check(done && std::abs(got3 - wanted3) < 0.05, "match pitch: beyond two octaves it uses the octave in reach (" + juce::String(got3, 2) + " st)");

        // Noise has no pitch: it is left alone, and so is the Master itself.
        line = plug.proc.matchPitchToMaster(2, &done);
        check(! done && std::abs(plug.plain(ParamIDs::trackSemis[2])) < 0.01f, "match pitch: a sound with no clear pitch is left alone (\"" + line + "\")");
        line = plug.proc.matchPitchToMaster(0, &done);
        check(! done, "match pitch: the Master isn't matched to itself (\"" + line + "\")");

        // Heard: the matched tone, played by the rig, sits on the Master's pitch.
        plug.proc.setMasterTrack(0);
        plug.proc.matchPitchToMaster(1, &done);
        CargoHold& hold = cargo;
        hold.setSkipSilence(1, false);
        for (int t : { 0, 2, 3 })
            hold.setMute(CargoHold::c_numBuiltIns + t, true);
        RigEngine rig;
        rig.setCargoHold(&hold);
        auto p = plainParams();
        p.trackPitch[1] = plug.plain(ParamIDs::trackSemis[1]) + plug.plain(ParamIDs::trackCents[1]) / 100.0f;
        rig.setParams(p);
        rig.prepare(c_fs, c_block);
        rig.setParams(p);
        const auto out = render(rig, 2.5);
        const int from = static_cast<int>(1.0 * c_fs), len = static_cast<int>(1.0 * c_fs);
        const auto heard = SampleClassifier::extractContours(out.getReadPointer(0, from), len, c_fs, {});
        const double off = heard.medianPitchHz > 0.0f ? 1200.0 * std::log2(static_cast<double>(heard.medianPitchHz) / 220.0) : 9999.0;
        std::cout << "  the matched track as the rig plays it: " << heard.medianPitchHz << " Hz (" << off << " cents from the Master)" << std::endl;
        check(std::abs(off) < 5.0, "match pitch: the matched track is heard within 5 cents of the Master");
    }

    //==========================================================================
    std::cout << "Choosing the bus" << std::endl;
    {
        CargoHold hold;
        hold.loadFile(0, writeWav(folder, "routing_tone1k", tone(1000.0, 3.0)), error);
        hold.setSkipSilence(0, false);
        hold.setRoleOverride(0, static_cast<int>(CargoRole::Tonal));
        const auto name = hold.getDisplayName(0);
        check(hold.getBus(0) == 0 && hold.getBusOverride(0) == -1, "bus: Auto follows the type (a tonal sound plays on bus A)");

        auto sections = [&](int bus)
        {
            hold.setBusOverride(0, bus);
            RigEngine rig;
            rig.setCargoHold(&hold);
            const auto p = plainParams();
            rig.setParams(p);
            rig.prepare(c_fs, c_block);
            rig.setParams(p);
            render(rig, 0.5);
            rig.takeSectionLevels();
            render(rig, 1.5);
            return rig.takeSectionLevels();
        };
        const auto onAuto = sections(-1), onA = sections(0), onB = sections(1), onHits = sections(CargoHold::c_hitsBus);
        auto say = [](const std::array<float, RigEngine::c_sections>& s) { return juce::String(s[0], 4) + " / " + juce::String(s[1], 4) + " / " + juce::String(s[2], 4); };
        std::cout << "  A / B / hits levels: Auto " << say(onAuto) << ", Bus A " << say(onA) << ", Bus B " << say(onB) << ", Hits " << say(onHits) << std::endl;
        check(onAuto[0] > 0.01f && onAuto[1] < 1.0e-5f && onAuto[2] < 1.0e-5f, "bus: Auto plays it on bus A only");
        check(onA[0] > 0.01f && onA[1] < 1.0e-5f && onA[2] < 1.0e-5f, "bus: Bus A plays it on bus A only");
        check(onB[1] > 0.01f && onB[0] < 1.0e-5f && onB[2] < 1.0e-5f, "bus: Bus B plays it on bus B only");
        check(onHits[2] > 0.01f && onHits[0] < 1.0e-5f && onHits[1] < 1.0e-5f, "bus: Hits plays it through the hits only");
        hold.setBusOverride(0, 1);
        check(hold.getBus(0) == 1 && hold.getRole(0) == CargoRole::Tonal && hold.getDisplayName(0) == name && hold.getRoleOverride(0) == static_cast<int>(CargoRole::Tonal),
              "bus: the type and the name stay as they were (\"" + name + "\")");
        hold.setRoleOverride(0, static_cast<int>(CargoRole::Impact));
        check(hold.getBus(0) == 1, "bus: a bus chosen by hand stays when the type changes");
        hold.setBusOverride(0, 7);
        check(hold.getBusOverride(0) == -1 && hold.getBus(0) == CargoHold::c_hitsBus, "bus: Auto again follows the type (an impact plays through the hits)");

        // Saved with the session, per track; a session without it is Auto.
        hold.setBusOverride(0, 1);
        hold.loadFile(3, bedFile, error);
        hold.setBusOverride(3, CargoHold::c_hitsBus);
        const auto tree = hold.toValueTree();
        CargoHold back;
        back.fromValueTree(tree);
        check(back.getBusOverride(0) == 1 && back.getBusOverride(3) == CargoHold::c_hitsBus && back.getBusOverride(1) == -1 && back.getBus(0) == 1,
              "bus: the chosen buses survive save and restore");
        auto old = tree.createCopy();
        for (auto node : old)
            node.removeProperty("busOverride", nullptr);
        back.fromValueTree(old);
        check(back.getBusOverride(0) == -1 && back.getBusOverride(3) == -1, "bus: a session saved without them opens on Auto");

        // A new sound on the lane starts on Auto; removing the lane and undoing brings the choice back.
        hold.clear(3);
        hold.undo(3);
        check(hold.getBusOverride(3) == CargoHold::c_hitsBus, "bus: undoing a removed track brings its bus back");
        hold.loadFile(3, masterFile, error);
        check(hold.getBusOverride(3) == -1, "bus: a new sound on the lane starts on Auto");
    }
    {
        // Through the plug-in: an undo step, and the whole state.
        Plug plug;
        plug.load(0, masterFile);
        plug.load(1, bedFile);
        plug.proc.setTrackBus(1, 0);
        plug.proc.setMasterTrack(0);
        plug.proc.setMorphSource(0, 1);
        plug.set(ParamIDs::rideFollowMaster, 1.0f);
        check(plug.proc.getCargo().getBusOverride(1) == 0, "bus: set through the plug-in");
        juce::MemoryBlock state;
        plug.proc.getStateInformation(state);

        Plug other;
        other.proc.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        auto& c = other.proc.getCargo();
        check(c.getBusOverride(1) == 0 && c.getBusOverride(0) == -1, "bus: the plug-in's state brings the chosen bus back");
        check(c.getMaster() == 0 && c.getLive(0) != nullptr && c.getLive(0)->getContours() != nullptr, "master: the plug-in's state brings the Master back, contours ready");
        check(c.getMorphSource(0) == 1 && c.getMorphSource(1) == -1, "ropes: the plug-in's state brings the ropes back");
        check(other.plain(ParamIDs::rideFollowMaster) > 0.5f, "follow master: saved with the session");

        juce::String what;
        plug.proc.undoEdit(&what); // Follow Master
        plug.proc.undoEdit(&what);
        check(what == "Morph Rope" && plug.proc.getCargo().getMorphSource(0) == -1, "ropes: a rope is one undo step");
        plug.proc.undoEdit(&what);
        check(what == "Master Track" && plug.proc.getCargo().getMaster() == -1, "master: choosing the Master is one undo step");
        plug.proc.undoEdit(&what);
        check(what == "Track Bus" && plug.proc.getCargo().getBusOverride(1) == -1, "bus: choosing a bus is one undo step");
        plug.proc.redoEdit(&what);
        check(plug.proc.getCargo().getBusOverride(1) == 0, "bus: and redo puts it back");

        // Follow Master is the session's: a preset or Randomise leaves it alone, and it sits at the end of the host's list.
        check(Presets::isSessionParameter(ParamIDs::rideFollowMaster), "follow master: belongs to the session, not to presets");
        const auto& params = plug.proc.getParameters();
        auto* last = dynamic_cast<juce::AudioProcessorParameterWithID*>(params.getLast());
        check(last != nullptr && last->paramID == ParamIDs::rideFollowMaster, "follow master: the newest parameter is the last in the host's list");
    }

    //==========================================================================
    std::cout << "Stowaway" << std::endl;
    {
        // A 1 kHz tone on track 1, steady faders, nothing else in the way.
        CargoHold hold;
        const auto toneFile = writeWav(folder, "routing_stow_tone1k", tone(1000.0, 3.0));
        hold.loadFile(0, toneFile, error);
        hold.setSkipSilence(0, false);
        hold.setRoleOverride(0, static_cast<int>(CargoRole::Tonal));
        auto plain = []
        {
            auto p = plainParams();
            p.hitBoost = 0.0f; // the hits at unity too: the reference for "straight through"
            p.stow.mode = RigEngine::StowDry; // the tracks as they are (a Sub keeps only their low end: see StowawayTest)
            return p;
        };
        struct Run { std::array<float, RigEngine::c_sections> levels; juce::AudioBuffer<float> out; float scopePeak = 0.0f; float view = 0.0f; };
        auto run = [&](int bus, const RigEngine::Params& p)
        {
            hold.setBusOverride(0, bus);
            RigEngine rig;
            rig.setCargoHold(&hold);
            rig.setParams(p);
            rig.prepare(c_fs, c_block);
            rig.setParams(p);
            render(rig, 0.5);
            rig.takeSectionLevels();
            Run r;
            r.out = render(rig, 1.5);
            r.levels = rig.takeSectionLevels();
            std::array<float, ScopeRing::c_points> lo {}, hi {};
            rig.getScope(RigEngine::SectionStow).read(lo, hi);
            for (const float v : hi)
                r.scopePeak = std::max(r.scopePeak, v);
            r.view = rig.getStowLevel();
            return r;
        };
        check(CargoHold::c_hitsBus == 2 && CargoHold::c_stowBus == 3 && CargoHold::c_maxBuses == 2 && RigEngine::c_maxBuses == 2,
              "stowaway: its own place beside the buses (the hits keep theirs, and there are still two rig buses)");
        const auto onStow = run(CargoHold::c_stowBus, plain());
        const auto onHits = run(CargoHold::c_hitsBus, plain());
        const auto onA = run(0, plain());
        const auto& s = onStow.levels;
        std::cout << "  on Stowaway, A / B / hits / Stowaway levels: " << s[0] << " / " << s[1] << " / " << s[2] << " / " << s[3] << std::endl;
        check(hold.getBusOverride(0) == 0 && (hold.setBusOverride(0, CargoHold::c_stowBus), hold.getBus(0) == CargoHold::c_stowBus),
              "stowaway: a track can be sent there by hand");
        check(s[RigEngine::SectionStow] > 0.01f && s[RigEngine::SectionBusA] < 1.0e-5f && s[RigEngine::SectionBusB] < 1.0e-5f && s[RigEngine::SectionHits] < 1.0e-5f,
              "stowaway: a track sent there plays on Stowaway only (not bus A, not bus B, not the hits)");
        // Unity: as loud as the same track through the hits with the gate open and no boost, which adds nothing.
        const float expected = onHits.levels[RigEngine::SectionHits];
        check(expected > 0.01f && std::abs(s[RigEngine::SectionStow] / expected - 1.0f) < 0.001f,
              "stowaway: it passes the track on at unity (" + juce::String(s[RigEngine::SectionStow], 4) + " RMS, " + juce::String(expected, 4) + " through the open hits)");
        float worstLR = 0.0f, worstVsHits = 0.0f, peak = 0.0f;
        for (int i = 0; i < onStow.out.getNumSamples(); ++i)
        {
            worstLR = std::max(worstLR, std::abs(onStow.out.getSample(0, i) - onStow.out.getSample(1, i)));
            worstVsHits = std::max(worstVsHits, std::abs(onStow.out.getSample(0, i) - onHits.out.getSample(0, i)));
            peak = std::max(peak, std::abs(onStow.out.getSample(0, i)));
        }
        std::cout << "  output peak " << peak << ", left - right " << worstLR << ", against the same track through the open hits " << worstVsHits << std::endl;
        check(peak > 0.1f && worstLR == 0.0f, "stowaway: it reaches the output, in the middle");
        check(worstVsHits < 1.0e-3f, "stowaway: the output is the track itself (the same as through the open hit gate, sample for sample)");
        check(onStow.scopePeak > 0.2f && onStow.view > 0.5f && onA.scopePeak == 0.0f && onA.view == 0.0f && onA.levels[RigEngine::SectionStow] == 0.0f,
              "stowaway: its scope, level view and Follower level show it, and stay at nothing with no track there");

        // The hits duck it in every mode but Sub. A second tone plays through the (open) hits.
        {
            hold.loadFile(1, writeWav(folder, "routing_stow_tone300", tone(300.0, 3.0)), error);
            hold.setSkipSilence(1, false);
            hold.setBusOverride(1, CargoHold::c_hitsBus);
            auto p = plain();
            p.hitDuck = 1.0f;
            float level[RigEngine::c_stowModes] {};
            for (int mode = RigEngine::StowWake; mode < RigEngine::c_stowModes; ++mode) // (a Sub is not ducked: StowawayTest)
            {
                p.stow.mode = mode;
                level[mode] = run(CargoHold::c_stowBus, p).levels[RigEngine::SectionStow];
            }
            std::cout << "  Stowaway under a full Hit Duck: Wake " << level[1] << ", Fly " << level[2] << ", Dry " << level[3]
                      << " (" << s[RigEngine::SectionStow] << " with no hits)" << std::endl;
            check(level[RigEngine::StowWake] < 0.5f * s[RigEngine::SectionStow] && level[RigEngine::StowFly] < 0.5f * s[RigEngine::SectionStow]
                      && level[RigEngine::StowDry] < 0.5f * s[RigEngine::SectionStow],
                  "stowaway: the hits duck it in Wake, Fly and Dry");
            hold.clear(1);
        }

        // A morph rope on a Stowaway track, with the morph off: the track still plays through Stowaway.
        {
            hold.setBusOverride(0, CargoHold::c_stowBus);
            hold.setMorphSource(0, 0);
            const auto roped = run(CargoHold::c_stowBus, plain());
            hold.setMorphSource(0, -1);
            check(std::abs(roped.levels[RigEngine::SectionStow] / s[RigEngine::SectionStow] - 1.0f) < 0.01f && roped.levels[RigEngine::SectionBusB] < 1.0e-5f
                      && roped.levels[RigEngine::SectionBusA] < 1.0e-5f,
                  "stowaway: a roped Stowaway track with the morph off still plays through Stowaway, and nowhere else");
        }

        // Throws reach it, from rest too: the rig is stopped, one throw of 2 s, the track only on Stowaway.
        {
            hold.setBusOverride(0, CargoHold::c_stowBus);
            auto p = plain();
            p.throwSeconds = 2.0f;
            RigEngine rig;
            rig.setCargoHold(&hold);
            rig.setParams(p);
            rig.prepare(c_fs, c_block);
            rig.setParams(p);
            rig.setReleased(true);
            const auto rest = render(rig, 0.5);
            rig.takeSectionLevels();
            rig.throwWhoosh(0, 1.0f);
            const auto thrown = render(rig, 3.0);
            const auto levels = rig.takeSectionLevels();
            auto peakOf = [](const juce::AudioBuffer<float>& a, double from, double to)
            {
                float m = 0.0f;
                for (int i = static_cast<int>(from * c_fs); i < std::min(a.getNumSamples(), static_cast<int>(to * c_fs)); ++i)
                    m = std::max(m, std::abs(a.getSample(0, i)));
                return m;
            };
            const float before = peakOf(rest, 0.3, 0.5), early = peakOf(thrown, 0.0, 0.2), atPeak = peakOf(thrown, 0.9, 1.1), after = peakOf(thrown, 2.3, 3.0);
            std::cout << "  a throw from rest on Stowaway: " << before << " before, " << early << " as it starts, " << atPeak << " at its peak, " << after
                      << " after its tail (levels A / B / hits / Stowaway " << levels[0] << " / " << levels[1] << " / " << levels[2] << " / " << levels[3] << ")" << std::endl;
            check(before < 1.0e-5f && levels[RigEngine::SectionStow] > 0.01f && std::abs(atPeak / peak - 1.0f) < 0.05f,
                  "stowaway: a throw from rest is heard on it, as loud at its peak as the track playing");
            check(levels[RigEngine::SectionBusA] < 1.0e-5f && levels[RigEngine::SectionBusB] < 1.0e-5f, "stowaway: and the thrown track still feeds neither bus");
            check(early < 0.25f * atPeak && after < 1.0e-4f, "stowaway: the throw swells to its peak and is gone with its tail");
        }

        // A throw with the rig playing: the feed is the track itself, in step with it, so the throw only lifts it
        // to its fader level and never stacks a second copy on top (faders steady here: no louder than playing).
        {
            hold.setBusOverride(0, CargoHold::c_stowBus);
            auto p = plain();
            p.throwSeconds = 2.0f;
            RigEngine rig;
            rig.setCargoHold(&hold);
            rig.setParams(p);
            rig.prepare(c_fs, c_block);
            rig.setParams(p);
            render(rig, 0.5);
            rig.throwWhoosh(0, 1.0f);
            const auto thrown = render(rig, 3.0);
            float thrownPeak = 0.0f;
            for (int i = 0; i < static_cast<int>(1.5 * c_fs); ++i) // the throw up to and past its peak, before the tone's loop comes round
                thrownPeak = std::max(thrownPeak, std::abs(thrown.getSample(0, i)));
            std::cout << "  a throw on Stowaway with the rig playing: peak " << thrownPeak << " (" << peak << " playing)" << std::endl;
            check(thrownPeak < 1.05f * peak, "stowaway: a throw with the rig playing does not stack a second copy of the track on it");
        }

        // The knobs and the Mode reach the engine as they are set, glided, with the modulators' offsets on top.
        {
            auto p = plain();
            p.stow.mode = RigEngine::StowFly;
            p.stow.follow = 0.25f;
            p.stow.low = 0.5f;
            p.stow.filter = 0.3f;
            p.stow.drive = 0.6f;
            p.stow.width = 0.1f;
            p.stowMod[RigEngine::StowFilter] = 0.2f;
            p.stowMod[RigEngine::StowWidth] = -0.5f; // held at the bottom of the travel
            RigEngine rig;
            rig.setCargoHold(&hold);
            rig.setParams(p);
            rig.prepare(c_fs, c_block);
            rig.setParams(p);
            render(rig, 0.5);
            const auto& now = rig.getStowNow();
            check(now.mode == RigEngine::StowFly && std::abs(now.follow - 0.25f) < 1.0e-3f && std::abs(now.low - 0.5f) < 1.0e-3f
                      && std::abs(now.filter - 0.5f) < 1.0e-3f && std::abs(now.drive - 0.6f) < 1.0e-3f && now.width == 0.0f,
                  "stowaway: its Mode and five knobs reach the engine, modulators included");
        }
    }
    {
        // Rumbles go to Stowaway by themselves, but only while it is a sub.
        CargoHold hold;
        const auto file = writeWav(folder, "routing_stow_rumble", tone(60.0, 2.0));
        hold.loadFile(0, file, error);
        hold.setRoleOverride(0, static_cast<int>(CargoRole::Rumble));
        check(! hold.getRumbleToStowaway() && hold.getBus(0) == 0 && hold.getBusOverride(0) == -1,
              "rumble: on Auto a rumble plays on bus A to begin with (Stowaway starts switched off)");
        hold.setRumbleToStowaway(true);
        check(hold.getBus(0) == CargoHold::c_stowBus, "rumble: with Stowaway on and a sub, it plays on Stowaway");
        hold.setRumbleToStowaway(false);
        check(hold.getBus(0) == 0, "rumble: with Stowaway not a sub, it plays on bus A");
        bool othersStay = true;
        for (const bool on : { false, true })
        {
            hold.setRumbleToStowaway(on);
            for (int r = 0; r < c_numCargoRoles; ++r)
                if (static_cast<CargoRole>(r) != CargoRole::Rumble)
                    othersStay = othersStay && CargoHold::busForRole(static_cast<CargoRole>(r), on) != CargoHold::c_stowBus
                                 && hold.autoBus(static_cast<CargoRole>(r)) == CargoHold::busForRole(static_cast<CargoRole>(r), false);
        }
        check(othersStay, "rumble: no other type is ever sent to Stowaway by itself");
        hold.setRumbleToStowaway(true);
        hold.setBusOverride(0, 1);
        check(hold.getBus(0) == 1, "rumble: a bus chosen by hand wins");
        hold.setBusOverride(0, CargoHold::c_stowBus);
        hold.setRumbleToStowaway(false);
        check(hold.getBus(0) == CargoHold::c_stowBus, "rumble: and Stowaway chosen by hand stays in every mode");
        // Saved with the session like the other choices; a rig sharing the sounds routes the same way.
        CargoHold back;
        back.fromValueTree(hold.toValueTree());
        check(back.getBusOverride(0) == CargoHold::c_stowBus, "stowaway: the choice survives save and restore");
        hold.setBusOverride(0, -1);
        CargoHold shared;
        hold.beginLending();
        shared.shareFrom(hold);
        check(! shared.getRumbleToStowaway() && shared.getBus(0) == 0, "rumble: a rig that shares the sounds routes rumbles the same way");
        shared.fromValueTree(juce::ValueTree("Cargo"));
        hold.endLending();

        // Through the plug-in: the Mode decides, at once (no audio needed), and with the audio running.
        Plug plug;
        plug.load(0, file);
        plug.load(1, file);
        auto& cargo = plug.proc.getCargo();
        cargo.setRoleOverride(0, static_cast<int>(CargoRole::Rumble));
        cargo.setRoleOverride(1, static_cast<int>(CargoRole::Tonal));
        auto* onParam = dynamic_cast<juce::AudioParameterBool*>(plug.proc.getState().getParameter(ParamIDs::stowOn));
        check(onParam != nullptr && ! onParam->get() && onParam->getName(64) == "Stowaway" && onParam->isAutomatable() && ! plug.proc.isStowawayOn(),
              "on / off: Stowaway has a switch in the host's list, and a new session starts with it off");
        check(juce::roundToInt(plug.plain(ParamIDs::stowMode)) == ParamIDs::StowSub && cargo.getBus(0) == 0 && cargo.getBus(1) == 0,
              "rumble: in the plug-in, Stowaway starts off and in Sub, so a rumble on Auto plays on bus A (it is not left waiting)");
        plug.set(ParamIDs::stowOn, 1.0f);
        check(plug.proc.isStowawayOn() && cargo.getBus(0) == CargoHold::c_stowBus && cargo.getBus(1) == 0,
              "rumble: switched on (and a sub), Stowaway takes the rumble at once (a tonal sound stays on bus A)");
        bool onlySub = true;
        for (int mode = ParamIDs::numStowModes - 1; mode >= 0; --mode)
        {
            plug.set(ParamIDs::stowMode, static_cast<float>(mode));
            onlySub = onlySub && cargo.getBus(0) == (mode == ParamIDs::StowSub ? CargoHold::c_stowBus : 0) && cargo.getBus(1) == 0;
        }
        check(onlySub, "rumble: it follows Stowaway's Mode: there in Sub, on bus A in Wake, Fly and Dry");
        plug.set(ParamIDs::stowOn, 0.0f);
        check(cargo.getBus(0) == 0, "rumble: switched off again, the rumble is back on bus A");
        plug.set(ParamIDs::stowOn, 1.0f);

        // The parameters: where they are, what they are called, how they read.
        auto* modeParam = dynamic_cast<juce::AudioParameterChoice*>(plug.proc.getState().getParameter(ParamIDs::stowMode));
        check(modeParam != nullptr && modeParam->choices == juce::StringArray({ "Sub", "Wake", "Fly", "Dry" }) && modeParam->getName(64) == "Stowaway Mode",
              "stowaway: Mode is a menu of Sub, Wake, Fly and Dry");
        const char* names[ParamIDs::numStowControls] = { "Stowaway Follow", "Stowaway Low End", "Stowaway Filter", "Stowaway Drive", "Stowaway Width" };
        bool named = true, targets = true;
        for (int k = 0; k < ParamIDs::numStowControls; ++k)
        {
            auto* param = plug.proc.getState().getParameter(ParamIDs::stow[k]);
            named = named && param != nullptr && param->getName(64) == names[k];
            targets = targets && ModMatrix::targetIndex(ParamIDs::stow[k]) >= 0;
        }
        check(named, "stowaway: five knobs: Follow, Low End, Filter, Drive, Width");
        check(targets && ModMatrix::targetIndex(ParamIDs::stowMode) < 0, "stowaway: the five knobs can be modulated (the Mode cannot)");
        auto text = [&](int k) { auto* param = plug.proc.getState().getParameter(ParamIDs::stow[k]); return param->getText(param->getValue(), 64); };
        auto textAt = [&](int k, float v) { return plug.proc.getState().getParameter(ParamIDs::stow[k])->getText(v, 64); };
        plug.set(ParamIDs::stowMode, static_cast<float>(ParamIDs::StowSub));
        std::cout << "  in Sub the knobs read: " << text(0) << ", " << text(1) << ", " << text(2) << ", " << text(3) << ", " << text(4)
                  << "; Filter from " << textAt(ParamIDs::StowFilter, 0.0f) << " to " << textAt(ParamIDs::StowFilter, 1.0f) << std::endl;
        check(text(ParamIDs::StowFilter) == "120 Hz" && textAt(ParamIDs::StowFilter, 0.0f) == "40 Hz" && textAt(ParamIDs::StowFilter, 1.0f) == "250 Hz",
              "stowaway: in Sub, Filter reads the crossover in Hz: 40 to 250 Hz, 120 Hz where it starts");
        bool rises = true;
        for (int i = 1; i <= 100; ++i)
            rises = rises && Params::stowCrossoverHz(static_cast<float>(i) / 100.0f) > Params::stowCrossoverHz(static_cast<float>(i - 1) / 100.0f)
                    && std::abs(Params::stowCrossoverKnob(Params::stowCrossoverHz(static_cast<float>(i) / 100.0f)) - static_cast<float>(i) / 100.0f) < 1.0e-4f;
        check(rises, "stowaway: the crossover rises all the way up the knob, and a frequency finds its place on it");
        check(text(ParamIDs::StowWidth) == "Mono" && text(ParamIDs::StowFollow) == "70%" && text(ParamIDs::StowLow) == "0%" && text(ParamIDs::StowDrive) == "0%",
              "stowaway: in Sub, Width reads Mono; Follow, Low End and Drive read a percentage");
        bool otherModes = true;
        for (int mode = ParamIDs::StowWake; mode < ParamIDs::numStowModes; ++mode)
        {
            plug.set(ParamIDs::stowMode, static_cast<float>(mode));
            otherModes = otherModes && text(ParamIDs::StowFilter) == "Open" && text(ParamIDs::StowWidth) == "100%"
                         && textAt(ParamIDs::StowFilter, 0.0f) == "Under 150 Hz" && textAt(ParamIDs::StowFilter, 1.0f) == "Over 2.5 kHz";
        }
        std::cout << "  in Dry: Filter " << text(ParamIDs::StowFilter) << " (" << textAt(ParamIDs::StowFilter, 0.0f) << " .. " << textAt(ParamIDs::StowFilter, 0.3f) << " .. "
                  << textAt(ParamIDs::StowFilter, 0.7f) << " .. " << textAt(ParamIDs::StowFilter, 1.0f) << "), Width " << text(ParamIDs::StowWidth) << std::endl;
        check(otherModes, "stowaway: in Wake, Fly and Dry, Filter is open where it starts and closes either way, and Width reads a percentage");

        // To the engine, with a modulator wired to one knob (a Macro at 40% through a 50% wire: +20%).
        plug.set(ParamIDs::stowMode, static_cast<float>(ParamIDs::StowWake));
        plug.set(ParamIDs::stow[ParamIDs::StowFollow], 0.2f);
        plug.set(ParamIDs::stow[ParamIDs::StowLow], 0.3f);
        plug.set(ParamIDs::stow[ParamIDs::StowFilter], 0.4f);
        plug.set(ParamIDs::stow[ParamIDs::StowDrive], 0.6f);
        plug.set(ParamIDs::stow[ParamIDs::StowWidth], 0.8f);
        plug.set(ParamIDs::modType[0], 3.0f);
        plug.set(ParamIDs::modKnob[0], 0.4f);
        plug.proc.getMods().connect(0, ParamIDs::stow[ParamIDs::StowDrive], 0.5f);
        cargo.setBusOverride(1, CargoHold::c_stowBus);
        plug.proc.prepareToPlay(c_fs, 512);
        plug.proc.setPlaying(true);
        juce::AudioBuffer<float> block(2, 512);
        juce::MidiBuffer midi;
        float heard = 0.0f;
        for (int i = 0; i < 200; ++i)
        {
            block.clear();
            plug.proc.processBlock(block, midi);
            heard = std::max(heard, block.getMagnitude(0, 512));
        }
        const auto now = plug.proc.getEngine().getStowNow();
        std::cout << "  the engine's Stowaway: mode " << now.mode << ", " << now.follow << " / " << now.low << " / " << now.filter << " / " << now.drive << " / " << now.width
                  << ", level " << plug.proc.getEngine().getStowLevel() << ", output peak " << heard << std::endl;
        check(now.mode == RigEngine::StowWake && std::abs(now.follow - 0.2f) < 0.01f && std::abs(now.low - 0.3f) < 0.01f && std::abs(now.filter - 0.4f) < 0.01f
                  && std::abs(now.width - 0.8f) < 0.01f,
              "stowaway: the plug-in's Mode and knobs reach the engine");
        check(std::abs(now.drive - 0.8f) < 0.02f, "stowaway: a modulator wired to a knob moves it in the engine");
        check(plug.proc.getEngine().getStowLevel() > 0.01f && heard > 0.01f && cargo.getBus(0) == 0, "stowaway: a track sent there by hand is heard through the plug-in, in any mode");
        {
            // The switch through the plug-in: off, the same session goes silent (the only track that is heard here
            // is the one on Stowaway: the other one is the rumble's file as a tonal sound on bus A, so mute it).
            cargo.setMute(CargoHold::c_numBuiltIns + 0, true);
            plug.set(ParamIDs::stowOn, 0.0f);
            // (Space rings on for a few seconds after the last sound went into it: listen once it has.)
            float off = 0.0f, back = 0.0f;
            for (int i = 0; i < 900; ++i)
            {
                block.clear();
                plug.proc.processBlock(block, midi);
                if (i >= 840)
                    off = std::max(off, block.getMagnitude(0, 512));
            }
            const float levelOff = plug.proc.getEngine().getStowLevel();
            plug.set(ParamIDs::stowOn, 1.0f);
            for (int i = 0; i < 120; ++i)
            {
                block.clear();
                plug.proc.processBlock(block, midi);
                back = std::max(back, block.getMagnitude(0, 512));
            }
            std::cout << "  the switch through the plug-in: " << off << " while off (level view " << levelOff << "), " << back << " back on" << std::endl;
            check(off < 1.0e-4f && levelOff == 0.0f && back > 0.01f, "on / off: the plug-in's switch silences Stowaway and brings it back");
            cargo.setMute(CargoHold::c_numBuiltIns + 0, false);
        }

        // Saved and restored: the Mode, the knobs, the track's place, the wire, and the rumble routing that goes with the Mode.
        juce::MemoryBlock state;
        plug.proc.getStateInformation(state);
        Plug other;
        other.load(0, file); // so there is something to route before the state arrives
        other.proc.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        const auto wires = other.proc.getMods().getRoutes();
        check(juce::roundToInt(other.plain(ParamIDs::stowMode)) == ParamIDs::StowWake && std::abs(other.plain(ParamIDs::stow[ParamIDs::StowFollow]) - 0.2f) < 1.0e-4f
                  && std::abs(other.plain(ParamIDs::stow[ParamIDs::StowLow]) - 0.3f) < 1.0e-4f && std::abs(other.plain(ParamIDs::stow[ParamIDs::StowFilter]) - 0.4f) < 1.0e-4f
                  && std::abs(other.plain(ParamIDs::stow[ParamIDs::StowDrive]) - 0.6f) < 1.0e-4f && std::abs(other.plain(ParamIDs::stow[ParamIDs::StowWidth]) - 0.8f) < 1.0e-4f,
              "stowaway: the Mode and the knobs are saved and restored");
        check(other.proc.getCargo().getBusOverride(1) == CargoHold::c_stowBus && other.proc.getCargo().getBus(1) == CargoHold::c_stowBus
                  && wires.size() == 1 && wires[0].param == ParamIDs::stow[ParamIDs::StowDrive],
              "stowaway: so are the track sent there and the wire to its knob");
        check(! other.proc.getCargo().getRumbleToStowaway() && other.proc.getCargo().getRoleOverride(0) == static_cast<int>(CargoRole::Rumble)
                  && other.proc.getCargo().getBus(0) == 0,
              "rumble: a session restored in Wake keeps its rumble on bus A");
        plug.proc.setTrackBus(0, CargoHold::c_stowBus);
        juce::String what;
        check(cargo.getBusOverride(0) == CargoHold::c_stowBus && plug.proc.undoEdit(&what) && what == "Track Bus" && cargo.getBusOverride(0) == -1,
              "stowaway: sending a track there through the plug-in is one undo step");

        // Presets and the dice: Stowaway is a section with a die and a lock; nothing random ever touches the Mode.
        check(juce::String(Presets::sectionName(Presets::Stowaway)) == "Stowaway" && ! Presets::isSessionParameter(ParamIDs::stowMode),
              "stowaway: a section of its own for the dice and the locks, and a preset may set its Mode");
        juce::Random r(7);
        bool modeLeft = true, knobsRolled = true, lockHolds = true, onlyItsOwn = true;
        for (int i = 0; i < 200; ++i)
        {
            const auto all = Presets::roll(r), locked = Presets::roll(r, 1u << Presets::Stowaway), die = Presets::rollSection(r, Presets::Stowaway);
            modeLeft = modeLeft && all.values.count(ParamIDs::stowMode) == 0 && die.values.count(ParamIDs::stowMode) == 0;
            onlyItsOwn = onlyItsOwn && die.values.size() == static_cast<size_t>(ParamIDs::numStowControls);
            for (const auto* id : ParamIDs::stow)
            {
                knobsRolled = knobsRolled && all.values.count(id) == 1 && die.values.count(id) == 1 && die.values.at(id) >= 0.0f && die.values.at(id) <= 1.0f;
                lockHolds = lockHolds && locked.values.count(id) == 0;
            }
        }
        check(modeLeft, "stowaway: Randomise and Stowaway's die never change the Mode");
        check(knobsRolled && onlyItsOwn, "stowaway: they roll its five knobs (the die: those and nothing else)");
        check(lockHolds, "stowaway: its lock keeps Randomise off them");
        Presets::Preset wake { "Wake", { { ParamIDs::stowMode, static_cast<float>(ParamIDs::StowFly) } } };
        bool presetSetsMode = false, rollKeepsMode = true;
        for (const auto& [param, value] : Presets::targets(plug.proc.getParameters(), wake, Presets::Unlisted::Default))
            if (param->getParameterID() == ParamIDs::stowMode)
                presetSetsMode = juce::roundToInt(param->convertFrom0to1(value)) == ParamIDs::StowFly;
        for (const auto& [param, value] : Presets::targets(plug.proc.getParameters(), Presets::roll(r), Presets::Unlisted::Keep))
            rollKeepsMode = rollKeepsMode && param->getParameterID() != ParamIDs::stowMode;
        check(presetSetsMode && rollKeepsMode, "stowaway: a preset that names the Mode sets it; a roll leaves it out");
        plug.proc.setSectionLocked(Presets::Stowaway, true);
        juce::MemoryBlock lockedState;
        plug.proc.getStateInformation(lockedState);
        Plug third;
        third.proc.setStateInformation(lockedState.getData(), static_cast<int>(lockedState.getSize()));
        check(third.proc.isSectionLocked(Presets::Stowaway) && ! third.proc.isSectionLocked(Presets::Output), "stowaway: its lock is saved with the session");

        // The Followers can listen to it, and a recording can mark its passes.
        const auto followers = ModMatrix::followerNames();
        auto* follows = dynamic_cast<juce::AudioParameterChoice*>(plug.proc.getState().getParameter(ParamIDs::modFollow[0]));
        check(followers.size() == ModMatrix::c_numFollowerInputs && followers[ModMatrix::FollowStow] == "Stowaway" && follows != nullptr && follows->choices == followers,
              "stowaway: a Follower can listen to it");
        check(TapeRecorder::markerLabel(TapeRecorder::MarkPassStow) == "Pass S" && static_cast<int>(RigEngine::MarkPassStow) == static_cast<int>(TapeRecorder::MarkPassStow),
              "stowaway: its own passes have a marker of their own");
    }

    //==========================================================================
    std::cout << "Stowaway on / off" << std::endl;
    {
        // A 300 Hz tone sent to Stowaway by hand, in Dry (the track as it is), steady faders.
        CargoHold hold;
        hold.loadFile(0, writeWav(folder, "routing_onoff_tone300", tone(300.0, 6.0, 0.4f)), error);
        hold.setSkipSilence(0, false);
        hold.setRoleOverride(0, static_cast<int>(CargoRole::Tonal));
        hold.setBusOverride(0, CargoHold::c_stowBus);
        auto p = plainParams();
        p.stow.mode = RigEngine::StowDry;
        check(RigEngine::StowParams().on, "on / off: an engine on its own has Stowaway on (the plug-in's switch is what starts off)");

        auto peakOf = [](const juce::AudioBuffer<float>& a, double from, double to)
        {
            float m = 0.0f;
            for (int i = static_cast<int>(from * c_fs); i < std::min(a.getNumSamples(), static_cast<int>(to * c_fs)); ++i)
                m = std::max(m, std::max(std::abs(a.getSample(0, i)), std::abs(a.getSample(1, i))));
            return m;
        };
        auto biggestStep = [](const juce::AudioBuffer<float>& a)
        {
            float m = std::abs(a.getSample(0, 0));
            for (int i = 1; i < a.getNumSamples(); ++i)
                m = std::max(m, std::abs(a.getSample(0, i) - a.getSample(0, i - 1)));
            return m;
        };

        // Off from the start: silent, and the track is not played anywhere else.
        p.stow.on = false;
        RigEngine rig;
        rig.setCargoHold(&hold);
        rig.setParams(p);
        rig.prepare(c_fs, c_block);
        rig.setParams(p);
        rig.takeSectionLevels();
        const auto off = render(rig, 1.0);
        const auto offLevels = rig.takeSectionLevels();
        std::array<float, ScopeRing::c_points> lo {}, hi {};
        rig.getScope(RigEngine::SectionStow).read(lo, hi);
        float scopePeak = 0.0f;
        for (const float v : hi)
            scopePeak = std::max(scopePeak, v);
        check(peakOf(off, 0.0, 1.0) == 0.0f, "on / off: switched off, a track sent to Stowaway is silent (exactly nothing at the output)");
        check(offLevels[RigEngine::SectionBusA] == 0.0f && offLevels[RigEngine::SectionBusB] == 0.0f && offLevels[RigEngine::SectionHits] == 0.0f
                  && offLevels[RigEngine::SectionStow] == 0.0f,
              "on / off: the track waits: it does not fall through to bus A, bus B or the hits");
        check(rig.isStowShut() && rig.getStowLevel() == 0.0f && rig.getStowEnv() == 0.0f && scopePeak == 0.0f && ! rig.isFlyAwake()
                  && ! rig.getStowFlight(0).active && ! rig.getStowFlight(1).active,
              "on / off: off, Stowaway does no work and shows nothing (its level, its scope, its Follower level and its passes are all at rest)");

        // A throw from rest does not wake it either.
        rig.throwWhoosh(0, 1.0f);
        const auto thrownOff = render(rig, 3.0);
        check(peakOf(thrownOff, 0.0, 3.0) == 0.0f && rig.isStowShut(), "on / off: a throw leaves a switched-off Stowaway silent");

        // On: it fades in. The steepest a 300 Hz tone at 0.4 ever gets is 0.0157 a sample; a click would be far more.
        const float toneStep = 0.4f * static_cast<float>(c_twoPi) * 300.0f / static_cast<float>(c_fs);
        p.stow.on = true;
        rig.setParams(p);
        const auto in = render(rig, 1.0);
        const float steady = peakOf(in, 0.5, 1.0);
        std::cout << "  switched on: " << peakOf(in, 0.0, 0.004) << " in the first 4 ms, " << peakOf(in, 0.02, 0.03) << " at 20 to 30 ms, " << steady
                  << " once in; biggest step " << biggestStep(in) << " (the tone's own " << toneStep << ")" << std::endl;
        check(steady > 0.1f && peakOf(in, 0.0, 0.004) < 0.12f * steady && peakOf(in, 0.02, 0.03) > 0.2f * steady && peakOf(in, 0.02, 0.03) < 0.75f * steady,
              "on / off: switched on, it fades in over about 60 ms");
        check(biggestStep(in) < 1.25f * toneStep * steady / 0.4f, "on / off: switching on does not click");

        // Off: it fades out, then it is exactly silent again and at rest.
        p.stow.on = false;
        rig.setParams(p);
        const auto out = render(rig, 1.0);
        std::cout << "  switched off: " << peakOf(out, 0.0, 0.004) << " in the first 4 ms, " << peakOf(out, 0.03, 0.04) << " at 30 to 40 ms, "
                  << peakOf(out, 0.1, 1.0) << " after 100 ms; biggest step " << biggestStep(out) << std::endl;
        check(peakOf(out, 0.0, 0.004) > 0.8f * steady && peakOf(out, 0.03, 0.04) < 0.6f * steady && peakOf(out, 0.1, 1.0) < 1.0e-5f
                  && peakOf(out, 0.5, 1.0) < 1.0e-9f && rig.isStowShut(), // (the output's DC blocker lets go of the last of it)
              "on / off: switched off, it fades out over about 60 ms and is then silent and at rest");
        check(biggestStep(out) < 1.25f * toneStep * steady / 0.4f, "on / off: switching off does not click");

        // The Mode can change while it is off, and it comes back in the new one (Sub: a 300 Hz tone is well down).
        p.stow.mode = RigEngine::StowSub;
        rig.setParams(p);
        render(rig, 0.2);
        p.stow.on = true;
        rig.setParams(p);
        const auto asSub = render(rig, 1.0);
        check(rig.getStowNow().mode == RigEngine::StowSub && peakOf(asSub, 0.5, 1.0) < 0.1f * steady && allFinite(asSub),
              "on / off: a Mode chosen while it was off is the one it comes back in");

        // The plug-in: presets switch it on and never off; Randomise leaves it alone; it is saved with the session.
        Plug plug;
        const auto& params = plug.proc.getParameters();
        auto targetOf = [&](const Presets::Preset& preset, Presets::Unlisted unlisted) -> float
        {
            for (const auto& [param, value] : Presets::targets(params, preset, unlisted))
                if (param->getParameterID() == ParamIDs::stowOn)
                    return value;
            return -1.0f; // not moved
        };
        const Presets::Preset uses { "Uses Stowaway", { { ParamIDs::stowOn, 1.0f }, { ParamIDs::stowMode, static_cast<float>(ParamIDs::StowFly) } } };
        const Presets::Preset says { "Says Off", { { ParamIDs::stowOn, 0.0f } } };
        bool rollsLeaveIt = true;
        juce::Random r(11);
        for (int i = 0; i < 40; ++i)
            rollsLeaveIt = rollsLeaveIt && Presets::roll(r).values.count(ParamIDs::stowOn) == 0 && Presets::rollSection(r, Presets::Stowaway).values.count(ParamIDs::stowOn) == 0
                           && targetOf(Presets::roll(r), Presets::Unlisted::Keep) < 0.0f;
        check(targetOf(uses, Presets::Unlisted::Default) == 1.0f, "on / off: a preset that uses Stowaway switches it on");
        check(targetOf(Presets::factory()[0], Presets::Unlisted::Default) < 0.0f && targetOf(says, Presets::Unlisted::Default) < 0.0f,
              "on / off: a preset that does not use it leaves the switch alone (it never switches Stowaway off)");
        check(rollsLeaveIt, "on / off: Randomise and Stowaway's own die never move the switch");
        plug.proc.applyLook(uses, Presets::Unlisted::Default, "Preset", "Uses Stowaway");
        const bool onByPreset = plug.proc.isStowawayOn();
        plug.proc.applyLook(Presets::factory()[0], Presets::Unlisted::Default, "Preset", Presets::factory()[0].name);
        check(onByPreset && plug.proc.isStowawayOn(), "on / off: through the plug-in, the preset switches it on and the next preset leaves it on");

        check(plug.proc.getStowawayPanel() == -1, "plate: a new session has not opened Stowaway's plate yet");
        plug.proc.setStowawayPanel(0);
        juce::MemoryBlock state;
        plug.proc.getStateInformation(state);
        Plug other;
        other.proc.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        check(other.proc.isStowawayOn() && other.proc.getStowawayPanel() == 0, "on / off: the switch, and whether the plate is folded, are saved with the session");
        other.proc.newSession();
        check(! other.proc.isStowawayOn() && other.proc.getStowawayPanel() == -1, "on / off: a new session starts with Stowaway off and its plate not yet opened");
    }

    //==========================================================================
    std::cout << "Morph ropes" << std::endl;
    {
        CargoHold hold;
        hold.loadFile(0, writeWav(folder, "routing_tone300b", tone(300.0, 4.0, 0.4f)), error);
        hold.loadFile(1, writeWav(folder, "routing_tone1kb", tone(1000.0, 4.0, 0.4f)), error);
        hold.loadFile(2, writeWav(folder, "routing_tone3k", tone(3000.0, 4.0, 0.4f)), error);
        hold.loadFile(3, writeWav(folder, "routing_tone5k", tone(5000.0, 4.0, 0.4f)), error);
        for (int t = 0; t < 4; ++t)
            hold.setSkipSilence(t, false);
        hold.setRoleOverride(0, static_cast<int>(CargoRole::Tonal)); // bus A
        hold.setRoleOverride(1, static_cast<int>(CargoRole::Tonal)); // bus A
        hold.setRoleOverride(2, static_cast<int>(CargoRole::Bed));   // bus B
        hold.setRoleOverride(3, static_cast<int>(CargoRole::Impact)); // hits
        check(hold.getMorphSource(0) == -1 && hold.getMorphSource(1) == -1, "ropes: none to begin with (each side is its bus)");

        struct Levels { float a300, a1k, b3k, h5k; bool finite; juce::AudioBuffer<float> out; };
        auto run = [&](bool morphOn, float x, int ropeA, int ropeB)
        {
            hold.setMorphSource(0, ropeA);
            hold.setMorphSource(1, ropeB);
            RigEngine rig;
            rig.setCargoHold(&hold);
            auto p = plainParams();
            p.morphOn = morphOn;
            p.morphX = x;
            p.morphY = 0.0f;
            p.morphMode = 0;
            rig.setParams(p);
            rig.prepare(c_fs, c_block);
            rig.setParams(p);
            Levels l;
            l.out = render(rig, 3.0);
            const int from = static_cast<int>(1.0 * c_fs), to = l.out.getNumSamples();
            l.a300 = bandDb(l.out, from, to, 300.0);
            l.a1k = bandDb(l.out, from, to, 1000.0);
            l.b3k = bandDb(l.out, from, to, 3000.0);
            l.h5k = bandDb(l.out, from, to, 5000.0);
            l.finite = allFinite(l.out);
            return l;
        };
        auto say = [](const Levels& l) { return juce::String(l.a300, 1) + " / " + juce::String(l.a1k, 1) + " / " + juce::String(l.b3k, 1) + " / " + juce::String(l.h5k, 1) + " dB"; };

        const auto dry = run(false, 1.0f, -1, -1);
        const auto dryRoped = run(false, 1.0f, 0, 2);
        std::cout << "  300 Hz / 1 kHz (bus A) / 3 kHz (bus B) / 5 kHz (hits):" << std::endl;
        std::cout << "    morph off              " << say(dry) << std::endl;
        float worstDifference = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < dry.out.getNumSamples(); ++i)
                worstDifference = std::max(worstDifference, std::abs(dry.out.getSample(ch, i) - dryRoped.out.getSample(ch, i)));
        std::cout << "    morph off, both roped  " << say(dryRoped) << " (largest difference to unroped: " << worstDifference << ")" << std::endl;
        check(dry.finite && dry.a300 > -40.0f && dry.a1k > -40.0f && dry.b3k > -40.0f && dry.h5k > -40.0f, "ropes: every track is heard with the morph off");
        check(worstDifference < 1.0e-6f, "ropes: with the morph off a roped track plays through its bus exactly as without the rope");

        // X at B: the morph gives out side B's sound only. Whatever side A hears is gone; whatever it doesn't hear plays on.
        const auto buses = run(true, 1.0f, -1, -1);
        std::cout << "    morph at B, no ropes   " << say(buses) << std::endl;
        check(buses.finite && buses.a300 < dry.a300 - 20.0f && buses.a1k < dry.a1k - 20.0f && buses.b3k > dry.b3k - 6.0f,
              "ropes: unroped, side A is the whole of bus A (both its tracks go into the morph)");

        const auto roped = run(true, 1.0f, 0, -1);
        std::cout << "    morph at B, A roped to the 300 Hz track  " << say(roped) << std::endl;
        check(roped.finite && roped.a300 < dry.a300 - 20.0f, "ropes: the roped track goes into the morph");
        check(std::abs(roped.a1k - dry.a1k) < 1.5f, "ropes: the rest of the roped side's bus plays on as it was");
        check(roped.b3k > dry.b3k - 6.0f && std::abs(roped.h5k - dry.h5k) < 1.5f, "ropes: side B is still its bus, and the hits are untouched");

        // X at A: side A's sound only. Roped to the 1 kHz track, that is what the morph gives; side B's bus is gone.
        const auto atA = run(true, 0.0f, 1, -1);
        std::cout << "    morph at A, A roped to the 1 kHz track   " << say(atA) << std::endl;
        check(atA.finite && atA.a1k > dry.a1k - 6.0f && atA.b3k < dry.b3k - 20.0f && std::abs(atA.a300 - dry.a300) < 1.5f,
              "ropes: at A the roped track is what the morph gives, and the unroped 300 Hz track plays on");

        // Side B roped to a hits track: the morph takes it from the hits (X at A: it is gone), bus B plays on.
        const auto hitRoped = run(true, 0.0f, -1, 3);
        std::cout << "    morph at A, B roped to the 5 kHz hit     " << say(hitRoped) << std::endl;
        check(hitRoped.finite && hitRoped.h5k < dry.h5k - 20.0f && std::abs(hitRoped.b3k - dry.b3k) < 1.5f,
              "ropes: a rope reaches a track on any bus (a hit track here), and the unroped bus B plays on");

        // One track can't be both sides.
        hold.setMorphSource(0, 1);
        hold.setMorphSource(1, 1);
        check(hold.getMorphSource(1) == 1 && hold.getMorphSource(0) == -1, "ropes: roping a track to one side lets go of the other side's rope to it");

        // Changing a rope while the morph is in doesn't click (steady tones: any jump shows in the second difference).
        {
            hold.setMorphSource(0, -1);
            hold.setMorphSource(1, -1);
            hold.setMute(CargoHold::c_numBuiltIns + 2, true); // the low tones only: their curve is small, a click is not
            hold.setMute(CargoHold::c_numBuiltIns + 3, true);
            RigEngine rig;
            rig.setCargoHold(&hold);
            auto p = plainParams();
            p.morphOn = true;
            p.morphX = 0.3f;
            rig.setParams(p);
            rig.prepare(c_fs, c_block);
            rig.setParams(p);
            const int on = static_cast<int>(2.0 * c_fs) / c_block * c_block, swap = static_cast<int>(3.0 * c_fs) / c_block * c_block,
                      off = static_cast<int>(4.0 * c_fs) / c_block * c_block;
            int seenAfterOn = -2;
            const auto out = render(rig, 5.0, [&](int pos)
            {
                if (pos == on) hold.setMorphSource(0, 0);
                if (pos == swap) { hold.setMorphSource(0, 1); seenAfterOn = rig.getMorphSource(0); }
                if (pos == off) hold.setMorphSource(0, -1);
            });
            auto maxCurve = [&](int from, int to)
            {
                float m = 0.0f;
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = from + 2; i < to; ++i)
                        m = std::max(m, std::abs(out.getSample(ch, i) - 2.0f * out.getSample(ch, i - 1) + out.getSample(ch, i - 2)));
                return m;
            };
            const float normal = maxCurve(static_cast<int>(1.0 * c_fs), on);
            const int window = static_cast<int>(0.3 * c_fs);
            const float around = std::max({ maxCurve(on, on + window), maxCurve(swap, swap + window), maxCurve(off, off + window) });
            std::cout << "  rope changes mid-morph: sharpest change " << around << " vs " << normal << " in steady play" << std::endl;
            check(allFinite(out) && around <= 2.0f * normal, "ropes: roping, re-roping and letting go mid-morph doesn't click");
            check(seenAfterOn == 0 && rig.getMorphSource(0) == -1, "ropes: the engine reports the rope in use");
        }
        // Moving a rope from one track to another leaves the rest of the side's bus alone: the unroped 1 kHz track
        // on bus A plays straight through while side A's rope goes from the 300 Hz track to the (muted) hit track.
        {
            RigEngine rig;
            rig.setCargoHold(&hold);
            auto p = plainParams();
            p.morphOn = true;
            p.morphX = 0.3f;
            rig.setParams(p);
            rig.prepare(c_fs, c_block);
            rig.setParams(p);
            hold.setMorphSource(0, 0);
            const int swap = static_cast<int>(3.0 * c_fs) / c_block * c_block;
            const auto out = render(rig, 4.0, [&](int pos) { if (pos == swap) hold.setMorphSource(0, 3); });
            // The 1 kHz level every 10 ms (a whole number of cycles of every tone here, so the others drop out).
            auto lowest = [&](int from, int to)
            {
                constexpr int n = 480;
                double low = 1.0e9;
                for (int start = from; start + n <= to; start += n)
                {
                    double re = 0.0, im = 0.0;
                    for (int i = 0; i < n; ++i)
                    {
                        const double x = out.getSample(0, start + i), ph = c_twoPi * 1000.0 * i / c_fs;
                        re += x * std::cos(ph);
                        im += x * std::sin(ph);
                    }
                    low = std::min(low, 2.0 * std::sqrt(re * re + im * im) / n);
                }
                return low;
            };
            const double steady = lowest(static_cast<int>(2.0 * c_fs), swap), around = lowest(swap, swap + static_cast<int>(0.2 * c_fs));
            std::cout << "  rope moved to another track: the unroped 1 kHz track's lowest level " << around << " vs " << steady << " before" << std::endl;
            check(allFinite(out) && steady > 1.0e-3 && around > 0.9 * steady, "ropes: moving a rope between tracks doesn't dip the rest of the side's bus");
            check(rig.getMorphSource(0) == 3, "ropes: the engine has taken the new rope");
            hold.setMorphSource(0, -1);
            hold.setMute(CargoHold::c_numBuiltIns + 2, false);
            hold.setMute(CargoHold::c_numBuiltIns + 3, false);
        }

        // Ropes follow their lanes: removing another track changes nothing, removing the roped one lets go, undo re-ties.
        hold.setMorphSource(0, 2);
        hold.setMorphSource(1, 3);
        hold.clear(0);
        check(hold.getMorphSource(0) == 2 && hold.getMorphSource(1) == 3, "ropes: removing another track leaves the ropes on their tracks");
        hold.clear(2);
        check(hold.getMorphSource(0) == -1 && hold.getMorphSource(1) == 3, "ropes: removing a roped track lets its rope go (the side is its bus again)");
        hold.undo(2);
        check(hold.getMorphSource(0) == 2, "ropes: undo brings the track back on its rope");

        // Saved with the session; a rope to a track that isn't there is dropped.
        auto tree = hold.toValueTree();
        CargoHold back;
        back.fromValueTree(tree);
        check(back.getMorphSource(0) == 2 && back.getMorphSource(1) == 3, "ropes: saved and restored with the session");
        tree.setProperty("morphSourceA", 7, nullptr); // an empty track
        tree.setProperty("master", 9, nullptr);
        back.fromValueTree(tree);
        check(back.getMorphSource(0) == -1 && back.getMorphSource(1) == 3 && back.getMaster() == -1, "ropes: a rope (or a Master) on a track that isn't there is dropped");

        // A second rig sharing the sounds (Render Variations) gets the same routing.
        hold.setMaster(1);
        hold.setBusOverride(1, 1);
        CargoHold shared;
        hold.beginLending();
        shared.shareFrom(hold);
        check(shared.getMaster() == 1 && shared.getBusOverride(1) == 1 && shared.getMorphSource(0) == 2 && shared.getMorphSource(1) == 3
                  && shared.getLive(1) != nullptr && shared.getLive(1)->getContours() != nullptr,
              "routing: a rig that shares the sounds has the same Master, buses and ropes");
        shared.fromValueTree(juce::ValueTree("Cargo"));
        hold.endLending();
    }

    std::cout << (g_failures == 0 ? "ALL PASSED" : juce::String(g_failures) + " FAILED") << std::endl;
    return g_failures == 0 ? 0 : 1;
}
