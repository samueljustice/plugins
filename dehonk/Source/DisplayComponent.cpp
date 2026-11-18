#include "DisplayComponent.h"

#include "Parameters.h"
#include "PluginProcessor.h"
#include "Theme.h"

namespace
{
    using dehonk::scene::PondScene;
    constexpr double settleAfter = 0.25;         // seconds without a new frame before the held one falls away
    constexpr int maxPicturePixels = 1600000;    // the scene is never drawn with more pixels than this (a bigger card is drawn at half size and doubled)
    constexpr double slowSceneMs = 7.0;          // a scene that takes longer than this on average is drawn more coarsely
    constexpr double fullGapMs = 6.5;            // from the fallback timer: never two pictures closer than this
    constexpr double everyOtherBelowMs = 6.7;    // a screen whose refresh is quicker than this (about 150 Hz) gets a picture at every other refresh

    /** A value handed from one thread to another, the newest winning: the writer fills its own
        slot and swaps it in; the reader swaps the newest out when there is one. Neither waits.
        With four slots the reader also keeps the one it had before (see held), so that a picture is
        not drawn over while the window may still be reading it. */
    template <typename T, int numSlots>
    class Handover
    {
        static_assert (numSlots == 3 || numSlots == 4);
    public:
        T& writing() noexcept                { return slots[(size_t) w]; }
        T& reading() noexcept                { return slots[(size_t) r]; }
        const T& reading() const noexcept    { return slots[(size_t) r]; }
        /** Writer: hands over what it has written. */
        void publish() noexcept               { w = state.exchange (w | fresh, std::memory_order_acq_rel) & 3; }
        /** Reader: takes the newest, if anything new has been handed over. */
        bool take() noexcept
        {
            if ((state.load (std::memory_order_acquire) & fresh) == 0)
                return false;
            const int give = numSlots == 4 ? held : r;
            if (numSlots == 4)
                held = r;
            r = state.exchange (give, std::memory_order_acq_rel) & 3;
            return true;
        }
        /** For use before the other thread starts (or after it has stopped). */
        T& slot (int i) noexcept              { return slots[(size_t) i]; }

    private:
        static constexpr int fresh = 4;
        std::array<T, (size_t) numSlots> slots {};
        int w = 0, r = 1, held = numSlots == 4 ? 2 : 1;
        std::atomic<int> state { numSlots - 1 };
    };
}

/** The picture itself: an opaque rectangle, so that repainting it never repaints what is behind. */
class DisplayComponent::Canvas : public juce::Component
{
public:
    explicit Canvas (DisplayComponent& d) : owner (d)
    {
        setOpaque (true);
        setInterceptsMouseClicks (false, false);
    }
    void paint (juce::Graphics& g) override { owner.paintPicture (g); }

private:
    DisplayComponent& owner;
};

/** The thread that moves the scene on and draws it. It owns the scene; the message thread sends
    it requests and takes its pictures through two Handovers, and wakes it with an atomic counter. */
class DisplayComponent::Renderer : private juce::Thread
{
public:
    using Fit = PondScene::Fit;

    struct Request
    {
        dehonk::scene::SceneInput in;
        double atMs = 0.0;          // the moment the picture is for
        int width = 0, height = 0;  // the image, in pixels
        Fit fit = Fit::same;
        float perPoint = 1.0f;
        float scale = 1.0f;         // the display's, the image is made for
        int clicks = 0;             // clicks all told, and where the last one was
        float clickX = 0.0f, clickY = 0.0f;
        std::uint32_t number = 0;
    };

    struct Picture
    {
        juce::Image image;
        float scale = 1.0f;          // the display scale it was made for
        bool coarse = false;         // drawn at half size and doubled (it is still the full size)
        PondScene::Hits hits;
        std::array<PondScene::TreeTop, dehonk::kNumEdges> tops {};
        bool moving = true, raining = false;
        double sceneMs = 0.0;
        std::uint32_t number = 0;    // the request it answers
    };

    Renderer() : juce::Thread ("De-Honk pond") {}
    ~Renderer() override { stop(); }

