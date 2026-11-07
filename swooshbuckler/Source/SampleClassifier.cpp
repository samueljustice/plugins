#include "SampleClassifier.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

// Tier 0 classifier (docs/experiments/02-auto-classify-and-name.md): hand-made DSP features on the sound
// regions + soft-scored rules + a file-name prior. Runs on the message thread; allocation is fine here.
//
// Feature notes:
//   - Spectral features run on a ~16 kHz box-decimated copy (frame 1024 / hop 256, Hann), only on frames whose
//     centre lies in a region and that are within 40 dB of the loudest frame (tails and pre-roll don't count).
//   - crest is stored in dB (peak / RMS over the sound regions).
//   - attackMs: 10 % -> 90 % rise of a 5 ms RMS envelope (1 ms hop) on the loudest region, full rate.
//   - pitchClarity: median McLeod NSDF peak (70 Hz .. 2 kHz) over the active frames, computed with an FFT.

namespace
{
    constexpr double c_maxAnalysisSeconds = 60.0;
    constexpr double c_targetRate = 16000.0;
    constexpr int c_fftOrder = 10;              // 1024-point spectrum frames
    constexpr int c_frame = 1 << c_fftOrder;
    constexpr int c_hop = 256;
    constexpr int c_acfOrder = c_fftOrder + 1;  // zero-padded autocorrelation
    constexpr int c_acfSize = 1 << c_acfOrder;

    float hi(float x, float threshold, float width) { return 1.0f / (1.0f + std::exp(-(x - threshold) / width)); }
    float lo(float x, float threshold, float width) { return 1.0f - hi(x, threshold, width); }

