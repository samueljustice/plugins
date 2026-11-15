#pragma once

#include <JuceHeader.h>

#include "Engine.h"
#include "Hud.h"

#include <cstdint>
#include <memory>
#include <vector>

/**
 * The FLIGHT view: the learned sound as a course flown in low-resolution 3D. The camera travels
 * along the sound's timeline; the track (cyan edges) rises and dips exactly as the pitch line in
 * use does, and beside it the flattened lane (lime edges) runs level at the target's height (or
 * part way back towards the track when Amount is below 100%). A small dart rides the lane at the
 * play head; a gate crosses the track every second.
 *
 * It is drawn the way a 32-bit console drew: flat-shaded triangles with their corners snapped to
 * whole pixels, a checker texture mapped affinely (not perspective-correct, so it swims a little),
 * fog to the window colour, and the finished frame ordered-dithered down to 5 bits a channel, then
 * shown at three times its size with no smoothing so the pixels show. All software, no GPU.
 *
 * The maths is in `flight` (plain C++), so the tests can check it without a window.
 */
namespace flight
{
struct Vec3
{
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

struct Rgb
{
    float r = 0.0f, g = 0.0f, b = 0.0f;   // 0..1
};

/** A pinhole camera looking along +z, level, with +y up and the horizon at row cy. */
struct Camera
{
    float x = 0.0f, y = 1.0f, z = 0.0f;
    float focal = 140.0f;                  // pixels
    float cx = 160.0f, cy = 30.0f;         // the image point straight ahead
    float nearZ = 0.25f;                   // nothing nearer than this is drawn
};

struct Projected
{
    int x = 0, y = 0;                      // snapped to whole pixels, the period's wobble
    float depth = 0.0f;                    // distance ahead of the camera
    bool inFront = false;
};

/** Where a point lands on screen. */
Projected project (const Camera&, Vec3) noexcept;

//==============================================================================
inline constexpr double kStepSeconds = 0.04;   // one course slice
inline constexpr float kCourseSpan = 2.2f;     // world units the sound's pitch range spans
inline constexpr double kMinSemitones = 8.0;   // a narrower range is not stretched beyond this
inline constexpr float kUnitsPerSecond = 7.0f; // course length per second of sound

/** World height per semitone, so the range from lowMidi to highMidi fits kCourseSpan. */
double unitsPerSemitone (double lowMidi, double highMidi) noexcept;

/** A course: the track and lane heights sampled every stepSeconds along the sound. */
struct Course
{
    bool valid = false;
    bool demo = false;                      // the attract loop, not a learned sound
    bool hasLane = false;                   // a flattened result exists
    double stepSeconds = kStepSeconds;
    double seconds = 0.0;
    std::vector<float> track, lane;
    float lowest = 0.0f, highest = 0.0f;   // over both, for the ground and the camera

    /** Height at a time (linear between slices, held at the ends). */
    static float at (const std::vector<float>&, double stepSeconds, double seconds) noexcept;
};

/**
 * The course for a pitch line: the track follows the line, the lane sits at the target, moved
 * back towards the line by (1 - amount). Heights are relative to the middle of the range.
 */
Course buildCourse (const pitchflattener::core::PitchLine&, bool flattened, double targetMidi, double amount,
                    double stepSeconds = kStepSeconds);

/** A gently undulating course with a level lane, flown while nothing is learned. */
Course demoCourse();

/** One channel (0..255) ordered-dithered (4 x 4 Bayer) to 5 bits (0..31) at pixel (x, y). */
int dither5 (int value, int x, int y) noexcept;
/** A 5-bit channel back to 8 bits, as the hardware's DAC would (0..31 -> 0..255). */
std::uint8_t expand5 (int value5) noexcept;

/** A frame being drawn: linear colour per pixel, dithered when it is turned into an image. */
class Raster
{
public:
    void setSize (int width, int height);
    int width() const noexcept { return m_width; }
    int height() const noexcept { return m_height; }
    const Rgb& at (int x, int y) const noexcept { return m_pixels[(std::size_t) (y * m_width + x)]; }
    void set (int x, int y, Rgb c) noexcept;

    struct Vertex
    {
        int x = 0, y = 0;
        float u = 0.0f, v = 0.0f, fog = 0.0f;
    };
    struct Material
    {
        Rgb a, b;                          // b is the checker's other square; checker off = a only
        bool checker = false;
        float shade = 1.0f;                // flat shading: one light value for the whole triangle
    };
    /** Fills a triangle: texture coordinates and fog interpolated affinely across the screen. */
    void triangle (const Vertex&, const Vertex&, const Vertex&, const Material&) noexcept;
    /** A one-pixel line, fogged from end to end. */
    void line (int x0, int y0, float fog0, int x1, int y1, float fog1, Rgb colour) noexcept;

private:
    int m_width = 0, m_height = 0;
    std::vector<Rgb> m_pixels;
};

/** What a frame shows. */
struct Scene
{
    const Course* course = nullptr;
    double focusSeconds = 0.0;             // the moment of the sound the dart is at
    double clockSeconds = 0.0;             // for the stars' drift
};

/** Draws a whole frame. */
void render (Raster&, const Scene&);

/** The camera a frame uses (exposed for the tests). */
Camera cameraFor (const Raster&, const Scene&) noexcept;
} // namespace flight

//==============================================================================
class FlightView final : public juce::Component, public juce::TooltipClient
{
public:
    explicit FlightView (const Hud::Clock&);

    /** What to fly: the learned result (null = the attract loop), and where the play head is. */
    void setView (std::shared_ptr<const pitchflattener::Result> result, double playSeconds /* -1 = not playing */);
    /** Draws a new frame and repaints (the editor's 30 Hz timer calls this while the view shows). */
    void renderFrame();

    void paint (juce::Graphics&) override;
    void resized() override;
    juce::String getTooltip() override;

    double lastFrameMilliseconds() const noexcept { return m_lastFrameMs; }
    static constexpr int kPixelScale = 3;

private:
    const Hud::Clock& m_clock;
    juce::SharedResourcePointer<Theme::Fonts> m_fonts;
    std::shared_ptr<const pitchflattener::Result> m_result;
    flight::Course m_course, m_demo;
    double m_playSeconds = -1.0, m_focus = 0.0;
    flight::Raster m_raster;
    juce::Image m_image;
    double m_lastFrameMs = 0.0;
};
