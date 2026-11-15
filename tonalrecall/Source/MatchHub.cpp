#include "MatchHub.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace tonalrecall
{

namespace
{
    // How often the thread looks at the store when nothing wakes it. The store's generations are
    // atomics, so a look costs next to nothing.
    constexpr auto kPollInterval = std::chrono::milliseconds(30);

    // Identifies captured audio, so arming or disarming Learn (which bumps the store's generation
    // without changing the audio) does not trigger a new analysis.
    std::uint64_t hashAudio(const std::vector<float>& audio) noexcept
    {
        std::uint64_t h = 1469598103934665603ull;
        const auto mix = [&h](std::uint32_t v)
        {
            h ^= v;
            h *= 1099511628211ull;
        };
        mix(static_cast<std::uint32_t>(audio.size()));
        mix(static_cast<std::uint32_t>(static_cast<std::uint64_t>(audio.size()) >> 32));
        for (float v : audio)
        {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &v, sizeof(bits));
            mix(bits);
        }
        return h == 0 ? 1 : h;
    }

    bool sameRate(double a, double b) noexcept { return a > 0.0 && std::abs(a - b) < 0.5; }
    bool sameBend(double a, double b) noexcept { return std::abs(a - b) < 1.0e-9; }

    // Two plans that would stamp exactly the same audio (so they have the same level).
    bool sameStamp(const StampPlan& a, const StampPlan& b) noexcept
    {
        return a.learnedMono == b.learnedMono && sameRate(a.sampleRate, b.sampleRate) && sameBend(a.staticSemitones, b.staticSemitones)
            && sameBend(a.curveHopSeconds, b.curveHopSeconds) && a.preserveFormants == b.preserveFormants && a.curve == b.curve;
    }
}

bool MatchOptions::operator==(const MatchOptions& o) const noexcept
{
    if (edits.size() != o.edits.size())
        return false;
    for (std::size_t i = 0; i < edits.size(); ++i)
        if (! sameBend(edits[i].seconds, o.edits[i].seconds) || ! sameBend(edits[i].semitones, o.edits[i].semitones))
            return false;
    if (lockedTarget != o.lockedTarget || lockedPitchClass != o.lockedPitchClass)
        return false;
    return target == o.target && pitchClass == o.pitchClass && octaveShift == o.octaveShift && sameBend(bend, o.bend)
        && sameBend(glide, o.glide)
        && method == o.method && preserveFormants == o.preserveFormants && stampEnabled == o.stampEnabled
        && trimEnabled == o.trimEnabled;
}

MatchOptions MatchOptions::withStyle(Style style) const noexcept
{
    MatchOptions o = *this;
    switch (style)
    {
        case Style::Locked:
            // One note: the one in use, or coming back from Subtle or Moving, the one that was.
            if (o.target == core::Target::Follow)
            {
                o.target = o.lockedTarget == core::Target::Follow ? core::Target::Nearest : o.lockedTarget;
                o.pitchClass = o.lockedPitchClass;
            }
            o.bend = 0.0;
            break;
        case Style::Subtle:
            if (o.target != core::Target::Follow)
            {
                o.lockedTarget = o.target;
                o.lockedPitchClass = o.pitchClass;
            }
            o.target = core::Target::Follow;
            o.bend = 0.35;
            o.glide = 1.0;
            break;
        case Style::Moving:
            if (o.target != core::Target::Follow)
            {
                o.lockedTarget = o.target;
                o.lockedPitchClass = o.pitchClass;
            }
            o.target = core::Target::Follow;
            o.bend = 0.8;
            o.glide = 1.0;
            break;
    }
    return o;
}

int MatchOptions::style() const noexcept
{
    if (target != core::Target::Follow && sameBend(bend, 0.0))
        return static_cast<int>(Style::Locked);
    if (target == core::Target::Follow && sameBend(glide, 1.0))
    {
        if (sameBend(bend, 0.35))
            return static_cast<int>(Style::Subtle);
        if (sameBend(bend, 0.8))
            return static_cast<int>(Style::Moving);
    }
    return -1;
}

