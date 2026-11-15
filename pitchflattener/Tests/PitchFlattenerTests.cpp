// Pitch Flattener's tests: the locator and the engine (EngineTests.cpp), then the processor and the
// editor as a host would see them.
//
//   PitchFlattenerTests [--png <folder>] [--sound <audio file>]
//
// --png writes pictures of the editor (every state, both views, the banners held and lit, the
// About window and the update popup) into the folder at 2x; --sound learns that file for the
// pictures instead of a synthetic tone. Other arguments are ignored. The exit code is non-zero on
// any failure.

#include <JuceHeader.h>

#include "Banner.h"
#include "Check.h"
#include "FlightView.h"
#include "PluginEditor.h"
#include "PluginProcessor.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

using namespace pftest;
using Processor = PitchFlattenerAudioProcessor;
using Editor = PitchFlattenerAudioProcessorEditor;
using State = pitchflattener::Engine::State;

namespace
{
juce::AudioBuffer<float> passBy (double rate, int channels, double seconds = 2.5)
{
    const auto n = (int) (seconds * rate);
    juce::AudioBuffer<float> b (channels, n);
    const double harmonics[] = { 1.0, 0.6, 0.45, 0.3, 0.25, 0.2, 0.15, 0.12 };
    double phase = 0.0;
    juce::Random random (17);
    for (int i = 0; i < n; ++i)
    {
        const double t = i / rate;
        phase += juce::MathConstants<double>::twoPi * 220.0 * std::exp2 ((3.5 - 7.0 / (1.0 + std::exp (-(t - seconds * 0.5) * 5.0))) / 12.0) / rate;
        double v = 0.0;
        for (int h = 0; h < 8; ++h)
            v += harmonics[h] * std::sin (phase * (h + 1));
        v += 0.02 * (random.nextDouble() * 2.0 - 1.0);
        for (int c = 0; c < channels; ++c)
            b.setSample (c, i, (float) (0.3 * v));
    }
    return b;
}

// As a host does it: the rate and block size first (getSampleRate() reads them), then prepare.
void prepare (Processor& p, double rate, int block)
{
    p.setRateAndBufferSizeDetails (rate, block);
    p.prepareToPlay (rate, block);
}

// Plays a buffer through the processor in blocks, in place; silence before and after if asked.
juce::AudioBuffer<float> play (Processor& p, const juce::AudioBuffer<float>& sound, double before, double after, int block = 512,
                               int fromSample = 0, int toSample = -1)
{
    const double rate = p.getSampleRate();
    const int channels = p.getTotalNumInputChannels();
    const int pre = (int) (before * rate), post = (int) (after * rate);
    const int end = toSample < 0 ? sound.getNumSamples() : toSample;
    juce::AudioBuffer<float> io (channels, pre + (end - fromSample) + post);
    io.clear();
    for (int c = 0; c < channels; ++c)
        io.copyFrom (c, pre, sound, std::min (c, sound.getNumChannels() - 1), fromSample, end - fromSample);

    juce::MidiBuffer midi;
    for (int pos = 0; pos < io.getNumSamples(); pos += block)
    {
        const int n = std::min (block, io.getNumSamples() - pos);
        juce::AudioBuffer<float> view (io.getArrayOfWritePointers(), channels, pos, n);
        p.processBlock (view, midi);
    }
    return io;
}

bool writePngOf (juce::Component& component, const juce::File& folder, const char* name)
{
    const auto image = component.createComponentSnapshot (component.getLocalBounds(), true, 2.0f);
    if (! image.isValid())
        return false;
    if (folder == juce::File())
        return true;
    folder.createDirectory();
    const auto file = folder.getChildFile (name);
    file.deleteFile();
    juce::FileOutputStream stream (file);
    juce::PNGImageFormat png;
    const bool ok = stream.openedOk() && png.writeImageToStream (image, stream);
    if (ok)
        std::printf ("    -> %s\n", file.getFullPathName().toRawUTF8());
    return ok;
}

bool writePng (Editor& editor, const juce::File& folder, const char* name)
{
    editor.refreshNow();
    return writePngOf (editor, folder, name);
}

// Reads a state at one moment and draws it a moment later, so a banner is caught held and lit
// (past its wipe and its first second of blinking).
bool snapshotLit (Editor& editor, double& clock, const juce::File& folder, const char* name, double hold = 1.3)
{
    clock += 0.5;
    editor.setClockForTests (clock);
    editor.refreshNow();
    clock += hold;
    editor.setClockForTests (clock);
    return writePng (editor, folder, name);
}

//==============================================================================
void testFlightMaths()
{
    beginCase ("flight view: projection, height map, dithering, cost");
    flight::Camera cam;
    cam.x = 0.0f; cam.y = 1.0f; cam.z = 0.0f;
    cam.focal = 100.0f; cam.cx = 50.0f; cam.cy = 20.0f;
    const auto ahead = flight::project (cam, { 0.0f, 1.0f, 5.0f });
    check (ahead.inFront && ahead.x == 50 && ahead.y == 20, "a point straight ahead lands on the horizon's centre");
    const auto low = flight::project (cam, { 1.0f, 0.0f, 2.0f });
    check (low.x == 100 && low.y == 70 && std::abs (low.depth - 2.0f) < 1.0e-6f, "a point below and right lands where the pinhole puts it");
    const auto snapped = flight::project (cam, { 0.013f, 1.0f, 3.0f });
    check (snapped.x == 50, "projected corners snap to whole pixels");
    check (! flight::project (cam, { 0.0f, 1.0f, 0.1f }).inFront, "a point nearer than the near plane is not in front");

    check (std::abs (flight::unitsPerSemitone (60.0, 72.0) - flight::kCourseSpan / 12.0) < 1.0e-9, "an octave fills the course span");
    check (std::abs (flight::unitsPerSemitone (60.0, 62.0) - flight::kCourseSpan / 8.0) < 1.0e-9, "a narrow range is not stretched past 8 semitones");

    pitchflattener::core::PitchLine line;
    line.usable = true;
    line.hopSeconds = 0.01;
    line.midi.assign (100, 60.0f);
    line.midi.resize (200, 72.0f);
    line.lowMidi = 60.0;
    line.highMidi = 72.0;
    const auto course = flight::buildCourse (line, true, 72.0, 1.0, 0.05);
    const double ups = flight::kCourseSpan / 12.0;
    check (course.valid && course.hasLane && std::abs (course.seconds - 2.0) < 1.0e-9, "a course as long as the line");
    check (std::abs (course.track[10] - (float) (-6.0 * ups)) < 1.0e-5f && std::abs (course.track[30] - (float) (6.0 * ups)) < 1.0e-5f,
           "the track's height follows the pitch: " + std::to_string (course.track[10]) + ", " + std::to_string (course.track[30]));
    bool level = true;
    for (const auto h : course.lane)
        level = level && std::abs (h - (float) (6.0 * ups)) < 1.0e-5f;
    check (level, "at 100% the lane is dead level at the target");
    const auto half = flight::buildCourse (line, true, 72.0, 0.5, 0.05);
    check (std::abs (half.lane[10]) < 1.0e-5f && std::abs (half.lane[30] - (float) (6.0 * ups)) < 1.0e-5f, "at 50% the lane sits half way back to the pitch");
    check (flight::buildCourse (line, false, 72.0, 1.0).lane.empty(), "nothing flattened: no lane");
    check (std::abs (flight::Course::at (course.track, 0.05, 1.0) - 0.5f * (course.track[19] + course.track[21])) < 0.05f, "heights between slices are interpolated");

    bool inRange = true, repeatable = true;
    double worstMean = 0.0;
    for (int v = 0; v <= 255; ++v)
    {
        double sum = 0.0;
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
            {
                const int q = flight::dither5 (v, x, y);
                inRange = inRange && q >= 0 && q <= 31;
                repeatable = repeatable && q == flight::dither5 (v, x + 4, y + 8);
                sum += q;
            }
        worstMean = std::max (worstMean, std::abs (sum / 16.0 - v * 31.0 / 255.0));
    }
    check (inRange, "dithered channels stay within 5 bits");
    check (repeatable, "dithering is deterministic (the same pattern every 4 pixels)");
    check (worstMean < 0.07, "a 4 x 4 cell averages to the colour: worst " + std::to_string (worstMean));
    check (flight::dither5 (0, 1, 2) == 0 && flight::dither5 (255, 3, 1) == 31 && flight::expand5 (31) == 255 && flight::expand5 (0) == 0,
           "black stays black and white stays white");

    // A whole frame at the view's size, the attract course: something is drawn, and quickly.
    flight::Raster raster;
    raster.setSize (315, 87);
    const auto demo = flight::demoCourse();
    flight::Scene scene;
    scene.course = &demo;
    int distinct = 0;
    flight::Rgb last { -1.0f, -1.0f, -1.0f };
    // The median of many frames: the cost itself, not whatever else the machine is doing.
    std::vector<double> frameTimes;
    for (int f = 0; f < 300; ++f)
    {
        scene.focusSeconds = 3.0 + f / 30.0;
        scene.clockSeconds = f / 30.0;
        const auto start = std::chrono::steady_clock::now();
        flight::render (raster, scene);
        frameTimes.push_back (std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - start).count());
    }
    std::sort (frameTimes.begin(), frameTimes.end());
    const double ms = frameTimes[frameTimes.size() / 2];
    for (int y = 0; y < raster.height(); y += 3)
        for (int x = 0; x < raster.width(); x += 3)
        {
            const auto& c = raster.at (x, y);
            if (std::abs (c.r - last.r) + std::abs (c.g - last.g) + std::abs (c.b - last.b) > 0.01f)
                ++distinct;
            last = c;
        }
    check (distinct > 200, "a frame shows a picture, not a flat fill");
    std::printf ("    -> flight frame (render only, 315 x 87): %.3f ms median\n", ms);
    check (ms < 2.0, "a flight frame renders in well under 2 ms: " + std::to_string (ms));
}

