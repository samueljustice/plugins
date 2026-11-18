#include "DoomAudio.h"

#include <array>
#include <cmath>

namespace
{
    constexpr double twoPi = 6.283185307179586;

    //==========================================================================
    // A small General-MIDI-ish synth: enough to make DOOM's MIDI sound like DOOM
    // (chugging distorted guitar, synth bass, gated drums) without a soundfont.
    class MiniSynth
    {
    public:
        void reset()
        {
            for (auto& v : voices) v.active = false;
            for (auto& c : channels) c = {};
        }

        void allNotesOff()
        {
            for (auto& v : voices)
                if (v.active)
                    v.stage = Voice::Release;
        }

        void handle (const uint8_t* m)
        {
            const int status = m[0] & 0xf0, ch = m[0] & 0x0f;
            auto& c = channels[(size_t) ch];

            switch (status)
            {
                case 0x90:
                    if (m[2] > 0) { noteOn (ch, m[1], m[2] / 127.0f); break; }
                    [[fallthrough]];
                case 0x80:
                    for (auto& v : voices)
                        if (v.active && v.ch == ch && v.note == m[1] && ! v.drum)
                            v.stage = Voice::Release;
                    break;
                case 0xb0:
                    switch (m[1])
                    {
                        case 7:   c.volume = m[2] / 127.0f; break;
                        case 10:  c.pan = m[2] / 127.0f; break;
                        case 11:  c.expression = m[2] / 127.0f; break;
                        case 120: case 123:
                            for (auto& v : voices) if (v.active && v.ch == ch) v.stage = Voice::Release;
                            break;
                        case 121: c.bend = 0.0f; c.expression = 1.0f; break;
                        default: break;
                    }
                    break;
                case 0xc0: c.program = m[1]; break;
                case 0xe0: c.bend = (float) (((m[2] << 7) | m[1]) - 8192) / 8192.0f; break;
                default: break;
            }
        }

        void render (float* left, float* right, int n, double sr)
        {
            for (auto& v : voices)
            {
                if (! v.active)
                    continue;

                const auto& c = channels[(size_t) v.ch];
                const float chanGain = c.volume * c.volume * c.expression * v.vel * 0.34f;
                const float panL = std::cos (c.pan * 1.5707963f), panR = std::sin (c.pan * 1.5707963f);
                const double f = v.freq * std::pow (2.0, c.bend * 2.0 / 12.0);
                const double inc = f / sr, inc2 = inc * v.detune;

                for (int i = 0; i < n; ++i)
                {
                    const float env = v.nextEnv (sr);
                    if (! v.active)
                        break;

                    float s = v.drum ? v.drumSample (sr) : v.toneSample (inc, inc2, sr);
                    s *= env * chanGain;
                    left[i] += s * panL;
                    right[i] += s * panR;
                }
            }
        }

    private:
        enum Kind { Piano, Mallet, Organ, CleanGuitar, DistGuitar, Bass, Strings, Brass, Reed, Lead, Pad, Other };

        struct Channel
        {
            int program = 0;
            float volume = 100.0f / 127.0f, expression = 1.0f, pan = 0.5f, bend = 0.0f;
        };

        struct Voice
        {
            enum Stage { Attack, Decay, Release };

            bool active = false, drum = false;
            int ch = 0, note = 0, kind = Other, drumType = 0;
            float vel = 1.0f;
            double freq = 440.0, phase = 0.0, phase2 = 0.0, detune = 1.0;
            float env = 0.0f, attack = 0.005f, decay = 0.3f, sustain = 0.7f, release = 0.1f;
            Stage stage = Attack;
            float lp = 0.0f, lpCutoff = 4000.0f, drive = 0.0f;
            float t = 0.0f, hpState = 0.0f;
            uint32_t age = 0, noise = 22222;

            float nextEnv (double sr)
            {
                const float dt = (float) (1.0 / sr);
                switch (stage)
                {
                    case Attack:
                        env += dt / attack;
                        if (env >= 1.0f) { env = 1.0f; stage = Decay; }
                        break;
                    case Decay:
                        env += (sustain - env) * (dt / decay) * 3.0f;
                        if (sustain <= 0.0f && env < 0.0005f) active = false;
                        break;
                    case Release:
                        env -= env * (dt / release) * 4.0f + 1.0e-6f;
                        if (env <= 0.0005f) active = false;
                        break;
                }
                return env;
            }

