#include "Cargo.h"
#include "SampleClassifier.h"

namespace
{
    constexpr int c_maxUndo = 40;
    constexpr juce::uint32 c_graveyardMs = 2500;
    constexpr float c_targetRms = 0.22f;

    bool isOneShotRole(CargoRole r)
    {
        return r == CargoRole::Tonal || r == CargoRole::Impact || r == CargoRole::Air;
    }

    /** 0..1 -> 0..1 with zero slope at both ends (click-free fades). */
    float smooth(float x)
    {
        x = sj::clamp(x, 0.0f, 1.0f);
        return x * x * (3.0f - 2.0f * x);
    }

    void classify(CargoData& d, const juce::String& fileName)
    {
        const float* data = d.audio.getNumSamples() > 0 ? d.audio.getReadPointer(0) : nullptr;
        if (data == nullptr)
            return;
        const auto a = SampleClassifier::analyse(data, d.audio.getNumSamples(), d.sampleRate, d.regions, fileName);
        d.detectedRole = a.role;
        d.roleConfidence = a.confidence;
        d.autoLabel = a.label;
    }

    //==========================================================================
    // Band-limited reading. One Kaiser-windowed sinc, eight zero crossings a side, tabulated finely and read with
    // linear interpolation. Played at the source's own speed or slower it is a plain 16-point interpolator (and
    // returns the sample itself on a whole position, so unpitched playback is bit-exact). Played faster, the
    // same curve is stretched in time by the speed, which lowers its cutoff to the new Nyquist: what would fold
    // back is filtered out instead. The octave copies (CargoData::mips) keep that stretch under about 2.5, so
    // the cost per sample stays bounded however far the pitch goes.
    constexpr int c_sincZeros = 8;
    constexpr int c_sincRes = 256;
    constexpr int c_sincSize = c_sincZeros * c_sincRes;
    // The stretched filter's -6 dB point sits at this fraction of the output Nyquist, so its stopband (about
    // 70 dB down) starts just above Nyquist and what little folds lands above 21 kHz.
    constexpr double c_sincGuard = 0.87;
    constexpr double c_maxStretch = 8.0;

    double besselI0(double x)
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 40; ++k)
        {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;
        }
        return sum;
    }

    struct SincTable
    {
        std::array<float, static_cast<size_t>(c_sincSize + 2)> v {};
        SincTable()
        {
            const double beta = 6.76; // about 70 dB
            const double norm = besselI0(beta);
            for (int n = 0; n <= c_sincSize; ++n)
            {
                const double x = static_cast<double>(n) / c_sincRes;
                const double px = juce::MathConstants<double>::pi * x;
                const double sinc = n == 0 ? 1.0 : std::sin(px) / px;
                const double r = x / c_sincZeros;
                const double window = besselI0(beta * std::sqrt(juce::jmax(0.0, 1.0 - r * r))) / norm;
                // Exactly zero on the other whole samples: a read on a whole position returns that sample.
                v[static_cast<size_t>(n)] = (n > 0 && n % c_sincRes == 0) ? 0.0f : static_cast<float>(sinc * window);
            }
            v[static_cast<size_t>(c_sincSize)] = 0.0f;
            v[static_cast<size_t>(c_sincSize + 1)] = 0.0f;
        }
    };
    const SincTable g_sinc; // built when the library loads, never on the audio thread

    /** How far the kernel is stretched for a read speed of `inc` samples of this buffer per output sample. */
    double stretchFor(double inc)
    {
        if (inc <= 1.0)
            return 1.0;
        // Ease the guard band in over the first 15% so the filter never steps as the speed passes 1.
        const double guard = 1.0 + (1.0 / c_sincGuard - 1.0) * juce::jmin(1.0, (inc - 1.0) / 0.15);
        return juce::jmin(c_maxStretch, inc * guard);
    }

    /** One sample of data at pos through the sinc stretched by `stretch` (>= 1). Edges repeat the end samples. */
    float sincRead(const float* data, int length, double pos, double stretch)
    {
        const double whole = std::floor(pos);
        const int i = static_cast<int>(whole);
        const double frac = pos - whole;
        if (stretch <= 1.0 && frac == 0.0)
            return data[juce::jlimit(0, length - 1, i)];

        const double s = juce::jmax(1.0, stretch);
        const double half = c_sincZeros * s;
        const int lo = static_cast<int>(std::ceil(frac - half));
        const int hi = static_cast<int>(std::floor(frac + half));
        const float step = static_cast<float>(c_sincRes / s);
        const float origin = static_cast<float>(frac * c_sincRes / s);
        const float* table = g_sinc.v.data();

        float sum = 0.0f, weight = 0.0f;
        if (i + lo >= 0 && i + hi < length)
        {
            const float* p = data + i;
            for (int j = lo; j <= hi; ++j)
            {
                const float a = std::abs(static_cast<float>(j) * step - origin);
                const int n = juce::jmin(c_sincSize, static_cast<int>(a));
                const float w = table[n] + (a - static_cast<float>(n)) * (table[n + 1] - table[n]);
                sum += w * p[j];
                weight += w;
            }
        }
        else
        {
            for (int j = lo; j <= hi; ++j)
            {
                const float a = std::abs(static_cast<float>(j) * step - origin);
                const int n = juce::jmin(c_sincSize, static_cast<int>(a));
                const float w = table[n] + (a - static_cast<float>(n)) * (table[n + 1] - table[n]);
                sum += w * data[juce::jlimit(0, length - 1, i + j)];
                weight += w;
            }
        }
        return weight > 1.0e-6f ? sum / weight : 0.0f;
    }

    /** Band-limited resample of a mono buffer (message thread): the same kernel the players read with. */
    juce::AudioBuffer<float> resampled(const juce::AudioBuffer<float>& in, double fromRate, double toRate)
    {
        if (fromRate <= 0.0 || toRate <= 0.0 || std::abs(fromRate - toRate) < 0.5 || in.getNumSamples() < 2)
            return in;
        const double ratio = fromRate / toRate;
        const int outLen = juce::jmax(1, static_cast<int>(in.getNumSamples() / ratio));
        juce::AudioBuffer<float> out(1, outLen);
        const float* src = in.getReadPointer(0);
        float* dst = out.getWritePointer(0);
        // Offline, so the kernel may be as long as the ratio asks for (no octave copies needed).
        const double stretch = ratio > 1.0 ? ratio / c_sincGuard : 1.0;
        for (int i = 0; i < outLen; ++i)
            dst[i] = sincRead(src, in.getNumSamples(), i * ratio, stretch);
        return out;
    }

    /** The low-pass in front of each halving: flat to 0.197 of the rate, 80 dB down from a quarter of it (the new
        Nyquist), so nothing folds into the copy. 95 taps, Kaiser. */
    constexpr int c_mipTaps = 95;
    const std::array<float, c_mipTaps>& mipKernel()
    {
        static const std::array<float, c_mipTaps> kernel = []
        {
            std::array<float, c_mipTaps> h {};
            const double beta = 7.857, norm = besselI0(beta), fc = 0.2236;
            const int mid = c_mipTaps / 2;
            double sum = 0.0;
            std::array<double, c_mipTaps> d {};
            for (int k = 0; k < c_mipTaps; ++k)
            {
                const double x = k - mid;
                const double px = juce::MathConstants<double>::twoPi * fc * x;
                const double sinc = k == mid ? 2.0 * fc : std::sin(px) / (juce::MathConstants<double>::pi * x);
                const double r = x / (mid + 1);
                d[static_cast<size_t>(k)] = sinc * besselI0(beta * std::sqrt(juce::jmax(0.0, 1.0 - r * r))) / norm;
                sum += d[static_cast<size_t>(k)];
            }
            for (int k = 0; k < c_mipTaps; ++k)
                h[static_cast<size_t>(k)] = static_cast<float>(d[static_cast<size_t>(k)] / sum);
            return h;
        }();
        return kernel;
    }

    /** src low-passed and decimated by two (out[m] sits at src[2m]); the ends repeat the edge samples. Reads the
        source in place: a long file needs no padded second copy of itself while its octave copies are built. */
    std::vector<float> halved(const float* src, int length)
    {
        const auto& h = mipKernel();
        const int mid = c_mipTaps / 2;
        const int outLength = (length + 1) / 2;
        std::vector<float> out(static_cast<size_t>(outLength));
        for (int m = 0; m < outLength; ++m)
        {
            const int first = 2 * m - mid; // the kernel covers src[first .. first + c_mipTaps - 1]
            if (first >= 0 && first + c_mipTaps <= length)
            {
                const float* p = src + first;
                float a0 = 0.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
                int k = 0;
                for (; k + 4 <= c_mipTaps; k += 4)
                {
                    a0 += h[static_cast<size_t>(k)] * p[k];
                    a1 += h[static_cast<size_t>(k + 1)] * p[k + 1];
                    a2 += h[static_cast<size_t>(k + 2)] * p[k + 2];
                    a3 += h[static_cast<size_t>(k + 3)] * p[k + 3];
                }
                for (; k < c_mipTaps; ++k)
                    a0 += h[static_cast<size_t>(k)] * p[k];
                out[static_cast<size_t>(m)] = (a0 + a1) + (a2 + a3);
            }
            else
            {
                float a = 0.0f;
                for (int k = 0; k < c_mipTaps; ++k)
                    a += h[static_cast<size_t>(k)] * src[juce::jlimit(0, length - 1, first + k)];
                out[static_cast<size_t>(m)] = a;
            }
        }
        return out;
    }

    juce::String clockTime(double seconds)
    {
        const int total = juce::jmax(0, static_cast<int>(seconds));
        return juce::String(total / 60) + ":" + juce::String(total % 60).paddedLeft('0', 2);
    }
}

