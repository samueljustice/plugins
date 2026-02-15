#ifndef AUDIO_FILE_READER_H
#define AUDIO_FILE_READER_H

#include <string>
#include <vector>

class AudioFileReader {
public:
    AudioFileReader();
    ~AudioFileReader();
    
    // Load audio file
    bool load(const std::string& filePath);
    
    // Get mono audio data (converts from stereo if needed)
    std::vector<float> getMonoAudio() const;
    
    // Get audio properties
    float getSampleRate() const { return m_sampleRate; }
    double getDuration() const { return m_duration; }
    int getChannels() const { return m_channels; }
    int getBitsPerSample() const { return m_bitsPerSample; }
    
private:
    std::vector<float> m_audioData;
    float m_sampleRate;
    double m_duration;
    int m_channels;
    int m_bitsPerSample;
    int m_audioFormat;
};

#endif // AUDIO_FILE_READER_H