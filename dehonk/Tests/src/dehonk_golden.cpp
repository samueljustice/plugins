// Writes the hashes of the golden cases (GoldenCases.h) for whichever core it is built against,
// one "name hash" line per case.
//
//   dehonk_golden out.txt                 mono and stereo, the core's own defaults
//   dehonk_golden --compat out.txt        the same at the settings that must reproduce the core as
//                                         it was before Max Cut, Release and Impact Guard were controls
//   dehonk_golden --surround out.txt      mono, stereo, 5.1 and 7.1.4 (a core with layouts only)
//   dehonk_golden --defaults out.txt      the current core at its defaults, for golden_defaults_<arch>.txt:
//                                         mono, stereo, 5.1 and 7.1.4, with the cases that make Max Cut and
//                                         the impact guard work, the guard's reading in the hash, and Diff
//                                         (the "diff" and "automation_diff" cases, and the offset it reports)
//   dehonk_golden --defaults-no-diff out.txt   the same without Diff, to compare with a core from before
//                                         Diff's cross-fade (SPEC 13.17), which is all that differs
//
// Built by CMakeLists.txt as dehonk_golden against the current core, and against any copy of an
// older core placed in the build folder: ref-dsp/ (dehonk_golden_ref, the core before surround,
// which writes golden_ref.txt), ref-dsp2/ (dehonk_golden_ref2, the core before the three
// controls, which writes golden_ref2.txt with the surround cases) and ref-dsp3/ (dehonk_golden_ref3,
// the core before the dialogue sidechain, the ranges' own timing and the link modes, which writes
// golden_ref3.txt the same way).
#include "GoldenCases.h"

#include <cstdio>
#include <cstring>

int main (int argc, char** argv)
{
    bool compat = false, surround = false, extended = false, withDiff = false;
    const char* out = nullptr;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp (argv[i], "--compat") == 0) compat = true;
        else if (std::strcmp (argv[i], "--surround") == 0) surround = true;
        else if (std::strcmp (argv[i], "--defaults") == 0) surround = extended = withDiff = true;
        else if (std::strcmp (argv[i], "--defaults-no-diff") == 0) surround = extended = true;
        else out = argv[i];
    }
    if (out == nullptr)
    {
        std::fprintf (stderr, "Usage: %s [--compat] [--surround | --defaults | --defaults-no-diff] out.txt\n", argv[0]);
        return 2;
    }
#if ! DEHONK_GOLDEN_SURROUND
    if (surround)
    {
        std::fprintf (stderr, "%s: built against a core without layouts, no surround cases\n", argv[0]);
        return 2;
    }
#endif
    std::FILE* f = std::fopen (out, "w");
    if (f == nullptr)
    {
        std::fprintf (stderr, "%s: cannot write %s\n", argv[0], out);
        return 1;
    }
    if (compat && extended)
    {
        std::fprintf (stderr, "%s: --defaults is the core as it is; it does not go with --compat\n", argv[0]);
        return 2;
    }
    for (const auto& r : golden::runAll (golden::PlainPrepare {}, compat, extended, withDiff))
        std::fprintf (f, "%s %016llx\n", r.name.c_str(), (unsigned long long) r.hash);
#if DEHONK_GOLDEN_SURROUND
    if (surround)
        for (const auto& r : golden::runAllSurround (golden::SurroundPrepare {}, "layout", compat, extended, withDiff))
            std::fprintf (f, "%s %016llx\n", r.name.c_str(), (unsigned long long) r.hash);
#endif
    std::fclose (f);
    return 0;
}
