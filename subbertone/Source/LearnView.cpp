#include "LearnView.h"

#include "SubbertoneLookAndFeel.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace
{
    namespace learn = subbertone::learn;
    using Activity = learn::Status::Activity;
    using Timeline = learn::LearnAudio::TimelineState;

    // Design-size layout (the editor scales everything uniformly)
    constexpr int c_headerHeight = 32;        // as the editor's section headers
    constexpr int c_statusHeight = 28;
    constexpr int c_controlInset = 5;         // controls are 22 px tall, as LO CUT and LIMIT
    constexpr float c_regionMinFontHeight = 9.5f;  // the region selector's text shrinks to fit, down to this
    constexpr float c_axisWidth = 96.0f;      // octave labels, left of the plot ("C6  1.05 kHz" fits)
    constexpr float c_tagColumn = 104.0f;     // line tags, right of the plot
    constexpr float c_plotTop = 12.0f;
    constexpr float c_timeAxisHeight = 20.0f;
    constexpr float c_tagHeight = 16.0f;
    constexpr float c_tagGap = 3.0f;
    constexpr float c_hitTolerance = 6.0f;    // screen px either side of a line that still counts as on it

    // The frequency axis
    constexpr double c_lowestShownHz = 20.0, c_highestShownHz = 2000.0;
    constexpr double c_minSpanOctaves = 0.75;
    constexpr double c_fitMarginOctaves = 0.3;
    constexpr double c_minFitOctaves = 2.0;
    constexpr double c_c0Hz = 16.351597831287414;   // the octave lines are on C

    // Where the curve is drawn fainter: by the analysis' confidence, with gap fill dotted.
    enum Category { strong, medium, weak, bridged, numCategories };
    constexpr std::array<float, numCategories> c_categoryAlpha { 1.0f, 0.65f, 0.4f, 0.9f };
    constexpr float c_strongConfidence = 0.35f, c_mediumConfidence = 0.15f;

    const juce::String c_separator = "  /  ";

    juce::String percentText(float share)
    {
        return juce::String(juce::roundToInt(juce::jlimit(0.0f, 1.0f, share) * 100.0f)) + "%";
    }

    juce::String millisecondsText(double ms)
    {
        return ms >= 1000.0 ? juce::String(ms / 1000.0, 2) + " s" : juce::String(juce::roundToInt(ms)) + " ms";
    }

    juce::String hertzText(double hz)
    {
        return hz >= 1000.0 ? juce::String(hz / 1000.0, 2) + " kHz" : juce::String(juce::roundToInt(hz)) + " Hz";
    }

    juce::String rateText(double rate)
    {
        const double k = rate / 1000.0;
        return (std::abs(k - std::round(k)) < 1.0e-6 ? juce::String(juce::roundToInt(k)) : juce::String(k, 1)) + " kHz";
    }

    // Width of a line of text as drawn (a trailing space, taken off again, keeps a final ligature from measuring short).
    float lineWidth(const juce::Font& font, const juce::String& text)
    {
        return juce::GlyphArrangement::getStringWidth(font, text + " ") - juce::GlyphArrangement::getStringWidth(font, " ");
    }

    bool sameRatio(float a, float b)
    {
        return a > 0.0f && b > 0.0f && std::abs(std::log(a / b)) < 1.0e-3f;
    }

    int categoryOf(const learn::Frame& a, const learn::Frame& b)
    {
        if (((a.flags | b.flags) & learn::Frame::bridged) != 0)
            return bridged;

        const float confidence = std::min(a.confidence, b.confidence);
        return confidence >= c_strongConfidence ? strong : confidence >= c_mediumConfidence ? medium : weak;
    }

    // The time grid: the shortest step that keeps labels this far apart.
    double timeStep(double span, float width)
    {
        static constexpr double steps[] { 0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0, 600.0 };

        for (const double step : steps)
            if (step / std::max(1.0e-9, span) * static_cast<double>(width) >= 84.0)
                return step;

        return steps[std::size(steps) - 1];
    }

    // Adds a polyline to a path, leaving out points within half a pixel of the last one kept (a minute of frames is far
    // more than the lane is wide), but never the corner of a step.
    class Polyline
    {
    public:
        explicit Polyline(juce::Path& path) : m_path(path) {}

        void start(juce::Point<float> p)
        {
            end();
            m_path.startNewSubPath(p);
            m_last = p;
            m_open = true;
        }

        void to(juce::Point<float> p)
        {
            if (std::abs(p.x - m_last.x) < 0.5f && std::abs(p.y - m_last.y) < 0.5f)
            {
                m_pending = p;
                m_hasPending = true;
                return;
            }

            if (m_hasPending)
                m_path.lineTo(m_pending);

            m_path.lineTo(p);
            m_last = p;
            m_hasPending = false;
        }

        void end()
        {
            if (m_open && m_hasPending)
                m_path.lineTo(m_pending);

            m_hasPending = false;
            m_open = false;
        }

    private:
        juce::Path& m_path;
        juce::Point<float> m_last, m_pending;
        bool m_hasPending = false;
        bool m_open = false;
    };
}

//==============================================================================
/** The pitch lane's lines, grid and tags. Buffered: it is drawn again only when what it shows changes, while the live
    layer above it moves with playback. */
class LearnView::PitchLane final : public juce::Component, public juce::TooltipClient
{
public:
    PitchLane(LearnView& owner, const Theme::Fonts& fonts)
        : m_owner(owner)
        , m_fonts(fonts)
    {
        setOpaque(false);
        setBufferedToImage(true);
    }

    /** A region to show (null: none). refit: fit the frequency axis to it; else the zoom stays, unless it was never
        changed. */
    void setRegion(std::shared_ptr<const learn::Snapshot> snapshot, const learn::Region* region, bool refit)
    {
        m_snapshot = std::move(snapshot);
        m_region = region;
        m_hover = -1;
        m_lines.clear();
        m_log2.clear();

        if (m_region != nullptr)
        {
            m_start = m_region->followStartSeconds();
            m_end = std::max(m_start + 1.0e-3, m_region->followEndSeconds());

            m_log2.resize(m_region->frames.size(), 0.0f);
            const float ln2 = std::log(2.0f);

            for (std::size_t f = 0; f < m_log2.size(); ++f)
                m_log2[f] = m_region->frames[f].logLineHz / ln2;

            // The lines one can follow: every candidate, and the followed line if it is none of them. A Tonal region's
            // candidates are not scored: their tags show no strength then.
            int scored = 0;

            for (std::size_t k = 0; k < m_region->candidates.size(); ++k)
            {
                const auto& candidate = m_region->candidates[k];
                Line line;
                line.ratio = candidate.ratio;
                line.score = candidate.score;
                line.followed = sameRatio(candidate.ratio, m_region->lineRatio);
                line.presenceBit = line.followed ? -1 : static_cast<int>(k);
                scored += candidate.score > 0.0f ? 1 : 0;
                m_lines.push_back(line);
            }

            if (std::none_of(m_lines.begin(), m_lines.end(), [](const Line& line) { return line.followed; }))
            {
                Line line;
                line.ratio = m_region->lineRatio;
                line.followed = true;
                m_lines.push_back(line);
            }

            m_scored = scored >= 2;

            for (auto& line : m_lines)
            {
                const bool own = sameRatio(line.ratio, 1.0f);
                line.tag = own ? juce::String("AUTO") : LearnView::describeRatio(line.ratio);

                if (m_scored && line.score > 0.0f)
                    line.tag << " " << percentText(line.score);
            }

            if (refit || ! m_userZoom)
            {
                m_userZoom = false;
                fit();
            }
        }

        rebuild();
    }

    /** What LEARN is doing: with no region, the lane says so instead. */
    void setActivity(const learn::Status& status)
    {
        juce::String title, hint;
        float progress = -1.0f;

        switch (status.activity)
        {
            case Activity::armed:
                title = "WAITING FOR PLAYBACK";
                hint = status.timeline == Timeline::none ? "THIS HOST ISN'T REPORTING A PLAYING TIMELINE HERE"
                                                         : "START THE LOOP IN YOUR HOST";
                break;

            case Activity::capturing:
                title = status.progress >= 0.0f ? "LISTENING TO THE LOOP (" + percentText(status.progress) + ")"
                                                : "LISTENING (" + juce::String(status.heardSeconds, 1) + " s)";
                hint = "KEEP IT PLAYING";
                progress = status.progress;
                break;

            case Activity::analysing:
                title = "WORKING OUT THE PITCH (" + percentText(status.progress) + ")";
                progress = juce::jmax(0.0f, status.progress);
                break;

            case Activity::idle:
                title = "NOTHING LEARNED YET";

                if (status.outcome == learn::Status::Outcome::failed && status.failure == learn::Status::Failure::nothingPitched)
                    hint = "NOTHING IN WHAT WAS HEARD HAD A PITCH  /  CHECK THE THRESHOLD AND TRY AGAIN";
                else if (status.outcome == learn::Status::Outcome::failed && status.failure == learn::Status::Failure::loopTooShort)
                    hint = "THE LOOP IS SHORTER THAN A SECOND  /  LENGTHEN IT IN YOUR HOST, THEN CLICK LEARN";
                else
                    hint = "LOOP THE TRICKY PART IN YOUR HOST, THEN CLICK LEARN";
                break;
        }

        if (title != m_emptyTitle || hint != m_emptyHint || std::abs(progress - m_emptyProgress) > 1.0e-3f)
        {
            m_emptyTitle = title;
            m_emptyHint = hint;
            m_emptyProgress = progress;

            if (m_region == nullptr)
                repaint();
        }
    }