    float median(std::vector<float> v)
    {
        if (v.empty())
            return 0.0f;
        const auto mid = v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2);
        std::nth_element(v.begin(), mid, v.end());
        return *mid;
    }

    std::vector<CargoRegion> clipRegions(const std::vector<CargoRegion>& regions, int numSamples)
    {
        std::vector<CargoRegion> out;
        for (auto r : regions)
        {
            r.start = juce::jlimit(0, numSamples, r.start);
            r.end = juce::jlimit(0, numSamples, r.end);
            if (r.end > r.start)
                out.push_back(r);
        }
        return out;
    }

    /** 1 ms block energies (sum of squares) of [start, end). */
    std::vector<double> blockEnergies(const float* x, int start, int end, int block)
    {
        std::vector<double> e(static_cast<size_t>((end - start + block - 1) / block), 0.0);
        for (int i = start; i < end; ++i)
            e[static_cast<size_t>((i - start) / block)] += static_cast<double>(x[i]) * x[i];
        return e;
    }

    /** RMS over a window of w blocks starting at each block (window clipped at the end). */
    std::vector<float> slidingRms(const std::vector<double>& e, int w, int block)
    {
        std::vector<float> out(e.size(), 0.0f);
        double sum = 0.0;
        const int n = static_cast<int>(e.size());
        for (int i = 0; i < juce::jmin(w, n); ++i)
            sum += e[static_cast<size_t>(i)];
        for (int i = 0; i < n; ++i)
        {
            const int count = juce::jmin(w, n - i);
            out[static_cast<size_t>(i)] = static_cast<float>(std::sqrt(juce::jmax(0.0, sum) / (count * block)));
            sum -= e[static_cast<size_t>(i)];
            if (i + w < n)
                sum += e[static_cast<size_t>(i + w)];
        }
        return out;
    }

    /** 10 % -> 90 % rise time (ms) of the 5 ms envelope, up to its maximum. */
    float attackMsOf(const std::vector<float>& env5)
    {
        if (env5.empty())
            return 0.0f;
        const auto peakIt = std::max_element(env5.begin(), env5.end());
        const float peak = *peakIt;
        if (peak <= 0.0f)
            return 0.0f;
        const int peakIndex = static_cast<int>(peakIt - env5.begin());
        int t10 = -1, t90 = peakIndex;
        for (int i = 0; i <= peakIndex; ++i)
        {
            if (t10 < 0 && env5[static_cast<size_t>(i)] >= 0.1f * peak) t10 = i;
            if (env5[static_cast<size_t>(i)] >= 0.9f * peak) { t90 = i; break; }
        }
        return static_cast<float>(juce::jmax(0, t90 - juce::jmax(0, t10))); // 1 block = 1 ms
    }

    //==========================================================================
    // File-name tokens and prior
    struct UcsCat { const char* code; CargoRole role; };

    // UCS category codes that are abbreviations (never real words), plus the role they hint at.
    const UcsCat c_ucsCodes[] = {
        { "ANML", CargoRole::Tonal }, { "CREA", CargoRole::Tonal }, { "VOX", CargoRole::Tonal }, { "HMN", CargoRole::Tonal },
        { "BIRD", CargoRole::Tonal }, { "WHSH", CargoRole::Air }, { "SWSH", CargoRole::Air }, { "CLOTH", CargoRole::Air },
        { "WING", CargoRole::Air }, { "WIND", CargoRole::Bed }, { "AMB", CargoRole::Bed }, { "WTHR", CargoRole::Bed },
        { "RAIN", CargoRole::Bed }, { "FIRE", CargoRole::Crackle }, { "ELEC", CargoRole::Crackle }, { "DEST", CargoRole::Crackle },
        { "DIRT", CargoRole::Crackle }, { "IMPCT", CargoRole::Impact }, { "EXPL", CargoRole::Impact }, { "FGHT", CargoRole::Impact },
        { "METL", CargoRole::Impact }, { "DOOR", CargoRole::Impact }, { "NTRL", CargoRole::Rumble },
        { "DSGN", CargoRole::Generic }, { "FOLY", CargoRole::Generic }, { "GLAS", CargoRole::Generic }, { "WATR", CargoRole::Generic },
        { "MACH", CargoRole::Generic }, { "MECH", CargoRole::Generic }, { "MOTR", CargoRole::Generic }, { "VEH", CargoRole::Generic },
        { "WEAP", CargoRole::Generic }, { "PAPR", CargoRole::Generic }, { "PLAS", CargoRole::Generic }, { "RUBR", CargoRole::Generic },
        { "LETHR", CargoRole::Generic }, { "SCIF", CargoRole::Generic }, { "MAGC", CargoRole::Generic }, { "MUSC", CargoRole::Generic },
        { "BLLT", CargoRole::Impact }, { "DRWR", CargoRole::Generic }, { "WNDW", CargoRole::Generic }, { "CERM", CargoRole::Generic },
        { "COMM", CargoRole::Generic }, { "COMP", CargoRole::Generic }, { "GEOT", CargoRole::Generic }, { "LASR", CargoRole::Generic },
        { "ALRM", CargoRole::Generic }, { "TOON", CargoRole::Generic }, { "FEET", CargoRole::Generic }, { "ROBO", CargoRole::Generic },
    };

    // Words that are pure abbreviations and get dropped when they stand alone as the first token.
    const char* const c_abbrevCodes[] = { "AMB", "ANML", "CREA", "HMN", "WHSH", "SWSH", "WTHR", "ELEC", "IMPCT", "EXPL", "FGHT",
        "METL", "NTRL", "DSGN", "FOLY", "GLAS", "WATR", "MACH", "MECH", "MOTR", "VEH", "WEAP", "PAPR", "PLAS", "RUBR",
        "LETHR", "SCIF", "MAGC", "MUSC", "BLLT", "DRWR", "WNDW", "CERM", "GEOT", "LASR", "ALRM" };

    struct Keyword { const char* word; CargoRole role; };
    const Keyword c_keywords[] = {
        { "squeal", CargoRole::Tonal }, { "pig", CargoRole::Tonal }, { "growl", CargoRole::Tonal }, { "roar", CargoRole::Tonal },
        { "dog", CargoRole::Tonal }, { "bird", CargoRole::Tonal }, { "vox", CargoRole::Tonal }, { "scream", CargoRole::Tonal },
        { "vocal", CargoRole::Tonal }, { "voice", CargoRole::Tonal }, { "whistle", CargoRole::Tonal }, { "monkey", CargoRole::Tonal },
        { "creature", CargoRole::Tonal }, { "whale", CargoRole::Tonal }, { "horse", CargoRole::Tonal }, { "screech", CargoRole::Tonal },
        { "howl", CargoRole::Tonal }, { "bark", CargoRole::Tonal }, { "cat", CargoRole::Tonal },
        { "hit", CargoRole::Impact }, { "impact", CargoRole::Impact }, { "punch", CargoRole::Impact }, { "boom", CargoRole::Impact },
        { "thud", CargoRole::Impact }, { "slam", CargoRole::Impact }, { "clang", CargoRole::Impact }, { "smash", CargoRole::Impact },
        { "whoosh", CargoRole::Air }, { "swish", CargoRole::Air }, { "swoosh", CargoRole::Air }, { "cloth", CargoRole::Air },
        { "flap", CargoRole::Air }, { "swipe", CargoRole::Air }, { "sword", CargoRole::Air }, { "wing", CargoRole::Air },
        { "wind", CargoRole::Bed }, { "amb", CargoRole::Bed }, { "ambience", CargoRole::Bed }, { "room", CargoRole::Bed },
        { "roomtone", CargoRole::Bed }, { "tone", CargoRole::Bed }, { "drone", CargoRole::Bed }, { "bed", CargoRole::Bed },
        { "rain", CargoRole::Bed }, { "steam", CargoRole::Bed },
        { "fire", CargoRole::Crackle }, { "crackle", CargoRole::Crackle }, { "debris", CargoRole::Crackle },
        { "gravel", CargoRole::Crackle }, { "spark", CargoRole::Crackle }, { "sizzle", CargoRole::Crackle },
        { "electric", CargoRole::Crackle }, { "crackl", CargoRole::Crackle },
        { "rumble", CargoRole::Rumble }, { "sub", CargoRole::Rumble }, { "lfe", CargoRole::Rumble }, { "thunder", CargoRole::Rumble },
        { "quake", CargoRole::Rumble },
    };

    // Technical / bookkeeping tokens that never belong in a label.
    const char* const c_junkWords[] = { "mono", "stereo", "st", "ms", "lr", "wav", "aif", "aiff", "flac", "mp3", "ogg",
        "take", "tk", "ver", "version", "final", "edit", "edited", "copy", "bounce", "export", "print", "mix",
        "render", "processed", "proc", "master", "new", "alt", "bit", "khz" };

    bool isAbbrevCode(const juce::String& s)
    {
        for (auto* c : c_abbrevCodes)
            if (s == c)
                return true;
        return false;
    }

    bool isAllUpper(const juce::String& s)
    {
        bool any = false;
        for (auto c : s)
        {
            if (juce::CharacterFunctions::isLowerCase(c)) return false;
            if (juce::CharacterFunctions::isLetter(c)) any = true;
        }
        return any;
    }

    /** "ANMLFarm" -> "ANML" (caps prefix of >= 3 letters followed by lower case), else empty. */
    juce::String ucsCatIdPrefix(const juce::String& token)
    {
        int caps = 0;
        while (caps < token.length() && juce::CharacterFunctions::isUpperCase(token[caps]))
            ++caps;
        if (caps < 3 || caps >= token.length())
            return {};
        for (int i = caps; i < token.length(); ++i)
            if (! juce::CharacterFunctions::isLowerCase(token[i]))
                return {};
        return token.substring(0, caps);
    }

    juce::String stripAudioExtension(const juce::String& name)
    {
        static const char* const exts[] = { ".wav", ".wave", ".aif", ".aiff", ".flac", ".mp3", ".ogg", ".caf", ".m4a" };
        for (auto* e : exts)
            if (name.endsWithIgnoreCase(e))
                return name.dropLastCharacters(static_cast<int>(std::strlen(e)));
        return name;
    }

    /** Splits on separators only. */
    juce::StringArray rawTokens(const juce::String& name)
    {
        juce::StringArray t;
        t.addTokens(name, "_-. ,()[]{}+&#~", {});
        t.removeEmptyStrings();
        return t;
    }

    /** Splits camelCase and letter/digit boundaries: "PigSqueal03" -> Pig, Squeal, 03. Keeps "PIG" whole. */
    juce::StringArray splitWord(const juce::String& w)
    {
        juce::StringArray out;
        juce::String cur;
        auto kind = [](juce::juce_wchar c) { return juce::CharacterFunctions::isDigit(c) ? 2
                                                  : juce::CharacterFunctions::isUpperCase(c) ? 1 : 0; };
        for (int i = 0; i < w.length(); ++i)
        {
            const auto c = w[i];
            if (cur.isNotEmpty())
            {
                const auto prev = cur.getLastCharacter();
                const int kp = kind(prev), kc = kind(c);
                const bool digitBoundary = (kp == 2) != (kc == 2);
                const bool camel = kp == 0 && kc == 1;
                // "ABCDef" -> "ABC", "Def"
                const bool capsRun = kp == 1 && kc == 0 && cur.length() > 1 && isAllUpper(cur);
                if (digitBoundary || camel)
                {
                    out.add(cur);
                    cur.clear();
                }
                else if (capsRun)
                {
                    out.add(cur.dropLastCharacters(1));
                    cur = juce::String::charToString(prev);
                }
            }
            cur += juce::String::charToString(c);
        }
        if (cur.isNotEmpty())
            out.add(cur);
        return out;
    }

    bool isVersionOrTake(const juce::String& lower)
    {
        auto digitsAfter = [&](int n) { return lower.length() > n && lower.substring(n).containsOnly("0123456789"); };
        if (lower.startsWith("v") && digitsAfter(1)) return true;
        if (lower.startsWith("take") && digitsAfter(4)) return true;
        if (lower.startsWith("tk") && digitsAfter(2)) return true;
        if (lower.startsWith("t") && digitsAfter(1)) return true;
        if (lower.startsWith("r") && digitsAfter(1)) return true;  // r2 (revision)
        // sample-rate / bit-depth tags: 48k, 96k, 44k1, 24b, 16bit
        if (lower.endsWith("k") && lower.dropLastCharacters(1).containsOnly("0123456789") && lower.length() > 1) return true;
        if (lower.endsWith("b") && lower.dropLastCharacters(1).containsOnly("0123456789") && lower.length() > 1) return true;
        if (lower.endsWith("bit") && lower.dropLastCharacters(3).containsOnly("0123456789") && lower.length() > 3) return true;
        if (lower.endsWith("khz") && lower.dropLastCharacters(3).containsOnly("0123456789.") && lower.length() > 3) return true;
        return false;
    }

    bool isJunk(const juce::String& lower)
    {
        if (lower.containsOnly("0123456789"))
            return true;
        for (auto* j : c_junkWords)
            if (lower == j)
                return true;
        return isVersionOrTake(lower);
    }

    /** All lower-case word tokens of a name (for keyword matching), with UCS prefixes reported separately. */
    void nameTokens(const juce::String& fileName, juce::StringArray& words, juce::StringArray& ucsCodes)
    {
        const auto raw = rawTokens(stripAudioExtension(fileName));
        for (int i = 0; i < raw.size(); ++i)
        {
            const auto& t = raw[i];
            if (i == 0)
            {
                const auto prefix = ucsCatIdPrefix(t);
                if (prefix.isNotEmpty()) ucsCodes.add(prefix);
                else if (isAllUpper(t)) ucsCodes.add(t);
            }
            for (const auto& piece : splitWord(t))
                words.add(piece.toLowerCase());
        }
    }

    std::array<float, c_numCargoRoles> fileNamePrior(const juce::String& fileName)
    {
        std::array<float, c_numCargoRoles> prior {};
        if (fileName.isEmpty())
            return prior;

        juce::StringArray words, codes;
        nameTokens(fileName, words, codes);

        for (const auto& code : codes)
            for (const auto& u : c_ucsCodes)
                if (code.startsWith(u.code) && u.role != CargoRole::Generic)
                    prior[static_cast<size_t>(u.role)] = 1.0f;

        for (const auto& w : words)
            for (const auto& k : c_keywords)
            {
                const juce::String kw(k.word);
                if (w == kw || (kw.length() >= 4 && w.startsWith(kw)))
                    prior[static_cast<size_t>(k.role)] = juce::jmax(prior[static_cast<size_t>(k.role)], 1.0f);
            }

        // Several roles hinted at: the later words describe the sound more than the first one does
        // ("Wind_Whoosh" is a whoosh), but keep it simple and just halve when ambiguous.
        int hits = 0;
        for (auto p : prior) if (p > 0.0f) ++hits;
        if (hits > 1)
            for (auto& p : prior) p *= 0.6f;
        return prior;
    }

    bool isMeaningfulLabel(const juce::String& label)
    {
        if (label.isEmpty() || ! label.containsAnyOf("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"))
            return false;
        static const char* const generic[] = { "audio", "untitled", "sample", "track", "recording", "rec", "pasted",
                                               "sound", "file", "clip", "region", "bounce", "export", "new" };
        juce::StringArray words;
        words.addTokens(label.toLowerCase(), " ", {});
        for (const auto& w : words)
        {
            bool isGeneric = false;
            for (auto* g : generic)
                if (w == g) isGeneric = true;
            if (! isGeneric && w.length() > 1)
                return true;
        }
        return false;
    }

    juce::String roleDefaultLabel(CargoRole role)
    {
        switch (role)
        {
            case CargoRole::Bed:     return "Wind Bed";
            case CargoRole::Tonal:   return "Tonal Sweetener";
            case CargoRole::Impact:  return "Hit";
            case CargoRole::Air:     return "Swish";
            case CargoRole::Crackle: return "Crackle";
            case CargoRole::Rumble:  return "Rumble";
            case CargoRole::Generic: break;
        }
        return "Sample";
    }
}

