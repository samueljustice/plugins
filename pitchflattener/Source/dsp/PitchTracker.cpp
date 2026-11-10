#include "PitchTracker.h"
#include <algorithm>
#include <cmath>

namespace pf
{
    namespace
    {
        constexpr float bigCost = 1.0e9f;

        float medianOf (float* v, int n)
        {
            if (n <= 0)
                return 0.0f;
            std::nth_element (v, v + n / 2, v + n);
            float m = v[n / 2];
            if ((n & 1) == 0)
            {
                const float lo = *std::max_element (v, v + n / 2);
                m = 0.5f * (m + lo);
            }
            return m;
        }
    }

    void PitchTracker::prepare (double hopSeconds)
    {
        hopSec = hopSeconds;
        reset();
    }

    void PitchTracker::reset()
    {
        numPushed = 0;
        cost.fill (0.0f);
        cost[(size_t) numStates - 1] = 0.0f;
        for (int i = 0; i < numStates - 1; ++i)
            cost[(size_t) i] = bigCost;
        stateCents.fill (0.0f);

        for (auto& l : lattice)   l = Lattice {};
        for (auto& d : decided)   d = Decided {};
        for (auto& d : repaired)  d = Decided {};
        for (auto& f : filled)    f = Filled {};
        for (auto& f : smoothed)  f = Filled {};
        std::fill (ratio.begin(), ratio.end(), 0.0f);

        lastVoicedRepaired = -1000000;
        latched = false;
        latchedCents = 0.0f;
        latchUnvoicedRun = 0;
        relatchPending = false;
        lastRatio = 0.0f;
        guideOffsetSet = false;
        guideUsable = false;
        ownJumped = guideJumped = false;
        lastOwnVoiced = lastGuideCents = 0.0f;
        std::fill (ownHistory.begin(), ownHistory.end(), 0.0f);
        guideOffset = 0.0f;
        lastGuideFrame = -1000000;
        telemetry = TelemetryFrame {};
    }

    void PitchTracker::setSettings (const LiveSettings& s, DetectorType detector)
    {
        settings = s;
        const float hopsPerSec = (float) (1.0 / hopSec);

        // Allowed movement per hop: tracking speed (oct/s) -> cents per hop. Floor keeps jitter cheap.
        sigmaCents = std::max (6.0f, 1200.0f * s.trackingSpeedOctPerSec / hopsPerSec);
        harmonicDetector = detector == DetectorType::HarmonicSum;
        octaveCost = harmonicDetector ? 4.0f : (detector == DetectorType::SpectralPeak || detector == DetectorType::SpectralMpm ? 3.0f : 2.5f);
        voicedOn = 0.5f;

        gapFrames = std::clamp ((int) std::lround (s.gapFillMs * 0.001f * hopsPerSec), 0, gapLag);
        smoothSigmaFrames = std::clamp (s.smoothingMs * 0.001f * hopsPerSec, 0.0f, smoothLag / 3.0f);
        autoHalfFrames = std::clamp ((int) std::lround (0.5f * s.autoWindowMs * 0.001f * hopsPerSec), 2, targetLag);
    }

    //==========================================================================
    void PitchTracker::push (const FrameObservation& obsIn)
    {
        stepViterbi (obsIn);
        const int64_t t = numPushed++;

        const int64_t d = t - viterbiLag;
        if (d < 0)
            return;

        // Fixed-lag decision: backtrack from the current best state
        int s = 0;
        for (int i = 1; i < numStates; ++i)
            if (cost[(size_t) i] < cost[(size_t) s])
                s = i;
        for (int64_t f = t; f > d; --f)
            s = lattice[(size_t) idx (f)].back[(size_t) s];

        const auto& L = lattice[(size_t) idx (d)];
        auto& dec = decided[(size_t) idx (d)];
        dec.voiced = s < L.numVoiced;
        dec.cents = dec.voiced ? L.cents[(size_t) s] : 0.0f;
        dec.prob = L.voicedProb;

        const int64_t r = d - repairLag;
        if (r >= 0) stageRepair (r);
        const int64_t g = r - gapLag;
        if (g >= 0) stageGap (g);
        const int64_t sm = g - smoothLag;
        if (sm >= 0) stageSmooth (sm);
        const int64_t tt = sm - targetLag;
        if (tt >= 0) stageTarget (tt);
    }