    const learn::Region* getRegion() const noexcept { return m_region; }
    double getStart() const noexcept { return m_start; }
    double getEnd() const noexcept { return m_end; }

    juce::Rectangle<float> getPlot() const
    {
        auto r = getLocalBounds().toFloat();
        r.removeFromLeft(c_axisWidth);
        r.removeFromRight(c_tagColumn);
        r.removeFromTop(c_plotTop);
        r.removeFromBottom(c_timeAxisHeight);
        return r;
    }

    float xFor(double seconds) const
    {
        const auto plot = getPlot();
        return plot.getX() + static_cast<float>((seconds - m_start) / (m_end - m_start)) * plot.getWidth();
    }

    float yFor(double log2Hz) const
    {
        const auto plot = getPlot();
        return plot.getBottom() - static_cast<float>((log2Hz - m_low) / (m_high - m_low)) * plot.getHeight();
    }

    //==============================================================================
    juce::String getTooltip() override
    {
        if (m_region == nullptr)
            return {};

        if (m_hover >= 0 && m_hover < static_cast<int>(m_lines.size()))
        {
            const auto& line = m_lines[static_cast<std::size_t>(m_hover)];
            const auto strength = m_scored && line.score > 0.0f ? " (" + percentText(line.score) + " as strong as the strongest)"
                                                                : juce::String();

            if (line.followed)
                return "The sub follows this line, an octave below it.";

            if (sameRatio(line.ratio, 1.0f))
                return "The line the analysis chose" + strength + ". Click to follow it.";

            return "The line at " + LearnView::describeRatio(line.ratio) + " of the one the analysis chose" + strength
                   + ". Click to follow it.";
        }

        return "Click a line, or its tag on the right, to follow it. The wheel zooms the frequency axis; double-click "
               "fits it again.";
    }

    void resized() override
    {
        rebuild();
    }

    void paint(juce::Graphics& g) override
    {
        using namespace Theme::Colours;

        const auto bounds = getLocalBounds().toFloat();
        g.setColour(scope);
        g.fillRoundedRectangle(bounds, 6.0f);
        g.setColour(outlineSoft);
        g.drawRoundedRectangle(bounds.reduced(0.5f), 6.0f, 1.0f);

        if (m_region == nullptr)
        {
            paintEmpty(g, bounds);
            return;
        }

        const auto plot = getPlot();
        paintGrid(g, plot);

        {
            juce::Graphics::ScopedSaveState state(g);
            g.reduceClipRegion(plot.expanded(2.0f, 1.0f).getSmallestIntegerContainer());

            // The other lines, faint, where they are present.
            for (std::size_t i = 0; i < m_lines.size(); ++i)
            {
                const auto& line = m_lines[i];

                if (line.followed || line.path.isEmpty())
                    continue;

                const bool hot = static_cast<int>(i) == m_hover;
                g.setColour(hot ? text.withAlpha(0.8f) : textDim.withAlpha(0.34f));
                g.strokePath(line.path, juce::PathStrokeType(hot ? 1.6f : 1.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }

            // The sub, then the line it follows on top.
            paintStrokes(g, m_sub, output, 1.5f);
            paintStrokes(g, m_followed, input, 2.0f);
        }

        paintTags(g, plot);
    }

    //==============================================================================
    void mouseMove(const juce::MouseEvent& e) override
    {
        setHover(lineAt(e.position));
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        setHover(-1);
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        // The line pressed, on the plot or by its tag. A click follows it when it is let go on that same line, as a
        // button clicks when let go on it: however long it was held, however much it moved along the line. (JUCE's
        // "dragged" is true of any press held 300 ms, so it would turn away a deliberate click.)
        const int line = e.mods.isPopupMenu() ? -1 : lineAt(e.position);
        m_pressedRatio = line >= 0 ? m_lines[static_cast<std::size_t>(line)].ratio : 0.0f;
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        const float pressed = std::exchange(m_pressedRatio, 0.0f);

        // (The second click of a double-click is answered by mouseDoubleClick(); a right-click asks for a menu.)
        if (e.getNumberOfClicks() != 1 || e.mods.isPopupMenu())
            return;

        const int line = lineAt(e.position);

        if (line < 0)
            return;

        const auto& released = m_lines[static_cast<std::size_t>(line)];

        if (sameRatio(released.ratio, pressed) && ! released.followed)
            m_owner.followLine(released.ratio);
    }

    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        if (m_region == nullptr || lineAt(e.position) >= 0)
            return;

        m_userZoom = false;
        fit();
        rebuild();
    }

    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        const float delta = wheel.deltaY * (wheel.isReversed ? -1.0f : 1.0f);

        if (m_region == nullptr || ! getPlot().contains(e.position) || juce::exactlyEqual(delta, 0.0f) || wheel.isInertial)
        {
            juce::Component::mouseWheelMove(e, wheel);
            return;
        }

        // Zoom around the frequency under the mouse.
        const auto plot = getPlot();
        const double share = juce::jlimit(0.0, 1.0, static_cast<double>((plot.getBottom() - e.position.y) / plot.getHeight()));
        const double anchor = m_low + share * (m_high - m_low);
        const double span = (m_high - m_low) * std::exp(-2.0 * static_cast<double>(delta));
        m_low = anchor - share * span;
        m_high = m_low + span;
        clampRange();
        m_userZoom = true;
        rebuild();
    }

private:
    struct Stroke
    {
        juce::Path path;
        juce::Path dots;   // a dotted stroke, made once
    };

    // A line one can follow: a candidate of the region, or the followed line if it is none of them.
    struct Line
    {
        float ratio = 1.0f;
        float score = 0.0f;
        int presenceBit = -1;    // -1: present wherever the curve has a pitch
        bool followed = false;
        juce::Path path;         // where present; the followed line is drawn from the strokes instead
        juce::String tag;
        juce::Rectangle<float> tagBounds;
        float anchorY = 0.0f;    // where the line ends at the plot's right edge
        bool hasTag = false;
    };

    double frameSeconds(std::size_t frame) const
    {
        return m_region->frameTime(frame) + m_region->alignmentSeconds;
    }

    bool presentAt(const Line& line, std::size_t frame) const
    {
        if (frame >= m_region->frames.size() || ! m_region->frames[frame].isVoiced())
            return false;

        return line.presenceBit < 0
            || (frame < m_region->presence.size() && ((m_region->presence[frame] >> line.presenceBit) & 1) != 0);
    }

    void fit()
    {
        double low = std::numeric_limits<double>::max();
        double high = std::numeric_limits<double>::lowest();

        for (std::size_t f = 0; m_region != nullptr && f < m_log2.size(); ++f)
        {
            for (const auto& line : m_lines)
            {
                if (! presentAt(line, f))
                    continue;

                const double value = static_cast<double>(m_log2[f]) + std::log2(static_cast<double>(line.ratio));
                high = std::max(high, value);
                low = std::min(low, line.followed ? value - 1.0 : value);   // the sub is an octave below
            }
        }

        if (low > high)
        {
            low = std::log2(40.0);
            high = std::log2(640.0);
        }

        low -= c_fitMarginOctaves;
        high += c_fitMarginOctaves;

        if (high - low < c_minFitOctaves)
        {
            const double middle = 0.5 * (low + high);
            low = middle - 0.5 * c_minFitOctaves;
            high = middle + 0.5 * c_minFitOctaves;
        }

        m_low = low;
        m_high = high;
        clampRange();
    }

    void clampRange()
    {
        const double lowest = std::log2(c_lowestShownHz);
        const double highest = std::log2(c_highestShownHz);
        const double span = juce::jlimit(c_minSpanOctaves, highest - lowest, m_high - m_low);

        m_low = juce::jlimit(lowest, highest - span, m_low);
        m_high = m_low + span;
    }