            float white()
            {
                noise ^= noise << 13; noise ^= noise >> 17; noise ^= noise << 5;
                return (float) noise * (2.0f / 4294967295.0f) - 1.0f;
            }

            static float blep (double t, double dt)
            {
                if (t < dt)       { t /= dt; return (float) (t + t - t * t - 1.0); }
                if (t > 1.0 - dt) { t = (t - 1.0) / dt; return (float) (t * t + t + t + 1.0); }
                return 0.0f;
            }

            static float saw (double ph, double dt)    { return (float) (2.0 * ph - 1.0) - blep (ph, dt); }
            static float square (double ph, double dt)
            {
                double ph2 = ph + 0.5; if (ph2 >= 1.0) ph2 -= 1.0;
                return (ph < 0.5 ? 1.0f : -1.0f) + blep (ph, dt) - blep (ph2, dt);
            }
            static float tri (double ph)               { return (float) (4.0 * std::abs (ph - 0.5) - 1.0); }

            float toneSample (double inc, double inc2, double sr)
            {
                float s = 0.0f;
                switch (kind)
                {
                    case Piano:       s = 0.6f * tri (phase) + 0.4f * saw (phase, inc); break;
                    case Mallet:      s = (float) std::sin (twoPi * phase); break;
                    case Organ:       s = 0.7f * square (phase, inc) + 0.3f * (float) std::sin (twoPi * phase * 2.0); break;
                    case CleanGuitar: s = saw (phase, inc); break;
                    case DistGuitar:  s = 0.5f * (saw (phase, inc) + saw (phase2, inc2)); break;
                    case Bass:        s = 0.55f * square (phase, inc) + 0.45f * tri (phase); break;
                    case Strings:
                    case Pad:         s = 0.5f * (saw (phase, inc) + saw (phase2, inc2)); break;
                    case Brass:       s = saw (phase, inc); break;
                    case Reed:        s = 0.6f * square (phase, inc) + 0.4f * (float) std::sin (twoPi * phase); break;
                    case Lead:        s = square (phase, inc); break;
                    default:          s = tri (phase); break;
                }

                phase += inc;  if (phase >= 1.0) phase -= 1.0;
                phase2 += inc2; if (phase2 >= 1.0) phase2 -= 1.0;

                const float a = 1.0f - std::exp (-6.2831853f * lpCutoff / (float) sr);
                lp += a * (s - lp);
                s = lp;

                if (drive > 0.0f)
                    s = std::tanh (s * drive) * 0.7f;

                return s;
            }

            float drumSample (double sr)
            {
                const float dt = (float) (1.0 / sr);
                t += dt;
                float s = 0.0f;

                auto hp = [this] (float x) { hpState += 0.35f * (x - hpState); return x - hpState; };

                switch (drumType)
                {
                    case 0: // kick
                    {
                        const double f = 48.0 + 110.0 * std::exp (-t * 30.0);
                        phase += f / sr; if (phase >= 1.0) phase -= 1.0;
                        s = 1.4f * (float) std::sin (twoPi * phase) * std::exp (-t * 9.0f);
                        s = std::tanh (s * 1.5f);
                        break;
                    }
                    case 1: // snare
                    {
                        phase += 185.0 / sr; if (phase >= 1.0) phase -= 1.0;
                        s = 0.7f * white() * std::exp (-t * 16.0f) + 0.6f * (float) std::sin (twoPi * phase) * std::exp (-t * 30.0f);
                        break;
                    }
                    case 2: s = 0.5f * hp (white()) * std::exp (-t * 45.0f); break;  // closed hat
                    case 3: s = 0.45f * hp (white()) * std::exp (-t * 7.0f); break;  // open hat
                    case 4: s = 0.4f * hp (white()) * std::exp (-t * 2.6f); break;   // crash
                    case 5: // tom
                    {
                        const double f = freq * (1.0 + 0.6 * std::exp (-t * 20.0));
                        phase += f / sr; if (phase >= 1.0) phase -= 1.0;
                        s = 0.9f * (float) std::sin (twoPi * phase) * std::exp (-t * 7.0f);
                        break;
                    }
                    default: s = 0.4f * white() * std::exp (-t * 25.0f); break;     // clap / percussion
                }

                if (t > 3.0f)
                    active = false;
                return s;
            }
        };

