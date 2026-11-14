#pragma once

#include <JuceHeader.h>

#include <cmath>
#include <functional>
#include <limits>
#include <vector>

/**
 * Typed values: turns what you type into a control ("-12 dB", "4.2 s", "35%", "300", "Off") into the position
 * of that control, using nothing but the control's own readout. Every control already says what each position
 * reads as; this looks for the position whose readout is what was typed. So it works for every law (the fader's
 * dB, Rate's seconds, a percentage) without a second formula per control that could drift from the first.
 *
 *  - A number is matched against the numbers in the readouts. Units are optional; "ms" and "s" (and "kHz" and
 *    "Hz") are converted, and a number typed without a unit is read in the unit the control shows right now.
 *  - A number the control cannot reach goes to the nearest end of its travel.
 *  - A readout that shows a minus sign for a positive amount (a duck of "-4.8 dB") accepts the amount without it.
 *  - Words are matched against the readouts too ("Off", "Rate", "Full"), in any case.
 *  - Where several positions read the same ("35%"), the middle of them is used, so 35% is exactly 0.35.
 *  - A readout may say which side of a control it is on with a word before the number ("Under 2.4 kHz",
 *    "Over 320 Hz"), and one number can then be read on both. The word typed picks the side; a number typed
 *    without one is looked for on the side showing now, then anywhere.
 */
namespace TypedValue
{
    struct Quantity
    {
        bool isNumber = false;
        double value = 0.0;      // in the base unit (seconds, Hz)
        juce::String unit;       // lower case, as written ("ms", "db", "%"); empty when there is none
    };

    /** How much of the base unit one of `unit` is (ms -> 0.001, kHz -> 1000, anything else 1). */
    inline double unitScale(const juce::String& unit)
    {
        if (unit == "ms") return 0.001;
        if (unit == "khz") return 1000.0;
        return 1.0;
    }

    /** Reads the first number in the text and the unit after it. unitIfNone: the unit a bare number is in. */
    inline Quantity parse(const juce::String& text, const juce::String& unitIfNone = {})
    {
        Quantity q;
        // A typed minus may be a real minus sign or a dash; a decimal comma is a decimal point.
        const auto t = text.trim().toLowerCase().replaceCharacter(static_cast<juce::juce_wchar>(0x2212), '-')
                           .replaceCharacter(static_cast<juce::juce_wchar>(0x2013), '-').replaceCharacter(',', '.');
        if (t.isEmpty())
            return q;
        if (t.startsWith("-inf") || t.startsWith("inf") || t.startsWith("+inf"))
        {
            q.isNumber = true;
            q.value = t.startsWithChar('-') ? -1.0e9 : 1.0e9;
            return q;
        }
        const int len = t.length();
        int i = 0;
        while (i < len && ! (juce::CharacterFunctions::isDigit(t[i]) || (t[i] == '.' && i + 1 < len && juce::CharacterFunctions::isDigit(t[i + 1]))))
            ++i;
        if (i >= len)
            return q;
        int start = i;
        if (start > 0 && (t[start - 1] == '-' || t[start - 1] == '+'))
            --start;
        bool seenPoint = false;
        while (i < len && (juce::CharacterFunctions::isDigit(t[i]) || (t[i] == '.' && ! seenPoint)))
        {
            seenPoint = seenPoint || t[i] == '.';
            ++i;
        }
        q.isNumber = true;
        q.unit = t.substring(i).trim().upToFirstOccurrenceOf(" ", false, false);
        if (q.unit.isEmpty())
            q.unit = unitIfNone;
        q.value = t.substring(start, i).getDoubleValue() * unitScale(q.unit);
        return q;
    }

    /** The word a readout or a typed value puts before its number ("under" in "Under 2.4 kHz"), in lower case;
        empty when there is none, or no number. */
    inline juce::String wordBefore(const juce::String& text)
    {
        const auto t = text.trim().toLowerCase();
        int i = 0;
        while (i < t.length() && ! juce::CharacterFunctions::isDigit(t[i]))
            ++i;
        if (i >= t.length())
            return {};
        const auto word = t.substring(0, i).trimCharactersAtEnd(" -+.");
        return word.containsOnly("abcdefghijklmnopqrstuvwxyz ") ? word : juce::String();
    }

