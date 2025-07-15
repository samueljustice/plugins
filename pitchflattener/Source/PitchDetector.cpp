#include "PitchDetector.h"
#include <algorithm>
#include <numeric>

// Include WORLD headers
extern "C" {
#include "world/dio.h"
}

PitchDetector::PitchDetector()
{
    worldOption = new DioOption();
    InitializeDioOption(static_cast<DioOption*>(worldOption));
}

PitchDetector::~PitchDetector()
{
    if (worldOption)
    {
        delete static_cast<DioOption*>(worldOption);
        worldOption = nullptr;
    }
}

void PitchDetector::prepare(double newSampleRate)
{
    sampleRate = newSampleRate;
    
    // Update period bounds based on actual sample rate and frequency bounds
    maxPeriod = static_cast<int>(sampleRate / minFrequency);
    minPeriod = static_cast<int>(sampleRate / maxFrequency);
    
    yinBuffer.resize(maxPeriod);
    
    // Prepare WORLD buffers
    if (worldOption)
    {
        DioOption* opt = static_cast<DioOption*>(worldOption);
        opt->f0_floor = minFrequency;
        opt->f0_ceil = maxFrequency;
        opt->frame_period = 2.0; // Default 2ms, will be updated from parameters
        opt->speed = 1; // Default speed, will be updated from parameters
        opt->allowed_range = 0.1; // Default threshold
        opt->channels_in_octave = 2.0; // Default frequency resolution
        
        // Calculate how many samples we need for WORLD
        // Use larger buffer size to avoid reallocation
        // Limit to 1.5 seconds to prevent memory issues
        int maxBufferSize = static_cast<int>(sampleRate * 1.5); // 1.5 seconds max
        worldSamplesPerFrame = GetSamplesForDIO(static_cast<int>(sampleRate), 
                                                maxBufferSize, 
                                                opt->frame_period);
        // Pre-allocate with extra space to avoid reallocation
        // Limit size to prevent excessive memory usage
        int maxFrames = std::min(worldSamplesPerFrame * 2, static_cast<int>(sampleRate / 10)); // Max 100ms worth of frames
        worldF0.resize(maxFrames);
        worldTimeAxis.resize(maxFrames);
        worldBuffer.resize(maxBufferSize);
        
        // Initialize rolling buffer for DIO based on user-configurable buffer time
        dioBufferSize = static_cast<int>(sampleRate * dioBufferTimeSeconds);  
        dioRollingBuffer.resize(dioBufferSize);
        std::fill(dioRollingBuffer.begin(), dioRollingBuffer.end(), 0.0);
        dioBufferWritePos = 0;
        dioSamplesAccumulated = 0;
        // Process when buffer is full (for initial analysis) or every 100ms for updates
        dioProcessingInterval = static_cast<int>(sampleRate * 0.1); // 100ms for updates
    }
}

void PitchDetector::setFrequencyBounds(float minFreq, float maxFreq)
{
    minFrequency = std::max(20.0f, minFreq);
    maxFrequency = std::min(4000.0f, maxFreq);
    
    // Update period bounds
    if (sampleRate > 0)
    {
        maxPeriod = static_cast<int>(sampleRate / minFrequency);
        minPeriod = static_cast<int>(sampleRate / maxFrequency);
        yinBuffer.resize(maxPeriod);
        
        // Update WORLD parameters
        if (worldOption)
        {
            DioOption* opt = static_cast<DioOption*>(worldOption);
            opt->f0_floor = minFrequency;
            opt->f0_ceil = maxFrequency;
        }
    }
}