        static Kind kindFor (int program)
        {
            if (program < 8)  return Piano;
            if (program < 16) return Mallet;
            if (program < 24) return Organ;
            if (program < 29) return CleanGuitar;
            if (program < 32) return DistGuitar;
            if (program < 40) return Bass;
            if (program < 56) return Strings;
            if (program < 64) return Brass;
            if (program < 80) return Reed;
            if (program < 88) return Lead;
            if (program < 96) return Pad;
            return Other;
        }

        void noteOn (int ch, int note, float vel)
        {
            Voice* v = nullptr;
            for (auto& cand : voices)
                if (! cand.active) { v = &cand; break; }
            if (v == nullptr)
            {
                v = &voices[0];
                for (auto& cand : voices)
                    if (cand.age < v->age) v = &cand;
            }

            *v = Voice();
            v->active = true;
            v->ch = ch;
            v->note = note;
            v->vel = vel;
            v->age = ++ageCounter;
            v->noise = 22222u + (uint32_t) note * 7919u + ageCounter;
            v->freq = 440.0 * std::pow (2.0, (note - 69) / 12.0);

            if (ch == 9)
            {
                v->drum = true;
                v->attack = 0.0005f; v->decay = 10.0f; v->sustain = 1.0f; v->release = 0.05f;
                switch (note)
                {
                    case 35: case 36: v->drumType = 0; break;
                    case 38: case 40: v->drumType = 1; break;
                    case 42: case 44: v->drumType = 2; break;
                    case 46:          v->drumType = 3; break;
                    case 49: case 51: case 52: case 55: case 57: case 59: v->drumType = 4; break;
                    case 41: case 43: case 45: case 47: case 48: case 50:
                        v->drumType = 5;
                        v->freq = 70.0 + (note - 41) * 14.0;
                        break;
                    default: v->drumType = 6; break;
                }
                return;
            }

            v->kind = kindFor (channels[(size_t) ch].program);
            switch (v->kind)
            {
                case Piano:       v->attack = 0.002f; v->decay = 0.9f;  v->sustain = 0.0f;  v->release = 0.2f;  v->lpCutoff = 3500; break;
                case Mallet:      v->attack = 0.001f; v->decay = 0.5f;  v->sustain = 0.0f;  v->release = 0.2f;  v->lpCutoff = 6000; break;
                case Organ:       v->attack = 0.01f;  v->decay = 0.1f;  v->sustain = 0.9f;  v->release = 0.08f; v->lpCutoff = 4000; break;
                case CleanGuitar: v->attack = 0.002f; v->decay = 0.7f;  v->sustain = 0.15f; v->release = 0.15f; v->lpCutoff = 2500; break;
                case DistGuitar:  v->attack = 0.003f; v->decay = 0.6f;  v->sustain = 0.75f; v->release = 0.08f; v->lpCutoff = 2800;
                                  v->drive = 5.0f; v->detune = 1.0045; break;
                case Bass:        v->attack = 0.003f; v->decay = 0.4f;  v->sustain = 0.6f;  v->release = 0.06f; v->lpCutoff = 1100; break;
                case Strings:     v->attack = 0.12f;  v->decay = 0.5f;  v->sustain = 0.8f;  v->release = 0.35f; v->lpCutoff = 3000; v->detune = 1.006; break;
                case Brass:       v->attack = 0.03f;  v->decay = 0.3f;  v->sustain = 0.75f; v->release = 0.12f; v->lpCutoff = 2400; break;
                case Reed:        v->attack = 0.02f;  v->decay = 0.3f;  v->sustain = 0.8f;  v->release = 0.1f;  v->lpCutoff = 3000; break;
                case Lead:        v->attack = 0.005f; v->decay = 0.3f;  v->sustain = 0.7f;  v->release = 0.1f;  v->lpCutoff = 3500; break;
                case Pad:         v->attack = 0.4f;   v->decay = 1.0f;  v->sustain = 0.8f;  v->release = 0.8f;  v->lpCutoff = 2000; v->detune = 1.008; break;
                default:          v->attack = 0.005f; v->decay = 0.4f;  v->sustain = 0.5f;  v->release = 0.15f; v->lpCutoff = 3000; break;
            }
        }

