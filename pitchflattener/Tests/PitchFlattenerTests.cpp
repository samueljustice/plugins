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

// Repaints a sequence of partial areas and checks the framebuffer ends up exactly as one full
// repaint of the same state leaves it: a partial repaint that draws differently from its
// neighbours (another block grid, clip or mask) leaves seams on screen.
int partialRepaintMismatches (Editor& editor, std::string& where)
{
    editor.renderForTests (editor.getLocalBounds());
    const auto full = editor.framebufferForTests().createCopy();

    std::vector<juce::Rectangle<int>> areas {
        { 19, 380, 942, 120 }, { 16, 74, 330, 64 }, { 205, 111, 261, 47 }, { 133, 123, 3, 13 }, { 16, 464, 60, 40 }, { 19, 178, 942, 257 }
    };
    for (int x = 301; x < 940; x += 37)
        areas.push_back ({ x, 150, 21, 290 });   // play-head strips
    juce::Random random (11);
    for (int i = 0; i < 60; ++i)
        areas.push_back ({ random.nextInt (970), random.nextInt (650), 1 + random.nextInt (300), 1 + random.nextInt (200) });
    for (const auto& a : areas)
        editor.renderForTests (a.getIntersection (editor.getLocalBounds()));

    const auto& now = editor.framebufferForTests();
    int mismatches = 0;
    for (int y = 0; y < full.getHeight(); ++y)
        for (int x = 0; x < full.getWidth(); ++x)
            if (full.getPixelAt (x, y) != now.getPixelAt (x, y))
            {
                if (mismatches++ == 0)
                    where = "first at " + std::to_string (x) + ", " + std::to_string (y);
            }
    return mismatches;
}


// The screen as a host shows it: only the areas JUCE asks to repaint are drawn, each from the
// framebuffer as it is after drawing that area, and the follow-up repaints the editor asks for are
// carried out too. After a change, repainting just the changed area (odd edges included) must
// leave the screen exactly as a full repaint would.
struct ScreenModel
{
    juce::Image screen;

    void paint (Editor& editor, juce::Rectangle<int> area)
    {
        area = area.getIntersection (editor.getLocalBounds());
        while (! area.isEmpty())
        {
            editor.renderForTests (area);
            juce::Graphics g (screen);
            g.drawImage (editor.framebufferForTests().getClippedImage (area), area.toFloat());
            area = editor.followUpRepaint (area);
        }
    }

    int mismatches (Editor& editor, std::string& where)
    {
        editor.renderForTests (editor.getLocalBounds());
        const auto& full = editor.framebufferForTests();
        int bad = 0;
        for (int y = 0; y < full.getHeight(); ++y)
            for (int x = 0; x < full.getWidth(); ++x)
                if (full.getPixelAt (x, y) != screen.getPixelAt (x, y) && bad++ == 0)
                    where = "first at " + std::to_string (x) + ", " + std::to_string (y);
        return bad;
    }
};

ScreenModel fullScreen (Editor& editor)
{
    ScreenModel m;
    m.screen = juce::Image (juce::Image::ARGB, editor.getWidth(), editor.getHeight(), true, juce::SoftwareImageType());
    m.paint (editor, editor.getLocalBounds());
    return m;
}

// An area widened to odd edges, as a host's dirty area can be.
juce::Rectangle<int> oddEdges (juce::Rectangle<int> r)
{
    const int x0 = (r.getX() & 1) == 0 ? r.getX() - 1 : r.getX(), y0 = (r.getY() & 1) == 0 ? r.getY() - 1 : r.getY();
    const int x1 = (r.getRight() & 1) == 0 ? r.getRight() + 1 : r.getRight(), y1 = (r.getBottom() & 1) == 0 ? r.getBottom() + 1 : r.getBottom();
    return { x0, y0, x1 - x0, y1 - y0 };
}

void checkPartialRepaints (Editor& editor, const char* state)
{
    std::string where;
    const int bad = partialRepaintMismatches (editor, where);
    check (bad == 0, std::string (state) + ": partial repaints match a full one (" + std::to_string (bad) + " pixels differ, " + where + ")");
}


//==============================================================================
juce::AudioBuffer<float> loadSound (const juce::File& soundFile, double& rate);