void testBannerLogic()
{
    beginCase ("banners: which one each change raises, one-shots never repeat");
    using Kind = BannerLogic::Kind;
    BannerLogic b;
    BannerLogic::Input in;
    in.state = State::Idle;
    b.update (in, 0.0);
    check (b.kind() == Kind::LearnFirst, "nothing learned: LEARN FIRST");
    in.state = State::Armed;
    b.update (in, 1.0);
    check (b.kind() == Kind::PlayTheSound, "armed: PLAY THE SOUND");
    in.state = State::Listening;
    b.update (in, 2.0);
    check (b.kind() == Kind::Listening, "listening: LISTENING");
    in.state = State::Analysing;
    b.update (in, 3.0);
    check (b.kind() == Kind::Analysing, "analysing: ANALYSING");
    in.state = State::Ready;
    in.learnedId = 5;
    b.update (in, 4.0);
    check (b.kind() == Kind::Learned && std::abs (b.since() - 4.0) < 1.0e-9, "just learned: LEARNED");
    b.update (in, 5.4);
    check (b.kind() == Kind::Learned, "LEARNED holds for 1.5 s");
    b.update (in, 5.6);
    check (b.kind() == Kind::PlayItAgain, "then PLAY IT AGAIN");
    b.update (in, 7.6);
    check (b.kind() == Kind::None, "then nothing (the guidance bar carries on)");
    const int raised = b.raised();
    b.update (in, 9.0);
    b.update (in, 30.0);
    check (b.kind() == Kind::None && b.raised() == raised, "a state that stays the same raises nothing more");

    in.playing = true;
    b.update (in, 31.0);
    check (b.kind() == Kind::Flattened && ! b.flatTag(), "playback starts: FLATTENED");
    b.update (in, 32.1);
    check (b.kind() == Kind::None && b.flatTag(), "then the FLAT tag while it plays");
    in.playing = false;
    b.update (in, 32.2);
    in.playing = true;
    b.update (in, 32.4);
    check (b.kind() == Kind::None && b.flatTag(), "a play position that drops for a moment (a new render) does not flash again");
    in.playing = false;
    b.update (in, 33.0);
    check (! b.flatTag(), "no FLAT tag once it stops");
    in.playing = true;
    b.update (in, 34.0);
    check (b.kind() == Kind::Flattened, "a new playback flashes again");
    in.playing = false;
    b.update (in, 36.0);

    in.rateMismatch = true;
    b.update (in, 37.0);
    check (b.kind() == Kind::WrongSampleRate, "learned at another rate: WRONG SAMPLE RATE");
    b.update (in, 60.0);
    check (b.kind() == Kind::WrongSampleRate, "WRONG SAMPLE RATE stays while it applies");
    in.rateMismatch = false;
    b.update (in, 61.0);
    check (b.kind() == Kind::None, "and goes when it no longer does");

    in.state = State::Idle;
    in.learnedId = 0;
    b.update (in, 62.0);
    check (b.kind() == Kind::Cleared, "cleared: CLEARED");
    b.update (in, 62.9);
    check (b.kind() == Kind::LearnFirst, "then LEARN FIRST");

    in.state = State::Analysing;
    b.update (in, 63.0);
    in.state = State::NothingToFlatten;
    in.learnedId = 6;
    b.update (in, 64.0);
    check (b.kind() == Kind::NoSteadyPitch, "no steady pitch: NO STEADY PITCH");
    b.update (in, 67.1);
    check (b.kind() == Kind::None, "for about 3 s");
    b.update (in, 70.0);
    check (b.kind() == Kind::None, "and not again");

    BannerLogic opened;
    BannerLogic::Input ready;
    ready.state = State::Ready;
    ready.learnedId = 9;
    opened.update (ready, 100.0);
    check (opened.kind() == Kind::None, "an editor opened on a sound learned before does not announce it");
}

