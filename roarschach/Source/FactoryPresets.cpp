#include "PresetManager.h"
#include "Params.h"

#include <algorithm>
#include <array>
#include <string_view>

namespace
{
    // Every factory preset spells out every parameter: its chain order (all 14 modules) and one
    // line per module with each of that module's values, plus IN and OUTPUT. A preset therefore
    // never inherits a stray value from the sound before it and reads as a complete recipe.
    // Percent values are 0..100, as on the knobs.
    namespace pm { [[maybe_unused]] constexpr int shift = 0, snap = 1, mono = 2; }                       // Pitch Mode
    namespace fm { [[maybe_unused]] constexpr int follow = 0, keep = 1; }                                // Formant
    namespace tone { [[maybe_unused]] constexpr int flat = 0, voice = 1, heavy = 2, radio = 3; }         // Clean Tone
    namespace ds { [[maybe_unused]] constexpr int split = 0, wide = 1; }                                 // De-ess Mode
    namespace spd { [[maybe_unused]] constexpr int natural = 0, snappy = 1; }                            // Detail Speed
    namespace oct { [[maybe_unused]] constexpr int down1 = 0, down2 = 1; }                               // Vocoder Octave
    namespace car { [[maybe_unused]] constexpr int sine = 0, triangle = 1, square = 2; }                 // Ring Carrier
    namespace trk { [[maybe_unused]] constexpr int fixed = 0, follow = 1; }                              // Ring Track
    namespace rmode { [[maybe_unused]] constexpr int ring = 0, am = 1; }                                 // Ring Mode
    namespace body { [[maybe_unused]] constexpr int off = 0, box = 1, can = 2; }                         // Ring Body
    namespace sh { [[maybe_unused]] constexpr int up = 0, down = 1, upDown = 2, stereo = 3; }            // Shimmer Mode
    namespace shape { [[maybe_unused]] constexpr int sine = 0, opto = 1, square = 2, throat = 3, rattle = 4; }   // Growl Shape
    namespace chr { [[maybe_unused]] constexpr int clean = 0, warm = 1, grind = 2; }                     // Growl Character
    namespace circ { [[maybe_unused]] constexpr int liquid4 = 0, smooth4 = 1, talky4 = 2, deep6 = 3, deep12 = 4, dark5 = 5; }   // Swirl Circuit
    namespace mot { [[maybe_unused]] constexpr int lfo = 0, lfoLevel = 1, random = 2; }                 // Swirl Motion
    namespace vc { [[maybe_unused]] constexpr int line = 0, mic = 1; }                                   // Roar Voicing
    namespace voices { [[maybe_unused]] constexpr int three = 0, six = 1; }                              // Ensemble Voices
    namespace emot { [[maybe_unused]] constexpr int slow = 0, medium = 1, fast = 2, vibrato = 3; }       // Ensemble Motion
    namespace reg { [[maybe_unused]] constexpr int follow = 0, tight = 1, deep = 2; }                    // Sub Register
    namespace sc { [[maybe_unused]] constexpr int clean = 0, resonant = 1, thick = 2; }                  // Sub Character
    namespace pat { [[maybe_unused]] constexpr int chest = 0, throat = 1, beast = 2, speaker = 3, broad = 4, tracked = 5; }   // Overtone Pattern
    namespace om { [[maybe_unused]] constexpr int add = 0, lean = 1, replace = 2; }                      // Overtone Mode

    constexpr bool on = true, off = false;

    // The reference order.
    constexpr const char* c_reference = "PI CL DS DT VO RM SH GR SW RO EN TA SU OV";

    class Sound
    {
    public:
        Sound()
        {
            for (size_t i = 0; i < m_values.size(); ++i)
            {
                m_values[i] = Params::all[i].defaultValue;
                m_set[i] = false;
            }
        }

        /** All 14 two-letter codes, space separated, first to last. */
        Sound& order(const char* codes)
        {
            std::string_view s(codes);
            int slot = 0;
            std::array<bool, Params::numModules> seen {};
            while (! s.empty())
            {
                while (! s.empty() && s.front() == ' ')
                    s.remove_prefix(1);
                if (s.size() < 2)
                    break;
                const int m = Params::moduleIndexForCode(s.substr(0, 2));
                jassert(m >= 0 && ! seen[static_cast<size_t>(m)]);
                if (m >= 0 && ! seen[static_cast<size_t>(m)])
                {
                    seen[static_cast<size_t>(m)] = true;
                    set(Params::modules[static_cast<size_t>(m)].posId, static_cast<float>(slot++));
                }
                s.remove_prefix(2);
            }
            jassert(slot == Params::numModules); // every preset names every module
            return *this;
        }

        Sound& in(float trimDb, bool autoLevel)
        {
            return set(Params::inTrim, trimDb).set(Params::inAuto, autoLevel);
        }

        Sound& pi(bool isOn, float semitones, float size, float drive, int mode, int formant)
        {
            return set(Params::pitchOn, isOn).set(Params::pitch, semitones).set(Params::pitchSize, size)
                  .set(Params::pitchDrive, drive).set(Params::pitchMode, mode).set(Params::pitchFormant, formant);
        }

        Sound& cl(bool isOn, float contain, float gate, int tone)
        {
            return set(Params::cleanOn, isOn).set(Params::cleanContain, contain).set(Params::cleanGate, gate)
                  .set(Params::cleanTone, tone);
        }

        Sound& ds(bool isOn, float amount, int mode)
        {
            return set(Params::deessOn, isOn).set(Params::deess, amount).set(Params::deessMode, mode);
        }

        Sound& dt(bool isOn, float low, float high, int speed)
        {
            return set(Params::detailOn, isOn).set(Params::detailLow, low).set(Params::detailHigh, high)
                  .set(Params::detailSpeed, speed);
        }

        Sound& vo(bool isOn, float gravel, int octave)
        {
            return set(Params::vocoderOn, isOn).set(Params::gravel, gravel).set(Params::vocoderOctave, octave);
        }

        Sound& rm(bool isOn, float amount, float freqHz, float ratio, int carrier, int track, int mode, int bodyType)
        {
            return set(Params::ringOn, isOn).set(Params::ring, amount).set(Params::ringFreq, freqHz).set(Params::ringRatio, ratio)
                  .set(Params::ringCarrier, carrier).set(Params::ringTrack, track).set(Params::ringMode, mode)
                  .set(Params::ringBody, bodyType);
        }

        Sound& sh(bool isOn, float amount, float rateHz, int mode)
        {
            return set(Params::shimmerOn, isOn).set(Params::shimmer, amount).set(Params::shimmerRate, rateHz)
                  .set(Params::shimmerMode, mode);
        }

        Sound& gr(bool isOn, float depth, float rateHz, int shape, int character)
        {
            return set(Params::growlOn, isOn).set(Params::growl, depth).set(Params::growlRate, rateHz)
                  .set(Params::growlShape, shape).set(Params::growlCharacter, character);
        }

        Sound& sw(bool isOn, float amount, float rateHz, int circuit, int motion)
        {
            return set(Params::swirlOn, isOn).set(Params::swirl, amount).set(Params::swirlRate, rateHz)
                  .set(Params::swirlCircuit, circuit).set(Params::swirlMotion, motion);
        }

        Sound& ro(bool isOn, float drive, float tone, int voicing)
        {
            return set(Params::roarOn, isOn).set(Params::roar, drive).set(Params::roarTone, tone).set(Params::roarVoicing, voicing);
        }

        Sound& en(bool isOn, float amount, float width, int numVoices, int motion)
        {
            return set(Params::ensembleOn, isOn).set(Params::ensemble, amount).set(Params::ensembleWidth, width)
                  .set(Params::ensembleVoices, numVoices).set(Params::ensembleMotion, motion);
        }

        Sound& ta(bool isOn, float amount)
        {
            return set(Params::tameOn, isOn).set(Params::tame, amount);
        }

        Sound& su(bool isOn, float level, float drive, int reg, int character)
        {
            return set(Params::subOn, isOn).set(Params::sub, level).set(Params::subDrive, drive)
                  .set(Params::subRegister, reg).set(Params::subCharacter, character);
        }