//==============================================================================
juce::String SampleClassifier::labelFromFileName(const juce::String& fileNameWithoutExtension)
{
    const auto raw = rawTokens(stripAudioExtension(fileNameWithoutExtension.trim()));
    juce::StringArray kept;
    bool keptMixedCase = false;

    for (int i = 0; i < raw.size(); ++i)
    {
        auto token = raw[i];

        if (i == 0 && raw.size() > 1)
        {
            // UCS CatID ("ANMLFarm", "WHSHSwish") or a bare abbreviation code ("WHSH", "IMPCT").
            if (ucsCatIdPrefix(token).isNotEmpty() || isAbbrevCode(token))
                continue;
        }

        // Creator / library IDs: short all-caps tokens after the name has started ("..._SJ_SBK").
        if (keptMixedCase && isAllUpper(token) && token.length() <= 4)
            continue;

        if (isJunk(token.toLowerCase()))
            continue;

        for (const auto& piece : splitWord(token))
        {
            const auto lower = piece.toLowerCase();
            if (isJunk(lower) || ! piece.containsAnyOf("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"))
                continue;
            if (lower.length() == 1 && kept.isEmpty() && piece != "A")
                continue; // stray letters ("A_01" is fine, "x" is not)
            kept.add(lower.substring(0, 1).toUpperCase() + lower.substring(1));
            if (! isAllUpper(piece))
                keptMixedCase = true;
        }
    }

    juce::StringArray words;
    for (const auto& k : kept)
        if (words.size() < 3)
            words.add(k);
    return words.joinIntoString(" ");
}