//==============================================================================
void testParametersAndState()
{
    beginCase ("processor: parameters, state round trip, layouts, latency");
    Processor a;
    a.setParameterFromEditor (Processor::targetId, 4.0f);
    a.setParameterFromEditor (Processor::noteId, 61.37f);
    a.setParameterFromEditor (Processor::amountId, 40.0f);
    a.setParameterFromEditor (Processor::lineId, 2.0f);
    a.syncOptions();
    const auto o = a.getEngine().options();
    check (o.target == pitchflattener::core::Target::Note && std::abs (o.noteMidi - 61.37) < 1.0e-3 && std::abs (o.amount - 0.4) < 1.0e-6 && o.line == 1,
           "the parameters reach the engine");

    juce::MemoryBlock state;
    a.getStateInformation (state);
    Processor b;
    b.setStateInformation (state.getData(), (int) state.getSize());
    const auto ob = b.optionsFromParameters();
    check (ob.target == o.target && std::abs (ob.noteMidi - o.noteMidi) < 1.0e-3 && std::abs (ob.amount - o.amount) < 1.0e-6 && ob.line == o.line,
           "state round trip");
    check (b.getEngine().options().target == pitchflattener::core::Target::Note, "restored state reaches the engine");
    {
        auto xml = juce::AudioProcessor::getXmlFromBinary (state.getData(), (int) state.getSize());
        check (xml != nullptr && xml->getIntAttribute ("stateVersion") == Processor::stateVersion, "state version 1 saved");
    }
    {
        Processor v;
        check (v.getDisplayView() == Processor::kViewUnset, "no view chosen at first");
        v.setDisplayView (Processor::kViewFlight);
        juce::MemoryBlock saved;
        v.getStateInformation (saved);
        Processor w;
        w.setStateInformation (saved.getData(), (int) saved.getSize());
        check (w.getDisplayView() == Processor::kViewFlight, "the chosen view round-trips in the state");
        check (w.parameters.getParameter ("displayView") == nullptr && w.getParameters().size() == 4, "the view is not a parameter");
        v.setDisplayView (Processor::kViewScope);
        v.getStateInformation (saved);
        w.setStateInformation (saved.getData(), (int) saved.getSize());
        check (w.getDisplayView() == Processor::kViewScope, "and back to SCOPE");
    }
    const char junk[] = "not a state";
    Processor c;
    c.setStateInformation (junk, (int) sizeof (junk));
    check (c.optionsFromParameters().target == pitchflattener::core::Target::HighPoint, "a foreign state is ignored");

    auto* note = a.parameters.getParameter (Processor::noteId);
    check (note->getText (note->convertTo0to1 (69.0f), 32) == "A4", "note shown as A4");
    check (std::abs (note->convertFrom0to1 (note->getValueForText ("C#4 +20")) - 61.2f) < 0.011f, "note typed as C#4 +20");

    using Set = juce::AudioChannelSet;
    const auto layout = [] (Set in, Set out)
    {
        juce::AudioProcessor::BusesLayout l;
        l.inputBuses.add (in);
        l.outputBuses.add (out);
        return l;
    };
    check (a.isBusesLayoutSupported (layout (Set::mono(), Set::mono())), "mono");
    check (a.isBusesLayoutSupported (layout (Set::stereo(), Set::stereo())), "stereo");
    check (! a.isBusesLayoutSupported (layout (Set::mono(), Set::stereo())), "mono in, stereo out refused");
    check (! a.isBusesLayoutSupported (layout (Set::create5point1(), Set::create5point1())), "5.1 refused");

    prepare (a, 48000.0, 512);
    check (a.getLatencySamples() == 19200, "latency 0.4 s at 48 kHz: " + juce::String (a.getLatencySamples()).toStdString());
    prepare (a, 96000.0, 512);
    check (a.getLatencySamples() == 38400, "latency 0.4 s at 96 kHz");
}

