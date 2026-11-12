#pragma once

#include <JuceHeader.h>
#include "LogoOutline.h"
#include "PluginProcessor.h"
#include "Theme.h"

#include <array>
#include <deque>

/**
 * The 3D view: the output spectrum rolls away from the camera as a moonlit sea
 * (a perspective waterfall, drawn back to front so near waves hide far ones),
 * and every doppler pass is a cannonball arcing across with a trail and a
 * reflection. When the ship is quiet the logo traces itself in the sky. Click anywhere to fire a whoosh.
 *
 * Drawn with juce::Graphics.
 */
class SeaVisualiser : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    SeaVisualiser(SwooshbucklerAudioProcessor& processor, const Theme::Fonts& fonts);
    ~SeaVisualiser() override;

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;

    std::function<void()> onClick;

private:
    static constexpr int c_fftOrder = 11;
    static constexpr int c_fftSize = 1 << c_fftOrder;
    static constexpr int c_bands = 80;
    static constexpr int c_rows = 34;
    static constexpr int c_trail = 34;

    struct P3 { float x, y, z; };

    void timerCallback() override;
    void analyse();
    juce::Point<float> project(P3 p) const;

    SwooshbucklerAudioProcessor& m_processor;
    const Theme::Fonts& m_fonts;

    juce::dsp::FFT m_fft { c_fftOrder };
    juce::dsp::WindowingFunction<float> m_window { c_fftSize, juce::dsp::WindowingFunction<float>::hann };
    std::array<float, c_fftSize * 2> m_fftData {};
    std::array<float, c_bands> m_smooth {};
    std::array<std::array<float, c_bands>, c_rows> m_rows {};
    int m_rowHead = 0;
    int m_frame = 0;

    std::array<std::deque<P3>, RigEngine::c_maxFlights> m_trails;
    std::array<float, RigEngine::c_maxFlights> m_flightEnv {};
    std::array<float, RigEngine::c_maxFlights> m_flash {};
    std::array<float, RigEngine::c_maxFlights> m_lastX {};


    struct Star { float x, y, size, phase; };
    std::vector<Star> m_stars;

    std::unique_ptr<LogoOutline> m_logo;
    juce::Image m_skyLogo;    // the logo star, re-rendered a few times a second rather than every frame
    int m_skyLogoAge = 0;
    float m_quietSeconds = 0.0f, m_logoTime = 0.0f, m_logoOpacity = 0.0f;
    double m_time = 0.0;

    // Projection, refreshed in paint
    float m_focal = 1.0f, m_horizon = 0.0f, m_cx = 0.0f, m_zNear = 0.85f, m_zStep = 0.12f;
};