juce::String SampleClassifier::roleName(CargoRole role)
{
    switch (role)
    {
        case CargoRole::Generic: return "Generic";
        case CargoRole::Bed:     return "Bed";
        case CargoRole::Tonal:   return "Tonal";
        case CargoRole::Impact:  return "Impact";
        case CargoRole::Air:     return "Air";
        case CargoRole::Crackle: return "Crackle";
        case CargoRole::Rumble:  return "Rumble";
    }
    return "Generic";
}

juce::String SampleClassifier::roleDescription(CargoRole role)
{
    switch (role)
    {
        case CargoRole::Generic: return "Loops the current sound and moves to the next one on each whoosh.";
        case CargoRole::Bed:     return "Continuous energy bed: loops smoothly and ignores individual whooshes.";
        case CargoRole::Tonal:   return "Sweetener: plays once per whoosh with its loudest moment on the peak.";
        case CargoRole::Impact:  return "Hit: plays once per whoosh with the transient exactly on the peak.";
        case CargoRole::Air:     return "Swish: plays once per whoosh, stretched to fit the flight.";
        case CargoRole::Crackle: return "Granular texture: loops with random jumps (Chaos).";
        case CargoRole::Rumble:  return "Low end: loops like a bed and follows pitch at half strength.";
    }
    return {};
}

