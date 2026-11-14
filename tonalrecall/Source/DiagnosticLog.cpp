#include "DiagnosticLog.h"

#include <JuceHeader.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>

#if JUCE_WINDOWS
 #include <process.h>
#else
 #include <unistd.h>
#endif

namespace tonalrecall
{

const char* threadKindName(ThreadKind kind) noexcept
{
    switch (kind)
    {
        case ThreadKind::Message: return "message";
        case ThreadKind::Audio:   return "audio";
        case ThreadKind::Timer:   return "timer";
        case ThreadKind::Other:   return "other";
    }
    return "?";
}

namespace
{
    std::int64_t wallMicros() noexcept
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();
    }

    long processId() noexcept
    {
       #if JUCE_WINDOWS
        return static_cast<long>(_getpid());
       #else
        return static_cast<long>(getpid());
       #endif
    }

    void formatTime(std::int64_t us, char* out, std::size_t size)
    {
        const auto secs = static_cast<std::time_t>(us / 1'000'000);
        std::tm tm {};
       #if JUCE_WINDOWS
        localtime_s(&tm, &secs);
       #else
        localtime_r(&secs, &tm);
       #endif
        char base[32];
        std::strftime(base, sizeof(base), "%Y-%m-%d %H:%M:%S", &tm);
        std::snprintf(out, size, "%s.%06lld", base, static_cast<long long>(us % 1'000'000));
    }
}

DiagnosticLog& DiagnosticLog::instance()
{
    static DiagnosticLog log;
    return log;
}

DiagnosticLog::DiagnosticLog()
    : m_cells(new Cell[kQueueSize])
{
    for (std::size_t i = 0; i < kQueueSize; ++i)
        m_cells[i].sequence.store(i, std::memory_order_relaxed);
}

DiagnosticLog::~DiagnosticLog()
{
    {
        const std::lock_guard<std::mutex> guard(m_threadMutex);
        m_stop = true;
    }
    m_wake.notify_all();
    if (m_thread.joinable())
        m_thread.join();
}

std::string DiagnosticLog::defaultFilePath()
{
   #if JUCE_MAC
    const auto dir = juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile("Library/Logs/SammyJ");
   #else
    const auto dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("SammyJ/Logs");
   #endif
    return dir.getChildFile("TonalRecall.log").getFullPathName().toStdString();
}

void DiagnosticLog::setFileForTesting(const std::string& path)
{
    const std::lock_guard<std::mutex> guard(m_pathMutex);
    m_path = path;
    m_rotated = true;   // a test file is never rotated
}

std::string DiagnosticLog::filePath() const
{
    const std::lock_guard<std::mutex> guard(m_pathMutex);
    return m_path.empty() ? defaultFilePath() : m_path;
}

void DiagnosticLog::retain()
{
    const std::lock_guard<std::mutex> guard(m_threadMutex);
    if (m_refs++ == 0)
    {
        if (m_thread.joinable())
            m_thread.join();
        m_stop = false;
        m_thread = std::thread([this] { writerLoop(); });
    }
}

void DiagnosticLog::release()
{
    std::thread finished;
    {
        const std::lock_guard<std::mutex> guard(m_threadMutex);
        if (m_refs == 0 || --m_refs > 0)
            return;
        m_stop = true;
        finished = std::move(m_thread);
    }
    m_wake.notify_all();
    if (finished.joinable())
        finished.join();
}

void DiagnosticLog::writerLoop()
{
    std::unique_lock<std::mutex> lock(m_threadMutex);
    while (! m_stop)
    {
        m_wake.wait_for(lock, std::chrono::milliseconds(100));
        lock.unlock();
        drain();
        lock.lock();
    }
    lock.unlock();
    drain();   // whatever came in while stopping
}

void DiagnosticLog::post(std::uint32_t instanceId, ThreadKind thread, const char* fmt, ...) noexcept
{
    LogEvent event;
    event.instance = instanceId;
    event.thread = thread;
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(event.text, sizeof(event.text), fmt, args);
    va_end(args);
    postEvent(event);
}

bool DiagnosticLog::postEvent(LogEvent& event) noexcept
{
    event.wallUs = wallMicros();

    // Bounded multi-producer queue (sequence numbers per cell): producers race only on the
    // enqueue position, with a compare-exchange; nothing waits.
    auto pos = m_enqueuePos.load(std::memory_order_relaxed);
    for (;;)
    {
        auto& cell = m_cells[pos & (kQueueSize - 1)];
        const auto seq = cell.sequence.load(std::memory_order_acquire);
        const auto diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);
        if (diff == 0)
        {
            if (m_enqueuePos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
            {
                cell.event = event;
                cell.sequence.store(pos + 1, std::memory_order_release);
                return true;
            }
        }
        else if (diff < 0)
        {
            m_dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        else
        {
            pos = m_enqueuePos.load(std::memory_order_relaxed);
        }
    }
}

bool DiagnosticLog::pop(LogEvent& out) noexcept
{
    auto& cell = m_cells[m_dequeuePos & (kQueueSize - 1)];
    const auto seq = cell.sequence.load(std::memory_order_acquire);
    if (seq != m_dequeuePos + 1)
        return false;
    out = cell.event;
    cell.sequence.store(m_dequeuePos + kQueueSize, std::memory_order_release);
    ++m_dequeuePos;
    return true;
}

void DiagnosticLog::flush()
{
    drain();
}

void DiagnosticLog::drain()
{
    const std::lock_guard<std::mutex> guard(m_drainMutex);

    juce::String lines;
    LogEvent event;
    char time[48], body[1024];
    const auto pid = processId();

    while (pop(event))
    {
        if (event.format != nullptr)
            event.format(event, body, sizeof(body));
        else
            std::snprintf(body, sizeof(body), "%s", event.text);
        formatTime(event.wallUs, time, sizeof(time));
        char head[128];
        std::snprintf(head, sizeof(head), "%s  pid %ld  inst %u  %-7s  ", time, pid,
                      static_cast<unsigned>(event.instance), threadKindName(event.thread));
        lines << head << juce::String::fromUTF8(body) << "\n";
    }

    const auto dropped = m_dropped.load(std::memory_order_relaxed);
    if (dropped != m_droppedReported)
    {
        lines << "(" << juce::String(static_cast<juce::int64>(dropped - m_droppedReported))
              << " log events dropped: the queue was full)\n";
        m_droppedReported = dropped;
    }

    if (lines.isEmpty())
        return;

    std::string path;
    bool rotate = false;
    {
        const std::lock_guard<std::mutex> pathGuard(m_pathMutex);
        path = m_path.empty() ? defaultFilePath() : m_path;
        rotate = ! m_rotated;
        m_rotated = true;
    }

    juce::File file(juce::String::fromUTF8(path.c_str()));
    file.getParentDirectory().createDirectory();
    // Keep the log readable: start afresh (keeping one old copy) once it passes 4 MB.
    if (rotate && file.getSize() > 4 * 1024 * 1024)
        file.moveFileTo(file.getSiblingFile(file.getFileNameWithoutExtension() + ".old.log"));

    juce::FileOutputStream out(file);
    if (out.openedOk())
        out.writeText(lines, false, false, "\n");
}

} // namespace tonalrecall
