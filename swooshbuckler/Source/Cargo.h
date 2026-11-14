#pragma once

#include <JuceHeader.h>
#include "DspUtils.h"

#include <array>
#include <atomic>
#include <memory>
#include <vector>

/**
 * The Cargo Hold: user sample tracks used as sources (pig squeals, cloth, a
 * door, anything), the way a sound designer throws any sound at all at a
 * whoosh template.
 *
 * Samples full of gaps are handled automatically:
 *  - On every edit the track is scanned for sound regions (gated RMS with
 *    hysteresis, short gaps merged, edges padded).
 *  - Regions keep their natural tails (detection follows the decay).
 *  - Playback (CargoPlayer) stays on one sound, looping it into itself with a
 *    long equal-power crossfade, and only moves on to the next sound when a new
 *    whoosh starts. The fade length follows the whoosh LENGTH, so the silence is
 *    skipped without any quick hopping. CHAOS shuffles which sound comes next.
 *  - A loudness trim (auto gain) computed from the regions levels every track.
 *
 * Stereo: every sound is played from its mono sum (what the doppler passes, the hits and the analysis use). A file
 * whose two sides differ also keeps their difference (CargoData::hasSide), which only a bus playing a wide bed
 * asks its player for.
 *
 * Threading: CargoHold lives on the message thread. Every edit builds a new
 * immutable CargoData and publishes a raw pointer through an atomic. Replaced
 * data is kept alive (undo history / a timed graveyard) long after the audio
 * thread could still be reading it.
 */
struct CargoRegion
{
    int start = 0, end = 0; // samples, end exclusive
    int focus = 0;          // the region's focus point (loudest moment / transient), start <= focus < end
};

/**
 * What a sample is, as far as whoosh design cares. Detected automatically
 * on load (SampleClassifier), can be overridden per track. The role decides how
 * the engine plays the track:
 *   Generic : loop the current sound, move to the next sound on each whoosh (v1 behaviour)
 *   Bed     : continuous energy bed; loops gracefully, cycles regions at their ends, ignores whooshes
 *   Tonal   : sweetener (animal, voice, creature, whistle); one-shot per whoosh, its focus point lands
 *             on the flight peak, then tails out to silence
 *   Impact  : hit / transient; one-shot per whoosh, transient lands exactly on the peak
 *   Air     : swish / cloth / wind gust; one-shot per whoosh, time-scaled (0.5x..2x) to the flight length,
 *             focus on the peak
 *   Crackle : granular texture (fire, debris); loops with random jumps (chaos)
 *   Rumble  : low end; loops like a bed, follows PITCH at half strength
 */
enum class CargoRole { Generic = 0, Bed, Tonal, Impact, Air, Crackle, Rumble };
inline constexpr int c_numCargoRoles = 7;

/**
 * How a sound moves over its length, for a track that leads the others (the Master track): its loudness, its pitch
 * and its brightness, one value every `hop` source samples, so each sound region's contour is its slice. Each is
 * 0..1 over the sound's own range, ready to turn a knob with. Made by SampleClassifier::extractContours on the
 * message thread, only for the tracks that need it (see CargoHold::setMaster).
 */
struct CargoContours
{
    int hop = 480;               // source samples per value (10 ms)
    std::vector<float> level;    // loudness: 0 = 34 dB or more under the sound's loudest moment .. 1 = the loudest
    std::vector<float> pitch;    // pitch: 0 = the sound's lowest .. 1 = its highest; held through unpitched moments
    std::vector<float> bright;   // brightness (spectral centroid): 0 = its dullest .. 1 = its brightest; held through silence
    float medianPitchHz = 0.0f;  // the pitch it sits at most of the time (0 = no clear pitch)
    float voiced = 0.0f;         // how much of the sound has a clear pitch (0..1)
    float pitchLowHz = 0.0f, pitchHighHz = 0.0f;   // what 0 and 1 of `pitch` stand for
    float brightLowHz = 0.0f, brightHighHz = 0.0f; // and of `bright`

    int size() const { return static_cast<int>(level.size()); }
    /** A contour at a source position (linear between values). */
    static float at(const std::vector<float>& contour, int hop, double sourcePosition)
    {
        if (contour.empty() || hop <= 0)
            return 0.0f;
        // Value k describes the samples k * hop .. (k + 1) * hop: it sits at their middle.
        const double x = std::max(0.0, sourcePosition / static_cast<double>(hop) - 0.5);
        const auto i = static_cast<size_t>(x);
        if (i + 1 >= contour.size())
            return contour.back();
        const auto f = static_cast<float>(x - static_cast<double>(i));
        return contour[i] + (contour[i + 1] - contour[i]) * f;
    }
};