    void PitchTracker::stepViterbi (const FrameObservation& obs)
    {
        const int64_t t = numPushed;
        auto& L = lattice[(size_t) idx (t)];
        L.numVoiced = std::min (obs.numCands, maxCandidates);
        L.voicedProb = std::clamp (obs.voicedProb, 0.0f, 1.0f);
        L.rmsDb = obs.rmsDb;
        L.rawHz = (obs.numCands > 0 && obs.voicedProb > 0.0f) ? obs.cands[0].hz : 0.0f;

        float salSum = 0.0f;
        for (int i = 0; i < L.numVoiced; ++i)
            salSum += obs.cands[(size_t) i].salience;

        for (int i = 0; i < L.numVoiced; ++i)
        {
            L.cents[(size_t) i] = hzToCents (obs.cands[(size_t) i].hz);
            const float share = salSum > 0.0f ? obs.cands[(size_t) i].salience / salSum : 0.0f;
            // likelihood of "pitched, and it's this candidate"
            L.emission[(size_t) i] = -std::log (L.voicedProb * (0.25f + 0.75f * share) * std::max (obs.cands[(size_t) i].salience, 0.02f) + 1.0e-3f);
        }
        L.emission[(size_t) numStates - 1] = -std::log (1.0f - L.voicedProb + 1.0e-3f);

        const float J = 8.0f;
        std::array<float, numStates> newCost;
        std::array<float, numStates> newCents;
        newCost.fill (bigCost);
        newCents.fill (0.0f);

        if (t == 0)
        {
            for (int j = 0; j < L.numVoiced; ++j)
            {
                newCost[(size_t) j] = L.emission[(size_t) j];
                newCents[(size_t) j] = L.cents[(size_t) j];
                L.back[(size_t) j] = (uint8_t) (numStates - 1);
            }
            newCost[(size_t) numStates - 1] = L.emission[(size_t) numStates - 1];
            L.back[(size_t) numStates - 1] = (uint8_t) (numStates - 1);
        }
        else
        {
            const auto& P = lattice[(size_t) idx (t - 1)];
            const int uv = numStates - 1;

            auto voicedTrans = [&] (float dc)
            {
                const float a = std::abs (dc);
                const float z = a / sigmaCents;
                return std::min (0.5f * z * z, J) + octaveCost * std::round (a / 1200.0f);
            };

            for (int j = 0; j < L.numVoiced; ++j)
            {
                const float cj = L.cents[(size_t) j];
                float best = bigCost;
                int arg = uv;

                for (int i = 0; i < P.numVoiced; ++i)
                {
                    const float c = cost[(size_t) i];
                    if (c >= bigCost) continue;
                    const float v = c + voicedTrans (cj - stateCents[(size_t) i]);
                    if (v < best) { best = v; arg = i; }
                }

                // from unvoiced: onset cost, softly continuous with the pitch before the gap
                if (cost[(size_t) uv] < bigCost)
                {
                    float v = cost[(size_t) uv] + 2.0f;
                    if (stateCents[(size_t) uv] != 0.0f)
                    {
                        const float z = std::abs (cj - stateCents[(size_t) uv]) / (sigmaCents * 6.0f);
                        v += std::min (0.5f * z * z, 2.0f);
                    }
                    if (v < best) { best = v; arg = uv; }
                }

                newCost[(size_t) j] = best + L.emission[(size_t) j];
                newCents[(size_t) j] = cj;
                L.back[(size_t) j] = (uint8_t) arg;
            }

            // to unvoiced
            {
                float best = cost[(size_t) uv];
                int arg = uv;
                for (int i = 0; i < P.numVoiced; ++i)
                {
                    const float v = cost[(size_t) i] + 2.0f;
                    if (v < best) { best = v; arg = i; }
                }
                newCost[(size_t) uv] = best + L.emission[(size_t) uv];
                newCents[(size_t) uv] = stateCents[(size_t) arg];   // carry the last pitch through the gap
                L.back[(size_t) uv] = (uint8_t) arg;
            }
        }

        // states beyond this frame's candidates are invalid
        for (int j = L.numVoiced; j < numStates - 1; ++j)
        {
            newCost[(size_t) j] = bigCost;
            L.back[(size_t) j] = (uint8_t) (numStates - 1);
        }

        float mn = bigCost;
        for (auto c : newCost) mn = std::min (mn, c);
        for (int i = 0; i < numStates; ++i)
            cost[(size_t) i] = newCost[(size_t) i] >= bigCost ? bigCost : newCost[(size_t) i] - mn;
        stateCents = newCents;
    }