    void start()
    {
        if (! isThreadRunning())
            startThread (juce::Thread::Priority::high);
    }
    void stop()
    {
        signalThreadShouldExit();
        wake();
        stopThread (2000);
    }

    /** Message thread: fill this in, then call send(). */
    Request& request() noexcept     { return requests.writing(); }
    void send() noexcept
    {
        requests.publish();
        wake();
    }
    /** Message thread: the newest picture, after take() said there is one. */
    bool take() noexcept                    { return pictures.take(); }
    const Picture& shown() const noexcept   { return pictures.reading(); }
    /** The number of the last request the drawing has started on. */
    std::uint32_t started() const noexcept  { return startedOn.load (std::memory_order_acquire); }

    /** Draws a first picture straight away, before the thread runs. */
    void drawFirst (const Request& r)
    {
        jassert (! isThreadRunning());
        draw (r, pictures.slot (1));
    }

private:
    void wake() noexcept
    {
        wakes.fetch_add (1, std::memory_order_release);
        wakes.notify_one();
    }

    void run() override
    {
        std::uint32_t seen = wakes.load (std::memory_order_acquire);
        while (! threadShouldExit())
        {
            wakes.wait (seen, std::memory_order_acquire);
            seen = wakes.load (std::memory_order_acquire);
            if (threadShouldExit())
                break;
            if (! requests.take())
                continue;
            const Request& r = requests.reading();
            startedOn.store (r.number, std::memory_order_release);
            draw (r, pictures.writing());
            pictures.publish();
        }
    }

    void draw (const Request& r, Picture& out)
    {
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        // The scene moves on to the moment the picture is for, by the time since the last one.
        const double seconds = lastAtMs > 0.0 ? juce::jlimit (0.0, 0.25, (r.atMs - lastAtMs) * 0.001) : 0.0;
        lastAtMs = r.atMs;
        if (r.clicks != clicksSeen)
        {
            clicksSeen = r.clicks;
            scene.click (r.clickX, r.clickY);
        }
        scene.advance (r.in, seconds);
        if (out.image.getWidth() != r.width || out.image.getHeight() != r.height)
            out.image = juce::Image (juce::Image::ARGB, r.width, r.height, false);
        scene.render (out.image, r.fit, r.perPoint);
        out.scale = r.scale;
        out.coarse = r.fit == Fit::doubled;
        out.hits = scene.hits();
        out.tops = scene.treeTops();
        out.moving = scene.isAnimating();
        out.raining = scene.isRaining();
        out.number = r.number;
        out.sceneMs = juce::Time::getMillisecondCounterHiRes() - t0;
    }

    PondScene scene;
    Handover<Request, 3> requests;
    Handover<Picture, 4> pictures;
    std::atomic<std::uint32_t> wakes { 0 }, startedOn { 0 };
    double lastAtMs = 0.0;
    int clicksSeen = 0;
};

DisplayComponent::DisplayComponent (DeHonkAudioProcessor& p)
    : processor (p)
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

    canvas = std::make_unique<Canvas> (*this);
    addAndMakeVisible (*canvas);
    if (auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
        deviceScale = (float) display->scale;

    // A first picture, drawn here before the drawing thread starts, so the window never shows an
    // empty panel.
    renderer = std::make_unique<Renderer>();
    {
        Renderer::Request first;
        first.in = gatherInput();
        first.width = PondScene::kWidth;
        first.height = PondScene::kHeight;
        renderer->drawFirst (first);
    }
    renderer->start();
    lastValues = watchedValues();
}

DisplayComponent::~DisplayComponent()
{
    vblank.reset();
    stopTimer();
    renderer->stop();
}

