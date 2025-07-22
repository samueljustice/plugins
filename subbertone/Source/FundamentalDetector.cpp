#include "FundamentalDetector.h"
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

FundamentalDetector::FundamentalDetector()
{
}

FundamentalDetector::~FundamentalDetector()
{
}

void FundamentalDetector::prepare(double newSampleRate, int maxBlockSize)
{
    sampleRate = newSampleRate;
    frameLength = static_cast<int>(sampleRate * 0.05); // 50ms buffer
    
    processBuffer.resize(frameLength);
    accumulator.clear();
    accumulator.reserve(frameLength * 2);
    
    // Yin buffer needs to be half the size of the input buffer
    yinBuffer.resize(frameLength / 2);
    
    // Reset tracking variables
    framesSinceLastUpdate = 0;
    lastFundamental = 0.0f;
    stableFundamental = 0.0f;
}

float FundamentalDetector::detectFundamental(const float* inputBuffer, int numSamples, float thresholdDb)
{
    // Check if we're getting any signal
    float maxSample = 0.0f;
    for (int i = 0; i < numSamples; ++i)
    {
        maxSample = std::max(maxSample, std::abs(inputBuffer[i]));
    }
    
    // Convert dB threshold to linear
    float linearThreshold = std::pow(10.0f, thresholdDb / 20.0f);
    
    // Debug logging
    static int debugCount = 0;
    if (debugCount < 20)
    {
        float maxSampleDb = maxSample > 0 ? 20.0f * std::log10(maxSample) : -100.0f;
        DBG("FundamentalDetector - Max sample: " << maxSample << " (" << maxSampleDb << " dB)");
        DBG("  Threshold: " << linearThreshold << " (" << thresholdDb << " dB)");
        DBG("  Signal above threshold: " << (maxSample >= linearThreshold ? "YES" : "NO"));
        debugCount++;
    }
    
    // If signal is too quiet, return 0
    if (maxSample < linearThreshold)
    {
        lastFundamental *= 0.95f;
        if (lastFundamental < 10.0f) lastFundamental = 0.0f;
        return lastFundamental;
    }
    
    // Add new samples to accumulator
    for (int i = 0; i < numSamples; ++i)
    {
        accumulator.push_back(static_cast<double>(inputBuffer[i]));
    }
    
    // Increment frame counter
    framesSinceLastUpdate++;
    
    // Process when we have enough samples
    if (accumulator.size() >= frameLength)
    {
        // Only update fundamental detection periodically to avoid constant changes
        if (framesSinceLastUpdate >= updateInterval)
        {
            framesSinceLastUpdate = 0;
            
            // Copy to processing buffer
            for (int i = 0; i < frameLength; ++i)
            {
                processBuffer[i] = static_cast<float>(accumulator[accumulator.size() - frameLength + i]);
            }
            
            // Run Yin pitch detection
            float detectedFundamental = detectPitchYin(processBuffer.data(), frameLength);
            
            // Apply smoothing
            if (detectedFundamental > 0.0f)
            {
                if (lastFundamental == 0.0f)
                {
                    // Initialize immediately if no previous value
                    lastFundamental = detectedFundamental;
                    stableFundamental = detectedFundamental;
                }
                else
                {
                    // Only update if the new frequency is reasonably close to the old one
                    // This helps filter out octave errors
                    float ratio = detectedFundamental / lastFundamental;
                    if (ratio > 0.8f && ratio < 1.25f)
                    {
                        lastFundamental = lastFundamental * smoothingFactor + 
                                         detectedFundamental * (1.0f - smoothingFactor);
                    }
                    else
                    {
                        // Large jump - might be octave error or new note
                        // Apply more conservative smoothing to prevent crackling
                        lastFundamental = lastFundamental * 0.95f + detectedFundamental * 0.05f;
                    }
                    
                    // Apply additional smoothing to the stable output
                    // Only update stable fundamental if change is significant (> 0.5 Hz)
                    float change = std::abs(lastFundamental - stableFundamental);
                    if (change > 0.5f)
                    {
                        // Smooth transition to new fundamental
                        stableFundamental = stableFundamental * 0.9f + lastFundamental * 0.1f;
                    }
                }
            }
            else if (lastFundamental > 0.0f)
            {
                // Decay slowly when no fundamental detected
                lastFundamental *= 0.98f;
                if (lastFundamental < 10.0f) 
                {
                    lastFundamental = 0.0f;
                    stableFundamental = 0.0f;
                }
                else
                {
                    stableFundamental = stableFundamental * 0.98f + lastFundamental * 0.02f;
                }
            }
        }
        
        // Remove old samples, keep some for overlap
        if (accumulator.size() > frameLength * 1.5)
        {
            accumulator.erase(accumulator.begin(), accumulator.begin() + numSamples);
        }
    }
    
    return stableFundamental;
}