MatchOptions MatchOptions::sanitised() const noexcept
{
    MatchOptions o = *this;
    o.pitchClass = ((o.pitchClass % 12) + 12) % 12;
    o.octaveShift = std::clamp(o.octaveShift, -2, 2);
    o.bend = std::clamp(std::isfinite(o.bend) ? o.bend : 0.0, -1.0, 1.0);   // (clamp alone lets NaN through)
    o.glide = std::clamp(std::isfinite(o.glide) ? o.glide : 0.0, 0.0, 1.0);
    if (o.edits.size() > kMaxEdits)
        o.edits.resize(kMaxEdits);
    for (auto& p : o.edits)
    {
        p.seconds = std::clamp(std::isfinite(p.seconds) ? p.seconds : 0.0, 0.0, 600.0);
        p.semitones = std::clamp(std::isfinite(p.semitones) ? p.semitones : 0.0, -24.0, 24.0);
    }
    std::sort(o.edits.begin(), o.edits.end(), [](const core::CurvePoint& a, const core::CurvePoint& b) { return a.seconds < b.seconds; });
    return o;
}

void MatchHub::SpinLock::lock() noexcept
{
    while (! tryLock())
        std::this_thread::yield();
}

MatchHub& MatchHub::instance()
{
    static MatchHub hub;
    return hub;
}

MatchHub::MatchHub() = default;

MatchHub::~MatchHub()
{
    {
        std::lock_guard<std::mutex> lock(m_threadMutex);
        m_stop = true;
        m_stopping.store(true, std::memory_order_release);
    }
    m_wake.notify_all();
    if (m_thread.joinable())
        m_thread.join();
}

void MatchHub::retain()
{
    std::lock_guard<std::mutex> lock(m_threadMutex);
    if (m_refs++ == 0)
    {
        m_stop = false;
        m_stopping.store(false, std::memory_order_release);
        m_thread = std::thread([this] { threadLoop(); });
    }
}

void MatchHub::release()
{
    std::thread finished;
    {
        std::lock_guard<std::mutex> lock(m_threadMutex);
        if (m_refs == 0 || --m_refs > 0)
            return;
        m_stop = true;
        m_stopping.store(true, std::memory_order_release);
        finished = std::move(m_thread);
    }
    // An analysis in progress finishes first (it cannot be interrupted); what it found is kept.
    // A measurement of the level in progress is abandoned.
    m_wake.notify_all();
    if (finished.joinable())
        finished.join();
}

MatchOptions MatchHub::options() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_options;
}

void MatchHub::setOptions(const MatchOptions& requested)
{
    const auto o = requested.sanitised();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (o == m_options)
            return;
        bool newPlan = false;
        if (o.method != m_options.method)
            m_sfxDirty = newPlan = true;
        if (o.target != m_options.target || o.pitchClass != m_options.pitchClass || o.octaveShift != m_options.octaveShift
            || ! sameBend(o.bend, m_options.bend) || ! sameBend(o.glide, m_options.glide) || o.preserveFormants != m_options.preserveFormants)
            m_matchDirty = newPlan = true;
        {
            // Hand adjustments change the curve too.
            auto withTheirEdits = m_options;
            withTheirEdits.edits = o.edits;
            if (! (withTheirEdits == m_options))
                m_matchDirty = newPlan = true;
        }
        // (Stamp and TRIM switch what is done with the plan, not the plan: no new one, and the
        // level measured for it stands.)
        if (newPlan)
            wantNewPlan();
        m_options = o;
        m_stampEnabled.store(o.stampEnabled, std::memory_order_release);
        m_trimEnabled.store(o.trimEnabled, std::memory_order_release);
        bump();
    }
    {
        std::lock_guard<std::mutex> lock(m_threadMutex);
        m_kick = true;
    }
    m_wake.notify_all();
}

