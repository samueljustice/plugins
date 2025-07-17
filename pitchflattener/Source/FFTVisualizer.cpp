#include "FFTVisualizer.h"

FFTVisualizer::FFTVisualizer()
    : forwardFFT(fftOrder),
      window(fftSize, juce::dsp::WindowingFunction<float>::hann)
{
    // Initialize all arrays to zero
    std::fill(fifo, fifo + fftSize, 0.0f);
    std::fill(fftData, fftData + (2 * fftSize), 0.0f);
    std::fill(scopeData, scopeData + scopeSize, 0.0f);
    fifoIndex = 0;
    nextFFTBlockReady = false;
    strongestFrequency = 0.0f;
    startTimerHz(30);
}

FFTVisualizer::~FFTVisualizer()
{
    stopTimer();
}

void FFTVisualizer::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds();
    
    // Only draw the spectrum, no background (let parent handle background)
    drawFrame(g);
    
    // Draw strongest frequency indicator if available
    if (strongestFrequency > 20.0f && strongestFrequency < 20000.0f)
    {
        // Map frequency to x position (logarithmic scale)
        float minFreq = 20.0f;
        float maxFreq = 20000.0f;
        float proportion = (std::log(strongestFrequency) - std::log(minFreq)) / (std::log(maxFreq) - std::log(minFreq));
        float xPos = bounds.getX() + proportion * bounds.getWidth();
        
        // Draw vertical line at strongest frequency
        g.setColour(juce::Colours::yellow.withAlpha(0.5f));
        g.drawLine(xPos, bounds.getY() + bounds.getHeight() * 0.3f, 
                   xPos, bounds.getBottom() - 20, 1.5f);
    }
}

void FFTVisualizer::timerCallback()
{
    if (nextFFTBlockReady)
    {
        drawNextFrameOfSpectrum();
        nextFFTBlockReady = false;
        repaint();
    }
}

void FFTVisualizer::pushSample(float sample)
{
    pushNextSampleIntoFifo(sample);
}

void FFTVisualizer::pushNextSampleIntoFifo(float sample)
{
    if (fifoIndex >= fftSize)
    {
        if (!nextFFTBlockReady)
        {
            std::copy(fifo, fifo + fftSize, fftData);
            nextFFTBlockReady = true;
        }
        fifoIndex = 0;
    }
    
    if (fifoIndex < fftSize)
        fifo[fifoIndex++] = sample;
}

void FFTVisualizer::drawNextFrameOfSpectrum()
{
    // Apply window function
    window.multiplyWithWindowingTable(fftData, fftSize);
    
    // Perform FFT
    forwardFFT.performFrequencyOnlyForwardTransform(fftData);
    
    auto mindB = -100.0f;
    auto maxdB = 0.0f;
    
    for (int i = 0; i < scopeSize; ++i)
    {
        auto skewedProportionX = 1.0f - std::exp(std::log(1.0f - (float)i / (float)scopeSize) * 0.2f);
        auto fftDataIndex = juce::jlimit(0, fftSize / 2 - 1, (int)(skewedProportionX * (float)fftSize * 0.5f));
        
        if (fftDataIndex >= 0 && fftDataIndex < fftSize)
        {
            auto magnitude = fftData[fftDataIndex];
            if (magnitude > 0.0f)
            {
                auto level = juce::jmap(juce::jlimit(mindB, maxdB, 
                                                     juce::Decibels::gainToDecibels(magnitude)
                                                     - juce::Decibels::gainToDecibels((float)fftSize)),
                                        mindB, maxdB, 0.0f, 1.0f);
                scopeData[i] = level;
            }
            else
            {
                scopeData[i] = 0.0f;
            }
        }
        else
        {
            scopeData[i] = 0.0f;
        }
    }
}

void FFTVisualizer::drawFrame(juce::Graphics& g)
{
    auto bounds = getLocalBounds();
    auto width = bounds.getWidth();
    auto height = bounds.getHeight();
    
    // Reduce height to not interfere with pitch meter elements
    auto spectrumBounds = bounds.withHeight(bounds.getHeight() - 25);
    
    juce::Path p;
    p.startNewSubPath(spectrumBounds.getX(), spectrumBounds.getBottom());
    
    for (int i = 0; i < scopeSize; ++i)
    {
        auto x = juce::jmap((float)i, 0.0f, (float)scopeSize - 1, 
                           (float)spectrumBounds.getX(), (float)spectrumBounds.getRight());
        auto y = juce::jmap(scopeData[i], 0.0f, 1.0f, 
                           (float)spectrumBounds.getBottom(), 
                           (float)spectrumBounds.getY() + spectrumBounds.getHeight() * 0.5f);
        
        if (i == 0)
            p.startNewSubPath(x, y);
        else
            p.lineTo(x, y);
    }
    
    p.lineTo(spectrumBounds.getRight(), spectrumBounds.getBottom());
    p.closeSubPath();
    
    // Gradient fill - more subtle
    juce::ColourGradient gradient(juce::Colours::cyan.withAlpha(0.2f), 0, spectrumBounds.getY(),
                                  juce::Colours::darkblue.withAlpha(0.1f), 0, spectrumBounds.getBottom(), false);
    g.setGradientFill(gradient);
    g.fillPath(p);
    
    // Draw outline - very subtle
    g.setColour(juce::Colours::cyan.withAlpha(0.3f));
    g.strokePath(p, juce::PathStrokeType(0.5f));
}

float FFTVisualizer::skewedProportionToX(float proportion, float skewFactor) const
{
    return 1.0f - std::exp(std::log(1.0f - proportion) * skewFactor);
}