// Mouse events as JUCE would deliver them to a component.
juce::MouseEvent mouseAt (juce::Component& c, juce::Point<float> p, juce::Point<float> downAt, juce::ModifierKeys mods = {},
                          int clicks = 1, bool dragged = false)
{
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), p, mods, juce::MouseInputSource::defaultPressure,
                             juce::MouseInputSource::defaultOrientation, juce::MouseInputSource::defaultRotation,
                             juce::MouseInputSource::defaultTiltX, juce::MouseInputSource::defaultTiltY, &c, &c, now, downAt, now, clicks, dragged);
}

void dragOn (juce::Component& c, juce::Point<float> from, juce::Point<float> to, int steps = 8, juce::ModifierKeys mods = {})
{
    c.mouseDown (mouseAt (c, from, from, mods));
    for (int i = 1; i <= steps; ++i)
        c.mouseDrag (mouseAt (c, from + (to - from) * ((float) i / (float) steps), from, mods, 1, true));
    c.mouseUp (mouseAt (c, to, from, mods, 1, true));
}

// Learns a sound through the processor, in full, and waits for its render.
bool learnSound (Processor& p, const juce::AudioBuffer<float>& sound)
{
    p.getEngine().armLearn();
    play (p, sound, 0.2, 1.3);
    return p.getEngine().waitUntilIdle (120.0) && p.getEngine().result() != nullptr;
}

std::vector<float> flattenedChannel (const Processor& p)
{
    const auto r = p.getEngine().result();
    return r != nullptr && r->playable() ? r->flattened.channels[0] : std::vector<float>();
}

bool sameSamples (const std::vector<float>& a, const std::vector<float>& b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (! identical (a[i], b[i]))
            return false;
    return true;
}

void testSourceLengthAndEdits()
{
    beginCase ("processor: source, length and character; corrections render and come off exactly");
    {
        Processor a;
        a.setParameterFromEditor (Processor::sourceId, 3.0f);
        a.setParameterFromEditor (Processor::keepLengthId, 1.0f);
        a.setParameterFromEditor (Processor::characterId, 1.0f);
        a.syncOptions();
        const auto o = a.getEngine().options();
        check (o.line == 2 && o.keepLength && o.character == pitchflattener::core::Character::Transients, "source Tone, Keep length and Transients reach the engine");
        juce::MemoryBlock state;
        a.getStateInformation (state);
        Processor b;
        b.setStateInformation (state.getData(), (int) state.getSize());
        const auto ob = b.optionsFromParameters();
        check (ob.line == 2 && ob.keepLength && ob.character == pitchflattener::core::Character::Transients, "source, length and character round-trip in the state");
        check (b.parameters.getParameter ("line") == nullptr, "the old line parameter is gone");
        Processor c;
        check (c.optionsFromParameters().line == -1 && ! c.optionsFromParameters().keepLength
               && c.optionsFromParameters().character == pitchflattener::core::Character::Harmonic, "defaults: Auto, Varispeed, Harmonic");
    }

    Processor p;
    p.setNonRealtime (true);
    prepare (p, 48000.0, 512);
    check (learnSound (p, passBy (48000.0, 2, 2.5)), "learned for the corrections");
    const int line = p.lineInUse();
    check (line >= 0 && line == p.getEngine().result()->options.line, "the reading in use is the one rendered");
    if (line < 0)
        return;
    const auto original = flattenedChannel (p);

    p.setEdits (line, { { 0.8, 2.0 }, { 1.4, 2.0 } });
    check (p.getEngine().waitUntilIdle (60.0), "rendered with the corrections");
    const auto corrected = flattenedChannel (p);
    check (! corrected.empty() && ! sameSamples (corrected, original), "a correction changes the flattened sound");
    check (p.getEngine().result()->options.edits.size() == 2, "the corrections are in the render's options");

    // Another reading has its own (none yet); coming back finds them as they were.
    int other = -1;
    for (int i = 0; i < pitchflattener::core::kNumTrackers; ++i)
        if (i != line && p.getEngine().result()->learned->analysis.lines[(size_t) i].usable)
            other = i;
    if (other >= 0)
    {
        p.setParameterFromEditor (Processor::sourceId, (float) (other + 1));
        p.syncOptions();
        check (p.lineInUse() == other && p.getEdits (other).empty() && p.getEngine().options().edits.empty(), "another reading starts with no corrections");
        p.setParameterFromEditor (Processor::sourceId, (float) (line + 1));
        p.syncOptions();
        check (p.getEdits (line).size() == 2, "and the first reading's corrections are kept for it");
        p.getEngine().waitUntilIdle (60.0);
    }

    p.setEdits (line, {});
    check (p.getEngine().waitUntilIdle (60.0), "rendered without them");
    check (sameSamples (flattenedChannel (p), original), "taking the corrections off restores the flattened sound exactly");

    p.setEdits (line, { { 1.0, -1.0 } });
    p.getEngine().clear();
    p.syncOptions();
    check (p.getEdits (line).empty() && p.getEngine().options().edits.empty(), "Clear forgets the corrections");
}

