#ifndef BEAT_DETECTOR_H
#define BEAT_DETECTOR_H

#include <vector>
#include <string>
#include <memory>

namespace breakfastquay {
    class MiniBPM;
}

struct BeatInfo {
    double time;      // Time in seconds
    double confidence; // Confidence level 0-1
    bool isDownbeat;  // True if this is a downbeat
};

class BeatDetector {
public:
    BeatDetector();
    ~BeatDetector();

    // Load audio file and detect beats
    bool loadAndDetect(const std::string& audioFilePath);
    
    // Get detected beats
    const std::vector<BeatInfo>& getBeats() const { return m_beats; }
    
    // Get estimated tempo
    double getEstimatedTempo() const { return m_estimatedTempo; }
    
    // Get all tempo candidates
    std::vector<double> getTempoCandidates() const;
    
    // Set BPM range
    void setBPMRange(double minBPM, double maxBPM);
    
    // Set beats per bar
    void setBeatsPerBar(int beatsPerBar);

private:
    std::unique_ptr<breakfastquay::MiniBPM> m_miniBPM;
    std::vector<BeatInfo> m_beats;
    double m_estimatedTempo;
    float m_sampleRate;
};

#endif // BEAT_DETECTOR_H