#pragma once

#include <JuceHeader.h>

#include <atomic>
#include <functional>

/**
 * Keeps a copy of the session on disk while the standalone app runs, so a crash or a power cut costs a minute
 * of work at most (the app itself only saves on a clean quit).
 *
 *  - Something was edited (a parameter, a track, a wire): the session is written about once a minute, and a few
 *    seconds after a track is loaded, edited or removed. Parameter moves the app makes by itself (the morph dot
 *    while Wander is on) are not edits, so a session that only plays is not written again and again.
 *  - The work happens on a background thread: the state embeds the audio (FLAC, encoded once per track and then
 *    reused), so a save can take a moment and must not stall the interface. The file is written beside its
 *    final name and swapped in, so a crash mid-save leaves the previous copy intact.
 *  - A clean quit calls finish(), which removes the file. If the file is there when the app opens, the last run
 *    did not end cleanly and recover() loads it.
 *
 * Everything except the save itself runs on the message thread; parameter changes may arrive on any thread
 * (they only set a flag).
 */
class SessionAutosave : private juce::Timer,
                        private juce::AudioProcessorListener,
                        private juce::ChangeListener,
                        private juce::Thread
{
public:
    struct Timing
    {
        int intervalMs = 60000;     // at most one save per this long while things keep changing
        int afterTrackMs = 3000;    // a track change is saved this soon (once the changes have settled)
    };

    /** tracks: told about loads, edits and removals (may be null). revision: a number that moves when something
        outside the parameters changed, polled once a second (may be empty). isEdit: given a parameter's index,
        whether a change of it right now is an edit to save (may be empty: every change is); called on whichever
        thread the change arrives on, so it must not lock or allocate. */
    SessionAutosave(juce::AudioProcessor& processor, juce::ChangeBroadcaster* tracks, std::function<int()> revision,
                    const juce::File& file, Timing timing, std::function<bool(int)> isEdit = {})
        : juce::Thread("Session autosave")
        , m_processor(processor)
        , m_tracks(tracks)
        , m_revision(std::move(revision))
        , m_isEdit(std::move(isEdit))
        , m_file(file)
        , m_timing(timing)
    {
        if (m_revision)
            m_lastRevision = m_revision();
        m_processor.addListener(this);
        if (m_tracks != nullptr)
            m_tracks->addChangeListener(this);
        startThread(juce::Thread::Priority::low);
        startTimer(1000);
    }

    SessionAutosave(juce::AudioProcessor& processor, juce::ChangeBroadcaster* tracks, std::function<int()> revision,
                    const juce::File& file)
        : SessionAutosave(processor, tracks, std::move(revision), file, Timing {})
    {
    }

    ~SessionAutosave() override { stop(); }

    /** Loads the copy a run that did not end cleanly left behind. False when there is none (the usual case) or it
        can't be read. The file is moved aside first, so a copy that can't be loaded is never tried twice. */
    static bool recover(juce::AudioProcessor& processor, const juce::File& file)
    {
        if (! file.existsAsFile())
            return false;
        const auto aside = file.getSiblingFile(file.getFileName() + ".loading");
        aside.deleteFile();
        if (! file.moveFileTo(aside))
            return false;
        juce::MemoryBlock data;
        const bool read = aside.loadFileAsData(data) && data.getSize() > 0 && data.getSize() < static_cast<size_t>(0x7fffffff);
        if (read)
            processor.setStateInformation(data.getData(), static_cast<int>(data.getSize()));
        aside.deleteFile();
        return read;
    }

    /** Something changed. Any thread. */
    void markChanged() { m_dirty.store(true); }
    /** A track was loaded, edited or removed: save soon rather than at the next minute. Message thread. */
    void noteTracksChanged()
    {
        m_dirty.store(true);
        m_trackChangeAt = juce::Time::getMillisecondCounter();
        m_trackChange = true;
    }

    /** The once-a-second check (the timer calls it; tests call it directly). Message thread. */
    void tick()
    {
        if (m_revision)
        {
            const int revision = m_revision();
            if (revision != m_lastRevision)
            {
                m_lastRevision = revision;
                m_dirty.store(true);
            }
        }
        if (! m_dirty.load() || m_saving.load())
            return;
        const auto now = juce::Time::getMillisecondCounter();
        const bool intervalUp = ! m_savedOnce || now - m_lastSaveAt >= static_cast<juce::uint32>(m_timing.intervalMs);
        const bool trackDue = m_trackChange && now - m_trackChangeAt >= static_cast<juce::uint32>(m_timing.afterTrackMs);
        if (! intervalUp && ! trackDue)
            return;
        m_dirty.store(false);
        m_trackChange = false;
        m_savedOnce = true;
        m_lastSaveAt = now;
        m_saving.store(true);
        notify(); // the thread does the work
    }

    /** Clean quit: the app has saved the session itself, so the copy is no longer needed. Message thread. */
    void finish()
    {
        stop();
        m_file.deleteFile();
    }

    bool isSaving() const { return m_saving.load(); }
    int getSaveCount() const { return m_saveCount.load(); }
    /** How long the last save took, in milliseconds (building the state and writing the file). */
    double getLastSaveMs() const { return m_lastSaveMs.load(); }
    juce::File getFile() const { return m_file; }

private:
    void stop()
    {
        stopTimer();
        if (! m_stopped)
        {
            m_stopped = true;
            m_processor.removeListener(this);
            if (m_tracks != nullptr)
                m_tracks->removeChangeListener(this);
        }
        stopThread(30000); // a save in progress finishes first (it never leaves half a file either way)
    }

    void timerCallback() override { tick(); }
    void audioProcessorParameterChanged(juce::AudioProcessor*, int index, float) override
    {
        if (! m_isEdit || m_isEdit(index))
            m_dirty.store(true);
    }
    void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails& details) override
    {
        // A new latency (the audio device changed) is not an edit; a loaded preset or program is.
        if (details.programChanged || details.nonParameterStateChanged || details.parameterInfoChanged)
            m_dirty.store(true);
    }
    void changeListenerCallback(juce::ChangeBroadcaster*) override { noteTracksChanged(); }

    void run() override
    {
        while (! threadShouldExit())
        {
            wait(-1);
            if (threadShouldExit())
                break;
            if (! m_saving.load())
                continue;

            const double start = juce::Time::getMillisecondCounterHiRes();
            juce::MemoryBlock data;
            m_processor.getStateInformation(data); // safe off the message thread (hosts save this way too)
            bool ok = false;
            if (data.getSize() > 0 && m_file.getParentDirectory().createDirectory())
            {
                juce::TemporaryFile temp(m_file);
                ok = temp.getFile().replaceWithData(data.getData(), data.getSize()) && temp.overwriteTargetFileWithTemporary();
            }
            m_lastSaveMs.store(juce::Time::getMillisecondCounterHiRes() - start);
            if (ok)
                m_saveCount.fetch_add(1);
            else
                m_dirty.store(true); // try again at the next interval
            m_saving.store(false);
        }
    }

    juce::AudioProcessor& m_processor;
    juce::ChangeBroadcaster* m_tracks = nullptr;
    std::function<int()> m_revision;
    std::function<bool(int)> m_isEdit;
    juce::File m_file;
    Timing m_timing;

    std::atomic<bool> m_dirty { false }, m_saving { false };
    std::atomic<int> m_saveCount { 0 };
    std::atomic<double> m_lastSaveMs { 0.0 };
    // Message thread
    int m_lastRevision = 0;
    bool m_savedOnce = false, m_trackChange = false, m_stopped = false;
    juce::uint32 m_lastSaveAt = 0, m_trackChangeAt = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SessionAutosave)
};
