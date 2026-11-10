#pragma once

#include <atomic>
#include <functional>
#include <limits>
#include <vector>

/**
 * Hindsight fundamental tracker: it works out the pitch of a stretch of audio
 * after the fact, on a background thread (see HindsightGuide).
 *
 * The frame detectors decide each frame from that frame alone. This tracker
 * gets the complete buffer, so each frame is decided knowing what follows it:
 * through an engine run-up it can keep following the same partial instead of
 * hopping from order to order.
 *
 *   Tonal   YIN at a reduced sample rate. Each dip of the normalised
 *           difference is a candidate period; a Viterbi search then picks
 *           the cheapest sequence of candidates (or "unpitched") across all
 *           frames, charging for every octave the pitch moves per frame.
 *   Engine  For sounds whose partials all move in step (engine orders,
 *           drones). Neighbouring frames of a whitened log-frequency
 *           spectrogram are cross-correlated to measure the shift of the entire set
 *           of partials; summing those shifts gives a contour, before
 *           any partial has been picked. The contour is then slid to the
 *           offset where it overlaps the most whitened level, raised an octave
 *           at a time while the partial up there still scores climbShare of
 *           that (in exhausts the top-scoring partial is frequently a half
 *           order, while the perceived pitch is the firing rate), and pulled
 *           onto the nearest ridge.
 *   Auto    Runs Tonal; if that result steps by more than three semitones
 *           between adjacent frames anywhere, Engine runs too. Bass lines also
 *           move by fourths, fifths and octaves, so Settings::autoCrossCheck adds
 *           a test: the Engine result is used only if most of the Tonal frames
 *           lie a whole number of octaves from it (an engine's Tonal pass jumps
 *           between octaves of the firing rate) and it never left its analysis
 *           grid. Otherwise the Tonal result is returned.
 *
 * Engine adds up frame-to-frame shifts over the entire buffer, and one shift
 * can be at most 1/4 octave across 80 ms. A faster jump (a loop seam, an edit,
 * an abrupt gear change) therefore offsets everything after it: read
 * Analysis::engineOnGrid.
 *
 * Silence is an absolute level (Settings::silenceDbfs), so what the analysis
 * calls silent does not depend on how loud the buffer's peak is.
 * Silent frames carry no pitch, and gap filling never crosses them, so the
 * boundaries between notes survive. Runs of audible but unpitched frames
 * between pitched ones, no longer than Settings::bridgeSeconds, are filled in
 * log-frequency: a straight line up to 100 ms, beyond that a cubic that carries
 * on the slopes either side.
 *
 * Memory is dominated by Engine's spectrogram, which Settings::compactGrid keeps
 * in 8 bits: 60 s at about 12 kHz is analysed as it is and peaks near 16.5 MB;
 * a buffer at a higher rate adds its decimated copy (19 MB).
 *
 * The result depends only on the samples, their rate and the settings: one
 * thread, no randomness, and every ordering decision has a tie-break, so the
 * same buffer always analyses to the same curve.
 *
 * Plain C++20: no JUCE, no global state. It allocates (std::bad_alloc is passed
 * on) and takes seconds on long buffers, so call it from a background thread,
 * never from the audio thread.
 */
namespace pf::hindsight
{
    enum class Method
    {
        Auto,
        Engine,
        Tonal
    };

    /** Analysis settings. */
    struct Settings
    {
        Method method = Method::Auto;

        double lowestHz = 30.0;        // search range for the followed partial
        double highestHz = 1000.0;
        double hopSeconds = 0.005;     // output frame spacing
        double workingRate = 12000.0;  // analysis rate: faster buffers are decimated by a whole factor to about this

        // A frame is silent when the level around it (RMS over 20 ms, DC removed) is
        // below silenceDbfs, or more than -gateDb below the buffer's peak.
        double silenceDbfs = -50.0;                               // absolute (dBFS)
        float gateDb = -std::numeric_limits<float>::infinity();   // relative to the peak; -inf = off