struct CargoData
{
    /** Channel 0 is the mono sum: all that the regions, the analysis and every mono reader look at. A stereo source
        whose sides differ has a channel 1 as well: the side signal, (left - right) / 2, so left = mono + side and
        right = mono - side. It follows channel 0 through every edit and doubles the memory the sound takes. */
    juce::AudioBuffer<float> audio;
    bool hasSide() const { return audio.getNumChannels() > 1; }
    double sampleRate = 48000.0;
    std::vector<CargoRegion> regions;
    float autoGain = 1.0f;
    juce::String name;              // file name (or "Pasted")

    // Filled by SampleClassifier on load and kept across edits (makeEdited copies them).
    CargoRole detectedRole = CargoRole::Generic;
    float roleConfidence = 0.0f;    // 0..1
    juce::String autoLabel;         // e.g. "Pig Squeal" (from file name / features); empty = use name

    /** Length of the file this was loaded from, in seconds, when only its beginning fitted (0 = all of it is here).
        Set by loadFile only; an edit makes new data without it. Not saved with the session. */
    double sourceSeconds = 0.0;

    /**
     * Band-limited octave copies for playing faster than the source (pitch up, or a 96 kHz file in a 48 kHz
     * session): mips[k] is the audio low-passed and decimated by 2^(k+1), sample n sitting at source sample
     * n * 2^(k+1). Built by analyse() on the message thread, never saved (the session stores `audio` only).
     * Empty for data that was never analysed; the player then reads `audio` for every speed. An undo step the
     * engine has let go of gives its copies up (CargoHold::purgeRetired) and gets them back when it is used.
     */
    static constexpr int c_numMips = 4;
    std::array<std::vector<float>, c_numMips> mips;
    std::array<std::vector<float>, c_numMips> sideMips; // the same for the side signal (empty without one)

    /**
     * The contours (see CargoContours), made the first time something asks for them and kept for as long as this
     * data lives. Message thread: ensureContours(). Any thread: getContours() (null until they are made).
     */
    const CargoContours* getContours() const { return m_contours.get(); }
    const CargoContours& ensureContours() const;

    int length() const { return audio.getNumSamples(); }

    /** Fills regions, autoGain and the octave copies from the audio. */
    void analyse();
    void buildMips();
    static std::vector<CargoRegion> detectRegions(const float* data, int numSamples, double sampleRate);

private:
    /** Filled once, then only read. A copy of the data starts without (its audio may be about to change). */
    struct ContourSlot
    {
        ContourSlot() = default;
        ContourSlot(const ContourSlot&) {}
        ContourSlot& operator=(const ContourSlot&) { return *this; }
        ~ContourSlot() { delete m_made.load(); }
        const CargoContours* get() const { return m_made.load(std::memory_order_acquire); }
        std::atomic<const CargoContours*> m_made { nullptr };
    };
    mutable ContourSlot m_contours;
};

//==============================================================================
class CargoHold : public juce::ChangeBroadcaster, private juce::Timer
{
public:
    static constexpr int c_numTracks = 10;  // a 10-20 track layering template
    static constexpr int c_numBuiltIns = 6; // built-in synth lanes: air roar flutter sub tone input
    static constexpr int c_numLanes = c_numBuiltIns + c_numTracks; // lane index == engine layer index

    CargoHold();
    ~CargoHold() override;

    /** Audio thread: current data of a track (may be null). Valid for the rest of the block. */
    const CargoData* getLive(int track) const { return m_tracks[static_cast<size_t>(track)].live.load(std::memory_order_acquire); }
    bool getSkipSilence(int track) const { return m_tracks[static_cast<size_t>(track)].skipSilence.load(); }

    /**
     * Audio thread: the data a player still reads (its current data and a pending switch target). Retired
     * data is only freed once it is old AND no player marks it any more, so a paused / bypassed processBlock can
     * never resume on freed data.
     */
    void markInUse(int track, const CargoData* current, const CargoData* pending);

    /** Message thread: frees retired data that is at least minAgeMs old and that no player marks any more (the
        timer calls this). Returns how many retired entries are still kept. */
    int purgeRetired(juce::uint32 minAgeMs);

