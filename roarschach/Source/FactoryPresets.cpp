#include "PresetManager.h"
#include "Params.h"

namespace
{
    // Every factory preset spells out all thirteen parameters, so a preset never inherits a stray
    // value from the sound before it and reads as a complete recipe. Fields left out of a Sound take
    // the plug-in's defaults (Params.h).
    //
    // Slots (detailSlot / vocoderSlot): 0 Start, 1 After Pitch, 2 After Growl, 3 After Liquid,
    // 4 After Roar, 5 End. vocoderOctave: 0 = -1 Oct, 1 = -2 Oct.
    struct Sound
    {
        float pitch = Params::pitchDefault; // semitones
        float size = Params::sizeDefault;   // percent from here on
        float growl = Params::growlDefault;
        float liquid = Params::liquidDefault;
        float roar = Params::roarDefault;
        float sub = Params::subDefault;
        float detailLow = Params::detailLowDefault;
        float detailHigh = Params::detailHighDefault;
        float gravel = Params::gravelDefault;
        float mix = Params::mixDefault;
        int detailSlot = Params::detailSlotDefault;
        int vocoderSlot = Params::vocoderSlotDefault;
        int vocoderOctave = Params::vocoderOctaveDefault;
    };

    std::map<juce::String, float> values(const Sound& s)
    {
        return {
            { Params::pitch, s.pitch },
            { Params::size, s.size },
            { Params::growl, s.growl },
            { Params::liquid, s.liquid },
            { Params::roar, s.roar },
            { Params::sub, s.sub },
            { Params::detailLow, s.detailLow },
            { Params::detailHigh, s.detailHigh },
            { Params::gravel, s.gravel },
            { Params::mix, s.mix },
            { Params::detailSlot, static_cast<float>(s.detailSlot) },
            { Params::vocoderSlot, static_cast<float>(s.vocoderSlot) },
            { Params::vocoderOctave, static_cast<float>(s.vocoderOctave) },
        };
    }

    enum Slot { slotStart = 0, afterPitch = 1, afterGrowl = 2, afterLiquid = 3, afterRoar = 4, slotEnd = 5 };
    enum Octave { oneDown = 0, twoDown = 1 };
}

