#include "BeatTracker.h"
#include <cmath>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <numeric>

extern "C" {
    #include "BTT.h"
}

BeatTracker::BeatTracker() : btt(nullptr), timeSignature(TIME_4_4), sampleRate(44100) {
    btt = btt_new_default();
    if (btt) {
        // Set callbacks with this as user data
        btt_set_onset_tracking_callback(btt, onsetCallback, this);
        btt_set_beat_tracking_callback(btt, beatCallback, this);
        
        // Configure for variable tempo tracking
        btt_set_tracking_mode(btt, BTT_ONSET_AND_TEMPO_AND_BEAT_TRACKING);
        
        // Set reasonable defaults
        btt_set_min_tempo(btt, 60);
        btt_set_max_tempo(btt, 180);
        
        // Adjust onset detection to be less sensitive to high frequency content
        btt_set_onset_threshold(btt, 0.3); // Higher threshold to ignore subtle onsets
        btt_set_onset_threshold_min(btt, 10.0); // Higher minimum to avoid false positives
        
        // Focus heavily on sub-bass frequencies where kick drums live (60-100 Hz)
        btt_set_oss_filter_cutoff(btt, 80.0); // Focus on kick drum frequencies
        
        // Use spectral compression to reduce the influence of loud synth stabs
        btt_set_spectral_compression_gamma(btt, 1000.0); // Compress spectral peaks
        
        // Make tempo tracking more stable
        btt_set_gaussian_tempo_histogram_decay(btt, 0.995); // Slower decay for more stability
        btt_set_gaussian_tempo_histogram_width(btt, 3.0); // Narrower width for more precision
        
        // Configure beat tracking parameters
        btt_set_cbss_alpha(btt, 0.95); // More weight on historical data
        btt_set_cbss_eta(btt, 400); // Narrower gaussian window
        
        // Apply a small negative adjustment to compensate for detection latency
        // This helps align beats closer to the actual transients
        btt_set_beat_prediction_adjustment(btt, -10); // -10ms adjustment
        
        btt_set_predicted_beat_gaussian_width(btt, 5.0); // Tighter beat prediction window
        
        // Adjust the autocorrelation for better tempo detection
        btt_set_autocorrelation_exponent(btt, 0.3); // Lower exponent can help with complex rhythms
    } else {
        // Log error if BTT initialization failed
        reportProgress("ERROR: Failed to initialize BTT library!");
    }
}

BeatTracker::~BeatTracker() {
    if (btt) {
        btt_destroy(btt);
    }
}

void BeatTracker::setMinTempo(double bpm) {
    if (btt) {
        btt_set_min_tempo(btt, bpm);
    }
}

void BeatTracker::setMaxTempo(double bpm) {
    if (btt) {
        btt_set_max_tempo(btt, bpm);
    }
}

void BeatTracker::setInitialTempo(double bpm) {
    if (btt && bpm > 0) {
        // Initialize BTT with a known tempo to help it lock on correctly
        btt_init_tempo(btt, bpm);
        
        // Also adjust the log gaussian tempo weight to favor this tempo
        btt_set_log_gaussian_tempo_weight_mean(btt, bpm);
    }
}

bool BeatTracker::processAudio(const std::vector<float>& audioData, double sr, TimeSignature timeSig) {
    if (!btt) {
        reportProgress("ERROR: BTT not initialized");
        return false;
    }
    
    if (audioData.empty()) {
        reportProgress("ERROR: Audio data is empty");
        return false;
    }
    
    sampleRate = sr;
    originalSampleRate = sr;
    timeSignature = timeSig;
    beats.clear();
    bars.clear();
    
    reportProgress("Starting beat detection...");
    
    // Process at original sample rate
    const std::vector<float>& processData = audioData;
    double processSampleRate = sampleRate;
    
    // Debug log the sample rate
    std::stringstream srMsg;
    srMsg << "Processing audio at " << sampleRate << " Hz";
    reportProgress(srMsg.str());
    
    if (sampleRate != 44100) {
        reportProgress("Warning: Audio is " + std::to_string((int)sampleRate) + "Hz, BTT is optimized for 44.1kHz");
    }
    
    // Process audio in chunks
    const int chunkSize = 64; // BTT works well with small chunks
    size_t totalSamples = processData.size();
    size_t processed = 0;
    
    // Convert to the format BTT expects
    std::vector<dft_sample_t> buffer(chunkSize);
    
    while (processed < totalSamples) {
        size_t samplesToProcess = std::min(size_t(chunkSize), totalSamples - processed);
        
        // Copy and convert samples
        for (size_t i = 0; i < samplesToProcess; i++) {
            buffer[i] = processData[processed + i];
        }
        
        // Process this chunk
        btt_process(btt, buffer.data(), samplesToProcess);
        
        processed += samplesToProcess;
        
        // Report progress
        if (processed % (size_t)(processSampleRate * 5) == 0) { // Every 5 seconds
            double percentComplete = (double)processed / totalSamples * 100;
            double currentTempo = btt_get_tempo_bpm(btt);
            std::stringstream ss;
            ss << "Processing: " << std::fixed << std::setprecision(1) << percentComplete 
               << "% - Current tempo: " << std::setprecision(1) << currentTempo << " BPM";
            reportProgress(ss.str());
        }
    }
    
    reportProgress("Beat detection complete. Calculating bars...");
    
    // Log beat detection summary
    if (!beats.empty()) {
        std::stringstream beatInfo;
        beatInfo << "Detected " << beats.size() << " beats";
        reportProgress(beatInfo.str());
        
        // Show tempo range
        double minTempo = 999, maxTempo = 0;
        for (const auto& beat : beats) {
            if (beat.tempo_at_beat < minTempo) minTempo = beat.tempo_at_beat;
            if (beat.tempo_at_beat > maxTempo) maxTempo = beat.tempo_at_beat;
        }
        std::stringstream tempoInfo;
        tempoInfo << "Tempo range: " << std::fixed << std::setprecision(1) 
                  << minTempo << " - " << maxTempo << " BPM";
        reportProgress(tempoInfo.str());
    }
    
    // Calculate bars from detected beats
    calculateBarsFromBeats();
    
    return true;
}