    // Mixer state for every lane on the timeline (lane 0..5 built-ins, 6..11 sample tracks).
    // Safe from any thread. laneGain() = 0 when muted, or when any lane is soloed and this one isn't.
    float laneGain(int lane) const;
    bool getMute(int lane) const { return m_mute[static_cast<size_t>(lane)].load(); }
    bool getSolo(int lane) const { return m_solo[static_cast<size_t>(lane)].load(); }
    void setMute(int lane, bool on);   // message thread (sends a change message)
    void setSolo(int lane, bool on);   // message thread (sends a change message)

    // Role. getRole() is the override if set, else the detected role. Audio-thread safe.
    CargoRole getRole(int track) const;
    int getRoleOverride(int track) const { return m_tracks[static_cast<size_t>(track)].roleOverride.load(); } // -1 = auto
    void setRoleOverride(int track, int roleOrMinusOne);   // message thread

    static constexpr int c_maxBuses = 2; // rig buses A and B
    static constexpr int c_hitsBus = 2;  // "bus" index meaning: the hit gate
    static constexpr int c_stowBus = 3;  // "bus" index meaning: Stowaway, the extra bus beside A and B
    /** A place a track can be sent by hand: bus A, bus B, the hits or Stowaway. */
    static constexpr bool isBusChoice(int bus) { return bus >= 0 && bus <= c_stowBus; }

    /** Where a track plays: the place chosen for it by hand, else the one its type picks (see busForRole).
        Any thread. */
    int getBus(int track) const;
    /** Where a type plays on Auto: impacts -> hits, beds and air -> bus B, everything else -> bus A; and with
        rumbleToStowaway, rumbles -> Stowaway. */
    static int busForRole(CargoRole role, bool rumbleToStowaway);
    /** The same, as this hold routes right now (see setRumbleToStowaway). Any thread. */
    int autoBus(CargoRole role) const { return busForRole(role, m_rumbleToStowaway.load(std::memory_order_relaxed)); }
    /** Rumble-type sounds on Auto play on Stowaway (on: Stowaway is switched on and in its Sub mode) or on bus A. Any thread: it
        only stores a flag, and tells no listener. */
    void setRumbleToStowaway(bool on) { m_rumbleToStowaway.store(on, std::memory_order_relaxed); }
    bool getRumbleToStowaway() const { return m_rumbleToStowaway.load(std::memory_order_relaxed); }
    /** The place chosen by hand: -1 = Auto (by type), 0 = bus A, 1 = bus B, c_hitsBus = the hits, c_stowBus =
        Stowaway. The type and the track's name stay as they are. Any thread. */
    int getBusOverride(int track) const { return m_tracks[static_cast<size_t>(track)].busOverride.load(std::memory_order_relaxed); }
    void setBusOverride(int track, int busOrMinusOne);     // message thread

    /**
     * The Master track: the one sound the others can be made to follow (-1 = none; only a loaded track can be
     * it). Its loudness, pitch and brightness contours are worked out when it is chosen, and again after every
     * edit to it, so the engine can read them as it plays. Any thread reads; the message thread sets.
     */
    int getMaster() const { return m_master.load(std::memory_order_relaxed); }
    void setMaster(int trackOrMinusOne);
    /** The pitch a track's sound sits at most of the time, in Hz, or 0 when it has no clear pitch (or no sound).
        Works the track's contours out if it has none yet (a long file takes a moment). Message thread. */
    float getMedianPitchHz(int track) const;

    /**
     * The morph's ropes: the track each side of the morph listens to (side 0 = A, 1 = B; -1 = that side's whole
     * bus). The two sides never hold the same track. Any thread reads; the message thread sets.
     */
    int getMorphSource(int side) const { return m_morphSource[static_cast<size_t>(side & 1)].load(std::memory_order_relaxed); }
    void setMorphSource(int side, int trackOrMinusOne);

    /** Start marker per track (0..1 of its length): playback starts here and loops back here. Any thread. */
    float getStart(int track) const { return m_start[static_cast<size_t>(track)].load(); }
    void setStart(int track, float fraction);            // message thread

    // Display name: user rename if set, else autoLabel, else file name, else "Track N".
    juce::String getDisplayName(int track) const;
    /** "Tonal Content", "Energy Bed", "Impact", "Air / Swish", "Crackle", "Rumble", "Mixed Content". */
    static juce::String typeName(CargoRole role);
    void rename(int track, const juce::String& name);       // empty = back to automatic