void MatchHub::snapshot(HubSnapshot& out) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    out.generation = m_generation.load(std::memory_order_acquire);
    out.options = m_options;
    out.musicBusy = m_musicBusy;
    out.sfxBusy = m_sfxBusy;
    out.hasMusic = m_hasMusic;
    out.hasSfx = m_hasSfx;
    out.music = m_music;
    out.sfx = m_sfx;
    out.match = m_match;
    out.musicRate = m_musicRate;
    out.sfxRate = m_sfxRate;
    out.offsetKnown = m_musicTimeline && m_sfxTimeline && sameRate(m_musicRate, m_sfxRate);
    out.sfxOffsetInMusicSeconds = out.offsetKnown ? static_cast<double>(m_sfxStart - m_musicStart) / m_musicRate : 0.0;
    out.planReady = m_planGeneration.load(std::memory_order_acquire) != 0;
    out.sfxChannels = m_sfxChannels;
    out.musicChannels = m_musicChannels;
    out.styleAfterMidi = m_styleAfterMidi;
    const auto plan = m_planGeneration.load(std::memory_order_acquire);
    out.trimKnown = plan != 0 && m_measuredFor == plan;
    out.trimMeasuring = plan != 0 && ! out.trimKnown;
    out.levels = out.trimKnown ? m_levels : core::StampLevels {};
    out.trimDb = out.trimKnown ? m_levels.trimDb() : 0.0f;
}

bool MatchHub::trimFor(std::uint64_t planGeneration, float& gain) const noexcept
{
    if (planGeneration == 0)
        return false;
    const auto word = m_trimWord.load(std::memory_order_acquire);
    if ((word >> 32) != (planGeneration & 0xffffffffull))
        return false;
    if (! m_trimEnabled.load(std::memory_order_acquire))
    {
        gain = 1.0f;
        return true;
    }
    const auto bits = static_cast<std::uint32_t>(word & 0xffffffffull);
    std::memcpy(&gain, &bits, sizeof(gain));
    return true;
}

void MatchHub::publishTrimLocked(std::uint64_t planGeneration, const core::StampLevels& levels)
{
    m_measuredFor = planGeneration;
    m_levels = levels;
    const float gain = levels.trim();   // 1 when it could not be measured: the limiter is there
    std::uint32_t bits = 0;
    std::memcpy(&bits, &gain, sizeof(bits));
    m_trimWord.store(((planGeneration & 0xffffffffull) << 32) | bits, std::memory_order_release);
}

bool MatchHub::tryAcquirePlan(std::shared_ptr<const StampPlan>& out) const noexcept
{
    // A shared_ptr copy is an atomic increment of the reference count: no allocation. The lock is
    // only ever held for that copy (here) or a pointer swap (publishPlanLocked), so a few tries
    // are enough; failing just means "keep what you have".
    for (int attempt = 0; attempt < 64; ++attempt)
    {
        if (m_planLock.tryLock())
        {
            out = m_plan;
            m_planLock.unlock();
            return true;
        }
    }
    return false;
}

void MatchHub::publishPlanLocked(std::shared_ptr<const StampPlan> plan)
{
    const auto generation = plan != nullptr ? plan->generation : 0;
    if (plan != nullptr)
        m_plans.push_back(plan);
    std::shared_ptr<const StampPlan> previous;
    m_planLock.lock();
    previous = std::exchange(m_plan, std::move(plan));
    m_planLock.unlock();
    m_planGeneration.store(generation, std::memory_order_release);
    // previous is still in m_plans: it is freed here, on this thread, once nobody holds it.
}

void MatchHub::freeUnusedPlansLocked()
{
    // Only the current plan can gain new holders (tryAcquirePlan copies m_plan), so an older plan
    // that only this list holds can never be picked up again.
    std::shared_ptr<const StampPlan> current;
    m_planLock.lock();
    current = m_plan;
    m_planLock.unlock();
    m_plans.erase(std::remove_if(m_plans.begin(), m_plans.end(),
                                 [&](const std::shared_ptr<const StampPlan>& p) { return p != current && p.use_count() == 1; }),
                  m_plans.end());
}

