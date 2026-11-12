#pragma once

#include <JuceHeader.h>

#include <vector>

/**
 * Animated outline of the Sweet Justice logo (grenade + cherry), drawn in the
 * sea view while the rig is quiet, and in About.
 *
 * Every filled path in the SVG is split into its subpaths, and each subpath is
 * drawn as one closed contour. The white details in the artwork are gaps
 * between (or holes in) the fills, so they appear as the inner outlines.
 * Everything is parsed, flattened, normalised and resampled at even arc length
 * in the constructor. Drawing only reads that data and keeps no state between
 * calls.
 *
 * Timeline (timeSeconds counts up from when the logo should start appearing):
 *  - 0-3 s: each contour is traced from its start point by a glowing comet,
 *    longest (outer) contours first, leaving a line that settles to ~40% alpha.
 *  - After that the outline stays lit. Each comet slows down into a shimmer
 *    pulse that keeps travelling around its contour, the alpha breathes, the
 *    colours drift, and every 12 s a new comet traces over the whole logo again.
 *    The outline never blanks and redraws.
 *
 * Drawn with juce::Graphics (renderPreview()); measureFrame() runs the same
 * per-vertex style function and view matrix with a counting sink, for tests.
 */
class LogoOutline
{
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LogoOutline)

public:
    /** Parses the SVG and precomputes all geometry. Call on the message thread (it creates Drawables). */
    LogoOutline(const void* svgData, size_t svgSize);

    bool isLoaded() const { return m_loaded; }

    /** Draws the frame at timeSeconds into a juce::Graphics, fitted into area. */
    void renderPreview(juce::Graphics& g, juce::Rectangle<float> area, float timeSeconds, float opacity) const;

    //==============================================================================
    // Introspection, mainly for tests.

    int getNumContours() const { return static_cast<int>(m_contours.size()); }
    int getNumPoints() const { return static_cast<int>(m_points.size()); }

    /** Combined length of all contours, in normalised units (the logo's longer side is 1). */
    float getTotalLength() const { return m_totalLength; }

    /** Bounds of the outline in the coordinates of the parsed SVG drawable. */
    juce::Rectangle<float> getSourceBounds() const { return m_sourceBounds; }

    /** Maps the parsed SVG's coordinates onto the normalised, centred, y-up logo coordinates. */
    juce::AffineTransform getNormalisingTransform() const { return m_normalise; }

    /** Width and height of the logo in normalised units (the larger one is 1). */
    juce::Point<float> getNormalisedSize() const { return { 2.0f * m_halfWidth, 2.0f * m_halfHeight }; }

    /** The resampled contours as closed subpaths, in normalised coordinates. */
    juce::Path createOutlinePath() const;

    struct FrameStats
    {
        int strips = 0;                    // line strips in the frame
        int vertices = 0;                  // vertices in the frame
        float revealed = 0.0f;             // drawn length / total outline length
        float minLineAlpha = 0.0f;         // range of alpha on the core line
        float maxLineAlpha = 0.0f;
        float maxGlowAlpha = 0.0f;         // brightest halo/glow vertex
        juce::Rectangle<float> ndcBounds;  // projected outline bounds; the viewport is -1..1
    };

    /** Runs the drawing code with a counting sink and no graphics. Does not allocate. */
    FrameStats measureFrame(float timeSeconds, float opacity, float viewportAspect) const;

private:
    struct Contour
    {
        int first = 0;              // index of the first point in m_points
        int count = 0;              // number of points; the loop closes from the last back to the first
        float length = 0.0f;        // normalised units
        bool cherry = false;
        float traceStart = 0.0f;    // seconds
        float traceDuration = 1.0f; // seconds
        float trailFraction = 0.2f; // comet trail length, as a fraction of the contour
        float shimmerLap = 6.0f;    // seconds per shimmer lap
    };

    struct FrameState;
    struct ContourState;

    FrameState makeFrameState(float timeSeconds, float opacity, float viewportAspect) const;
    static ContourState computeContourState(const Contour& contour, double time);

    template <typename Sink>
    void emitFrame(const FrameState& frame, Sink& sink) const;

    template <typename Sink>
    void emitContour(const Contour& contour, const ContourState& state, size_t passIndex,
                     const FrameState& frame, Sink& sink) const;

    std::vector<juce::Point<float>> m_points; // normalised, centred, y-up; contours back to back
    std::vector<float> m_driftSin;            // per point: sin/cos of the colour drift's spatial phase,
    std::vector<float> m_driftCos;            // so drawing needs no trig per vertex
    std::vector<Contour> m_contours;

    juce::Rectangle<float> m_sourceBounds;
    juce::AffineTransform m_normalise;
    float m_halfWidth = 0.5f;
    float m_halfHeight = 0.5f;
    float m_totalLength = 0.0f;
    bool m_loaded = false;
};