        // Tonal
        double moveCost = 12.0;           // Viterbi charge for each octave the pitch moves between adjacent frames
        double unpitchedCost = 1.0;       // Viterbi charge for declaring an audible frame unpitched
        double voicingChangeCost = 2.0;   // Viterbi charge for entering or leaving the unpitched state
        double longPeriodPenalty = 0.08;  // per octave of period length: tips ties towards the higher candidate
        bool thresholdVoting = false;     // score dips by the threshold votes they collect instead of their depth

        // Auto
        bool autoCrossCheck = true;        // use Engine only when it agrees with the Tonal pass (see above)
        double autoCrossCheckShare = 0.6;  // fraction of Tonal frames that must lie a whole number of octaves from Engine

        // Engine
        double climbShare = 0.6;   // step to the partial an octave higher whenever its score reaches this fraction
        int parallelLines = 6;     // how many alternative partials (parallel curves) to return as well
        bool compactGrid = true;   // 8-bit spectrogram: a quarter of the memory (engine run-up: curve moved <= 3 cents)

        // Finishing, both methods
        double bridgeSeconds = 0.15;         // longest audible unpitched run that gets filled in
        double maxSemitonesPerSecond = 0.0;  // 0 = off; else a slew limit that also lets any gap it can span be filled
    };

    struct Frame
    {
        float hz = 0.0f;          // 0 = unpitched or silent
        float confidence = 0.0f;  // 0..1, 0 on bridged frames
        bool bridged = false;     // filled in across a gap, not measured
        bool silent = false;      // below the silence threshold: no pitch, and never bridged
    };

    struct Line
    {
        std::vector<Frame> frames;  // frame i is centred i * hopSeconds after the start of the buffer
        float score = 0.0f;         // Engine: mean whitened level along the line (log units); Tonal: 0
        float ratio = 1.0f;         // frequency ratio to the chosen line (1 for the chosen line)
    };

    struct Summary
    {
        float lowestHz = 0.0f;
        float highestHz = 0.0f;
        float logMeanHz = 0.0f;       // mean in log-frequency over pitched frames
        float pitchedShare = 0.0f;    // share of frames with a pitch (bridged ones included)
        float silentShare = 0.0f;     // share of frames below the silence threshold
        float bridgedShare = 0.0f;    // share of frames filled in across a gap
        float meanConfidence = 0.0f;  // over measured frames (pitched, not bridged)
        int jumps = 0;                // adjacent pitched frames more than a semitone apart
    };

    struct Analysis
    {
        Method usedMethod = Method::Tonal;  // Engine or Tonal, also when Auto was asked for
        double hopSeconds = 0.005;
        double durationSeconds = 0.0;

        Line line;                   // the chosen curve, finished (gaps bridged, slew limited)
        std::vector<Line> parallel;  // Engine: other lines parallel to `line`, strongest first
        Line other;                  // Auto that ran both methods: the curve of the one it did not use
        int tonalLeaps = -1;         // Auto: leaps of more than a quarter octave in the Tonal pass
        float octaveAgreement = -1;  // Auto that ran Engine: share of Tonal's pitched hops within 50 cents of an octave multiple of the Engine line
        bool engineOnGrid = true;    // Engine: the line stayed inside its analysis grid (false = it drifted, do not trust it)
        Summary summary;             // of `line`
        bool cancelled = false;
    };

    using Progress = std::function<void(float)>;  // 0..1, rising, called from the analysing thread

    /** Analyses a mono buffer at any sample rate. Returns early with `cancelled` set once `cancel` reads true. */
    Analysis analyse(const float* mono, int numSamples, double sampleRate, const Settings& settings = {},
                     const std::atomic<bool>* cancel = nullptr, const Progress& progress = {});

    /** Finishing steps, exposed so a view can change gap filling without analysing again. */
    void clearBridges(std::vector<Frame>& frames);
    void bridgeGaps(std::vector<Frame>& frames, double hopSeconds, double bridgeSeconds, double maxSemitonesPerSecond);
    void limitSlew(std::vector<Frame>& frames, double hopSeconds, double maxSemitonesPerSecond);
    Summary summarise(const std::vector<Frame>& frames);

    /** Pitch at a time (seconds from the buffer start), interpolated in log-frequency; 0 where unpitched. */
    float hzAt(const std::vector<Frame>& frames, double hopSeconds, double seconds) noexcept;
} // namespace pf::hindsight
