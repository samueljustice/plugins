#include "DoomAudio.h"

#include <array>
#include <cmath>
#include <unordered_map>

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

        /** Fades every voice out over ~5 ms, keeping its current gain, pan and pitch, then resets
            the channels. Used when the song changes: no clicks, and the new song sets up its own
            instruments from event 0. */
        void killAll()
        {
            fadeVoices();
            for (auto& c : channels)
                c = {};
        }

        /** Fades every voice out over ~5 ms but keeps the channels' instruments, volumes and pans,
            for when the song carries on from where it was (the sound is handed over). */
        void fadeVoices()
        {
            for (auto& v : voices)
            {
                if (! v.active || v.killing)
                    continue;
                const auto& c = channels[(size_t) v.ch];
                v.frozenGain = c.volume * c.volume * c.expression * v.vel * 0.34f;
                v.frozenPan = c.pan;
                v.frozenBend = c.bend;
                v.killing = true;
                v.stage = Voice::Release;
                v.release = 0.005f;
            }
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
                const float chanGain = v.killing ? v.frozenGain : c.volume * c.volume * c.expression * v.vel * 0.34f;
                const float pan = v.killing ? v.frozenPan : c.pan;
                const float panL = std::cos (pan * 1.5707963f), panR = std::sin (pan * 1.5707963f);
                const double f = v.freq * std::pow (2.0, (v.killing ? v.frozenBend : c.bend) * 2.0 / 12.0);
                const double inc = f / sr, inc2 = inc * v.detune;
                v.lpA = 1.0f - std::exp (-6.2831853f * v.lpCutoff / (float) sr);

                for (int i = 0; i < n; ++i)
                {
                    const float env = v.nextEnv (sr);
                    if (! v.active)
                        break;

                    float s = v.drum ? v.drumSample (sr) : v.toneSample (inc, inc2);
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

            bool active = false, drum = false, killing = false;
            float frozenGain = 0.0f, frozenPan = 0.5f, frozenBend = 0.0f;
            int ch = 0, note = 0, kind = Other, drumType = 0;
            float vel = 1.0f;
            double freq = 440.0, phase = 0.0, phase2 = 0.0, detune = 1.0;
            float env = 0.0f, attack = 0.005f, decay = 0.3f, sustain = 0.7f, release = 0.1f;
            Stage stage = Attack;
            float lp = 0.0f, lpA = 0.3f, lpCutoff = 4000.0f, drive = 0.0f;
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

            float toneSample (double inc, double inc2)
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

                lp += lpA * (s - lp);
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
        uint32_t generation = 0;
        bool active = false;
        bool fadingOut = false; // ramps to silence over one block, then stops
    };

    constexpr int numSfxChannels = 32;

    /** Sent from the DOOM thread to the audio thread. */
    struct Command
    {
        enum Type { Start, Update, Stop } type = Stop;
        int channel = 0;
        const unsigned char* data = nullptr;
        int length = 0, rate = 0;
        float gainL = 0.0f, gainR = 0.0f;
        uint32_t generation = 0;
    };

    /**
     * No locks between the game and the audio thread: the DOOM thread can be descheduled at
     * any moment, and an audio callback waiting on it would drop out. Sound effects travel
     * through a single-writer/single-reader queue; the music is described by a few atomics
     * the audio thread follows.
     */
    struct Mixer
    {
        // DOOM thread -> audio thread
        static constexpr int queueSize = 512;
        juce::AbstractFifo fifo { queueSize };
        std::array<Command, queueSize> queue {};

        bool push (const Command& c)
        {
            int s1, n1, s2, n2;
            fifo.prepareToWrite (1, s1, n1, s2, n2);
            if (n1 + n2 < 1)
                return false;
            queue[(size_t) (n1 > 0 ? s1 : s2)] = c;
            fifo.finishedWrite (1);
            return true;
        }

        std::array<std::atomic<uint32_t>, numSfxChannels> playingGeneration {}; // 0 = channel free
        uint32_t nextGeneration = 1;                                           // DOOM thread only

        std::atomic<const Song*> wantedSong { nullptr };
        std::atomic<bool> wantedLoop { false }, musicPaused { false }, musicPlaying { false };
        std::atomic<uint32_t> musicSerial { 0 };
        std::atomic<float> musicVolume { 1.0f };
        std::vector<std::unique_ptr<Song>> songs; // DOOM thread only; never shrunk, the audio thread holds pointers

        std::unordered_map<uint64_t, int> songHandles; // DOOM thread only: content hash -> handle

        std::atomic<const void*> owner { nullptr };
        std::atomic<uint32_t> ownerHeartbeat { 0 }; // ms; the owner stamps it on every render
        std::atomic<bool> gameRunning { false };
        std::atomic<bool> flush { false };

        // Audio thread only
        std::array<SfxChannel, numSfxChannels> sfx {};
        const Song* song = nullptr;
        size_t nextEvent = 0;
        double songTime = 0.0;
        uint32_t seenSerial = 0;
        bool wasRunning = false, wasPaused = false;
        MiniSynth synth;
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

    bool validChannel (int channel) { return channel >= 0 && channel < numSfxChannels; }
}

//==============================================================================
// Called by DoomSoundModule.c on the DOOM thread.
extern "C"
{
    void dg_audio_sfx_start (int channel, const unsigned char* pcm, int length, int rate, int vol, int sep)
    {
        auto& m = mixer();
        if (! validChannel (channel))
            return;

        // Nobody is listening: let DOOM see the channel as free.
        if (m.owner.load() == nullptr)
        {
            m.playingGeneration[(size_t) channel] = 0;
            return;
        }

        Command c;
        c.type = Command::Start;
        c.channel = channel;
        c.data = pcm;
        c.length = length;
        c.rate = rate;
        sepToGains (vol, sep, c.gainL, c.gainR);
        c.generation = m.nextGeneration++;
        if (m.nextGeneration == 0)
            m.nextGeneration = 1;

        // Publish before queueing, so the audio thread can't finish the sound before we
        // mark it playing (which would leave the channel looking busy for good).
        auto& slot = m.playingGeneration[(size_t) channel];
        slot.store (c.generation);
        if (! m.push (c) || m.owner.load() == nullptr)
            slot.store (0); // queue full, or the owner let go meanwhile
    }

    void dg_audio_sfx_update (int channel, int vol, int sep)
    {
        auto& m = mixer();
        if (! validChannel (channel) || m.playingGeneration[(size_t) channel].load() == 0)
            return;
        Command c;
        c.type = Command::Update;
        c.channel = channel;
        sepToGains (vol, sep, c.gainL, c.gainR);
        m.push (c);
    }

    void dg_audio_sfx_stop (int channel)
    {
        auto& m = mixer();
        if (! validChannel (channel))
            return;
        m.playingGeneration[(size_t) channel] = 0;
        Command c;
        c.type = Command::Stop;
        c.channel = channel;
        m.push (c);
    }

    int dg_audio_sfx_playing (int channel)
    {
        return validChannel (channel) && mixer().playingGeneration[(size_t) channel].load() != 0 ? 1 : 0;
    }

    int dg_audio_music_register (const void* midi, int length)
    {
        // The attract loop and every level change register the same tracks again. Keep one
        // Song per distinct track (Songs are never freed: the audio thread may be playing one).
        auto& m = mixer();
        uint64_t hash = 1469598103934665603ull ^ (uint64_t) length;
        for (int i = 0; i < length; ++i)
        {
            hash ^= static_cast<const unsigned char*> (midi)[i];
            hash *= 1099511628211ull;
        }
        if (auto it = m.songHandles.find (hash); it != m.songHandles.end())
            return it->second;

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

        m.songs.push_back (std::move (song));
        const int handle = (int) m.songs.size();
        m.songHandles[hash] = handle;
        return handle;
    }

    void dg_audio_music_play (int handle, int looping)
    {
        auto& m = mixer();
        if (handle <= 0 || handle > (int) m.songs.size())
            return;
        m.wantedLoop = looping != 0;
        m.musicPaused = false;
        m.wantedSong = m.songs[(size_t) handle - 1].get();
        m.musicPlaying = true;
        m.musicSerial.fetch_add (1);
    }

    void dg_audio_music_stop()
    {
        auto& m = mixer();
        m.wantedSong = nullptr;
        m.musicPlaying = false;
        m.musicSerial.fetch_add (1);
    }

    void dg_audio_music_pause (int paused)
    {
        mixer().musicPaused = paused != 0;
    }

    void dg_audio_music_volume (int volume)
    {
        mixer().musicVolume = juce::jlimit (0, 127, volume) / 127.0f;
    }

    int dg_audio_music_playing()
    {
        return mixer().musicPlaying.load() ? 1 : 0;
    }
}

//==============================================================================
namespace DoomAudio
{
    bool isOwner (const void* owner)
    {
        return mixer().owner.load() == owner;
    }

    bool isOwnedByAnother (const void* owner)
    {
        const auto* current = mixer().owner.load();
        return current != nullptr && current != owner;
    }

    void init()
    {
        (void) mixer();
    }

    void release (const void* owner)
    {
        auto& m = mixer();
        if (m.owner.load() != owner)
            return; // non-owners call this every block: nothing to do

        // Flag the flush before letting go, so whoever takes over starts clean.
        m.flush = true;
        for (auto& g : m.playingGeneration)
            g = 0;
        m.owner.store (nullptr);
    }

    void setGameRunning (bool running)
    {
        mixer().gameRunning = running;
    }

    bool render (const void* owner, float* left, float* right, int numSamples, double sampleRate)
    {
        auto& m = mixer();

        const auto now = juce::Time::getMillisecondCounter();

        const void* expected = nullptr;
        if (! m.owner.compare_exchange_strong (expected, owner) && expected != owner)
        {
            // An owner that has stopped rendering (track or device switched off without the
            // host deactivating it) gives way after a quarter of a second.
            // Signed: a heartbeat stamped just after we read the clock counts as fresh.
            if ((int32_t) (now - m.ownerHeartbeat.load()) < 250 || ! m.owner.compare_exchange_strong (expected, owner))
                return false;
            m.flush = true;
        }
        m.ownerHeartbeat = now;

        if (m.flush.exchange (false))
        {
            // Drop commands queued for whoever owned it before, and fade out what was sounding.
            m.fifo.finishedRead (m.fifo.getNumReady());
            for (auto& c : m.sfx)
                if (c.active)
                    c.fadingOut = true;
            // Dropped and fading sounds will never finish normally: free their channels for DOOM.
            for (auto& g : m.playingGeneration)
                g = 0;
            m.synth.fadeVoices(); // the song carries on, so keep its instruments
        }

        // Sound-effect commands from the game.
        {
            int s1, n1, s2, n2;
            m.fifo.prepareToRead (m.fifo.getNumReady(), s1, n1, s2, n2);
            auto apply = [&m] (const Command& c)
            {
                auto& ch = m.sfx[(size_t) c.channel];
                switch (c.type)
                {
                    case Command::Start:
                        ch.data = c.data; ch.length = c.length; ch.rate = c.rate; ch.pos = 0.0;
                        ch.gainL = c.gainL; ch.gainR = c.gainR; ch.generation = c.generation; ch.active = true;
                        ch.fadingOut = false;
                        break;
                    case Command::Update: ch.gainL = c.gainL; ch.gainR = c.gainR; break;
                    case Command::Stop:   ch.active = false; break;
                }
            };
            for (int i = 0; i < n1; ++i) apply (m.queue[(size_t) (s1 + i)]);
            for (int i = 0; i < n2; ++i) apply (m.queue[(size_t) (s2 + i)]);
            m.fifo.finishedRead (n1 + n2);
        }

        // Music: follow what the game asked for.
        if (const auto serial = m.musicSerial.load(); serial != m.seenSerial)
        {
            m.seenSerial = serial;
            m.synth.killAll(); // old song fades over 5 ms instead of being cut
            m.song = m.wantedSong.load();
            m.nextEvent = 0;
            m.songTime = 0.0;
        }

        const bool running = m.gameRunning.load();
        const bool paused = m.musicPaused.load();
        if ((! running && m.wasRunning) || (paused && ! m.wasPaused))
            m.synth.allNotesOff(); // let notes ring out rather than hang
        m.wasRunning = running;
        m.wasPaused = paused;

        const float musicVolume = m.musicVolume.load();
        const bool looping = m.wantedLoop.load();

        constexpr int chunk = 32;
        for (int start = 0; start < numSamples; start += chunk)
        {
            const int n = juce::jmin (chunk, numSamples - start);

            if (m.song != nullptr && ! paused && running)
            {
                const auto& ev = m.song->events;
                while (m.nextEvent < ev.size() && ev[m.nextEvent].time <= m.songTime)
                    m.synth.handle (ev[m.nextEvent++].bytes);

                m.songTime += n / sampleRate;

                if (m.nextEvent >= ev.size() && m.songTime >= m.song->length)
                {
                    if (looping)
                    {
                        m.songTime = 0.0;
                        m.nextEvent = 0;
                    }
                    else
                    {
                        m.song = nullptr;
                        m.musicPlaying = false;
                        m.synth.allNotesOff();
                    }
                }
            }

            float musL[chunk] = {}, musR[chunk] = {};
            m.synth.render (musL, musR, n, sampleRate);
            for (int i = 0; i < n; ++i)
            {
                left[start + i]  += musL[i] * musicVolume;
                right[start + i] += musR[i] * musicVolume;
            }
        }

        // Sound effects: linear-interpolated 8-bit PCM, crunch included.
        for (size_t chIndex = 0; chIndex < m.sfx.size(); ++chIndex)
        {
            auto& c = m.sfx[chIndex];
            if (! c.active)
                continue;

            const double step = c.rate / sampleRate;
            const float fadeStep = c.fadingOut ? 1.0f / (float) juce::jmax (1, numSamples) : 0.0f;
            float fade = 1.0f;
            for (int i = 0; i < numSamples; ++i)
            {
                const int idx = (int) c.pos;
                if (idx + 1 >= c.length)
                {
                    c.active = false;
                    // Free the channel for DOOM, unless it has already started something newer on it.
                    uint32_t mine = c.generation;
                    m.playingGeneration[chIndex].compare_exchange_strong (mine, 0);
                    break;
                }
                const float frac = (float) (c.pos - idx);
                const float a = (c.data[idx] - 128) / 128.0f, b = (c.data[idx + 1] - 128) / 128.0f;
                const float s = a + frac * (b - a);
                left[i] += s * c.gainL * fade;
                right[i] += s * c.gainR * fade;
                fade -= fadeStep;
                c.pos += step;
            }
            if (c.fadingOut)
                c.active = c.fadingOut = false;
        }

        return true;
    }
}