void MatchHub::recomputeMatchLocked()
{
    core::MatchOptions mo;
    mo.target = m_options.target;
    mo.pitchClass = m_options.pitchClass;
    mo.octaveShift = m_options.octaveShift;
    mo.bend = m_options.bend;
    mo.glide = m_options.glide;
    mo.edits = m_options.edits;
    // Both timeline starts come from Preview and are approximate; they place the effect within
    // the music for a "follow" bend, which needs no more than that.
    if (m_musicTimeline && m_sfxTimeline && sameRate(m_musicRate, m_sfxRate))
    {
        mo.offsetKnown = true;
        mo.sfxOffsetInMusicSeconds = static_cast<double>(m_sfxStart - m_musicStart) / m_musicRate;
    }

    if (m_hasMusic && m_hasSfx)
        m_match = core::match(m_music, m_sfx, mo);
    else
        m_match = {};

    // What each style would do, for the editor to offer.
    for (int style = 0; style < MatchOptions::kNumStyles; ++style)
    {
        m_styleAfterMidi[static_cast<std::size_t>(style)].clear();
        if (! m_match.valid)
            continue;
        const auto styled = m_options.withStyle(static_cast<MatchOptions::Style>(style));
        auto so = mo;
        so.target = styled.target;
        so.bend = styled.bend;
        so.glide = styled.glide;
        auto result = core::match(m_music, m_sfx, so);
        if (result.valid)
            m_styleAfterMidi[static_cast<std::size_t>(style)] = std::move(result.afterMidi);
    }

    std::shared_ptr<const StampPlan> plan;
    if (m_match.valid && m_sfxAudio != nullptr && m_sfxRate > 0.0)
    {
        auto p = std::make_shared<StampPlan>();
        p->generation = m_nextPlanGeneration++;
        p->sampleRate = m_sfxRate;
        p->staticSemitones = m_match.semitones;
        p->curveHopSeconds = m_match.hopSeconds;
        p->curve = m_match.curve;
        p->learnedMono = m_sfxAudio;
        p->preserveFormants = m_options.preserveFormants;
        p->bendActive = m_options.bendActive();
        p->targetMidi = m_match.targetMidi;
        plan = std::move(p);
    }
    // A plan that stamps exactly what the measured one did (an option that ended up changing
    // nothing) takes its level over instead of being measured again.
    const bool sameLevel = plan != nullptr && m_measuredPlan != nullptr && sameStamp(*plan, *m_measuredPlan);
    const auto kept = plan;
    publishPlanLocked(std::move(plan));
    if (sameLevel)
    {
        publishTrimLocked(kept->generation, m_levels);
        m_measuredPlan = kept;
    }
    m_matchDirty = false;
    bump();
}

