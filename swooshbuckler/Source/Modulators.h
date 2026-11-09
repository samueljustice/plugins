#pragma once

#include <JuceHeader.h>

#include <array>
#include <atomic>
#include <mutex>
#include <vector>

/**
 * Modulation in the spirit of MegaMod / Bitwig modulators: four modulators at the
 * bottom of the window, each wired to any knob by dragging its jack onto it.
 *
 *   LFO      : sine, triangle, ramp, square or steps (sample and hold), bipolar
 *   Random   : a smooth random walk, bipolar
 *   Follower : follows the loudness of bus A, bus B, the hits or the output, unipolar
 *   Macro    : a knob you turn (or automate), unipolar
 *
 * A wire adds depth x modulator to the knob's own (normalised) value. The parameter
 * itself is never written, so host automation stays clean; the processor applies
 * the offsets when it hands values to the engine.
 *
 * Threading: wires live in fixed slots of atomics (audio thread reads, message
 * thread writes); the message thread keeps the parameter IDs for saving and the UI.
 */
class ModMatrix
{
public:
    static constexpr int c_numSources = 4;
    static constexpr int c_maxRoutes = 48;

    enum class Type { Lfo = 0, Random, Follower, Macro };
    enum class Shape { Sine = 0, Triangle, Ramp, Square, Steps };

    /** The parameters a wire can reach (every knob outside the morph pad), in a fixed order. */
    static const std::vector<juce::String>& targets();
    static int targetIndex(const juce::String& paramId);

    struct Route
    {
        int slot = -1;
        int source = 0;
        juce::String param;
        float depth = 0.5f; // -1..1
    };

    // Message thread ---------------------------------------------------------
    /** Wires source to param (an existing wire between the two is kept and returned). Returns the slot, or -1 when
        the matrix is full or the parameter can't be modulated. */
    int connect(int source, const juce::String& paramId, float depth = 0.5f);
    /** expectedGeneration (from generation()) guards against a restore having renumbered the slots meanwhile. */
    void disconnect(int slot, int expectedGeneration = -1);
    /** Doesn't call onChange (it fires on every mouse move of a depth drag); the caller updates its own view. */
    bool setDepth(int slot, float depth, int expectedGeneration = -1);
    /** Bumped whenever slots are renumbered (restore, clear). */
    int generation() const { return m_generation.load(); }
    void clearSource(int source);
    void clear();
    std::vector<Route> getRoutes() const;
    int numRoutes(int source) const;

    juce::ValueTree toValueTree() const;          // any thread
    void fromValueTree(const juce::ValueTree&);   // any thread (notifies only when called on the message thread)
    void notifyChanged() { changed(); }           // message thread

    /** Called on every change (message thread), e.g. to redraw the wires. */
    std::function<void()> onChange;

    // Audio thread -----------------------------------------------------------
    struct SourceSettings
    {
        Type type = Type::Lfo;
        Shape shape = Shape::Sine;
        float knob = 0.4f; // LFO / Random: rate; Follower: speed; Macro: the value
        int follow = 0;    // Follower: which input (an index into FollowerInputs)
    };

    /** What a Follower can listen to: RMS of the last block of each. */
    enum FollowerInput { FollowBusA = 0, FollowBusB, FollowHits, FollowOutput, c_numFollowerInputs };
    using FollowerInputs = std::array<float, c_numFollowerInputs>;

    void prepare(double sampleRate);
    void reset();
    /** Advances the modulators by numSamples and sums the offsets. */
    void process(const std::array<SourceSettings, c_numSources>& settings, int numSamples, const FollowerInputs& followerInputs);
    /** Normalised offset for a target (sum of its wires, smoothed). 0 when nothing is wired to it. */
    float offset(int target) const { return m_offsets[static_cast<size_t>(target)]; }

    // Any thread (UI views) ----------------------------------------------------
    /** Current value of a source: -1..1 for LFO / Random, 0..1 for Follower / Macro. */
    float getSourceValue(int source) const { return m_sourceView[static_cast<size_t>(source)].load(std::memory_order_relaxed); }
    /** Audio thread: does any wire reach this target (as of the last process call)? */
    bool isWired(int target) const { return m_wired[static_cast<size_t>(target)]; }
    bool isBipolar(int source) const { return m_bipolarView[static_cast<size_t>(source)].load(std::memory_order_relaxed); }
    /** The modulated value (0..1) a target is at right now, or < 0 if nothing is wired to it. */
    float getModulatedView(int target) const { return m_targetView[static_cast<size_t>(target)].load(std::memory_order_relaxed); }
    void publishTarget(int target, float normalised) { m_targetView[static_cast<size_t>(target)].store(normalised, std::memory_order_relaxed); }

    static float rateHz(float knob) { return 0.05f * std::pow(160.0f, juce::jlimit(0.0f, 1.0f, knob)); } // 0.05 .. 8 Hz
    /** Follower release time (its attack is 0.3x this): 2 s .. 26 ms (faster is finer than it updates). */
    static float followerSeconds(float knob) { return 2.0f * std::pow(0.0133f, juce::jlimit(0.0f, 1.0f, knob)); }
    static juce::String typeName(Type t);
    static juce::String shapeName(Shape s);

private:
    struct Slot
    {
        std::atomic<int> target { -1 }; // -1 = free
        std::atomic<int> source { 0 };
        std::atomic<float> depth { 0.0f };
    };
    std::array<Slot, c_maxRoutes> m_slots;

    std::atomic<int> m_generation { 0 };
    mutable std::mutex m_lock;                      // guards m_params (message thread / state save)
    std::array<juce::String, c_maxRoutes> m_params; // param ID per slot ("" = free)
    void changed();

    double m_sampleRate = 48000.0;
    struct State
    {
        double phase = 0.0;
        float held = 0.0f;                  // Steps
        float from = 0.0f, to = 0.0f;       // Random
        float env = 0.0f;                   // Follower
        float value = 0.0f;
    };
    std::array<State, c_numSources> m_state;
    juce::Random m_rng { 0x5eed };
    std::vector<float> m_offsets = std::vector<float>(64, 0.0f);
    std::vector<float> m_targetOffsets = std::vector<float>(64, 0.0f);
    std::array<bool, 64> m_wired {};
    std::array<std::atomic<float>, c_numSources> m_sourceView {};
    std::array<std::atomic<bool>, c_numSources> m_bipolarView {};
    std::array<std::atomic<float>, 64> m_targetView {};
};