void DisplayComponent::setActive (bool shouldBeActive)
{
    if (! shouldBeActive)
    {
        vblank.reset();
        stopTimer();
        rateHz = 0;
        lastPictureMs = 0.0;
    }
    else if (vblank == nullptr)
    {
        rateHz = fullRateHz;
        lastVBlankMs = juce::Time::getMillisecondCounterHiRes();
        vblank = std::make_unique<juce::VBlankAttachment> (this, [this]
        {
            const double now = juce::Time::getMillisecondCounterHiRes();
            // How far apart the refreshes are (60 Hz, 120 Hz, or whatever this screen does).
            const double gap = now - lastVBlankMs;
            if (gap > 3.0 && gap < 40.0)
                refreshMs += (gap - refreshMs) * 0.05;
            lastVBlankMs = now;
            tick (now, true);
            const double ms = juce::Time::getMillisecondCounterHiRes() - now;
            stats.clockMs += ms;
            stats.clockWorstMs = juce::jmax (stats.clockWorstMs, ms);
            ++stats.clockTicks;
        });
        // Where the screen's refresh does not reach this window, a plain timer keeps the pond going.
        startTimerHz (30);
    }
}

void DisplayComponent::timerCallback()
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (now - lastVBlankMs > 120.0)
        tick (now, false);
}

void DisplayComponent::setFollowerRateHz (int hz) { followerHz = hz <= 0 ? 0 : juce::jmax (15, hz); }