const std::vector<PresetManager::FactoryPreset>& PresetManager::getFactoryPresets()
{
    static const std::vector<FactoryPreset> presets
    {
        // Every parameter at its default.
        { "Init", "", {} },

        // CREATURES: full creature voices, the chain pushed well past the defaults
        // A wet, rolling beast: deep and wide, with the liquid shimmer and ensemble well forward.
        { "Liquid Beast", "Creatures",
          values({ .pitch = -4.0f, .size = 55.0f, .growl = 30.0f, .liquid = 70.0f, .roar = 40.0f, .sub = 30.0f,
                   .detailLow = 35.0f, .detailHigh = 25.0f, .gravel = 0.0f, .mix = 100.0f,
                   .detailSlot = slotStart, .vocoderSlot = slotStart, .vocoderOctave = oneDown }) },

        // A rasping throat: the vocoder after the pitch shift adds gravel under a strong growl.
        { "Gravel Throat", "Creatures",
          values({ .pitch = -3.0f, .size = 45.0f, .growl = 45.0f, .liquid = 20.0f, .roar = 50.0f, .sub = 25.0f,
                   .detailLow = 40.0f, .detailHigh = 25.0f, .gravel = 45.0f, .mix = 100.0f,
                   .detailSlot = slotStart, .vocoderSlot = afterPitch, .vocoderOctave = oneDown }) },

        // Slow and huge: low pitch, big body, lots of sub, not much movement.
        { "Cave Troll", "Creatures",
          values({ .pitch = -7.0f, .size = 75.0f, .growl = 35.0f, .liquid = 25.0f, .roar = 45.0f, .sub = 45.0f,
                   .detailLow = 30.0f, .detailHigh = 25.0f, .gravel = 0.0f, .mix = 100.0f,
                   .detailSlot = slotStart, .vocoderSlot = slotStart, .vocoderOctave = oneDown }) },

        // Building-sized: everything big, a two-octave vocoder bed under it and heavy loud-part control.
        { "Kaiju", "Creatures",
          values({ .pitch = -9.0f, .size = 95.0f, .growl = 55.0f, .liquid = 45.0f, .roar = 70.0f, .sub = 70.0f,
                   .detailLow = 25.0f, .detailHigh = 40.0f, .gravel = 25.0f, .mix = 100.0f,
                   .detailSlot = slotStart, .vocoderSlot = slotStart, .vocoderOctave = twoDown }) },

        // Stony and grinding: dry (little liquid), hard growl, gravel vocoded after the growl.
        { "Gargoyle", "Creatures",
          values({ .pitch = -5.0f, .size = 60.0f, .growl = 60.0f, .liquid = 10.0f, .roar = 55.0f, .sub = 20.0f,
                   .detailLow = 35.0f, .detailHigh = 30.0f, .gravel = 30.0f, .mix = 100.0f,
                   .detailSlot = slotStart, .vocoderSlot = afterGrowl, .vocoderOctave = oneDown }) },

        // A snarling animal: only a little lower, but growl and roar near the top.
        { "Hellhound", "Creatures",
          values({ .pitch = -2.0f, .size = 30.0f, .growl = 75.0f, .liquid = 15.0f, .roar = 75.0f, .sub = 20.0f,
                   .detailLow = 30.0f, .detailHigh = 35.0f, .gravel = 0.0f, .mix = 100.0f,
                   .detailSlot = slotStart, .vocoderSlot = slotStart, .vocoderOctave = oneDown }) },

        // Heavy and mechanical: almost no growl or liquid, a vocoder after the saturation for a rigid tone.
        { "Golem", "Creatures",
          values({ .pitch = -8.0f, .size = 85.0f, .growl = 20.0f, .liquid = 5.0f, .roar = 30.0f, .sub = 55.0f,
                   .detailLow = 25.0f, .detailHigh = 30.0f, .gravel = 35.0f, .mix = 100.0f,
                   .detailSlot = slotStart, .vocoderSlot = afterRoar, .vocoderOctave = oneDown }) },

        // From the deep: liquid all the way up, big and low, with a rolling sub.
        { "Kraken", "Creatures",
          values({ .pitch = -6.0f, .size = 80.0f, .growl = 25.0f, .liquid = 90.0f, .roar = 35.0f, .sub = 50.0f,
                   .detailLow = 30.0f, .detailHigh = 25.0f, .gravel = 0.0f, .mix = 100.0f,
                   .detailSlot = slotStart, .vocoderSlot = slotStart, .vocoderOctave = oneDown }) },

        // VOICES: characters that still read clearly as speech
        // Deeper and bigger, with only a trace of grit: the detail compressor keeps every word up front.
        { "Big Bad Narrator", "Voices",
          values({ .pitch = -3.0f, .size = 35.0f, .growl = 10.0f, .liquid = 10.0f, .roar = 20.0f, .sub = 30.0f,
                   .detailLow = 45.0f, .detailHigh = 30.0f, .gravel = 0.0f, .mix = 100.0f,
                   .detailSlot = slotStart, .vocoderSlot = slotStart, .vocoderOctave = oneDown }) },

        // Breathy and close: a strong lift of the quiet parts, and the vocoder's noise carrier on the
        // consonants after the liquid stage.
        { "Demon Whisper", "Voices",
          values({ .pitch = -5.0f, .size = 50.0f, .growl = 40.0f, .liquid = 40.0f, .roar = 25.0f, .sub = 15.0f,
                   .detailLow = 70.0f, .detailHigh = 15.0f, .gravel = 25.0f, .mix = 100.0f,
                   .detailSlot = slotStart, .vocoderSlot = afterLiquid, .vocoderOctave = oneDown }) },

        // A big, calm voice: low and wide, little distortion.
        { "Giant", "Voices",
          values({ .pitch = -6.0f, .size = 70.0f, .growl = 10.0f, .liquid = 15.0f, .roar = 25.0f, .sub = 45.0f,
                   .detailLow = 35.0f, .detailHigh = 25.0f, .gravel = 0.0f, .mix = 100.0f,
                   .detailSlot = slotStart, .vocoderSlot = slotStart, .vocoderOctave = oneDown }) },

        // Two voices at once: some of the dry performance stays under a wobbling, two-octave vocoded one.
        { "Possessed", "Voices",
          values({ .pitch = -4.0f, .size = 40.0f, .growl = 55.0f, .liquid = 55.0f, .roar = 50.0f, .sub = 25.0f,
                   .detailLow = 40.0f, .detailHigh = 30.0f, .gravel = 40.0f, .mix = 80.0f,
                   .detailSlot = slotStart, .vocoderSlot = afterLiquid, .vocoderOctave = twoDown }) },

        // Squashed and driven: hard upward and downward compression at the end of the chain, like a
        // pushed transmission.
        { "Ogre Radio", "Voices",
          values({ .pitch = -5.0f, .size = 55.0f, .growl = 30.0f, .liquid = 5.0f, .roar = 65.0f, .sub = 10.0f,
                   .detailLow = 55.0f, .detailHigh = 55.0f, .gravel = 0.0f, .mix = 100.0f,
                   .detailSlot = slotEnd, .vocoderSlot = slotStart, .vocoderOctave = oneDown }) },

        // VOCODER: the pitch-tracked pulse vocoder up front
        // A chorus of pulse voices spread in stereo after the liquid stage; light on distortion.
        { "Pulse Choir", "Vocoder",
          values({ .pitch = -2.0f, .size = 30.0f, .growl = 10.0f, .liquid = 50.0f, .roar = 20.0f, .sub = 15.0f,
                   .detailLow = 35.0f, .detailHigh = 20.0f, .gravel = 70.0f, .mix = 100.0f,
                   .detailSlot = slotStart, .vocoderSlot = afterLiquid, .vocoderOctave = oneDown }) },

        // The voice at its own pitch, the vocoder carrier two octaves under it.
        { "Two Octaves Down", "Vocoder",
          values({ .pitch = 0.0f, .size = 40.0f, .growl = 15.0f, .liquid = 25.0f, .roar = 30.0f, .sub = 35.0f,
                   .detailLow = 35.0f, .detailHigh = 25.0f, .gravel = 75.0f, .mix = 100.0f,
                   .detailSlot = slotStart, .vocoderSlot = slotStart, .vocoderOctave = twoDown }) },

        // Gravel all the way up, after the growl, then driven by the roar.
        { "Gravel Pit", "Vocoder",
          values({ .pitch = -3.0f, .size = 50.0f, .growl = 40.0f, .liquid = 20.0f, .roar = 55.0f, .sub = 30.0f,
                   .detailLow = 40.0f, .detailHigh = 30.0f, .gravel = 100.0f, .mix = 100.0f,
                   .detailSlot = slotStart, .vocoderSlot = afterGrowl, .vocoderOctave = oneDown }) },

        // Robotic and heavy: no growl or liquid movement, the vocoder last and two octaves down.
        { "Robot Ogre", "Vocoder",
          values({ .pitch = -5.0f, .size = 60.0f, .growl = 0.0f, .liquid = 0.0f, .roar = 40.0f, .sub = 25.0f,
                   .detailLow = 40.0f, .detailHigh = 35.0f, .gravel = 90.0f, .mix = 100.0f,
                   .detailSlot = slotStart, .vocoderSlot = slotEnd, .vocoderOctave = twoDown }) },

        // Wheezing and mechanical: breath lifted by the detail compressor right after the pitch stage,
        // the vocoder after the saturation, two octaves down.
        { "Iron Lung", "Vocoder",
          values({ .pitch = -4.0f, .size = 55.0f, .growl = 25.0f, .liquid = 35.0f, .roar = 45.0f, .sub = 20.0f,
                   .detailLow = 55.0f, .detailHigh = 25.0f, .gravel = 60.0f, .mix = 100.0f,
                   .detailSlot = afterPitch, .vocoderSlot = afterRoar, .vocoderOctave = twoDown }) },

        // UTILITY
        // Only the detail compressor: the creature stages are all off (pitch 0, size 0).
        { "Detail Only", "Utility",
          values({ .pitch = 0.0f, .size = 0.0f, .growl = 0.0f, .liquid = 0.0f, .roar = 0.0f, .sub = 0.0f,
                   .detailLow = 45.0f, .detailHigh = 30.0f, .gravel = 0.0f, .mix = 100.0f,
                   .detailSlot = slotStart, .vocoderSlot = slotStart, .vocoderOctave = oneDown }) },

        // A touch more weight on an ordinary voice, nothing that reads as an effect.
        { "Subtle Size", "Utility",
          values({ .pitch = -1.0f, .size = 30.0f, .growl = 5.0f, .liquid = 5.0f, .roar = 10.0f, .sub = 10.0f,
                   .detailLow = 30.0f, .detailHigh = 20.0f, .gravel = 0.0f, .mix = 100.0f,
                   .detailSlot = slotStart, .vocoderSlot = slotStart, .vocoderOctave = oneDown }) },

        // The natural voice, brought forward: detail lift and control, a little body and warmth.
        { "Clean Lift", "Utility",
          values({ .pitch = 0.0f, .size = 15.0f, .growl = 0.0f, .liquid = 0.0f, .roar = 12.0f, .sub = 12.0f,
                   .detailLow = 55.0f, .detailHigh = 35.0f, .gravel = 0.0f, .mix = 100.0f,
                   .detailSlot = slotStart, .vocoderSlot = slotStart, .vocoderOctave = oneDown }) },
    };

    return presets;
}