//==============================================================================
void SampleClassifier::findFocusPoints(const float* mono, int numSamples, double sampleRate, std::vector<CargoRegion>& regions)
{
    if (mono == nullptr || numSamples <= 0 || sampleRate <= 0.0)
        return;

    const int block = juce::jmax(1, static_cast<int>(sampleRate / 1000.0)); // 1 ms
    for (auto& r : regions)
    {
        const int start = juce::jlimit(0, numSamples, r.start);
        const int end = juce::jlimit(0, numSamples, r.end);
        if (end <= start)
        {
            r.focus = r.start;
            continue;
        }

        const auto e = blockEnergies(mono, start, end, block);
        const int n = static_cast<int>(e.size());
        const auto env20 = slidingRms(e, 20, block);
        const auto env5 = slidingRms(e, 5, block);

        // Loudest 20 ms window (centre).
        int focus = (start + end) / 2;
        if (n > 20)
        {
            const int best = static_cast<int>(std::max_element(env20.begin(), env20.begin() + (n - 19)) - env20.begin());
            focus = start + best * block + 10 * block;
        }

        // Impact-like: the steepest rise of the 1 ms envelope before the loudest moment.
        if (attackMsOf(env5) < 15.0f)
        {
            const int peakBlock = static_cast<int>(std::max_element(env5.begin(), env5.end()) - env5.begin()) + 5;
            float bestRise = 0.0f;
            int bestIndex = -1;
            float prev = 0.0f;
            for (int i = 0; i < juce::jmin(n, peakBlock + 1); ++i)
            {
                const float v = static_cast<float>(std::sqrt(e[static_cast<size_t>(i)] / block));
                if (v - prev > bestRise) { bestRise = v - prev; bestIndex = i; }
                prev = v;
            }
            if (bestIndex >= 0)
                focus = start + bestIndex * block;
        }

        r.focus = juce::jlimit(start, end - 1, focus);
    }
}