float PitchDetector::detectPitch(const float* buffer, int numSamples)
{
    if (algorithm == Algorithm::WORLD_DIO)
    {
        return detectPitchWORLD(buffer, numSamples);
    }
    
    // Default YIN algorithm
    if (numSamples < maxPeriod * 2)
        return 0.0f;
    
    // Step 1: Calculate the difference function
    differenceFunction(buffer, numSamples, yinBuffer.data(), maxPeriod);
    
    // Step 2: Calculate the cumulative mean normalized difference function
    cumulativeMeanNormalizedDifferenceFunction(yinBuffer.data(), maxPeriod);
    
    // Step 3: Find the first minimum below the threshold
    int tau = absoluteThreshold(yinBuffer.data(), maxPeriod, yinThreshold);
    
    if (tau == -1)
        return 0.0f; // No pitch found
    
    // Step 4: Parabolic interpolation for better precision
    float betterTau = parabolicInterpolation(tau, yinBuffer.data(), maxPeriod);
    
    // Convert period to frequency
    float pitch = static_cast<float>(sampleRate / betterTau);
    
    // Sanity check
    if (pitch < 40.0f || pitch > 2000.0f)
        return 0.0f;
    
    return pitch;
}

void PitchDetector::differenceFunction(const float* buffer, int numSamples, float* result, int maxLag)
{
    // Calculate autocorrelation using the difference function
    for (int tau = 0; tau < maxLag; ++tau)
    {
        float sum = 0.0f;
        for (int i = 0; i < maxLag; ++i)
        {
            float delta = buffer[i] - buffer[i + tau];
            sum += delta * delta;
        }
        result[tau] = sum;
    }
}

void PitchDetector::cumulativeMeanNormalizedDifferenceFunction(float* df, int size)
{
    df[0] = 1.0f;
    float runningSum = 0.0f;
    
    for (int tau = 1; tau < size; ++tau)
    {
        runningSum += df[tau];
        df[tau] = df[tau] * tau / runningSum;
    }
}

int PitchDetector::absoluteThreshold(const float* yinBuffer, int size, float threshold)
{
    // Start from minPeriod to avoid high frequency noise
    for (int tau = minPeriod; tau < size - 1; ++tau)
    {
        if (yinBuffer[tau] < threshold)
        {
            // Check if this is a local minimum
            while (tau + 1 < size && yinBuffer[tau + 1] < yinBuffer[tau])
            {
                tau++;
            }
            return tau;
        }
    }
    
    // No pitch found below threshold, find the minimum value
    int minTau = minPeriod;
    float minValue = yinBuffer[minPeriod];
    
    for (int tau = minPeriod + 1; tau < size; ++tau)
    {
        if (yinBuffer[tau] < minValue)
        {
            minValue = yinBuffer[tau];
            minTau = tau;
        }
    }
    
    return minValue < 0.5f ? minTau : -1;
}

float PitchDetector::parabolicInterpolation(int tauEstimate, const float* yinBuffer, int yinBufferSize)
{
    if (tauEstimate < 1 || tauEstimate >= yinBufferSize - 1)
        return static_cast<float>(tauEstimate);
    
    float s0 = yinBuffer[tauEstimate - 1];
    float s1 = yinBuffer[tauEstimate];
    float s2 = yinBuffer[tauEstimate + 1];
    
    float a = (s0 - 2.0f * s1 + s2) / 2.0f;
    float b = (s2 - s0) / 2.0f;
    
    if (a == 0.0f)
        return static_cast<float>(tauEstimate);
    
    float xOffset = -b / (2.0f * a);
    
    return static_cast<float>(tauEstimate) + xOffset;
}