//==============================================================================
std::vector<CargoRegion> CargoData::detectRegions(const float* data, int numSamples, double sampleRate)
{
    std::vector<CargoRegion> regions;
    if (data == nullptr || numSamples <= 0)
        return regions;

    const int window = juce::jmax(32, static_cast<int>(sampleRate * 0.01));
    const int numWindows = (numSamples + window - 1) / window;

    std::vector<float> db(static_cast<size_t>(numWindows));
    float peakDb = -200.0f;
    for (int w = 0; w < numWindows; ++w)
    {
        const int s = w * window;
        const int e = juce::jmin(numSamples, s + window);
        double sum = 0.0;
        for (int i = s; i < e; ++i)
            sum += static_cast<double>(data[i]) * data[i];
        const float rms = static_cast<float>(std::sqrt(sum / juce::jmax(1, e - s)));
        db[static_cast<size_t>(w)] = juce::Decibels::gainToDecibels(rms, -200.0f);
        peakDb = juce::jmax(peakDb, db[static_cast<size_t>(w)]);
    }

    if (peakDb < -80.0f)
        return regions; // effectively silent

    const float openDb = juce::jmax(peakDb - 36.0f, -60.0f);
    const float closeDb = openDb - 6.0f;

    // Gate with hysteresis, in windows.
    std::vector<std::pair<int, int>> raw;
    bool open = false;
    int startWindow = 0;
    for (int w = 0; w < numWindows; ++w)
    {
        const float v = db[static_cast<size_t>(w)];
        if (! open && v >= openDb) { open = true; startWindow = w; }
        else if (open && v < closeDb) { open = false; raw.emplace_back(startWindow, w); }
    }
    if (open)
        raw.emplace_back(startWindow, numWindows);

    // Merge short gaps, drop blips.
    const int mergeGap = static_cast<int>(0.08 * sampleRate / window) + 1;
    const int minLength = static_cast<int>(0.04 * sampleRate / window) + 1;
    std::vector<std::pair<int, int>> merged;
    for (const auto& r : raw)
    {
        if (! merged.empty() && r.first - merged.back().second <= mergeGap)
            merged.back().second = r.second;
        else
            merged.push_back(r);
    }

    // Keep natural tails: follow each sound's decay down to ~54 dB under the loudest part
    // (up to 2.5 s, never into the next sound), and give the attack a short pre-roll.
    const float tailDb = juce::jmax(peakDb - 54.0f, -75.0f);
    const int maxTailWindows = static_cast<int>(2.5 * sampleRate / window);
    const int preRoll = static_cast<int>(0.03 * sampleRate);
    const int pad = static_cast<int>(0.02 * sampleRate);
    for (size_t m = 0; m < merged.size(); ++m)
    {
        const auto& r = merged[m];
        if (r.second - r.first < minLength && merged.size() > 1)
            continue;

        const int limit = m + 1 < merged.size() ? merged[m + 1].first : numWindows;
        int endWindow = r.second;
        while (endWindow < limit && endWindow - r.second < maxTailWindows && db[static_cast<size_t>(endWindow)] > tailDb)
            ++endWindow;

        CargoRegion region;
        region.start = juce::jmax(0, r.first * window - preRoll);
        region.end = juce::jmin(numSamples, endWindow * window + pad);
        region.focus = region.start;
        if (! regions.empty() && region.start <= regions.back().end)
            regions.back().end = region.end;
        else
            regions.push_back(region);
    }
    return regions;
}

void CargoData::analyse()
{
    const float* data = audio.getNumSamples() > 0 ? audio.getReadPointer(0) : nullptr;
    regions = detectRegions(data, audio.getNumSamples(), sampleRate);

    if (data != nullptr && ! regions.empty())
    {
        SampleClassifier::findFocusPoints(data, audio.getNumSamples(), sampleRate, regions);
        for (auto& r : regions) // never trust a focus outside its region
            r.focus = juce::jlimit(r.start, juce::jmax(r.start, r.end - 1), r.focus);
    }

    double sum = 0.0;
    std::int64_t count = 0;
    float peak = 0.0f;
    for (const auto& r : regions)
    {
        for (int i = r.start; i < r.end; ++i)
        {
            sum += static_cast<double>(data[i]) * data[i];
            peak = juce::jmax(peak, std::abs(data[i]));
        }
        count += r.end - r.start;
    }
    const float rms = count > 0 ? static_cast<float>(std::sqrt(sum / static_cast<double>(count))) : 0.0f;
    // Level every track to the same RMS, but never lift a peaky sound (a hit, a click) over full scale.
    float gain = rms > 1.0e-6f ? c_targetRms / rms : 1.0f;
    if (peak > 1.0e-6f)
        gain = juce::jmin(gain, 1.0f / peak);
    autoGain = rms > 1.0e-6f ? juce::jlimit(0.1f, 16.0f, gain) : 1.0f;

    buildMips();
}

void CargoData::buildMips()
{
    for (auto& m : mips)
        m.clear();
    const float* src = audio.getNumSamples() > 0 ? audio.getReadPointer(0) : nullptr;
    int length = audio.getNumSamples();
    for (auto& m : mips)
    {
        if (src == nullptr || length < 2)
            break;
        m = halved(src, length);
        src = m.data();
        length = static_cast<int>(m.size());
    }
}

//==============================================================================
CargoHold::CargoHold()
{
    m_formats.registerBasicFormats();
    for (auto& track : playheads)
        for (auto& p : track)
            p.store(-1.0f);
    for (auto& m : m_mute) m.store(false);
    for (auto& s : m_solo) s.store(false);
    for (auto& slots : m_inUse)
        for (auto& p : slots)
            p.store(nullptr);
    startTimer(500);
}

CargoHold::~CargoHold()
{
    stopTimer();
}

void CargoHold::timerCallback()
{
    purgeRetired(c_graveyardMs);
}

int CargoHold::purgeRetired(juce::uint32 minAgeMs)
{
    const auto now = juce::Time::getMillisecondCounter();
    // Old enough AND no player still reads it (a stalled / bypassed audio thread keeps its pointer marked).
    m_graveyard.erase(std::remove_if(m_graveyard.begin(), m_graveyard.end(),
                                     [this, now, minAgeMs](const Retired& r)
                                     {
                                         if (now - r.time < minAgeMs || isInUse(r.data.get()))
                                             return false;
                                         // Still held by an undo / redo step: it keeps the audio, and gives up its
                                         // octave copies (nearly as big again) until that step is used. Safe here
                                         // and only here: the audio thread has let go of it, and it can't pick it
                                         // up again unless undo / redo publish it (they rebuild the copies first).
                                         if (r.data.use_count() > 1 && ! isLive(r.data.get()))
                                             dropMips(r.data);
                                         return true;
                                     }),
                      m_graveyard.end());
    return static_cast<int>(m_graveyard.size());
}

bool CargoHold::isLive(const CargoData* data) const
{
    for (const auto& t : m_tracks)
        if (t.live.load(std::memory_order_acquire) == data)
            return true;
    return false;
}

void CargoHold::dropMips(const std::shared_ptr<const CargoData>& data)
{
    // Every CargoData is made non-const (std::make_shared<CargoData>); the const is how the rest of the program
    // sees published data. The caller has made sure no thread reads this one.
    for (auto& m : const_cast<CargoData&>(*data).mips)
        std::vector<float>().swap(m);
}

void CargoHold::restoreMips(const std::shared_ptr<const CargoData>& data)
{
    if (data != nullptr && data->length() >= 2 && data->mips[0].empty())
        const_cast<CargoData&>(*data).buildMips();
}

std::int64_t CargoHold::bytesOf(const CargoData* data)
{
    if (data == nullptr)
        return 0;
    std::int64_t samples = data->length();
    for (const auto& m : data->mips)
        samples += static_cast<std::int64_t>(m.size());
    return samples * static_cast<std::int64_t>(sizeof(float));
}

std::int64_t CargoHold::getUndoBytes() const
{
    std::vector<const CargoData*> seen;
    std::int64_t bytes = 0;
    for (const auto& t : m_tracks)
        for (const auto* list : { &t.undo, &t.redo })
            for (const auto& d : *list)
                if (d != nullptr && std::find(seen.begin(), seen.end(), d.get()) == seen.end())
                {
                    seen.push_back(d.get());
                    bytes += bytesOf(d.get());
                }
    return bytes;
}

void CargoHold::trimUndoToBytes()
{
    // Over the limit: the oldest edits go first, across every track. The newest edit can always be undone.
    while (m_undoOrder.size() > 1 && getUndoBytes() > m_undoByteLimit)
    {
        auto& t = m_tracks[static_cast<size_t>(m_undoOrder.front())];
        m_undoOrder.erase(m_undoOrder.begin());
        if (t.undo.empty())
            continue; // that edit's step had gone already
        retire(t.undo.front());
        t.undo.erase(t.undo.begin());
        if (! t.undoMeta.empty())
            t.undoMeta.erase(t.undoMeta.begin());
    }
}

void CargoHold::markInUse(int track, const CargoData* current, const CargoData* pending)
{
    if (track < 0 || track >= c_numTracks)
        return;
    auto& slots = m_inUse[static_cast<size_t>(track)];
    slots[0].store(current, std::memory_order_release);
    slots[1].store(pending, std::memory_order_release);
}

bool CargoHold::isInUse(const CargoData* data) const
{
    if (data == nullptr)
        return false;
    for (const auto& slots : m_inUse)
        for (const auto& p : slots)
            if (p.load(std::memory_order_acquire) == data)
                return true;
    return false;
}

