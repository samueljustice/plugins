#pragma once

#include <JuceHeader.h>

class CustomFonts
{
public:
    CustomFonts();
    ~CustomFonts() = default;
    
    // Get the custom font with specified size and style
    juce::Font getFont(float height, int styleFlags = juce::Font::plain) const;
    
    // Get the medium weight font
    juce::Typeface::Ptr getMediumTypeface() const { return mediumTypeface; }
    
    // Get the bold weight font
    juce::Typeface::Ptr getBoldTypeface() const { return boldTypeface; }
    
    // Check if custom fonts are loaded
    bool isLoaded() const { return mediumTypeface != nullptr && boldTypeface != nullptr; }
    
private:
    juce::Typeface::Ptr mediumTypeface;
    juce::Typeface::Ptr boldTypeface;
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CustomFonts)
};

// Global instance for easy access
extern CustomFonts* getCustomFonts();