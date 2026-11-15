#pragma once

#include <JuceHeader.h>

#include <cmath>
#include <vector>

/**
 * The motion language: how long anything in the interface takes to change, and the one clock it changes on.
 *
 * THREE TIERS, AND NOTHING ELSE.
 *
 *   Tier 0  Truth, 0 ms.  Anything that reports the sound or sits under the hand moves the instant it changes:
 *                         meters rising, lamps lighting, playheads, a value arc under a drag, a dragged dot.
 *                         Only the way back is eased (a lamp's afterglow, a meter's fall, a peak hold).
 *   Tier 1  Deck.         Every other change of state eases on Motion::Fade with ease-out cubic, never longer
 *                         than 240 ms: hover in 90 / out 140, a toggle's fill 120, dimming 160, an overlay or a
 *                         toast 180 in / 140 out, a lamp's afterglow 300.
 *   Tier 2  Rigging.      Things that are hardware move on one of two springs, and no others:
 *                         Helm  4.5 Hz, damping 0.6  - the wheels, plugs, small parts that are knocked about;
 *                         Swell 2.2 Hz, damping 0.55 - the preset glide, the Swell button, a rope's sag.
 *
 * WHAT NEVER MOVES: a control's position, a panel's layout, and text. No label slides, no numeral counts up, no
 * caption is animated. A readout changes to its new value and that is all.
 *
 * ONE CLOCK. Motion::FrameClock is ticked once a frame by the editor's timer with the real time since the last
 * tick (clamped, so a stalled message thread does not throw a spring across the window), and everything that
 * animates listens to it rather than starting a timer of its own. Offline (tools/UiShot.cpp, Tests/UiTest.cpp)
 * the same tick is called by hand, so the pictures are made the same way the window draws them.
 */
namespace Motion
{
    /** How long a change of state takes, in milliseconds. Nothing in tier 1 is longer than c_longest. */
    namespace Ms
    {
        constexpr float hoverIn = 90.0f, hoverOut = 140.0f;
        constexpr float toggle = 120.0f;
        constexpr float dim = 160.0f;
        constexpr float overlayIn = 180.0f, overlayOut = 140.0f;
        constexpr float afterglow = 300.0f;  // a lamp going out
        constexpr float spinUp = 250.0f, coastDown = 600.0f; // the tape's reels
        constexpr float longest = 240.0f;
    }

    /** The two springs, and no others. */
    namespace Springs
    {
        constexpr float helmHz = 4.5f, helmDamping = 0.6f;
        constexpr float swellHz = 2.2f, swellDamping = 0.55f;
    }

    /** The one easing curve of tier 1: fast away from the old state, settling into the new one. */
    inline float easeOutCubic(float t) noexcept
    {
        t = juce::jlimit(0.0f, 1.0f, t);
        const float u = 1.0f - t;
        return 1.0f - u * u * u;
    }

    /** The longest step any spring or fade is given, however late the frame is: a stalled message thread makes
        the interface catch up in one jump rather than fly apart. */
    constexpr float c_maxStep = 1.0f / 20.0f;

    /**
     * A value that eases to a new one over a time. Set a target with to(), advance it once a frame with step(),
     * and draw `value`. step() says whether it moved, so a component repaints only when it did.
     */
    struct Fade
    {
        float value = 0.0f;

        void snap(float v) noexcept { value = m_from = m_target = v; m_progress = 1.0f; }
        void to(float t, float milliseconds) noexcept
        {
            if (std::abs(t - m_target) < 1.0e-6f)
                return;
            m_from = value;
            m_target = t;
            m_progress = 0.0f;
            m_duration = juce::jmax(1.0f, milliseconds);
        }
        float target() const noexcept { return m_target; }
        bool moving() const noexcept { return m_progress < 1.0f; }

        /** Advances by dt seconds. True while it is still moving. */
        bool step(float dt) noexcept
        {
            if (m_progress >= 1.0f)
                return false;
            m_progress = juce::jmin(1.0f, m_progress + juce::jmin(dt, c_maxStep) * 1000.0f / m_duration);
            value = m_from + (m_target - m_from) * easeOutCubic(m_progress);
            if (m_progress >= 1.0f)
                value = m_target;
            return true;
        }

    private:
        float m_from = 0.0f, m_target = 0.0f, m_progress = 1.0f, m_duration = Ms::toggle;
    };

