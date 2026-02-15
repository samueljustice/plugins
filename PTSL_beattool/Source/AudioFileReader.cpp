#include "AudioFileReader.h"
#include <iostream>
#include <fstream>
#include <cstring>
#include <algorithm>

// Simple WAV file reader implementation
struct WavHeader {
    char riff[4];           // "RIFF"
    uint32_t fileSize;
    char wave[4];           // "WAVE"
    char fmt[4];            // "fmt "
    uint32_t fmtSize;
    uint16_t audioFormat;
    uint16_t numChannels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
};

AudioFileReader::AudioFileReader() 
    : m_sampleRate(44100.0f)
    , m_duration(0.0)
    , m_channels(1)
    , m_bitsPerSample(16)
    , m_audioFormat(1) {
}

AudioFileReader::~AudioFileReader() = default;

bool AudioFileReader::load(const std::string& filePath) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Failed to open file: " << filePath << std::endl;
        return false;
    }
    
    // Read RIFF header
    char riff[4];
    uint32_t fileSize;
    char wave[4];
    
    file.read(riff, 4);
    file.read(reinterpret_cast<char*>(&fileSize), 4);
    file.read(wave, 4);
    
    // Verify it's a WAV file
    if (std::strncmp(riff, "RIFF", 4) != 0 || 
        std::strncmp(wave, "WAVE", 4) != 0) {
        std::cerr << "Not a valid WAV file" << std::endl;
        return false;
    }
    
    // Find and read fmt chunk
    bool foundFmt = false;
    char chunkId[4];
    uint32_t chunkSize;
    
    std::cout << "AudioFileReader: Searching for fmt chunk..." << std::endl;
    
    while (file.read(chunkId, 4)) {
        file.read(reinterpret_cast<char*>(&chunkSize), 4);
        
        std::cout << "AudioFileReader: Found chunk '" << std::string(chunkId, 4) << "' size: " << chunkSize << std::endl;
        
        if (std::strncmp(chunkId, "fmt ", 4) == 0) {
            foundFmt = true;
            
            // Read format chunk
            uint16_t audioFormat;
            file.read(reinterpret_cast<char*>(&audioFormat), 2);
            m_audioFormat = audioFormat;
            
            file.read(reinterpret_cast<char*>(&m_channels), 2);
            
            uint32_t sampleRate;
            file.read(reinterpret_cast<char*>(&sampleRate), 4);
            m_sampleRate = static_cast<float>(sampleRate);
            
            uint32_t byteRate;
            uint16_t blockAlign;
            uint16_t bitsPerSample;
            file.read(reinterpret_cast<char*>(&byteRate), 4);
            file.read(reinterpret_cast<char*>(&blockAlign), 2);
            file.read(reinterpret_cast<char*>(&bitsPerSample), 2);
            
            // Store bits per sample for later use
            m_bitsPerSample = bitsPerSample;
            
            std::cout << "AudioFileReader: Format: " << m_audioFormat 
                      << ", Channels: " << m_channels 
                      << ", Sample Rate: " << m_sampleRate 
                      << ", Bits: " << m_bitsPerSample << std::endl;
            
            // Skip any extra format bytes
            if (chunkSize > 16) {
                file.seekg(chunkSize - 16, std::ios::cur);
            }
            break;
        } else {
            // Skip this chunk
            if (chunkSize % 2 != 0) chunkSize++; // Pad odd chunks
            file.seekg(chunkSize, std::ios::cur);
            if (!file.good()) {
                std::cerr << "AudioFileReader: Error skipping chunk" << std::endl;
                break;
            }
        }
    }
    
    if (!foundFmt) {
        std::cerr << "No fmt chunk found in WAV file" << std::endl;
        return false;
    }
    
    // Find data chunk - handle additional chunks that might exist
    bool foundData = false;
    
    // Reset to start looking for data chunk
    // Skip any additional chunks after fmt chunk
    while (file.read(chunkId, 4)) {
        file.read(reinterpret_cast<char*>(&chunkSize), 4);
        
        if (std::strncmp(chunkId, "data", 4) == 0) {
            foundData = true;
            break;
        } else {
            // Skip this chunk
            // Make sure we handle odd-sized chunks (WAV spec requires padding)
            if (chunkSize % 2 != 0) {
                chunkSize++;
            }
            file.seekg(chunkSize, std::ios::cur);
            
            if (!file.good()) {
                std::cerr << "Error reading WAV file - unexpected end of file" << std::endl;
                return false;
            }
        }
    }
    
    if (!foundData) {
        std::cerr << "No data chunk found in WAV file" << std::endl;
        return false;
    }
    
    // Read audio data
    int bytesPerSample = m_bitsPerSample / 8;
    int numSamples = chunkSize / bytesPerSample;
    
    m_audioData.clear();
    m_audioData.reserve(numSamples);
    
    if (m_bitsPerSample == 16) {
        // 16-bit audio
        std::vector<int16_t> samples(numSamples);
        file.read(reinterpret_cast<char*>(samples.data()), chunkSize);
        
        // Convert to float [-1, 1]
        for (int16_t sample : samples) {
            m_audioData.push_back(static_cast<float>(sample) / 32768.0f);
        }
    } else if (m_bitsPerSample == 24) {
        // 24-bit audio
        std::vector<uint8_t> bytes(chunkSize);
        file.read(reinterpret_cast<char*>(bytes.data()), chunkSize);
        
        for (size_t i = 0; i < bytes.size(); i += 3) {
            int32_t sample = (bytes[i] | (bytes[i+1] << 8) | (bytes[i+2] << 16));
            if (sample & 0x800000) sample |= 0xFF000000; // Sign extend
            m_audioData.push_back(static_cast<float>(sample) / 8388608.0f);
        }
    } else if (m_bitsPerSample == 32) {
        if (m_audioFormat == 3) {
            // 32-bit float audio (IEEE Float)
            std::cout << "AudioFileReader: Reading 32-bit float audio" << std::endl;
            m_audioData.resize(numSamples);
            file.read(reinterpret_cast<char*>(m_audioData.data()), chunkSize);
        } else {
            // 32-bit integer audio
            std::cout << "AudioFileReader: Reading 32-bit integer audio" << std::endl;
            std::vector<int32_t> samples(numSamples);
            file.read(reinterpret_cast<char*>(samples.data()), chunkSize);
            
            // Convert to float [-1, 1]
            for (int32_t sample : samples) {
                m_audioData.push_back(static_cast<float>(sample) / 2147483648.0f);
            }
        }
    } else {
        std::cerr << "Unsupported bit depth: " << m_bitsPerSample << std::endl;
        return false;
    }
    
    m_duration = static_cast<double>(m_audioData.size()) / (m_sampleRate * m_channels);
    
    return true;
}

std::vector<float> AudioFileReader::getMonoAudio() const {
    if (m_channels == 1) {
        return m_audioData;
    }
    
    // Convert to mono by averaging channels
    std::vector<float> monoAudio;
    monoAudio.reserve(m_audioData.size() / m_channels);
    
    for (size_t i = 0; i < m_audioData.size(); i += m_channels) {
        float sum = 0.0f;
        for (int ch = 0; ch < m_channels; ++ch) {
            sum += m_audioData[i + ch];
        }
        monoAudio.push_back(sum / m_channels);
    }
    
    return monoAudio;
}