    /**
     * The position (0..1) whose readout is what was typed, or -1 when the text can't be understood.
     * textFor: the control's readout at a position. current: its position now (for the unit of a bare number).
     */
    inline float toNormalised(const std::function<juce::String(float)>& textFor, const juce::String& typed, float current)
    {
        const auto wanted = typed.trim();
        if (wanted.isEmpty())
            return -1.0f;

        constexpr int c_steps = 512;
        const auto at = [](int i) { return static_cast<float>(i) / static_cast<float>(c_steps); };

        // Words first: a readout that is exactly what was typed ("Off", "Rate"), and a readout typed back whole.
        {
            int first = -1, last = -1;
            for (int i = 0; i <= c_steps; ++i)
                if (textFor(at(i)).trim().equalsIgnoreCase(wanted))
                {
                    if (first < 0) first = i;
                    last = i;
                }
            const bool wordOnly = ! parse(wanted).isNumber;
            if (first >= 0 && wordOnly)
                return first == 0 ? 0.0f : (last == c_steps ? 1.0f : at((first + last) / 2));
            if (wordOnly)
                return -1.0f;
        }

        const auto shown = parse(textFor(juce::jlimit(0.0f, 1.0f, current)));
        auto target = parse(wanted, shown.isNumber ? shown.unit : juce::String());
        const double nan = std::numeric_limits<double>::quiet_NaN();

        // Which side of the control, when its readouts name one (see wordBefore): only positions that read with
        // that word are looked at.
        bool sided = false;
        juce::String side;
        {
            // Whether any position reads a number with this word, and whether one of them can read the target.
            const auto reach = [&](const juce::String& word, bool& canRead)
            {
                bool found = false;
                double low = std::numeric_limits<double>::max(), high = std::numeric_limits<double>::lowest();
                for (int i = 0; i <= c_steps; ++i)
                {
                    const auto text = textFor(at(i));
                    if (wordBefore(text) != word)
                        continue;
                    if (const auto q = parse(text); q.isNumber)
                    {
                        found = true;
                        low = std::min(low, q.value);
                        high = std::max(high, q.value);
                    }
                }
                canRead = found && target.value >= low && target.value <= high;
                return found;
            };
            bool canRead = false;
            if (const auto typedWord = wordBefore(wanted); typedWord.isNotEmpty())
            {
                sided = reach(typedWord, canRead);
                side = typedWord;
            }
            else if (const auto showing = wordBefore(textFor(juce::jlimit(0.0f, 1.0f, current))); showing.isNotEmpty())
            {
                sided = reach(showing, canRead) && canRead;
                side = showing;
            }
        }
        const auto valueAt = [&](float n)
        {
            const auto text = textFor(n);
            if (sided && wordBefore(text) != side)
                return nan;
            const auto q = parse(text);
            return q.isNumber ? q.value : nan;
        };

        std::vector<double> values(static_cast<size_t>(c_steps) + 1);
        double lowest = std::numeric_limits<double>::max(), highest = std::numeric_limits<double>::lowest();
        bool any = false;
        for (int i = 0; i <= c_steps; ++i)
        {
            const double v = values[static_cast<size_t>(i)] = valueAt(at(i));
            if (! std::isnan(v))
            {
                any = true;
                lowest = std::min(lowest, v);
                highest = std::max(highest, v);
            }
        }
        if (! any)
            return -1.0f;
        // "-4.8 dB" readouts for a positive amount: the amount typed without its minus.
        if ((target.value < lowest || target.value > highest) && -target.value >= lowest && -target.value <= highest)
            target.value = -target.value;
        const double x = target.value;
        const double tolerance = 1.0e-6 * std::max(1.0, std::abs(x));
        const auto matches = [&](double v) { return ! std::isnan(v) && std::abs(v - x) <= tolerance; };

        // The edge of the stretch that reads x, between a position that does (inside) and one that doesn't.
        const auto edge = [&](float inside, float outside)
        {
            for (int k = 0; k < 30; ++k)
            {
                const float mid = 0.5f * (inside + outside);
                (matches(valueAt(mid)) ? inside : outside) = mid;
            }
            return inside;
        };
        // The middle of the stretch that reads x around a position that does, or an end of the travel.
        const auto middleAround = [&](float hit, float before, float after)
        {
            if (hit <= 0.0f) return 0.0f;
            if (hit >= 1.0f) return 1.0f;
            const float lo = matches(valueAt(before)) ? before : edge(hit, before);
            const float hi = matches(valueAt(after)) ? after : edge(hit, after);
            if (lo <= 0.0f) return 0.0f;
            if (hi >= 1.0f) return 1.0f;
            return 0.5f * (lo + hi);
        };

        // The nearest sample, then the samples either side of the crossing.
        int best = -1;
        for (int i = 0; i <= c_steps; ++i)
        {
            const double v = values[static_cast<size_t>(i)];
            if (! std::isnan(v) && (best < 0 || std::abs(v - x) < std::abs(values[static_cast<size_t>(best)] - x)))
                best = i;
        }
        if (matches(values[static_cast<size_t>(best)]))
        {
            int first = best, last = best;
            while (first > 0 && matches(values[static_cast<size_t>(first) - 1])) --first;
            while (last < c_steps && matches(values[static_cast<size_t>(last) + 1])) ++last;
            if (first == 0) return 0.0f;
            if (last == c_steps) return 1.0f;
            return 0.5f * (edge(at(first), at(first - 1)) + edge(at(last), at(last + 1)));
        }
        for (const int other : { best - 1, best + 1 })
        {
            if (other < 0 || other > c_steps || std::isnan(values[static_cast<size_t>(other)]))
                continue;
            const double a = values[static_cast<size_t>(best)] - x, b = values[static_cast<size_t>(other)] - x;
            if (a * b >= 0.0)
                continue;
            // x lies between these two samples: close in on it.
            float lo = at(best), hi = at(other);
            double gLo = a;
            for (int k = 0; k < 30; ++k)
            {
                const float mid = 0.5f * (lo + hi);
                const double v = valueAt(mid);
                if (std::isnan(v))
                    break;
                if (matches(v))
                    return middleAround(mid, lo, hi);
                if ((v - x) * gLo > 0.0) { lo = mid; gLo = v - x; }
                else hi = mid;
            }
            return std::abs(valueAt(lo) - x) <= std::abs(valueAt(hi) - x) ? lo : hi;
        }
        return at(best); // out of reach: the nearest end
    }
}
