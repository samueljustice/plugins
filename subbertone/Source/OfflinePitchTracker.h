#pragma once

#include <atomic>
#include <functional>
#include <limits>
#include <vector>

/**
 * Offline fundamental tracker for LEARN: it works out the pitch of a captured
 * loop after the fact, on a background thread.
 *
 * PitchDetector lives on the audio thread and can only look back. This tracker
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
 *           of partials; summing those shifts gives a first contour, before
 *           any partial has been picked. That sum wanders on a steady note and
 *           carries every mis-measured shift to the end, so the contour is then
 *           found again from the set of partials itself: the clearest frame
 *           gives a pattern (then the average of all frames lined up on the
 *           contour), every frame is matched against it at every shift, and a
 *           Viterbi search picks the shifts, trusting the measured
 *           frame-to-frame shift where the two frames matched well and paying
 *           a fixed charge for a break (less after silence). The contour is
 *           slid to the offset where it overlaps the most whitened level while
 *           every frame of it stays inside lowestHz..highestHz, raised an octave
 *           at a time while the partial up there still scores climbShare of
 *           the strongest one (in exhausts the top-scoring partial is frequently
 *           half the firing rate, while the perceived pitch is the firing rate)
 *           and still fits in the range, and pulled onto the nearest ridge. The
 *           other partials are returned as lines at exact small fractions of the
 *           chosen one (1/2, 3/4, 3/2, 2 ...).
 *   Auto    Runs Tonal. A result without a step or a hole in it is returned as
 *           it is. Otherwise Engine runs too and the audio decides
 *           (Settings::autoCrossCheck). At each step of the Tonal line the
 *           partials around it are compared before and after: if they kept
 *           their places the sound changed pitch (a note); if they all moved by
 *           the step, the sound carried on and the line hopped to another
 *           partial. Undoing the hops gives a steadied Tonal line.
 *             - Only changes of pitch (a bass line, a melody), or notes too
 *               short to judge: the Tonal result as it stands.
 *             - Hops (an engine, a drone, a growl): the steadied line when it
 *               is whole, with gaps between its stretches filled along
 *               Engine's contour; Engine instead when it keeps one frequency
 *               ratio to that line (the same contour, and Engine comes with the
 *               other partials to choose from), or when the steadied line still
 *               has steps or holes in it and Engine's line can be trusted
 *               (Analysis::engineOnGrid); otherwise what the Tonal pass has.
 *             - No step to judge, but holes in the Tonal line: Engine when its
 *               line is on a partial nearly throughout, else Tonal.
 *
 * Engine's contour is measured against one pattern in every frame, so a jump
 * (a gear change, a lift-off, an edit, a loop seam) is a step in one line and
 * nothing after it is offset: Analysis::engineBreaks counts the steps. What it
 * cannot tell apart is a set of partials and the same set an octave away, so a
 * melody's octave leaps are not followed. Its line never leaves
 * lowestHz..highestHz. When the contour spans more than that range, or the line
 * is on a partial for too little of the sound, it is not to be trusted: read
 * Analysis::engineOnGrid and Analysis::engineConfidence. Engine gives every
 * audible frame a pitch, noise included. Sounds that are mostly noise with a
 * tone passing through (a pass-by at a distance, a cabin recording) are not what
 * either method is for.
 *
 * Silence is an absolute level (Settings::silenceDbfs; Learn passes the
 * Threshold knob less 10 dB), so what the analysis calls silent agrees with the
 * engine's level gate instead of depending on how loud the loop's peak is.
 * Silent frames carry no pitch, and gap filling never crosses them, so the
 * boundaries between notes survive. Runs of audible but unpitched frames
 * between pitched ones, no longer than Settings::bridgeSeconds, are filled in
 * log-frequency: a straight line up to 100 ms, beyond that a cubic that carries
 * on the slopes either side.
 *
 * Memory is dominated by Engine's spectrogram, which Settings::compactGrid keeps
 * in 8 bits: a 60 s loop at about 12 kHz (what Learn captures) is analysed as it
 * is and peaks near 18.5 MB (the contour search adds 2 MB); a buffer at a host
 * rate adds its decimated copy (21 MB).
 *
 * The result depends only on the samples, their rate and the settings: one
 * thread, no randomness, and every ordering decision has a tie-break, so the
 * same capture always analyses to the same curve.
 *
 * Plain C++20: no JUCE, no global state. It allocates (std::bad_alloc is passed
 * on) and takes seconds on long buffers, so call it from a background thread,
 * never from the audio thread.
 */
