#pragma once

#include <JuceHeader.h>

/**
 * Runs doomgeneric on its own thread.
 *
 * DOOM is one big pile of globals, so there is exactly one game per process no matter
 * how many EQ instances are open. Editors hold this through a SharedResourcePointer:
 * while at least one editor is open the thread ticks the game; when the last one closes
 * the thread stops and the game's clock freezes, so it resumes exactly where it was
 * (demo, menu or mid-firefight) the next time any editor opens.
 */
class DoomEngine : private juce::Thread
{
public:
    static constexpr int width = 640;
    static constexpr int height = 400;

    enum class State { Idle, Booting, Running, Dead };

    DoomEngine();
    ~DoomEngine() override;

    /** doomKey is a doomkeys.h code. Safe from any thread. */
    static void pushKey (bool pressed, int doomKey);

    /** Copies the latest frame into image (ARGB, width x height) if it changed. */
    static bool fetchFrame (juce::Image& image, uint32_t& lastSerial);

    static State getState();

    /** The unpacked Freedoom IWAD (unpacks it on first call). Any thread. */
    static juce::File getWadFile();
    static juce::String getStatusText();

private:
    void run() override;

    JUCE_DECLARE_NON_COPYABLE (DoomEngine)
};