void testProcessor()
{
    beginCase ("processor: learn and play through processBlock; bypass");
    Processor p;
    p.setNonRealtime (true);
    prepare (p, 48000.0, 512);
    const auto sound = passBy (48000.0, 2);
    p.getEngine().armLearn();
    play (p, sound, 0.2, 1.3);
    check (p.getEngine().waitUntilIdle (60.0) && p.getEngine().state() == State::Ready, "learned through processBlock");
    const auto r = p.getEngine().result();
    if (r == nullptr || ! r->playable())
        return;
    const auto out = play (p, sound, 0.5, 1.0, 480);
    const int L = p.getLatencySamples();
    // A stretch in the middle of the sound comes out as the flattened sound.
    const auto N = p.getEngine().lastMatchInput();
    check (N >= 0, "recognised");
    bool flat = true;
    const auto F0 = (juce::int64) std::llround (r->flattened.outputSampleFor ((double) p.getEngine().lastMatchSource()));
    const auto base = (juce::int64) (0.2 * 48000.0) + sound.getNumSamples() + (juce::int64) (1.3 * 48000.0);   // the first play's length
    for (int t = 24000 + 1000; t < 24000 + 40000; ++t)
    {
        const auto f = F0 + (base + t - N);
        if (f >= (juce::int64) r->flattened.channels[0].size())
            break;
        flat = flat && identical (out.getSample (0, t + L), r->flattened.channels[0][(size_t) f]);
    }
    check (flat, "the flattened sound comes out, latency later");

    // Bypassed: the input, delayed.
    juce::AudioBuffer<float> io (2, 48000);
    juce::AudioBuffer<float> dry (2, 48000);
    for (int c = 0; c < 2; ++c)
        dry.copyFrom (c, 0, sound, c, 20000, 48000);
    io.makeCopyOf (dry);
    juce::MidiBuffer midi;
    for (int pos = 0; pos < 48000; pos += 512)
    {
        juce::AudioBuffer<float> view (io.getArrayOfWritePointers(), 2, pos, std::min (512, 48000 - pos));
        p.processBlockBypassed (view, midi);
    }
    bool delayed = true;
    for (int t = 0; t + L < 48000; ++t)
        delayed = delayed && identical (io.getSample (1, t + L), dry.getSample (1, t));
    check (delayed, "bypassed: the input delayed by the latency");
    p.reset();
}

