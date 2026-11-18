#include "DisplayComponent.h"

#include "Parameters.h"
#include "PluginProcessor.h"
#include "Theme.h"

namespace
{
    using dehonk::scene::PondScene;
    constexpr double settleAfter = 0.25;         // seconds without a new frame before the held one falls away
}

DisplayComponent::DisplayComponent (DeHonkAudioProcessor& p)
    : processor (p),
      // A software image: on macOS a native image asked for as RGB is made ARGB, which the scene refuses.
      picture (juce::Image::RGB, PondScene::kWidth, PondScene::kHeight, true, juce::SoftwareImageType())
{
    auto& apvts = p.getParameters();
    for (int e = 0; e < dehonk::kNumEdges; ++e)
        edges[(size_t) e] = apvts.getParameter (ParamIDs::edges[e]);
    for (int m = 0; m < dehonk::kNumModules; ++m)
        switches[(size_t) m] = apvts.getParameter (ParamIDs::moduleOn[m]);
    snapSwitch = apvts.getParameter (ParamIDs::snapOn);
    threshold = apvts.getParameter (ParamIDs::threshold);
    bypass = apvts.getParameter (ParamIDs::bypass);
    maxCut = apvts.getParameter (ParamIDs::maxCut);
    impactGuard = apvts.getParameter (ParamIDs::impactGuard);

    setInterceptsMouseClicks (false, false);

    // A first picture, so the window never shows an empty panel.
    runScene (0.0);
    lastValues = watchedValues();
}

DisplayComponent::~DisplayComponent() { stopTimer(); }

void DisplayComponent::setActive (bool shouldBeActive)
{
    if (! shouldBeActive)
    {
        stopTimer();
        rateHz = 0;
        lastTickMs = 0.0;
    }
    else if (! isTimerRunning())
    {
        rateHz = 30;
        startTimerHz (rateHz);
    }
}

float DisplayComponent::value (const juce::RangedAudioParameter& p) const noexcept { return p.convertFrom0to1 (p.getValue()); }
bool DisplayComponent::moduleOn (int m) const noexcept { return switches[(size_t) m]->getValue() >= 0.5f; }

std::array<float, dehonk::kNumEdges> DisplayComponent::edgesAsUsed() const noexcept
{
    std::array<float, dehonk::kNumEdges> raw {};
    for (size_t i = 0; i < raw.size(); ++i)
        raw[i] = value (*edges[i]);
    return ParamRanges::edgesAsUsed (raw);
}

//==============================================================================
dehonk::scene::SceneInput DisplayComponent::gatherInput() const
{
    dehonk::scene::SceneInput in;
    if (hasFrame)
        in.frame = frame;
    for (int m = 0; m < dehonk::kNumModules; ++m)
        in.moduleOn[m] = moduleOn (m);
    in.snapOn = snapSwitch->getValue() >= 0.5f;
    const auto used = edgesAsUsed();
    for (int e = 0; e < dehonk::kNumEdges; ++e)
        in.edgeHz[e] = used[(size_t) e];
    in.thresholdDb = value (*threshold);
    in.maxCutDb = value (*maxCut);
    in.guardAmount = impactGuard->getValue();
    in.soloHonk = processor.isSoloHonk();
    in.diff = processor.isDiff();
    in.bypass = bypass->getValue() >= 0.5f;
    in.calm = false;
    return in;
}

std::array<float, DisplayComponent::numWatched> DisplayComponent::watchedValues() const
{
    std::array<float, numWatched> now {};
    size_t i = 0;
    for (auto* p : edges)    now[i++] = p->getValue();
    for (auto* p : switches) now[i++] = p->getValue();
    now[i++] = snapSwitch->getValue();
    now[i++] = threshold->getValue();
    now[i++] = bypass->getValue();
    now[i++] = maxCut->getValue();
    now[i++] = impactGuard->getValue();
    now[i++] = processor.isSoloHonk() ? 1.0f : 0.0f;
    now[i] = processor.isDiff() ? 1.0f : 0.0f;
    return now;
}

void DisplayComponent::runScene (double seconds)
{
    // The picture is drawn at the card's own shape, at about four fifths of its width in pixels:
    // enough to look smooth when scaled up, small enough to stay cheap.
    const auto card = Toy::body (getLocalBounds());
    if (card.getWidth() > 1.0f && card.getHeight() > 1.0f)
    {
        const int w = juce::jlimit (480, 800, juce::roundToInt (card.getWidth() * 0.8f));
        const int h = juce::jmax (110, juce::roundToInt ((float) w * card.getHeight() / card.getWidth()));
        if (picture.getWidth() != w || picture.getHeight() != h)
            picture = juce::Image (juce::Image::RGB, w, h, true, juce::SoftwareImageType());
    }
    scene.advance (gatherInput(), seconds);
    scene.render (picture);
}