bool MatchHub::workOnce()
{
    auto& store = CaptureStore::instance();
    bool did = false;

    for (int s = 0; s < kNumSlots; ++s)
    {
        const auto slot = static_cast<Slot>(s);
        const auto index = static_cast<std::size_t>(s);
        const bool isMusic = slot == Slot::Music;
        const auto generation = store.generation(slot);
        std::uint64_t epoch = 0;
        core::SfxMethod method = core::SfxMethod::Auto;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (generation == m_slots[index].seenGeneration && ! m_slots[index].pending)
                continue;
            m_slots[index].seenGeneration = generation;
            m_slots[index].pending = false;
            epoch = m_epoch;
            method = m_options.method;
        }

        SlotSnapshot snap;
        store.snapshot(slot, snap);
        if (snap.capturing)
            continue;   // the end of the pass bumps the generation again

        if (snap.samplesCaptured <= 0)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (epoch != m_epoch)
                continue;
            m_slots[index].audioHash = 0;
            if (isMusic ? m_hasMusic : m_hasSfx)
            {
                if (isMusic) { m_music = {}; m_hasMusic = false; m_musicTimeline = false; m_musicChannels.reset(); }
                else         { m_sfx = {}; m_hasSfx = false; m_sfxAudio.reset(); m_sfxChannels.reset(); m_sfxTimeline = false; m_options.edits.clear(); m_editsBelongTo = 0; m_measuredPlan.reset(); }
                m_matchDirty = true;
                bump();
                did = true;
            }
            continue;
        }

        auto audio = std::make_shared<std::vector<float>>();
        store.copyAudio(slot, *audio);
        // The channels as they arrived, for playing back (the analysis uses only the sum).
        auto separate = std::make_shared<std::vector<std::vector<float>>>();
        store.copyChannels(slot, *separate);
        const auto hash = hashAudio(*audio);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (epoch != m_epoch || hash == m_slots[index].audioHash)
                continue;
            (isMusic ? m_musicBusy : m_sfxBusy) = true;
            bump();
        }

        // The long part, without any lock.
        core::MusicAnalysis music;
        core::SfxAnalysis sfx;
        const int count = static_cast<int>(std::min<std::size_t>(audio->size(), static_cast<std::size_t>(0x7fffffff)));
        if (isMusic)
            music = core::analyseMusic(audio->data(), count, snap.sampleRate);
        else
            sfx = core::analyseSfx(audio->data(), count, snap.sampleRate, method);

        std::lock_guard<std::mutex> lock(m_mutex);
        did = true;
        if (epoch != m_epoch)
            continue;
        m_slots[index].audioHash = hash;
        if (isMusic)
        {
            m_music = std::move(music);
            m_hasMusic = true;
            m_musicBusy = false;
            m_musicRate = snap.sampleRate;
            m_musicTimeline = snap.timelineKnown;
            m_musicStart = snap.captured.start;
            m_musicChannels = std::move(separate);
        }
        else
        {
            // Hand adjustments were made against the previous effect's timeline.
            if (hash != m_editsBelongTo)
            {
                m_options.edits.clear();
                m_editsBelongTo = hash;
            }
            m_sfx = std::move(sfx);
            m_hasSfx = true;
            m_sfxBusy = false;
            m_sfxRate = snap.sampleRate;
            m_sfxTimeline = snap.timelineKnown;
            m_sfxStart = snap.captured.start;
            m_sfxAudio = std::move(audio);
            m_sfxChannels = std::move(separate);
            // The method may have changed while this ran.
            if (method == m_options.method)
                m_sfxDirty = false;
        }
        m_matchDirty = true;
        bump();
    }

    // The effect method changed: analyse the effect again from the audio already copied.
    std::shared_ptr<const std::vector<float>> sfxAudio;
    double sfxRate = 0.0;
    core::SfxMethod method = core::SfxMethod::Auto;
    std::uint64_t epoch = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_sfxDirty)
        {
            m_sfxDirty = false;
            if (m_sfxAudio != nullptr && m_hasSfx)
            {
                sfxAudio = m_sfxAudio;
                sfxRate = m_sfxRate;
                method = m_options.method;
                epoch = m_epoch;
                m_sfxBusy = true;
                bump();
            }
        }
    }
    if (sfxAudio != nullptr)
    {
        auto sfx = core::analyseSfx(sfxAudio->data(), static_cast<int>(sfxAudio->size()), sfxRate, method);
        std::lock_guard<std::mutex> lock(m_mutex);
        did = true;
        if (epoch == m_epoch && sfxAudio == m_sfxAudio)
        {
            m_sfx = std::move(sfx);
            m_sfxBusy = false;
            m_matchDirty = true;
            if (method != m_options.method)
                m_sfxDirty = true;
            bump();
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_matchDirty)
        {
            recomputeMatchLocked();
            did = true;
        }
        freeUnusedPlansLocked();
    }

    if (measureLevels())
        did = true;
    return did;
}

