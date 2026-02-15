#include <iostream>
#include <string>
#include <vector>
#include <iomanip>
#include "BeatTracker.h"
#include "PyPTSL.h"
#include "AudioFileReader.h"

void printUsage(const char* programName) {
    std::cout << "PTSL Beat Tool CLI - Variable Tempo Detection for Pro Tools\n";
    std::cout << "Copyright © 2025 Samuel Justice\n\n";
    std::cout << "Usage: " << programName << " [options] <audio_file>\n\n";
    std::cout << "Options:\n";
    std::cout << "  -h, --help              Show this help message\n";
    std::cout << "  -s, --start <timecode>  Start timecode (default: 01:00:00:00)\n";
    std::cout << "  -t, --time-sig <sig>    Time signature: 4/4, 3/4, 6/8, 5/4, 7/8 (default: 4/4)\n";
    std::cout << "  -c, --clear             Clear existing memory locations\n";
    std::cout << "  --min-bpm <value>       Minimum BPM (default: 60)\n";
    std::cout << "  --max-bpm <value>       Maximum BPM (default: 180)\n";
    std::cout << "  --hint-bpm <value>      Hint for initial tempo (helps with ambiguous music)\n";
    std::cout << "  --bar-offset <n>        Number of beats to offset bar 1 (default: 0)\n";
    std::cout << "  --show-all-beats        Export all detected beats, not just bars\n";
    std::cout << "  --no-send               Don't send to Pro Tools (just detect beats)\n";
    std::cout << "  -f, --format <fmt>      Output format: text, json, csv (default: text)\n";
}

