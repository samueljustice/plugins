#include "SwirlFx.h"

#include <algorithm>
#include <cmath>

namespace
{
    constexpr double c_pi = 3.14159265358979323846;

    /** The wet path's soft clip: 2 tanh(x / 2), unity for small sounds, never past 2. */
    inline float softClip(float x) { return 2.0f * sj::fastTanh(0.5f * x); }

    /** What a one-pole of `seconds` moves by in `dt`. */
    inline double glide(double dt, double seconds) { return 1.0 - std::exp(-dt / seconds); }
}

//==============================================================================
float SwirlFx::mixFor(float knob)
{
    return 0.5f * std::pow(sj::clamp(knob, 0.0f, 1.0f), 0.7f);
}

double SwirlFx::leanSpeed(double varispeed)
{
    const double v = std::clamp(varispeed, -1.0, 1.0);
    const double a = std::abs(v);
    if (a <= c_handDead)
        return 0.0;
    const double s = c_handSpeed * std::pow((a - c_handDead) / (1.0 - c_handDead), 1.5);
    return v < 0.0 ? -s : s;
}

double SwirlFx::ownShape(double phase)
{
    const double p = phase - std::floor(phase);
    const double s = std::sin(c_pi * p);
    return std::sin(2.0 * c_pi * (p - c_ownWarp * s * s));
}

double SwirlFx::wanderRangeMs(double rateHz)
{
    return std::min(c_wanderMaxMs, c_wanderMsHz / std::max(0.01, rateHz));
}

double SwirlFx::thickenGain(double delayMs)
{
    if (delayMs >= c_thickenEndMs)
        return 0.0;
    return -c_thickenGain * std::pow(1.0 - std::max(0.0, delayMs) / c_thickenEndMs, c_thickenPower);
}

double SwirlFx::stops(double offsetMs)
{
    const double limit = static_cast<double>(c_maxOffsetMs);
    const double knee = limit - c_stopKneeMs;
    const double a = std::abs(offsetMs);
    if (a <= knee)
        return offsetMs;
    const double eased = limit - c_stopKneeMs * std::exp(-(a - knee) / c_stopKneeMs);
    return offsetMs < 0.0 ? -eased : eased;
}

//==============================================================================
void SwirlFx::prepare(double sampleRate)
{
    m_sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    m_drySamples = static_cast<int>(std::lround(c_dryMs * 0.001 * m_sampleRate));
    const int lineLength = static_cast<int>(std::ceil(c_lineSeconds * m_sampleRate)) + 8;
    for (auto& lane : m_lane)
    {
        lane.line.prepare(lineLength);
        lane.dry.assign(static_cast<size_t>(m_drySamples), 0.0f);
        for (auto& pole : lane.wetLp)
            pole.set(static_cast<float>(c_lowPassHz), static_cast<float>(m_sampleRate));
        for (auto& pole : lane.loopLp)
            pole.set(static_cast<float>(c_lowPassHz), static_cast<float>(m_sampleRate));
    }
    for (auto& side : m_side)
        side.assign(static_cast<size_t>(m_drySamples), 0.0f);
    // A one-pole y += a (x - y) is (1 - a) / a samples late at low frequencies.
    const double a = static_cast<double>(m_lane[0].wetLp[0].a);
    m_lowPassDelay = 2.0 * (1.0 - a) / a;
    m_dirty = true;
    reset();
}

void SwirlFx::clearSound()
{
    if (! m_dirty)
        return;
    for (auto& lane : m_lane)
    {
        lane.line.reset();
        std::fill(lane.dry.begin(), lane.dry.end(), 0.0f);
        lane.dryAt = 0;
        for (auto& pole : lane.wetLp)
            pole.reset();
        for (auto& pole : lane.loopLp)
            pole.reset();
    }
    for (auto& side : m_side)
        std::fill(side.begin(), side.end(), 0.0f);
    m_sideAt = 0;
    m_dirty = false;
}

void SwirlFx::reset()
{
    clearSound();
    m_kind = None;
    m_snap = true;
    m_hand = 0.0;
    m_speed = 0.0;
    m_recentre = false;
    m_startPhase = 0.0f;
    m_ownPhase = m_wavePhase = 0.0;
    for (auto& lane : m_lane)
        lane.offsetMs = 0.0;
}

void SwirlFx::scatter(float phase)
{
    m_startPhase = sj::clamp(phase, 0.0f, 1.0f);
    m_ownPhase = m_wavePhase = static_cast<double>(m_startPhase);
}

void SwirlFx::begin(Kind kind)
{
    if (m_kind != kind)
    {
        // Straight from the other type: nothing of its sound is left in the lines. The held offset stays.
        clearSound();
        m_kind = kind;
        m_snap = true;
    }
    m_dirty = true;
}