float PitchDetector::detectPitchWORLD(const float* buffer, int numSamples)
{
    static float lastValidPitch = 0.0f;
    
    if (!worldOption || dioBufferSize == 0)
    {
        DBG("DIO: No world option or buffer size is 0");
        return lastValidPitch;
    }
    
    std::lock_guard<std::mutex> lock(dioBufferMutex);
    
    // Add new samples to rolling buffer
    for (int i = 0; i < numSamples; ++i)
    {
        if (dioBufferWritePos >= 0 && dioBufferWritePos < static_cast<int>(dioRollingBuffer.size()))
        {
            dioRollingBuffer[dioBufferWritePos] = static_cast<double>(buffer[i]);
            dioBufferWritePos = (dioBufferWritePos + 1) % dioBufferSize;
        }
    }
    
    dioTotalSamplesReceived += numSamples;
    dioSamplesAccumulated += numSamples;
    
    // Check if we've filled the buffer for the first time
    if (!dioBufferFilled && dioTotalSamplesReceived >= dioBufferSize)
    {
        dioBufferFilled = true;
        DBG("DIO: Buffer filled! Starting pitch detection after " << dioBufferTimeSeconds << " seconds");
    }
    
    // Don't process until buffer is filled (like Z-Noise prebuffer)
    if (!dioBufferFilled)
    {
        return 0.0f; // Return 0 during prebuffer phase
    }
    
    // Process continuously after buffer is filled
    // No need to wait for intervals - process on every call for responsiveness
    
    static int dioDebugCounter = 0;
    if (++dioDebugCounter % 5 == 0)
    {
        DBG("DIO: Processing - Total samples: " << dioTotalSamplesReceived << ", buffer size: " << dioBufferSize);
    }
    
    // Copy rolling buffer to linear buffer for DIO
    // Start from the oldest sample (write position is the next position to write, so oldest is at writePos)
    int readPos = dioBufferWritePos;
    int buffersToCopy = std::min(dioBufferSize, static_cast<int>(worldBuffer.size()));
    buffersToCopy = std::min(buffersToCopy, static_cast<int>(dioRollingBuffer.size()));
    
    for (int i = 0; i < buffersToCopy; ++i)
    {
        if (i < static_cast<int>(worldBuffer.size()) && readPos < static_cast<int>(dioRollingBuffer.size()))
        {
            worldBuffer[i] = dioRollingBuffer[readPos];
        }
        readPos = (readPos + 1) % dioBufferSize;
    }
    
    DioOption* opt = static_cast<DioOption*>(worldOption);
    
    // Process the entire rolling buffer
    int samplesToProcess = std::min(dioBufferSize, static_cast<int>(worldBuffer.size()));
    
    // Sanity check for large buffer sizes
    // Limit to 1.5 seconds to prevent crashes
    const int maxSafeSamples = static_cast<int>(sampleRate * 1.5);
    if (samplesToProcess <= 0 || samplesToProcess > maxSafeSamples)
    {
        DBG("DIO: Invalid samples to process: " << samplesToProcess << " (max: " << maxSafeSamples << ")");
        return lastValidPitch;
    }
    
    int frameCount = GetSamplesForDIO(static_cast<int>(sampleRate), 
                                      samplesToProcess, 
                                      opt->frame_period);
    
    // Check buffer size and sanity
    if (frameCount <= 0 || frameCount > static_cast<int>(worldF0.size()))
    {
        if (frameCount > static_cast<int>(worldF0.size()))
        {
            DBG("DIO: Frame count " << frameCount << " exceeds buffer size " << worldF0.size());
            // Don't resize, just limit frameCount
            frameCount = static_cast<int>(worldF0.size());
        }
        else
        {
            DBG("DIO: Invalid frame count: " << frameCount);
            return lastValidPitch;
        }
    }
    
    // Call WORLD DIO for pitch detection
    Dio(worldBuffer.data(), 
        samplesToProcess, 
        static_cast<int>(sampleRate), 
        opt,
        worldTimeAxis.data(), 
        worldF0.data());
    
    // Get the most recent valid pitch (from the end of the buffer)
    float latestPitch = 0.0f;
    
    // Look for the most recent valid pitch without averaging
    // This gives us the most up-to-date pitch
    for (int i = frameCount - 1; i >= 0 && i >= frameCount - 10; --i)
    {
        if (worldF0[i] > 0.0)
        {
            latestPitch = static_cast<float>(worldF0[i]);
            break;
        }
    }
    
    if (dioDebugCounter % 50 == 0)
    {
        DBG("DIO: Frame count: " << frameCount << ", Latest pitch: " << latestPitch);
        if (frameCount > 0)
        {
            DBG("DIO: First 5 F0 values: " << worldF0[0] << ", " << worldF0[1] << ", " << worldF0[2] << ", " << worldF0[3] << ", " << worldF0[4]);
            DBG("DIO: Last 5 F0 values: " << worldF0[std::max(0, frameCount-5)] << ", " << worldF0[std::max(0, frameCount-4)] << ", " << worldF0[std::max(0, frameCount-3)] << ", " << worldF0[std::max(0, frameCount-2)] << ", " << worldF0[std::max(0, frameCount-1)]);
        }
        DBG("DIO: Buffer write pos: " << dioBufferWritePos << "/" << dioBufferSize);
    }
    
    // Apply frequency bounds check
    if (latestPitch < minFrequency || latestPitch > maxFrequency)
    {
        return lastValidPitch; // Return last valid instead of 0
    }
    
    if (latestPitch > 0.0f)
    {
        lastValidPitch = latestPitch;
    }
    
    return latestPitch;
}