    void rebuild()
    {
        for (auto& stroke : m_followed)
            stroke = {};

        for (auto& stroke : m_sub)
            stroke = {};

        for (auto& line : m_lines)
        {
            line.path.clear();
            line.hasTag = false;
        }

        const auto plot = getPlot();

        if (m_region == nullptr || plot.getWidth() <= 1.0f || plot.getHeight() <= 1.0f)
        {
            repaint();
            return;
        }

        const auto& frames = m_region->frames;
        const double followedOffset = std::log2(static_cast<double>(m_region->lineRatio));
        auto point = [this](std::size_t f, double offset)
        {
            return juce::Point<float>(xFor(frameSeconds(f)), yFor(static_cast<double>(m_log2[f]) + offset));
        };

        // The followed line and its sub, split where the confidence or the gap fill changes how they are drawn.
        {
            std::array<Polyline, numCategories> line { Polyline(m_followed[0].path), Polyline(m_followed[1].path),
                                                       Polyline(m_followed[2].path), Polyline(m_followed[3].path) };
            std::array<Polyline, numCategories> sub { Polyline(m_sub[0].path), Polyline(m_sub[1].path),
                                                      Polyline(m_sub[2].path), Polyline(m_sub[3].path) };
            int previous = -1;

            for (std::size_t f = 0; f + 1 < frames.size(); ++f)
            {
                if (! frames[f].isVoiced() || ! frames[f + 1].isVoiced())
                {
                    if (previous >= 0)
                    {
                        line[static_cast<std::size_t>(previous)].end();
                        sub[static_cast<std::size_t>(previous)].end();
                    }

                    previous = -1;
                    continue;
                }

                const int category = categoryOf(frames[f], frames[f + 1]);
                const auto c = static_cast<std::size_t>(category);

                if (category != previous)
                {
                    if (previous >= 0)
                    {
                        line[static_cast<std::size_t>(previous)].end();
                        sub[static_cast<std::size_t>(previous)].end();
                    }

                    line[c].start(point(f, followedOffset));
                    sub[c].start(point(f, followedOffset - 1.0));
                }

                line[c].to(point(f + 1, followedOffset));
                sub[c].to(point(f + 1, followedOffset - 1.0));
                previous = category;
            }

            for (auto& polyline : line)
                polyline.end();

            for (auto& polyline : sub)
                polyline.end();
        }

        for (auto* strokes : { &m_followed, &m_sub })
        {
            auto& gapFill = (*strokes)[bridged];

            if (! gapFill.path.isEmpty())
            {
                const float thickness = strokes == &m_followed ? 2.0f : 1.5f;
                const float dashes[] { 0.01f, thickness * 2.4f };
                juce::PathStrokeType(thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded)
                    .createDashedStroke(gapFill.dots, gapFill.path, dashes, 2);
            }
        }

        // The other lines, where present, and every line's tag at the right edge, next to where it ends.
        for (auto& line : m_lines)
        {
            const double offset = std::log2(static_cast<double>(line.ratio));
            std::size_t last = frames.size();

            if (! line.followed)
            {
                Polyline polyline(line.path);
                bool open = false;

                for (std::size_t f = 0; f + 1 < frames.size(); ++f)
                {
                    if (! presentAt(line, f) || ! presentAt(line, f + 1))
                    {
                        polyline.end();
                        open = false;
                        continue;
                    }

                    if (! open)
                        polyline.start(point(f, offset));

                    polyline.to(point(f + 1, offset));
                    open = true;
                }

                polyline.end();
            }

            for (std::size_t f = frames.size(); f-- > 0;)
            {
                if (presentAt(line, f))
                {
                    last = f;
                    break;
                }
            }

            if (last == frames.size())
                continue;

            line.anchorY = juce::jlimit(plot.getY(), plot.getBottom(), yFor(static_cast<double>(m_log2[last]) + offset));
            const float width = lineWidth(m_fonts.body(10.5f), line.tag) + 16.0f;
            line.tagBounds = { plot.getRight() + 12.0f, line.anchorY - 0.5f * c_tagHeight, width, c_tagHeight };
            line.hasTag = true;
        }

        layoutTags(plot);
        repaint();
    }

    // Tags stack where their lines end, pushed apart so none covers another, all inside the plot's height.
    void layoutTags(juce::Rectangle<float> plot)
    {
        std::vector<Line*> tags;

        for (auto& line : m_lines)
            if (line.hasTag)
                tags.push_back(&line);

        std::stable_sort(tags.begin(), tags.end(), [](const Line* a, const Line* b) { return a->tagBounds.getY() < b->tagBounds.getY(); });

        float next = plot.getY();

        for (auto* line : tags)
        {
            line->tagBounds.setY(std::max(line->tagBounds.getY(), next));
            next = line->tagBounds.getBottom() + c_tagGap;
        }

        float limit = plot.getBottom();

        for (auto it = tags.rbegin(); it != tags.rend(); ++it)
        {
            auto& bounds = (*it)->tagBounds;
            bounds.setY(std::min(bounds.getY(), limit - c_tagHeight));
            limit = bounds.getY() - c_tagGap;
        }
    }

