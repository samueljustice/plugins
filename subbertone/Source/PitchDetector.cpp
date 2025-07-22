#include "PitchDetector.h"
#include <algorithm>
#include <numeric>

PitchDetector::PitchDetector()
{
    window = std::make_unique<juce::dsp::WindowingFunction<float>>(
        static_cast<size_t>(windowSize), 
        juce::dsp::WindowingFunction<float>::WindowingMethod::hann);
}

void PitchDetector::prepare(double newSampleRate, int maxBlockSize)
{
    sampleRate = newSampleRate;
    
    // Adjust window size based on sample rate for consistent frequency resolution
    windowSize = static_cast<int>(sampleRate * 0.04); // 40ms window
    windowSize = std::min(windowSize, maxBlockSize * 4); // Limit to reasonable size
    
    // Resize buffers
    windowBuffer.resize(windowSize);
    autocorrelation.resize(windowSize);
    
    // Recreate window function with new size
    window = std::make_unique<juce::dsp::WindowingFunction<float>>(
        static_cast<size_t>(windowSize), 
        juce::dsp::WindowingFunction<float>::WindowingMethod::hann);
    
    // Reset pitch tracking
    previousPitch = 0.0f;
    smoothedPitch = 0.0f;
}

float PitchDetector::detectPitch(const float* inputBuffer, int numSamples, float threshold)
{
    // Check signal level
    float rms = 0.0f;
    for (int i = 0; i < numSamples; ++i)
    {
        rms += inputBuffer[i] * inputBuffer[i];
    }
    rms = std::sqrt(rms / static_cast<float>(numSamples));
    
    // Return smoothed previous pitch if signal is too quiet
    if (rms < threshold * 0.1f)
    {
        smoothedPitch *= 0.95f; // Gradual decay
        if (smoothedPitch < minFrequency)
            smoothedPitch = 0.0f;
        return smoothedPitch;
    }
    
    // Copy input to window buffer (take most recent samples if we have more than windowSize)
    int startIdx = std::max(0, numSamples - windowSize);
    int copySize = std::min(numSamples, windowSize);
    
    // Shift existing samples if needed
    if (copySize < windowSize)
    {
        std::memmove(windowBuffer.data(), windowBuffer.data() + copySize, 
                     (windowSize - copySize) * sizeof(float));
    }
    
    // Copy new samples
    std::memcpy(windowBuffer.data() + (windowSize - copySize), 
                inputBuffer + startIdx, copySize * sizeof(float));
    
    // Detect pitch using autocorrelation
    float detectedPitch = detectPitchAutocorrelation(windowBuffer.data(), windowSize);
    
    // Apply smoothing
    if (detectedPitch > 0.0f)
    {
        if (previousPitch == 0.0f)
        {
            // Initialize immediately if no previous pitch
            smoothedPitch = detectedPitch;
        }
        else
        {
            // Check if pitch jump is reasonable (within 1 octave)
            float ratio = detectedPitch / previousPitch;
            if (ratio > 0.5f && ratio < 2.0f)
            {
                // Normal smoothing
                smoothedPitch = smoothedPitch * pitchSmoothingFactor + 
                               detectedPitch * (1.0f - pitchSmoothingFactor);
            }
            else
            {
                // Large jump - update more quickly
                smoothedPitch = smoothedPitch * 0.5f + detectedPitch * 0.5f;
            }
        }
        previousPitch = detectedPitch;
    }
    else
    {
        // No pitch detected - decay smoothly
        smoothedPitch *= 0.9f;
        if (smoothedPitch < minFrequency)
        {
            smoothedPitch = 0.0f;
            previousPitch = 0.0f;
        }
    }
    
    return smoothedPitch;
}