    /** First track index without audio, or -1. */
    int firstEmptyTrack() const;

    // Message thread ---------------------------------------------------------
    std::shared_ptr<const CargoData> get(int track) const { return m_tracks[static_cast<size_t>(track)].current; }
    void setSkipSilence(int track, bool skip);

    bool loadFile(int track, const juce::File& file, juce::String& error);
    /** Loads audio made in memory (the tools' and tests' demo sounds) into a track, exactly as a file of that name would be: analysed,
        typed, routed, and one undo step. One channel, or two (left and right). Message thread. */
    bool loadBuffer(int track, const juce::AudioBuffer<float>& audio, double sampleRate, const juce::String& name);

    /** The longest file that is loaded whole; a longer one keeps its beginning (see CargoData::sourceSeconds). */
    static constexpr double c_maxLoadSeconds = 600.0;

    /** "Loaded the first 10:00 of 23:41" when only the beginning of the track's file fitted, else empty. */
    juce::String getLoadNote(int track) const;

    /** What a batch load did, for the status line. */
    struct LoadReport
    {
        std::vector<int> tracks;        // the tracks that were filled, in order
        juce::StringArray names;        // their display names
        juce::StringArray failed;       // file names that could not be loaded
        juce::StringArray reasons;      // why, parallel to `failed` (a full sentence each)
        juce::StringArray notes;        // one per file that was cut short ("Loaded the first 10:00 of 23:41")
        int noRoom = 0;                 // files left out because every track is in use

        bool hasProblems() const { return noRoom > 0 || ! failed.isEmpty(); }
        /** One plain line: "Added 10. 2 didn't fit (ten tracks at most)." / "Couldn't read x.mp4 ..." /
            "Loaded the first 10:00 of 23:41". Empty when nothing happened. */
        juce::String summary() const;
    };

    /** Loads files into `firstTrack` (replacing it; -1 = none) and then into the empty tracks, one file each.
        Files that fail, and files there is no track left for, are collected in the report, never dropped. */
    LoadReport loadFiles(const juce::StringArray& paths, int firstTrack = -1);

    /** Any channel layout (mono .. 7.1.4) to the mono carrier: BS.775 stereo downmix, gain safe, phase safe. */
    static juce::AudioBuffer<float> foldToMono(const juce::AudioBuffer<float>& multi, const juce::AudioChannelSet& layout);
    /** The same carrier in channel 0 and, when the stereo downmix has two sides that differ, their difference in
        channel 1 (see CargoData::audio). One channel for a mono source, for two sides that are the same (under
        -60 dB of difference) and for sides that cancel (the carrier is then the louder side alone). */
    static juce::AudioBuffer<float> foldToMidSide(const juce::AudioBuffer<float>& multi, const juce::AudioChannelSet& layout);
    void clear(int track);

    // Editing. Ranges are in samples; all operations are undoable.
    void cut(int track, int start, int end);
    void copy(int track, int start, int end);
    void paste(int track, int position);
    void duplicate(int track, int start, int end);
    void trim(int track, int start, int end);
    void erase(int track, int start, int end);
    void reverse(int track, int start, int end);
    void normalise(int track, int start, int end);
    void fade(int track, int start, int end, bool fadeIn);
    void undo(int track);
    void redo(int track);
    bool canUndo(int track) const { return ! m_tracks[static_cast<size_t>(track)].undo.empty(); }
    bool canRedo(int track) const { return ! m_tracks[static_cast<size_t>(track)].redo.empty(); }
    /** Undo / redo across every track, in the order the edits were made (what Cmd+Z and Shift+Cmd+Z do).
        Return the track they acted on, or -1 when there is nothing to undo / redo. */
    int undoLatest();
    int redoLatest();
    bool canUndoLatest() const;
    /** Called (message thread) each time an edit that can be undone has been made, with its track: the owner's
        undo history puts it in line with everything else that was changed. Not called by undo or redo. */
    std::function<void(int)> onUndoableEdit;
    bool canRedoLatest() const;
    /** How much audio the undo and redo steps of every track hold, in bytes, and the most they may hold (the
        oldest edits are forgotten first; the newest one always stays). Message thread. */
    std::int64_t getUndoBytes() const;
    static constexpr std::int64_t c_undoByteLimit = 512ll * 1024 * 1024;
    void setUndoByteLimit(std::int64_t bytes) { m_undoByteLimit = bytes; }
    bool hasClipboard() const { return m_clipboard.getNumSamples() > 0; }