    void paintStrokes(juce::Graphics& g, const std::array<Stroke, numCategories>& strokes, juce::Colour colour, float thickness) const
    {
        for (std::size_t c = 0; c < strokes.size(); ++c)
        {
            const auto& stroke = strokes[c];
            g.setColour(colour.withAlpha(c_categoryAlpha[c]));

            if (c == bridged)
                g.fillPath(stroke.dots);
            else if (! stroke.path.isEmpty())
                g.strokePath(stroke.path, juce::PathStrokeType(thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
    }

    void paintGrid(juce::Graphics& g, juce::Rectangle<float> plot) const
    {
        using namespace Theme::Colours;

        const auto font = m_fonts.label(9.5f);
        g.setFont(font);

        // Octave lines on C, each named with its frequency.
        for (int octave = 0; octave <= 8; ++octave)
        {
            const double level = std::log2(c_c0Hz) + octave;

            if (level < m_low || level > m_high)
                continue;

            const float y = std::round(yFor(level));
            g.setColour(grid);
            g.fillRect(juce::Rectangle<float>(plot.getX(), y, plot.getWidth(), 1.0f));
            g.setColour(textFaint);
            g.drawText("C" + juce::String(octave) + "  " + hertzText(std::exp2(level)),
                       juce::Rectangle<float>(4.0f, y - 8.0f, c_axisWidth - 14.0f, 16.0f), juce::Justification::centredRight, false);
        }

        // Time: the region's own timeline positions.
        const double step = timeStep(m_end - m_start, plot.getWidth());
        const auto first = static_cast<juce::int64>(std::ceil(m_start / step - 1.0e-9));

        for (auto k = first;; ++k)
        {
            const double t = static_cast<double>(k) * step;

            if (t > m_end + 1.0e-9)
                break;

            const float x = std::round(xFor(t));
            g.setColour(grid.withAlpha(0.6f));
            g.fillRect(juce::Rectangle<float>(x, plot.getY(), 1.0f, plot.getHeight()));
            g.setColour(textFaint);
            g.drawText(LearnView::formatTime(t), juce::Rectangle<float>(x - 40.0f, plot.getBottom() + 4.0f, 80.0f, 14.0f),
                       juce::Justification::centred, false);
        }
    }

    void paintTags(juce::Graphics& g, juce::Rectangle<float> plot) const
    {
        using namespace Theme::Colours;

        const auto font = m_fonts.body(10.5f);
        g.setFont(font);

        for (std::size_t i = 0; i < m_lines.size(); ++i)
        {
            const auto& line = m_lines[i];

            if (! line.hasTag)
                continue;

            const bool hot = static_cast<int>(i) == m_hover && ! line.followed;
            const auto& r = line.tagBounds;
            const float radius = r.getHeight() * 0.5f;

            // A leader from where the line ends to its tag.
            g.setColour((line.followed ? input : textDim).withAlpha(hot ? 0.7f : 0.35f));
            g.drawLine(plot.getRight() + 2.0f, line.anchorY, r.getX(), r.getCentreY(), 1.0f);

            // A chip, as the harmonic strip's readout.
            g.setColour(background.withAlpha(0.94f));
            g.fillRoundedRectangle(r, radius);

            if (line.followed)
            {
                g.setColour(input.withAlpha(0.18f));
                g.fillRoundedRectangle(r, radius);
            }

            g.setColour(line.followed ? input.withAlpha(0.8f) : hot ? textDim : outline);
            g.drawRoundedRectangle(r.reduced(0.5f), radius, 1.0f);
            g.setColour(line.followed || hot ? text : textDim);
            g.drawText(line.tag, r, juce::Justification::centred, false);
        }
    }

    void paintEmpty(juce::Graphics& g, juce::Rectangle<float> bounds) const
    {
        using namespace Theme::Colours;

        const auto centre = bounds.getCentre();
        g.setFont(m_fonts.heading(11.0f));
        g.setColour(textDim);
        g.drawText(m_emptyTitle, juce::Rectangle<float>(bounds.getWidth(), 18.0f).withCentre(centre.translated(0.0f, -12.0f)),
                   juce::Justification::centred, false);

        if (m_emptyProgress >= 0.0f)
        {
            const auto track = juce::Rectangle<float>(220.0f, 4.0f).withCentre(centre.translated(0.0f, 6.0f));
            g.setColour(outline);
            g.fillRoundedRectangle(track, 2.0f);
            g.setColour(input);
            g.fillRoundedRectangle(track.withWidth(std::max(4.0f, track.getWidth() * juce::jlimit(0.0f, 1.0f, m_emptyProgress))), 2.0f);
        }

        g.setFont(m_fonts.label(9.5f));
        g.setColour(textFaint);
        g.drawText(m_emptyHint, juce::Rectangle<float>(bounds.getWidth(), 16.0f).withCentre(centre.translated(0.0f, 22.0f)),
                   juce::Justification::centred, false);
    }

    // The line under a point: a tag, else the nearest line within a few screen pixels (-1: none).
    int lineAt(juce::Point<float> position) const
    {
        if (m_region == nullptr || m_log2.empty())
            return -1;

        for (std::size_t i = 0; i < m_lines.size(); ++i)
            if (m_lines[i].hasTag && m_lines[i].tagBounds.expanded(2.0f).contains(position))
                return static_cast<int>(i);

        const auto plot = getPlot();

        if (! plot.expanded(0.0f, c_hitTolerance).contains(position))
            return -1;

        const float scale = juce::Component::getApproximateScaleFactorForComponent(this);
        const float tolerance = c_hitTolerance / (scale > 0.0f ? scale : 1.0f);
        const double seconds = m_start + static_cast<double>((position.x - plot.getX()) / plot.getWidth()) * (m_end - m_start);
        const double index = (seconds - m_region->alignmentSeconds - m_region->firstFrameSeconds) / learn::c_frameSeconds;
        const auto centre = static_cast<juce::int64>(std::llround(index));
        int best = -1;
        float bestDistance = tolerance;

        for (std::size_t i = 0; i < m_lines.size(); ++i)
        {
            const auto& line = m_lines[i];
            const double offset = std::log2(static_cast<double>(line.ratio));

            // A few frames either side, so a steep line is as easy to hit as a flat one.
            for (auto f = centre - 3; f <= centre + 3; ++f)
            {
                if (f < 0 || f >= static_cast<juce::int64>(m_log2.size()) || ! presentAt(line, static_cast<std::size_t>(f)))
                    continue;

                const auto p = juce::Point<float>(xFor(frameSeconds(static_cast<std::size_t>(f))),
                                                  yFor(static_cast<double>(m_log2[static_cast<std::size_t>(f)]) + offset));
                const float distance = p.getDistanceFrom(position);

                if (distance < bestDistance)
                {
                    bestDistance = distance;
                    best = static_cast<int>(i);
                }
            }
        }

        return best;
    }

    void setHover(int line)
    {
        if (line == m_hover)
            return;

        m_hover = line;
        const bool choosable = line >= 0 && ! m_lines[static_cast<std::size_t>(line)].followed;
        setMouseCursor(choosable ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
        repaint();
    }

    LearnView& m_owner;
    const Theme::Fonts& m_fonts;

    std::shared_ptr<const learn::Snapshot> m_snapshot;   // keeps m_region alive
    const learn::Region* m_region = nullptr;
    std::vector<float> m_log2;                           // per frame: log2 Hz of the analysis' own line
    std::vector<Line> m_lines;
    bool m_scored = false;                               // the candidates carry strengths worth showing
    std::array<Stroke, numCategories> m_followed, m_sub;

    double m_start = 0.0, m_end = 1.0;                   // seconds shown
    double m_low = std::log2(30.0), m_high = std::log2(1000.0);   // log2 Hz shown
    bool m_userZoom = false;
    int m_hover = -1;
    float m_pressedRatio = 0.0f;                         // the line the mouse was pressed on (0: none)

    juce::String m_emptyTitle, m_emptyHint;
    float m_emptyProgress = -1.0f;
};

//==============================================================================
/** Over the lane, moving with playback: the stretches that fell back to live tracking, what the live tracker hears,
    and the playhead. It takes no mouse clicks: they go to the lane underneath.

    It repaints only what moved since it was last set: the playhead where it was and where it is, the dots that came and
    went, and where the stretches grew or shrank. A change of the lane's own (another region, a zoom, a new size)
    repaints the lane, and this layer with it. When the trail holds less than two passes (short host blocks: it keeps a
    point a block), its oldest dots go at every update, far from the playhead; repainted at once, they would stretch
    every update's repaint from there to the playhead (on macOS the one rectangle around both is drawn). So they stay on
    show a little longer and go together, every c_tailUpdates updates. */
class LearnView::LiveLayer final : public juce::Component
{
public:
    struct Dot
    {
        double seconds = 0.0;
        float log2Hz = 0.0f;
        bool latest = true;   // of the latest pass (the one before is drawn fainter)
    };

    using Band = std::pair<double, double>;   // seconds, [start, end)

    explicit LiveLayer(const PitchLane& lane)
        : m_lane(lane)
    {
        setInterceptsMouseClicks(false, false);
        setOpaque(false);
    }

    /** playheadKnown: the host gave the playhead's position, playheadSeconds (which may lie before 0). */
    void set(std::vector<Dot> dots, std::vector<Band> bands, bool playheadKnown, double playheadSeconds, bool playing)
    {
        // With no region on show there is nothing to draw, however the playhead moves.
        const bool shows = m_lane.getRegion() != nullptr;

        if (shows != m_shows)
        {
            repaint();
            m_tailUpdates = 0;
        }
        else if (shows)
        {
            repaintChanges(dots, bands, playheadKnown, playheadSeconds, playing);
        }

        m_dots = std::move(dots);
        m_bands = std::move(bands);
        m_playheadKnown = playheadKnown;
        m_playhead = playheadSeconds;
        m_playing = playing;
        m_shows = shows;
    }

    double getBandSeconds() const
    {
        double total = 0.0;

        for (const auto& band : m_bands)
            total += band.second - band.first;

        return total;
    }

    void paint(juce::Graphics& g) override
    {
        using namespace Theme::Colours;

        if (m_lane.getRegion() == nullptr)
            return;

        const auto plot = m_lane.getPlot();
        g.reduceClipRegion(plot.expanded(1.0f).getSmallestIntegerContainer());

        // Only what reaches the area being drawn (most repaints here are a few pixels wide): the rest costs nothing.
        const auto clip = g.getClipBounds().toFloat().expanded(3.0f);

        for (const auto& [from, to] : m_bands)
        {
            const float x0 = m_lane.xFor(from), width = std::max(1.0f, m_lane.xFor(to) - x0);

            if (x0 > clip.getRight() || x0 + width < clip.getX())
                continue;

            g.setColour(bad.withAlpha(0.11f));
            g.fillRect(juce::Rectangle<float>(x0, plot.getY(), width, plot.getHeight()));
            g.setColour(bad.withAlpha(0.8f));
            g.fillRect(juce::Rectangle<float>(x0, plot.getBottom() - 2.0f, width, 2.0f));
        }

        juce::Path latest, older;

        for (const auto& dot : m_dots)
        {
            const float x = m_lane.xFor(dot.seconds);
            const float y = m_lane.yFor(static_cast<double>(dot.log2Hz));

            if (y >= plot.getY() && y <= plot.getBottom() && clip.contains(x, y))
                (dot.latest ? latest : older).addRectangle(x - 1.1f, y - 1.1f, 2.2f, 2.2f);
        }

        g.setColour(text.withAlpha(0.26f));
        g.fillPath(older);
        g.setColour(text.withAlpha(0.8f));
        g.fillPath(latest);

        if (m_playheadKnown && m_playhead >= m_lane.getStart() && m_playhead <= m_lane.getEnd())
        {
            const float x = m_lane.xFor(m_playhead);
            const auto colour = text.withAlpha(m_playing ? 0.75f : 0.35f);
            g.setColour(colour);
            g.fillRect(juce::Rectangle<float>(x - 0.5f, plot.getY(), 1.0f, plot.getHeight()));

            juce::Path marker;
            marker.addTriangle(x - 4.0f, plot.getY(), x + 4.0f, plot.getY(), x, plot.getY() + 5.0f);
            g.fillPath(marker);
        }
    }

private:
    static bool same(const Dot& a, const Dot& b) noexcept
    {
        return juce::exactlyEqual(a.seconds, b.seconds) && juce::exactlyEqual(a.log2Hz, b.log2Hz) && a.latest == b.latest;
    }

    // dots: the new ones, which may gain the oldest of last time (see the class notes).
    void repaintChanges(std::vector<Dot>& dots, const std::vector<Band>& bands, bool playheadKnown, double playheadSeconds,
                        bool playing)
    {
        // The playhead, where it was and where it is.
        if (playheadKnown != m_playheadKnown || ! juce::exactlyEqual(playheadSeconds, m_playhead) || playing != m_playing)
        {
            repaint(playheadArea(m_playheadKnown, m_playhead));
            repaint(playheadArea(playheadKnown, playheadSeconds));
        }

        // The dots. Anything but some new at the front and some gone from the back (a new pass, whose dots of the pass
        // before all turn fainter) repaints the plot.
        if (! repaintDotChanges(dots))
        {
            repaint(m_lane.getPlot().expanded(2.0f).getSmallestIntegerContainer());
            return;
        }

        // The stretches: where a band's ends moved, and the bands that came or went.
        double from = std::numeric_limits<double>::max(), to = std::numeric_limits<double>::lowest();
        const auto span = [&from, &to](double a, double b)
        {
            from = std::min({ from, a, b });
            to = std::max({ to, a, b });
        };
        const auto common = std::min(bands.size(), m_bands.size());

        for (std::size_t i = 0; i < common; ++i)
        {
            if (! juce::exactlyEqual(bands[i].first, m_bands[i].first))
                span(bands[i].first, m_bands[i].first);

            if (! juce::exactlyEqual(bands[i].second, m_bands[i].second))
                span(bands[i].second, m_bands[i].second);
        }

        for (auto i = common; i < bands.size(); ++i)
            span(bands[i].first, bands[i].second);

        for (auto i = common; i < m_bands.size(); ++i)
            span(m_bands[i].first, m_bands[i].second);

        if (from <= to)
        {
            // Over the plot's height (a band fills it), at least a pixel wide, with a pixel to spare either side.
            const auto plot = m_lane.getPlot();
            const float x0 = m_lane.xFor(from), x1 = m_lane.xFor(to);
            repaint(juce::Rectangle<float>::leftTopRightBottom(x0 - 2.0f, plot.getY() - 1.0f, x1 + 3.0f, plot.getBottom() + 1.0f)
                        .getSmallestIntegerContainer());
        }
    }

    // The dots come newest first: those before the newest of last time are new, the rest must be as they were, and
    // those past the end of either list came or went at the back. Those that went stay on show (they are added to dots)
    // until c_tailUpdates updates have passed. False if the rest are not as they were.
    bool repaintDotChanges(std::vector<Dot>& dots)
    {
        if (dots.empty() || m_dots.empty())
        {
            repaint(dotsArea(dots, 0, dots.size()));
            repaint(dotsArea(m_dots, 0, m_dots.size()));
            m_tailUpdates = 0;
            return true;
        }

        std::size_t added = 0;

        while (added < dots.size() && ! same(dots[added], m_dots.front()))
            ++added;

        const auto kept = std::min(m_dots.size(), dots.size() - added);   // (0 when the newest of last time is gone)
        bool asBefore = added < dots.size();

        for (std::size_t i = 0; asBefore && i < kept; ++i)
            asBefore = same(dots[added + i], m_dots[i]);

        if (! asBefore)
        {
            m_tailUpdates = 0;
            return false;
        }

        repaint(dotsArea(dots, 0, added));
        repaint(dotsArea(dots, added + kept, dots.size()));

        if (kept < m_dots.size())
        {
            if (++m_tailUpdates < c_tailUpdates)
            {
                dots.insert(dots.end(), m_dots.begin() + static_cast<std::ptrdiff_t>(kept), m_dots.end());
            }
            else
            {
                repaint(dotsArea(m_dots, kept, m_dots.size()));
                m_tailUpdates = 0;
            }
        }

        return true;
    }

    // Around the dots list[from, to) that are drawn (inside the plot's height), with a pixel to spare.
    juce::Rectangle<int> dotsArea(const std::vector<Dot>& list, std::size_t from, std::size_t to) const
    {
        const auto plot = m_lane.getPlot();
        juce::Rectangle<float> area;

        for (auto i = from; i < to; ++i)
        {
            const float y = m_lane.yFor(static_cast<double>(list[i].log2Hz));

            if (y < plot.getY() || y > plot.getBottom())
                continue;

            const auto dot = juce::Rectangle<float>(m_lane.xFor(list[i].seconds) - 2.0f, y - 2.0f, 4.0f, 4.0f);
            area = area.isEmpty() ? dot : area.getUnion(dot);
        }

        return area.getSmallestIntegerContainer();
    }

    // Where the playhead is drawn (nothing while it is not shown): its line and its marker, with a pixel to spare.
    juce::Rectangle<int> playheadArea(bool known, double seconds) const
    {
        if (! known || seconds < m_lane.getStart() || seconds > m_lane.getEnd())
            return {};

        const auto plot = m_lane.getPlot();
        const float x = m_lane.xFor(seconds);
        return juce::Rectangle<float>(x - 5.0f, plot.getY() - 1.0f, 10.0f, plot.getHeight() + 2.0f).getSmallestIntegerContainer();
    }

    static constexpr int c_tailUpdates = 10;   // the trail's oldest dots go together, at most this many updates late

    const PitchLane& m_lane;
    std::vector<Dot> m_dots;
    std::vector<Band> m_bands;
    bool m_playheadKnown = false;
    double m_playhead = 0.0;
    bool m_playing = false;
    bool m_shows = false;      // a region was on show when last set
    int m_tailUpdates = 0;     // updates for which dots gone from the trail's end have stayed on show
};

//==============================================================================
/** The region selector: a pill like the preset name, with a chevron like the editor's selectors. */
class LearnView::MenuButton final : public juce::TextButton
{
public:
    void paintButton(juce::Graphics& g, bool isMouseOverButton, bool isButtonDown) override
    {
        using namespace Theme::Colours;

        auto& lf = getLookAndFeel();
        lf.drawButtonBackground(g, *this, findColour(juce::TextButton::buttonColourId), isMouseOverButton, isButtonDown);

        auto r = getLocalBounds().toFloat().reduced(0.5f);
        const float pad = std::round(r.getHeight() * 0.5f);
        const auto chevronArea = r.removeFromRight(pad + 8.0f);
        r.removeFromLeft(pad);

        // A long name (ten regions or more, 10 h or more along the timeline or before -1 h) shrinks a little rather
        // than lose its end time. (Down to a size that still reads: a name too long even then ends in "...". A pixel
        // of slack: Graphics::drawText() drops a word that overruns its area by any amount.)
        const auto label = getButtonText();
        auto font = lf.getTextButtonFont(*this, getHeight());

        while (font.getHeight() > c_regionMinFontHeight && lineWidth(font, label) > r.getWidth() - 1.0f)
            font = font.withHeight(std::max(c_regionMinFontHeight, font.getHeight() - 0.5f));

        g.setFont(font);
        g.setColour(isEnabled() ? Theme::Colours::text : textFaint);   // (a Button has a member called text)
        g.drawText(label, r, juce::Justification::centredLeft, true);

        const auto c = chevronArea.getCentre().translated(-2.0f, 0.0f);
        juce::Path chevron;
        chevron.startNewSubPath(c.x - 4.0f, c.y - 2.0f);
        chevron.lineTo(c.x, c.y + 2.0f);
        chevron.lineTo(c.x + 4.0f, c.y - 2.0f);
        g.setColour(isEnabled() ? input : textFaint);
        g.strokePath(chevron, juce::PathStrokeType(1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
};

/** Close: a small pill with a cross drawn in strokes. */
class LearnView::CloseButton final : public juce::Button
{
public:
    CloseButton()
        : juce::Button("Close")
    {
        setTitle("Close");
    }

    void paintButton(juce::Graphics& g, bool isMouseOverButton, bool isButtonDown) override
    {
        using namespace Theme::Colours;

        getLookAndFeel().drawButtonBackground(g, *this, findColour(juce::TextButton::buttonColourId), isMouseOverButton, isButtonDown);

        const auto c = getLocalBounds().toFloat().getCentre();
        const float s = static_cast<float>(getHeight()) * 0.16f;
        juce::Path cross;
        cross.startNewSubPath(c.x - s, c.y - s);
        cross.lineTo(c.x + s, c.y + s);
        cross.startNewSubPath(c.x + s, c.y - s);
        cross.lineTo(c.x - s, c.y + s);
        g.setColour(isMouseOverButton || isButtonDown ? Theme::Colours::text : textDim);   // (a Button has a member called text)
        g.strokePath(cross, juce::PathStrokeType(1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
};

//==============================================================================
LearnView::LearnView(learn::Learner& learner, const Theme::Fonts& fonts)
    : m_learner(learner)
    , m_fonts(fonts)
{
    using namespace Theme::Colours;

    setOpaque(false);   // a rounded panel: its corners show the editor behind
    setWantsKeyboardFocus(true);

    m_regionButton = std::make_unique<MenuButton>();
    m_regionButton->setTooltip("The learned region on show. Its menu lists every region learned here, and can forget this one.");
    m_regionButton->setColour(juce::PopupMenu::highlightedBackgroundColourId, input);   // its menu's accent
    m_regionButton->onClick = [this] { showRegionMenu(); };
    addAndMakeVisible(*m_regionButton);

    for (auto* label : { &m_methodLabel, &m_gapFillLabel })
    {
        label->setColour(juce::Label::textColourId, textDim);
        label->setJustificationType(juce::Justification::centredLeft);
        label->setInterceptsMouseClicks(false, false);
        addAndMakeVisible(*label);
    }

    m_methodLabel.setText("METHOD", juce::dontSendNotification);
    m_gapFillLabel.setText("GAP FILL", juce::dontSendNotification);

    const juce::String again = " Choosing one analyses the region on show again (or, if its audio is no longer kept, "
                               "learns the loop with it on its next pass).";
    const std::array<std::pair<const char*, juce::String>, 3> methods {{
        { "AUTO",   "Auto: the analysis tries both methods and keeps the one that fits the sound." + again },
        { "ENGINE", "Engine: for sounds whose partials all move together, such as engines and drones." + again },
        { "TONAL",  "Tonal: for notes, such as bass lines and melodies." + again } }};

    for (std::size_t k = 0; k < m_methodButtons.size(); ++k)
    {
        auto& button = m_methodButtons[k];
        button.setButtonText(methods[k].first);
        button.setTooltip(methods[k].second);
        button.setColour(juce::TextButton::buttonOnColourId, input.withAlpha(0.28f));
        button.setColour(juce::TextButton::textColourOnId, text);
        button.onClick = [this, k] { chooseMethod(static_cast<learn::Method>(k)); };

        // A screen reader reads which one is lit. (No radio group: JUCE's would click the one it switches off too.
        // refreshControls() lights exactly one, whatever a click or a screen reader's toggle did.)
        button.setToggleable(true);
        addAndMakeVisible(button);
    }

    m_gapFillSlider.setTitle("Gap fill");   // what a screen reader calls it (the caption beside it is a label of its own)
    m_gapFillSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    m_gapFillSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    m_gapFillSlider.setRange(0.0, learn::c_maxGapFillSeconds * 1000.0, 1.0);
    m_gapFillSlider.setSkewFactorFromMidPoint(200.0);
    m_gapFillSlider.setDoubleClickReturnValue(true, learn::c_defaultGapFillSeconds * 1000.0);
    m_gapFillSlider.setValue(learn::c_defaultGapFillSeconds * 1000.0, juce::dontSendNotification);
    m_gapFillSlider.setColour(juce::Slider::rotarySliderFillColourId, input);
    m_gapFillSlider.textFromValueFunction = [](double v) { return millisecondsText(v); };
    m_gapFillSlider.setTooltip("The longest stretch without a clear pitch that the learned curve bridges, joining the pitch "
                               "either side. Silences are never bridged, so notes keep their ends.");
    m_gapFillSlider.onValueChange = [this] { setGapFillFromSlider(false); };
    m_gapFillSlider.onDragEnd = [this] { setGapFillFromSlider(true); };
    addAndMakeVisible(m_gapFillSlider);

    m_gapFillValue.setFont(m_fonts.body(12.0f));
    m_gapFillValue.setColour(juce::Label::textColourId, text);
    m_gapFillValue.setJustificationType(juce::Justification::centredLeft);
    m_gapFillValue.setInterceptsMouseClicks(false, false);
    m_gapFillValue.setText(millisecondsText(m_gapFillSlider.getValue()), juce::dontSendNotification);
    addAndMakeVisible(m_gapFillValue);

    m_followButton.setButtonText("USE LEARNED");
    m_followButton.setClickingTogglesState(true);
    m_followButton.setToggleState(true, juce::dontSendNotification);
    m_followButton.setColour(juce::TextButton::buttonOnColourId, input.withAlpha(0.28f));
    m_followButton.setColour(juce::TextButton::textColourOnId, text);
    m_followButton.setTooltip("Follow what was learned. Off, the pitch is tracked live everywhere, and what was learned is kept.");
    m_followButton.onClick = [this] { m_learner.setFollow(m_followButton.getToggleState()); };
    addAndMakeVisible(m_followButton);

    m_clearButton.setButtonText("CLEAR");
    m_clearButton.setTooltip("Forget everything learned here. UNDO brings it back for 10 s.");
    m_clearButton.onClick = [this] { m_learner.forgetAll(); applyState(m_learner.getStatus(), m_learner.getSnapshot()); };
    addAndMakeVisible(m_clearButton);

    m_undoButton.setButtonText("UNDO");
    m_undoButton.setTooltip("Bring back what was just forgotten.");
    m_undoButton.onClick = [this] { m_learner.undoForget(); applyState(m_learner.getStatus(), m_learner.getSnapshot()); };
    addAndMakeVisible(m_undoButton);

    m_closeButton = std::make_unique<CloseButton>();
    m_closeButton->setTooltip("Back to the scope (Esc). Double-clicking LEARN (or Shift-Return on it) opens this view again.");
    m_closeButton->onClick = [this] { if (onClose) onClose(); };
    addAndMakeVisible(*m_closeButton);

    m_lane = std::make_unique<PitchLane>(*this, fonts);
    addAndMakeVisible(*m_lane);
    m_live = std::make_unique<LiveLayer>(*m_lane);
    addAndMakeVisible(*m_live);

    applyState(m_learner.getStatus(), m_learner.getSnapshot());
}

LearnView::~LearnView() = default;

//==============================================================================
juce::String LearnView::describeRatio(float ratio)
{
    if (! (ratio > 0.0f) || ! std::isfinite(ratio))
        return "x?";

    // The simplest fraction within 15 cents, so "x5/2" reads as what it is.
    for (int q = 1; q <= 8; ++q)
    {
        const int p = juce::roundToInt(ratio * static_cast<float>(q));

        if (p < 1)
            continue;

        const double cents = 1200.0 * std::log2(static_cast<double>(ratio) * q / p);

        if (std::abs(cents) <= 15.0)
            return "x" + juce::String(p) + (q > 1 ? "/" + juce::String(q) : juce::String());
    }

    return "x" + juce::String(ratio, ratio < 1.0f ? 3 : 2);
}

juce::String LearnView::describeMethod(learn::Method method)
{
    switch (method)
    {
        case learn::Method::Engine: return "Engine";
        case learn::Method::Tonal:  return "Tonal";
        case learn::Method::Auto:   break;
    }

    return "Auto";
}

juce::String LearnView::formatTime(double seconds)
{
    // Rounded first, then signed: a host may report times before 0 (a count-in, bars before bar 1), and nothing that
    // rounds to 0:00.0 gets a minus.
    auto tenths = static_cast<juce::int64>(std::llround(std::isfinite(seconds) ? seconds * 10.0 : 0.0));
    juce::String text;

    if (tenths < 0)
    {
        text << "-";
        tenths = -tenths;
    }

    const auto whole = tenths / 10;
    const auto minutes = whole / 60;

    if (minutes >= 60)
        text << juce::String(minutes / 60) << ":" << juce::String(minutes % 60).paddedLeft('0', 2);
    else
        text << juce::String(minutes);

    text << ":" << juce::String(whole % 60).paddedLeft('0', 2) << "." << juce::String(tenths % 10);
    return text;
}

//==============================================================================
void LearnView::update(const learn::Status& status, const std::shared_ptr<const learn::Snapshot>& snapshot,
                       const std::vector<learn::TrailPoint>& trail)
{
    applyState(status, snapshot);
    applyTrail(trail);
    refreshStatusRow();   // again after the trail: the status line says how much no longer matches
}

void LearnView::applyState(const learn::Status& status, const std::shared_ptr<const learn::Snapshot>& snapshot)
{
    m_status = status;

    if (snapshot == nullptr)
        return;

    const bool changed = m_snapshot == nullptr || snapshot->revision != m_revision;
    m_snapshot = snapshot;
    m_revision = snapshot->revision;

    const auto before = m_selected;
    chooseRegion(*m_snapshot, status);

    if (changed || m_selected != before)
        m_lane->setRegion(m_snapshot, selectedRegion(), m_selected != before);

    m_lane->setActivity(status);

    // A METHOD click that armed LEARN is news while LEARN waits or listens.
    if (m_methodArmed && status.activity != Activity::armed && status.activity != Activity::capturing)
        m_methodArmed = false;

    refreshControls();
    refreshStatusRow();
}

void LearnView::applyTrail(const std::vector<learn::TrailPoint>& trail)
{
    std::vector<LiveLayer::Dot> dots;
    std::vector<LiveLayer::Band> bands;

    if (const auto* region = m_lane->getRegion())
    {
        // Newest first: the latest pass, then the one before (fainter). Going back in time, a pass starts where the
        // position jumps forward (the wrap, or a locate).
        const double start = m_lane->getStart(), end = m_lane->getEnd();
        double later = std::numeric_limits<double>::max();
        int pass = 0;

        for (auto it = trail.rbegin(); it != trail.rend(); ++it)
        {
            if (it->seconds > later + 0.02 && ++pass > 1)
                break;

            later = it->seconds;

            if (it->detectorHz > 0.0f && it->seconds >= start && it->seconds <= end)
                dots.push_back({ it->seconds, std::log2(it->detectorHz), pass == 0 });
        }

        // Fallback stretches: those of the pass under way, and where it has not got to yet, those of the pass before.
        const auto& live = *region->live;
        const auto hops = static_cast<std::uint32_t>(region->numHops());
        const double hop = static_cast<double>(region->hopSamples) / region->learnedRate;
        const double origin = region->followStartSeconds();
        const double playhead = m_status.playheadSeconds;   // when m_status.playheadKnown

        const auto read = [&](const auto& firsts, const auto& ends, int count, double from)
        {
            count = juce::jlimit(0, learn::RegionLive::c_maxStretches, count);

            for (std::size_t i = 0; i < static_cast<std::size_t>(count); ++i)
            {
                // Written by the audio thread meanwhile: a stretch may be read half updated, so keep it inside the region.
                const auto firstHop = std::min(hops, firsts[i].load(std::memory_order_relaxed));
                const auto endHop = std::min(hops, ends[i].load(std::memory_order_relaxed));
                const double to = origin + static_cast<double>(endHop) * hop;

                if (endHop > firstHop && to > from)
                    bands.emplace_back(std::max(from, origin + static_cast<double>(firstHop) * hop), to);
            }
        };

        read(live.stretchFirst, live.stretchEnd, live.numStretches.load(std::memory_order_relaxed), origin);

        if (m_status.playheadKnown && region->covers(playhead))
            read(live.lastStretchFirst, live.lastStretchEnd, live.lastNumStretches.load(std::memory_order_relaxed), playhead);
    }

    m_live->set(std::move(dots), std::move(bands), m_status.playheadKnown, m_status.playheadSeconds,
                m_status.timeline == Timeline::playing);
}

const learn::Region* LearnView::selectedRegion() const
{
    return m_snapshot != nullptr ? m_snapshot->findId(m_selected) : nullptr;
}

void LearnView::chooseRegion(const learn::Snapshot& snapshot, const learn::Status& status)
{
    // The playhead moved into another region: show that one.
    if (status.regionHere != 0 && status.regionHere != m_lastRegionHere && snapshot.findId(status.regionHere) != nullptr)
        m_selected = status.regionHere;

    m_lastRegionHere = status.regionHere;

    if (snapshot.findId(m_selected) == nullptr)
    {
        // Gone (analysed anew, cut back by a newer region, forgotten): the region that overlaps where it was the most,
        // else the one at the playhead, else the newest.
        std::uint32_t best = 0;
        double overlap = 0.0;

        if (m_selected != 0)
        {
            for (const auto& region : snapshot.regions)
            {
                const double shared = std::min(m_selectedEnd, region->followEndSeconds())
                                    - std::max(m_selectedStart, region->followStartSeconds());

                if (shared > overlap)
                {
                    overlap = shared;
                    best = region->id;
                }
            }
        }

        if (best == 0 && snapshot.findId(status.regionHere) != nullptr)
            best = status.regionHere;

        if (best == 0)
        {
            const learn::Region* newest = nullptr;

            for (const auto& region : snapshot.regions)
                if (newest == nullptr || newest->learnedBefore(*region))
                    newest = region.get();

            best = newest != nullptr ? newest->id : 0;
        }

        m_selected = best;
    }

    if (const auto* region = snapshot.findId(m_selected))
    {
        m_selectedStart = region->followStartSeconds();
        m_selectedEnd = region->followEndSeconds();
    }
}

void LearnView::selectRegion(std::uint32_t id)
{
    if (m_snapshot == nullptr || m_snapshot->findId(id) == nullptr || id == m_selected)
        return;

    m_selected = id;
    chooseRegion(*m_snapshot, m_status);
    m_lane->setRegion(m_snapshot, selectedRegion(), true);
    refreshControls();
    refreshStatusRow();
}

void LearnView::refreshStatusRow()
{
    // Repainted only when the line changes: a row that reads the same, repainted 20 times a second, is wasted, and
    // drawn in one with whatever else is repainted in that frame it is not cheap (see the class notes).
    auto runs = statusRuns();

    if (runs != m_shownRuns)
    {
        m_shownRuns = std::move(runs);
        repaint(m_statusRow);
    }
}

void LearnView::refreshControls()
{
    const auto* region = selectedRegion();
    const int count = m_snapshot != nullptr ? static_cast<int>(m_snapshot->regions.size()) : 0;
    int index = 0;

    for (int i = 0; i < count; ++i)
        if (m_snapshot->regions[static_cast<std::size_t>(i)]->id == m_selected)
            index = i;

    m_regionButton->setButtonText(region != nullptr ? juce::String(index + 1) + " OF " + juce::String(count) + c_separator
                                                          + formatTime(region->followStartSeconds()) + " - "
                                                          + formatTime(region->followEndSeconds())
                                                    : juce::String("NOTHING LEARNED YET"));
    m_regionButton->setEnabled(count > 0);

    // METHOD: what the capture or analysis under way uses, else what the region on show asked for.
    const auto method = m_status.activity != Activity::idle ? m_status.method
                      : region != nullptr                   ? region->requested
                                                            : learn::Method::Auto;

    for (std::size_t k = 0; k < m_methodButtons.size(); ++k)
        m_methodButtons[k].setToggleState(static_cast<std::size_t>(method) == k, juce::dontSendNotification);

    m_gapFillSlider.setEnabled(region != nullptr);

    if (region != nullptr && ! m_gapFillSlider.isMouseButtonDown())
        m_gapFillSlider.setValue(static_cast<double>(region->gapFillSeconds) * 1000.0, juce::dontSendNotification);

    m_gapFillValue.setText(millisecondsText(m_gapFillSlider.getValue()), juce::dontSendNotification);
    m_gapFillValue.setColour(juce::Label::textColourId, region != nullptr ? Theme::Colours::text : Theme::Colours::textFaint);

    m_followButton.setToggleState(m_status.follow, juce::dontSendNotification);
    m_clearButton.setEnabled(count > 0 || m_status.activity != Activity::idle);
    m_undoButton.setEnabled(m_status.canUndo);
}

//==============================================================================
double LearnView::getMismatchSeconds() const
{
    return m_live->getBandSeconds();
}

juce::String LearnView::getStatusText() const
{
    juce::String text;

    for (const auto& run : m_shownRuns)
        text << run.text;

    return text;
}

void LearnView::followLine(float ratio)
{
    if (const auto* region = selectedRegion())
        if (m_learner.setLine(region->id, ratio))
            applyState(m_learner.getStatus(), m_learner.getSnapshot());
}

void LearnView::chooseMethod(learn::Method method)
{
    if (const auto* region = selectedRegion())
    {
        if (m_status.activity == Activity::idle && region->requested == method)
            return;   // what it was learned with already

        m_methodArmed = ! m_learner.relearn(region->id, method);
    }
    else
    {
        m_methodArmed = m_learner.startLearning(method);
    }

    m_armedMethod = method;
    applyState(m_learner.getStatus(), m_learner.getSnapshot());
}

void LearnView::setGapFillFromSlider(bool dragEnded)
{
    const double ms = m_gapFillSlider.getValue();
    m_gapFillValue.setText(millisecondsText(ms), juce::dontSendNotification);

    // While the slider is dragged the curve follows a few times a second, not at every step: each change is a new
    // state for the host to save. Where it is let go always applies.
    const double now = juce::Time::getMillisecondCounterHiRes();

    if (! dragEnded && m_gapFillSlider.isMouseButtonDown() && now - m_lastGapFillMs < c_gapFillDragIntervalMs)
        return;

    m_lastGapFillMs = now;

    // Against what is learned now (the snapshot on show may be a change behind).
    const auto seconds = static_cast<float>(ms / 1000.0);
    const auto current = m_learner.getSnapshot();

    if (const auto* region = current != nullptr ? current->findId(m_selected) : nullptr)
        if (! juce::approximatelyEqual(region->gapFillSeconds, seconds))
            m_learner.setGapFill(region->id, seconds);
}

void LearnView::showRegionMenu()
{
    if (m_snapshot == nullptr || m_snapshot->regions.empty())
        return;

    constexpr int forgetItem = 10000;
    std::vector<std::uint32_t> ids;

    juce::PopupMenu menu;
    menu.setLookAndFeel(&getLookAndFeel());   // a menu without a parent window would use JUCE's default look
    menu.addSectionHeader("LEARNED REGIONS");

    for (const auto& region : m_snapshot->regions)
    {
        ids.push_back(region->id);
        menu.addItem(static_cast<int>(ids.size()), juce::String(static_cast<int>(ids.size())) + "   "
                         + formatTime(region->followStartSeconds()) + " - " + formatTime(region->followEndSeconds())
                         + " (" + describeMethod(region->used) + ")",
                     true, region->id == m_selected);
    }

    menu.addSeparator();
    menu.addItem(forgetItem, "Forget this region", selectedRegion() != nullptr);

    menu.showMenuAsync(SubbertoneLookAndFeel::menuOptionsFor(*m_regionButton, m_regionButton->getWidth()),
                       [safe = juce::Component::SafePointer<LearnView>(this), ids, forget = m_selected](int result)
    {
        if (safe == nullptr || result == 0)
            return;

        if (result == forgetItem)
        {
            safe->m_learner.forgetRegion(forget);   // the region named when the menu opened
            safe->applyState(safe->m_learner.getStatus(), safe->m_learner.getSnapshot());
        }
        else if (result >= 1 && result <= static_cast<int>(ids.size()))
        {
            safe->selectRegion(ids[static_cast<std::size_t>(result - 1)]);
        }
    });
}

//==============================================================================
std::vector<LearnView::Run> LearnView::statusRuns() const
{
    using namespace Theme::Colours;

    std::vector<Run> runs;
    const auto upper = [](learn::Method m) { return describeMethod(m).toUpperCase(); };
    const auto with = m_status.method == learn::Method::Auto ? juce::String() : " WITH " + upper(m_status.method);

    // What LEARN is doing, first.
    switch (m_status.activity)
    {
        case Activity::armed:
            runs.push_back({ m_methodArmed ? "PLAY THE LOOP AGAIN TO LEARN IT WITH " + upper(m_armedMethod)
                                           : juce::String("WAITING FOR PLAYBACK: START THE LOOP"), input });
            break;

        case Activity::capturing:
            runs.push_back({ (m_status.progress >= 0.0f ? "LISTENING TO THE LOOP (" + percentText(m_status.progress) + ")"
                                                        : "LISTENING (" + juce::String(m_status.heardSeconds, 1) + " s)") + with, input });
            break;

        case Activity::analysing:
            runs.push_back({ "WORKING OUT THE PITCH" + with + " (" + percentText(m_status.progress) + ")", input });
            break;

        case Activity::idle:
            break;
    }

    const auto* region = selectedRegion();

    if (region == nullptr)
        return runs;

    const auto separator = [&runs] { if (! runs.empty()) runs.push_back({ c_separator, textFaint }); };
    const auto& quality = region->quality;
    const auto method = region->requested == learn::Method::Auto ? "AUTO: " + upper(region->used) : upper(region->used);

    separator();
    runs.push_back({ method, textDim });

    // The warnings next, before the figures: a line too long for its row is cut at its end (paintStatusLine()), and
    // what it then loses should be a figure, never a warning.
    if (const double live = m_live->getBandSeconds(); live >= 0.05)
    {
        separator();
        runs.push_back({ juce::String(live, 1) + " s NO LONGER MATCHES: TRACKED LIVE", bad });
    }

    if (region->uncertain)
    {
        separator();
        runs.push_back({ "AMBIGUOUS: CHECK THE LINE", output });
    }

    // STEPS: where the line moves by more than a semitone at once, each move counted once (a bass line's note
    // changes; an engine's line glides, so it has few if any).
    for (const auto& item : { "LINE " + describeRatio(region->lineRatio),
                              percentText(quality.voicedShare) + " VOICED",
                              juce::String(quality.jumps) + (quality.jumps == 1 ? " STEP" : " STEPS"),
                              millisecondsText(static_cast<double>(quality.bridgedSeconds) * 1000.0) + " FILLED",
                              "CONF " + juce::String(quality.meanConfidence, 2),
                              "LEARNED AT " + rateText(region->learnedRate),
                              juce::String(region->durationSeconds(), 1) + " s" })
    {
        separator();
        runs.push_back({ item, textDim });
    }

    return runs;
}

void LearnView::paintStatusLine(juce::Graphics& g, juce::Rectangle<float> area) const
{
    const auto font = m_fonts.label(10.0f);
    g.setFont(font);

    for (const auto& run : m_shownRuns)
    {
        const float width = lineWidth(font, run.text);
        g.setColour(run.colour);

        if (width > area.getWidth())
        {
            // The rest does not fit: as much as does, then "...".
            auto text = run.text;

            while (text.isNotEmpty() && lineWidth(font, text.trimEnd() + "...") > area.getWidth())
                text = text.dropLastCharacters(1);

            if (text.trim().isNotEmpty())
                g.drawText(text.trimEnd() + "...", area, juce::Justification::centredLeft, false);

            return;
        }

        g.drawText(run.text, area, juce::Justification::centredLeft, false);
        area.removeFromLeft(width);
    }
}

float LearnView::paintLegend(juce::Graphics& g, juce::Rectangle<float> area) const
{
    using namespace Theme::Colours;

    // Right-aligned, like the harmonic strip's hint: a swatch of each mark, then its name.
    const auto font = m_fonts.label(9.5f);
    const std::array<const char*, 5> names { "LINE", "SUB", "OTHER LINES", "LIVE TRACKER", "NO MATCH" };
    constexpr float swatch = 14.0f, gap = 6.0f, spacing = 18.0f;
    float x = area.getRight();
    const float y = area.getCentreY();

    g.setFont(font);

    for (auto i = names.size(); i-- > 0;)
    {
        const float width = lineWidth(font, names[i]);
        x -= width;
        g.setColour(textFaint);
        g.drawText(names[i], juce::Rectangle<float>(x, area.getY(), width + 2.0f, area.getHeight()), juce::Justification::centredLeft, false);
        x -= gap + swatch;

        const auto s = juce::Rectangle<float>(x, y - 4.0f, swatch, 8.0f);

        switch (i)
        {
            case 0: g.setColour(input); g.fillRoundedRectangle(s.withSizeKeepingCentre(swatch, 2.0f), 1.0f); break;
            case 1: g.setColour(output); g.fillRoundedRectangle(s.withSizeKeepingCentre(swatch, 1.5f), 0.75f); break;
            case 2: g.setColour(textDim.withAlpha(0.5f)); g.fillRect(s.withSizeKeepingCentre(swatch, 1.2f)); break;
            case 3:
                g.setColour(text.withAlpha(0.8f));
                for (int d = 0; d < 3; ++d)
                    g.fillRect(juce::Rectangle<float>(2.2f, 2.2f).withCentre({ s.getX() + 2.0f + 5.0f * static_cast<float>(d), y + (d == 1 ? -1.5f : 0.5f) }));
                break;
            default:
                g.setColour(bad.withAlpha(0.22f));
                g.fillRect(s.withSizeKeepingCentre(swatch - 4.0f, 8.0f));
                g.setColour(bad.withAlpha(0.8f));
                g.fillRect(s.withSizeKeepingCentre(swatch - 4.0f, 8.0f).removeFromBottom(2.0f));
                break;
        }

        x -= spacing;
    }

    return area.getRight() - x;
}

void LearnView::paint(juce::Graphics& g)
{
    using namespace Theme::Colours;

    const auto bounds = getLocalBounds().toFloat();

    if (auto* lf = dynamic_cast<SubbertoneLookAndFeel*>(&getLookAndFeel()))
    {
        lf->drawPanel(g, bounds, "LEARN", input, static_cast<float>(c_headerHeight));
    }
    else
    {
        g.setColour(panel);
        g.fillRoundedRectangle(bounds, 8.0f);
    }

    // The legend and the status line only when their row is drawn: most repaints in the view are a few pixels of the
    // lane, and the row is text.
    if (g.clipRegionIntersects(m_statusRow))
    {
        auto status = m_statusRow.toFloat().reduced(14.0f, 0.0f);
        const float legend = paintLegend(g, status);
        paintStatusLine(g, status.withTrimmedRight(legend + 24.0f));
    }
}

void LearnView::resized()
{
    auto bounds = getLocalBounds();
    m_headerRow = bounds.removeFromTop(c_headerHeight);
    m_statusRow = bounds.removeFromBottom(c_statusHeight);
    m_laneArea = bounds.reduced(12, 0).withTrimmedTop(10);
    m_lane->setBounds(m_laneArea);
    m_live->setBounds(m_laneArea);

    // Header: after the dot and title (as SubbertoneLookAndFeel::drawPanel draws them), the region, METHOD and
    // GAP FILL; USE LEARNED, CLEAR, UNDO and close on the right.
    auto row = m_headerRow.reduced(12, c_controlInset);
    const auto caption = [this](const juce::String& text)
    {
        return juce::roundToInt(lineWidth(m_fonts.label(10.5f), text)) + 12;
    };

    row.removeFromLeft(14 + juce::roundToInt(lineWidth(m_fonts.heading(11.0f), "LEARN")) + 22);
    m_regionButton->setBounds(row.removeFromLeft(276));
    row.removeFromLeft(22);
    m_methodLabel.setBounds(row.removeFromLeft(caption(m_methodLabel.getText())));

    constexpr std::array<int, 3> methodWidths { 58, 70, 62 };

    for (std::size_t k = 0; k < m_methodButtons.size(); ++k)
    {
        m_methodButtons[k].setBounds(row.removeFromLeft(methodWidths[k]));
        row.removeFromLeft(4);
    }

    row.removeFromLeft(18);
    m_gapFillLabel.setBounds(row.removeFromLeft(caption(m_gapFillLabel.getText())));
    m_gapFillSlider.setBounds(row.removeFromLeft(150));
    row.removeFromLeft(8);
    m_gapFillValue.setBounds(row.removeFromLeft(64));

    m_closeButton->setBounds(row.removeFromRight(row.getHeight()));
    row.removeFromRight(10);
    m_undoButton.setBounds(row.removeFromRight(64));
    row.removeFromRight(6);
    m_clearButton.setBounds(row.removeFromRight(64));
    row.removeFromRight(16);
    m_followButton.setBounds(row.removeFromRight(118));
}

bool LearnView::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        if (onClose)
            onClose();

        return true;
    }

    return false;
}
