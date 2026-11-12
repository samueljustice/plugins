#pragma once

#include <JuceHeader.h>

#include <array>

/**
 * The morph dot's wander: the path it flies along and where it is on it.
 *
 * The dot travels along the chosen shape, fitted into the pad, at a constant speed across the pad. Lorenz is the
 * Lorenz attractor
 *     dx/dt = 10 (y - x),  dy/dt = x (28 - z) - y,  dz/dt = x y - 8/3 z
 * seen side on (x across, z up) and integrated live, so the dot loops round one wing or the other chaotically and
 * never repeats. The closed shapes turn slowly (1 degree per lap), so they never repeat exactly either.
 *
 * The processor steps one of these at control rate (so the morph moves with the window closed and in an offline
 * bounce) and publishes the position; the pad only draws it. The shapes' points are shared tables, built once by
 * prepareTables() off the audio thread; step() and join() only read them (no allocation, no locks).
 */
class MorphWander
{
public:
    enum Shape { Lorenz = 0, Cherries, FigureEight, Rose, Circle, Butterfly, NumShapes };
    static juce::StringArray shapeNames() { return { "Lorenz", "Cherries", "Figure Eight", "Rose", "Circle", "Butterfly" }; }

    static constexpr int c_curvePoints = 2400;
    using Curve = std::array<juce::Point<float>, c_curvePoints>;

    /** Builds the shared tables. Call before the first step() / join(), not on the audio thread. The Cherries shape
        is drawn from the logo artwork, which has to be read on the message thread: called from another thread, it
        is filled in a moment later, and until then Cherries flies a circle. */
    static void prepareTables();
    /** A shape's raw points (before they are turned and fitted into the pad). */
    static const Curve& curve(int shape);
    /** The bounds of a shape's raw points after turning them by rotation (radians). */
    static juce::Rectangle<float> bounds(int shape, float rotation);
    /** A raw point turned and fitted into the pad: 0..1 across and up, inside the margin. */
    static juce::Point<float> toPad(juce::Point<float> raw, float rotation, juce::Rectangle<float> bounds);

    /** Starts from where the shape passes closest to (x, y), so the dot doesn't jump. */
    void join(int shape, float x, float y);
    /** Moves the dot on by this many seconds at this Speed (0..1). */
    void step(float seconds, int shape, float speed);

    float getX() const { return m_x; }
    float getY() const { return m_y; }
    /** How far the closed shapes have turned (radians; 0 for Lorenz). */
    float getRotation() const { return m_rotation; }

    /** Pad widths per second for a Speed setting: 0.05 (a very slow drift) .. 1.5 (busy). */
    static float unitsPerSecond(float speed);

private:
    juce::Point<float> shapeNorm(int shape, double t, juce::Rectangle<float> box) const;
    /** bounds(), remembered: it walks every point of the shape, and step() runs up to a thousand times a second.
        Worked out again only when the shape changes or has turned far enough to matter (0.01 degrees). */
    juce::Rectangle<float> boxFor(int shape, float rotation);
    juce::Rectangle<float> m_box;
    int m_boxShape = -1;
    float m_boxRotation = 0.0f;
    bool m_boxCherries = false;

    int m_shape = -1;
    double m_t = 0.0, m_laps = 0.0;
    float m_rotation = 0.0f;
    juce::Vector3D<double> m_lorenz { 1.0, 1.0, 20.0 };
    float m_x = 0.5f, m_y = 0.5f;
};
