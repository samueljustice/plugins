#pragma once

#include <JuceHeader.h>

/**
 * DOOM's sound effects and music, rendered on the audio thread.
 *
 * There is one game per process, so there is one DOOM audio stream too. The first
 * plug-in instance that asks for it owns it; others get nothing until it lets go
 * (otherwise two instances would both advance the music).
 */
namespace DoomAudio
{
    /** Creates the shared mixer. Call from a non-audio thread before any render. */
    void init();

    /** Adds DOOM's audio into left/right. Returns false if another instance owns the stream. */
    bool render (const void* owner, float* left, float* right, int numSamples, double sampleRate);

    /** Gives up ownership (call when the toggle goes off or the instance is destroyed). */
    void release (const void* owner);

    /** True when this instance currently owns the stream. */
    bool isOwner (const void* owner);

    /** True when a different instance owns the stream. */
    bool isOwnedByAnother (const void* owner);

    /** Set by the engine: music only advances while the game is ticking. */
    void setGameRunning (bool running);
}
