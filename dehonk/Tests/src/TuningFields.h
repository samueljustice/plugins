// Name-based access to dehonk::Tuning for "--set name=value". If a field is added to Tuning,
// add it to the lists here; a renamed or removed field is a compile error, which is intended.
#pragma once

#include "DeHonkCore.h"

#include <cstddef>
#include <cstdlib>
#include <utility>
#include <string>

#define DEHONK_TUNING_FLOAT_FIELDS(X) \
    X (depthSlope)                     \
    X (deadZoneDb)                     \
    X (kneeDb)                         \
    X (referenceExponent)              \
    X (broadWidthOctaves)              \
    X (narrowWidthOctaves)             \
    X (surroundRatio)                  \
    X (thresholdKneeDb)                \
    X (levelAttackMs)                  \
    X (levelReleaseMs)                 \
    X (parentRule)                     \
    X (parentStartDb)                  \
    X (parentFullDb)                   \
    X (attackSlowMs)                   \
    X (attackFastMs)                   \
    X (bellQ)                          \
    X (notchesPerModule)               \
    X (notchSpacingOctaves)            \
    X (notchHoldDb)                    \
    X (clickLowHz)                     \
    X (clickHighHz)                    \
    X (clickMarginDb)                  \
    X (clickNeighbourDb)               \
    X (clickSlope)                     \
    X (clickMaxCutDb)                  \
    X (clickBodyMs)                    \
    X (clickReleaseMs)                 \
    X (diffMatchSeconds)               \
    X (keySeconds)                     \
    X (peakTimeMs)                     \
    X (peakMinDb)                      \
    X (guardRiseDb)                    \
    X (guardShare)                     \
    X (guardLowHz)                     \
    X (guardLagMs)                     \
    X (guardAverageMs)                 \
    X (guardFloorDb)                   \
    X (guardCeilingDb)                 \
    X (guardRefractoryMs)              \
    X (guardLevelMs)                   \
    X (guardConfirmMs)                 \
    X (guardDecayDb)                   \
    X (guardBackDb)                    \
    X (guardHoldMs)                    \
    X (guardReturnMs)                  \
    X (guardLiftMs)                    \
    X (guardEase)                      \
    X (guardOverStartDb)               \
    X (guardOverFullDb)                \
    X (guardEaseFallMs)                \
    X (guardDutyLimit)                 \
    X (guardDutySeconds)               \
    X (moveReleaseMs)                  \
    X (unsettledReleaseMs)             \
    X (movedWithinDb)                  \
    X (thresholdCloseMs)               \
    X (earnMs)                         \
    X (earnProudDb)                    \
    X (depthFloor)

#define DEHONK_TUNING_BOOL_FIELDS(X) X (handOver) X (detrend)

namespace dh
{

inline bool setTuningField (dehonk::Tuning& t, const std::string& name, const std::string& value, std::string& err)
{
    char* end = nullptr;
#define DH_SET_FLOAT(f)                                                     \
    if (name == #f)                                                         \
    {                                                                       \
        const float v = std::strtof (value.c_str(), &end);                  \
        if (end == value.c_str() || *end != '\0')                           \
        {                                                                   \
            err = "not a number for " + name + ": " + value;                \
            return false;                                                   \
        }                                                                   \
        t.f = v;                                                            \
        return true;                                                        \
    }
#define DH_SET_BOOL(f)                                                      \
    if (name == #f)                                                         \
    {                                                                       \
        if (value == "1" || value == "true" || value == "on")              \
            t.f = true;                                                     \
        else if (value == "0" || value == "false" || value == "off")       \
            t.f = false;                                                    \
        else                                                                \
        {                                                                   \
            err = "expected 0/1/true/false for " + name + ": " + value;    \
            return false;                                                   \
        }                                                                   \
        return true;                                                        \
    }
    DEHONK_TUNING_FLOAT_FIELDS (DH_SET_FLOAT)
    DEHONK_TUNING_BOOL_FIELDS (DH_SET_BOOL)
#undef DH_SET_FLOAT
#undef DH_SET_BOOL
    (void) end;
    err = "unknown Tuning field: " + name;
    return false;
}

namespace detail
{
    /** Converts to anything, so Tuning { AnyField{}, ... } compiles exactly when the number of
        initialisers is at most the number of fields. */
    struct AnyField
    {
        template <typename T>
        operator T() const;
    };

    template <typename T, std::size_t... I>
    constexpr bool initialisableWith (std::index_sequence<I...>)
    {
        return requires { T { ((void) I, AnyField {})... }; };
    }

    /** Number of fields of a flat aggregate (no arrays or nested aggregates, which Tuning has not):
        the largest N for which T { N initialisers } compiles. */
    template <typename T, std::size_t N = 0>
    constexpr std::size_t aggregateFieldCount()
    {
        if constexpr (N > 128)
            return N;
        else if constexpr (initialisableWith<T> (std::make_index_sequence<N + 1> {}))
            return aggregateFieldCount<T, N + 1>();
        else
            return N;
    }
} // namespace detail

inline constexpr std::size_t kTuningFieldCount = detail::aggregateFieldCount<dehonk::Tuning>();

inline constexpr std::size_t kListedTuningFieldCount = 0
#define DH_COUNT(f) +1
    DEHONK_TUNING_FLOAT_FIELDS (DH_COUNT) DEHONK_TUNING_BOOL_FIELDS (DH_COUNT)
#undef DH_COUNT
    ;

/** True when the lists above name as many fields as dehonk::Tuning has, i.e. no field has been
    added to the struct without being added here. The count comes from aggregate initialisation,
    so a field of any type is counted, whatever padding it lands in. Checked by the tuning_fields
    test, not by a static_assert, so a new field in the core never breaks the harness build. A
    field whose type changes (float to double, say) is not detected; it still converts. */
inline bool tuningFieldsComplete() { return kListedTuningFieldCount == kTuningFieldCount; }

inline std::string tuningFieldNames()
{
    std::string s;
#define DH_NAME(f) s += (s.empty() ? "" : ", ") + std::string (#f);
    DEHONK_TUNING_FLOAT_FIELDS (DH_NAME)
    DEHONK_TUNING_BOOL_FIELDS (DH_NAME)
#undef DH_NAME
    return s;
}

inline std::string describeTuning (const dehonk::Tuning& t)
{
    std::string s;
#define DH_DESC_F(f) s += (s.empty() ? "" : " ") + std::string (#f) + "=" + std::to_string (t.f);
#define DH_DESC_B(f) s += (s.empty() ? "" : " ") + std::string (#f) + "=" + (t.f ? "1" : "0");
    DEHONK_TUNING_FLOAT_FIELDS (DH_DESC_F)
    DEHONK_TUNING_BOOL_FIELDS (DH_DESC_B)
#undef DH_DESC_F
#undef DH_DESC_B
    return s;
}

} // namespace dh