void CargoHold::retire(std::shared_ptr<const CargoData> data)
{
    if (data != nullptr)
        m_graveyard.push_back({ std::move(data), juce::Time::getMillisecondCounter() });
}

void CargoHold::setSkipSilence(int track, bool skip)
{
    m_tracks[static_cast<size_t>(track)].skipSilence = skip;
    sendChangeMessage();
}

//==============================================================================
float CargoHold::laneGain(int lane) const
{
    if (lane < 0 || lane >= c_numLanes)
        return 1.0f;
    const auto i = static_cast<size_t>(lane);
    if (m_mute[i].load(std::memory_order_relaxed))
        return 0.0f;
    if (m_soloCount.load(std::memory_order_relaxed) > 0 && ! m_solo[i].load(std::memory_order_relaxed))
        return 0.0f;
    return 1.0f;
}

void CargoHold::setMute(int lane, bool on)
{
    if (lane < 0 || lane >= c_numLanes)
        return;
    if (m_mute[static_cast<size_t>(lane)].exchange(on) != on)
        sendChangeMessage();
}

void CargoHold::setSolo(int lane, bool on)
{
    if (lane < 0 || lane >= c_numLanes)
        return;
    const bool was = m_solo[static_cast<size_t>(lane)].exchange(on);
    if (was == on)
        return;
    m_soloCount.fetch_add(on ? 1 : -1);
    sendChangeMessage();
}

CargoRole CargoHold::getRole(int track) const
{
    if (track < 0 || track >= c_numTracks)
        return CargoRole::Generic;
    const auto& t = m_tracks[static_cast<size_t>(track)];
    const int o = t.roleOverride.load(std::memory_order_relaxed);
    if (o >= 0 && o < c_numCargoRoles)
        return static_cast<CargoRole>(o);
    if (const auto* d = t.live.load(std::memory_order_acquire))
        return d->detectedRole;
    return CargoRole::Generic;
}

void CargoHold::setRoleOverride(int track, int roleOrMinusOne)
{
    if (track < 0 || track >= c_numTracks)
        return;
    const int v = (roleOrMinusOne >= 0 && roleOrMinusOne < c_numCargoRoles) ? roleOrMinusOne : -1;
    if (m_tracks[static_cast<size_t>(track)].roleOverride.exchange(v) != v)
        sendChangeMessage();
}

juce::String CargoHold::typeName(CargoRole role)
{
    switch (role)
    {
        case CargoRole::Bed:     return "Energy Bed";
        case CargoRole::Tonal:   return "Tonal Content";
        case CargoRole::Impact:  return "Impact";
        case CargoRole::Air:     return "Air / Swish";
        case CargoRole::Crackle: return "Crackle";
        case CargoRole::Rumble:  return "Rumble";
        case CargoRole::Generic:
        default:                 return "Mixed Content";
    }
}

juce::String CargoHold::getDisplayName(int track) const
{
    // A user rename wins. Otherwise the track is named after what the scan found it to be
    // ("Tonal Content", "Energy Bed", ...). Not numbered: the lane colour tells two of a kind apart.
    if (track < 0 || track >= c_numTracks)
        return {};
    const auto& t = m_tracks[static_cast<size_t>(track)];
    if (t.userName.isNotEmpty())
        return t.userName;
    if (t.current == nullptr)
        return "Track " + juce::String(track + 1);
    return typeName(getRole(track));
}

void CargoHold::rename(int track, const juce::String& name)
{
    if (track < 0 || track >= c_numTracks)
        return;
    {
        const juce::ScopedLock sl(m_lock);
        m_tracks[static_cast<size_t>(track)].userName = name.trim();
    }
    sendChangeMessage();
}

int CargoHold::firstEmptyTrack() const
{
    for (int i = 0; i < c_numTracks; ++i)
    {
        const auto& cur = m_tracks[static_cast<size_t>(i)].current;
        if (cur == nullptr || cur->length() <= 0)
            return i;
    }
    return -1;
}

//==============================================================================
CargoHold::Meta CargoHold::metaOf(const Track& t) const
{
    Meta m;
    m.valid = true;
    m.roleOverride = t.roleOverride.load();
    m.userName = t.userName;
    m.start = m_start[static_cast<size_t>(&t - m_tracks.data())].load();
    return m;
}

void CargoHold::applyMeta(Track& t, const Meta& m)
{
    if (! m.valid)
        return;
    t.roleOverride = m.roleOverride;
    t.userName = m.userName;
    m_start[static_cast<size_t>(&t - m_tracks.data())] = m.start;
}

void CargoHold::publish(int track, std::shared_ptr<CargoData> data, bool recordUndo)
{
    publish(track, std::move(data), recordUndo, Meta {});
}

void CargoHold::publish(int track, std::shared_ptr<CargoData> data, bool recordUndo, const Meta& undoMeta)
{
    auto& t = m_tracks[static_cast<size_t>(track)];
    if (data != nullptr && data->regions.empty()) // loadFile / restore may have analysed already
        data->analyse();

    {
        const juce::ScopedLock sl(m_lock);
        if (recordUndo)
        {
            t.undo.push_back(t.current);
            t.undoMeta.push_back(undoMeta);
            if (static_cast<int>(t.undo.size()) > c_maxUndo)
            {
                retire(t.undo.front());
                t.undo.erase(t.undo.begin());
                t.undoMeta.erase(t.undoMeta.begin());
            }
            for (auto& r : t.redo)
                retire(r);
            t.redo.clear();
            t.redoMeta.clear();
            m_undoOrder.push_back(track);
            if (m_undoOrder.size() > static_cast<size_t>(c_maxUndo * c_numTracks))
                m_undoOrder.erase(m_undoOrder.begin());
            m_redoOrder.clear(); // a new edit ends the redo chain, as in any editor
            trimUndoToBytes();
        }

        // Keep the old data alive a while even if no undo entry holds it (and while a player still reads it).
        retire(t.current);
        t.current = std::move(data);
        t.live.store(t.current.get(), std::memory_order_release);
    }
    sendChangeMessage();
}

std::shared_ptr<CargoData> CargoHold::makeEdited(int track, const juce::AudioBuffer<float>& audio) const
{
    auto d = std::make_shared<CargoData>();
    const auto& cur = m_tracks[static_cast<size_t>(track)].current;
    d->sampleRate = cur != nullptr ? cur->sampleRate : 48000.0;
    d->name = cur != nullptr ? cur->name : juce::String("Cargo");
    if (cur != nullptr)
    {
        d->detectedRole = cur->detectedRole;
        d->roleConfidence = cur->roleConfidence;
        d->autoLabel = cur->autoLabel;
    }
    d->audio = audio;
    return d; // publish() re-runs analyse() (regions, focus points, auto gain)
}

bool CargoHold::validRange(int track, int& start, int& end) const
{
    const auto& cur = m_tracks[static_cast<size_t>(track)].current;
    if (cur == nullptr)
        return false;
    if (start > end)
        std::swap(start, end);
    start = juce::jlimit(0, cur->length(), start);
    end = juce::jlimit(0, cur->length(), end);
    return end > start;
}

juce::AudioBuffer<float> CargoHold::foldToMono(const juce::AudioBuffer<float>& multi, const juce::AudioChannelSet& reported)
{
    using CT = juce::AudioChannelSet::ChannelType;
    const int channels = multi.getNumChannels(), length = multi.getNumSamples();
    juce::AudioBuffer<float> mono(1, length);
    if (channels == 1)
    {
        mono.copyFrom(0, 0, multi, 0, 0, length);
        return mono;
    }

    // Channel types: from the file when it says (WAVE_FORMAT_EXTENSIBLE mask, CAF layout), otherwise
    // the usual WAV order for the channel count (L R C LFE Ls Rs Lb Rb), otherwise alternate L / R.
    std::vector<CT> types(static_cast<size_t>(channels), CT::unknown);
    if (reported.size() == channels && ! reported.isDiscreteLayout())
    {
        for (int ch = 0; ch < channels; ++ch)
            types[static_cast<size_t>(ch)] = reported.getTypeOfChannel(ch);
    }
    else
    {
        static const std::vector<std::vector<CT>> defaults {
            {}, {}, { CT::left, CT::right },
            { CT::left, CT::right, CT::centre },
            { CT::left, CT::right, CT::leftSurround, CT::rightSurround },
            { CT::left, CT::right, CT::centre, CT::leftSurround, CT::rightSurround },
            { CT::left, CT::right, CT::centre, CT::LFE, CT::leftSurround, CT::rightSurround },
            { CT::left, CT::right, CT::centre, CT::LFE, CT::leftSurround, CT::rightSurround, CT::centreSurround },
            { CT::left, CT::right, CT::centre, CT::LFE, CT::leftSurroundRear, CT::rightSurroundRear, CT::leftSurroundSide, CT::rightSurroundSide },
        };
        for (int ch = 0; ch < channels; ++ch)
            types[static_cast<size_t>(ch)] = channels < static_cast<int>(defaults.size()) ? defaults[static_cast<size_t>(channels)][static_cast<size_t>(ch)]
                                                                                       : (ch % 2 == 0 ? CT::left : CT::right);
    }

    // ITU-R BS.775 style weights into stereo: centre and surrounds at -3 dB, centre surround and heights
    // at -6 dB, LFE left out.
    juce::AudioBuffer<float> stereo(2, length);
    stereo.clear();
    constexpr float h = 0.70710678f;
    for (int ch = 0; ch < channels; ++ch)
    {
        float l = 0.0f, r = 0.0f;
        switch (types[static_cast<size_t>(ch)])
        {
            case CT::left: case CT::wideLeft:                             l = 1.0f; break;
            case CT::right: case CT::wideRight:                           r = 1.0f; break;
            case CT::centre:                                              l = r = h; break;
            case CT::LFE: case CT::LFE2:                                  break;
            case CT::leftCentre: case CT::leftSurround: case CT::leftSurroundSide: case CT::leftSurroundRear: l = h; break;
            case CT::rightCentre: case CT::rightSurround: case CT::rightSurroundSide: case CT::rightSurroundRear: r = h; break;
            case CT::centreSurround:                                      l = r = 0.5f; break;
            case CT::topFrontLeft: case CT::topRearLeft: case CT::topSideLeft: l = 0.5f; break;
            case CT::topFrontRight: case CT::topRearRight: case CT::topSideRight: r = 0.5f; break;
            case CT::topMiddle: case CT::topFrontCentre: case CT::topRearCentre: l = r = 0.35f; break;
            default:                                                      if (ch % 2 == 0) l = h; else r = h; break;
        }
        if (l > 0.0f) stereo.addFrom(0, 0, multi, ch, 0, length, l);
        if (r > 0.0f) stereo.addFrom(1, 0, multi, ch, 0, length, r);
    }

    // Gain safe: never louder than the loudest source channel.
    float sourcePeak = 0.0f;
    for (int ch = 0; ch < channels; ++ch)
        sourcePeak = juce::jmax(sourcePeak, multi.getMagnitude(ch, 0, length));
    const float mixPeak = juce::jmax(stereo.getMagnitude(0, 0, length), stereo.getMagnitude(1, 0, length));
    if (mixPeak > sourcePeak && mixPeak > 0.0f)
        stereo.applyGain(sourcePeak / mixPeak);

    // Mono carrier: the sum, unless the channels cancel (out of phase), then the louder side.
    mono.copyFrom(0, 0, stereo, 0, 0, length);
    mono.addFrom(0, 0, stereo, 1, 0, length);
    mono.applyGain(0.5f);
    const float rl = stereo.getRMSLevel(0, 0, length), rr = stereo.getRMSLevel(1, 0, length);
    if (mono.getRMSLevel(0, 0, length) < 0.5f * 0.5f * (rl + rr))
        mono.copyFrom(0, 0, stereo, rl >= rr ? 0 : 1, 0, length);
    return mono;
}