    //==========================================================================
    void PitchTracker::stageRepair (int64_t r)
    {
        const auto& in = decided[(size_t) idx (r)];
        auto& out = repaired[(size_t) idx (r)];
        out = in;
        if (! in.voiced)
            return;

        // local median of voiced frames
        const int span = harmonicDetector ? repairLag : repairLag / 2;
        int n = 0;
        for (int64_t f = std::max<int64_t> (0, r - span); f <= r + span; ++f)
        {
            const auto& d = decided[(size_t) idx (f)];
            if (d.voiced && n < (int) scratch.size())
                scratch[(size_t) n++] = d.cents;
        }
        const float m = medianOf (scratch.data(), n);

        auto fold = [m] (float c)
        {
            for (int pass = 0; pass < 2; ++pass)
                for (int k : { 1, -1, 2, -2 })
                    if (std::abs (c - m - 1200.0f * (float) k) < 150.0f)
                        c -= 1200.0f * (float) k;
            return c;
        };

        // median-5 of octave-repaired neighbours
        n = 0;
        for (int64_t f = std::max<int64_t> (0, r - 2); f <= r + 2; ++f)
        {
            const auto& d = decided[(size_t) idx (f)];
            if (d.voiced)
                scratch[(size_t) n++] = fold (d.cents);
        }
        out.cents = n > 0 ? medianOf (scratch.data(), n) : fold (in.cents);
    }

    void PitchTracker::stageGap (int64_t g)
    {
        const auto& in = repaired[(size_t) idx (g)];
        auto& out = filled[(size_t) idx (g)];

        if (in.voiced)
        {
            out = { in.cents, std::max (in.prob, 0.2f), Voiced, 0 };
            lastVoicedRepaired = g;
            return;
        }

        // next voiced frame within the gap-fill horizon (all of these are already repaired)
        int64_t q = -1;
        for (int64_t f = g + 1; f <= g + std::max (gapFrames, 1) && f <= g + gapLag; ++f)
            if (repaired[(size_t) idx (f)].voiced) { q = f; break; }

        const int64_t p = lastVoicedRepaired;
        const bool havePrev = p >= 0 && g - p <= (int64_t) ringSize / 2;

        if (havePrev && q >= 0 && (q - p - 1) <= gapFrames)
        {
            const float a = repaired[(size_t) idx (p)].cents;
            const float b = repaired[(size_t) idx (q)].cents;
            const float fr = (float) (g - p) / (float) (q - p);
            out = { a + (b - a) * fr, 0.3f, Interp, 0 };
        }
        else if (q >= 0 && (! havePrev || g - p > gapFrames))
        {
            // pre-roll: the next sound starts soon, set its correction up in advance
            out = { repaired[(size_t) idx (q)].cents, 0.05f, Held, 0 };
        }
        else if (havePrev)
        {
            out = { repaired[(size_t) idx (p)].cents, 0.05f, Held, (int) (g - p) };
        }
        else
        {
            out = { 0.0f, 0.0f, None, 0 };
        }
    }

    void PitchTracker::stageSmooth (int64_t s)
    {
        const auto& in = filled[(size_t) idx (s)];
        auto& out = smoothed[(size_t) idx (s)];
        out = in;
        if (in.flag == None || smoothSigmaFrames < 0.3f)
            return;

        const int K = std::min (smoothLag, (int) std::ceil (3.0f * smoothSigmaFrames));
        const float inv2s2 = 1.0f / (2.0f * smoothSigmaFrames * smoothSigmaFrames);
        double sum = 0.0, wsum = 0.0;
        for (int k = -K; k <= K; ++k)
        {
            const int64_t f = s + k;
            if (f < 0) continue;
            const auto& v = filled[(size_t) idx (f)];
            if (v.flag == None) continue;
            const double w = std::exp (-(float) (k * k) * inv2s2) * v.weight;
            sum += w * v.cents;
            wsum += w;
        }
        if (wsum > 0.0)
            out.cents = (float) (sum / wsum);
    }

