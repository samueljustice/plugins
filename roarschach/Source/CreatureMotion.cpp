#include "CreatureMotion.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>

namespace roar
{
    namespace
    {
        constexpr float c_twoPi = 6.28318530718f;

        float k (float dt, float tau) { return 1.0f - std::exp (-dt / std::max (tau, 1.0e-4f)); }

        float clean (float v, float lo, float hi) { return std::isfinite (v) ? std::min (hi, std::max (lo, v)) : 0.0f; }

        float cleanDt (float dt, float hi) { return std::isfinite (dt) ? std::min (hi, std::max (0.0f, dt)) : 0.0f; }

        void follow (float& s, float target, float dt, float up, float down)
        {
            s += (target - s) * k (dt, target > s ? up : down);
            if (! std::isfinite (s))
                s = 0.0f;
        }
    }

    //==============================================================================================
    void JawMotion::step (float dt, float x, float xf, float heat, float sizeJ)
    {
        dt = cleanDt (dt, 0.1f);
        x = clean (x, 0.0f, 1.0f);
        xf = clean (xf, 0.0f, 1.0f);
        heat = clean (heat, 0.0f, 1.0f);
        sizeJ = clean (sizeJ, 0.0f, 1.0f);
        justEngaged = false;

        if (dt <= 0.0f)
            return;

        const float prevP = P;
        quiet = xf < 0.15f ? std::min (quiet + dt, 60.0f) : 0.0f;

        // The phrase level: quick up, held, slow down inside a phrase, let go in real silence.
        if (x >= P)
        {
            P += (x - P) * k (dt, 0.03f);
            hold = 0.20f;
        }
        else
        {
            hold = std::max (hold - dt, -1.0f);
            if (quiet >= 0.15f)
                P += (xf - P) * k (dt, 0.22f);
            else if (hold <= 0.0f)
                P += (x - P) * k (dt, 0.90f);
        }

        // The syllable shape: two one-poles at 2.5 Hz.
        S1 += (x - S1) * k (dt, 0.064f);
        S2 += (S1 - S2) * k (dt, 0.064f);
        const float S = S2;

        const bool was = engaged;
        engaged = engaged ? (P >= c_closeThreshold) : (P > c_openThreshold);
        justEngaged = engaged && ! was;
        dPdt = (P - prevP) / dt;
        // An onset (the switch engaging, or the phrase level jumping): the jaw drops fast, so the
        // mouth is open while the sound still is; the slow spring takes over once it gets there.
        if (justEngaged || (engaged && dPdt > 3.0f))
            attack = 0.35f;
        else
            attack = std::max (0.0f, attack - dt);

        // The roar hold: a long loud stretch keeps the mouth open through short dips.
        if (xf > 0.72f || heat > 0.5f)
            roarT = std::min (0.5f, roarT + dt);
        else
            roarT = std::max (0.0f, roarT - (xf > 0.4f ? 2.0f : 8.0f) * dt);
        roarHold = roarT > 0.30f ? 0.45f : std::max (0.0f, roarHold - dt);
        if (quiet > 0.12f)
            roarHold = 0.0f;

        // The target: the phrase sets most of it, the syllables shape it (slowly), and the level is
        // spread so a soft phrase opens little and a shout opens wide.
        float T = 0.0f;
        if (engaged)
        {
            const float raw = 0.62f * P + 0.38f * std::min (S, P);
            T = 0.08f + 0.92f * std::min (1.0f, std::max (0.0f, (raw - 0.30f) / 0.56f));
        }
        if (roarHold > 0.0f)
            T = std::max (T, 0.08f + 0.92f * std::min (1.0f, std::max (0.0f, (0.95f * P - 0.30f) / 0.56f)));

        // The spring, in substeps of at most 4 ms.
        const float fOpen = 1.6f + (1.0f - 1.6f) * sizeJ;
        const float fAttack = 5.6f + (5.0f - 5.6f) * sizeJ;
        const float fClose = 1.45f * fOpen;
        const int n = std::min (16, std::max (1, (int) std::ceil (dt / 0.004f)));
        const float h = dt / (float) n;
        for (int i = 0; i < n; ++i)
        {
            const bool opening = T > y;
            if (opening && attack > 0.0f && y > 0.97f * T)
                attack = 0.0f;   // (there: the heavy spring carries on)
            const bool fastOpen = opening && attack > 0.0f;
            const float w = c_twoPi * (opening ? (fastOpen ? fAttack : fOpen) : fClose);
            const float zeta = opening ? 1.00f : 0.72f;
            const float a = w * w * (T - y) - 2.0f * zeta * w * v;
            v = std::min (fastOpen ? c_vMaxAttack : c_vMax, std::max (c_vMin, v + a * h));
            y += v * h;
            if (y < 0.0f)
            {
                y = 0.0f;
                v = -0.25f * v;   // the teeth meet: a small inelastic rebound
            }
            y = std::min (y, 1.05f);
        }

        // The lips carry the syllables; the chin never does.
        Lf += (xf - Lf) * k (dt, xf > Lf ? 0.015f : 0.040f);
        lip = std::min (1.0f, std::max (-1.0f, (Lf - S) * 4.0f));

        if (! (std::isfinite (y) && std::isfinite (v) && std::isfinite (P) && std::isfinite (S1) && std::isfinite (S2)
               && std::isfinite (Lf) && std::isfinite (lip)))
            reset();
    }