bool CargoHold::loadFile(int track, const juce::File& file, juce::String& error)
{
    std::unique_ptr<juce::AudioFormatReader> reader(m_formats.createReaderFor(file));
    if (reader == nullptr)
    {
        error = "Couldn't read " + file.getFileName() + " (try WAV, AIFF, FLAC, OGG or MP3).";
        return false;
    }

    const double fileRate = reader->sampleRate > 0 ? reader->sampleRate : 48000.0;
    const auto length = static_cast<int>(juce::jmin<juce::int64>(reader->lengthInSamples,
                                                                static_cast<juce::int64>(fileRate * c_maxLoadSeconds)));
    if (length <= 0)
    {
        error = file.getFileName() + " is empty.";
        return false;
    }

    const int channels = static_cast<int>(juce::jmax(1u, reader->numChannels));
    juce::AudioBuffer<float> multi(channels, length);
    reader->read(&multi, 0, length, 0, true, true);

    auto d = std::make_shared<CargoData>();
    d->sampleRate = fileRate; // kept: played at its own rate
    d->name = file.getFileNameWithoutExtension();
    d->audio = foldToMono(multi, reader->getChannelLayout());
    if (reader->lengthInSamples > static_cast<juce::int64>(length)) // only the beginning fitted: say so
        d->sourceSeconds = static_cast<double>(reader->lengthInSamples) / fileRate;

    d->analyse();
    classify(*d, d->name);

    // A new sample: back to the automatic role and name, and the start marker back to the beginning (undo brings
    // the old ones back).
    auto& t = m_tracks[static_cast<size_t>(track)];
    const Meta before = metaOf(t);
    {
        const juce::ScopedLock sl(m_lock);
        t.roleOverride = -1;
        t.userName = {};
    }
    m_start[static_cast<size_t>(track)] = 0.0f;

    publish(track, std::move(d), true, before);
    return true;
}

juce::String CargoHold::getLoadNote(int track) const
{
    if (track < 0 || track >= c_numTracks)
        return {};
    const auto& cur = m_tracks[static_cast<size_t>(track)].current;
    if (cur == nullptr || cur->sourceSeconds <= 0.0 || cur->sampleRate <= 0.0)
        return {};
    return "Loaded the first " + clockTime(cur->length() / cur->sampleRate) + " of " + clockTime(cur->sourceSeconds);
}

CargoHold::LoadReport CargoHold::loadFiles(const juce::StringArray& paths, int firstTrack)
{
    LoadReport report;
    int next = firstTrack;
    for (const auto& path : paths)
    {
        const juce::File file(path);
        const int target = (next >= 0 && next < c_numTracks) ? next : firstEmptyTrack();
        if (target < 0)
        {
            ++report.noRoom; // counted, not dropped
            continue;
        }
        juce::String error;
        if (loadFile(target, file, error))
        {
            report.tracks.push_back(target);
            report.names.add(getDisplayName(target));
            const auto note = getLoadNote(target);
            if (note.isNotEmpty())
                report.notes.add(paths.size() > 1 ? note + " (" + file.getFileName() + ")" : note);
            next = -1; // after the first explicit target, fill empty tracks
        }
        else
        {
            report.failed.add(file.getFileName());
            report.reasons.add(error);
        }
    }
    return report;
}

juce::String CargoHold::LoadReport::summary() const
{
    juce::StringArray parts;
    if (names.size() > 3)
        parts.add("Added " + juce::String(names.size()) + ".");
    else if (! names.isEmpty())
        parts.add("Added " + names.joinIntoString(", ") + ".");

    if (noRoom > 0)
    {
        const juce::String most = c_numTracks == 10 ? juce::String("ten") : juce::String(c_numTracks);
        parts.add(juce::String(noRoom) + " didn't fit (" + most + " tracks at most).");
    }

    if (failed.size() == 1)
        parts.add(reasons[0]);
    else if (failed.size() > 1)
    {
        juce::StringArray shown;
        for (int i = 0; i < juce::jmin(3, failed.size()); ++i)
            shown.add(failed[i]);
        juce::String text = "Couldn't read " + shown.joinIntoString(", ");
        if (failed.size() > shown.size())
            text << " and " << (failed.size() - shown.size()) << " more";
        parts.add(text + ".");
    }

    for (const auto& note : notes)
        parts.add(note + ".");
    return parts.joinIntoString(" ");
}

void CargoHold::clear(int track)
{
    auto& t = m_tracks[static_cast<size_t>(track)];
    const Meta before = metaOf(t);
    {
        const juce::ScopedLock sl(m_lock);
        t.roleOverride = -1;
        t.userName = {};
    }
    m_start[static_cast<size_t>(track)] = 0.0f;
    if (t.current != nullptr)
        publish(track, nullptr, true, before);
    else
        sendChangeMessage();
}

void CargoHold::copy(int track, int start, int end)
{
    if (! validRange(track, start, end))
        return;
    const auto& cur = m_tracks[static_cast<size_t>(track)].current;
    m_clipboard.setSize(1, end - start);
    m_clipboard.copyFrom(0, 0, cur->audio, 0, start, end - start);
    m_clipboardRate = cur->sampleRate;
    sendChangeMessage();
}

void CargoHold::cut(int track, int start, int end)
{
    copy(track, start, end);
    erase(track, start, end);
}

void CargoHold::erase(int track, int start, int end)
{
    if (! validRange(track, start, end))
        return;
    const auto& src = m_tracks[static_cast<size_t>(track)].current->audio;
    const int length = src.getNumSamples() - (end - start);
    if (length <= 0)
    {
        publish(track, nullptr, true);
        return;
    }
    juce::AudioBuffer<float> out(1, length);
    out.copyFrom(0, 0, src, 0, 0, start);
    out.copyFrom(0, start, src, 0, end, src.getNumSamples() - end);
    publish(track, makeEdited(track, out), true);
}

void CargoHold::paste(int track, int position)
{
    if (! hasClipboard())
        return;
    const auto& cur = m_tracks[static_cast<size_t>(track)].current;
    if (cur == nullptr)
    {
        auto d = std::make_shared<CargoData>();
        d->name = "Pasted";
        d->sampleRate = m_clipboardRate;
        d->audio = m_clipboard;
        d->analyse();
        classify(*d, {});
        publish(track, std::move(d), true);
        return;
    }
    const auto clipboard = resampled(m_clipboard, m_clipboardRate, cur->sampleRate);
    const auto& src = cur->audio;
    position = juce::jlimit(0, src.getNumSamples(), position);
    const int clip = clipboard.getNumSamples();
    juce::AudioBuffer<float> out(1, src.getNumSamples() + clip);
    out.copyFrom(0, 0, src, 0, 0, position);
    out.copyFrom(0, position, clipboard, 0, 0, clip);
    out.copyFrom(0, position + clip, src, 0, position, src.getNumSamples() - position);
    publish(track, makeEdited(track, out), true);
}

void CargoHold::duplicate(int track, int start, int end)
{
    if (! validRange(track, start, end))
        return;
    const auto& src = m_tracks[static_cast<size_t>(track)].current->audio;
    const int len = end - start;
    juce::AudioBuffer<float> out(1, src.getNumSamples() + len);
    out.copyFrom(0, 0, src, 0, 0, end);
    out.copyFrom(0, end, src, 0, start, len);
    out.copyFrom(0, end + len, src, 0, end, src.getNumSamples() - end);
    publish(track, makeEdited(track, out), true);
}