    juce::AudioFormatManager& getFormatManager() { return m_formats; }

    // Playback positions for the UI (0..1 of the track length, < 0 = idle).
    std::array<std::array<std::atomic<float>, 2>, c_numTracks> playheads;

    /** Safe from any thread (hosts save from background threads): snapshots each track under a lock. */
    juce::ValueTree toValueTree() const;
    /** Message thread. Replaces EVERY track (tracks missing from the tree are cleared) and drops undo history. */
    void fromValueTree(const juce::ValueTree& tree);

    /** Message thread. Makes this hold the same as `other` without copying any audio: both hold the same
        (immutable) sample data, and this one gets its own start markers, types, names and mixer state to change
        as it likes. For the offline renderer's second rig. Call other.beginLending() first and other.endLending()
        once this hold has gone (or been emptied). */
    void shareFrom(const CargoHold& other);
    /** While something is lent out, sample data this hold has replaced keeps its octave copies (the borrower may
        still be reading them); it gives them up as usual after endLending(). Message thread; calls nest. */
    void beginLending() { ++m_lending; }
    void endLending() { m_lending = juce::jmax(0, m_lending - 1); }

private:
    /** Name / role / start marker saved with undo entries of actions that reset them (load, clear). */
    struct Meta
    {
        bool valid = false;
        int roleOverride = -1;
        int busOverride = -1;
        bool master = false;      // it was the Master track
        int ropes = 0;            // bit 0: the morph's side A was roped to it, bit 1: side B
        juce::String userName;
        float start = 0.0f;
    };

    struct Track
    {
        std::shared_ptr<const CargoData> current;
        std::vector<std::shared_ptr<const CargoData>> undo, redo;
        std::vector<Meta> undoMeta, redoMeta;   // parallel to undo / redo
        std::atomic<const CargoData*> live { nullptr };
        std::atomic<bool> skipSilence { true };
        std::atomic<int> roleOverride { -1 };   //
        std::atomic<int> busOverride { -1 };    // -1 = by type
        juce::String userName;                  // message thread only
    };

    void install(int track, const juce::AudioBuffer<float>& multi, const juce::AudioChannelSet& layout, double sampleRate,
                 const juce::String& name, double sourceSeconds);   // what loadFile and loadBuffer share
    void publish(int track, std::shared_ptr<CargoData> data, bool recordUndo);
    void publish(int track, std::shared_ptr<CargoData> data, bool recordUndo, const Meta& undoMeta); //
    Meta metaOf(const Track& t) const;                                                              //
    void applyMeta(Track& t, const Meta& m);                                                        //
    bool isInUse(const CargoData* data) const;                                                      //
    std::shared_ptr<CargoData> makeEdited(int track, const juce::AudioBuffer<float>& audio) const;
    void retire(std::shared_ptr<const CargoData> data);
    bool isLive(const CargoData* data) const;
    static void dropMips(const std::shared_ptr<const CargoData>& data);     // see purgeRetired
    static void restoreMips(const std::shared_ptr<const CargoData>& data);  //
    static std::int64_t bytesOf(const CargoData* data);
    void trimUndoToBytes();
    void timerCallback() override;
    bool validRange(int track, int& start, int& end) const;

    std::array<Track, c_numTracks> m_tracks;
    // Which track each undoable edit touched, oldest first (message thread). An edit pushes its track to
    // m_undoOrder and empties m_redoOrder; undoLatest / redoLatest move tracks between the two. Entries whose
    // track has nothing left to undo (history trimmed at c_maxUndo) are skipped.
    std::vector<int> m_undoOrder, m_redoOrder;
    std::int64_t m_undoByteLimit = c_undoByteLimit;
    std::array<std::atomic<bool>, c_numLanes> m_mute {}, m_solo {};   //
    std::atomic<int> m_soloCount { 0 };
    std::atomic<bool> m_rumbleToStowaway { false };                   // see setRumbleToStowaway (Stowaway starts switched off)
    std::atomic<int> m_master { -1 };                                 // see setMaster
    std::array<std::atomic<int>, 2> m_morphSource { { { -1 }, { -1 } } }; // see setMorphSource
    /** A track that is cleared stops being the Master and lets go of its ropes (undo brings them back). */
    void dropLinks(int track);
    std::array<std::atomic<float>, c_numTracks> m_start {};
    juce::AudioBuffer<float> m_clipboard;
    double m_clipboardRate = 48000.0;                                  //
    juce::AudioFormatManager m_formats;

