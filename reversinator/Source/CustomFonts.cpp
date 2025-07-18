#include "CustomFonts.h"
#include "BinaryData.h"

// Global instance
static std::unique_ptr<CustomFonts> customFontsInstance;

CustomFonts* getCustomFonts()
{
    if (!customFontsInstance)
        customFontsInstance = std::make_unique<CustomFonts>();
    return customFontsInstance.get();
}

CustomFonts::CustomFonts()
{
    // Load the embedded Azeret Mono fonts
    mediumTypeface = juce::Typeface::createSystemTypefaceFor(
        BinaryData::AzeretMonoMedium_ttf,
        BinaryData::AzeretMonoMedium_ttfSize);
    
    boldTypeface = juce::Typeface::createSystemTypefaceFor(
        BinaryData::AzeretMonoBold_ttf,
        BinaryData::AzeretMonoBold_ttfSize);
    
    // Fallback to system fonts if loading fails
    if (mediumTypeface == nullptr || boldTypeface == nullptr)
    {
        juce::Font systemFont;
        
        #if JUCE_MAC
            systemFont = juce::Font("Helvetica Neue", 16.0f, juce::Font::plain);
        #elif JUCE_WINDOWS
            systemFont = juce::Font("Segoe UI", 16.0f, juce::Font::plain);
        #else
            systemFont = juce::Font("Arial", 16.0f, juce::Font::plain);
        #endif
        
        if (mediumTypeface == nullptr)
            mediumTypeface = systemFont.getTypefacePtr();
        if (boldTypeface == nullptr)
            boldTypeface = systemFont.withStyle(juce::Font::bold).getTypefacePtr();
    }
}

juce::Font CustomFonts::getFont(float height, int styleFlags) const
{
    // Use bold typeface for bold text, medium for everything else
    if ((styleFlags & juce::Font::bold) != 0 && boldTypeface != nullptr)
    {
        return juce::Font(boldTypeface).withHeight(height);
    }
    else if (mediumTypeface != nullptr)
    {
        return juce::Font(mediumTypeface).withHeight(height);
    }
    
    // Fallback to default font
    return juce::Font(height, styleFlags);
}