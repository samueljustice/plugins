#pragma once

// A stopwatch for the speed budgets in the tests ("faster than 10x real time", "under 150 ms").
//
// It counts the processor time this thread has used, not the time on the wall, so a budget is not missed because
// the machine was busy with something else (a build beside it, a shared CI runner) while the work waited its turn.
// Where the system has no such clock it falls back to the time on the wall.

#include <chrono>

#if defined(__APPLE__) || defined(__linux__)
 #include <time.h>
#endif

namespace TestClock
{
    /** Milliseconds of processor time the calling thread has used so far. Take two readings and subtract. */
    inline double cpuMs()
    {
       #if defined(__APPLE__) || defined(__linux__)
        timespec t {};
        if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t) == 0)
            return static_cast<double>(t.tv_sec) * 1000.0 + static_cast<double>(t.tv_nsec) * 1.0e-6;
       #endif
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
}