//==============================================================================
void SwirlFx::leanHand(double varispeed, double dt)
{
    const double limit = static_cast<double>(c_maxOffsetMs);
    if (m_recentre)
    {
        // Home, no faster than a full lean, easing in over the last of it.
        m_speed = 0.0;
        const double most = c_handSpeed * 1000.0 * dt;
        const double step = std::clamp(-m_hand * glide(dt, 0.06), -most, most);
        m_hand += step;
        if (std::abs(m_hand) < 1.0e-3)
        {
            m_hand = 0.0;
            m_recentre = false;
        }
        return;
    }

    const double target = leanSpeed(varispeed);
    const double before = m_speed;
    m_speed += (target - m_speed) * glide(dt, c_handSeconds);
    if (target == 0.0 && std::abs(m_speed) < 1.0e-8)
        m_speed = 0.0;
    // Faster means earlier. Trapezoid over the call.
    double step = -0.5 * (before + m_speed) * dt * 1000.0;
    const double easeFrom = limit * (1.0 - c_handEase);
    const bool outward = (step > 0.0 && m_hand > 0.0) || (step < 0.0 && m_hand < 0.0);
    if (outward && std::abs(m_hand) > easeFrom)
        step *= std::max(0.0, (limit - std::abs(m_hand)) / (limit - easeFrom));
    m_hand = std::clamp(m_hand + step, -limit, limit);
}

void SwirlFx::processTape(const Tape& settings, float knob, const Lanes& lanes, int frames)
{
    if (frames <= 0)
        return;
    begin(TapeFlanger);

    const double fs = m_sampleRate;
    const double dt = static_cast<double>(frames) / fs;
    const double msToSamples = 0.001 * fs;
    const int count = std::clamp(lanes.count, 1, c_lanes);

    leanHand(static_cast<double>(settings.varispeed), dt);

    const double depth = std::clamp(static_cast<double>(settings.depthMs), 0.0, static_cast<double>(c_maxOffsetMs));
    const double rate = std::clamp(static_cast<double>(settings.rateHz), 0.01, 4.0);
    const double wobble = std::clamp(static_cast<double>(settings.wobble), 0.0, 1.0);
    double own = 0.0;
    if (settings.motion == OnItsOwn)
    {
        if (! m_snap)
        {
            m_ownPhase += rate * dt;
            m_ownPhase -= std::floor(m_ownPhase);
        }
        own = std::min(depth, c_ownSwingMsHz / rate) * ownShape(m_ownPhase);
    }

    const float m = mixFor(knob) * (0.5f * sj::c_pi);
    const float dryTo = std::cos(m);
    const float wetTo = settings.invert ? -std::sin(m) : std::sin(m);
    const float loopTo = static_cast<float>(c_tapeFeedback) * sj::clamp(settings.feedback, -1.0f, 1.0f);
    if (m_snap)
    {
        m_dryGain = dryTo;
        m_wetGain = wetTo;
    }
    const float dryFrom = m_dryGain, wetFrom = m_wetGain;

    const double kFast = glide(dt, c_settleSeconds);
    const double omega = 2.0 * c_pi * c_ringHz;
    const double ringMax = c_ringMaxMs;
    const double limit = static_cast<double>(c_maxOffsetMs);
    const double floorSamples = std::max(3.0, c_feedbackFloorMs * msToSamples);
    const double base = static_cast<double>(m_drySamples) - m_lowPassDelay;
    const float step = 1.0f / static_cast<float>(frames);
    const double stepD = 1.0 / static_cast<double>(frames);

    for (int l = 0; l < c_lanes; ++l) // every lane's motion runs, so one that joins later is already in place
    {
        auto& lane = m_lane[static_cast<size_t>(l)];

        // Where this lane's copy is at the end of the call.
        double motion = own;
        if (settings.motion == WithPass)
            motion = depth * (1.0 - c_passLead * std::clamp(static_cast<double>(lanes.passNearTo[static_cast<size_t>(l)]), 0.0, 1.25));
        else if (settings.motion != OnItsOwn)
            motion = 0.0;
        const double target = motion + m_hand;
        if (m_snap)
        {
            lane.fast1 = lane.fast2 = lane.ringAt = target;
            lane.ringSpeed = 0.0;
        }
        lane.fast1 += (target - lane.fast1) * kFast;
        lane.fast2 += (lane.fast1 - lane.fast2) * kFast;
        // The resonance: a lightly damped spring that follows the settled move; its speed is the ring.
        lane.ringSpeed += (omega * omega * (lane.fast2 - lane.ringAt) - 2.0 * c_ringDamping * omega * lane.ringSpeed) * dt;
        lane.ringAt += lane.ringSpeed * dt;
        const double ring = ringMax * std::tanh(lane.ringSpeed / (omega * ringMax));
        double d = stops(lane.fast2 + wobble * c_ringShare * ring);
        if (l == 1)
            d *= 1.0 - c_laneBend * d / limit;
        else if (l == 2)
            d *= 1.0 + c_laneBend * d / limit;

        const double delayTo = base + d * msToSamples;
        if (m_snap)
        {
            lane.delay = delayTo;
            lane.offsetMs = d;
            lane.loopGain = loopTo;
        }
        const double delayFrom = lane.delay;
        const double offsetFrom = lane.offsetMs * msToSamples, offsetTo = d * msToSamples;
        const float loopFrom = lane.loopGain;
        lane.delay = delayTo;
        lane.offsetMs = d;
        lane.loopGain = loopTo;

        float* x = l < count ? lanes.lane[static_cast<size_t>(l)] : nullptr;
        if (x == nullptr)
        {
            // Nothing in this lane now: silence goes in, so what it held is not there when it comes back.
            for (int i = 0; i < frames; ++i)
            {
                lane.line.push(0.0f);
                lane.dry[static_cast<size_t>(lane.dryAt)] = 0.0f;
                if (++lane.dryAt == m_drySamples)
                    lane.dryAt = 0;
            }
            for (auto& pole : lane.wetLp) pole.reset();
            for (auto& pole : lane.loopLp) pole.reset();
            continue;
        }

        auto& line = lane.line;
        float* ring8 = lane.dry.data();
        int at = lane.dryAt;
        const int ringLength = m_drySamples;
        auto& wet0 = lane.wetLp[0];
        auto& wet1 = lane.wetLp[1];
        const bool looping = loopFrom != 0.0f || loopTo != 0.0f;
        if (! looping)
        {
            lane.loopLp[0].reset();
            lane.loopLp[1].reset();
        }
        for (int i = 0; i < frames; ++i)
        {
            const float t = static_cast<float>(i + 1) * step;
            const double td = static_cast<double>(i + 1) * stepD;
            const float in = x[i];
            float feed = in;
            if (looping)
            {
                // The recirculating comb: as long as the copy is far from the dry, either side.
                const double off = std::abs(offsetFrom + (offsetTo - offsetFrom) * td);
                const float back = line.read(std::max(floorSamples, off) - 1.0);
                feed += (loopFrom + (loopTo - loopFrom) * t) * softClip(lane.loopLp[1].lowpass(lane.loopLp[0].lowpass(back)));
            }
            line.push(feed);
            const float wet = softClip(wet1.lowpass(wet0.lowpass(line.read(delayFrom + (delayTo - delayFrom) * td))));
            const float dry = ring8[at];
            ring8[at] = in;
            if (++at == ringLength)
                at = 0;
            x[i] = (dryFrom + (dryTo - dryFrom) * t) * dry + (wetFrom + (wetTo - wetFrom) * t) * wet;
        }
        lane.dryAt = at;
    }

    m_dryGain = dryTo;
    m_wetGain = wetTo;
    m_snap = false;
}