void CargoHold::trim(int track, int start, int end)
{
    if (! validRange(track, start, end))
        return;
    const auto& src = m_tracks[static_cast<size_t>(track)].current->audio;
    juce::AudioBuffer<float> out(1, end - start);
    out.copyFrom(0, 0, src, 0, start, end - start);
    publish(track, makeEdited(track, out), true);
}

void CargoHold::reverse(int track, int start, int end)
{
    if (! validRange(track, start, end))
        return;
    juce::AudioBuffer<float> out(m_tracks[static_cast<size_t>(track)].current->audio);
    out.reverse(0, start, end - start);
    publish(track, makeEdited(track, out), true);
}

void CargoHold::normalise(int track, int start, int end)
{
    if (! validRange(track, start, end))
        return;
    juce::AudioBuffer<float> out(m_tracks[static_cast<size_t>(track)].current->audio);
    const float peak = out.getMagnitude(0, start, end - start);
    if (peak < 1.0e-6f)
        return;
    out.applyGain(0, start, end - start, juce::Decibels::decibelsToGain(-1.0f) / peak);
    publish(track, makeEdited(track, out), true);
}

void CargoHold::fade(int track, int start, int end, bool fadeIn)
{
    if (! validRange(track, start, end))
        return;
    juce::AudioBuffer<float> out(m_tracks[static_cast<size_t>(track)].current->audio);
    float* d = out.getWritePointer(0);
    const int len = end - start;
    for (int i = 0; i < len; ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(juce::jmax(1, len - 1));
        const float g = std::sin(0.5f * juce::MathConstants<float>::pi * (fadeIn ? t : 1.0f - t));
        d[start + i] *= g * g;
    }
    publish(track, makeEdited(track, out), true);
}

int CargoHold::undoLatest()
{
    while (! m_undoOrder.empty())
    {
        const int track = m_undoOrder.back();
        m_undoOrder.pop_back();
        if (canUndo(track))
        {
            undo(track);
            m_redoOrder.push_back(track);
            return track;
        }
    }
    return -1;
}

int CargoHold::redoLatest()
{
    while (! m_redoOrder.empty())
    {
        const int track = m_redoOrder.back();
        m_redoOrder.pop_back();
        if (canRedo(track))
        {
            redo(track);
            m_undoOrder.push_back(track);
            return track;
        }
    }
    return -1;
}

bool CargoHold::canUndoLatest() const
{
    for (const int track : m_undoOrder)
        if (canUndo(track))
            return true;
    return false;
}

bool CargoHold::canRedoLatest() const
{
    for (const int track : m_redoOrder)
        if (canRedo(track))
            return true;
    return false;
}

void CargoHold::undo(int track)
{
    auto& t = m_tracks[static_cast<size_t>(track)];
    if (t.undo.empty())
        return;
    {
        const juce::ScopedLock sl(m_lock);
        const Meta entry = t.undoMeta.empty() ? Meta {} : t.undoMeta.back();
        t.redo.push_back(t.current);
        t.redoMeta.push_back(entry.valid ? metaOf(t) : Meta {});
        retire(t.current);
        restoreMips(t.undo.back()); // an old step keeps only its audio (see purgeRetired)
        t.current = t.undo.back();
        t.undo.pop_back();
        if (! t.undoMeta.empty())
            t.undoMeta.pop_back();
        applyMeta(t, entry); // undoing a load / clear brings back the lane's name and role
        t.live.store(t.current.get(), std::memory_order_release);
    }
    sendChangeMessage();
}

void CargoHold::redo(int track)
{
    auto& t = m_tracks[static_cast<size_t>(track)];
    if (t.redo.empty())
        return;
    {
        const juce::ScopedLock sl(m_lock);
        const Meta entry = t.redoMeta.empty() ? Meta {} : t.redoMeta.back();
        t.undo.push_back(t.current);
        t.undoMeta.push_back(entry.valid ? metaOf(t) : Meta {});
        retire(t.current);
        restoreMips(t.redo.back());
        t.current = t.redo.back();
        t.redo.pop_back();
        if (! t.redoMeta.empty())
            t.redoMeta.pop_back();
        applyMeta(t, entry);
        t.live.store(t.current.get(), std::memory_order_release);
    }
    sendChangeMessage();
}

//==============================================================================
juce::ValueTree CargoHold::toValueTree() const
{
    juce::ValueTree tree("Cargo");

    // Mixer state for every lane (built-ins and sample tracks), as "0"/"1" strings.
    {
        juce::String mute, solo;
        for (int lane = 0; lane < c_numLanes; ++lane)
        {
            mute << (m_mute[static_cast<size_t>(lane)].load() ? "1" : "0");
            solo << (m_solo[static_cast<size_t>(lane)].load() ? "1" : "0");
        }
        juce::ValueTree mixer("Mixer");
        mixer.setProperty("mute", mute, nullptr);
        mixer.setProperty("solo", solo, nullptr);
        tree.appendChild(mixer, nullptr);
    }

    // Start markers, one per track (fromValueTree reads them back).
    {
        juce::StringArray starts;
        for (const auto& s : m_start)
            starts.add(juce::String(s.load(), 6));
        tree.setProperty("starts", starts.joinIntoString(","), nullptr);
    }

    // Snapshot under the lock (hosts may save from a background thread while the UI edits). The local
    // shared_ptrs keep the data alive for the whole encode, whatever the message thread does meanwhile.
    struct Snapshot
    {
        std::shared_ptr<const CargoData> data;
        juce::String userName;
        bool skip = true;
        int role = -1;
    };
    std::array<Snapshot, c_numTracks> snap;
    {
        const juce::ScopedLock sl(m_lock);
        for (size_t i = 0; i < snap.size(); ++i)
        {
            const auto& t = m_tracks[i];
            snap[i] = { t.current, t.userName, t.skipSilence.load(), t.roleOverride.load() };
        }
    }

    const juce::ScopedLock encodeLock(m_encodeLock);
    for (int i = 0; i < c_numTracks; ++i)
    {
        const auto& sn = snap[static_cast<size_t>(i)];
        juce::ValueTree node("Track");
        node.setProperty("index", i, nullptr);
        node.setProperty("skipSilence", sn.skip, nullptr);
        node.setProperty("roleOverride", sn.role, nullptr);
        if (sn.userName.isNotEmpty())
            node.setProperty("userName", sn.userName, nullptr);

        const auto& d = sn.data;
        if (d != nullptr && d->length() > 0)
        {
            node.setProperty("name", d->name, nullptr);
            node.setProperty("sampleRate", d->sampleRate, nullptr);
            node.setProperty("detectedRole", static_cast<int>(d->detectedRole), nullptr);
            node.setProperty("roleConfidence", d->roleConfidence, nullptr);
            node.setProperty("autoLabel", d->autoLabel, nullptr);

            // Encoding is cached per data object: autosaves / undo snapshots don't re-encode unchanged tracks.
            auto& cache = m_encoded[static_cast<size_t>(i)];
            const auto cached = cache.source.lock();
            if (cached.get() != d.get())
            {
                cache = Encoded {};
                // FLAC keeps the saved session small. Integer FLAC clips at full scale, so audio that goes over
                // 0 dBFS (float sources) is stored scaled down with its gain; raw floats if FLAC refuses the rate.
                const float peak = d->audio.getMagnitude(0, 0, d->length());
                const double gain = peak > 1.0f ? static_cast<double>(peak) : 1.0;
                juce::FlacAudioFormat flac;
                auto* stream = new juce::MemoryOutputStream(cache.block, false);
                std::unique_ptr<juce::AudioFormatWriter> writer(
                    flac.createWriterFor(stream, d->sampleRate, 1, 24, {}, 0));
                if (writer != nullptr)
                {
                    if (gain > 1.0)
                    {
                        juce::AudioBuffer<float> scaled(d->audio);
                        scaled.applyGain(static_cast<float>(1.0 / gain));
                        writer->writeFromAudioSampleBuffer(scaled, 0, d->length());
                    }
                    else
                    {
                        writer->writeFromAudioSampleBuffer(d->audio, 0, d->length());
                    }
                    writer.reset();
                    cache.gain = gain;
                }
                else
                {
                    delete stream;
                    cache.block.reset();
                    cache.block.append(d->audio.getReadPointer(0), sizeof(float) * static_cast<size_t>(d->length()));
                    cache.raw = true;
                }
                cache.source = d;
            }

            if (cache.raw)
                node.setProperty("raw", juce::var(cache.block), nullptr);
            else
            {
                node.setProperty("flac", juce::var(cache.block), nullptr);
                if (cache.gain > 1.0)
                    node.setProperty("flacGain", cache.gain, nullptr);
            }
        }
        tree.appendChild(node, nullptr);
    }

    return tree;
}

void CargoHold::setStart(int track, float fraction)
{
    if (track < 0 || track >= c_numTracks)
        return;
    m_start[static_cast<size_t>(track)] = juce::jlimit(0.0f, 0.98f, fraction);
    sendChangeMessage();
}

int CargoHold::busForRole(CargoRole role)
{
    switch (role)
    {
        case CargoRole::Impact:  return c_hitsBus;   // hits: opened at the loudest moments
        case CargoRole::Bed:
        case CargoRole::Air:     return 1;           // smooth bus
        case CargoRole::Generic:
        case CargoRole::Tonal:
        case CargoRole::Crackle:
        case CargoRole::Rumble:
        default:                 return 0;           // aggressive bus: tonal, growls, crackle, rumble
    }
}

int CargoHold::getBus(int track) const
{
    return busForRole(getRole(track));
}