void testCurveEditor (const juce::File& pngFolder, const juce::File& soundFile)
{
    beginCase ("curve editor: pull, snap, draw, undo, redo, reset, zoom, choosing a reading");
    using Kind = BannerLogic::Kind;
    Processor p;
    p.setNonRealtime (true);
    double rate = 48000.0;
    const auto sound = loadSound (soundFile, rate);
    prepare (p, rate, 512);
    std::unique_ptr<Editor> editor (dynamic_cast<Editor*> (p.createEditor()));
    double clock = 1.0;
    editor->setClockForTests (clock);
    check (learnSound (p, sound) && p.getEngine().state() == State::Ready, "learned for the editor");
    if (p.getEngine().state() != State::Ready)
        return;
    editor->refreshNow();   // the announcement starts here
    clock += 10.0;          // and is over by now
    editor->setClockForTests (clock);
    editor->refreshNow();
    editor->chooseView (Processor::kViewScope);
    clock += 1.0;           // past the last banner's wipe
    editor->setClockForTests (clock);
    editor->refreshNow();
    auto& d = editor->scopeForTests();
    const int line = p.lineInUse();
    check (line >= 0 && editor->bannerKind() == Kind::None, "the scope, with a reading in use and no banner");

    // Three tags, none covering another.
    {
        const auto tags = d.tagAreas();
        int shown = 0;
        bool apart = true;
        for (int i = 0; i < 3; ++i)
            if (! tags[(size_t) i].isEmpty())
            {
                ++shown;
                for (int j = i + 1; j < 3; ++j)
                    apart = apart && (tags[(size_t) j].isEmpty() || ! tags[(size_t) i].expanded (1.0f).intersects (tags[(size_t) j]));
            }
        check (shown == 3 && apart, "three reading tags, none covering another: " + std::to_string (shown));
    }
    check (writePng (*editor, pngFolder, "learned-scope.png"), "learned scope snapshot");
    if (pngFolder != juce::File())
    {
        // Where everything is in the pictures (2x), for cropping them.
        const auto at2x = [] (juce::Rectangle<int> r) { return (r * 2).toString().toStdString(); };
        for (const auto& [name, r] : editor->panelsForTests())
            std::printf ("    -> layout %s: %s\n", name.toRawUTF8(), at2x (r).c_str());
        for (auto* child : editor->getChildren())
            if (auto* b = dynamic_cast<juce::Button*> (child))
                std::printf ("    -> layout button %s: %s%s\n", b->getButtonText().toRawUTF8(), at2x (b->getBounds()).c_str(), b->isVisible() ? "" : " (hidden here)");
            else if (auto* sl = dynamic_cast<juce::Slider*> (child))
                std::printf ("    -> layout gauge %s: %s\n", sl->getName().toRawUTF8(), at2x (sl->getBounds()).c_str());
        std::printf ("    -> layout scope overview bar: %s\n", at2x (d.overviewArea().toNearestInt() + d.getPosition()).c_str());
    }

    // Zoom: the moment under the pointer stays put; limits; pan clamps; FIT restores.
    {
        const double dur = d.durationSeconds(), t = dur * 0.3;
        const float x = d.xFor (t);
        d.zoomTime (t, 0.25);
        check (std::abs (d.xFor (t) - x) < 0.01f && std::abs ((d.viewTo() - d.viewFrom()) - dur * 0.25) < 1.0e-9, "zooming time keeps the moment under the pointer");
        d.zoomTime (t, 1.0e-6);
        check (std::abs ((d.viewTo() - d.viewFrom()) - PitchDisplay::kMinSpanSeconds) < 1.0e-9 && std::abs (d.xFor (t) - x) < 0.5f,
               "zoomed in as far as 50 ms, about the same moment");
        d.panTime (1.0e6);
        check (std::abs (d.viewTo() - dur) < 1.0e-9, "panning stops at the end");
        d.panTime (-1.0e6);
        check (std::abs (d.viewFrom()) < 1.0e-9, "and at the start");
        const double mid = 0.5 * (d.lowMidi() + d.highMidi());
        const float y = d.yFor (mid);
        d.zoomPitch (mid, 1.0e-6);
        check (std::abs ((d.highMidi() - d.lowMidi()) - PitchDisplay::kMinSemitones) < 1.0e-9 && std::abs (d.yFor (mid) - y) < 0.5f,
               "pitch zooms in to two semitones about the pointer");
        d.fit();
        check (! d.isZoomed() && std::abs (d.viewTo() - dur) < 1.0e-9, "FIT shows the whole sound again");
    }

    // Pull: drag the line up 30 px at a moment.
    const auto plot = d.plotArea();
    // (A moment where the line sits low enough for the drag to stay on the graph.)
    double t0 = d.durationSeconds() * 0.45;
    for (double f = 0.3; f < 0.9; f += 0.01)
        if (d.correctedAt (d.durationSeconds() * f) < d.lowMidi() + 0.6 * (d.highMidi() - d.lowMidi()))
        {
            t0 = d.durationSeconds() * f;
            break;
        }
    const juce::Point<float> at0 { d.xFor (t0), d.yFor (d.correctedAt (t0)) };
    const double perPixel = (d.highMidi() - d.lowMidi()) / plot.getHeight();
    dragOn (d, at0, at0.translated (0.0f, -30.0f));
    editor->refreshNow();
    {
        const auto e = p.getEdits (line);
        check (e.size() == 1 && std::abs (e[0].seconds - t0) < d.durationSeconds() / plot.getWidth()
               && std::abs (e[0].semitones - 30.0 * perPixel) < 0.01, "pulling the line adds a point there, raised by the drag");
        check (editor->canUndo(), "and it can be undone");
    }
    editor->undoEdit();
    check (p.getEdits (line).empty() && editor->canRedo(), "undo takes it off");
    editor->redoEdit();
    check (p.getEdits (line).size() == 1, "redo puts it back");

    // Snap: the corrected pitch lands on a semitone.
    d.setSnap (true);
    const double t1 = d.durationSeconds() * 0.7;
    const juce::Point<float> at1 { d.xFor (t1), d.yFor (d.correctedAt (t1)) };
    dragOn (d, at1, at1.translated (0.0f, -17.0f));
    editor->refreshNow();
    {
        const double pitch = d.correctedAt (t1);
        check (p.getEdits (line).size() == 2 && std::abs (pitch - std::round (pitch)) < 1.0e-4, "with snap, the pitch lands on a semitone: " + std::to_string (pitch));
    }
    d.setSnap (false);

    // A double-click on a point removes it.
    {
        const auto e = p.getEdits (line);
        const juce::Point<float> pt { d.xFor (e[1].seconds), d.yFor (d.correctedAt (e[1].seconds)) };
        d.mouseDoubleClick (mouseAt (d, pt, pt, {}, 2));
        editor->refreshNow();
        check (p.getEdits (line).size() == 1, "a double-click removes a point");
    }
    check (writePng (*editor, pngFolder, "edited.png"), "edited snapshot");

    // Draw: a freehand stroke replaces the points under it.
    d.setDrawMode (true);
    {
        const float xa = d.xFor (d.durationSeconds() * 0.15), xb = d.xFor (d.durationSeconds() * 0.35);
        const float yDrawn = plot.getY() + plot.getHeight() * 0.3f;
        dragOn (d, { xa, yDrawn }, { xb, yDrawn }, 60);
        editor->refreshNow();
        const auto e = p.getEdits (line);
        const double mid = d.durationSeconds() * 0.25;
        check (e.size() > 10 && e.size() <= Processor::kMaxEdits, "a stroke leaves a line of points: " + std::to_string (e.size()));
        check (std::abs (d.correctedAt (mid) - d.midiFor (yDrawn)) < 0.15, "the line follows the stroke");
    }
    check (writePng (*editor, pngFolder, "draw.png"), "draw mode snapshot");
    d.setDrawMode (false);

    editor->resetCurve();
    check (p.getEdits (line).empty(), "reset curve takes every correction off");
    editor->undoEdit();
    check (! p.getEdits (line).empty(), "and undo brings them back");

    // Zoomed far in, a point goes exactly where it is put.
    {
        const double t = d.durationSeconds() * 0.55;
        d.zoomTime (t, 1.0e-6);
        d.zoomPitch (d.correctedAt (t), 0.2);
        const auto before = p.getEdits (line).size();
        const juce::Point<float> at { d.xFor (t) + 3.0f, d.yFor (d.correctedAt (t)) };
        const double wanted = d.secondsFor (at.x);
        dragOn (d, at, at.translated (0.0f, -10.0f), 8, juce::ModifierKeys::shiftModifier);
        editor->refreshNow();
        const auto e = p.getEdits (line);
        bool found = false;
        for (const auto& q : e)
            found = found || std::abs (q.seconds - wanted) < 0.001;
        check (e.size() == before + 1 && found, "zoomed in, a point lands within a millisecond of where it was put");
        check (writePng (*editor, pngFolder, "zoomed.png"), "zoomed snapshot");
        d.fit();
    }

    // Clicking another reading's tag chooses it; its own corrections (none) apply, the first's are kept.
    {
        const auto tags = d.tagAreas();
        int other = -1;
        for (int i = 0; i < 3; ++i)
            if (i != line && ! tags[(size_t) i].isEmpty() && p.getEngine().result()->learned->analysis.lines[(size_t) i].usable)
                other = i;
        if (other >= 0)
        {
            const auto c = tags[(size_t) other].getCentre();
            d.mouseDown (mouseAt (d, c, c));
            d.mouseUp (mouseAt (d, c, c));
            editor->refreshNow();
            check (p.lineInUse() == other && p.getEdits (other).empty(), "a tag chooses its reading");
            const auto back = d.tagAreas()[(size_t) line].getCentre();
            d.mouseDown (mouseAt (d, back, back));
            d.mouseUp (mouseAt (d, back, back));
            editor->refreshNow();
            check (p.lineInUse() == line && ! p.getEdits (line).empty(), "and coming back finds the corrections kept");
        }
        else
            std::printf ("    -> (only one usable reading: no switch to test)\n");
    }
    p.getEngine().waitUntilIdle (60.0);

    // Keep length, with a character.
    p.setParameterFromEditor (Processor::keepLengthId, 1.0f);
    p.setParameterFromEditor (Processor::characterId, 1.0f);
    p.syncOptions();
    check (p.getEngine().waitUntilIdle (120.0), "rendered with Keep length");
    editor->refreshNow();
    check (writePng (*editor, pngFolder, "keep-length.png"), "keep length snapshot");
    check (Editor::textOverflowCount() == 0, "curve editor: every text fits its box");
}