    bool PitchTracker::guideOverrides (const Filled& own, float guideCents)
    {
        // The guide repairs hops onto another partial; where the plugin's own track is on the same line
        // (within a semitone and a half), the own track's finer detail is kept.
        constexpr float sameLineCents = 150.0f;
        return own.flag == None || std::abs (guideCents - own.cents) > sameLineCents;
    }

    bool PitchTracker::registerOffset (float ownMinusGuide, float& offset)
    {
        // The guide may follow another member of the same family of partials (an octave, a fifth below...).
        // Its shape is what matters; the register stays the plugin's: snap to the nearest simple ratio.
        static constexpr float ratios[] = { -2400.0f, -1902.0f, -1200.0f, -702.0f, 0.0f, 702.0f, 1200.0f, 1902.0f, 2400.0f };
        float best = 0.0f;
        for (float r : ratios)
            if (std::abs (ownMinusGuide - r) < std::abs (ownMinusGuide - best))
                best = r;
        offset = best;
        return std::abs (ownMinusGuide - best) < 150.0f;   // no simple ratio: the guide stays out of this stretch
    }

    bool PitchTracker::guideMayLead (const Filled& own, float guideCents) const
    {
        if (! guideOverrides (own, guideCents))
            return false;               // same line: keep the own track's detail
        if (guideJumped)
            return false;               // the guide left the line: trust the own track
        return own.flag == None || ownJumped;
    }

    bool PitchTracker::targetTrack (int64_t f, float& cents, float& weight) const
    {
        const auto& v = smoothed[(size_t) idx (f)];
        float g = 0.0f;
        if (guide != nullptr && guideOffsetSet && guideUsable && lattice[(size_t) idx (f)].rmsDb >= settings.gateDb
            && guide->lookup (f, g) && guideMayLead (v, g + guideOffset))
        {
            cents = g + guideOffset;
            weight = 1.0f;
            return true;
        }
        cents = v.cents;
        weight = v.weight;
        return v.flag == Voiced;
    }

