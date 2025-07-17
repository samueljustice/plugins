#include "SpectrogramVisualizer.h"

SpectrogramVisualizer::SpectrogramVisualizer()
    : forwardFFT(fftOrder),
      window(fftSize, juce::dsp::WindowingFunction<float>::hann)
{
    // Initialize arrays
    std::fill(fifo, fifo + fftSize, 0.0f);
    std::fill(fftData, fftData + (2 * fftSize), 0.0f);
    fifoIndex = 0;
    nextFFTBlockReady = false;
    detectedFrequency = 0.0f;
    processedFrequency = 0.0f;
    
    // Initialize spectrogram data
    spectrogramData.clear();
    
    // Initialize pitch trails
    detectedPitchTrail.clear();
    processedPitchTrail.clear();
    
    startTimerHz(30);
}

SpectrogramVisualizer::~SpectrogramVisualizer()
{
    stopTimer();
}

void SpectrogramVisualizer::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds();
    
    // Draw background
    g.fillAll(juce::Colours::black);
    
    // Split the display: left side for piano keys, right side for spectrogram
    auto pianoArea = bounds.removeFromLeft(40);
    auto spectrogramArea = bounds;
    
    // Draw piano keyboard reference
    drawPianoKeys(g);
    
    // Draw faint spectrogram background
    g.saveState();
    g.reduceClipRegion(spectrogramArea);
    g.setOrigin(spectrogramArea.getX(), 0);
    g.setOpacity(0.3f);
    drawSpectrogram(g);
    g.setOpacity(1.0f);
    
    // Draw pitch trails - processed first (behind), then detected (front)
    drawPitchTrail(g, processedPitchTrail, juce::Colours::red);
    drawPitchTrail(g, detectedPitchTrail, juce::Colours::yellow);
    g.restoreState();
    
    // Draw legend
    drawLegend(g);
    
    // Draw current frequency indicators
    if (detectedFrequency > minFreq && detectedFrequency < maxFreq)
    {
        float y = frequencyToY(detectedFrequency);
        
        // Draw detected frequency text
        g.setColour(juce::Colours::white);
        g.setFont(10.0f);
        auto freqText = juce::String(detectedFrequency, 1) + " Hz";
        g.drawText(freqText, spectrogramArea.getX() + 5, (int)(y - 10), 60, 20, 
                   juce::Justification::left);
    }
}

void SpectrogramVisualizer::resized()
{
    // Clear and resize spectrogram data if needed
    if (spectrogramData.size() > spectrogramWidth)
    {
        while (spectrogramData.size() > spectrogramWidth)
            spectrogramData.pop_front();
    }
}

void SpectrogramVisualizer::timerCallback()
{
    if (nextFFTBlockReady)
    {
        drawNextFrameOfSpectrum();
        nextFFTBlockReady = false;
    }
    
    updatePitchTrails();
    updateViewRange();
    repaint();
}

void SpectrogramVisualizer::pushSample(float sample)
{
    pushNextSampleIntoFifo(sample);
}

void SpectrogramVisualizer::pushNextSampleIntoFifo(float sample)
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

void SpectrogramVisualizer::drawNextFrameOfSpectrum()
{
    // Apply window function
    window.multiplyWithWindowingTable(fftData, fftSize);
    
    // Perform FFT
    forwardFFT.performFrequencyOnlyForwardTransform(fftData);
    
    // Create frequency bin data for this time slice
    std::vector<float> binData(spectrogramHeight, 0.0f);
    
    auto mindB = -60.0f;
    auto maxdB = 0.0f;
    
    // Process FFT data into frequency bins
    for (int y = 0; y < spectrogramHeight; ++y)
    {
        float freq = yToFrequency(y * getHeight() / spectrogramHeight);
        
        // Find the FFT bin for this frequency
        int bin = (int)(freq * fftSize / sampleRate);
        if (bin >= 0 && bin < frequencyBins)
        {
            float magnitude = fftData[bin];
            if (magnitude > 0.0f)
            {
                float dB = juce::Decibels::gainToDecibels(magnitude);
                float normalizedValue = juce::jmap(dB, mindB, maxdB, 0.0f, 1.0f);
                binData[y] = juce::jlimit(0.0f, 1.0f, normalizedValue);
            }
        }
    }
    
    // Add to spectrogram history
    spectrogramData.push_back(binData);
    
    // Keep only the latest data
    while (spectrogramData.size() > spectrogramWidth)
        spectrogramData.pop_front();
}