bool MatchHub::measureLevels()
{
    // The plan to measure: the current one, if its level is not known yet.
    std::shared_ptr<const StampPlan> plan;
    std::shared_ptr<const std::vector<std::vector<float>>> channels;
    std::uint64_t wanted = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_matchDirty || m_sfxDirty)
            return false;   // a newer plan comes first
        m_planLock.lock();
        plan = m_plan;
        m_planLock.unlock();
        if (plan == nullptr || plan->generation == m_measuredFor)
            return false;
        channels = m_sfxChannels;
        wanted = m_planWanted.load(std::memory_order_acquire);
    }

    // The long part, without any lock. Dragging the curve publishes plans many times a second, so
    // the measurement gives up as soon as anything would make it stale: options that change the
    // plan, a new capture on either side, a reset, or the thread being stopped.
    auto& store = CaptureStore::instance();
    const auto musicGeneration = store.generation(Slot::Music);
    const auto sfxGeneration = store.generation(Slot::Sfx);
    const auto stale = [&]
    {
        return m_stopping.load(std::memory_order_acquire) || m_planWanted.load(std::memory_order_acquire) != wanted
            || m_planGeneration.load(std::memory_order_acquire) != plan->generation
            || store.generation(Slot::Music) != musicGeneration || store.generation(Slot::Sfx) != sfxGeneration;
    };

    core::Stamper::Setup setup;
    setup.sampleRate = plan->sampleRate;
    setup.staticSemitones = plan->staticSemitones;
    setup.curveHopSeconds = plan->curveHopSeconds;
    setup.curve = plan->curve;
    setup.learned = plan->learnedMono;
    setup.preserveFormants = plan->preserveFormants;
    core::StampLevels levels;
    if (channels != nullptr && ! channels->empty())
        levels = core::measureStamp(setup, *channels, stale);
    const bool abandoned = stale();
    setup.learned.reset();

    std::lock_guard<std::mutex> lock(m_mutex);
    // Abandoned, or overtaken by a newer plan: the next pass measures that one. A measurement
    // that simply could not be made (nothing to measure) is published as it is, with no trim.
    if (! abandoned && m_planGeneration.load(std::memory_order_acquire) == plan->generation)
    {
        publishTrimLocked(plan->generation, levels);
        m_measuredPlan = plan;
        bump();
    }
    return true;
}

void MatchHub::threadLoop()
{
    for (;;)
    {
        {
            std::unique_lock<std::mutex> lock(m_threadMutex);
            m_wake.wait_for(lock, kPollInterval, [this] { return m_stop || m_kick; });
            if (m_stop)
                return;
            m_kick = false;
        }
        if (! workOnce())
            m_idlePasses.fetch_add(1, std::memory_order_acq_rel);
    }
}

bool MatchHub::waitUntilIdle(double timeoutSeconds)
{
    // Two passes that found nothing to do, both started after this call: everything the store
    // and the options held at the call has been analysed, matched and measured.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeoutSeconds);
    const auto start = m_idlePasses.load(std::memory_order_acquire);
    while (m_idlePasses.load(std::memory_order_acquire) < start + 2)
    {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        {
            std::lock_guard<std::mutex> lock(m_threadMutex);
            m_kick = true;
        }
        m_wake.notify_all();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

void MatchHub::resetForTesting()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_epoch;
    wantNewPlan();   // a measurement in progress is dropped too
    m_options = {};
    m_stampEnabled.store(true, std::memory_order_release);
    m_trimEnabled.store(true, std::memory_order_release);
    m_measuredFor = 0;
    m_levels = {};
    m_measuredPlan.reset();
    m_trimWord.store(0, std::memory_order_release);
    m_music = {};
    m_sfx = {};
    m_match = {};
    m_hasMusic = m_hasSfx = false;
    m_musicBusy = m_sfxBusy = false;
    m_musicRate = m_sfxRate = 0.0;
    m_musicTimeline = m_sfxTimeline = false;
    m_musicStart = m_sfxStart = 0;
    m_sfxChannels.reset();
    m_musicChannels.reset();
    m_sfxAudio.reset();
    m_sfxDirty = m_matchDirty = false;
    for (auto& s : m_slots)
        s = {};
    publishPlanLocked(nullptr);
    bump();
}

} // namespace tonalrecall