    void PitchTracker::stageTarget (int64_t t)
    {
        const auto& tr = smoothed[(size_t) idx (t)];
        const auto& L = lattice[(size_t) idx (t)];

        const float releaseFrames = (float) (0.25 / hopSec);
        bool active = tr.flag != None && ! (tr.flag == Held && (float) tr.heldFor > releaseFrames);
        float trackCents = tr.cents;
        bool guided = false;
        if (guide != nullptr)
        {
            // The hindsight line, where there is one, is the pitch being flattened.
            // A guided stretch takes its register from the plugin's own track at its first voiced frame, and
            // ends after a second without the guide (also for the lookahead reads in targetTrack).
            if (t - lastGuideFrame > (int64_t) (1.0 / hopSec))
            {
                guideOffsetSet = false;
                ownJumped = guideJumped = false;
            }
            float g = 0.0f;
            const bool haveGuide = guide->lookup (t, g);
            ownHistory[(size_t) idx (t)] = tr.flag != None ? tr.cents : 0.0f;
            if (! haveGuide)
                lastGuideCents = 0.0f;
            if (haveGuide)
            {
                lastGuideFrame = t;
                if (! guideOffsetSet && tr.flag == Voiced)
                {
                    guideUsable = registerOffset (tr.cents - g, guideOffset);
                    guideOffsetSet = true;
                }
                if (guideOffsetSet && guideUsable)
                {
                    const float gc = g + guideOffset;
                    if (tr.flag != None && std::abs (gc - tr.cents) <= 150.0f)
                        ownJumped = guideJumped = false;   // back on the same line
                    else
                    {
                        // Which of the two left the line? The own track is smoothed, so a hop shows over ~30 ms (or
                        // across a gap, against its last voiced value); the guide is not, so a hop of its own is a
                        // step from one frame to the next, which a fast glide never makes.
                        const int64_t back = t - 6;
                        const float ownBefore = back >= 0 && ownHistory[(size_t) idx (back)] != 0.0f ? ownHistory[(size_t) idx (back)]
                                                                                                      : lastOwnVoiced;
                        if (tr.flag != None && ownBefore != 0.0f && std::abs (tr.cents - ownBefore) > 150.0f)
                            ownJumped = true;
                        if (lastGuideCents != 0.0f && std::abs (gc - lastGuideCents) > 60.0f)
                            guideJumped = true;
                    }
                    lastGuideCents = gc;
                    // Never past the Gate: a frame the user gated out stays uncorrected, whatever the guide says.
                    guided = L.rmsDb >= settings.gateDb && guideMayLead (tr, gc);
                }
            }
            if (tr.flag == Voiced || tr.flag == Interp)
                lastOwnVoiced = tr.cents;
            if (guided)
                trackCents = g + guideOffset;
            active = active || guided;
            guide->markUsed (t);
        }

        // --- latch state -----------------------------------------------------
        // Manual Override replaces the target, so nothing is latched while it is on: turning it off
        // latches afresh to the next sound.
        if (relatchPending || settings.manualOverride)
        {
            latched = false;
            relatchPending = false;
        }

        if (tr.flag == Voiced || tr.flag == Interp || guided)
            latchUnvoicedRun = 0;
        else if (++latchUnvoicedRun > (int64_t) (1.0 / hopSec))
            latched = false;    // a new sound after >1 s of silence re-latches

        float target = trackCents;
        if (settings.manualOverride)
        {
            // Manual Override: the detected pitch is flattened to Override Hz, whatever the target mode.
            target = hzToCents (settings.overrideHz);
        }
        else switch (settings.targetMode)
        {
            case TargetMode::Fixed:
                target = hzToCents (settings.targetHz);
                break;

            case TargetMode::Auto:
            {
                const int K = autoHalfFrames;
                const float sig = std::max (1.0f, 0.5f * (float) K);
                const float inv2s2 = 1.0f / (2.0f * sig * sig);
                double sum = 0.0, wsum = 0.0;
                for (int k = -K; k <= K; ++k)
                {
                    const int64_t f = t + k;
                    if (f < 0) continue;
                    float c = 0.0f, wt = 0.0f;
                    if (! targetTrack (f, c, wt) && smoothed[(size_t) idx (f)].flag != Interp) continue;
                    const double w = std::exp (-(float) (k * k) * inv2s2) * wt;
                    sum += w * c;
                    wsum += w;
                }
                target = wsum > 0.0 ? (float) (sum / wsum) : trackCents;
                break;
            }

            case TargetMode::Latch:
            case TargetMode::NumModes:
            default:
            {
                if (! latched && active)
                {
                    // Latch before the sound plays: from its very first tracked frame (including the lead-in the
                    // gap fill bridges before the detector is sure), look ahead to its first confident frame and
                    // lock to the median of the ~300 ms from there. Otherwise the start of every sound would play
                    // uncorrected until that first confident frame came round, then snap to the latch.
                    int first = -1;
                    float c = 0.0f, wt = 0.0f;
                    for (int k = 0; k <= targetLag && first < 0; ++k)
                        if (targetTrack (t + k, c, wt))
                            first = k;

                    if (first >= 0)
                    {
                        const int last = std::min (targetLag, first + (int) std::lround (0.3 / hopSec));
                        const int stride = std::max (1, (last - first) / 60);
                        int n = 0;
                        for (int k = first; k <= last && n < (int) scratch.size(); k += stride)
                        {
                            if (targetTrack (t + k, c, wt))
                                scratch[(size_t) n++] = c;
                        }
                        latchedCents = n > 0 ? medianOf (scratch.data(), n) : trackCents;
                        latched = true;
                    }
                }
                target = latched ? latchedCents : trackCents;
                break;
            }
        }

        float r = 0.0f;
        if (active)
        {
            const float maxC = settings.maxShiftSemitones * 100.0f;
            r = std::clamp ((target - trackCents) * std::clamp (settings.amount, 0.0f, 1.0f), -maxC, maxC);
        }
        else
        {
            // relax towards no correction (~25 ms time constant)
            r = lastRatio * 0.8f;
            if (std::abs (r) < 0.5f) r = 0.0f;
        }

        ratio[(size_t) idx (t)] = r;
        lastRatio = r;

        telemetry.frame = t;
        telemetry.rawHz = L.rawHz;
        telemetry.trackHz = active ? centsToHz (trackCents) : 0.0f;
        telemetry.targetHz = active ? centsToHz (target) : 0.0f;
        telemetry.ratioCents = r;
        telemetry.voicedProb = L.voicedProb;
        telemetry.rmsDb = L.rmsDb;
    }

    float PitchTracker::ratioCents (int64_t f) const
    {
        if (f < 0 || f > lastFinalFrame())
            return 0.0f;
        return ratio[(size_t) idx (f)];
    }
}