void testSeveralSounds (const juce::File& pngFolder)
{
    beginCase ("several sounds in one recording: each flattened to its own pitch");
    Processor p;
    p.setNonRealtime (true);
    prepare (p, 48000.0, 512);
    // Two pass-bys a fifth apart, half a second of silence between them.
    const auto a = passBy (48000.0, 2, 2.0);
    juce::AudioBuffer<float> both (2, a.getNumSamples() * 2 + 24000);
    both.clear();
    for (int c = 0; c < 2; ++c)
    {
        both.copyFrom (c, 0, a, c, 0, a.getNumSamples());
        for (int i = 0; i < a.getNumSamples(); ++i)   // the second a fifth higher: every other sample of a longer pass-by
            both.setSample (c, a.getNumSamples() + 24000 + i, a.getSample (c, (i * 3 / 2) % a.getNumSamples()));
    }
    std::unique_ptr<Editor> editor (dynamic_cast<Editor*> (p.createEditor()));
    editor->setClockForTests (1.0);
    check (learnSound (p, both), "learned the recording");
    const auto r = p.getEngine().result();
    if (r == nullptr || ! r->playable())
        return;
    const auto events = r->learned->analysis.events.size();
    check (events == 2 && r->flattened.eventTargets.size() == 2, "two separate sounds, a target each: " + std::to_string (events));
    editor->setClockForTests (20.0);
    editor->refreshNow();
    editor->setClockForTests (30.0);
    editor->chooseView (Processor::kViewScope);
    editor->refreshNow();
    check (editor->flatOnText() == (events > 1 ? juce::String ("SEVERAL ") + juce::String ((int) events) : PitchFlattenerAudioProcessor::noteText (r->flattened.targetMidi)),
           "FLAT ON: " + editor->flatOnText().toStdString());
    check (editor->scopeForTests().targetSpans().size() == events, "the scope draws one target segment per sound");
    editor->setClockForTests (31.0);   // past the banner's wipe
    check (writePng (*editor, pngFolder, "several-sounds.png"), "several sounds snapshot");
    check (Editor::textOverflowCount() == 0, "several sounds: every text fits its box");
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
    check (ms < 20.0, "a flight frame renders in a few milliseconds at most (about 0.1 ms on an idle machine; the limit only catches it becoming many times slower): " + std::to_string (ms));
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
    a.setParameterFromEditor (Processor::sourceId, 2.0f);
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
        check (w.parameters.getParameter ("displayView") == nullptr && w.getParameters().size() == 6, "the view is not a parameter");
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
    checkPartialRepaints (*editor, "nothing learned");

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
    checkPartialRepaints (*editor, "learned");
    check (editor->bannerKind() == (state == State::Ready ? Kind::Learned : Kind::NoSteadyPitch), "learned: the banner says so");
    {
        // A tooltip comes and goes over the Learn button; the screen repaints only where it was.
        auto screen = fullScreen (*editor);
        editor->showTooltipForTests (editor->getScreenPosition() + juce::Point<int> (133, 97), "Learn: a tooltip long enough to run past the button");
        const auto tip = editor->tooltipBoundsForTests();
        screen.paint (*editor, oddEdges (tip));
        // While it shows, the Learn button and the display repaint under and around it (a hover,
        // the play head).
        screen.paint (*editor, { 16, 74, 330, 64 });
        screen.paint (*editor, { 19, 148, 942, 290 });
        editor->hideTooltipForTests();
        screen.paint (*editor, oddEdges (tip));
        std::string where;
        const int bad = screen.mismatches (*editor, where);
        check (! tip.isEmpty() && bad == 0, "a tooltip that came and went leaves nothing behind (" + std::to_string (bad) + " pixels, " + where + ")");

        // The LEARNED banner goes; only the band's area is repainted.
        auto screen2 = fullScreen (*editor);
        clock += 4.0;
        editor->setClockForTests (clock);
        editor->refreshNow();
        screen2.paint (*editor, oddEdges ({ 19, 300, 942, 140 }));
        const int bad2 = screen2.mismatches (*editor, where);
        check (bad2 == 0, "a banner that went leaves nothing behind (" + std::to_string (bad2) + " pixels, " + where + ")");
    }

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
        check (flightTimes[30] < 20.0, "the flight view stays cheap to draw (the limit allows for a busy machine)");

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
            check (fullFastest < 40.0, "a full-window repaint stays cheap (about 5 ms on an idle machine; the limit allows for a busy one): " + std::to_string (fullFastest));
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
        checkPartialRepaints (*editor, "playing, flight");
        editor->chooseView (Processor::kViewScope);
        check (writePng (*editor, pngFolder, "playing.png"), "playing snapshot");
        checkPartialRepaints (*editor, "playing, scope");

        // Each line and target: no flash for either.
        p.setParameterFromEditor (Processor::targetId, 2.0f);
        p.setParameterFromEditor (Processor::sourceId, p.getEngine().result()->options.line == 0 ? 2.0f : 1.0f);
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
        checkPartialRepaints (*editor, "another rate");
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
    testSourceLengthAndEdits();
    testCurveEditor (pngFolder, soundFile);
    testSeveralSounds (pngFolder);

    std::printf ("\n%d checks, %d failed (%.1f s)\n", g_checks, g_failures,
                 std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count());
    return g_failures == 0 ? 0 : 1;
}
