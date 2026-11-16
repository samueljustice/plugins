#pragma once

// The diagnostic log: what the host does, written to a text file so a session can be read
// afterwards: ~/Library/Logs/SammyJ/PitchFlattener.log on macOS, %APPDATA%\SammyJ\Logs\PitchFlattener.log
// on Windows (started afresh, keeping one older copy, once it passes 4 MB). Any thread (the audio
// thread included) posts events into a bounded lock-free queue; a background thread formats them
// and appends them to the file.
//
// The audio thread must not format text (printf of floating point may allocate), so its events
// carry raw numbers and a formatting function that runs later, on the background thread.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <condition_variable>

namespace pitchflattener
{

enum class ThreadKind : std::uint8_t { Message, Audio, Timer, Other };
const char* threadKindName(ThreadKind kind) noexcept;

struct LogEvent
{
    using FormatFn = void (*)(const LogEvent& event, char* out, std::size_t outSize);

    std::int64_t wallUs = 0;          // stamped by post()
    std::uint32_t instance = 0;       // 0 = the process, not one instance
    ThreadKind thread = ThreadKind::Other;
    FormatFn format = nullptr;        // null: text holds the message
    std::uint32_t flags = 0;          // for format: which fields are present, booleans
    std::int64_t i[16] {};
    double d[8] {};
    char text[384] {};
};

class DiagnosticLog
{
public:
    static DiagnosticLog& instance();

    // Each processor retains the log for its lifetime; the writer thread runs while any does.
    void retain();
    void release();

    // Off the audio thread: printf-style text.
    void post(std::uint32_t instanceId, ThreadKind thread, const char* fmt, ...) noexcept
#if defined(__GNUC__) || defined(__clang__)
        __attribute__((format(printf, 4, 5)))
#endif
        ;

    // Any thread, the audio thread included: never allocates, never blocks. False if the queue
    // was full (the event is counted as dropped and reported in the log).
    bool postEvent(LogEvent& event) noexcept;

    // Writes everything queued so far (tests; the writer thread does this every 100 ms).
    void flush();

    void setFileForTesting(const std::string& path);
    std::string filePath() const;
    static std::string defaultFilePath();

    std::uint64_t droppedEvents() const noexcept { return m_dropped.load(std::memory_order_relaxed); }

private:
    DiagnosticLog();
    ~DiagnosticLog();

    static constexpr std::size_t kQueueSize = 2048;   // power of two
    struct Cell
    {
        std::atomic<std::size_t> sequence { 0 };
        LogEvent event;
    };

    bool pop(LogEvent& out) noexcept;
    void drain();
    void writerLoop();

    std::unique_ptr<Cell[]> m_cells;
    std::atomic<std::size_t> m_enqueuePos { 0 };
    std::size_t m_dequeuePos = 0;                      // guarded by m_drainMutex
    std::atomic<std::uint64_t> m_dropped { 0 };
    std::uint64_t m_droppedReported = 0;

    std::mutex m_drainMutex;
    mutable std::mutex m_pathMutex;
    std::string m_path;
    bool m_rotated = false;

    std::mutex m_threadMutex;
    std::condition_variable m_wake;
    bool m_stop = false;
    int m_refs = 0;
    std::thread m_thread;
};

} // namespace pitchflattener
