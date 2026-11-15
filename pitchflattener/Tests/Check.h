#pragma once

// The tests' shared tally: every check counts, every failure is printed and counted.

#include <bit>
#include <cstdint>
#include <cstdio>
#include <string>

namespace pftest
{
inline int g_checks = 0;
inline int g_failures = 0;

inline void check(bool ok, const std::string& what)
{
    ++g_checks;
    if (! ok)
    {
        ++g_failures;
        std::printf("    FAILED: %s\n", what.c_str());
    }
}

/** The same sample, bit for bit. */
inline bool identical(float a, float b) noexcept
{
    return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
}

inline void beginCase(const char* name)
{
    std::printf("%s\n", name);
    std::fflush(stdout);
}

// The JUCE-free halves (EngineTests.cpp).
void runLocatorTests();
void runEngineTests();
} // namespace pftest