    //==============================================================================================
    float HeadMotion::breathHz (float sleep, float phrase) const
    {
        const float s = clean (sleep, 0.0f, 1.0f);
        return (0.20f + (0.11f - 0.20f) * s) * (1.0f - 0.7f * clean (phrase, 0.0f, 1.0f));
    }

    void HeadMotion::step (float dt, float jawY, float phrase, bool justEngaged, float dPdt, float roarHold,
                           float growl, float sleep)
    {
        dt = cleanDt (dt, 0.1f);
        jawY = clean (jawY, 0.0f, 1.1f);
        phrase = clean (phrase, 0.0f, 1.0f);
        dPdt = clean (dPdt, -100.0f, 100.0f);
        roarHold = clean (roarHold, 0.0f, 1.0f);
        growl = clean (growl, 0.0f, 1.0f);
        sleep = clean (sleep, 0.0f, 1.0f);
        const float awake = 1.0f - sleep;

        if (dt <= 0.0f)
            return;

        clock = std::fmod (clock + dt, 4096.0f);

        // Lag: the head tips back as the jaw opens (and a little with the growl), lunging on an onset.
        if (justEngaged && dPdt > 2.0f)
            lagV += -0.03f;
        const float target = -0.010f * jawY - 0.002f * growl;
        const float w = c_twoPi * 0.8f, zeta = 0.55f;
        const int n = std::min (16, std::max (1, (int) std::ceil (dt / 0.008f)));
        const float h = dt / (float) n;
        for (int i = 0; i < n; ++i)
        {
            lagV += (w * w * (target - lag) - 2.0f * zeta * w * lagV) * h;
            lag += lagV * h;
        }

        // Breath: slower asleep, nearly held through a loud phrase, one deep breath after a roar.
        phase = std::fmod (phase + dt * c_twoPi * breathHz (sleep, phrase), c_twoPi);
        if (prevRoarHold > 0.0f && roarHold <= 0.0f)
            deep = 1.0f;
        prevRoarHold = roarHold;
        deep *= std::exp (-dt / 2.0f);
        breath = (std::sin (phase) + 0.25f * std::sin (2.0f * phase - 0.6f)) * (1.0f + deep);

        // Sway (up and down and in scale only: the mirror stays exact).
        const float sway = (0.004f * std::sin (c_twoPi * 0.071f * clock) + 0.003f * std::sin (c_twoPi * 0.113f * clock + 1.7f))
                         * awake * (1.0f - 0.5f * phrase);

        offY = lag + sway;
        scaleAdd = 0.5f * sway;
        follow += (offY - follow) * k (dt, 0.35f);

        if (! (std::isfinite (lag) && std::isfinite (lagV) && std::isfinite (phase) && std::isfinite (follow) && std::isfinite (breath)))
            reset();
    }

    //==============================================================================================
    bool AudioForces::allowFlash()
    {
        int recent = 0;
        size_t oldest = 0;
        for (size_t i = 0; i < flashTimes.size(); ++i)
        {
            if (clock - flashTimes[i] < 1.0f)
                ++recent;
            if (flashTimes[i] < flashTimes[oldest])
                oldest = i;
        }
        if (recent >= c_maxFlashesPerSecond)
            return false;
        flashTimes[oldest] = clock;
        ++flashesAllowed;
        return true;
    }