BeatTracker::TimeSignature parseTimeSignature(const std::string& sig) {
    if (sig == "3/4") return BeatTracker::TIME_3_4;
    if (sig == "6/8") return BeatTracker::TIME_6_8;
    if (sig == "5/4") return BeatTracker::TIME_5_4;
    if (sig == "7/8") return BeatTracker::TIME_7_8;
    return BeatTracker::TIME_4_4;  // Default
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }
    
    // Parse command line arguments
    std::string audioFile;
    std::string startTimecode = "01:00:00:00";
    std::string outputFormat = "text";
    std::string timeSignature = "4/4";
    double minBPM = 60.0;
    double maxBPM = 180.0;
    double hintBPM = 0.0;
    int barOffset = 0;
    bool clearExisting = false;
    bool sendToProTools = true;
    bool showAllBeats = false;
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        } else if (arg == "-s" || arg == "--start") {
            if (++i < argc) {
                startTimecode = argv[i];
            }
        } else if (arg == "-t" || arg == "--time-sig") {
            if (++i < argc) {
                timeSignature = argv[i];
            }
        } else if (arg == "-c" || arg == "--clear") {
            clearExisting = true;
        } else if (arg == "--min-bpm") {
            if (++i < argc) {
                minBPM = std::stod(argv[i]);
            }
        } else if (arg == "--max-bpm") {
            if (++i < argc) {
                maxBPM = std::stod(argv[i]);
            }
        } else if (arg == "--hint-bpm") {
            if (++i < argc) {
                hintBPM = std::stod(argv[i]);
            }
        } else if (arg == "--bar-offset") {
            if (++i < argc) {
                barOffset = std::stoi(argv[i]);
            }
        } else if (arg == "--show-all-beats") {
            showAllBeats = true;
        } else if (arg == "--no-send") {
            sendToProTools = false;
        } else if (arg == "-f" || arg == "--format") {
            if (++i < argc) {
                outputFormat = argv[i];
            }
        } else if (!arg.empty() && arg[0] != '-') {
            audioFile = arg;
        }
    }
    
    if (audioFile.empty()) {
        std::cerr << "Error: No audio file specified\n";
        printUsage(argv[0]);
        return 1;
    }
    
    std::cout << "PTSL Beat Tool CLI - Variable Tempo Detection\n";
    std::cout << "=============================================\n";
    std::cout << "Processing: " << audioFile << "\n";
    std::cout << "Time Signature: " << timeSignature << "\n";
    std::cout << "BPM Range: " << minBPM << " - " << maxBPM << "\n";
    if (barOffset != 0) {
        std::cout << "Bar Offset: " << barOffset << " beats\n";
    }
    std::cout << "\n";
    
    // Load audio file
    AudioFileReader reader;
    std::cout << "Loading audio file...\n";
    if (!reader.load(audioFile)) {
        std::cerr << "ERROR: Failed to load audio file\n";
        return 1;
    }
    
    std::cout << "Audio loaded: " << reader.getSampleRate() << " Hz, " 
              << reader.getDuration() << " seconds\n";
    
    // Create beat tracker
    BeatTracker tracker;
    tracker.setMinTempo(minBPM);
    tracker.setMaxTempo(maxBPM);
    
    // Set initial tempo hint if provided
    if (hintBPM > 0) {
        std::cout << "Using tempo hint: " << hintBPM << " BPM\n";
        tracker.setInitialTempo(hintBPM);
    }
    
    // Set bar offset if provided
    if (barOffset != 0) {
        std::cout << "Bar offset: " << barOffset << " beats\n";
        tracker.setBarOffset(barOffset);
    }
    
    // Set progress callback to show progress
    tracker.setProgressCallback([](const std::string& msg) {
        std::cout << "  " << msg << "\n";
    });
    
    // Process audio
    std::cout << "\nDetecting beats...\n";
    std::vector<float> audioData = reader.getMonoAudio();
    if (!tracker.processAudio(audioData, reader.getSampleRate(), parseTimeSignature(timeSignature))) {
        std::cerr << "ERROR: Failed to detect beats\n";
        return 1;
    }
    
    const auto& bars = tracker.getBars();
    const auto& beats = tracker.getBeats();
    double avgTempo = tracker.getAverageTempo();
    
    // Show results
    std::cout << "\nDETECTION RESULTS\n";
    std::cout << "=================\n";
    std::cout << "Average tempo: " << std::fixed << std::setprecision(1) << avgTempo << " BPM\n";
    std::cout << "Found " << beats.size() << " beats\n";
    std::cout << "Found " << bars.size() << " bars\n";
    
    // If showing all beats, display beat info
    if (showAllBeats) {
        std::cout << "\nShowing all beats (for debugging alignment):\n";
        std::cout << "First 20 beats:\n";
        for (size_t i = 0; i < std::min(beats.size(), size_t(20)); ++i) {
            const auto& beat = beats[i];
            std::cout << "  Beat " << i << ": " 
                      << std::fixed << std::setprecision(3) << beat.position_seconds << "s"
                      << " - " << std::setprecision(1) << beat.tempo_at_beat << " BPM"
                      << (beat.is_downbeat ? " [DOWNBEAT]" : "") << "\n";
        }
        if (beats.size() > 20) {
            std::cout << "  ... and " << (beats.size() - 20) << " more beats\n";
        }
    }
    
    // Output bars in requested format
    if (outputFormat == "json") {
        std::cout << "\n{\"bars\":[";
        for (size_t i = 0; i < bars.size(); ++i) {
            if (i > 0) std::cout << ",";
            std::cout << "{\"time\":" << bars[i].position_seconds 
                      << ",\"bpm\":" << bars[i].bpm
                      << ",\"bar_number\":" << bars[i].bar_number << "}";
        }
        std::cout << "]}\n";
    } else if (outputFormat == "csv") {
        std::cout << "\ntime,bar_number,bpm\n";
        for (const auto& bar : bars) {
            std::cout << bar.position_seconds << "," << bar.bar_number << "," 
                      << bar.bpm << "\n";
        }
    } else {
        // Default text format
        std::cout << "\nFirst 10 bars:\n";
        for (size_t i = 0; i < std::min(bars.size(), size_t(10)); ++i) {
            const auto& bar = bars[i];
            std::cout << "  Bar " << bar.bar_number << ": " 
                      << std::fixed << std::setprecision(3) << bar.position_seconds << "s"
                      << " - " << std::setprecision(1) << bar.bpm << " BPM\n";
        }
        if (bars.size() > 10) {
            std::cout << "  ... and " << (bars.size() - 10) << " more bars\n";
        }
    }
    
    // Send to Pro Tools if requested
    if (sendToProTools) {
        std::cout << "\nSending to Pro Tools...\n";
        std::cout << "Start timecode: " << startTimecode << "\n";
        
        if (showAllBeats) {
            // Send ALL beats as markers for debugging
            std::cout << "Sending ALL beats as markers (debug mode)...\n";
            std::vector<PyPTSL::BarMarker> markers;
            for (size_t i = 0; i < beats.size(); ++i) {
                const auto& beat = beats[i];
                PyPTSL::BarMarker marker;
                marker.time = beat.position_seconds;
                marker.bpm = beat.tempo_at_beat;
                marker.barNumber = i + 1; // Beat number instead of bar number
                markers.push_back(marker);
            }
            
            PyPTSL ptsl;
            if (ptsl.sendBarsToProTools(markers, startTimecode, clearExisting)) {
                std::cout << "Successfully created " << markers.size() << " beat markers in Pro Tools!\n";
            } else {
                std::cerr << "Failed to create beat markers\n";
                std::cerr << "Error: " << ptsl.getLastError() << "\n";
                return 1;
            }
        } else {
            // Convert bars to PyPTSL format
            std::vector<PyPTSL::BarMarker> markers;
            for (const auto& bar : bars) {
                PyPTSL::BarMarker marker;
                marker.time = bar.position_seconds;
                marker.bpm = bar.bpm;
                marker.barNumber = bar.bar_number;
                markers.push_back(marker);
            }
            
            PyPTSL ptsl;
            if (ptsl.sendBarsToProTools(markers, startTimecode, clearExisting)) {
                std::cout << "Successfully created bar markers in Pro Tools!\n";
            } else {
                std::cerr << "Failed to create bar markers\n";
                std::cerr << "Error: " << ptsl.getLastError() << "\n";
                return 1;
            }
        }
    }
    
    return 0;
}