void CargoHold::fromValueTree(const juce::ValueTree& tree)
{
    {
        juce::StringArray starts;
        starts.addTokens(tree.getProperty("starts").toString(), ",", {});
        for (int i = 0; i < c_numTracks; ++i)
            m_start[static_cast<size_t>(i)] = i < starts.size() ? juce::jlimit(0.0f, 0.98f, starts[i].getFloatValue()) : 0.0f;
    }
    // Mixer (absent in v1 sessions: everything unmuted / unsoloed).
    {
        const auto mixer = tree.getChildWithName("Mixer");
        const auto mute = mixer.isValid() ? mixer.getProperty("mute").toString() : juce::String();
        const auto solo = mixer.isValid() ? mixer.getProperty("solo").toString() : juce::String();
        for (int lane = 0; lane < c_numLanes; ++lane)
        {
            setMute(lane, lane < mute.length() && mute[lane] == '1');
            setSolo(lane, lane < solo.length() && solo[lane] == '1');
        }
    }

    // A restored session replaces everything: tracks missing from the tree (older 4-track sessions, a state with
    // no cargo) are cleared, and no undo step may bring back audio from the previous session.
    for (int i = 0; i < c_numTracks; ++i)
    {
        auto& t = m_tracks[static_cast<size_t>(i)];
        const juce::ScopedLock sl(m_lock);
        for (auto& u : t.undo) retire(u);
        for (auto& r : t.redo) retire(r);
        t.undo.clear();
        t.redo.clear();
        t.undoMeta.clear();
        t.redoMeta.clear();
        m_undoOrder.clear();
        m_redoOrder.clear();
        t.skipSilence = true;
        t.roleOverride = -1;
        t.userName = {};
    }

    std::array<bool, c_numTracks> restored {};
    for (const auto& node : tree)
    {
        if (! node.hasType("Track"))
            continue;
        const int i = node.getProperty("index", -1);
        if (i < 0 || i >= c_numTracks)
            continue;

        auto& t = m_tracks[static_cast<size_t>(i)];
        {
            const juce::ScopedLock sl(m_lock);
            t.skipSilence = static_cast<bool>(node.getProperty("skipSilence", true));
            const int role = node.getProperty("roleOverride", -1);
            t.roleOverride = (role >= 0 && role < c_numCargoRoles) ? role : -1;
            t.userName = node.getProperty("userName", juce::String()).toString();
        }

        std::shared_ptr<CargoData> d;
        const double rate = node.getProperty("sampleRate", 48000.0);
        if (auto* flac = node.getProperty("flac").getBinaryData())
        {
            juce::FlacAudioFormat format;
            std::unique_ptr<juce::AudioFormatReader> reader(
                format.createReaderFor(new juce::MemoryInputStream(*flac, false), true));
            if (reader != nullptr && reader->lengthInSamples > 0)
            {
                d = std::make_shared<CargoData>();
                const int len = static_cast<int>(reader->lengthInSamples);
                d->audio.setSize(1, len);
                reader->read(&d->audio, 0, len, 0, true, false);
                const double gain = node.getProperty("flacGain", 1.0);
                if (gain > 1.0)
                    d->audio.applyGain(static_cast<float>(gain));
            }
        }
        else if (auto* raw = node.getProperty("raw").getBinaryData())
        {
            d = std::make_shared<CargoData>();
            const int len = static_cast<int>(raw->getSize() / sizeof(float));
            d->audio.setSize(1, len);
            std::memcpy(d->audio.getWritePointer(0), raw->getData(), sizeof(float) * static_cast<size_t>(len));
        }

        if (d != nullptr)
        {
            d->sampleRate = rate;
            d->name = node.getProperty("name", "Cargo").toString();
            d->analyse();
            if (node.hasProperty("detectedRole"))
            {
                const int detected = node.getProperty("detectedRole", 0);
                d->detectedRole = static_cast<CargoRole>(juce::jlimit(0, c_numCargoRoles - 1, detected));
                d->roleConfidence = static_cast<float>(static_cast<double>(node.getProperty("roleConfidence", 0.0)));
                d->autoLabel = node.getProperty("autoLabel", juce::String()).toString();
            }
            else
            {
                classify(*d, d->name); // v1 session: detect now
            }
        }
        publish(i, std::move(d), false);
        restored[static_cast<size_t>(i)] = true;
    }

    for (int i = 0; i < c_numTracks; ++i)
        if (! restored[static_cast<size_t>(i)])
            publish(i, nullptr, false);
}

//==============================================================================
void CargoPlayer::prepare(double sampleRate)
{
    m_sampleRate = sampleRate;
    reset();
}

void CargoPlayer::reset()
{
    for (auto& h : m_heads)
        h = Head {};
    m_xfadeLeft = 0;
    m_advancePending = false;
    m_switching = false;
    m_switchGain = 1.0f;
    m_data = nullptr;
    m_pending = nullptr;
    m_loopLevel = 0.0f;
    m_jumpLeft = 0;
    m_lastSkip = -1;
    m_loopMip = MipState {};
    resetShots();
    m_lastPlayhead[0] = m_lastPlayhead[1] = -1.0f;
}

void CargoPlayer::reseatHeads(const CargoData& d, bool skipSilence)
{
    // Skip Silence flipped: the heads' region index now points into a different region set. Point each active
    // head at the region that contains it, so playback carries on seamlessly. A head sitting in a gap keeps the
    // next region; it is then past that region's end and fades out over the floored crossfade in renderLoop.
    for (auto& h : m_heads)
    {
        if (! h.active)
            continue;
        if (! skipSilence || d.regions.empty())
        {
            h.region = 0;
            continue;
        }
        // Region indices count from the first sound after the start marker (see regionAt).
        const int first = firstRegion(d, m_startFraction);
        int index = static_cast<int>(d.regions.size()) - 1;
        for (size_t r = 0; r < d.regions.size(); ++r)
        {
            if (h.pos < static_cast<double>(d.regions[r].end))
            {
                index = static_cast<int>(r);
                break;
            }
        }
        h.region = juce::jlimit(0, juce::jmax(0, numRegions(d, true) - 1), index - first);
    }
}

void CargoPlayer::resetShots()
{
    for (auto& s : m_shots)
        s = Shot {};
    m_shotRegion = -1;
}

int CargoPlayer::firstRegion(const CargoData& d, float startFraction)
{
    // The first sound that reaches past the start marker (all of them if the marker is past the last one).
    // Regions are sorted and don't overlap: binary search (this runs per sample in the loop player).
    const int startSample = static_cast<int>(startFraction * static_cast<float>(d.length()));
    const auto it = std::partition_point(d.regions.begin(), d.regions.end(),
                                         [startSample](const CargoRegion& r) { return r.end <= startSample + 64; });
    return it == d.regions.end() ? 0 : static_cast<int>(it - d.regions.begin());
}

int CargoPlayer::countRegions(const CargoData& d, bool skipSilence, float startFraction)
{
    if (d.length() <= 0)
        return 0;
    return skipSilence ? static_cast<int>(d.regions.size()) - firstRegion(d, startFraction) : 1;
}

CargoRegion CargoPlayer::regionAt(const CargoData& d, bool skipSilence, float startFraction, int index)
{
    const int startSample = static_cast<int>(startFraction * static_cast<float>(d.length()));
    const int first = firstRegion(d, startFraction);
    if (! skipSilence || d.regions.empty())
    {
        // Skip silence off: the whole file from the marker plays as it is. Its focus (where hit sync lines it up)
        // is the transient of the first sound after the marker, not of a sound the marker has already passed.
        CargoRegion whole { juce::jlimit(0, juce::jmax(0, d.length() - 64), startSample), d.length(), 0 };
        whole.focus = whole.start;
        if (! d.regions.empty())
            whole.focus = juce::jlimit(whole.start, juce::jmax(whole.start, whole.end - 1), d.regions[static_cast<size_t>(first)].focus);
        return whole;
    }
    auto r = d.regions[static_cast<size_t>(juce::jlimit(0, static_cast<int>(d.regions.size()) - 1, first + index))];
    if (r.start < startSample && r.end > startSample + 64)
    {
        r.start = startSample;
        r.focus = juce::jlimit(r.start, r.end - 1, r.focus);
    }
    return r;
}

int CargoPlayer::numRegions(const CargoData& d, bool skipSilence) const
{
    return countRegions(d, skipSilence, m_startFraction);
}

CargoRegion CargoPlayer::regionFor(const CargoData& d, bool skipSilence, int index) const
{
    return regionAt(d, skipSilence, m_startFraction, index);
}

void HitWindow::extend(const CargoData& d, bool skipSilence, float startFraction)
{
    if (d.sampleRate <= 0.0)
        return;
    const int n = CargoPlayer::countRegions(d, skipSilence, startFraction);
    for (int i = 0; i < n; ++i)
    {
        const auto r = CargoPlayer::regionAt(d, skipSilence, startFraction, i);
        int end = r.end;
        // Skip silence off plays the whole file as one hit. Its tail would stretch every hit track's beat to the
        // cap, so the beat is sized by its first sound; the rest of the file plays on until the next beat.
        if (! skipSilence && ! d.regions.empty())
            end = juce::jlimit(r.focus + 1, r.end, d.regions[static_cast<size_t>(CargoPlayer::firstRegion(d, startFraction))].end);
        pre = std::max(pre, (r.focus - r.start) / d.sampleRate);
        post = std::max(post, (end - r.focus) / d.sampleRate);
    }
}