void SpectrogramVisualizer::drawPianoKeys(juce::Graphics& g)
{
    auto bounds = juce::Rectangle<int>(0, 0, 40, getHeight());
    
    // Draw white background
    g.setColour(juce::Colours::white);
    g.fillRect(bounds);
    
    // Draw piano keys using view range
    float octaveHeight = getHeight() / (std::log2(viewMaxFreq / viewMinFreq));
    
    for (int note = 0; note < 128; ++note)
    {
        float noteFreq = 440.0f * std::pow(2.0f, (note - 69) / 12.0f);
        if (noteFreq < viewMinFreq || noteFreq > viewMaxFreq)
            continue;
            
        float y = frequencyToY(noteFreq);
        int noteInOctave = note % 12;
        
        // Check if it's a black key
        bool isBlackKey = (noteInOctave == 1 || noteInOctave == 3 || 
                          noteInOctave == 6 || noteInOctave == 8 || noteInOctave == 10);
        
        if (isBlackKey)
        {
            g.setColour(juce::Colours::black);
            g.fillRect((float)bounds.getX(), y - 2, bounds.getWidth() * 0.7f, 4.0f);
        }
        else
        {
            g.setColour(juce::Colours::grey);
            g.drawLine(bounds.getX(), y, bounds.getRight(), y, 0.5f);
        }
        
        // Draw note names for C notes
        if (noteInOctave == 0)
        {
            g.setColour(juce::Colours::black);
            g.setFont(9.0f);
            g.drawText("C" + juce::String(note / 12 - 1), 
                      bounds.getX() + 2, (int)(y - 10), 30, 20, 
                      juce::Justification::left);
        }
    }
    
    // Draw border
    g.setColour(juce::Colours::darkgrey);
    g.drawRect(bounds);
}

void SpectrogramVisualizer::drawSpectrogram(juce::Graphics& g)
{
    if (spectrogramData.empty())
        return;
        
    auto bounds = juce::Rectangle<int>(0, 0, getWidth() - 40, getHeight());
    
    // Draw spectrogram data as vertical lines
    float xScale = bounds.getWidth() / (float)spectrogramWidth;
    
    for (size_t x = 0; x < spectrogramData.size(); ++x)
    {
        const auto& column = spectrogramData[x];
        float xPos = x * xScale;
        
        for (size_t y = 0; y < column.size(); ++y)
        {
            float yPos = y * getHeight() / (float)spectrogramHeight;
            float value = column[y];
            
            if (value > 0.01f) // Only draw if there's significant energy
            {
                g.setColour(getHeatmapColour(value));
                g.fillRect(xPos, yPos, xScale + 1, getHeight() / (float)spectrogramHeight + 1);
            }
        }
    }
    
    // Draw time grid
    g.setColour(juce::Colours::darkgrey.withAlpha(0.3f));
    for (int i = 0; i < 5; ++i)
    {
        float x = bounds.getWidth() * i / 4;
        g.drawLine(x, 0, x, bounds.getHeight(), 0.5f);
    }
}

float SpectrogramVisualizer::frequencyToY(float frequency) const
{
    if (frequency <= 0)
        return getHeight();
        
    // Logarithmic scale using view range
    float logMin = std::log2(viewMinFreq);
    float logMax = std::log2(viewMaxFreq);
    float logFreq = std::log2(frequency);
    
    float normalized = (logFreq - logMin) / (logMax - logMin);
    return getHeight() * (1.0f - normalized);
}

float SpectrogramVisualizer::yToFrequency(float y) const
{
    float normalized = 1.0f - (y / getHeight());
    float logMin = std::log2(viewMinFreq);
    float logMax = std::log2(viewMaxFreq);
    float logFreq = logMin + normalized * (logMax - logMin);
    return std::pow(2.0f, logFreq);
}

int SpectrogramVisualizer::frequencyToMidiNote(float frequency) const
{
    return (int)std::round(69.0f + 12.0f * std::log2(frequency / 440.0f));
}