juce::AudioBuffer<float> loadSound (const juce::File& soundFile, double& rate)
{
    juce::AudioBuffer<float> sound;
    if (soundFile.existsAsFile())
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        if (std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (soundFile)); reader != nullptr)
        {
            rate = reader->sampleRate;
            sound.setSize ((int) reader->numChannels, (int) reader->lengthInSamples);
            reader->read (&sound, 0, (int) reader->lengthInSamples, 0, true, true);
            std::printf ("    -> learning %s (%.2f s, %d channels, %.0f Hz)\n", soundFile.getFileName().toRawUTF8(),
                         sound.getNumSamples() / rate, sound.getNumChannels(), rate);
        }
    }
    if (sound.getNumSamples() == 0)
        sound = passBy (rate, 2, 4.0);
    return sound;
}

void testEditor (const juce::File& pngFolder, const juce::File& soundFile)
{
    beginCase ("editor: builds, says what to do, flashes each change, and paints each state");
    using Kind = BannerLogic::Kind;
    Processor p;
    p.setNonRealtime (true);

    double rate = 48000.0;
    const auto sound = loadSound (soundFile, rate);

    prepare (p, rate, 512);
    std::unique_ptr<Editor> editor (dynamic_cast<Editor*> (p.createEditor()));
    check (editor != nullptr && editor->getWidth() == Editor::width && editor->getHeight() == Editor::height, "editor size");
    if (editor == nullptr)
        return;

    double clock = 1.5;
    editor->setClockForTests (clock);
    editor->refreshNow();
    check (editor->statusText() == "PRESS LEARN, THEN PLAY THE SOUND ONCE", "nothing learned: says what to do");
    check (editor->bannerKind() == Kind::LearnFirst && editor->learnButtonPulses(), "nothing learned: LEARN FIRST, and Learn pulses");
    check (editor->shownView() == Processor::kViewFlight, "nothing learned: the flight view's attract loop");
    check (writePng (*editor, pngFolder, "empty.png"), "empty snapshot");

    p.getEngine().armLearn();
    editor->refreshNow();
    check (editor->learnButtonText() == "ARMED", "Learn shows ARMED");
    check (editor->bannerKind() == Kind::PlayTheSound && ! editor->learnButtonPulses(), "armed: PLAY THE SOUND, and Learn stops pulsing");
    check (snapshotLit (*editor, clock, pngFolder, "armed.png"), "armed snapshot");
    play (p, sound, 0.2, 0.0, 512, 0, std::min (sound.getNumSamples(), (int) (rate * 1.5)));
    editor->refreshNow();
    check (editor->learnButtonText().startsWith ("LISTENING"), "Learn shows LISTENING and the time: " + editor->learnButtonText().toStdString());
    check (editor->bannerKind() == Kind::Listening, "listening: LISTENING");
    check (snapshotLit (*editor, clock, pngFolder, "listening.png"), "listening snapshot");
    play (p, sound, 0.0, 1.3, 512, std::min (sound.getNumSamples(), (int) (rate * 1.5)));
    editor->refreshNow();
    if (p.getEngine().state() == State::Analysing)
    {
        check (editor->bannerKind() == Kind::Analysing, "analysing: ANALYSING");
        check (snapshotLit (*editor, clock, pngFolder, "analysing.png"), "analysing snapshot");
    }
    else
        std::printf ("    -> (the analysis finished before it could be drawn)\n");
    check (p.getEngine().waitUntilIdle (120.0), "learned");
    editor->refreshNow();
    const auto state = p.getEngine().state();
    check (editor->learnButtonText() == "LEARNED", "Learn shows LEARNED");
    check (state == State::Ready ? editor->statusText() == "LEARNED: PLAY THE SOUND AGAIN TO HEAR IT FLATTENED"
                                 : editor->statusText() == "NO STEADY PITCH FOUND: THE SOUND IS LEFT AS IT IS",
           "learned: " + editor->statusText().toStdString());
    check (editor->shownView() == Processor::kViewScope, "learned: the scope, as nothing was chosen");
    check (snapshotLit (*editor, clock, pngFolder, "learned.png", 1.0), "learned snapshot");
    check (editor->bannerKind() == (state == State::Ready ? Kind::Learned : Kind::NoSteadyPitch), "learned: the banner says so");

    if (state == State::Ready)
    {
        clock += 4.0;   // past LEARNED and PLAY IT AGAIN
        editor->setClockForTests (clock);
        editor->refreshNow();
        check (editor->bannerKind() == Kind::None, "the one-shots have gone");
        editor->chooseView (Processor::kViewFlight);
        check (editor->shownView() == Processor::kViewFlight && p.getDisplayView() == Processor::kViewFlight, "FLIGHT chosen and kept by the processor");
        clock += 0.3;   // past the last banner's wipe out
        editor->setClockForTests (clock);
        check (writePng (*editor, pngFolder, "learned-flight.png"), "learned flight snapshot");

        // Play it again and stop part way, while the flattened sound plays.
        const int upTo = (int) (sound.getNumSamples() * 0.55);
        play (p, sound, 0.3, 0.0, 512, 0, upTo);
        clock += 0.5;
        editor->setClockForTests (clock);
        editor->refreshNow();
        check (p.getEngine().playPosition() >= 0 && editor->statusText() == "PLAYING THE FLATTENED SOUND", "playing: " + editor->statusText().toStdString());
        check (editor->bannerKind() == Kind::Flattened, "playing: FLATTENED");
        clock += 0.22;
        editor->setClockForTests (clock);
        check (writePng (*editor, pngFolder, "flattened.png"), "flattened banner snapshot");
        clock += 1.2;
        editor->setClockForTests (clock);
        editor->refreshNow();
        check (editor->bannerKind() == Kind::None && editor->flatTagShown(), "then the FLAT tag");
        clock += 0.3;

        // The flight view's cost per frame, as the editor draws it (the dither into the image included).
        std::vector<double> flightTimes;
        for (int f = 0; f < 60; ++f)
        {
            editor->setClockForTests (clock + f / 30.0);
            editor->refreshNow();
            flightTimes.push_back (editor->flightFrameMilliseconds());
        }
        editor->setClockForTests (clock);
        std::sort (flightTimes.begin(), flightTimes.end());
        std::printf ("    -> flight frame in the editor: %.3f ms median, %.3f ms worst\n", flightTimes[30], flightTimes.back());
        check (flightTimes[30] < 2.0, "the flight view stays well under 2 ms a frame");

        // The framebuffer's cost: the whole window drawn afresh and shown at 2x (as on a Retina
        // screen), and the areas that animate.
        {
            // Each area is drawn many times; the fastest run is the cost itself, the median what a
            // busy machine adds to it (other processes pre-empting the thread inflate the slow runs).
            const auto timeArea = [&] (juce::Rectangle<int> area, int runs)
            {
                std::vector<double> times;
                for (int i = 0; i < runs; ++i)
                {
                    const auto t0 = std::chrono::steady_clock::now();
                    const auto img = editor->createComponentSnapshot (area, true, 2.0f);
                    times.push_back (std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count());
                }
                std::sort (times.begin(), times.end());
                return std::pair<double, double> { times.front(), times[times.size() / 2] };
            };
            editor->refreshNow();
            const auto [fullFastest, fullMedian] = timeArea (editor->getLocalBounds(), 20);
            std::printf ("    -> full-window repaint at 2x: %.2f ms fastest, %.2f ms median (framebuffer alone %.2f ms)\n",
                         fullFastest, fullMedian, editor->framebufferMilliseconds());
            check (fullFastest < 8.0, "a full-window repaint stays under 8 ms: " + std::to_string (fullFastest));
            const auto [flightFastest, flightMedian] = timeArea ({ 19, 324, 942, 257 }, 30);
            const double flightFramebuffer = editor->framebufferMilliseconds();
            const auto [bandFastest, bandMedian] = timeArea ({ 19, 380, 942, 120 }, 30);
            const double bandFramebuffer = editor->framebufferMilliseconds();
            const auto [learnFastest, learnMedian] = timeArea ({ 16, 74, 330, 64 }, 30);
            const double learnFramebuffer = editor->framebufferMilliseconds();
            const auto [guideFastest, guideMedian] = timeArea ({ 16, 464, 60, 40 }, 30);
            const double guideFramebuffer = editor->framebufferMilliseconds();
            std::printf ("    -> animated repaints at 2x, fastest / median (framebuffer alone): flight view %.2f / %.2f (%.2f) ms, "
                         "banner band %.2f / %.2f (%.2f) ms, Learn pulse %.2f / %.2f (%.2f) ms, guidance marker %.2f / %.2f (%.2f) ms\n",
                         flightFastest, flightMedian, flightFramebuffer, bandFastest, bandMedian, bandFramebuffer,
                         learnFastest, learnMedian, learnFramebuffer, guideFastest, guideMedian, guideFramebuffer);
        }

        check (writePng (*editor, pngFolder, "playing-flight.png"), "playing flight snapshot");
        editor->chooseView (Processor::kViewScope);
        check (writePng (*editor, pngFolder, "playing.png"), "playing snapshot");

        // Each line and target: no flash for either.
        p.setParameterFromEditor (Processor::targetId, 2.0f);
        p.setParameterFromEditor (Processor::lineId, p.getEngine().result()->options.line == 0 ? 2.0f : 1.0f);
        p.syncOptions();
        p.getEngine().waitUntilIdle (60.0);
        play (p, sound, 0.0, 0.0, 512, upTo, std::min (sound.getNumSamples(), upTo + 4800));
        clock += 0.3;
        editor->setClockForTests (clock);
        editor->refreshNow();
        check (editor->bannerKind() == Kind::None, "another line and target: no banner");
        clock += 0.3;
        editor->setClockForTests (clock);
        check (writePng (*editor, pngFolder, "other-line.png"), "other line snapshot");

        prepare (p, rate * 2.0, 512);
        juce::AudioBuffer<float> quiet (2, 1024);
        quiet.clear();
        juce::MidiBuffer midi;
        p.processBlock (quiet, midi);
        editor->refreshNow();
        const auto want = "LEARNED AT " + juce::String (juce::roundToInt (rate)) + " HZ: THE SESSION IS AT " + juce::String (juce::roundToInt (rate * 2.0)) + " HZ";
        check (editor->statusText() == want, "another rate: " + editor->statusText().toStdString());
        check (snapshotLit (*editor, clock, pngFolder, "other-rate.png"), "rate snapshot");
        check (editor->bannerKind() == Kind::WrongSampleRate, "another rate: WRONG SAMPLE RATE");
    }

    p.getEngine().clear();
    clock += 0.5;
    editor->setClockForTests (clock);
    editor->refreshNow();
    check (editor->statusText() == "PRESS LEARN, THEN PLAY THE SOUND ONCE", "cleared");
    check (editor->bannerKind() == Kind::Cleared, "cleared: CLEARED");
    clock += 1.0;
    editor->setClockForTests (clock);
    editor->refreshNow();
    check (editor->bannerKind() == Kind::LearnFirst, "then LEARN FIRST");

    // The update popup over the editor, and the About window.
    clock += 1.3;
    editor->setClockForTests (clock);
    {
        sammyj::UpdateInfo info;
        info.pluginId = "pitchflattener";
        info.currentVersion = "3.0.0";
        info.latestVersion = "3.1.0";
        editor->showUpdatePopup (info);
        check (writePng (*editor, pngFolder, "update.png"), "update popup snapshot");
    }
    editor->showAbout();
    check (editor->aboutContent() != nullptr && writePngOf (*editor->aboutContent(), pngFolder, "about.png"), "About snapshot");
    editor.reset();

    // A sound with no pitch to flatten: noise.
    {
        Processor q;
        q.setNonRealtime (true);
        prepare (q, 48000.0, 512);
        juce::AudioBuffer<float> noise (2, 48000 * 2);
        juce::Random random (3);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < noise.getNumSamples(); ++i)
                noise.setSample (c, i, 0.3f * (random.nextFloat() * 2.0f - 1.0f));
        std::unique_ptr<Editor> noisy (dynamic_cast<Editor*> (q.createEditor()));
        double t = 1.0;
        noisy->setClockForTests (t);
        noisy->refreshNow();
        q.getEngine().armLearn();
        play (q, noise, 0.1, 1.3);
        q.getEngine().waitUntilIdle (60.0);
        noisy->refreshNow();
        if (q.getEngine().state() == State::NothingToFlatten)
        {
            check (noisy->bannerKind() == Kind::NoSteadyPitch, "noise: NO STEADY PITCH");
            check (snapshotLit (*noisy, t, pngFolder, "no-steady-pitch.png", 1.2), "no steady pitch snapshot");
            check (noisy->statusText() == "NO STEADY PITCH FOUND: THE SOUND IS LEFT AS IT IS", "noise: the guidance explains");
        }
        else
            check (false, "noise was expected to have no steady pitch");
    }

    check (Editor::textOverflowCount() == 0, "every text fits its box: " + std::to_string (Editor::textOverflowCount()) + " did not");
}
} // namespace

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    juce::File pngFolder, soundFile;
    for (int i = 1; i < argc; ++i)
    {
        const juce::String arg (argv[i]);
        if (arg == "--png" && i + 1 < argc)
            pngFolder = juce::File (juce::String (argv[++i]));
        else if (arg == "--sound" && i + 1 < argc)
            soundFile = juce::File (juce::String (argv[++i]));
    }

    const auto start = std::chrono::steady_clock::now();
    runLocatorTests();
    runEngineTests();
    testParametersAndState();
    testFlightMaths();
    testBannerLogic();
    testProcessor();
    testEditor (pngFolder, soundFile);

    std::printf ("\n%d checks, %d failed (%.1f s)\n", g_checks, g_failures,
                 std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count());
    return g_failures == 0 ? 0 : 1;
}