    void AudioForces::step (float dt, const Inputs& in)
    {
        dt = cleanDt (dt, 0.1f);
        if (dt <= 0.0f)
            return;

        clock += dt;
        if (clock > 3600.0f)
        {
            for (auto& t : flashTimes)
                t -= clock;
            clock = 0.0f;
        }

        const float subRaw = clean (in.subLevel, 0.0f, 1.0f);

        // Sub onsets: a jump over the slow level, at most one per 120 ms (and within the flash limit).
        sinceOnset = std::min (sinceOnset + dt, 10.0f);
        if (subRaw - slowSub > c_subOnsetJump && sinceOnset >= c_subRefractory && in.awake > 0.1f && allowFlash())
        {
            sinceOnset = 0.0f;
            subPulse = 1.5f + 2.5f * subRaw;
            ++onsets;
        }
        subPulse *= std::exp (-dt / 0.12f);
        follow (slowSub, subRaw, dt, c_attack, 0.250f);
        prevSubRaw = subRaw;

        follow (roar, clean (in.roarHeat, 0.0f, 1.0f), dt, c_attack, 0.250f);
        follow (growl, std::abs (clean (in.growlMod, -1.0f, 1.0f)), dt, c_attack, c_release);
        follow (sub, subRaw, dt, c_attack, 0.250f);
        follow (swirl, clean (in.swirlOn, 0.0f, 1.0f), dt, 0.060f, 0.300f);
        follow (ensemble, clean (in.ensemble, 0.0f, 1.0f), dt, 0.150f, 0.300f);
        follow (clip, clean (in.clip, 0.0f, 1.0f), dt, c_attack, c_release);
        follow (strain, clean (in.strain, 0.0f, 1.0f), dt, c_attack, c_release);

        puff = std::min (2.0f, (puff + 4.0f * std::max (clean (in.jawVel, -10.0f, 10.0f), 0.0f) * dt) * std::exp (-dt / 0.4f));

        // Clip sparks: a few a second at most, each a new set of edge points.
        sparkClock += dt;
        clipSparks *= std::exp (-dt / 0.08f);
        if (clip > 0.05f && sparkClock > 0.12f && in.awake > 0.1f && allowFlash())
        {
            sparkClock = 0.0f;
            sparkSeed = (sparkSeed + 1) & 0xffff;
            clipSparks = clip;
        }
    }

    void AudioForces::vortices (double t, float aspect, float strengthScale, std::array<std::array<float, 4>, 6>& out)
    {
        const auto s = [t] (double period, double phase) { return (float) std::sin (6.283185307179586 * t / period + phase); };
        const float half = 0.5f * std::max (1.0f, aspect);

        // Pair A by the wing tips, pair B above the horns: mirror images, opposite spin.
        const float ax = std::min (half - 0.18f, 0.80f + 0.10f * s (37.0, 0.0)), ay = 0.05f + 0.07f * s (53.0, 1.0);
        const float bx = 0.55f + 0.07f * s (71.0, 0.4), by = 0.30f + 0.05f * s (97.0, 2.0);
        const float aS = (0.20f + 0.04f * s (53.0, 2.5)) * strengthScale;
        const float bS = (0.16f + 0.03f * s (37.0, 1.2)) * strengthScale;
        out[0] = { ax, ay, 0.19f, aS };
        out[1] = { -ax, ay, 0.19f, -aS };
        out[2] = { bx, by, 0.13f, -bS };
        out[3] = { -bx, by, 0.13f, bS };
        // Two free ones wander the sky.
        out[4] = { (half - 0.25f) * s (97.0, 0.3), -0.10f + 0.25f * s (71.0, 1.1), 0.24f, 0.17f * strengthScale };
        out[5] = { -(half - 0.35f) * s (53.0, 2.1), 0.12f + 0.22f * s (37.0, 0.7), 0.17f, -0.14f * strengthScale };
    }

    //==============================================================================================
    void QualityGovernor::reset (int start)
    {
        *this = QualityGovernor();
        startTier = std::min (3, std::max (0, start));
        tier = startTier;
    }

    void QualityGovernor::resume (int start, int settled, bool locked)
    {
        reset (start);
        tier = std::min (startTier, std::max (floorTier(), settled));
        upgradesLocked = locked;
        if (tier < startTier)
            cooldown = c_dropCooldownSeconds;
    }

    int QualityGovernor::step (float interval, float target, float gpuMs, bool stalled)
    {
        interval = std::isfinite (interval) ? std::min (10.0f, std::max (0.0f, interval)) : 0.0f;
        target = std::isfinite (target) && target > 0.0f ? target : 1.0f / 30.0f;
        elapsed += interval;

        if (startTier <= 0)
            return tier = 0;

        cooldown = std::max (0.0f, cooldown - interval);
        sinceChange = std::min (sinceChange + interval, 1.0e9f);
        sinceUpgrade = std::min (sinceUpgrade + interval, 1.0e9f);

        if (elapsed < c_warmupSeconds || stalled)
            return tier;

        if (std::isfinite (gpuMs) && gpuMs > 0.0f && gpuMs <= 250.0f)
        {
            ema = emaValid ? 0.9f * ema + 0.1f * gpuMs : gpuMs;
            emaValid = true;
        }

        // With a GPU timer the GPU's own cost decides; the interval also measures the host's message
        // thread, so it decides only without one.
        const bool slowInterval = interval > c_slowIntervalRatio * target;
        const bool slow = emaValid ? ema > c_slowMs : slowInterval;
        const bool fast = emaValid ? ema < c_fastMs : interval <= 1.1f * target;
        slowFrames = slow ? slowFrames + 1 : 0;
        fastFrames = fast ? fastFrames + 1 : 0;

        const bool failedUpgrade = sinceUpgrade < c_failedUpgradeSeconds;
        // (far over budget, the frames are long: half as many of them decide)
        const bool farOver = emaValid && ema > c_farOverRatio * c_slowMs;
        const int needed = farOver ? c_slowFramesToDrop / 2 : c_slowFramesToDrop;
        if (slowFrames >= needed && (cooldown <= 0.0f || failedUpgrade) && tier > floorTier())
        {
            if (failedUpgrade)
                upgradesLocked = true;
            const int by = farOver ? 2 : 1;
            tier = std::max (floorTier(), tier - by);
            cooldown = c_dropCooldownSeconds;
            sinceChange = 0.0f;
            sinceUpgrade = 1.0e9f;
            slowFrames = fastFrames = 0;
            emaValid = false;   // (the new tier's first reading replaces it: no second drop on the old tier's cost)
            ++changes;
        }
        else if (fastFrames >= c_fastFramesToRise && sinceChange >= c_cooldownSeconds && ! upgradesLocked && tier < startTier)
        {
            ++tier;
            sinceChange = 0.0f;
            sinceUpgrade = 0.0f;
            slowFrames = fastFrames = 0;
            ++changes;
        }
        return tier;
    }