    struct Retired
    {
        std::shared_ptr<const CargoData> data;
        juce::uint32 time = 0;
    };
    std::vector<Retired> m_graveyard;
    int m_lending = 0; // see beginLending

    // Pointers the audio thread still reads (see markInUse).
    std::array<std::array<std::atomic<const CargoData*>, 2>, c_numTracks> m_inUse {};
    // Guards Track::current / undo / redo / userName against toValueTree on a host save thread.
    mutable juce::CriticalSection m_lock;
    // Session encoding cache (FLAC is only re-encoded when a track's data changed).
    struct Encoded
    {
        std::weak_ptr<const CargoData> source;
        juce::MemoryBlock block;
        bool raw = false;
        double gain = 1.0;   // FLAC stores audio / gain when the peak is over full scale
        juce::MemoryBlock sideBlock; // the side signal, the same way (empty without one)
        bool sideRaw = false;
        double sideGain = 1.0;
    };
    mutable std::array<Encoded, c_numTracks> m_encoded;
    mutable juce::CriticalSection m_encodeLock;
};

//==============================================================================
/** Sent to the players when a whoosh starts. */
/**
 * Hit sync's shared beat: the longest lead-in to a transient and the longest tail after it, over every region
 * every hit track can fire (as the players will play them, so skip silence and start markers count).
 * Source seconds; the engine divides by the pitch ratio. The engine and the track view both use this.
 */
struct HitWindow
{
    double pre = 0.01, post = 0.2;
    void extend(const CargoData& d, bool skipSilence, float startFraction);
};

struct CargoCue
{
    bool active = false;        // a whoosh started in this chunk
    double secondsToPeak = 0.0; // how long until that whoosh's peak is heard (attack minus doppler delay at the peak)
    double flightSeconds = 0.0; // total whoosh length (Air time-scales its region to this)
};

/** Plays one cargo track as a carrier (loops) or as accents aligned to whoosh peaks. Audio thread only. */
class CargoPlayer
{
public:
    void prepare(double sampleRate);
    void reset();

    /**
     * Renders frames samples of the track into out (overwrites; zeros if empty / silent).
     *  role        : how to play (see CargoRole). Generic / Bed / Crackle / Rumble loop; Tonal / Impact / Air are
     *                one-shots per whoosh, silent between whooshes.
     *  rate        : playback speed multiplier (pitch).
     *  fadeSeconds : crossfade / tail length (tied to the whoosh length by the engine).
     *  cue         : set on the chunk in which a new whoosh started (see CargoCue).
     *  sideOut     : null, or where the side signal of what `out` plays goes (overwritten; zeros when the sound
     *                has none): the same heads, fades and gain, so out + sideOut is the left and out - sideOut
     *                the right of the source. `out` is the same whether it is asked for or not.
     */
    void render(const CargoData* data, CargoRole role, bool skipSilence, float rate, float chaos, sj::Random& rng,
                float fadeSeconds, const CargoCue& cue, float* out, int frames, float* sideOut = nullptr);

    float getPlayhead(int head) const;

    /** Where in the track playback starts (0..1 of its length); loops return here. Audio thread. */
    void setStart(float fraction) { m_startFraction = sj::clamp(fraction, 0.0f, 0.98f); }

    /** The sound regions a player uses, as it will play them: with skip silence each detected region (from the
        start marker on, the first one clipped to it), without it the whole file from the start marker as ONE region
        whose focus is the first transient after the marker. Any thread (pure function of the data). */
    static int countRegions(const CargoData& d, bool skipSilence, float startFraction);
    static CargoRegion regionAt(const CargoData& d, bool skipSilence, float startFraction, int index);
    /** Index into d.regions of the first sound reaching past the start marker. */
    static int firstRegion(const CargoData& d, float startFraction);

    /** Index (into regionAt) of the region of the newest one-shot, -1 = none yet. */
    int getShotRegion() const { return m_shotRegion; }

