#ifndef BEAT_TRACKER_H
#define BEAT_TRACKER_H

#include <vector>
#include <functional>
#include <string>
#include <mutex>

// Forward declare the BTT struct
extern "C" {
    typedef struct Opaque_BTT_Struct BTT;
}

class BeatTracker {
public:
    struct Beat {
        double position_seconds;
        double position_samples;
        double tempo_at_beat;
        bool is_downbeat;
    };
    
    struct Bar {
        double position_seconds;
        double position_samples;
        double bpm;
        int bar_number;
    };
    
    enum TimeSignature {
        TIME_4_4 = 4,
        TIME_3_4 = 3,
        TIME_6_8 = 6,
        TIME_5_4 = 5,
        TIME_7_8 = 7
    };
    
    using ProgressCallback = std::function<void(const std::string&)>;
    
    BeatTracker();
    ~BeatTracker();
    
    // Process audio and detect beats
    bool processAudio(const std::vector<float>& audioData, double sampleRate, TimeSignature timeSig = TIME_4_4);
    
    // Get results
    const std::vector<Beat>& getBeats() const { return beats; }
    const std::vector<Bar>& getBars() const { return bars; }
    double getAverageTempo() const;
    
    // Set progress callback for GUI updates
    void setProgressCallback(ProgressCallback callback) { progressCallback = callback; }
    
    // Configuration
    void setMinTempo(double bpm);
    void setMaxTempo(double bpm);
    void setTimeSignature(TimeSignature sig) { timeSignature = sig; }
    void setInitialTempo(double bpm);  // Hint for tempo at start of track
    void setBarOffset(int offset) { barOffset = offset; }
    
private:
    BTT* btt;
    std::vector<Beat> beats;
    std::vector<Bar> bars;
    ProgressCallback progressCallback;
    TimeSignature timeSignature;
    double sampleRate;
    double originalSampleRate;
    std::mutex beatMutex;
    int barOffset = 0;
    
    // BTT callbacks (static because C library)
    static void onsetCallback(void* self, unsigned long long sampleTime);
    static void beatCallback(void* self, unsigned long long sampleTime);
    
    // Internal methods
    void calculateBarsFromBeats();
    void reportProgress(const std::string& message);
    double calculateLocalBPM(size_t beatIndex);
    int getBeatsPerBar() const;
};

#endif // BEAT_TRACKER_H