float FundamentalDetector::detectPitchYin(const float* buffer, int bufferSize)
{
    int yinBufferSize = bufferSize / 2;
    
    // Step 1: Calculate the difference function
    differenceFunction(buffer, bufferSize, yinBuffer.data());
    
    // Step 2: Calculate the cumulative mean normalized difference function
    cumulativeMeanNormalizedDifferenceFunction(yinBuffer.data(), yinBufferSize);
    
    // Step 3: Find the first minimum below the threshold
    int tau = absoluteThreshold(yinBuffer.data(), yinBufferSize, yinThreshold);
    
    if (tau == -1)
    {
        // No pitch found
        return 0.0f;
    }
    
    // Step 4: Parabolic interpolation for more accurate pitch
    float betterTau = parabolicInterpolation(tau, yinBuffer.data(), yinBufferSize);
    
    // Convert period to frequency
    float frequency = static_cast<float>(sampleRate) / betterTau;
    
    // Sanity check the frequency
    if (frequency < minFrequency || frequency > maxFrequency)
    {
        return 0.0f;
    }
    
    return frequency;
}

void FundamentalDetector::differenceFunction(const float* buffer, int bufferSize, float* yinBuffer)
{
    int halfBufferSize = bufferSize / 2;
    
    // Calculate the difference function
    for (int tau = 0; tau < halfBufferSize; ++tau)
    {
        float sum = 0.0f;
        for (int i = 0; i < halfBufferSize; ++i)
        {
            float delta = buffer[i] - buffer[i + tau];
            sum += delta * delta;
        }
        yinBuffer[tau] = sum;
    }
}

void FundamentalDetector::cumulativeMeanNormalizedDifferenceFunction(float* yinBuffer, int yinBufferSize)
{
    yinBuffer[0] = 1.0f;
    
    float runningSum = 0.0f;
    for (int tau = 1; tau < yinBufferSize; ++tau)
    {
        runningSum += yinBuffer[tau];
        if (runningSum == 0.0f)
        {
            yinBuffer[tau] = 1.0f;
        }
        else
        {
            yinBuffer[tau] *= tau / runningSum;
        }
    }
}

int FundamentalDetector::absoluteThreshold(float* yinBuffer, int yinBufferSize, float threshold)
{
    int tau = 2;  // Start at 2 to avoid very high frequencies
    
    while (tau < yinBufferSize)
    {
        if (yinBuffer[tau] < threshold)
        {
            // We found a dip below the threshold
            // Make sure it's a local minimum
            while (tau + 1 < yinBufferSize && yinBuffer[tau + 1] < yinBuffer[tau])
            {
                tau++;
            }
            return tau;
        }
        tau++;
    }
    
    // No pitch found
    return -1;
}

float FundamentalDetector::parabolicInterpolation(int tauEstimate, float* yinBuffer, int yinBufferSize)
{
    if (tauEstimate == 0 || tauEstimate == yinBufferSize - 1)
    {
        return static_cast<float>(tauEstimate);
    }
    
    float s0 = yinBuffer[tauEstimate - 1];
    float s1 = yinBuffer[tauEstimate];
    float s2 = yinBuffer[tauEstimate + 1];
    
    float betterTau = tauEstimate + (s2 - s0) / (2.0f * (2.0f * s1 - s2 - s0));
    
    return betterTau;
}