juce::String SpectrogramVisualizer::getMidiNoteName(int midiNote) const
{
    const char* noteNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    int octave = (midiNote / 12) - 1;
    int noteInOctave = midiNote % 12;
    return juce::String(noteNames[noteInOctave]) + juce::String(octave);
}

juce::Colour SpectrogramVisualizer::getHeatmapColour(float value) const
{
    // Create a heat map color gradient
    value = juce::jlimit(0.0f, 1.0f, value);
    
    if (value < 0.25f)
    {
        // Black to blue
        float t = value * 4.0f;
        return juce::Colour::fromFloatRGBA(0.0f, 0.0f, t, 1.0f);
    }
    else if (value < 0.5f)
    {
        // Blue to cyan
        float t = (value - 0.25f) * 4.0f;
        return juce::Colour::fromFloatRGBA(0.0f, t, 1.0f, 1.0f);
    }
    else if (value < 0.75f)
    {
        // Cyan to yellow
        float t = (value - 0.5f) * 4.0f;
        return juce::Colour::fromFloatRGBA(t, 1.0f, 1.0f - t, 1.0f);
    }
    else
    {
        // Yellow to red
        float t = (value - 0.75f) * 4.0f;
        return juce::Colour::fromFloatRGBA(1.0f, 1.0f - t * 0.5f, 0.0f, 1.0f);
    }
}

void SpectrogramVisualizer::drawPitchTrail(juce::Graphics& g, const std::deque<TrailPoint>& trail, juce::Colour baseColour)
{
    if (trail.size() < 2)
        return;
        
    auto bounds = juce::Rectangle<int>(0, 0, getWidth() - 40, getHeight());
    float xScale = bounds.getWidth() / (float)trailLength;
    
    // Create a path for the main trail
    juce::Path trailPath;
    bool firstPoint = true;
    
    for (size_t i = 0; i < trail.size(); ++i)
    {
        const auto& point = trail[i];
        if (point.frequency <= 0)
            continue;
            
        float x = i * xScale;
        float y = frequencyToY(point.frequency);
        
        if (firstPoint)
        {
            trailPath.startNewSubPath(x, y);
            firstPoint = false;
        }
        else
        {
            trailPath.lineTo(x, y);
        }
    }
    
    // Draw the trail with glow effect
    // First pass - wide glow
    g.setColour(baseColour.withAlpha(0.1f));
    g.strokePath(trailPath, juce::PathStrokeType(8.0f));
    
    // Second pass - medium glow
    g.setColour(baseColour.withAlpha(0.3f));
    g.strokePath(trailPath, juce::PathStrokeType(4.0f));
    
    // Final pass - bright center line
    g.setColour(baseColour.withAlpha(0.8f));
    g.strokePath(trailPath, juce::PathStrokeType(2.0f));
    
    // Draw fading tail points for smoke effect
    for (size_t i = 0; i < trail.size(); ++i)
    {
        const auto& point = trail[i];
        if (point.frequency <= 0)
            continue;
            
        float x = i * xScale;
        float y = frequencyToY(point.frequency);
        float alpha = point.intensity * (1.0f - point.age);
        
        if (alpha > 0.01f)
        {
            g.setColour(baseColour.withAlpha(alpha * 0.5f));
            float radius = 3.0f * (1.0f - point.age);
            g.fillEllipse(x - radius, y - radius, radius * 2, radius * 2);
        }
    }
}

