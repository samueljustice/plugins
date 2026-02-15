#ifndef PY_PTSL_H
#define PY_PTSL_H

#include <string>
#include <vector>

// Standalone Python runner that doesn't require Python.h
class PyPTSL {
public:
    PyPTSL();
    ~PyPTSL();
    
    // Structure for bar information
    struct BarMarker {
        double time;        // Time in seconds
        double bpm;         // BPM at this bar
        int barNumber;      // Bar number
    };
    
    // Send bars to Pro Tools via py-ptsl
    bool sendBarsToProTools(const std::vector<BarMarker>& bars,
                           const std::string& startTimecode,
                           bool clearExisting);
    
    // Get last error message
    std::string getLastError() const { return m_lastError; }
    
private:
    std::string m_lastError;
    
    // Convert bars to JSON for Python
    std::string barsToJson(const std::vector<BarMarker>& bars);
    
    // Find Python executable
    std::string findPython();
    
    // Get embedded Python script path
    std::string getPythonScriptPath();
    
    // Get bundled Python interpreter path
    std::string getBundledPythonPath();
};

#endif // PY_PTSL_H