void PitchDetector::setDIOSpeed(int speed)
{
    if (worldOption)
    {
        DioOption* opt = static_cast<DioOption*>(worldOption);
        opt->speed = std::max(1, std::min(12, speed)); // Clamp to valid range
    }
}

void PitchDetector::setDIOFramePeriod(float framePeriod)
{
    if (worldOption)
    {
        DioOption* opt = static_cast<DioOption*>(worldOption);
        opt->frame_period = framePeriod;
    }
}

void PitchDetector::setDIOAllowedRange(float allowedRange)
{
    if (worldOption)
    {
        DioOption* opt = static_cast<DioOption*>(worldOption);
        opt->allowed_range = allowedRange;
    }
}

void PitchDetector::setDIOChannelsInOctave(float channels)
{
    if (worldOption)
    {
        DioOption* opt = static_cast<DioOption*>(worldOption);
        opt->channels_in_octave = channels;
    }
}

void PitchDetector::setDIOBufferTime(float bufferTime)
{
    // Clamp buffer time to safe limits (max 1.5 seconds to prevent crashes)
    dioBufferTimeSeconds = std::clamp(bufferTime, 0.05f, 1.5f);
    
    // Resize the buffer if sample rate is already set
    if (sampleRate > 0)
    {
        int newBufferSize = static_cast<int>(sampleRate * dioBufferTimeSeconds);
        
        // Sanity check - limit to 2 seconds worth of samples at max
        const int maxSafeBufferSize = static_cast<int>(sampleRate * 2.0);
        newBufferSize = std::min(newBufferSize, maxSafeBufferSize);
        
        if (newBufferSize != dioBufferSize && newBufferSize > 0)
        {
            std::lock_guard<std::mutex> lock(dioBufferMutex);
            
            // Create new buffers with the new size
            std::vector<double> newRollingBuffer(newBufferSize, 0.0);
            std::vector<double> newWorldBuffer(newBufferSize);
            
            // Copy existing data if possible
            int copySize = std::min(dioBufferSize, newBufferSize);
            if (copySize > 0 && dioBufferSize > 0)
            {
                int readPos = dioBufferWritePos;
                for (int i = 0; i < copySize; ++i)
                {
                    newRollingBuffer[i] = dioRollingBuffer[readPos];
                    readPos = (readPos + 1) % dioBufferSize;
                }
            }
            
            // Swap with new buffers
            dioRollingBuffer.swap(newRollingBuffer);
            worldBuffer.swap(newWorldBuffer);
            
            dioBufferSize = newBufferSize;
            dioBufferWritePos = 0;
            dioSamplesAccumulated = 0;
            dioTotalSamplesReceived = 0;
            dioBufferFilled = false;
        }
    }
}

void PitchDetector::setAlgorithm(Algorithm algo) 
{ 
    algorithm = algo; 
    if (algo == Algorithm::WORLD_DIO)
    {
        resetDIOState();
    }
}

void PitchDetector::resetDIOState()
{
    dioBufferWritePos = 0;
    dioSamplesAccumulated = 0;
    dioTotalSamplesReceived = 0;
    dioBufferFilled = false;
    if (dioBufferSize > 0 && dioRollingBuffer.size() > 0)
    {
        std::fill(dioRollingBuffer.begin(), dioRollingBuffer.end(), 0.0);
    }
}