    /**
     * A damped spring, integrated with the real time of the frame in small enough steps that a late frame cannot
     * make it blow up. Give it one of the two tunings in Springs.
     */
    struct Spring
    {
        float value = 0.0f, velocity = 0.0f, target = 0.0f;

        void snap(float v) noexcept { value = target = v; velocity = 0.0f; }
        void to(float t) noexcept { target = t; }
        /** Knocks the spring: a plug that has just landed, a wheel let go of with the hand still moving. */
        void kick(float v) noexcept { velocity += v; }

        /** Advances by dt seconds on the given tuning. True while it is still moving. */
        bool step(float dt, float hz, float damping) noexcept
        {
            dt = juce::jmin(dt, c_maxStep);
            if (dt <= 0.0f)
                return std::abs(target - value) > 1.0e-4f || std::abs(velocity) > 1.0e-3f;
            // Small enough sub-steps for the stiffest tuning to stay stable, however late the frame was.
            const int steps = juce::jlimit(1, 8, static_cast<int>(std::ceil(dt / (1.0f / 120.0f))));
            const float h = dt / static_cast<float>(steps);
            const float omega = juce::MathConstants<float>::twoPi * hz;
            for (int i = 0; i < steps; ++i)
            {
                velocity += (omega * omega * (target - value) - 2.0f * damping * omega * velocity) * h;
                value += velocity * h;
            }
            if (std::abs(target - value) < 1.0e-4f && std::abs(velocity) < 1.0e-3f)
            {
                value = target;
                velocity = 0.0f;
                return false;
            }
            return true;
        }

        bool stepHelm(float dt) noexcept { return step(dt, Springs::helmHz, Springs::helmDamping); }
        bool stepSwell(float dt) noexcept { return step(dt, Springs::swellHz, Springs::swellDamping); }
    };

    /**
     * The one frame clock. The editor owns it and ticks it from its timer; anything that animates asks to be
     * called rather than starting a timer of its own, so every moving thing in the window moves on the same
     * frame and with the same idea of how long that frame was.
     *
     * Message thread only. A listener may be removed from inside its own frame() (by its destructor, say).
     */
    class FrameClock
    {
    public:
        struct Listener
        {
            virtual ~Listener() = default;
            /** dt is the seconds since the last frame, clamped to c_maxStep. */
            virtual void frame(float dt) = 0;
        };

        void addListener(Listener* l)
        {
            JUCE_ASSERT_MESSAGE_THREAD
            if (l != nullptr && std::find(m_listeners.begin(), m_listeners.end(), l) == m_listeners.end())
                m_listeners.push_back(l);
        }

        void removeListener(Listener* l)
        {
            JUCE_ASSERT_MESSAGE_THREAD
            for (auto& entry : m_listeners)
                if (entry == l)
                    entry = nullptr; // cleared now, dropped after the frame: a listener may go while it is called
            if (! m_ticking)
                compact();
        }

        /** One frame, with the seconds since the last one. */
        void tick(float dt)
        {
            JUCE_ASSERT_MESSAGE_THREAD
            dt = juce::jlimit(0.0f, c_maxStep, dt);
            m_ticking = true;
            for (size_t i = 0; i < m_listeners.size(); ++i)
                if (auto* l = m_listeners[i])
                    l->frame(dt);
            m_ticking = false;
            compact();
        }

        /** One frame, timed from the clock: what the editor's timer calls. */
        void tick()
        {
            const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
            const float dt = m_last > 0.0 ? static_cast<float>(now - m_last) : 1.0f / 60.0f;
            m_last = now;
            tick(dt);
        }

        int listenerCount() const noexcept
        {
            int n = 0;
            for (auto* l : m_listeners)
                n += (l != nullptr ? 1 : 0);
            return n;
        }

    private:
        void compact()
        {
            m_listeners.erase(std::remove(m_listeners.begin(), m_listeners.end(), nullptr), m_listeners.end());
        }

        std::vector<Listener*> m_listeners;
        double m_last = 0.0;
        bool m_ticking = false;
    };

    /** A listener that takes itself off the clock when it goes. */
    class ClockClient : public FrameClock::Listener
    {
    public:
        ~ClockClient() override { listenTo(nullptr); }
        void listenTo(FrameClock* clock)
        {
            if (clock == m_clock)
                return;
            if (m_clock != nullptr)
                m_clock->removeListener(this);
            m_clock = clock;
            if (m_clock != nullptr)
                m_clock->addListener(this);
        }
        FrameClock* clock() const noexcept { return m_clock; }

    private:
        FrameClock* m_clock = nullptr;
    };
}