float PitchDetector::detectPitchAutocorrelation(const float* buffer, int bufferSize)
{
    // Apply window to reduce edge artifacts
    std::vector<float> windowedBuffer(bufferSize);
    std::memcpy(windowedBuffer.data(), buffer, bufferSize * sizeof(float));
    window->multiplyWithWindowingTable(windowedBuffer.data(), static_cast<size_t>(bufferSize));
    
    // Calculate autocorrelation
    calculateAutocorrelation(windowedBuffer.data(), bufferSize);
    
    // Find the peak in the autocorrelation function
    int maxLag = static_cast<int>(sampleRate / minFrequency);
    int minLag = static_cast<int>(sampleRate / maxFrequency);
    
    // Ensure we don't exceed buffer bounds
    maxLag = std::min(maxLag, bufferSize - 1);
    
    int peakLag = findPeakInRange(minLag, maxLag);
    
    if (peakLag <= 0)
        return 0.0f;
    
    // Check if peak is strong enough (using normalized autocorrelation)
    float normalizedPeak = normalizeAutocorrelation(peakLag);
    if (normalizedPeak < 0.3f) // Threshold for reliable detection
        return 0.0f;
    
    // Refine peak position using parabolic interpolation
    float refinedLag = refineWithParabolicInterpolation(peakLag);
    
    // Convert lag to frequency
    float frequency = static_cast<float>(sampleRate) / refinedLag;
    
    // Clamp to valid range
    return juce::jlimit(minFrequency, maxFrequency, frequency);
}

void PitchDetector::calculateAutocorrelation(const float* buffer, int bufferSize)
{
    // Standard unbiased autocorrelation
    for (int lag = 0; lag < bufferSize; ++lag)
    {
        float sum = 0.0f;
        int count = bufferSize - lag;
        
        for (int i = 0; i < count; ++i)
        {
            sum += buffer[i] * buffer[i + lag];
        }
        
        // Normalize by the number of samples used
        autocorrelation[lag] = sum / static_cast<float>(count);
    }
}

int PitchDetector::findPeakInRange(int minLag, int maxLag)
{
    // Skip lag 0 (always maximum)
    float maxValue = -std::numeric_limits<float>::infinity();
    int maxIndex = -1;
    
    // Find first significant peak after the initial decline
    bool foundValley = false;
    float previousValue = autocorrelation[minLag];
    
    for (int lag = minLag + 1; lag < maxLag; ++lag)
    {
        float currentValue = autocorrelation[lag];
        
        // Look for valley first (autocorrelation typically drops after lag 0)
        if (!foundValley && currentValue < previousValue * 0.9f)
        {
            foundValley = true;
        }
        
        // After finding valley, look for peak
        if (foundValley && currentValue > maxValue)
        {
            maxValue = currentValue;
            maxIndex = lag;
        }
        
        previousValue = currentValue;
    }
    
    // Verify this is a true peak (not just noise)
    if (maxIndex > minLag && maxIndex < maxLag - 1)
    {
        // Check if it's a local maximum
        if (autocorrelation[maxIndex] > autocorrelation[maxIndex - 1] &&
            autocorrelation[maxIndex] > autocorrelation[maxIndex + 1])
        {
            return maxIndex;
        }
    }
    
    return -1;
}

float PitchDetector::refineWithParabolicInterpolation(int peakIndex)
{
    // Parabolic interpolation for sub-sample accuracy
    if (peakIndex <= 0 || peakIndex >= static_cast<int>(autocorrelation.size()) - 1)
        return static_cast<float>(peakIndex);
    
    float y1 = autocorrelation[peakIndex - 1];
    float y2 = autocorrelation[peakIndex];
    float y3 = autocorrelation[peakIndex + 1];
    
    float a = (y1 - 2.0f * y2 + y3) * 0.5f;
    float b = (y3 - y1) * 0.5f;
    
    if (std::abs(a) < 1e-10f) // Avoid division by very small number
        return static_cast<float>(peakIndex);
    
    float xOffset = -b / (2.0f * a);
    
    // Limit offset to reasonable range
    xOffset = juce::jlimit(-0.5f, 0.5f, xOffset);
    
    return static_cast<float>(peakIndex) + xOffset;
}

float PitchDetector::normalizeAutocorrelation(int lag)
{
    // Normalize by the zero-lag autocorrelation (signal energy)
    if (autocorrelation[0] <= 0.0f)
        return 0.0f;
    
    return autocorrelation[lag] / autocorrelation[0];
}