        std::array<Voice, 48> voices {};
        std::array<Channel, 16> channels {};
        uint32_t ageCounter = 0;
    };

    //==========================================================================
    struct Song
    {
        struct Event { double time; uint8_t bytes[3]; };
        std::vector<Event> events;
        double length = 0.0;
    };

    struct SfxChannel
    {
        const unsigned char* data = nullptr;
        int length = 0, rate = 11025;
        double pos = 0.0;
        float gainL = 0.0f, gainR = 0.0f;
        bool active = false;
    };

    struct Mixer
    {
        juce::SpinLock lock;
        std::array<SfxChannel, 32> sfx {};

        std::vector<std::unique_ptr<Song>> songs; // never shrunk: the audio thread may hold one
        const Song* song = nullptr;
        size_t nextEvent = 0;
        double songTime = 0.0;
        bool playing = false, looping = false, paused = false;
        float musicVolume = 1.0f;
        MiniSynth synth;

        std::atomic<const void*> owner { nullptr };
        std::atomic<bool> gameRunning { false };
        bool wasRunning = false;
    };

    Mixer& mixer()
    {
        static Mixer* m = new Mixer(); // leaked like the rest of DOOM's globals
        return *m;
    }

    void sepToGains (int vol, int sep, float& l, float& r)
    {
        vol = juce::jlimit (0, 127, vol);
        sep = juce::jlimit (0, 254, sep);
        l = (float) ((254 - sep) * vol) / (127.0f * 254.0f) * 1.4f;
        r = (float) (sep * vol) / (127.0f * 254.0f) * 1.4f;
    }
}

//==============================================================================
extern "C"
{
    void dg_audio_sfx_start (int channel, const unsigned char* pcm, int length, int rate, int vol, int sep)
    {
        auto& m = mixer();
        if (channel < 0 || channel >= (int) m.sfx.size())
            return;
        if (m.owner.load() == nullptr)
            return; // nobody is listening; let DOOM see the channel as free
        const juce::SpinLock::ScopedLockType sl (m.lock);
        auto& c = m.sfx[(size_t) channel];
        c.data = pcm;
        c.length = length;
        c.rate = rate;
        c.pos = 0.0;
        sepToGains (vol, sep, c.gainL, c.gainR);
        c.active = true;
    }

    void dg_audio_sfx_update (int channel, int vol, int sep)
    {
        auto& m = mixer();
        if (channel < 0 || channel >= (int) m.sfx.size())
            return;
        const juce::SpinLock::ScopedLockType sl (m.lock);
        auto& c = m.sfx[(size_t) channel];
        sepToGains (vol, sep, c.gainL, c.gainR);
    }

    void dg_audio_sfx_stop (int channel)
    {
        auto& m = mixer();
        if (channel < 0 || channel >= (int) m.sfx.size())
            return;
        const juce::SpinLock::ScopedLockType sl (m.lock);
        m.sfx[(size_t) channel].active = false;
    }

    int dg_audio_sfx_playing (int channel)
    {
        auto& m = mixer();
        if (channel < 0 || channel >= (int) m.sfx.size())
            return 0;
        const juce::SpinLock::ScopedLockType sl (m.lock);
        return m.sfx[(size_t) channel].active ? 1 : 0;
    }

    int dg_audio_music_register (const void* midi, int length)
    {
        juce::MemoryInputStream in (midi, (size_t) length, false);
        juce::MidiFile file;
        if (! file.readFrom (in))
            return 0;
        file.convertTimestampTicksToSeconds();

        juce::MidiMessageSequence all;
        for (int t = 0; t < file.getNumTracks(); ++t)
            all.addSequence (*file.getTrack (t), 0.0);
        all.sort();

        auto song = std::make_unique<Song>();
        for (auto* e : all)
        {
            const auto& msg = e->message;
            if (msg.isMetaEvent() || msg.isSysEx() || msg.getRawDataSize() < 2 || msg.getRawDataSize() > 3)
                continue;
            Song::Event ev {};
            ev.time = msg.getTimeStamp();
            std::memcpy (ev.bytes, msg.getRawData(), (size_t) msg.getRawDataSize());
            song->events.push_back (ev);
        }
        song->length = all.getEndTime();

        auto& m = mixer();
        const juce::SpinLock::ScopedLockType sl (m.lock);
        m.songs.push_back (std::move (song));
        return (int) m.songs.size();
    }

    void dg_audio_music_play (int handle, int looping)
    {
        auto& m = mixer();
        const juce::SpinLock::ScopedLockType sl (m.lock);
        if (handle <= 0 || handle > (int) m.songs.size())
            return;
        m.synth.reset();
        m.song = m.songs[(size_t) handle - 1].get();
        m.nextEvent = 0;
        m.songTime = 0.0;
        m.looping = looping != 0;
        m.paused = false;
        m.playing = true;
    }

    void dg_audio_music_stop()
    {
        auto& m = mixer();
        const juce::SpinLock::ScopedLockType sl (m.lock);
        m.playing = false;
        m.synth.allNotesOff();
    }

    void dg_audio_music_pause (int paused)
    {
        auto& m = mixer();
        const juce::SpinLock::ScopedLockType sl (m.lock);
        m.paused = paused != 0;
        if (m.paused)
            m.synth.allNotesOff();
    }

    void dg_audio_music_volume (int volume)
    {
        auto& m = mixer();
        const juce::SpinLock::ScopedLockType sl (m.lock);
        m.musicVolume = juce::jlimit (0, 127, volume) / 127.0f;
    }

    int dg_audio_music_playing()
    {
        auto& m = mixer();
        const juce::SpinLock::ScopedLockType sl (m.lock);
        return m.playing ? 1 : 0;
    }
}