void CargoPlayer::startHead(Head& h, const CargoData& d, bool skipSilence, float chaos, sj::Random& rng, bool first, bool advance)
{
    const int n = numRegions(d, skipSilence);
    const int previous = m_heads[static_cast<size_t>(m_current)].region;

    // Stay on the same sound unless a new whoosh asked us to move on.
    int next = first ? 0 : (advance ? (previous + 1) % juce::jmax(1, n) : previous);
    if ((first || advance) && n > 1 && rng.unipolar() < chaos * 0.8f)
        next = static_cast<int>(rng.unipolar() * static_cast<float>(n)) % n;

    const auto r = regionFor(d, skipSilence, next);
    const int len = r.end - r.start;
    double offset = 0.0;
    if ((first || advance) && chaos > 0.0f && len > static_cast<int>(d.sampleRate * 0.3))
        offset = static_cast<double>(rng.unipolar() * chaos * 0.3f * static_cast<float>(len));

    h.region = next;
    h.pos = r.start + offset;
    h.active = true;
}

float CargoPlayer::readBandLimited(const CargoData& d, int level, double pos, double inc)
{
    if (level <= 0 || level > CargoData::c_numMips || d.mips[static_cast<size_t>(level - 1)].empty())
        return sincRead(d.audio.getReadPointer(0), d.length(), pos, stretchFor(inc));
    const auto& mip = d.mips[static_cast<size_t>(level - 1)];
    const double scale = 1.0 / static_cast<double>(1 << level);
    return sincRead(mip.data(), static_cast<int>(mip.size()), pos * scale, stretchFor(inc * scale));
}

int CargoPlayer::levelFor(const CargoData& d, double inc)
{
    // The copy whose own speed is between 1 and 2: nothing above the output's Nyquist is left in it to speak of,
    // and nothing below is missing.
    int level = 0;
    while (level < CargoData::c_numMips && ! d.mips[static_cast<size_t>(level)].empty() && inc >= 2.0)
    {
        inc *= 0.5;
        ++level;
    }
    return level;
}

CargoPlayer::MipRead CargoPlayer::beginRead(MipState& state, const CargoData& d, double inc, int frames)
{
    MipRead r;
    r.inc = inc;
    r.level = levelFor(d, inc);
    r.from = r.level;
    const int current = state.level;
    const bool usable = current == 0
                        || (current > 0 && current <= CargoData::c_numMips && ! d.mips[static_cast<size_t>(current - 1)].empty());
    if (usable && current != r.level)
    {
        // A little slack either side, so a pitch hovering on an octave doesn't flip copies every chunk.
        const double own = inc / static_cast<double>(1 << current);
        if (own >= 0.9 && own < 2.2)
            r.level = r.from = current;
        else
        {
            r.from = current;                 // crossfade from the copy the last chunk used
            r.fade = juce::jmin(frames, 96);
        }
    }
    state.level = r.level;
    return r;
}

float CargoPlayer::readAt(const CargoData& d, const MipRead& r, double pos, int sampleInChunk)
{
    const float now = readBandLimited(d, r.level, pos, r.inc);
    if (sampleInChunk >= r.fade)
        return now;
    const float before = readBandLimited(d, r.from, pos, r.inc);
    return before + (now - before) * (static_cast<float>(sampleInChunk + 1) / static_cast<float>(r.fade));
}

float CargoPlayer::getPlayhead(int head) const
{
    return m_lastPlayhead[head];
}

//==============================================================================
void CargoPlayer::render(const CargoData* data, CargoRole role, bool skipSilence, float rate, float chaos, sj::Random& rng,
                         float fadeSeconds, const CargoCue& cue, float* out, int frames)
{
    if (role == CargoRole::Rumble)
        rate = std::sqrt(juce::jmax(0.01f, rate)); // follows PITCH at half strength

    // A new edit arrived: fade the old data out (it is kept alive by the hold), then switch.
    if (data != m_data)
    {
        if (m_data == nullptr)
        {
            m_data = data;
            for (auto& h : m_heads) h.active = false;
            m_xfadeLeft = 0;
            m_switchGain = 0.0f;
            m_loopLevel = 0.0f;
            resetShots();
        }
        else
        {
            m_switching = true;
            m_pending = data;
        }
    }

    std::fill(out, out + frames, 0.0f);

    const int skipNow = skipSilence ? 1 : 0;
    if (m_lastSkip != skipNow)
    {
        if (m_lastSkip >= 0 && m_data != nullptr)
            reseatHeads(*m_data, skipSilence);
        m_lastSkip = skipNow;
    }

    if (m_data == nullptr || numRegions(*m_data, skipSilence) == 0)
    {
        if (m_switching)
        {
            m_data = m_pending;
            m_switching = false;
            m_switchGain = 0.0f;
            for (auto& h : m_heads) h.active = false;
            m_xfadeLeft = 0;
            m_loopLevel = 0.0f;
            resetShots();
        }
        m_lastPlayhead[0] = m_lastPlayhead[1] = -1.0f;
        return;
    }

    const CargoData& d = *m_data;
    const bool oneShot = isOneShotRole(role);

    // Loop behaviours (also run while fading out after a switch to a one-shot role).
    renderLoop(d, role, skipSilence, rate, chaos, rng, fadeSeconds, cue.active && ! oneShot, out, frames);
    // One-shots (also run while tails finish after a switch to a loop role).
    renderShots(d, role, skipSilence, rate, chaos, rng, fadeSeconds, cue, oneShot, out, frames);

    const float switchStep = 1.0f / 256.0f;
    for (int i = 0; i < frames; ++i)
    {
        if (m_switching)
        {
            m_switchGain -= switchStep;
            if (m_switchGain <= 0.0f)
            {
                m_switchGain = 0.0f;
                m_data = m_pending;
                m_switching = false;
                for (auto& h : m_heads) h.active = false;
                m_xfadeLeft = 0;
                m_loopLevel = 0.0f;
                resetShots();
                std::fill(out + i, out + frames, 0.0f);
                m_lastPlayhead[0] = m_lastPlayhead[1] = -1.0f;
                return; // continue with the new data next chunk
            }
        }
        else if (m_switchGain < 1.0f)
        {
            m_switchGain = juce::jmin(1.0f, m_switchGain + switchStep);
        }
        out[i] *= d.autoGain * m_switchGain;
    }

    // Playheads for the UI: the loop heads first, then the newest playing one-shots.
    const float invLength = 1.0f / static_cast<float>(juce::jmax(1, d.length()));
    int slot = 0;
    float heads[2] { -1.0f, -1.0f };
    if (m_loopLevel > 0.0f)
    {
        const auto& cur = m_heads[static_cast<size_t>(m_current)];
        const auto& old = m_heads[static_cast<size_t>(1 - m_current)];
        if (cur.active) heads[slot++] = static_cast<float>(cur.pos) * invLength;
        if (old.active && m_xfadeLeft > 0 && slot < 2) heads[slot++] = static_cast<float>(old.pos) * invLength;
    }
    const Shot* taken = nullptr;
    while (slot < 2)
    {
        const Shot* newest = nullptr;
        for (const auto& s : m_shots)
            if (s.state == 2 && &s != taken && (newest == nullptr || s.age > newest->age))
                newest = &s;
        if (newest == nullptr)
            break;
        heads[slot++] = static_cast<float>(newest->pos) * invLength;
        taken = newest;
    }
    m_lastPlayhead[0] = heads[0];
    m_lastPlayhead[1] = heads[1];
}