        Sound& ov(bool isOn, float amount, float freqOct, int pattern, int mode)
        {
            return set(Params::overtoneOn, isOn).set(Params::overtone, amount).set(Params::overtoneFreq, freqOct)
                  .set(Params::overtonePattern, pattern).set(Params::overtoneMode, mode);
        }

        /** Push, Ceiling, Clip (dB, 0 = Off), Clip Shape (0 Soft .. 100 Hard), Attack (ms), Release (ms), Auto, Mix. */
        Sound& out(float pushDb, float ceilingDb, float clipDb, float clipShape, float attackMs, float releaseMs,
                   bool autoRelease, float mixPercent)
        {
            return set(Params::outPush, pushDb).set(Params::outCeiling, ceilingDb).set(Params::outClip, clipDb)
                  .set(Params::outClipShape, clipShape).set(Params::outAttack, attackMs).set(Params::outRelease, releaseMs)
                  .set(Params::outAuto, autoRelease).set(Params::mix, mixPercent);
        }

        /** Every parameter, by ID (the table PresetManager applies). */
        std::map<juce::String, float> values() const
        {
            jassert(std::all_of(m_set.begin(), m_set.end(), [](bool s) { return s; })); // a preset that forgot a module line
            std::map<juce::String, float> v;
            for (size_t i = 0; i < m_values.size(); ++i)
                v[Params::all[i].id] = m_values[i];
            return v;
        }

    private:
        Sound& set(const char* id, float value)
        {
            const int i = Params::indexOf(id);
            jassert(i >= 0);
            if (i >= 0)
            {
                m_values[static_cast<size_t>(i)] = value;
                m_set[static_cast<size_t>(i)] = true;
            }
            return *this;
        }

        Sound& set(const char* id, bool value) { return set(id, value ? 1.0f : 0.0f); }
        Sound& set(const char* id, int value) { return set(id, static_cast<float>(value)); }

        std::array<float, Params::numParameters> m_values {};
        std::array<bool, Params::numParameters> m_set {};
    };

    PresetManager::FactoryPreset preset(const char* name, const char* category, const Sound& sound)
    {
        return { name, category, sound.values() };
    }
}