    /** The data this player still reads (for CargoHold::markInUse). */
    const CargoData* getData() const { return m_data; }
    const CargoData* getPending() const { return m_switching ? m_pending : nullptr; }

private:
    struct Head
    {
        double pos = 0.0;
        int region = 0;
        bool active = false;
    };

    void startHead(Head& h, const CargoData& d, bool skipSilence, float chaos, sj::Random& rng, bool first, bool advance);

    /**
     * Band-limited read. A voice (the loop, each one-shot) keeps a MipState: the octave copy it reads from follows
     * its speed, and when that changes the old and new copies are crossfaded over the start of the chunk.
     */
    struct MipState
    {
        int level = -1;   // -1 = not chosen yet
    };
    struct MipRead
    {
        int level = 0, from = 0;   // this chunk's level, and the level being faded out (== level when steady)
        int fade = 0;              // samples of crossfade at the start of the chunk
        double inc = 1.0;          // source samples (level 0) per output sample
    };
    static MipRead beginRead(MipState& state, const CargoData& d, double inc, int frames);
    static float readAt(const CargoData& d, const MipRead& r, double pos, int sampleInChunk);
    static float readSideAt(const CargoData& d, const MipRead& r, double pos, int sampleInChunk);
    static float readSideBandLimited(const CargoData& d, int level, double pos, double inc);

public:
    /** One band-limited sample of `d` at source position `pos`, for a read speed of `inc` source samples per
        output sample, from octave copy `level` (0 = the audio itself). Any thread. */
    static float readBandLimited(const CargoData& d, int level, double pos, double inc);
    /** The octave copy a steady read at this speed uses. */
    static int levelFor(const CargoData& d, double inc);

private:
    CargoRegion regionFor(const CargoData& d, bool skipSilence, int index) const;
    int numRegions(const CargoData& d, bool skipSilence) const;

    double m_sampleRate = 48000.0;
    const CargoData* m_data = nullptr;
    std::array<Head, 2> m_heads;
    int m_current = 0;
    int m_xfadeTotal = 1, m_xfadeLeft = 0;
    bool m_advancePending = false;
    float m_switchGain = 1.0f;       // fade used when the data pointer changes
    const CargoData* m_pending = nullptr;
    bool m_switching = false;
    float m_lastPlayhead[2] { -1.0f, -1.0f };
    float m_startFraction = 0.0f;

    // One-shot voices (Tonal / Impact / Air) and loop-role helpers. Private, added by the engine owner.
    struct Shot
    {
        int state = 0;           // 0 idle, 1 waiting for its start, 2 playing
        int wait = 0;            // output samples until the start (state 1)
        double pos = 0.0;        // source position
        int end = 0;             // region end (source samples)
        float rateScale = 1.0f;  // Air time-scaling
        float level = 0.0f;      // 0..1 (smoothstepped on output)
        float step = 0.0f;       // per-sample level change (+ fade-in, - fade-out)
        bool fadingOut = false;
        int age = 0;             // cue counter when started (oldest is stolen first)
        float tail = 1.0f;       // output samples of the fade to silence before the region end
        MipState mip;            // which octave copy it reads
    };
    static constexpr int c_numShots = 4;

    void renderLoop(const CargoData& d, CargoRole role, bool skipSilence, float rate, float chaos, sj::Random& rng,
                    float fadeSeconds, bool cue, float* out, int frames, float* sideOut);
    void renderShots(const CargoData& d, CargoRole role, bool skipSilence, float rate, float chaos, sj::Random& rng,
                     float fadeSeconds, const CargoCue& cue, bool acceptCue, float* out, int frames, float* sideOut);
    void cueShot(const CargoData& d, CargoRole role, bool skipSilence, float rate, float chaos, sj::Random& rng,
                 float fadeSeconds, const CargoCue& cue);
    void resetShots();
    void fadeOlderShots(int age, float fadeOutSamples); //

    std::array<Shot, c_numShots> m_shots;
    int m_shotRegion = -1;      // last region used by a one-shot
    int m_shotCounter = 0;
    float m_loopLevel = 0.0f;   // crossfade between the loop and one-shot behaviours when the role changes
    int m_jumpLeft = 0;         // Crackle: samples until the next random jump
    int m_lastSkip = -1;        // skipSilence seen last block (-1 = unknown): a change re-seats the loop heads
    MipState m_loopMip;         // which octave copy the loop heads read
    void reseatHeads(const CargoData& d, bool skipSilence); //
};
