#include "PyPTSL.h"
#include <sstream>
#include <iostream>
#include <iomanip>
#include <cstdlib>
#include <unistd.h>
#include <mach-o/dyld.h>

PyPTSL::PyPTSL() {
}

PyPTSL::~PyPTSL() {
}

std::string PyPTSL::findPython() {
    // Try to find Python 3 on the system
    const char* pythonCmds[] = {
        "/opt/homebrew/bin/python3",  // Prefer homebrew Python for compatibility
        "/usr/local/bin/python3",
        "/opt/local/bin/python3",
        "/usr/bin/python3",
        "python3",
        "python"
    };
    
    for (const char* cmd : pythonCmds) {
        if (access(cmd, X_OK) == 0) {
            // Verify it's Python 3
            std::string checkCmd = std::string(cmd) + " --version 2>&1";
            FILE* pipe = popen(checkCmd.c_str(), "r");
            if (pipe) {
                char buffer[128];
                std::string result;
                while (fgets(buffer, sizeof(buffer), pipe)) {
                    result += buffer;
                }
                pclose(pipe);
                
                if (result.find("Python 3") != std::string::npos) {
                    return cmd;
                }
            }
        }
    }
    
    m_lastError = "Python 3 not found on system";
    return "";
}

std::string PyPTSL::getPythonScriptPath() {
    // Get the path to the executable
    char pathbuf[1024];
    uint32_t bufsize = sizeof(pathbuf);
    
    if (_NSGetExecutablePath(pathbuf, &bufsize) == 0) {
        std::string exePath(pathbuf);
        
        // Check if we're in an app bundle
        size_t pos = exePath.find(".app/Contents/MacOS/");
        if (pos != std::string::npos) {
            // We're in an app bundle
            std::string bundlePath = exePath.substr(0, pos + 4);
            return bundlePath + "/Contents/Resources/python/ptsl_client.py";
        } else {
            // Standalone executable - check for bundled python directory
            size_t lastSlash = exePath.rfind('/');
            if (lastSlash != std::string::npos) {
                std::string exeDir = exePath.substr(0, lastSlash + 1);
                std::string bundledScript = exeDir + "python/ptsl_client.py";
                
                // Check if bundled version exists
                if (access(bundledScript.c_str(), R_OK) == 0) {
                    return bundledScript;
                }
                
                // Fallback to non-bundled
                return exeDir + "ptsl_client.py";
            }
        }
    }
    
    // Fallback
    return "./ptsl_client.py";
}

std::string PyPTSL::getBundledPythonPath() {
    // Get the path to the executable
    char pathbuf[1024];
    uint32_t bufsize = sizeof(pathbuf);
    
    if (_NSGetExecutablePath(pathbuf, &bufsize) == 0) {
        std::string exePath(pathbuf);
        
        // Check if we're in an app bundle
        size_t pos = exePath.find(".app/Contents/MacOS/");
        if (pos != std::string::npos) {
            // We're in an app bundle - use bundled Python
            std::string bundlePath = exePath.substr(0, pos + 4);
            std::string bundledPython = bundlePath + "/Contents/MacOS/python3";
            
            // Check if bundled Python exists
            if (access(bundledPython.c_str(), X_OK) == 0) {
                return bundledPython;
            }
        }
    }
    
    // No bundled Python found
    return "";
}

std::string PyPTSL::barsToJson(const std::vector<BarMarker>& bars) {
    std::stringstream json;
    json << "{\"bars\": [";
    
    bool first = true;
    
    for (const auto& bar : bars) {
        if (!first) json << ", ";
        json << "{";
        json << "\"time\": " << std::fixed << std::setprecision(3) << bar.time;
        json << ", \"bpm\": " << std::fixed << std::setprecision(1) << bar.bpm;
        json << ", \"bar_number\": " << bar.barNumber;
        json << "}";
        
        first = false;
    }
    
    json << "]}";
    return json.str();
}

bool PyPTSL::sendBarsToProTools(const std::vector<BarMarker>& bars,
                               const std::string& startTimecode,
                               bool clearExisting) {
    
    // First try bundled Python
    std::string pythonCmd = getBundledPythonPath();
    
    // Fall back to system Python if no bundled version
    if (pythonCmd.empty()) {
        pythonCmd = findPython();
        if (pythonCmd.empty()) {
            return false;
        }
    }
    
    // Get script path
    std::string scriptPath = getPythonScriptPath();
    
    // Check if script exists
    if (access(scriptPath.c_str(), R_OK) != 0) {
        m_lastError = "Python script not found: " + scriptPath;
        return false;
    }
    
    // Debug: Log the command being executed
    std::cerr << "PyPTSL: Using script path: " << scriptPath << std::endl;
    
    // Convert bars to JSON
    std::string barsJson = barsToJson(bars);
    
    // Build command
    std::stringstream cmd;
    
    // Check if we're using bundled Python (either app bundle or CLI)
    if (scriptPath.find("/python/ptsl_client.py") != std::string::npos) {
        // Get the python directory path
        size_t lastSlash = scriptPath.rfind('/');
        std::string pythonDir = scriptPath.substr(0, lastSlash);
        
        std::cerr << "PyPTSL: Using bundled Python at: " << pythonDir << std::endl;
        
        // Set PYTHONPATH to include the bundled modules
        cmd << "PYTHONPATH=\"" << pythonDir << "\" ";
    }
    
    // Add the python command with properly quoted paths
    cmd << "\"" << pythonCmd << "\" \"" << scriptPath << "\" ";
    cmd << "--bars ";
    
    // Write the JSON to a temporary file to avoid shell escaping issues
    std::string tempJsonFile = "/tmp/ptsl_bars_" + std::to_string(getpid()) + ".json";
    FILE* jsonFile = fopen(tempJsonFile.c_str(), "w");
    if (jsonFile) {
        fprintf(jsonFile, "%s", barsJson.c_str());
        fclose(jsonFile);
        
        cmd << "\"" << tempJsonFile << "\" ";
    } else {
        // Fallback to inline JSON (may have issues with special characters)
        cmd << "'" << barsJson << "' ";
    }
    
    cmd << "\"" << startTimecode << "\" ";
    cmd << (clearExisting ? "--clear" : "");
    
    std::cerr << "PyPTSL: Executing command: " << cmd.str() << std::endl;
    
    // Execute command
    FILE* pipe = popen(cmd.str().c_str(), "r");
    if (!pipe) {
        m_lastError = "Failed to execute Python script";
        return false;
    }
    
    // Read output
    std::string output;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        output += buffer;
    }
    
    int returnCode = pclose(pipe);
    
    // Clean up temporary JSON file if it was created
    if (access(tempJsonFile.c_str(), F_OK) == 0) {
        unlink(tempJsonFile.c_str());
    }
    
    // Check result
    if (returnCode == 0 && output.find("\"success\": true") != std::string::npos) {
        return true;
    } else {
        // Try to parse JSON error message
        size_t msgStart = output.find("\"message\": \"");
        if (msgStart != std::string::npos) {
            msgStart += 12; // length of "\"message\": \""
            size_t msgEnd = output.find("\"", msgStart);
            if (msgEnd != std::string::npos) {
                m_lastError = output.substr(msgStart, msgEnd - msgStart);
            } else {
                m_lastError = output;
            }
        } else if (output.empty()) {
            m_lastError = "Failed to communicate with Pro Tools. Check that py-ptsl is installed.";
        } else {
            m_lastError = output;
        }
        return false;
    }
}