const std::vector<PresetManager::FactoryPreset>& PresetManager::getFactoryPresets()
{
    static const std::vector<FactoryPreset> presets
    {
        // The reference chain at its subtle settings: a slightly lower, bigger voice with a liquid
        // wobble, a fine 27 Hz chop, warm tube edge, near-mono width, a solid octave underneath and
        // a touch of overtone weight.
        preset("Init", "", Sound()
            .order(c_reference)
            .in(0.0f, on)
            .pi(on, -2.0f, 0.0f, 5.0f, pm::shift, fm::follow)
            .cl(on, 35.0f, 35.0f, tone::voice)
            .ds(on, 30.0f, ds::split)
            .dt(on, 30.0f, 25.0f, spd::natural)
            .vo(off, 50.0f, oct::down1)
            .rm(off, 40.0f, 150.0f, 1.0f, car::sine, trk::fixed, rmode::ring, body::off)
            .sh(on, 12.0f, 11.0f, sh::up)
            .gr(on, 48.0f, 27.0f, shape::sine, chr::warm)
            .sw(on, 30.0f, 0.35f, circ::liquid4, mot::lfoLevel)
            .ro(on, 45.0f, 10.0f, vc::line)
            .en(on, 18.0f, 12.0f, voices::three, emot::medium)
            .ta(on, 30.0f)
            .su(on, 45.0f, 60.0f, reg::follow, sc::clean)
            .ov(on, 10.0f, 0.0f, pat::chest, om::add)
            .out(8.0f, -1.0f, 0.0f, 0.0f, 2.0f, 60.0f, on, 100.0f)),

        // CREATURES -------------------------------------------------------------------------------
        // A liquid alien monster, built the classic way: a voice pitched a little lower, contained
        // and de-essed, then many subtle layers - a frequency-shift wobble, a fine 27 Hz chop, a
        // phaser, warm tube drive on lows and highs, a near-mono chorus that makes the low end
        // liquid, top-band taming and a tight octave underneath, kept intelligible by the limiter.
        preset("Symbiote", "Creatures", Sound()
            .order("PI CL DS SH GR SW RO EN TA SU DT VO RM OV")
            .in(0.0f, on)
            .pi(on, -2.0f, 0.0f, 20.0f, pm::shift, fm::follow)
            .cl(on, 40.0f, 45.0f, tone::voice)
            .ds(on, 40.0f, ds::split)
            .dt(off, 30.0f, 25.0f, spd::natural)
            .vo(off, 50.0f, oct::down1)
            .rm(off, 40.0f, 150.0f, 1.0f, car::sine, trk::fixed, rmode::ring, body::off)
            .sh(on, 25.0f, 11.0f, sh::upDown)
            .gr(on, 55.0f, 27.0f, shape::sine, chr::warm)
            .sw(on, 32.0f, 0.35f, circ::liquid4, mot::lfoLevel)
            .ro(on, 50.0f, 0.0f, vc::line)
            .en(on, 30.0f, 15.0f, voices::six, emot::medium)
            .ta(on, 35.0f)
            .su(on, 55.0f, 40.0f, reg::tight, sc::clean)
            .ov(off, 10.0f, 0.0f, pat::chest, om::add)
            .out(8.0f, -1.0f, 0.0f, 0.0f, 2.0f, 60.0f, on, 100.0f)),

        // A rolling, wet beast with a wide liquid body.
        preset("Liquid Beast", "Creatures", Sound()
            .order(c_reference)
            .in(0.0f, on)
            .pi(on, -4.0f, 25.0f, 25.0f, pm::shift, fm::follow)
            .cl(on, 35.0f, 35.0f, tone::voice)
            .ds(on, 30.0f, ds::split)
            .dt(on, 30.0f, 25.0f, spd::natural)
            .vo(off, 50.0f, oct::down1)
            .rm(off, 40.0f, 150.0f, 1.0f, car::sine, trk::fixed, rmode::ring, body::off)
            .sh(on, 40.0f, 6.0f, sh::up)
            .gr(on, 40.0f, 27.0f, shape::sine, chr::warm)
            .sw(on, 50.0f, 0.25f, circ::deep6, mot::lfoLevel)
            .ro(on, 40.0f, 0.0f, vc::line)
            .en(on, 60.0f, 70.0f, voices::six, emot::slow)
            .ta(on, 30.0f)
            .su(on, 55.0f, 50.0f, reg::follow, sc::clean)
            .ov(on, 10.0f, 0.0f, pat::chest, om::add)
            .out(7.0f, -1.0f, 0.0f, 0.0f, 2.0f, 60.0f, on, 100.0f)),

        // A rasping throat with vocoder gravel under a strong chop, and gruff upper overtones.
        preset("Gravel Throat", "Creatures", Sound()
            .order(c_reference)
            .in(0.0f, on)
            .pi(on, -3.0f, 0.0f, 45.0f, pm::shift, fm::follow)
            .cl(on, 35.0f, 35.0f, tone::voice)
            .ds(on, 30.0f, ds::split)
            .dt(on, 30.0f, 25.0f, spd::natural)
            .vo(on, 45.0f, oct::down1)
            .rm(off, 40.0f, 150.0f, 1.0f, car::sine, trk::fixed, rmode::ring, body::off)
            .sh(on, 12.0f, 11.0f, sh::up)
            .gr(on, 55.0f, 32.0f, shape::opto, chr::grind)
            .sw(on, 30.0f, 0.35f, circ::liquid4, mot::lfoLevel)
            .ro(on, 50.0f, 0.0f, vc::line)
            .en(on, 18.0f, 12.0f, voices::three, emot::medium)
            .ta(on, 30.0f)
            .su(on, 50.0f, 50.0f, reg::follow, sc::clean)
            .ov(on, 35.0f, 0.0f, pat::throat, om::add)
            .out(7.0f, -1.0f, 0.0f, 0.0f, 2.0f, 60.0f, on, 100.0f)),

        // Slow and huge, with a low body, a deep octave, a heavy slow chop and a hollow low snarl.
        preset("Cave Troll", "Creatures", Sound()
            .order(c_reference)
            .in(0.0f, on)
            .pi(on, -7.0f, 40.0f, 25.0f, pm::shift, fm::follow)
            .cl(on, 35.0f, 35.0f, tone::voice)
            .ds(on, 30.0f, ds::split)
            .dt(on, 30.0f, 25.0f, spd::natural)
            .vo(off, 50.0f, oct::down1)
            .rm(off, 40.0f, 150.0f, 1.0f, car::sine, trk::fixed, rmode::ring, body::off)
            .sh(on, 15.0f, 12.0f, sh::up)
            .gr(on, 50.0f, 18.0f, shape::opto, chr::warm)
            .sw(on, 30.0f, 0.35f, circ::liquid4, mot::lfoLevel)
            .ro(on, 45.0f, -30.0f, vc::line)
            .en(on, 30.0f, 10.0f, voices::three, emot::medium)
            .ta(on, 30.0f)
            .su(on, 70.0f, 50.0f, reg::deep, sc::clean)
            .ov(on, 40.0f, 0.0f, pat::beast, om::add)
            .out(7.0f, -1.0f, 0.0f, 0.0f, 2.0f, 60.0f, on, 100.0f)),

        // Building-sized, with a two-octave vocoder bed, hard containment and a dense output.
        preset("Kaiju", "Creatures", Sound()
            .order(c_reference)
            .in(0.0f, on)
            .pi(on, -9.0f, 60.0f, 50.0f, pm::shift, fm::follow)
            .cl(on, 35.0f, 35.0f, tone::voice)
            .ds(on, 30.0f, ds::split)
            .dt(on, 30.0f, 25.0f, spd::natural)
            .vo(on, 40.0f, oct::down2)
            .rm(off, 40.0f, 150.0f, 1.0f, car::sine, trk::fixed, rmode::ring, body::off)
            .sh(on, 12.0f, 11.0f, sh::up)
            .gr(on, 60.0f, 27.0f, shape::rattle, chr::warm)
            .sw(on, 30.0f, 0.35f, circ::liquid4, mot::lfoLevel)
            .ro(on, 65.0f, -20.0f, vc::line)
            .en(on, 18.0f, 12.0f, voices::three, emot::medium)
            .ta(on, 70.0f)
            .su(on, 80.0f, 65.0f, reg::follow, sc::resonant)
            .ov(on, 40.0f, 0.0f, pat::beast, om::add)
            .out(7.0f, -1.0f, 4.0f, 0.0f, 2.0f, 60.0f, on, 100.0f)),

        // Stony and bright, with a talking phaser and a square-edged chop.
        preset("Gargoyle", "Creatures", Sound()
            .order(c_reference)
            .in(0.0f, on)
            .pi(on, -5.0f, 30.0f, 25.0f, pm::shift, fm::follow)
            .cl(on, 35.0f, 35.0f, tone::voice)
            .ds(on, 30.0f, ds::split)
            .dt(on, 30.0f, 25.0f, spd::natural)
            .vo(off, 50.0f, oct::down1)
            .rm(off, 40.0f, 150.0f, 1.0f, car::sine, trk::fixed, rmode::ring, body::off)
            .sh(on, 12.0f, 11.0f, sh::up)
            .gr(on, 35.0f, 22.0f, shape::square, chr::warm)
            .sw(on, 50.0f, 0.35f, circ::talky4, mot::lfoLevel)
            .ro(on, 60.0f, 40.0f, vc::mic)
            .en(on, 18.0f, 12.0f, voices::three, emot::medium)
            .ta(on, 30.0f)
            .su(on, 50.0f, 50.0f, reg::follow, sc::clean)
            .ov(on, 10.0f, 0.0f, pat::chest, om::add)
            .out(7.5f, -1.0f, 0.0f, 0.0f, 2.0f, 60.0f, on, 100.0f)),

        // Snarling, with a fast rattle that speeds up when it barks, crunched at the output.
        preset("Hellhound", "Creatures", Sound()
            .order(c_reference)
            .in(0.0f, on)
            .pi(on, -4.0f, 0.0f, 60.0f, pm::shift, fm::follow)
            .cl(on, 35.0f, 35.0f, tone::voice)
            .ds(on, 30.0f, ds::split)
            .dt(on, 30.0f, 25.0f, spd::natural)
            .vo(off, 50.0f, oct::down1)
            .rm(off, 40.0f, 150.0f, 1.0f, car::sine, trk::fixed, rmode::ring, body::off)
            .sh(on, 12.0f, 11.0f, sh::up)
            .gr(on, 70.0f, 35.0f, shape::rattle, chr::grind)
            .sw(on, 30.0f, 0.35f, circ::liquid4, mot::lfoLevel)
            .ro(on, 70.0f, 20.0f, vc::line)
            .en(on, 18.0f, 12.0f, voices::three, emot::medium)
            .ta(on, 30.0f)
            .su(on, 45.0f, 70.0f, reg::follow, sc::clean)
            .ov(on, 35.0f, 0.0f, pat::throat, om::add)
            .out(8.0f, -2.0f, 3.0f, 60.0f, 0.5f, 3.0f, off, 100.0f)),

        // Mechanical and heavy, with stepped pitch, a slow square chop and big detail.
        preset("Golem", "Creatures", Sound()
            .order(c_reference)
            .in(0.0f, on)
            .pi(on, -8.0f, 30.0f, 25.0f, pm::snap, fm::follow)
            .cl(on, 35.0f, 35.0f, tone::voice)
            .ds(on, 30.0f, ds::split)
            .dt(on, 60.0f, 35.0f, spd::natural)
            .vo(off, 50.0f, oct::down1)
            .rm(off, 40.0f, 150.0f, 1.0f, car::sine, trk::fixed, rmode::ring, body::off)
            .sh(on, 12.0f, 11.0f, sh::up)
            .gr(on, 30.0f, 12.0f, shape::square, chr::clean)
            .sw(on, 30.0f, 0.35f, circ::liquid4, mot::lfoLevel)
            .ro(on, 40.0f, 0.0f, vc::line)
            .en(on, 18.0f, 12.0f, voices::three, emot::medium)
            .ta(on, 30.0f)
            .su(on, 60.0f, 50.0f, reg::tight, sc::clean)
            .ov(on, 10.0f, 0.0f, pat::chest, om::add)
            .out(6.5f, -1.0f, 0.0f, 0.0f, 2.0f, 60.0f, on, 100.0f)),

        // Underwater and swirling, with deep motion everywhere.
        preset("Kraken", "Creatures", Sound()
            .order(c_reference)
            .in(0.0f, on)
            .pi(on, -6.0f, 0.0f, 25.0f, pm::shift, fm::follow)
            .cl(on, 35.0f, 35.0f, tone::voice)
            .ds(on, 30.0f, ds::split)
            .dt(on, 30.0f, 25.0f, spd::natural)
            .vo(off, 50.0f, oct::down1)
            .rm(off, 40.0f, 150.0f, 1.0f, car::sine, trk::fixed, rmode::ring, body::off)
            .sh(on, 60.0f, 3.0f, sh::upDown)
            .gr(on, 20.0f, 27.0f, shape::sine, chr::warm)
            .sw(on, 55.0f, 0.15f, circ::deep12, mot::random)
            .ro(on, 45.0f, 10.0f, vc::line)
            .en(on, 70.0f, 90.0f, voices::six, emot::slow)
            .ta(on, 30.0f)
            .su(on, 50.0f, 50.0f, reg::follow, sc::clean)
            .ov(on, 10.0f, 0.0f, pat::chest, om::add)
            .out(7.5f, -1.0f, 0.0f, 0.0f, 2.0f, 60.0f, on, 100.0f)),

        // An undertone growl heard as a second, lower pitch: the octave plus tracked overtones at
        // 1.5 and 2.5 times the voice give a period-doubled, two-pitch throat.
        preset("Throat Singer", "Creatures", Sound()
            .order(c_reference)
            .in(0.0f, on)
            .pi(on, -4.0f, 0.0f, 25.0f, pm::shift, fm::follow)
            .cl(on, 35.0f, 35.0f, tone::voice)
            .ds(on, 30.0f, ds::split)
            .dt(on, 30.0f, 25.0f, spd::natural)
            .vo(off, 50.0f, oct::down1)
            .rm(off, 40.0f, 150.0f, 1.0f, car::sine, trk::fixed, rmode::ring, body::off)
            .sh(on, 12.0f, 11.0f, sh::up)
            .gr(on, 40.0f, 27.0f, shape::throat, chr::warm)
            .sw(on, 30.0f, 0.35f, circ::liquid4, mot::lfoLevel)
            .ro(on, 45.0f, 10.0f, vc::line)
            .en(on, 18.0f, 12.0f, voices::three, emot::medium)
            .ta(on, 30.0f)
            .su(on, 60.0f, 50.0f, reg::tight, sc::clean)
            .ov(on, 55.0f, 0.0f, pat::tracked, om::add)
            .out(7.0f, -1.0f, 0.0f, 0.0f, 2.0f, 60.0f, on, 100.0f)),

        // VOICES (intelligibility first) ----------------------------------------------------------
        // A huge but clear storyteller.
        preset("Big Bad Narrator", "Voices", Sound()
            .order(c_reference)
            .in(0.0f, on)
            .pi(on, -3.0f, 0.0f, 30.0f, pm::shift, fm::follow)
            .cl(on, 35.0f, 35.0f, tone::voice)
            .ds(on, 30.0f, ds::split)
            .dt(on, 70.0f, 40.0f, spd::natural)
            .vo(off, 50.0f, oct::down1)
            .rm(off, 40.0f, 150.0f, 1.0f, car::sine, trk::fixed, rmode::ring, body::off)
            .sh(on, 12.0f, 11.0f, sh::up)
            .gr(on, 15.0f, 27.0f, shape::sine, chr::warm)
            .sw(on, 20.0f, 0.35f, circ::liquid4, mot::lfoLevel)
            .ro(on, 30.0f, 0.0f, vc::line)
            .en(on, 18.0f, 12.0f, voices::three, emot::medium)
            .ta(on, 30.0f)
            .su(on, 40.0f, 50.0f, reg::follow, sc::clean)
            .ov(on, 10.0f, 0.0f, pat::chest, om::add)
            .out(7.5f, -1.0f, 0.0f, 0.0f, 2.0f, 60.0f, on, 100.0f)),

        // Breath and spit lifted into a hissing presence.
        preset("Demon Whisper", "Voices", Sound()
            .order(c_reference)
            .in(0.0f, on)
            .pi(on, -5.0f, 0.0f, 25.0f, pm::shift, fm::follow)
            .cl(on, 50.0f, 15.0f, tone::voice)
            .ds(on, 70.0f, ds::split)
            .dt(on, 60.0f, 40.0f, spd::natural)
            .vo(off, 50.0f, oct::down1)
            .rm(off, 40.0f, 150.0f, 1.0f, car::sine, trk::fixed, rmode::ring, body::off)
            .sh(on, 35.0f, 5.0f, sh::stereo)
            .gr(on, 48.0f, 27.0f, shape::sine, chr::warm)
            .sw(on, 30.0f, 0.35f, circ::liquid4, mot::lfoLevel)
            .ro(on, 45.0f, 40.0f, vc::line)
            .en(on, 35.0f, 60.0f, voices::three, emot::medium)
            .ta(on, 30.0f)
            .su(on, 25.0f, 50.0f, reg::follow, sc::clean)
            .ov(on, 10.0f, 0.0f, pat::chest, om::add)
            .out(8.0f, -1.0f, 0.0f, 0.0f, 2.0f, 60.0f, on, 100.0f)),

        // A plain big human: no chop and no swirl, just size and octave.
        preset("Giant", "Voices", Sound()
            .order(c_reference)
            .in(0.0f, on)
            .pi(on, -6.0f, 35.0f, 25.0f, pm::shift, fm::follow)
            .cl(on, 35.0f, 35.0f, tone::voice)
            .ds(on, 30.0f, ds::split)
            .dt(on, 30.0f, 25.0f, spd::natural)
            .vo(off, 50.0f, oct::down1)
            .rm(off, 40.0f, 150.0f, 1.0f, car::sine, trk::fixed, rmode::ring, body::off)
            .sh(on, 12.0f, 11.0f, sh::up)
            .gr(off, 45.0f, 27.0f, shape::sine, chr::warm)
            .sw(off, 35.0f, 0.35f, circ::liquid4, mot::lfoLevel)
            .ro(on, 30.0f, -30.0f, vc::line)
            .en(on, 25.0f, 0.0f, voices::three, emot::medium)
            .ta(on, 30.0f)
            .su(on, 65.0f, 50.0f, reg::follow, sc::clean)
            .ov(on, 10.0f, 0.0f, pat::chest, om::add)
            .out(6.5f, -1.0f, 0.0f, 0.0f, 2.0f, 60.0f, on, 100.0f)),

        // A monotone drone voice with a throaty subharmonic chop.
        preset("Possessed", "Voices", Sound()
            .order(c_reference)
            .in(0.0f, on)
            .pi(on, -5.0f, 0.0f, 25.0f, pm::mono, fm::follow)
            .cl(on, 35.0f, 35.0f, tone::voice)
            .ds(on, 30.0f, ds::split)
            .dt(on, 30.0f, 25.0f, spd::natural)
            .vo(on, 35.0f, oct::down1)
            .rm(off, 40.0f, 150.0f, 1.0f, car::sine, trk::fixed, rmode::ring, body::off)
            .sh(on, 30.0f, 12.0f, sh::upDown)
            .gr(on, 40.0f, 27.0f, shape::throat, chr::warm)
            .sw(on, 30.0f, 0.35f, circ::liquid4, mot::lfoLevel)
            .ro(on, 40.0f, 0.0f, vc::line)
            .en(on, 18.0f, 12.0f, voices::three, emot::medium)
            .ta(on, 30.0f)
            .su(on, 50.0f, 50.0f, reg::follow, sc::clean)
            .ov(on, 10.0f, 0.0f, pat::chest, om::add)
            .out(7.0f, -1.0f, 0.0f, 0.0f, 2.0f, 60.0f, on, 100.0f)),

        // A band-limited, overdriven creature over a small speaker.
        preset("Ogre Radio", "Voices", Sound()
            .order(c_reference)
            .in(0.0f, on)
            .pi(on, -4.0f, 0.0f, 25.0f, pm::shift, fm::follow)
            .cl(on, 50.0f, 45.0f, tone::radio)
            .ds(on, 30.0f, ds::split)
            .dt(on, 30.0f, 25.0f, spd::natural)
            .vo(off, 50.0f, oct::down1)
            .rm(off, 40.0f, 150.0f, 1.0f, car::sine, trk::fixed, rmode::ring, body::off)
            .sh(on, 12.0f, 11.0f, sh::up)
            .gr(on, 48.0f, 27.0f, shape::sine, chr::warm)
            .sw(on, 30.0f, 0.35f, circ::liquid4, mot::lfoLevel)
            .ro(on, 70.0f, 50.0f, vc::mic)
            .en(off, 35.0f, 15.0f, voices::three, emot::medium)
            .ta(on, 30.0f)
            .su(on, 20.0f, 50.0f, reg::follow, sc::clean)
            .ov(on, 10.0f, 0.0f, pat::chest, om::add)
            .out(8.0f, -1.0f, 0.0f, 0.0f, 2.0f, 60.0f, on, 100.0f)),

        // MACHINES (ring-modulated voices) --------------------------------------------------------
        // A yelled voice pushed through a struggling machine larynx: tone-shaped, ring-modulated in a
        // hard little voice box, then pitched barely lower, with gravel on the peaks.
        preset("Cyber Commander", "Machines", Sound()
            .order("CL RM PI RO DS DT GR TA VO SH SW EN SU OV")
            .in(0.0f, on)
            .pi(on, -1.0f, 0.0f, 40.0f, pm::shift, fm::follow)
            .cl(on, 55.0f, 40.0f, tone::heavy)
            .ds(on, 60.0f, ds::split)
            .dt(on, 65.0f, 35.0f, spd::natural)
            .vo(off, 50.0f, oct::down1)
            .rm(on, 45.0f, 150.0f, 1.0f, car::sine, trk::fixed, rmode::ring, body::box)
            .sh(off, 25.0f, 12.0f, sh::up)
            .gr(on, 20.0f, 27.0f, shape::throat, chr::grind)
            .sw(off, 35.0f, 0.35f, circ::liquid4, mot::lfoLevel)
            .ro(on, 45.0f, 30.0f, vc::mic)
            .en(off, 35.0f, 15.0f, voices::three, emot::medium)
            .ta(on, 55.0f)
            .su(off, 50.0f, 50.0f, reg::follow, sc::clean)
            .ov(off, 30.0f, 0.0f, pat::chest, om::add)
            .out(9.0f, -2.5f, 3.0f, 40.0f, 2.0f, 60.0f, on, 100.0f)),

        // A wet, gargling machine breath: a carrier at half the voice's pitch gurgles in tune, a
        // small metal can resonates, and breaths and wheezes are lifted forward (and ring-modulated).
        preset("Respirator Rasp", "Machines", Sound()
            .order("CL PI DT RM GR RO DS SH TA VO SW EN SU OV")
            .in(0.0f, on)
            .pi(on, -2.0f, 10.0f, 30.0f, pm::shift, fm::follow)
            .cl(on, 50.0f, 20.0f, tone::voice)
            .ds(on, 35.0f, ds::split)
            .dt(on, 85.0f, 40.0f, spd::snappy)
            .vo(off, 50.0f, oct::down1)
            .rm(on, 35.0f, 150.0f, 0.5f, car::sine, trk::follow, rmode::ring, body::can)
            .sh(on, 15.0f, 3.0f, sh::down)
            .gr(on, 35.0f, 22.0f, shape::opto, chr::warm)
            .sw(off, 35.0f, 0.35f, circ::liquid4, mot::lfoLevel)
            .ro(on, 35.0f, 10.0f, vc::line)
            .en(off, 35.0f, 15.0f, voices::three, emot::medium)
            .ta(on, 30.0f)
            .su(off, 50.0f, 50.0f, reg::follow, sc::clean)
            .ov(off, 30.0f, 0.0f, pat::chest, om::add)
            .out(7.5f, -1.0f, 0.0f, 0.0f, 2.0f, 60.0f, on, 100.0f)),

        // A menacing, buzzing tin speaker: a square-wave ring a fifth above the voice, crushed
        // through a narrow radio band and an overdriven tube, clipped hard at the output.
        preset("Rasp Machine", "Machines", Sound()
            .order("PI RM CL RO GR DS DT TA VO SH SW EN SU OV")
            .in(0.0f, on)
            .pi(on, -1.5f, 0.0f, 55.0f, pm::shift, fm::follow)
            .cl(on, 60.0f, 45.0f, tone::radio)
            .ds(on, 55.0f, ds::wide)
            .dt(on, 55.0f, 45.0f, spd::snappy)
            .vo(off, 50.0f, oct::down1)
            .rm(on, 55.0f, 150.0f, 1.5f, car::square, trk::follow, rmode::ring, body::box)
            .sh(off, 25.0f, 12.0f, sh::up)
            .gr(on, 25.0f, 38.0f, shape::rattle, chr::grind)
            .sw(off, 35.0f, 0.35f, circ::liquid4, mot::lfoLevel)
            .ro(on, 65.0f, 40.0f, vc::mic)
            .en(off, 35.0f, 15.0f, voices::three, emot::medium)
            .ta(on, 60.0f)
            .su(off, 50.0f, 50.0f, reg::follow, sc::clean)
            .ov(off, 30.0f, 0.0f, pat::chest, om::add)
            .out(9.0f, -3.0f, 6.0f, 70.0f, 0.5f, 30.0f, off, 100.0f)),

        // Stepped pitch and a square chop: machine-like.
        preset("Robot Ogre", "Machines", Sound()
            .order(c_reference)
            .in(0.0f, on)
            .pi(on, -4.0f, 0.0f, 25.0f, pm::snap, fm::follow)
            .cl(on, 35.0f, 35.0f, tone::voice)
            .ds(on, 30.0f, ds::split)
            .dt(on, 30.0f, 25.0f, spd::natural)
            .vo(on, 80.0f, oct::down1)
            .rm(off, 40.0f, 150.0f, 1.0f, car::sine, trk::fixed, rmode::ring, body::off)
            .sh(on, 12.0f, 11.0f, sh::up)
            .gr(on, 40.0f, 16.0f, shape::square, chr::warm)
            .sw(on, 30.0f, 0.35f, circ::liquid4, mot::lfoLevel)
            .ro(on, 45.0f, 10.0f, vc::line)
            .en(on, 18.0f, 12.0f, voices::three, emot::medium)
            .ta(on, 30.0f)
            .su(on, 50.0f, 50.0f, reg::follow, sc::clean)
            .ov(on, 10.0f, 0.0f, pat::chest, om::add)
            .out(7.0f, -1.0f, 0.0f, 0.0f, 2.0f, 60.0f, on, 100.0f)),

        // SIGNAL FLOW (reordered chains; each shows off a placement) ------------------------------
        // Saturate first, then modulate: the notches and the shimmer cut through the grit instead
        // of being blurred by it.
        preset("Scorched Swirl", "Signal Flow", Sound()
            .order("PI CL DS RO SH GR SW DT VO RM EN TA SU OV")
            .in(0.0f, on)
            .pi(on, -2.0f, 0.0f, 5.0f, pm::shift, fm::follow)
            .cl(on, 35.0f, 35.0f, tone::voice)
            .ds(on, 30.0f, ds::split)
            .dt(on, 30.0f, 25.0f, spd::natural)
            .vo(off, 50.0f, oct::down1)
            .rm(off, 40.0f, 150.0f, 1.0f, car::sine, trk::fixed, rmode::ring, body::off)
            .sh(on, 12.0f, 11.0f, sh::up)
            .gr(on, 48.0f, 27.0f, shape::sine, chr::warm)
            .sw(on, 55.0f, 0.35f, circ::talky4, mot::lfoLevel)
            .ro(on, 50.0f, 0.0f, vc::line)
            .en(on, 18.0f, 12.0f, voices::three, emot::medium)
            .ta(on, 30.0f)
            .su(on, 50.0f, 50.0f, reg::follow, sc::clean)
            .ov(on, 10.0f, 0.0f, pat::chest, om::add)
            .out(7.0f, -1.0f, 0.0f, 0.0f, 2.0f, 60.0f, on, 100.0f)),

        // Chorus into the tube, so each side distorts differently. Wide and smeared.
        preset("Molten Chorus", "Signal Flow", Sound()
            .order("PI CL DS DT VO RM SH GR SW EN RO TA SU OV")
            .in(0.0f, on)
            .pi(on, -2.0f, 0.0f, 5.0f, pm::shift, fm::follow)
            .cl(on, 35.0f, 35.0f, tone::voice)
            .ds(on, 30.0f, ds::split)
            .dt(on, 30.0f, 25.0f, spd::natural)
            .vo(off, 50.0f, oct::down1)
            .rm(off, 40.0f, 150.0f, 1.0f, car::sine, trk::fixed, rmode::ring, body::off)
            .sh(on, 12.0f, 11.0f, sh::up)
            .gr(on, 48.0f, 27.0f, shape::sine, chr::warm)
            .sw(on, 30.0f, 0.35f, circ::liquid4, mot::lfoLevel)
            .ro(on, 55.0f, 0.0f, vc::line)
            .en(on, 55.0f, 80.0f, voices::three, emot::medium)
            .ta(on, 30.0f)
            .su(on, 50.0f, 50.0f, reg::follow, sc::clean)
            .ov(on, 10.0f, 0.0f, pat::chest, om::add)
            .out(8.5f, -1.0f, 0.0f, 0.0f, 2.0f, 60.0f, on, 100.0f)),

        // The octave goes through the tube: a rumble with harmonics locked to the voice.
        preset("Sub Furnace", "Signal Flow", Sound()
            .order("PI CL DS DT VO RM SH GR SW SU OV RO EN TA")
            .in(0.0f, on)
            .pi(on, -2.0f, 0.0f, 5.0f, pm::shift, fm::follow)
            .cl(on, 35.0f, 35.0f, tone::voice)
            .ds(on, 30.0f, ds::split)
            .dt(on, 30.0f, 25.0f, spd::natural)
            .vo(off, 50.0f, oct::down1)
            .rm(off, 40.0f, 150.0f, 1.0f, car::sine, trk::fixed, rmode::ring, body::off)
            .sh(on, 12.0f, 11.0f, sh::up)
            .gr(on, 48.0f, 27.0f, shape::sine, chr::warm)
            .sw(on, 30.0f, 0.35f, circ::liquid4, mot::lfoLevel)
            .ro(on, 60.0f, -30.0f, vc::line)
            .en(on, 18.0f, 12.0f, voices::three, emot::medium)
            .ta(on, 30.0f)
            .su(on, 65.0f, 75.0f, reg::follow, sc::thick)
            .ov(off, 10.0f, 0.0f, pat::chest, om::add)
            .out(5.5f, -1.0f, 0.0f, 0.0f, 2.0f, 60.0f, on, 100.0f)),

        // A barrel-chested roar: the octave, then its overtones, then into the tube, so the
        // distortion is built on the low harmonics.
        preset("Ribcage", "Signal Flow", Sound()
            .order("PI CL DS DT VO RM SH GR SW SU OV RO EN TA")
            .in(0.0f, on)
            .pi(on, -2.0f, 0.0f, 5.0f, pm::shift, fm::follow)
            .cl(on, 35.0f, 35.0f, tone::voice)
            .ds(on, 30.0f, ds::split)
            .dt(on, 30.0f, 25.0f, spd::natural)
            .vo(off, 50.0f, oct::down1)
            .rm(off, 40.0f, 150.0f, 1.0f, car::sine, trk::fixed, rmode::ring, body::off)
            .sh(on, 12.0f, 11.0f, sh::up)
            .gr(on, 48.0f, 27.0f, shape::sine, chr::warm)
            .sw(on, 30.0f, 0.35f, circ::liquid4, mot::lfoLevel)
            .ro(on, 45.0f, -10.0f, vc::line)
            .en(on, 18.0f, 12.0f, voices::three, emot::medium)
            .ta(on, 30.0f)
            .su(on, 60.0f, 50.0f, reg::follow, sc::clean)
            .ov(on, 60.0f, -0.25f, pat::chest, om::add)
            .out(6.5f, -1.0f, 0.0f, 0.0f, 2.0f, 60.0f, on, 100.0f)),

        // Grit and chop on the human voice, then pitched down: everything thickens and slows (the
        // chop lands lower too).
        preset("Tar Pit", "Signal Flow", Sound()
            .order("CL DS RO GR PI SH SW DT VO RM EN TA SU OV")
            .in(0.0f, on)
            .pi(on, -7.0f, 0.0f, 25.0f, pm::shift, fm::follow)
            .cl(on, 35.0f, 35.0f, tone::voice)
            .ds(on, 30.0f, ds::split)
            .dt(on, 30.0f, 25.0f, spd::natural)
            .vo(off, 50.0f, oct::down1)
            .rm(off, 40.0f, 150.0f, 1.0f, car::sine, trk::fixed, rmode::ring, body::off)
            .sh(on, 12.0f, 11.0f, sh::up)
            .gr(on, 50.0f, 27.0f, shape::sine, chr::warm)
            .sw(on, 30.0f, 0.35f, circ::liquid4, mot::lfoLevel)
            .ro(on, 50.0f, 0.0f, vc::line)
            .en(on, 18.0f, 12.0f, voices::three, emot::medium)
            .ta(on, 30.0f)
            .su(on, 50.0f, 50.0f, reg::follow, sc::clean)
            .ov(on, 10.0f, 0.0f, pat::chest, om::add)
            .out(7.5f, -1.0f, 0.0f, 0.0f, 2.0f, 60.0f, on, 100.0f)),

        // Detail last: it lifts every modulation tail and breath into a claustrophobic halo.
        preset("Breathing Walls", "Signal Flow", Sound()
            .order("PI CL DS VO RM SH GR SW RO EN TA SU OV DT")
            .in(0.0f, on)
            .pi(on, -2.0f, 0.0f, 5.0f, pm::shift, fm::follow)
            .cl(on, 35.0f, 35.0f, tone::voice)
            .ds(on, 30.0f, ds::split)
            .dt(on, 80.0f, 30.0f, spd::snappy)
            .vo(off, 50.0f, oct::down1)
            .rm(off, 40.0f, 150.0f, 1.0f, car::sine, trk::fixed, rmode::ring, body::off)
            .sh(on, 12.0f, 11.0f, sh::up)
            .gr(on, 48.0f, 27.0f, shape::sine, chr::warm)
            .sw(on, 30.0f, 0.35f, circ::liquid4, mot::lfoLevel)
            .ro(on, 45.0f, 10.0f, vc::line)
            .en(on, 18.0f, 12.0f, voices::three, emot::medium)
            .ta(on, 30.0f)
            .su(on, 50.0f, 50.0f, reg::follow, sc::clean)
            .ov(on, 10.0f, 0.0f, pat::chest, om::add)
            .out(7.5f, -1.0f, 0.0f, 0.0f, 2.0f, 60.0f, on, 100.0f)),

        // A stereo shimmer, then a stuttering chop at the very end of the chain.
        preset("Chopped Space", "Signal Flow", Sound()
            .order("PI CL DS DT VO RM SW RO EN SH GR TA SU OV")
            .in(0.0f, on)
            .pi(on, -2.0f, 0.0f, 5.0f, pm::shift, fm::follow)
            .cl(on, 35.0f, 35.0f, tone::voice)
            .ds(on, 30.0f, ds::split)
            .dt(on, 30.0f, 25.0f, spd::natural)
            .vo(off, 50.0f, oct::down1)
            .rm(off, 40.0f, 150.0f, 1.0f, car::sine, trk::fixed, rmode::ring, body::off)
            .sh(on, 35.0f, 4.0f, sh::stereo)
            .gr(on, 50.0f, 9.0f, shape::square, chr::warm)
            .sw(on, 30.0f, 0.35f, circ::liquid4, mot::lfoLevel)
            .ro(on, 45.0f, 10.0f, vc::line)
            .en(on, 18.0f, 12.0f, voices::three, emot::medium)
            .ta(on, 30.0f)
            .su(on, 50.0f, 50.0f, reg::follow, sc::clean)
            .ov(on, 10.0f, 0.0f, pat::chest, om::add)
            .out(8.0f, -1.0f, 0.0f, 0.0f, 2.0f, 60.0f, on, 100.0f)),

        // The vocoder after the tube, so the bands are excited by a saturated voice. Bright and gritty.
        preset("Vocoded Fire", "Signal Flow", Sound()
            .order("PI CL DS DT RM SH GR SW RO VO EN TA SU OV")
            .in(0.0f, on)
            .pi(on, -2.0f, 0.0f, 5.0f, pm::shift, fm::follow)
            .cl(on, 35.0f, 35.0f, tone::voice)
            .ds(on, 30.0f, ds::split)
            .dt(on, 30.0f, 25.0f, spd::natural)
            .vo(on, 55.0f, oct::down1)
            .rm(off, 40.0f, 150.0f, 1.0f, car::sine, trk::fixed, rmode::ring, body::off)
            .sh(on, 12.0f, 11.0f, sh::up)
            .gr(on, 48.0f, 27.0f, shape::sine, chr::warm)
            .sw(on, 30.0f, 0.35f, circ::liquid4, mot::lfoLevel)
            .ro(on, 55.0f, 0.0f, vc::line)
            .en(on, 18.0f, 12.0f, voices::three, emot::medium)
            .ta(on, 30.0f)
            .su(on, 50.0f, 50.0f, reg::follow, sc::clean)
            .ov(on, 10.0f, 0.0f, pat::chest, om::add)
            .out(7.5f, -1.0f, 0.0f, 0.0f, 2.0f, 60.0f, on, 100.0f)),

        // A wide chorus ring-modulated into its own bell cluster per voice: a wide metallic crowd.
        preset("Clanking Choir", "Signal Flow", Sound()
            .order("PI CL DS DT VO EN RM RO SW GR TA SU OV SH")
            .in(0.0f, on)
            .pi(on, -2.0f, 0.0f, 5.0f, pm::shift, fm::follow)
            .cl(on, 35.0f, 35.0f, tone::voice)
            .ds(on, 30.0f, ds::split)
            .dt(on, 30.0f, 25.0f, spd::natural)
            .vo(off, 50.0f, oct::down1)
            .rm(on, 50.0f, 300.0f, 1.0f, car::sine, trk::fixed, rmode::ring, body::off)
            .sh(on, 12.0f, 11.0f, sh::up)
            .gr(on, 48.0f, 27.0f, shape::sine, chr::warm)
            .sw(on, 30.0f, 0.35f, circ::liquid4, mot::lfoLevel)
            .ro(on, 45.0f, 10.0f, vc::line)
            .en(on, 60.0f, 90.0f, voices::six, emot::medium)
            .ta(on, 30.0f)
            .su(on, 50.0f, 50.0f, reg::follow, sc::clean)
            .ov(on, 10.0f, 0.0f, pat::chest, om::add)
            .out(11.5f, -1.0f, 0.0f, 0.0f, 2.0f, 60.0f, on, 100.0f)),

        // VOCODER ----------------------------------------------------------------------------------
        // A wide pulse choir under the words.
        preset("Pulse Choir", "Vocoder", Sound()
            .order(c_reference)
            .in(0.0f, on)
            .pi(on, -2.0f, 0.0f, 5.0f, pm::shift, fm::follow)
            .cl(on, 35.0f, 35.0f, tone::voice)
            .ds(on, 30.0f, ds::split)
            .dt(on, 30.0f, 25.0f, spd::natural)
            .vo(on, 70.0f, oct::down1)
            .rm(off, 40.0f, 150.0f, 1.0f, car::sine, trk::fixed, rmode::ring, body::off)
            .sh(on, 12.0f, 11.0f, sh::up)
            .gr(off, 45.0f, 27.0f, shape::sine, chr::warm)
            .sw(on, 30.0f, 0.35f, circ::liquid4, mot::lfoLevel)
            .ro(on, 20.0f, 0.0f, vc::line)
            .en(on, 60.0f, 100.0f, voices::six, emot::medium)
            .ta(on, 30.0f)
            .su(on, 30.0f, 50.0f, reg::follow, sc::clean)
            .ov(on, 10.0f, 0.0f, pat::chest, om::add)
            .out(7.5f, -1.0f, 0.0f, 0.0f, 2.0f, 60.0f, on, 100.0f)),

        // The vocoder an octave lower again, with a dominant sub.
        preset("Two Octaves Down", "Vocoder", Sound()
            .order(c_reference)
            .in(0.0f, on)
            .pi(on, -2.0f, 0.0f, 5.0f, pm::shift, fm::follow)
            .cl(on, 35.0f, 35.0f, tone::voice)
            .ds(on, 30.0f, ds::split)
            .dt(on, 30.0f, 25.0f, spd::natural)
            .vo(on, 75.0f, oct::down2)
            .rm(off, 40.0f, 150.0f, 1.0f, car::sine, trk::fixed, rmode::ring, body::off)
            .sh(on, 12.0f, 11.0f, sh::up)
            .gr(on, 48.0f, 27.0f, shape::sine, chr::warm)
            .sw(on, 30.0f, 0.35f, circ::liquid4, mot::lfoLevel)
            .ro(on, 45.0f, 10.0f, vc::line)
            .en(on, 18.0f, 12.0f, voices::three, emot::medium)
            .ta(on, 30.0f)
            .su(on, 60.0f, 50.0f, reg::follow, sc::clean)
            .ov(on, 10.0f, 0.0f, pat::chest, om::add)
            .out(7.0f, -1.0f, 0.0f, 0.0f, 2.0f, 60.0f, on, 100.0f)),

        // Maximum vocoder gravel with a grinding chop.
        preset("Gravel Pit", "Vocoder", Sound()
            .order(c_reference)
            .in(0.0f, on)
            .pi(on, -2.0f, 0.0f, 5.0f, pm::shift, fm::follow)
            .cl(on, 35.0f, 35.0f, tone::voice)
            .ds(on, 30.0f, ds::split)
            .dt(on, 30.0f, 25.0f, spd::natural)
            .vo(on, 90.0f, oct::down1)
            .rm(off, 40.0f, 150.0f, 1.0f, car::sine, trk::fixed, rmode::ring, body::off)
            .sh(on, 12.0f, 11.0f, sh::up)
            .gr(on, 50.0f, 27.0f, shape::sine, chr::grind)
            .sw(on, 30.0f, 0.35f, circ::liquid4, mot::lfoLevel)
            .ro(on, 55.0f, 0.0f, vc::line)
            .en(on, 18.0f, 12.0f, voices::three, emot::medium)
            .ta(on, 30.0f)
            .su(on, 50.0f, 50.0f, reg::follow, sc::clean)
            .ov(on, 10.0f, 0.0f, pat::chest, om::add)
            .out(7.5f, -1.0f, 0.0f, 0.0f, 2.0f, 60.0f, on, 100.0f)),

        // A breathy, two-octave vocoder lung with a slow swirl.
        preset("Iron Lung", "Vocoder", Sound()
            .order(c_reference)
            .in(0.0f, on)
            .pi(on, -2.0f, 0.0f, 5.0f, pm::shift, fm::follow)
            .cl(on, 35.0f, 35.0f, tone::voice)
            .ds(on, 30.0f, ds::split)
            .dt(on, 80.0f, 35.0f, spd::snappy)
            .vo(on, 60.0f, oct::down2)
            .rm(off, 40.0f, 150.0f, 1.0f, car::sine, trk::fixed, rmode::ring, body::off)
            .sh(on, 50.0f, 2.0f, sh::upDown)
            .gr(on, 48.0f, 27.0f, shape::sine, chr::warm)
            .sw(on, 50.0f, 0.35f, circ::deep12, mot::lfoLevel)
            .ro(on, 45.0f, 10.0f, vc::line)
            .en(on, 18.0f, 12.0f, voices::three, emot::medium)
            .ta(on, 30.0f)
            .su(on, 50.0f, 50.0f, reg::follow, sc::clean)
            .ov(on, 10.0f, 0.0f, pat::chest, om::add)
            .out(7.0f, -1.0f, 0.0f, 0.0f, 2.0f, 60.0f, on, 100.0f)),

        // UTILITY ----------------------------------------------------------------------------------
        // Just the detail lift on a natural voice.
        preset("Detail Only", "Utility", Sound()
            .order(c_reference)
            .in(0.0f, on)
            .pi(off, -2.0f, 0.0f, 25.0f, pm::shift, fm::follow)
            .cl(off, 50.0f, 45.0f, tone::voice)
            .ds(off, 50.0f, ds::split)
            .dt(on, 70.0f, 35.0f, spd::natural)
            .vo(off, 50.0f, oct::down1)
            .rm(off, 40.0f, 150.0f, 1.0f, car::sine, trk::fixed, rmode::ring, body::off)
            .sh(off, 25.0f, 12.0f, sh::up)
            .gr(off, 45.0f, 27.0f, shape::sine, chr::warm)
            .sw(off, 35.0f, 0.35f, circ::liquid4, mot::lfoLevel)
            .ro(off, 35.0f, 0.0f, vc::line)
            .en(off, 35.0f, 15.0f, voices::three, emot::medium)
            .ta(off, 50.0f)
            .su(off, 50.0f, 50.0f, reg::follow, sc::clean)
            .ov(off, 30.0f, 0.0f, pat::chest, om::add)
            .out(7.5f, -1.0f, 0.0f, 0.0f, 2.0f, 60.0f, on, 100.0f)),

        // A slightly bigger voice, with every flavour barely on.
        preset("Subtle Size", "Utility", Sound()
            .order(c_reference)
            .in(0.0f, on)
            .pi(on, -1.0f, 15.0f, 15.0f, pm::shift, fm::follow)
            .cl(on, 35.0f, 35.0f, tone::voice)
            .ds(on, 30.0f, ds::split)
            .dt(on, 30.0f, 25.0f, spd::natural)
            .vo(off, 50.0f, oct::down1)
            .rm(off, 40.0f, 150.0f, 1.0f, car::sine, trk::fixed, rmode::ring, body::off)
            .sh(on, 15.0f, 12.0f, sh::up)
            .gr(on, 25.0f, 27.0f, shape::sine, chr::warm)
            .sw(on, 20.0f, 0.35f, circ::liquid4, mot::lfoLevel)
            .ro(on, 20.0f, 0.0f, vc::line)
            .en(on, 20.0f, 15.0f, voices::three, emot::medium)
            .ta(on, 30.0f)
            .su(on, 35.0f, 50.0f, reg::follow, sc::clean)
            .ov(on, 20.0f, 0.0f, pat::chest, om::add)
            .out(7.5f, -1.0f, 0.0f, 0.0f, 2.0f, 60.0f, on, 100.0f)),

        // Containment only (gate, level, de-ess, detail, top tame, gentle push), for dialogue
        // before other processing.
        preset("Clean Lift", "Utility", Sound()
            .order(c_reference)
            .in(0.0f, on)
            .pi(off, -2.0f, 0.0f, 25.0f, pm::shift, fm::follow)
            .cl(on, 35.0f, 35.0f, tone::voice)
            .ds(on, 30.0f, ds::split)
            .dt(on, 55.0f, 30.0f, spd::natural)
            .vo(off, 50.0f, oct::down1)
            .rm(off, 40.0f, 150.0f, 1.0f, car::sine, trk::fixed, rmode::ring, body::off)
            .sh(off, 25.0f, 12.0f, sh::up)
            .gr(off, 45.0f, 27.0f, shape::sine, chr::warm)
            .sw(off, 35.0f, 0.35f, circ::liquid4, mot::lfoLevel)
            .ro(off, 35.0f, 0.0f, vc::line)
            .en(off, 35.0f, 15.0f, voices::three, emot::medium)
            .ta(on, 30.0f)
            .su(off, 50.0f, 50.0f, reg::follow, sc::clean)
            .ov(off, 30.0f, 0.0f, pat::chest, om::add)
            .out(7.5f, -1.0f, 0.0f, 0.0f, 2.0f, 60.0f, on, 100.0f)),

        // Init pushed hard into the output for trailer-loud density: a soft clip takes the tips.
        preset("Loud & Contained", "Utility", Sound()
            .order(c_reference)
            .in(0.0f, on)
            .pi(on, -2.0f, 0.0f, 5.0f, pm::shift, fm::follow)
            .cl(on, 70.0f, 45.0f, tone::voice)
            .ds(on, 30.0f, ds::split)
            .dt(on, 30.0f, 25.0f, spd::natural)
            .vo(off, 50.0f, oct::down1)
            .rm(off, 40.0f, 150.0f, 1.0f, car::sine, trk::fixed, rmode::ring, body::off)
            .sh(on, 12.0f, 11.0f, sh::up)
            .gr(on, 48.0f, 27.0f, shape::sine, chr::warm)
            .sw(on, 30.0f, 0.35f, circ::liquid4, mot::lfoLevel)
            .ro(on, 45.0f, 10.0f, vc::line)
            .en(on, 18.0f, 12.0f, voices::three, emot::medium)
            .ta(on, 70.0f)
            .su(on, 50.0f, 50.0f, reg::follow, sc::clean)
            .ov(on, 10.0f, 0.0f, pat::chest, om::add)
            .out(10.5f, -1.0f, 4.0f, 0.0f, 2.0f, 60.0f, on, 100.0f)),

        // Init with a crunchy output in parallel: a fast release lets the limiter's gain follow the
        // low end, a hardish clip takes the peaks, and a 70 % mix keeps the clean voice underneath.
        preset("Crunch Lift", "Utility", Sound()
            .order(c_reference)
            .in(0.0f, on)
            .pi(on, -2.0f, 0.0f, 5.0f, pm::shift, fm::follow)
            .cl(on, 35.0f, 35.0f, tone::voice)
            .ds(on, 30.0f, ds::split)
            .dt(on, 30.0f, 25.0f, spd::natural)
            .vo(off, 50.0f, oct::down1)
            .rm(off, 40.0f, 150.0f, 1.0f, car::sine, trk::fixed, rmode::ring, body::off)
            .sh(on, 12.0f, 11.0f, sh::up)
            .gr(on, 48.0f, 27.0f, shape::sine, chr::warm)
            .sw(on, 30.0f, 0.35f, circ::liquid4, mot::lfoLevel)
            .ro(on, 45.0f, 10.0f, vc::line)
            .en(on, 18.0f, 12.0f, voices::three, emot::medium)
            .ta(on, 30.0f)
            .su(on, 50.0f, 50.0f, reg::follow, sc::clean)
            .ov(on, 10.0f, 0.0f, pat::chest, om::add)
            .out(10.0f, -1.0f, 3.0f, 60.0f, 0.5f, 3.0f, off, 70.0f)),

        // Init smashed flat: deep push, a hard clip and the fastest limiter.
        preset("Smashed", "Utility", Sound()
            .order(c_reference)
            .in(0.0f, on)
            .pi(on, -2.0f, 0.0f, 5.0f, pm::shift, fm::follow)
            .cl(on, 35.0f, 35.0f, tone::voice)
            .ds(on, 30.0f, ds::split)
            .dt(on, 30.0f, 25.0f, spd::natural)
            .vo(off, 50.0f, oct::down1)
            .rm(off, 40.0f, 150.0f, 1.0f, car::sine, trk::fixed, rmode::ring, body::off)
            .sh(on, 12.0f, 11.0f, sh::up)
            .gr(on, 48.0f, 27.0f, shape::sine, chr::warm)
            .sw(on, 30.0f, 0.35f, circ::liquid4, mot::lfoLevel)
            .ro(on, 45.0f, 10.0f, vc::line)
            .en(on, 18.0f, 12.0f, voices::three, emot::medium)
            .ta(on, 30.0f)
            .su(on, 50.0f, 50.0f, reg::follow, sc::clean)
            .ov(on, 10.0f, 0.0f, pat::chest, om::add)
            .out(13.0f, -1.0f, 9.0f, 100.0f, 0.05f, 1.0f, off, 100.0f)),

        // A creature that must read on phones and laptops: the low end exists only as overtones,
        // so it is loud and clear on small speakers with no woofer load.
        preset("Phone Monster", "Utility", Sound()
            .order(c_reference)
            .in(0.0f, on)
            .pi(on, -2.0f, 0.0f, 5.0f, pm::shift, fm::follow)
            .cl(on, 35.0f, 35.0f, tone::voice)
            .ds(on, 30.0f, ds::split)
            .dt(on, 30.0f, 25.0f, spd::natural)
            .vo(off, 50.0f, oct::down1)
            .rm(off, 40.0f, 150.0f, 1.0f, car::sine, trk::fixed, rmode::ring, body::off)
            .sh(on, 12.0f, 11.0f, sh::up)
            .gr(on, 48.0f, 27.0f, shape::sine, chr::warm)
            .sw(on, 30.0f, 0.35f, circ::liquid4, mot::lfoLevel)
            .ro(on, 45.0f, 10.0f, vc::line)
            .en(on, 18.0f, 12.0f, voices::three, emot::medium)
            .ta(on, 30.0f)
            .su(on, 55.0f, 50.0f, reg::follow, sc::clean)
            .ov(on, 60.0f, 0.25f, pat::speaker, om::replace)
            .out(7.5f, -1.0f, 0.0f, 0.0f, 2.0f, 60.0f, on, 100.0f)),
    };

    return presets;
}