//==============================================================================
void CargoPlayer::renderLoop(const CargoData& d, CargoRole role, bool skipSilence, float rate, float chaos, sj::Random& rng,
                             float fadeSeconds, bool cue, float* out, int frames)
{
    const bool loopRole = ! isOneShotRole(role);
    const bool cycles = role == CargoRole::Bed || role == CargoRole::Rumble; // move on at region ends, ignore whooshes
    const bool crackle = role == CargoRole::Crackle;

    if (cue && ! cycles)
        m_advancePending = true;

    if (! loopRole)
    {
        m_advancePending = false;
        if (m_loopLevel <= 0.0f || ! m_heads[static_cast<size_t>(m_current)].active)
        {
            m_loopLevel = 0.0f;
            for (auto& h : m_heads) h.active = false;
            m_xfadeLeft = 0;
            return;
        }
    }

    const int length = d.length();
    const double inc = static_cast<double>(juce::jmax(0.01f, rate)) * d.sampleRate / m_sampleRate;
    const MipRead mip = beginRead(m_loopMip, d, inc, frames);
    const int xfadeLen = juce::jmax(64, static_cast<int>(fadeSeconds * m_sampleRate));
    const float levelStep = 1.0f / static_cast<float>(0.02 * m_sampleRate);
    const float levelTarget = loopRole ? 1.0f : 0.0f;

    auto crackleInterval = [&]
    {
        const float seconds = (0.12f + 1.4f * (1.0f - chaos)) * rng.range(0.6f, 1.4f);
        return juce::jmax(64, static_cast<int>(seconds * static_cast<float>(m_sampleRate)));
    };

    if (! m_heads[static_cast<size_t>(m_current)].active)
    {
        startHead(m_heads[static_cast<size_t>(m_current)], d, skipSilence, chaos, rng, true, false);
        m_advancePending = false;
        m_xfadeLeft = 0;
        m_jumpLeft = crackleInterval();
    }

    for (int i = 0; i < frames; ++i)
    {
        // Role change fade (loop <-> one-shot)
        if (m_loopLevel < levelTarget) m_loopLevel = juce::jmin(levelTarget, m_loopLevel + levelStep);
        else if (m_loopLevel > levelTarget) m_loopLevel = juce::jmax(levelTarget, m_loopLevel - levelStep);
        if (! loopRole && m_loopLevel <= 0.0f)
        {
            for (auto& h : m_heads) h.active = false;
            m_xfadeLeft = 0;
            return;
        }

        auto& cur = m_heads[static_cast<size_t>(m_current)];
        auto& other = m_heads[static_cast<size_t>(1 - m_current)];

        if (m_xfadeLeft == 0)
        {
            const auto r = regionFor(d, skipSilence, cur.region);
            const double remaining = (static_cast<double>(r.end) - cur.pos) / inc;
            const bool atEnd = remaining <= xfadeLen;
            bool jump = false;
            int jumpInterval = xfadeLen * 2;
            if (crackle && --m_jumpLeft <= 0)
            {
                jump = true;
                jumpInterval = crackleInterval();
                m_jumpLeft = jumpInterval;
            }

            if (m_advancePending || atEnd || jump)
            {
                // Generic: move to the next sound when a new whoosh starts, otherwise tail into its own
                // beginning again. Bed / Rumble: cycle to the next sound at region ends. Crackle: jump to a
                // random spot. Every move is one long equal-power fade.
                const bool advanceNow = m_advancePending || (atEnd && cycles);
                m_advancePending = false;
                int fadeCap = xfadeLen;
                if (crackle)
                {
                    const int n = numRegions(d, skipSilence);
                    const int next = n > 1 ? static_cast<int>(rng.unipolar() * static_cast<float>(n)) % n : 0;
                    const auto nr = regionFor(d, skipSilence, next);
                    const int len = nr.end - nr.start;
                    const int keep = juce::jmin(len / 2, static_cast<int>(0.25 * d.sampleRate));
                    other.region = next;
                    other.pos = nr.start + static_cast<double>(rng.unipolar()) * static_cast<double>(juce::jmax(0, len - keep));
                    other.active = true;
                    if (jump)
                        fadeCap = juce::jmax(64, jumpInterval / 2);
                }
                else
                {
                    startHead(other, d, skipSilence, chaos, rng, false, advanceNow);
                }
                const auto nr = regionFor(d, skipSilence, other.region);
                const int newLength = static_cast<int>((static_cast<double>(nr.end) - other.pos) / inc);
                // Normally remaining ~ xfadeLen here. It is tiny or negative only when the region set changed under
                // the head (Skip Silence toggled, regions re-detected): never cut the old head in one sample, fade
                // it out from where it is (it still reads real audio past its region end).
                const double fileLeft = (static_cast<double>(length) - cur.pos) / inc;
                const double oldLeft = juce::jmax(remaining, juce::jmin(256.0, juce::jmax(64.0, fileLeft)));
                m_xfadeTotal = juce::jmax(64, juce::jmin(fadeCap, juce::jmax(64, newLength / 2), static_cast<int>(oldLeft)));
                m_xfadeLeft = m_xfadeTotal;
                m_current = 1 - m_current;
            }
        }

        float value = 0.0f;
        auto& now = m_heads[static_cast<size_t>(m_current)];
        auto& old = m_heads[static_cast<size_t>(1 - m_current)];
        if (m_xfadeLeft > 0)
        {
            const float t = 1.0f - static_cast<float>(m_xfadeLeft) / static_cast<float>(m_xfadeTotal);
            const float gNew = std::sin(0.5f * sj::c_pi * t);
            const float gOld = std::cos(0.5f * sj::c_pi * t);
            value = gNew * readAt(d, mip, now.pos, i) + gOld * readAt(d, mip, old.pos, i);
            old.pos += inc;
            if (--m_xfadeLeft == 0)
                old.active = false;
        }
        else
        {
            value = readAt(d, mip, now.pos, i);
        }
        now.pos += inc;

        out[i] += value * smooth(m_loopLevel);
    }
}

//==============================================================================
void CargoPlayer::cueShot(const CargoData& d, CargoRole role, bool skipSilence, float rate, float chaos, sj::Random& rng,
                          float fadeSeconds, const CargoCue& cue)
{
    const int n = numRegions(d, skipSilence);
    if (n == 0)
        return;

    const float fadeOutSamples = juce::jmax(1.0f, juce::jmin(fadeSeconds, 0.3f) * static_cast<float>(m_sampleRate));

    // Accents still waiting for their start are KEPT: in dense Energy mode / fast MIDI the next whoosh usually
    // arrives before the previous accent has started, and each one belongs to its own flight's peak. Older
    // accents tail out when a newer one actually starts playing (see fadeOlderShots).

    // Free voice, else drop the oldest still-silent (waiting) one, else steal the quietest playing one.
    Shot* slot = nullptr;
    for (auto& s : m_shots)
        if (s.state == 0) { slot = &s; break; }
    if (slot == nullptr)
        for (auto& s : m_shots)
            if (s.state == 1 && (slot == nullptr || s.age < slot->age))
                slot = &s;
    if (slot == nullptr)
    {
        slot = &m_shots[0];
        for (auto& s : m_shots)
            if (s.level < slot->level - 1.0e-6f || (s.level <= slot->level + 1.0e-6f && s.age < slot->age))
                slot = &s;
    }
    juce::ignoreUnused(fadeOutSamples);

    int next = (m_shotRegion + 1) % n;
    if (n > 1 && rng.unipolar() < chaos * 0.8f)
        next = static_cast<int>(rng.unipolar() * static_cast<float>(n)) % n;
    m_shotRegion = next;

    const auto r = regionFor(d, skipSilence, next);
    if (r.end - r.start < 2)
        return;
    const int focus = juce::jlimit(r.start, r.end - 1, r.focus);

    float scale = 1.0f;
    if (role == CargoRole::Air && cue.flightSeconds > 0.01)
    {
        const double lenSeconds = static_cast<double>(r.end - r.start) / d.sampleRate;
        scale = static_cast<float>(juce::jlimit(0.5, 2.0, lenSeconds / cue.flightSeconds));
    }

    const double inc = static_cast<double>(juce::jmax(0.01f, rate * scale)) * d.sampleRate / m_sampleRate;
    const double preFocus = static_cast<double>(focus - r.start) / inc; // output samples from start to focus
    const double target = juce::jmax(0.0, cue.secondsToPeak) * m_sampleRate;

    Shot s;
    s.rateScale = scale;
    s.end = r.end;
    s.level = 0.0f;
    s.age = ++m_shotCounter;
    if (preFocus <= target)
    {
        s.pos = r.start;
        s.wait = static_cast<int>(std::lround(target - preFocus));
        s.step = 1.0f / static_cast<float>(0.001 * m_sampleRate); // region starts on its pre-roll: a tiny fade is enough
    }
    else
    {
        s.pos = focus - target * inc; // not enough time: start inside the region
        s.wait = 0;
        s.step = 1.0f / static_cast<float>(0.01 * m_sampleRate);
    }
    // End fade: up to fadeOutSamples, but never more than half of what is left to play (short regions).
    const double playable = (static_cast<double>(s.end) - s.pos) / inc;
    s.tail = static_cast<float>(juce::jmax(1.0, juce::jmin(static_cast<double>(fadeOutSamples), playable * 0.5)));
    s.state = s.wait > 0 ? 1 : 2;
    *slot = s;
    if (s.state == 2)
        fadeOlderShots(s.age, fadeOutSamples);
}

void CargoPlayer::fadeOlderShots(int age, float fadeOutSamples)
{
    // A newer accent started: the ones before it tail out.
    for (auto& s : m_shots)
    {
        if (s.state != 2 || s.age >= age)
            continue;
        const float step = -juce::jmax(s.level, 1.0e-3f) / fadeOutSamples;
        if (! s.fadingOut || step < s.step)
            s.step = step;
        s.fadingOut = true;
    }
}

void CargoPlayer::renderShots(const CargoData& d, CargoRole role, bool skipSilence, float rate, float chaos, sj::Random& rng,
                              float fadeSeconds, const CargoCue& cue, bool acceptCue, float* out, int frames)
{
    if (acceptCue && cue.active)
        cueShot(d, role, skipSilence, rate, chaos, rng, fadeSeconds, cue);

    const float fadeOutSamples = juce::jmax(1.0f, juce::jmin(fadeSeconds, 0.3f) * static_cast<float>(m_sampleRate));
    if (! acceptCue)
    {
        // Role changed to a loop: let the accents tail out, drop the ones not yet started.
        for (auto& s : m_shots)
        {
            if (s.state == 1)
                s = Shot {};
            else if (s.state == 2 && ! s.fadingOut)
            {
                s.fadingOut = true;
                s.step = -juce::jmax(s.level, 1.0e-3f) / fadeOutSamples;
            }
        }
    }

    const double baseInc = static_cast<double>(juce::jmax(0.01f, rate)) * d.sampleRate / m_sampleRate;

    for (auto& s : m_shots)
    {
        if (s.state == 0)
            continue;
        const double inc = baseInc * static_cast<double>(s.rateScale);
        const MipRead mip = beginRead(s.mip, d, inc, frames);
        for (int i = 0; i < frames; ++i)
        {
            if (s.state == 1)
            {
                if (s.wait > 0) { --s.wait; continue; }
                s.state = 2;
                fadeOlderShots(s.age, fadeOutSamples);
            }

            // Tail out to silence at the region end (independent of the fade-in / cue fade-out level).
            const double remaining = (static_cast<double>(s.end) - s.pos) / inc;
            const float endGain = juce::jlimit(0.0f, 1.0f, static_cast<float>(remaining) / s.tail);

            s.level += s.step;
            if (! s.fadingOut && s.level >= 1.0f) { s.level = 1.0f; s.step = 0.0f; }

            if (s.pos >= static_cast<double>(s.end) || (s.fadingOut && s.level <= 0.0f))
            {
                s = Shot {};
                break;
            }

            out[i] += smooth(s.level) * smooth(endGain) * readAt(d, mip, s.pos, i);
            s.pos += inc;
        }
    }
}