namespace subbertone::offline
{
    enum class Method
    {
        Auto,
        Engine,
        Tonal
    };

    /** The defaults are what Learn uses; it sets silenceDbfs from the Threshold knob. */
    struct Settings
    {
        Method method = Method::Auto;

        double lowestHz = 30.0;        // search range for the followed partial
        double highestHz = 1000.0;
        double hopSeconds = 0.005;     // output frame spacing
        double workingRate = 12000.0;  // analysis rate: faster buffers are decimated by a whole factor to about this

        // A frame is silent when the level around it (RMS over 20 ms, DC removed) is
        // below silenceDbfs, or more than -gateDb below the buffer's peak.
        double silenceDbfs = -50.0;                               // absolute (dBFS, like the Threshold knob)
        float gateDb = -std::numeric_limits<float>::infinity();   // relative to the peak; -inf = off

        // Tonal
        double moveCost = 12.0;           // Viterbi charge for each octave the pitch moves between adjacent frames
        double unpitchedCost = 1.0;       // Viterbi charge for declaring an audible frame unpitched
        double voicingChangeCost = 2.0;   // Viterbi charge for entering or leaving the unpitched state
        double longPeriodPenalty = 0.08;  // per octave of period length: tips ties towards the higher candidate
        bool thresholdVoting = false;     // score dips by the threshold votes they collect instead of their depth

        // Auto
        bool autoCrossCheck = true;        // let the audio decide between Engine and Tonal (see above); false: Engine whenever Tonal leaps
        double autoCrossCheckShare = 0.75; // share of the steadied Tonal line that must keep one frequency ratio to Engine for Engine to be preferred

        // Engine
        double climbShare = 0.75;  // step to the partial an octave higher while its score reaches this fraction of the strongest partial's
        int parallelLines = 6;     // how many alternative partials (lines at exact fractions of the chosen one) to return as well
        bool compactGrid = true;   // 8-bit spectrogram: a quarter of the memory (an engine run-up: curve moved <= 3 cents)

        // Finishing, both methods
        double bridgeSeconds = 0.15;         // longest audible unpitched run that gets filled in
        double maxSemitonesPerSecond = 0.0;  // 0 = off; else a slew limit that also lets any gap it can span be filled
    };

    struct Frame
    {
        float hz = 0.0f;          // 0 = unpitched or silent
        float confidence = 0.0f;  // 0..1, 0 on bridged frames
        bool bridged = false;     // filled in across a gap, not measured (frames Auto borrows from Engine's contour count as measured)
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
        Method usedMethod = Method::Tonal;  // Engine or Tonal, also when Auto was asked for (Tonal covers Auto's steadied Tonal line)
        double hopSeconds = 0.005;
        double durationSeconds = 0.0;

        Line line;                   // the chosen curve, finished (gaps bridged, slew limited)
        std::vector<Line> parallel;  // Engine: other partials, each `line` times an exact fraction (Line::ratio), strongest first
        Line other;                  // Auto that ran both methods: the curve of the one it did not use
        int tonalLeaps = -1;         // Auto: leaps of more than a quarter octave in the Tonal pass
        float octaveAgreement = -1;  // Auto that ran Engine: share of the steadied Tonal line's own frames where Engine keeps one
                                     // frequency ratio to it, within 35 cents (the name is from when only octaves counted).
                                     // -1 = not judged: Engine did not run, the two lines share no frame, or Auto took Engine
                                     // for another reason than this agreement (the steadied line was not whole, or Engine
                                     // filled the holes of a Tonal line without steps), so a low value never means a doubt there
        bool engineOnGrid = true;    // Engine: the line can be trusted: its contour fitted inside the search range and it sits
                                     // on a partial for at least 80 % of the sound (false = it did not lock, do not trust it)
        float engineConfidence = -1;    // Engine ran: share of audible frames where its line is on a partial, 0..1
        float enginePatternMatch = -1;  // Engine ran: how well the partial pattern was found along the line, 0..1
        int engineBreaks = -1;          // Engine ran: breaks (gear changes, lift-offs, a loop seam) its line stepped across
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
} // namespace subbertone::offline