//==============================================================================
SampleAnalysis SampleClassifier::analyse(const float* mono, int numSamples, double sampleRate,
                                         const std::vector<CargoRegion>& regionsIn, const juce::String& fileName)
{
    SampleAnalysis a;
    const auto prior = fileNamePrior(fileName);
    const auto fileLabel = labelFromFileName(stripAudioExtension(fileName));

    auto finish = [&](std::array<float, c_numCargoRoles> scores)
    {
        for (size_t i = 0; i < scores.size(); ++i)
            scores[i] += 0.6f * prior[i];
        scores[static_cast<size_t>(CargoRole::Generic)] = juce::jmax(scores[static_cast<size_t>(CargoRole::Generic)], 0.35f);

        size_t best = 0;
        for (size_t i = 1; i < scores.size(); ++i)
            if (scores[i] > scores[best]) best = i;
        float second = 0.0f;
        for (size_t i = 0; i < scores.size(); ++i)
            if (i != best) second = juce::jmax(second, scores[i]);

        a.role = static_cast<CargoRole>(best);
        a.confidence = juce::jlimit(0.0f, 1.0f, scores[best] - second);
        a.label = isMeaningfulLabel(fileLabel) ? fileLabel : roleDefaultLabel(a.role);
        return a;
    };

    if (mono == nullptr || numSamples <= 0 || sampleRate <= 0.0)
        return finish({});

    const int n = juce::jmin(numSamples, static_cast<int>(c_maxAnalysisSeconds * sampleRate));
    const auto regions = clipRegions(regionsIn, n);
    if (regions.empty())
        return finish({});

    //--------------------------------------------------------------------------
    // Time-domain features at full rate
    std::int64_t soundSamples = 0;
    double sumSq = 0.0;
    float peak = 0.0f;
    float loudestRegionPeak = -1.0f;
    size_t loudestRegion = 0;
    for (size_t ri = 0; ri < regions.size(); ++ri)
    {
        const auto& r = regions[ri];
        float rp = 0.0f;
        for (int i = r.start; i < r.end; ++i)
        {
            const float v = mono[i];
            sumSq += static_cast<double>(v) * v;
            rp = juce::jmax(rp, std::abs(v));
        }
        soundSamples += r.end - r.start;
        peak = juce::jmax(peak, rp);
        if (rp > loudestRegionPeak) { loudestRegionPeak = rp; loudestRegion = ri; }
    }

    a.soundSeconds = static_cast<float>(static_cast<double>(soundSamples) / sampleRate);
    a.sustainRatio = static_cast<float>(static_cast<double>(soundSamples) / n);
    const double rms = std::sqrt(sumSq / static_cast<double>(juce::jmax<std::int64_t>(1, soundSamples)));
    a.crest = rms > 1.0e-9 ? juce::Decibels::gainToDecibels(static_cast<float>(peak / rms)) : 0.0f;
    if (peak < 1.0e-6f)
        return finish({});

    {
        const int block = juce::jmax(1, static_cast<int>(sampleRate / 1000.0));
        const auto& r = regions[loudestRegion];
        a.attackMs = attackMsOf(slidingRms(blockEnergies(mono, r.start, r.end, block), 5, block))
                     * static_cast<float>(block * 1000.0 / sampleRate);
    }

    const float meanRegionSeconds = a.soundSeconds / static_cast<float>(regions.size());

    //--------------------------------------------------------------------------
    // Decimated copy for the spectral features
    const int decim = juce::jmax(1, static_cast<int>(std::round(sampleRate / c_targetRate)));
    const double rate = sampleRate / decim;
    const int dn = n / decim;
    std::vector<float> x(static_cast<size_t>(dn));
    for (int i = 0; i < dn; ++i)
    {
        float s = 0.0f;
        for (int k = 0; k < decim; ++k)
            s += mono[i * decim + k];
        x[static_cast<size_t>(i)] = s / static_cast<float>(decim);
    }

    // Region membership per decimated sample (frame centre test)
    auto inRegion = [&](int decimatedIndex)
    {
        const int s = decimatedIndex * decim;
        for (const auto& r : regions)
            if (s >= r.start && s < r.end)
                return true;
        return false;
    };

    const int numFrames = dn >= c_frame ? 1 + (dn - c_frame) / c_hop : (dn > 0 ? 1 : 0);
    if (numFrames == 0)
        return finish({});

    juce::dsp::FFT specFft(c_fftOrder);
    juce::dsp::FFT acfFft(c_acfOrder);
    std::vector<float> window(static_cast<size_t>(c_frame));
    for (int i = 0; i < c_frame; ++i)
        window[static_cast<size_t>(i)] = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::twoPi * static_cast<float>(i) / static_cast<float>(c_frame));

    std::vector<float> specBuf(static_cast<size_t>(2 * c_frame));
    std::vector<float> acfBuf(static_cast<size_t>(2 * c_acfSize));
    std::vector<float> frame(static_cast<size_t>(c_frame));
    std::vector<double> prefixSq(static_cast<size_t>(c_frame + 1));
    const int bins = c_frame / 2;
    std::vector<float> logPrev(static_cast<size_t>(bins + 1), 0.0f), logCur(static_cast<size_t>(bins + 1));

    const int minLag = juce::jmax(2, static_cast<int>(rate / 2000.0));
    const int maxLag = juce::jmin(c_frame / 2, static_cast<int>(rate / 70.0));
    const int lowBin = juce::jmax(1, static_cast<int>(std::round(150.0 * c_frame / rate)));

    struct FrameInfo { float energyDb, flatness, centroid, clarity, flux; double power, lowPower; bool active; };
    std::vector<FrameInfo> frames(static_cast<size_t>(numFrames));

    double maxPower = 0.0;
    // First pass: frame powers (to set the active threshold and the log floor)
    std::vector<double> framePower(static_cast<size_t>(numFrames), 0.0);
    for (int f = 0; f < numFrames; ++f)
    {
        const int off = f * c_hop;
        double s = 0.0;
        for (int i = 0; i < c_frame && off + i < dn; ++i)
            s += static_cast<double>(x[static_cast<size_t>(off + i)]) * x[static_cast<size_t>(off + i)];
        framePower[static_cast<size_t>(f)] = s / c_frame;
        maxPower = juce::jmax(maxPower, framePower[static_cast<size_t>(f)]);
    }
    if (maxPower <= 0.0)
        return finish({});
    const double activeFloor = maxPower * 1.0e-4; // -40 dB

    // Per-bin log floor: about 100 dB under the loudest frame's mean bin power.
    const float logFloor = static_cast<float>(std::log10(maxPower * c_frame * c_frame * 0.25 * 1.0e-10 + 1.0e-30));

    for (int f = 0; f < numFrames; ++f)
    {
        auto& fi = frames[static_cast<size_t>(f)];
        const int off = f * c_hop;
        for (int i = 0; i < c_frame; ++i)
            frame[static_cast<size_t>(i)] = off + i < dn ? x[static_cast<size_t>(off + i)] : 0.0f;

        // Remove DC
        const float mean = std::accumulate(frame.begin(), frame.end(), 0.0f) / static_cast<float>(c_frame);
        for (auto& v : frame) v -= mean;

        fi.power = framePower[static_cast<size_t>(f)];
        fi.energyDb = static_cast<float>(10.0 * std::log10(fi.power / maxPower + 1.0e-12));
        fi.active = fi.power >= activeFloor && inRegion(off + c_frame / 2);

        // Spectrum
        std::fill(specBuf.begin(), specBuf.end(), 0.0f);
        for (int i = 0; i < c_frame; ++i)
            specBuf[static_cast<size_t>(i)] = frame[static_cast<size_t>(i)] * window[static_cast<size_t>(i)];
        specFft.performFrequencyOnlyForwardTransform(specBuf.data(), true);

        double sumP = 0.0, sumLogP = 0.0, sumFP = 0.0, lowP = 0.0;
        float flux = 0.0f;
        for (int b = 1; b <= bins; ++b)
        {
            const double mag = specBuf[static_cast<size_t>(b)];
            const double p = mag * mag + 1.0e-20;
            sumP += p;
            sumLogP += std::log(p);
            sumFP += p * (b * rate / c_frame);
            if (b < lowBin) lowP += p;
            const float lp = juce::jmax(logFloor, static_cast<float>(std::log10(p)));
            logCur[static_cast<size_t>(b)] = lp;
            if (f > 0)
                flux += juce::jmax(0.0f, lp - logPrev[static_cast<size_t>(b)]);
        }
        std::swap(logPrev, logCur);
        fi.flux = flux / static_cast<float>(bins);
        fi.flatness = static_cast<float>(std::exp(sumLogP / bins) / (sumP / bins));
        fi.centroid = static_cast<float>(sumFP / sumP);
        fi.power = sumP;
        fi.lowPower = lowP;

        // Pitch clarity: McLeod NSDF via FFT autocorrelation (only needed for active frames)
        fi.clarity = 0.0f;
        if (fi.active)
        {
            std::fill(acfBuf.begin(), acfBuf.end(), 0.0f);
            std::copy(frame.begin(), frame.end(), acfBuf.begin());
            acfFft.performRealOnlyForwardTransform(acfBuf.data(), true);
            for (int b = 0; b <= c_acfSize / 2; ++b)
            {
                const float re = acfBuf[static_cast<size_t>(2 * b)], im = acfBuf[static_cast<size_t>(2 * b + 1)];
                acfBuf[static_cast<size_t>(2 * b)] = re * re + im * im;
                acfBuf[static_cast<size_t>(2 * b + 1)] = 0.0f;
            }
            acfFft.performRealOnlyInverseTransform(acfBuf.data()); // acfBuf[tau] = r(tau) (scaled by JUCE's 1/N)

            prefixSq[0] = 0.0;
            for (int i = 0; i < c_frame; ++i)
                prefixSq[static_cast<size_t>(i + 1)] = prefixSq[static_cast<size_t>(i)]
                                                       + static_cast<double>(frame[static_cast<size_t>(i)]) * frame[static_cast<size_t>(i)];
            const double r0 = acfBuf[0];
            const double total = prefixSq[static_cast<size_t>(c_frame)];
            if (r0 > 0.0 && total > 0.0)
            {
                const double scale = total / r0; // undo the FFT scaling: r(0) == sum of squares
                bool crossed = false;
                float best = 0.0f;
                for (int tau = 1; tau <= maxLag; ++tau)
                {
                    const double m = prefixSq[static_cast<size_t>(c_frame - tau)]                       // x[0 .. N-tau)
                                     + (total - prefixSq[static_cast<size_t>(tau)]);                     // x[tau .. N)
                    const double nsdf = m > 0.0 ? 2.0 * acfBuf[static_cast<size_t>(tau)] * scale / m : 0.0;
                    if (! crossed)
                    {
                        if (nsdf < 0.0) crossed = true;
                        continue;
                    }
                    if (tau >= minLag)
                        best = juce::jmax(best, static_cast<float>(nsdf));
                }
                fi.clarity = juce::jlimit(0.0f, 1.0f, best);
            }
        }
    }

    // Aggregate over active frames
    std::vector<float> flat, clar, levels;
    double sumPower = 0.0, sumLow = 0.0, sumCentroid = 0.0;
    int activeFrames = 0;
    for (const auto& fi : frames)
    {
        if (! fi.active) continue;
        ++activeFrames;
        flat.push_back(fi.flatness);
        clar.push_back(fi.clarity);
        levels.push_back(fi.energyDb);
        sumPower += fi.power;
        sumLow += fi.lowPower;
        sumCentroid += fi.centroid * fi.power;
    }
    if (activeFrames == 0)
        return finish({});

    a.flatness = median(flat);
    a.pitchClarity = median(clar);
    a.lowRatio = static_cast<float>(sumLow / sumPower);
    a.centroidHz = static_cast<float>(sumCentroid / sumPower);

    // Level steadiness: spread of the frame levels within 30 dB of the loudest (bed vs swell)
    float levelSpread = 0.0f;
    {
        std::vector<float> top;
        for (auto l : levels) if (l > -30.0f) top.push_back(l);
        if (top.size() > 1)
        {
            const float m = std::accumulate(top.begin(), top.end(), 0.0f) / static_cast<float>(top.size());
            float v = 0.0f;
            for (auto l : top) v += (l - m) * (l - m);
            levelSpread = std::sqrt(v / static_cast<float>(top.size()));
        }
    }

    // Onsets: spectral-flux peaks over an adaptive (local median) threshold, in active frames only.
    int onsets = 0;
    {
        const int half = 8;
        const int minSpacing = juce::jmax(1, static_cast<int>(0.04 * rate / c_hop));
        int last = -minSpacing - 1;
        std::vector<float> local;
        for (int f = 1; f + 1 < numFrames; ++f)
        {
            const auto& fi = frames[static_cast<size_t>(f)];
            if (! fi.active) continue;
            const float v = fi.flux;
            if (v <= frames[static_cast<size_t>(f - 1)].flux || v < frames[static_cast<size_t>(f + 1)].flux) continue;
            local.clear();
            for (int k = juce::jmax(1, f - half); k <= juce::jmin(numFrames - 1, f + half); ++k)
                local.push_back(frames[static_cast<size_t>(k)].flux);
            const float threshold = 1.5f * median(local) + 0.15f;
            if (v > threshold && f - last >= minSpacing)
            {
                ++onsets;
                last = f;
            }
        }
    }
    a.onsetRate = a.soundSeconds > 0.0f ? static_cast<float>(onsets) / a.soundSeconds : 0.0f;
    const float onsetsPerRegion = static_cast<float>(onsets) / static_cast<float>(regions.size());

    //--------------------------------------------------------------------------
    // Soft rules
    std::array<float, c_numCargoRoles> s {};
    auto& impact = s[static_cast<size_t>(CargoRole::Impact)];
    auto& rumble = s[static_cast<size_t>(CargoRole::Rumble)];
    auto& tonal = s[static_cast<size_t>(CargoRole::Tonal)];
    auto& crackle = s[static_cast<size_t>(CargoRole::Crackle)];
    auto& air = s[static_cast<size_t>(CargoRole::Air)];
    auto& bed = s[static_cast<size_t>(CargoRole::Bed)];

    const float notLow = lo(a.lowRatio, 0.5f, 0.08f);

    impact = lo(meanRegionSeconds, 1.2f, 0.2f) * lo(a.attackMs, 15.0f, 4.0f) * hi(a.crest, 13.0f, 1.5f)
             * lo(onsetsPerRegion, 2.5f, 0.6f);
    rumble = hi(a.lowRatio, 0.5f, 0.08f) * lo(a.centroidHz, 300.0f, 60.0f);
    tonal = hi(a.pitchClarity, 0.7f, 0.06f) * lo(a.flatness, 0.15f, 0.04f) * notLow;
    crackle = hi(a.onsetRate, 6.0f, 1.5f) * hi(a.flatness, 0.15f, 0.05f) * hi(a.crest, 14.0f, 2.0f);
    air = hi(a.flatness, 0.15f, 0.05f) * hi(meanRegionSeconds, 0.3f, 0.08f) * lo(meanRegionSeconds, 4.0f, 0.6f)
          * lo(onsetsPerRegion, 2.5f, 0.6f) * hi(a.attackMs, 40.0f, 12.0f) * notLow;
    bed = hi(meanRegionSeconds, 3.0f, 0.6f) * hi(a.sustainRatio, 0.6f, 0.08f) * lo(levelSpread, 6.0f, 1.5f)
          * lo(a.onsetRate, 4.0f, 1.0f) * notLow;

    return finish(s);
}