void SpectrogramVisualizer::updatePitchTrails()
{
    // Add new detected pitch point
    if (detectedFrequency > minFreq && detectedFrequency < maxFreq)
    {
        TrailPoint newPoint;
        newPoint.frequency = detectedFrequency;
        newPoint.intensity = 1.0f;
        newPoint.age = 0.0f;
        detectedPitchTrail.push_back(newPoint);
    }
    else
    {
        // Add a gap in the trail
        TrailPoint newPoint;
        newPoint.frequency = 0.0f;
        newPoint.intensity = 0.0f;
        newPoint.age = 1.0f;
        detectedPitchTrail.push_back(newPoint);
    }
    
    // Add new processed pitch point
    if (processedFrequency > minFreq && processedFrequency < maxFreq)
    {
        TrailPoint newPoint;
        newPoint.frequency = processedFrequency;
        newPoint.intensity = 1.0f;
        newPoint.age = 0.0f;
        processedPitchTrail.push_back(newPoint);
    }
    else
    {
        // Add a gap in the trail
        TrailPoint newPoint;
        newPoint.frequency = 0.0f;
        newPoint.intensity = 0.0f;
        newPoint.age = 1.0f;
        processedPitchTrail.push_back(newPoint);
    }
    
    // Update age of all points in both trails
    for (auto& point : detectedPitchTrail)
    {
        point.age += 0.02f; // Age factor
        if (point.age > 1.0f)
            point.age = 1.0f;
    }
    
    for (auto& point : processedPitchTrail)
    {
        point.age += 0.02f; // Age factor
        if (point.age > 1.0f)
            point.age = 1.0f;
    }
    
    // Keep trails at maximum length
    while (detectedPitchTrail.size() > trailLength)
        detectedPitchTrail.pop_front();
        
    while (processedPitchTrail.size() > trailLength)
        processedPitchTrail.pop_front();
}

void SpectrogramVisualizer::drawLegend(juce::Graphics& g)
{
    // Draw legend as a horizontal strip at the bottom left to avoid overlapping with status text
    auto legendBounds = getLocalBounds().removeFromBottom(25).removeFromLeft(350).reduced(10, 2);
    
    // Semi-transparent background
    g.setColour(juce::Colours::black.withAlpha(0.7f));
    g.fillRoundedRectangle(legendBounds.toFloat(), 3.0f);
    
    // Legend text
    g.setColour(juce::Colours::white);
    g.setFont(11.0f);
    
    // "Legend:" label
    auto labelBounds = legendBounds.removeFromLeft(50);
    g.drawText("Legend:", labelBounds, juce::Justification::centred);
    legendBounds.removeFromLeft(5); // spacing
    
    // Detected pitch
    auto detectedBounds = legendBounds.removeFromLeft(140);
    g.setColour(juce::Colours::yellow);
    g.drawLine(detectedBounds.getX(), detectedBounds.getCentreY(),
               detectedBounds.getX() + 25, detectedBounds.getCentreY(), 2.0f);
    g.setColour(juce::Colours::white);
    g.drawText("Detected Pitch", detectedBounds.withLeft(detectedBounds.getX() + 30),
               juce::Justification::left);
    
    // Altered pitch  
    auto processedBounds = legendBounds.removeFromLeft(140);
    g.setColour(juce::Colours::red);
    g.drawLine(processedBounds.getX(), processedBounds.getCentreY(),
               processedBounds.getX() + 25, processedBounds.getCentreY(), 2.0f);
    g.setColour(juce::Colours::white);
    g.drawText("Altered Pitch", processedBounds.withLeft(processedBounds.getX() + 30),
               juce::Justification::left);
}

void SpectrogramVisualizer::updateViewRange()
{
    // Find the highest frequency we need to display (either detected or processed)
    float maxFreqToShow = std::max(detectedFrequency, processedFrequency);
    
    // Add some padding above and below the current frequency
    float octaveRange = 2.0f; // Show about 2 octaves total
    float centerFreq = maxFreqToShow;
    
    if (centerFreq > 0.0f)
    {
        // Calculate target view range centered on the active frequency
        float logCenter = std::log2(centerFreq);
        float logMin = logCenter - octaveRange / 2.0f;
        float logMax = logCenter + octaveRange / 2.0f;
        
        targetViewMin = std::pow(2.0f, logMin);
        targetViewMax = std::pow(2.0f, logMax);
        
        // Clamp to absolute limits
        targetViewMin = std::max(targetViewMin, minFreq);
        targetViewMax = std::min(targetViewMax, maxFreq);
        
        // Make sure we have a minimum range
        if (targetViewMax - targetViewMin < 500.0f)
        {
            float midPoint = (targetViewMax + targetViewMin) / 2.0f;
            targetViewMin = midPoint - 250.0f;
            targetViewMax = midPoint + 250.0f;
        }
    }
    
    // Smooth scrolling - interpolate towards target
    float scrollSpeed = 0.1f;
    viewMinFreq += (targetViewMin - viewMinFreq) * scrollSpeed;
    viewMaxFreq += (targetViewMax - viewMaxFreq) * scrollSpeed;
}