    // Integrated and entry-level parts start lower than the discrete ones in the same family.
    float MistMotion::target (float sleep, float heat, float level)
    {
        const auto c01 = [] (float x) { return std::isfinite (x) ? std::min (1.0f, std::max (0.0f, x)) : 0.0f; };
        const float s = c01 (sleep), awake = 1.0f - s;
        const float loud = c01 (0.8f * c01 (heat) + 0.35f * std::max (0.0f, c01 (level) - 0.4f));
        const float awakeA = c_awake + (c_roar - c_awake) * loud * loud * (3.0f - 2.0f * loud);
        return std::max (c_min, awakeA * awake + c_asleep * s);
    }

    float MistMotion::step (float dt, float sleep, float heat, float level, double clock)
    {
        const float tgt = target (sleep, heat, level);
        const float d = std::isfinite (dt) ? std::min (0.25f, std::max (0.0f, dt)) : 0.0f;
        if (opacity < 0.0f || ! std::isfinite (opacity))
            opacity = tgt;
        else
            opacity += (tgt - opacity) * (1.0f - std::exp (-d / (tgt < opacity ? c_thinSeconds : c_closeSeconds)));
        // a slow wander: mostly sideways, a little rising (view heights per second)
        const double c = std::isfinite (clock) ? clock : 0.0;
        drift[0] = std::fmod (drift[0] + d * (0.010 + 0.004 * std::sin (c * 0.031)), 4096.0);
        drift[1] = std::fmod (drift[1] + d * (0.004 + 0.003 * std::cos (c * 0.023)), 4096.0);
        return opacity;
    }

    int QualityGovernor::startTierFor (const char* renderer, const char* vendor)
    {
        std::string s = std::string (renderer != nullptr ? renderer : "") + " " + (vendor != nullptr ? vendor : "");
        for (auto& c : s)
            c = (char) std::tolower ((unsigned char) c);
        const auto has = [&s] (const char* w) { return s.find (w) != std::string::npos; };

        if (has ("llvmpipe") || has ("softpipe") || has ("gdi generic") || has ("microsoft basic render") || has ("software")
            || has ("swiftshader"))
            return 0;
        // AMD APUs ("AMD Radeon(TM) Graphics", Vega 3/8/11 and the like): integrated
        if (has ("radeon(tm) graphics") || has ("radeon graphics") || has ("radeon(tm) vega") || has ("radeon vega")
            || (has ("vega") && ! has ("vega 56") && ! has ("vega 64") && ! has ("radeon vii") && ! has ("pro vega") && ! has ("vega frontier"))
            || has ("radeon r5") || has ("radeon r7"))
            return 1;
        // the entry-level Radeon Pro 4xx / 5xx of older Intel Macs (three digits, not the 5x00 series)
        for (size_t at = s.find ("radeon pro "); at != std::string::npos; at = s.find ("radeon pro ", at + 1))
        {
            const size_t d = at + 11;
            const auto digit = [&s] (size_t i) { return i < s.size() && std::isdigit ((unsigned char) s[i]) != 0; };
            if (d + 2 < s.size() && (s[d] == '4' || s[d] == '5') && digit (d + 1) && digit (d + 2) && ! digit (d + 3))
                return 2;
        }
        if (has ("apple") || has ("nvidia") || has ("geforce") || has ("quadro") || has ("amd") || has ("radeon") || has ("ati "))
            return 3;
        if (has ("intel"))
            return ((has ("iris") && has ("xe")) || has (" arc")) ? 2 : 1;
        return 2;
    }
}
