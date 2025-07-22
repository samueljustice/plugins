#include "SimpleWaveformVisualizer.h"
#include "PluginProcessor.h"

SimpleWaveformVisualizer::SimpleWaveformVisualizer(SubbertoneAudioProcessor& p)
    : audioProcessor(p)
{
    startTimerHz(30); // 30 FPS update
}

SimpleWaveformVisualizer::~SimpleWaveformVisualizer()
{
    stopTimer();
}

void SimpleWaveformVisualizer::paint(juce::Graphics& g)
{
    // Fill background
    g.fillAll(bgColor);
    
    // Draw border
    g.setColour(juce::Colours::white);
    g.drawRect(getLocalBounds(), 1);
    
    // Draw labels
    g.setColour(inputColor);
    g.setFont(16.0f);
    g.drawText("INPUT", 10, 10, 100, 20, juce::Justification::left);
    
    g.setColour(outputColor);
    g.drawText("OUTPUT", 10, getHeight() - 30, 100, 20, juce::Justification::left);
    
    // Draw waveforms
    auto drawWaveform = [&](const std::vector<float>& waveform, const juce::Colour& color, float yCenter)
    {
        if (waveform.size() < 2) return;
        
        g.setColour(color);
        juce::Path path;
        
        const float width = static_cast<float>(getWidth());
        const float scale = 100.0f; // Amplitude scale
        
        for (size_t i = 0; i < waveform.size(); ++i)
        {
            float x = (i / static_cast<float>(waveform.size() - 1)) * width;
            float y = yCenter - (waveform[i] * scale);
            
            if (i == 0)
                path.startNewSubPath(x, y);
            else
                path.lineTo(x, y);
        }
        
        g.strokePath(path, juce::PathStrokeType(2.0f));
        
        // Draw center line
        g.setColour(color.withAlpha(0.3f));
        g.drawHorizontalLine(static_cast<int>(yCenter), 0.0f, width);
    };
    
    // Draw input waveform at 1/3 height
    drawWaveform(inputWaveform, inputColor, getHeight() * 0.33f);
    
    // Draw output waveform at 2/3 height
    drawWaveform(outputWaveform, outputColor, getHeight() * 0.66f);
    
    // Draw fundamental frequency
    float fundamental = audioProcessor.getCurrentFundamental();
    if (fundamental > 0.0f)
    {
        g.setColour(juce::Colours::white);
        g.drawText("F0: " + juce::String(fundamental, 1) + " Hz",
                  getWidth() - 150, 10, 140, 20, juce::Justification::right);
        
        g.drawText("SUB: " + juce::String(fundamental * 0.5f, 1) + " Hz",
                  getWidth() - 150, 35, 140, 20, juce::Justification::right);
    }
}

void SimpleWaveformVisualizer::resized()
{
}

void SimpleWaveformVisualizer::timerCallback()
{
    // Get waveform data
    inputWaveform = audioProcessor.getInputWaveform();
    outputWaveform = audioProcessor.getOutputWaveform();
    
    // Downsample for display
    const int targetSize = 512;
    if (inputWaveform.size() > targetSize)
    {
        std::vector<float> downsampled(targetSize);
        float ratio = inputWaveform.size() / static_cast<float>(targetSize);
        for (int i = 0; i < targetSize; ++i)
        {
            int idx = static_cast<int>(i * ratio);
            downsampled[i] = inputWaveform[idx];
        }
        inputWaveform = downsampled;
    }
    
    if (outputWaveform.size() > targetSize)
    {
        std::vector<float> downsampled(targetSize);
        float ratio = outputWaveform.size() / static_cast<float>(targetSize);
        for (int i = 0; i < targetSize; ++i)
        {
            int idx = static_cast<int>(i * ratio);
            downsampled[i] = outputWaveform[idx];
        }
        outputWaveform = downsampled;
    }
    
    repaint();
}