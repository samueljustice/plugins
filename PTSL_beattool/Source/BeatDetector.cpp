#include "BeatDetector.h"
#include "AudioFileReader.h"
#include "MiniBpm.h"
#include <iostream>
#include <cmath>

BeatDetector::BeatDetector() 
    : m_estimatedTempo(0.0)
    , m_sampleRate(44100.0f) {
    m_miniBPM = std::make_unique<breakfastquay::MiniBPM>(m_sampleRate);
}

BeatDetector::~BeatDetector() = default;

bool BeatDetector::loadAndDetect(const std::string& audioFilePath) {
    AudioFileReader reader;
    
    if (!reader.load(audioFilePath)) {
        std::cerr << "BeatDetector: Failed to load audio file: " << audioFilePath << std::endl;
        return false;
    }
    
    m_sampleRate = reader.getSampleRate();
    m_miniBPM = std::make_unique<breakfastquay::MiniBPM>(m_sampleRate);
    
    // Get mono audio data
    std::vector<float> audioData = reader.getMonoAudio();
    
    if (audioData.empty()) {
        std::cerr << "BeatDetector: No audio data found in file" << std::endl;
        return false;
    }
    
    std::cout << "BeatDetector: Processing " << audioData.size() << " samples at " 
              << m_sampleRate << " Hz" << std::endl;
    
    // Estimate tempo using MiniBPM
    m_estimatedTempo = m_miniBPM->estimateTempoOfSamples(audioData.data(), audioData.size());
    
    if (m_estimatedTempo <= 0) {
        std::cerr << "BeatDetector: Failed to estimate tempo (result: " << m_estimatedTempo << ")" << std::endl;
        return false;
    }
    
    std::cout << "BeatDetector: Detected tempo: " << m_estimatedTempo << " BPM" << std::endl;
    
    // Calculate beat positions based on estimated tempo
    double beatInterval = 60.0 / m_estimatedTempo; // seconds per beat
    double currentTime = 0.0;
    int beatCount = 0;
    int beatsPerBar = m_miniBPM->getBeatsPerBar();
    
    m_beats.clear();
    
    // Generate beat positions
    while (currentTime < reader.getDuration()) {
        BeatInfo beat;
        beat.time = currentTime;
        beat.confidence = 1.0; // MiniBPM doesn't provide per-beat confidence
        beat.isDownbeat = (beatCount % beatsPerBar == 0);
        
        m_beats.push_back(beat);
        
        currentTime += beatInterval;
        beatCount++;
    }
    
    return true;
}

std::vector<double> BeatDetector::getTempoCandidates() const {
    return m_miniBPM->getTempoCandidates();
}

void BeatDetector::setBPMRange(double minBPM, double maxBPM) {
    m_miniBPM->setBPMRange(minBPM, maxBPM);
}

void BeatDetector::setBeatsPerBar(int beatsPerBar) {
    m_miniBPM->setBeatsPerBar(beatsPerBar);
}