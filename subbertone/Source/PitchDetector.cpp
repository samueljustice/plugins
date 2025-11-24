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
    yin_buffer.resize(static_cast<size_t>(windowSize));
    
    // Recreate window function with new size
    window = std::make_unique<juce::dsp::WindowingFunction<float>>(
                                                                   static_cast<size_t>(windowSize),
                                                                   juce::dsp::WindowingFunction<float>::WindowingMethod::hann);
    
    // Setup pre-filter
    auto spec = juce::dsp::ProcessSpec{newSampleRate, static_cast<juce::uint32>(maxBlockSize), 1};
    preFilter.prepare(spec);
    auto coeffs = juce::dsp::IIR::Coefficients<float>::makeHighPass(newSampleRate, 30.0f, 0.707f);
    preFilter.coefficients = coeffs;
    
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
    
    // Apply prefiltering
    preFilter.reset();
    for (int i = 0; i < windowSize; ++i)
    {
        windowBuffer[i] = preFilter.processSample(windowBuffer[i]);
    }
    
    // Detect pitch - Autocorrelation or YIN
    float detectedPitch = (currentMethod == DetectionMethod::YIN) ?
    detectPitchYIN(windowBuffer.data(), windowSize) :
    detectPitchAutocorrelation(windowBuffer.data(), windowSize);
    
    // Apply (Adaptive) Smoothing
    if (detectedPitch > 0.0f)
    {
        if (previousPitch == 0.0f)
        {
            smoothedPitch = detectedPitch;
        }
        else
        {
            float ratio = detectedPitch / previousPitch;
            if (ratio > 0.5f && ratio < 2.0f)
            {
                float adaptiveSmoothingFactor;
                if (detectedPitch < 100.0f)
                {
                    adaptiveSmoothingFactor = 0.8f;
                }
                else
                {
                    adaptiveSmoothingFactor = 0.85f;
                }
                
                smoothedPitch = smoothedPitch * adaptiveSmoothingFactor +
                detectedPitch * (1.0f - adaptiveSmoothingFactor);
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
    std::vector<float> windowedBuffer(bufferSize);
    std::memcpy(windowedBuffer.data(), buffer, bufferSize * sizeof(float));
    window->multiplyWithWindowingTable(windowedBuffer.data(), static_cast<size_t>(bufferSize));
    
    calculateAutocorrelation(windowedBuffer.data(), bufferSize);
    
    int maxLag = static_cast<int>(sampleRate / minFrequency);
    int minLag = static_cast<int>(sampleRate / maxFrequency);
    maxLag = std::min(maxLag, bufferSize - 1);
    
    int peakLag = findPeakInRange(minLag, maxLag);
    
    if (peakLag <= 0)
        return 0.0f;
    
    // Dynamic threshold based on signal characteristics
    float normalizedPeak = normalizeAutocorrelation(peakLag);
    
    // Calculate signal's harmonic richness by looking at autocorr variance
    float variance = 0.0f;
    float mean = 0.0f;
    int sampleCount = maxLag - minLag;
    
    for (int i = minLag; i < maxLag; ++i)
    {
        mean += autocorrelation[i];
    }
    mean /= sampleCount;
    
    for (int i = minLag; i < maxLag; ++i)
    {
        float diff = autocorrelation[i] - mean;
        variance += diff * diff;
    }
    variance /= sampleCount;
    
    // Higher variance = more harmonic content = lower threshold needed
    float adaptiveThreshold = juce::jmap(variance, 0.0f, 0.1f, 0.45f, 0.25f);
    adaptiveThreshold = juce::jlimit(0.2f, 0.5f, adaptiveThreshold);
    
    if (normalizedPeak < adaptiveThreshold)
        return 0.0f;
    
    float refinedLag = refineWithParabolicInterpolation(peakLag);
    float frequency = static_cast<float>(sampleRate) / refinedLag;
    
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
    float maxValue = -std::numeric_limits<float>::infinity();
    int maxIndex = -1;
    
    bool foundValley = false;
    float previousValue = autocorrelation[minLag];
    
    for (int lag = minLag + 1; lag < maxLag; ++lag)
    {
        float currentValue = autocorrelation[lag];
        
        // Mark valley when value drops significantly
        if (!foundValley && currentValue < previousValue * 0.85f)
        {
            foundValley = true;
        }
        
        // After valley, find highest peak
        if (foundValley && currentValue > maxValue)
        {
            maxValue = currentValue;
            maxIndex = lag;
        }
        
        previousValue = currentValue;
    }
    
    // Validate peak is local maximum and significant
    if (maxIndex > minLag && maxIndex < maxLag - 1)
    {
        bool isLocalMax = autocorrelation[maxIndex] > autocorrelation[maxIndex - 1] &&
        autocorrelation[maxIndex] > autocorrelation[maxIndex + 1];
        
        // Must be significantly above zero lag normalized value
        float normalizedPeak = autocorrelation[maxIndex] / autocorrelation[0];
        bool isSignificant = normalizedPeak > 0.4f;
        
        if (isLocalMax && isSignificant)
        {
            return maxIndex;
        }
    }
    
    return -1;
}

float PitchDetector::normalizeAutocorrelation(int lag)
{
    // Normalize by the zero-lag autocorrelation (signal energy)
    if (autocorrelation[0] <= 0.0f)
        return 0.0f;
    
    return autocorrelation[lag] / autocorrelation[0];
}

float PitchDetector::refineWithParabolicInterpolation(int peakIndex)
{
    if (currentMethod == DetectionMethod::YIN)
    {
        // Use YIN buffer for interpolation
        if (peakIndex <= 0 || peakIndex >= static_cast<int>(yin_buffer.size()) - 1)
            return static_cast<float>(peakIndex);
        
        float y1 = yin_buffer[peakIndex - 1];
        float y2 = yin_buffer[peakIndex];
        float y3 = yin_buffer[peakIndex + 1];
        
        float a = (y1 - 2.0f * y2 + y3) * 0.5f;
        float b = (y3 - y1) * 0.5f;
        
        if (std::abs(a) < 1e-10f)
            return static_cast<float>(peakIndex);
        
        float xOffset = -b / (2.0f * a);
        xOffset = juce::jlimit(-0.5f, 0.5f, xOffset);
        
        return static_cast<float>(peakIndex) + xOffset;
    }
    else
    {
        // Use autocorrelation buffer for interpolation
        if (peakIndex <= 0 || peakIndex >= static_cast<int>(autocorrelation.size()) - 1)
            return static_cast<float>(peakIndex);
        
        float y1 = autocorrelation[peakIndex - 1];
        float y2 = autocorrelation[peakIndex];
        float y3 = autocorrelation[peakIndex + 1];
        
        float a = (y1 - 2.0f * y2 + y3) * 0.5f;
        float b = (y3 - y1) * 0.5f;
        
        if (std::abs(a) < 1e-10f)
            return static_cast<float>(peakIndex);
        
        float xOffset = -b / (2.0f * a);
        xOffset = juce::jlimit(-0.5f, 0.5f, xOffset);
        
        return static_cast<float>(peakIndex) + xOffset;
    }
}

// YIN - Pitch Detection Method
float PitchDetector::detectPitchYIN(const float* buffer, int bufferSize)
{
    std::vector<float> windowedBuffer(bufferSize);
    std::memcpy(windowedBuffer.data(), buffer, bufferSize * sizeof(float));
    window->multiplyWithWindowingTable(windowedBuffer.data(), static_cast<size_t>(bufferSize));
    
    calculateDifferenceFunction(windowedBuffer.data(), bufferSize);
    calculateCumulativeMeanNormalizedDifference();
    
    int minLag = static_cast<int>(sampleRate / maxFrequency);
    int maxLag = static_cast<int>(sampleRate / minFrequency);
    maxLag = std::min(maxLag, bufferSize / 2);
    
    int bestLag = findAbsoluteMinimum(minLag, maxLag);
    
    if (bestLag <= 0)
        return 0.0f;
    
    float refinedLag = refineWithParabolicInterpolation(bestLag);
    if (refinedLag <= 0.0f) return 0.0f;
    
    float frequency = static_cast<float>(sampleRate) / refinedLag;
    
    return juce::jlimit(minFrequency, maxFrequency, frequency);
}

void PitchDetector::calculateDifferenceFunction(const float* buffer, int bufferSize)
{
    int halfSize = bufferSize / 2;
    
    for (int lag = 0; lag < halfSize; ++lag)
    {
        float sum = 0.0f;
        for (int i = 0; i < halfSize; ++i)
        {
            float diff = buffer[i] - buffer[i + lag];
            sum += diff * diff;
        }
        yin_buffer[lag] = sum;
    }
}

void PitchDetector::calculateCumulativeMeanNormalizedDifference()
{
    yin_buffer[0] = 1.0f;
    
    float runningSum = 0.0f;
    for (int lag = 1; lag < static_cast<int>(yin_buffer.size()) / 2; ++lag)
    {
        runningSum += yin_buffer[lag];
        if (runningSum > 0.0f)
        {
            float mean = runningSum / static_cast<float>(lag);
            yin_buffer[lag] = yin_buffer[lag] / mean;
        }
        else
        {
            yin_buffer[lag] = 1.0f;
        }
    }
}

int PitchDetector::findAbsoluteMinimum(int minLag, int maxLag)
{
    return findBestLocalMinimum(minLag, maxLag);
}

int PitchDetector::findBestLocalMinimum(int minLag, int maxLag)
{
    for (int lag = minLag + 1; lag < maxLag - 1 && lag < static_cast<int>(yin_buffer.size()) - 1; ++lag)
    {
        if (yin_buffer[lag] < yin_buffer[lag - 1] &&
            yin_buffer[lag] < yin_buffer[lag + 1])
        {
            if (yin_buffer[lag] < yinThreshold)
            {
                return lag;
            }
        }
    }
    
    return -1;
}