bool DisplayComponent::settleHeldFrame (double seconds)
{
    // The host has stopped calling the plug-in: let the last frame's cuts and levels fall away, so
    // the pictures come to rest instead of holding what was there when it stopped.
    const float keep = (float) std::exp (-seconds / 0.12);
    const float fall = (float) (90.0 * seconds);   // dB
    bool moving = false;
    auto decay = [&] (float& v)
    {
        v = v * keep < 0.05f ? 0.0f : v * keep;
        moving = moving || v > 0.0f;
    };
    auto drop = [&] (float& db)
    {
        db = juce::jmax (-120.0f, db - fall);
        moving = moving || db > -120.0f;
    };

    const int n = juce::jlimit (0, dehonk::kMaxBands, frame.numBands);
    for (int k = 0; k < n; ++k)
    {
        decay (frame.cutDb[k]);
        drop (frame.levelDb[k]);
    }
    for (auto& db : frame.moduleCutDb)
        decay (db);
    decay (frame.totalCutDb);
    decay (frame.guard);
    decay (frame.activity);
    drop (frame.programDb);
    drop (frame.keyDb);
    frame.keyed = false;           // no programme, no key: the sidechain's dragonflies go home
    frame.snapCutDb = 0.0f;
    return moving;
}

bool DisplayComponent::frameMatters (const dehonk::DisplayFrame& f)
{
    // The host sends frames whenever it runs, even in silence: only one that would change the
    // picture keeps the full frame rate. Levels count from where the reeds start to show (about
    // -65 dB), so a hiss floor flickering far below that does not keep the pond awake.
    auto moved = [] (float a, float b) { return (a > -68.0f || b > -68.0f) && std::abs (a - b) > 0.5f; };
    bool matters = f.activity > 0.0f || f.totalCutDb > 0.05f || f.guard > 0.01f || f.snapCutDb > 0.0f
                   || f.keyed != shownFrame.keyed || f.numBands != shownFrame.numBands
                   || moved (f.programDb, shownFrame.programDb) || moved (f.keyDb, shownFrame.keyDb);
    const int n = juce::jlimit (0, dehonk::kMaxBands, f.numBands);
    for (int k = 0; k < n && ! matters; ++k)
        matters = moved (f.levelDb[k], shownFrame.levelDb[k]) || f.cutDb[k] > 0.05f;
    if (matters)
        shownFrame = f;
    return matters;
}

void DisplayComponent::timerCallback()
{
    const bool newFrame = processor.popLatestDisplayFrame (frame);
    const bool freshNews = newFrame && frameMatters (frame);
    hasFrame = hasFrame || newFrame;
    if (newFrame && onFrame != nullptr)
        onFrame (frame);

    const auto now = watchedValues();
    const bool changed = now != lastValues;
    lastValues = now;

    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    const double seconds = lastTickMs > 0.0 ? (nowMs - lastTickMs) * 0.001 : 1.0 / 30.0;
    lastTickMs = nowMs;

    sinceFrame = newFrame ? 0.0 : sinceFrame + seconds;
    const bool settling = hasFrame && sinceFrame > settleAfter && settleHeldFrame (seconds);
    // Whatever follows the frame outside this component (the module meters, the guard's lamp)
    // must see it settle too, or it would stay as it was when the host stopped calling: once
    // more after the last change, so that it sees the zeros.
    if (! newFrame && (settling || wasSettling) && onFrame != nullptr)
        onFrame (frame);
    wasSettling = settling;

    runScene (seconds);
    repaint();

    if (onTick != nullptr)
    {
        dehonk::scene::BandViewState s;
        const auto in = gatherInput();
        s.frame = frame;
        s.hasFrame = hasFrame;
        for (int m = 0; m < dehonk::kNumModules; ++m)
            s.moduleOn[m] = in.moduleOn[m];
        s.snapOn = in.snapOn;
        for (int e = 0; e < dehonk::kNumEdges; ++e)
            s.edgeHz[e] = in.edgeHz[e];
        s.thresholdDb = in.thresholdDb;
        s.maxCutDb = value (*maxCut);
        s.soloHonk = in.soloHonk;
        s.diff = in.diff;
        s.bypass = in.bypass;
        onTick (s, seconds);
    }

    // A click is an event: the pictures have been told of it once and the held frame forgets it.
    frame.snapCutDb = 0.0f;

    // Full rate while anything moves or something that shows arrives; a few frames a second while
    // the pond is still, even with the host sending silent frames.
    const int wanted = (scene.isAnimating() || freshNews || changed || settling) ? 30 : 5;
    if (wanted != rateHz)
    {
        rateHz = wanted;
        startTimerHz (rateHz);
    }
}

//==============================================================================
void DisplayComponent::paint (juce::Graphics& g)
{
    using namespace dehonk;
    const auto card = Toy::body (getLocalBounds());

    g.setColour (toy::ink.withAlpha (0.28f));
    g.fillRoundedRectangle (card.translated (0.0f, toy::dropY), toy::cardCorner);

    {
        juce::Path rounded;
        rounded.addRoundedRectangle (card, toy::cardCorner);
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (rounded);
        g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
        g.setOpacity (1.0f);     // an image is drawn at the context's opacity
        g.drawImage (picture, card, juce::RectanglePlacement::stretchToFit);
    }

    g.setColour (toy::ink);
    g.drawRoundedRectangle (card, toy::cardCorner, toy::outline);
}