void DisplayComponent::setFocusModule (int module)
{
    module = module >= 0 && module <= dehonk::kNumModules ? module : -1;
    if (module == focusModule)
        return;
    focusModule = module;
    poked = true;              // the pond wakes to show it
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

juce::Rectangle<int> DisplayComponent::pictureBounds() const noexcept
{
    auto r = getLocalBounds().reduced (1);
    return r.withTrimmedTop (juce::jmin (treeRoom, juce::jmax (0, r.getHeight() - 90)));
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
    in.focusModule = focusModule;
    in.axisLeft = axisLeft;
    in.axisRight = axisRight;
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

void DisplayComponent::askForPicture (double seconds, double atMs)
{
    // The image is the card in device pixels, so that it goes to the window pixel for pixel, at
    // any scale (rounded up: a pixel over at the right or the bottom is cut off). How the scene
    // fills it depends on what the display needs and the machine can do (see quality):
    //   on a display with fewer than one and a half pixels to the point, drawn at twice the size
    //     and averaged down, so that every edge is smoothed;
    //   otherwise drawn pixel for pixel;
    //   and past a budget of pixels (a very large window), or if that proves too slow here, at half
    //     the size and doubled smoothly.
    using Fit = PondScene::Fit;
    juce::ignoreUnused (seconds);
    const auto card = pictureBounds();
    auto& r = renderer->request();
    r.width = PondScene::kWidth;
    r.height = PondScene::kHeight;
    r.fit = Fit::same;
    r.perPoint = 1.0f;
    r.scale = deviceScale;
    if (card.getWidth() >= 160 && card.getHeight() >= 90)
    {
        const float scale = juce::jlimit (0.5f, 4.0f, deviceScale);
        int w = (int) std::ceil ((float) card.getWidth() * scale - 0.01f);
        int h = (int) std::ceil ((float) card.getHeight() * scale - 0.01f);
        const double pixels = (double) w * (double) h;
        Fit fit = Fit::same;
        if (quality >= 2 && scale < 1.5f && 4.0 * pixels <= maxPicturePixels)
            fit = Fit::halved;
        else if ((quality <= 0 || pixels > maxPicturePixels) && w >= 320 && h >= 180)
            fit = Fit::doubled;
        if (fit == Fit::doubled)
        {
            w = (w + 1) & ~1;
            h = (h + 1) & ~1;
        }
        else if (fit == Fit::same && (w < 160 || h < 90))
        {
            w = juce::jmax (160, w);
            h = juce::jmax (90, h);
        }
        r.width = w;
        r.height = h;
        r.fit = fit;
        r.perPoint = scale * (fit == Fit::halved ? 2.0f : fit == Fit::doubled ? 0.5f : 1.0f);
    }

    r.in = gatherInput();
    // A click of Snap is an event: it goes with every request until the scene has taken it.
    snapSent = juce::jmax (snapSent, snapWaiting);
    snapWaiting = 0.0f;
    r.in.frame.snapCutDb = snapSent;
    r.atMs = atMs;
    r.clicks = clicks;
    r.clickX = clickX;
    r.clickY = clickY;
    r.number = ++requests;
    if (snapSent > 0.0f)
        snapSentWith = r.number;
    renderer->send();
}

bool DisplayComponent::takePicture()
{
    if (! renderer->take())
        return false;
    const auto& pic = renderer->shown();
    sceneMoving = pic.moving;
    sceneRaining = pic.raining;
    watchCost (pic.sceneMs);
    stats.sceneMs += pic.sceneMs;
    stats.sceneWorstMs = juce::jmax (stats.sceneWorstMs, pic.sceneMs);
    ++stats.pictures;
    canvas->repaint();
    treesChanged();
    return true;
}

void DisplayComponent::watchCost (double sceneMs)
{
    // If drawing the scene this finely takes too long on this machine (the mean of two seconds'
    // pictures, so that one slow moment does not count), draw it one step more coarsely. After a
    // while it tries the finer step again, waiting twice as long each time that fails.
    costMs += sceneMs;
    if (++costCount < 120)
        return;
    const double mean = costMs / costCount;
    costCount = 0;
    costMs = 0.0;
    if (mean > slowSceneMs && quality > 0)
    {
        --quality;
        retryIn = retryAfter;
        retryAfter = juce::jmin (retryAfter * 2, 64);
    }
    else if (quality < 2 && mean < 0.3 * slowSceneMs && --retryIn <= 0)
        ++quality;
}

void DisplayComponent::setAxisSpan (float left01, float right01)
{
    left01 = juce::jlimit (0.0f, 0.4f, left01);
    right01 = juce::jlimit (0.6f, 1.0f, right01);
    if (std::abs (left01 - axisLeft) < 1.0e-5f && std::abs (right01 - axisRight) < 1.0e-5f)
        return;
    axisLeft = left01;
    axisRight = right01;
    poked = true;
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
    // picture keeps the full frame rate. Levels count from where the band view starts to show them
    // (about -65 dB), so a hiss floor flickering far below that does not keep the pond awake.
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

void DisplayComponent::tick (double nowMs, bool fromRefresh)
{
    const double sinceTick = lastTickMs > 0.0 ? nowMs - lastTickMs : refreshMs;
    lastTickMs = nowMs;

    // Every refresh: put up the picture drawn since the last one, if there is one.
    takePicture();
    if (snapSentWith != 0 && renderer->started() - snapSentWith < 0x80000000u)
    {
        snapSent = 0.0f;           // the scene has it
        snapSentWith = 0;
    }

    // Take what has arrived (cheap), and see whether anything has been changed.
    if (processor.popLatestDisplayFrame (frameReader, frame, true))
    {
        hasFrame = true;
        frameForFollowers = true;
        lastNewFrameMs = nowMs;
        snapWaiting = juce::jmax (snapWaiting, frame.snapCutDb);
        followerSnap = juce::jmax (followerSnap, frame.snapCutDb);
        freshNews = frameMatters (frame) || freshNews;
    }
    {
        const auto now = watchedValues();
        changed = changed || now != lastValues;
        lastValues = now;
    }

    // Is a picture due? At the full rate, one for each refresh of the screen (one for every other
    // refresh only on a screen faster than about 150 Hz); while the pond is still, five a second,
    // and at once when something happens. With nothing moving but rain falling, twenty-four a
    // second. At the full rate the refreshes are counted, not timed, so that a refresh that is
    // called a little early after a late one still gets its picture; the time between them only
    // has to be more than a third of a refresh (two calls for one refresh get one picture).
    if (fromRefresh)
        ++refreshesSincePicture;
    const bool still = rateHz < fullRateHz;
    const double since = nowMs - lastPictureMs;
    if (lastPictureMs > 0.0)
    {
        if (still && ! (freshNews || changed || poked))
        {
            if (since < 1000.0 / juce::jmax (1, rateHz) - 5.0)
                return;
        }
        else if (fromRefresh)
        {
            if (refreshesSincePicture < (refreshMs < everyOtherBelowMs ? 2 : 1) || since < refreshMs * 0.34)
                return;
        }
        else if (since < fullGapMs)
            return;
    }
    refreshesSincePicture = 0;
    const double seconds = lastPictureMs > 0.0 ? juce::jmin (0.25, since * 0.001) : 1.0 / fullRateHz;
    lastPictureMs = nowMs;
    juce::ignoreUnused (sinceTick);

    const bool settling = hasFrame && (nowMs - lastNewFrameMs) * 0.001 > settleAfter && settleHeldFrame (seconds);

    if (frameStale && isShowing())
        makeFrame();

    // The picture asked for now is put up at the next refresh: it shows the scene as it will be then.
    askForPicture (seconds, nowMs + juce::jlimit (4.0, 34.0, refreshMs));
    stats.seconds += seconds;

    tellFollowers (seconds, settling);
    frame.snapCutDb = 0.0f;

    // Full rate while anything moves or something that shows arrives; a few pictures a second
    // while the pond is still, even with the host sending silent frames.
    rateHz = (sceneMoving || freshNews || changed || poked || settling || wasSettling) ? fullRateHz
             : sceneRaining ? rainRateHz : stillRateHz;
    freshNews = changed = poked = false;
}

void DisplayComponent::tellFollowers (double seconds, bool settling)
{
    // Whatever follows the frames outside this component (the band view, the module meters, the
    // guard's lamp) is told with each picture, or at its own rate if one was set. It must see the
    // held frame settle too, or it would stay as it was when the host stopped calling: once more
    // after the last change, so that it sees the zeros.
    followerSeconds += seconds;
    if (followerHz > 0 && followerSeconds < 1.0 / followerHz - 0.004)
        return;

    auto shown = frame;
    shown.snapCutDb = followerSnap;      // a click is an event: they are told of it once
    followerSnap = 0.0f;
    if ((frameForFollowers || settling || wasSettling) && onFrame != nullptr)
        onFrame (shown);
    frameForFollowers = false;
    wasSettling = settling;

    if (onTick != nullptr)
    {
        dehonk::scene::BandViewState s;
        const auto in = gatherInput();
        s.frame = shown;
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
        onTick (s, followerSeconds);
    }
    followerSeconds = 0.0;
}

//==============================================================================
void DisplayComponent::resized()
{
    quality = 2;
    costCount = 0;
    costMs = 0.0;
    retryAfter = 8;
    canvas->setBounds (pictureBounds());
    frameStale = true;
    poked = true;
}

void DisplayComponent::parentHierarchyChanged() { frameStale = true; }

void DisplayComponent::treesChanged()
{
    const auto& now = renderer->shown().tops;
    bool differs = false;
    for (size_t i = 0; i < now.size(); ++i)
        differs = differs || now[i].colour != tops[i].colour || std::abs (now[i].x - tops[i].x) > 1.0e-4f
                          || std::abs (now[i].y - tops[i].y) > 1.0e-4f || std::abs (now[i].radius - tops[i].radius) > 1.0e-4f;
    if (! differs)
        return;
    tops = now;
    repaint (getLocalBounds().withBottom (pictureBounds().getY() + 1));      // only the strip the treetops rise into
}

void DisplayComponent::makeFrame()
{
    // The card's four corners, made once for this size: in each, whatever lies behind this
    // component outside the rounded corner (so that the picture can be an opaque rectangle), and
    // the outline's arc. What lies behind is asked of the parent, with this component left out.
    frameStale = false;
    const auto card = pictureBounds();
    const int r = (int) std::ceil (dehonk::toy::cardCorner) + 1;
    if (card.getWidth() < 2 * r || card.getHeight() < 2 * r)
        return;
    const float scale = juce::jlimit (1.0f, 4.0f, deviceScale);
    const auto shape = card.toFloat().reduced (dehonk::toy::outline * 0.5f);
    const juce::Rectangle<int> squares[4] = { { card.getX(), card.getY(), r, r }, { card.getRight() - r, card.getY(), r, r },
                                              { card.getX(), card.getBottom() - r, r, r }, { card.getRight() - r, card.getBottom() - r, r, r } };

    auto* parent = getParentComponent();
    capturing = true;
    canvas->setOpaque (false);
    for (int i = 0; i < 4; ++i)
    {
        const auto sq = squares[i];
        juce::Image behind;
        if (parent != nullptr)
            behind = parent->createComponentSnapshot (sq + getPosition(), true, scale);

        juce::Image corner (juce::Image::ARGB, juce::roundToInt ((float) r * scale), juce::roundToInt ((float) r * scale), true);
        {
            juce::Graphics g (corner);
            g.addTransform (juce::AffineTransform::scale (scale).translated ((float) -sq.getX() * scale, (float) -sq.getY() * scale));
            {
                juce::Graphics::ScopedSaveState state (g);
                juce::Path outside;
                outside.addRectangle (sq.toFloat().expanded (1.0f));
                outside.addRoundedRectangle (shape, dehonk::toy::cardCorner);
                outside.setUsingNonZeroWinding (false);
                g.reduceClipRegion (outside);
                if (behind.isValid())
                    g.drawImage (behind, sq.toFloat());
                else
                    g.fillAll (dehonk::toy::cream);
            }
            g.setColour (dehonk::toy::ink);
            g.drawRoundedRectangle (shape, dehonk::toy::cardCorner, dehonk::toy::outline);
        }
        corners[(size_t) i] = corner;
    }
    canvas->setOpaque (true);
    capturing = false;
}

void DisplayComponent::paint (juce::Graphics& g)
{
    if (capturing)
        return;
    // The treetops that rise above the card, each a flat shape in the colour of its top.
    const auto card = pictureBounds();
    g.reduceClipRegion (getLocalBounds().withBottom (card.getY()));
    const float w = (float) card.getWidth();
    for (const auto& t : tops)
    {
        if (t.radius <= 0.0f)
            continue;
        g.setColour (t.colour);
        g.fillEllipse (juce::Rectangle<float> (2.0f * t.radius * w, 2.0f * t.radius * w)
                           .withCentre ({ (float) card.getX() + t.x * w, (float) card.getY() + t.y * w }));
    }
}

void DisplayComponent::paintPicture (juce::Graphics& g)
{
    if (capturing)
        return;
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    const auto area = canvas->getLocalBounds();
    const float scale = g.getInternalContext().getPhysicalPixelScaleFactor();
    if (std::abs (scale - deviceScale) > 0.01f)
    {
        quality = 2;
        costCount = 0;
        deviceScale = scale;       // the next picture is made for it
        frameStale = true;
        poked = true;
    }

    // Pixel for pixel when the image was made for this window: drawn at the display's own scale,
    // so that each of its pixels is one of the screen's (it may be a pixel bigger than the card,
    // rounded up: that is cut off). Otherwise (for a refresh after the window or the display
    // changes, and at every refresh while the window is being resized) the picture made for the
    // old size is drawn as it is, at its own scale, over paper: stretching it would have
    // CoreGraphics resample a picture of millions of pixels on the message thread each time.
    const auto& pic = renderer->shown();
    const auto& image = pic.image;
    const float wantW = (float) area.getWidth() * scale, wantH = (float) area.getHeight() * scale;
    const bool exact = std::abs (pic.scale - scale) < 0.01f
                       && (float) image.getWidth() >= wantW - 0.02f && (float) image.getWidth() < wantW + (pic.coarse ? 2.0f : 1.0f)
                       && (float) image.getHeight() >= wantH - 0.02f && (float) image.getHeight() < wantH + (pic.coarse ? 2.0f : 1.0f);
    g.setOpacity (1.0f);     // an image is drawn at the context's opacity
    if (exact)
    {
        g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
        g.drawImageTransformed (image, juce::AffineTransform::scale (1.0f / scale));
    }
    else if (image.isValid())
    {
        g.fillAll (dehonk::toy::cream);
        g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
        g.drawImageTransformed (image, juce::AffineTransform::scale (1.0f / juce::jmax (0.25f, pic.scale)));
    }
    else
        g.fillAll (dehonk::toy::cream);

    // The frame: the corners made in makeFrame, and the straight runs of the outline between them.
    const float line = dehonk::toy::outline;
    const int r = corners[0].isValid() ? (int) std::ceil (dehonk::toy::cardCorner) + 1 : 0;
    g.setColour (dehonk::toy::ink);
    if (r > 0)
    {
        const juce::Point<int> at[4] = { { 0, 0 }, { area.getRight() - r, 0 }, { 0, area.getBottom() - r }, { area.getRight() - r, area.getBottom() - r } };
        for (int i = 0; i < 4; ++i)
            g.drawImage (corners[(size_t) i], juce::Rectangle<int> (at[i].x, at[i].y, r, r).toFloat());
        const float x0 = (float) r, x1 = (float) (area.getRight() - r), y0 = (float) r, y1 = (float) (area.getBottom() - r);
        g.fillRect (juce::Rectangle<float> (x0, 0.0f, x1 - x0, line));
        g.fillRect (juce::Rectangle<float> (x0, (float) area.getBottom() - line, x1 - x0, line));
        g.fillRect (juce::Rectangle<float> (0.0f, y0, line, y1 - y0));
        g.fillRect (juce::Rectangle<float> ((float) area.getRight() - line, y0, line, y1 - y0));
    }
    else
        g.drawRoundedRectangle (area.toFloat().reduced (line * 0.5f), dehonk::toy::cardCorner, line);

    // Where a treetop crosses the top edge it stands in front of the outline.
    {
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (juce::Rectangle<int> (r, 0, area.getWidth() - 2 * r, (int) std::ceil (line)));
        const float w = (float) area.getWidth();
        for (const auto& t : tops)
            if (t.radius > 0.0f && t.y - t.radius < 0.0f)
            {
                g.setColour (t.colour);
                g.fillEllipse (juce::Rectangle<float> (2.0f * t.radius * w, 2.0f * t.radius * w).withCentre ({ t.x * w, t.y * w }));
            }
    }

    const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
    stats.paintMs += ms;
    stats.paintWorstMs = juce::jmax (stats.paintWorstMs, ms);
    ++stats.paints;
}

juce::Point<int> DisplayComponent::pictureSize() const noexcept
{
    const auto& image = renderer->shown().image;
    return { image.getWidth(), image.getHeight() };
}

DisplayComponent::Timing DisplayComponent::timing (bool reset)
{
    Timing t = stats;
    if (t.pictures > 0) t.sceneMs /= t.pictures;
    if (t.paints > 0)   t.paintMs /= t.paints;
    if (t.clockTicks > 0) t.clockMs /= t.clockTicks;
    if (reset)
        stats = {};
    return t;
}

//==============================================================================
bool DisplayComponent::hitTest (int x, int y) { return pictureBounds().contains (x, y); }

void DisplayComponent::mouseMove (const juce::MouseEvent& e)
{
    // Asked of the picture on the screen, not the scene (which is being drawn on another thread).
    const auto card = pictureBounds().toFloat();
    const bool someone = renderer->shown().hits.somethingAt ((e.position.x - card.getX()) / card.getWidth(), (e.position.y - card.getY()) / card.getHeight());
    setMouseCursor (someone ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
}

void DisplayComponent::mouseExit (const juce::MouseEvent&) { setMouseCursor (juce::MouseCursor::NormalCursor); }

void DisplayComponent::mouseDown (const juce::MouseEvent& e)
{
    // The scene takes the click with the next picture it draws.
    const auto card = pictureBounds().toFloat();
    const float x01 = (e.position.x - card.getX()) / card.getWidth(), y01 = (e.position.y - card.getY()) / card.getHeight();
    if (renderer->shown().hits.somethingAt (x01, y01))
    {
        ++clicks;
        clickX = x01;
        clickY = y01;
        poked = true;          // the pond wakes at once to show it
    }
}