//==============================================================================
namespace DoomAudio
{
    bool isOwner (const void* owner)
    {
        return mixer().owner.load() == owner;
    }

    void release (const void* owner)
    {
        auto& m = mixer();
        const void* expected = owner;
        if (m.owner.compare_exchange_strong (expected, nullptr))
        {
            const juce::SpinLock::ScopedLockType sl (m.lock);
            for (auto& c : m.sfx)
                c.active = false;
            m.synth.allNotesOff();
        }
    }

    void setGameRunning (bool running)
    {
        mixer().gameRunning = running;
    }

    bool render (const void* owner, float* left, float* right, int numSamples, double sampleRate)
    {
        auto& m = mixer();

        const void* expected = nullptr;
        if (! m.owner.compare_exchange_strong (expected, owner) && expected != owner)
            return false;

        const juce::SpinLock::ScopedLockType sl (m.lock);

        const bool running = m.gameRunning.load();
        if (! running && m.wasRunning)
            m.synth.allNotesOff(); // game paused (editor closed): let notes ring out, don't hang
        m.wasRunning = running;

        constexpr int chunk = 32;
        for (int start = 0; start < numSamples; start += chunk)
        {
            const int n = juce::jmin (chunk, numSamples - start);

            if (m.playing && ! m.paused && running && m.song != nullptr)
            {
                const auto& ev = m.song->events;
                while (m.nextEvent < ev.size() && ev[m.nextEvent].time <= m.songTime)
                    m.synth.handle (ev[m.nextEvent++].bytes);

                m.songTime += n / sampleRate;

                if (m.nextEvent >= ev.size() && m.songTime >= m.song->length)
                {
                    if (m.looping) { m.songTime = 0.0; m.nextEvent = 0; }
                    else           { m.playing = false; m.synth.allNotesOff(); }
                }
            }

            // Music, scaled by DOOM's own music volume.
            float musL[chunk] = {}, musR[chunk] = {};
            m.synth.render (musL, musR, n, sampleRate);
            for (int i = 0; i < n; ++i)
            {
                left[start + i]  += musL[i] * m.musicVolume;
                right[start + i] += musR[i] * m.musicVolume;
            }
        }

        // Sound effects: linear-interpolated 8-bit PCM, crunch included.
        for (auto& c : m.sfx)
        {
            if (! c.active)
                continue;

            const double step = c.rate / sampleRate;
            for (int i = 0; i < numSamples; ++i)
            {
                const int idx = (int) c.pos;
                if (idx + 1 >= c.length)
                {
                    c.active = false;
                    break;
                }
                const float frac = (float) (c.pos - idx);
                const float a = (c.data[idx] - 128) / 128.0f, b = (c.data[idx + 1] - 128) / 128.0f;
                const float s = a + frac * (b - a);
                left[i] += s * c.gainL;
                right[i] += s * c.gainR;
                c.pos += step;
            }
        }

        return true;
    }
}