void BeatTracker::onsetCallback(void* self, unsigned long long sampleTime) {
    // We're not using onsets directly, but could log them
    BeatTracker* tracker = static_cast<BeatTracker*>(self);
    (void)tracker; // Suppress unused warning
}

void BeatTracker::beatCallback(void* self, unsigned long long sampleTime) {
    BeatTracker* tracker = static_cast<BeatTracker*>(self);
    
    std::lock_guard<std::mutex> lock(tracker->beatMutex);
    
    Beat beat;
    beat.position_samples = sampleTime;
    beat.position_seconds = sampleTime / tracker->sampleRate;
    beat.tempo_at_beat = btt_get_tempo_bpm(tracker->btt);
    beat.is_downbeat = false; // Will be determined later
    
    // Debug logging
    if (tracker->beats.size() < 10) {  // Log first 10 beats
        std::stringstream debugMsg;
        debugMsg << "Beat " << tracker->beats.size() << " detected: "
                 << "sample=" << sampleTime 
                 << ", time=" << std::fixed << std::setprecision(3) << beat.position_seconds << "s"
                 << ", tempo=" << std::setprecision(1) << beat.tempo_at_beat << " BPM"
                 << " (SR=" << tracker->sampleRate << "Hz)";
        tracker->reportProgress(debugMsg.str());
    }
    
    tracker->beats.push_back(beat);
}

void BeatTracker::calculateBarsFromBeats() {
    if (beats.empty()) return;
    
    int beatsPerBar = getBeatsPerBar();
    bars.clear();
    
    // Simple approach: place a bar marker every N beats based on time signature
    // No quantization or BPM-based grid alignment - just trust the beat detection
    
    reportProgress("Placing bar markers based on detected beats...");
    
    // Apply bar offset - start placing bars from the offset beat position
    size_t startBeat = barOffset >= 0 ? barOffset : 0;
    if (startBeat >= beats.size()) {
        reportProgress("Warning: Bar offset exceeds number of detected beats, starting at beat 0");
        startBeat = 0;
    }
    
    if (barOffset > 0) {
        std::stringstream offsetMsg;
        offsetMsg << "Applying bar offset: Starting bar 1 at beat " << barOffset;
        reportProgress(offsetMsg.str());
    }
    
    // Log time signature info
    std::stringstream tsInfo;
    tsInfo << "Time signature: " << beatsPerBar << " beats per bar";
    reportProgress(tsInfo.str());
    
    int barNumber = 1;
    
    // Place a bar marker every N beats
    for (size_t i = startBeat; i < beats.size(); i += beatsPerBar) {
        Bar bar;
        bar.position_samples = beats[i].position_samples;
        bar.position_seconds = beats[i].position_seconds;
        bar.bar_number = barNumber++;
        
        // Use the tempo that BTT detected at this beat
        bar.bpm = beats[i].tempo_at_beat;
        
        // Mark this beat as a downbeat
        beats[i].is_downbeat = true;
        
        bars.push_back(bar);
        
        // Log first few bars
        if (barNumber <= 6) {  // Show first 5 bars
            std::stringstream ss;
            ss << "Bar " << bar.bar_number << " at " << std::fixed << std::setprecision(3) 
               << bar.position_seconds << "s - BPM: " << std::setprecision(1) << bar.bpm;
            reportProgress(ss.str());
        }
    }
    
    // Log summary
    if (!bars.empty()) {
        std::stringstream summary;
        summary << "Created " << bars.size() << " bar markers (every " << beatsPerBar << " beats)";
        reportProgress(summary.str());
        
        // Show tempo variation if significant
        double minTempo = 999, maxTempo = 0;
        for (const auto& bar : bars) {
            if (bar.bpm < minTempo) minTempo = bar.bpm;
            if (bar.bpm > maxTempo) maxTempo = bar.bpm;
        }
        
        if (maxTempo - minTempo > 10) {
            std::stringstream tempoVar;
            tempoVar << "Note: Tempo varies from " << std::fixed << std::setprecision(1) 
                     << minTempo << " to " << maxTempo << " BPM";
            reportProgress(tempoVar.str());
        }
    }
}

int BeatTracker::getBeatsPerBar() const {
    switch (timeSignature) {
        case TIME_3_4: return 3;
        case TIME_4_4: return 4;
        case TIME_5_4: return 5;
        case TIME_6_8: return 6;  // Assuming compound time, 6 eighth notes
        case TIME_7_8: return 7;
        default: return 4;
    }
}

double BeatTracker::getAverageTempo() const {
    if (bars.empty()) return 0;
    
    double totalBPM = 0;
    for (const auto& bar : bars) {
        totalBPM += bar.bpm;
    }
    
    return totalBPM / bars.size();
}

void BeatTracker::reportProgress(const std::string& message) {
    if (progressCallback) {
        progressCallback(message);
    }
}

// Resampling removed - processing at original sample rate