//==============================================================================
void SwirlFx::processDoubler(const Doubler& settings, float knob, const Lanes& lanes, int frames)
{
    if (frames <= 0)
        return;
    begin(DoublerKind);

    const double fs = m_sampleRate;
    const double dt = static_cast<double>(frames) / fs;
    const double msToSamples = 0.001 * fs;
    const int count = std::clamp(lanes.count, 1, c_lanes);

    const double delay = std::clamp(static_cast<double>(settings.delayMs), 3.0, 75.0);
    const double wander = std::clamp(static_cast<double>(settings.wander), 0.0, 1.0);
    const double rate = std::clamp(static_cast<double>(settings.rateHz), 0.01, 6.0);
    const double range = wanderRangeMs(rate);

    // Where each lane is in its wander (0..1).
    std::array<double, c_lanes> u {};
    if (settings.wave == WaveRandom)
    {
        // White noise through two one-poles at 0.4 x Rate, scaled so what comes out has a spread of 1 whatever
        // the length of the call, then folded into 0..1.
        const double omega = 2.0 * c_pi * c_randomRate * rate;
        const double k = glide(dt, 1.0 / omega);
        const double drive = std::sqrt(3.0 * 4.0 / (omega * dt));
        for (auto& lane : m_lane)
        {
            if (m_snap)
                lane.noise1 = lane.noise2 = 1.2 * static_cast<double>(m_rng.bipolar());
            const double white = drive * static_cast<double>(m_rng.bipolar());
            lane.noise1 += (white - lane.noise1) * k;
            lane.noise2 += (lane.noise1 - lane.noise2) * k;
        }
        for (int l = 0; l < c_lanes; ++l)
            u[static_cast<size_t>(l)] = 0.5 + 0.5 * std::tanh(c_randomFold * m_lane[static_cast<size_t>(settings.spread ? l : 0)].noise2);
    }
    else
    {
        if (! m_snap)
        {
            m_wavePhase += rate * dt;
            m_wavePhase -= std::floor(m_wavePhase);
        }
        auto shape = [&settings](double phase)
        {
            const double p = phase - std::floor(phase);
            return settings.wave == WaveTriangle ? 1.0 - std::abs(2.0 * p - 1.0) : 0.5 - 0.5 * std::cos(2.0 * c_pi * p);
        };
        u[0] = shape(m_wavePhase);
        u[1] = settings.spread ? 1.0 - u[0] : u[0];
        u[2] = settings.spread ? shape(m_wavePhase + 0.25) : u[0];
    }

    const float m = mixFor(knob) * (0.5f * sj::c_pi);
    const float dryTo = std::cos(m), wetTo = std::sin(m);
    if (m_snap)
    {
        m_dryGain = dryTo;
        m_wetGain = wetTo;
    }
    const float dryFrom = m_dryGain, wetFrom = m_wetGain;
    const double kSmooth = glide(dt, c_doublerSmoothSeconds);
    const float step = 1.0f / static_cast<float>(frames);
    const double stepD = 1.0 / static_cast<double>(frames);
    const double most = static_cast<double>(m_lane[0].line.maxDelay()) - 1.0;

    for (int l = 0; l < c_lanes; ++l) // every lane's motion runs, so one that joins later is already in place
    {
        auto& lane = m_lane[static_cast<size_t>(l)];
        const double wide = ! settings.spread ? 1.0 : (l == 1 ? c_spreadSecond : (l == 2 ? c_spreadThird : 1.0));
        const double target = (1.0 - wander) * delay * wide + wander * (c_wanderFloorMs + range * u[static_cast<size_t>(l)]);
        if (m_snap)
            lane.fast1 = lane.fast2 = target;
        lane.fast1 += (target - lane.fast1) * kSmooth;
        lane.fast2 += (lane.fast1 - lane.fast2) * kSmooth;
        const double d = lane.fast2;
        const float loopTo = settings.thicken ? static_cast<float>(thickenGain(d))
                                              : static_cast<float>(c_doublerFeedback) * sj::clamp(settings.feedback, -1.0f, 1.0f);
        // Read before the push, so one sample less than the delay.
        const double delayTo = std::min(most, d * msToSamples) - 1.0;
        if (m_snap)
        {
            lane.delay = delayTo;
            lane.loopGain = loopTo;
        }
        const double delayFrom = lane.delay;
        const float loopFrom = lane.loopGain;
        lane.delay = delayTo;
        lane.offsetMs = d;
        lane.loopGain = loopTo;

        float* x = l < count ? lanes.lane[static_cast<size_t>(l)] : nullptr;
        if (x == nullptr)
        {
            for (int i = 0; i < frames; ++i)
                lane.line.push(0.0f);
            for (auto& pole : lane.wetLp) pole.reset();
            continue;
        }

        auto& line = lane.line;
        auto& wet0 = lane.wetLp[0];
        auto& wet1 = lane.wetLp[1];
        for (int i = 0; i < frames; ++i)
        {
            const float t = static_cast<float>(i + 1) * step;
            const float in = x[i];
            const float wet = wet1.lowpass(wet0.lowpass(line.read(delayFrom + (delayTo - delayFrom) * (static_cast<double>(i + 1) * stepD))));
            line.push(in + (loopFrom + (loopTo - loopFrom) * t) * wet);
            x[i] = (dryFrom + (dryTo - dryFrom) * t) * in + (wetFrom + (wetTo - wetFrom) * t) * wet;
        }
    }

    m_dryGain = dryTo;
    m_wetGain = wetTo;
    m_snap = false;
}

//==============================================================================
void SwirlFx::delaySides(float* left, float* right, int frames)
{
    if (frames <= 0 || m_drySamples <= 0)
        return;
    m_dirty = true;
    float* sides[2] = { left, right };
    int at = m_sideAt;
    for (int c = 0; c < 2; ++c)
    {
        at = m_sideAt;
        float* ring = m_side[static_cast<size_t>(c)].data();
        float* x = sides[c];
        for (int i = 0; i < frames; ++i)
        {
            if (x != nullptr)
            {
                const float out = ring[at];
                ring[at] = x[i];
                x[i] = out;
            }
            else
                ring[at] = 0.0f;
            if (++at == m_drySamples)
                at = 0;
        }
    }
    m_sideAt = at;
}
