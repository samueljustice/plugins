#include "SignalFlowStrip.h"

#include "DiceButton.h"
#include "RoarschachLookAndFeel.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>

namespace
{
    using namespace Theme::Colours;

    // Design-size layout of the strip (1728 x 336 in the editor): IN, two rows of eight module
    // slots (the last one an empty end-of-row drop target), OUTPUT.
    constexpr int c_inWidth = 112;
    constexpr int c_outWidth = 292;
    constexpr int c_endGap = 16;      // between IN, the module grid and OUTPUT (the connectors run here)
    constexpr int c_gap = 10;         // between two tiles in a row
    constexpr int c_rowGap = 12;      // between the rows (the return connector runs here)
    constexpr int c_columns = 8;
    constexpr int c_footerHeight = 16;
    constexpr int c_chipHeight = 18;
    constexpr int c_pad = 8;
    constexpr int c_captionHeight = 13;
    constexpr float c_radius = 8.0f;
    constexpr float c_offAlpha = 0.45f;       // an Off tile, and a knob the current settings do not use
    constexpr float c_weakAlpha = 0.7f;       // a knob that does little at the current settings
    constexpr float c_tileValueFont = 11.0f;
    // Type sizes (design px; the window opens at 80-100 % of them). Captions are 11 where the tile
    // has the room and never under c_minCaptionFont; the small readout labels are 10.
    constexpr float c_captionFont = 11.0f, c_minCaptionFont = 9.0f;
    constexpr float c_smallFont = 10.0f;
    constexpr float c_chamberEdgeFrom = 60.0f;   // CHAMBER Amount (%) from which Edge is clearly heard
    constexpr int c_diceSize = 18;    // a module's die (its pill, as large as the On button), at the right end of its chip row
    constexpr int c_diceHit = 22;     // ... and its clickable area, a little larger than the pill
    constexpr int c_diceGap = 6;      // between the mode chip and the die's pill
    constexpr int c_rollAllSize = 34; // ROLL ALL's die, in the empty slot at the end of row 2
    constexpr const char* c_rollAllCaption = "RANDOMISE ALL";


    constexpr double c_slideMs = 120.0;    // tiles making room for a dragged tile, a drop, a menu move
    constexpr double c_settleMs = 220.0;   // an order changed by the host or a preset
    constexpr double c_grHoldSeconds = 1.0;

    // Every control: its caption on the tile and its tooltip (plain words, on its own terms).
    struct Control
    {
        const char* id;
        const char* caption;
        const char* tip;
    };

    constexpr Control c_controls[] {
        { Params::inTrim, "INPUT", "Level into the plug-in, before everything else. With Auto on, it raises or lowers the levelled voice by half as much." },
        { Params::inAuto, "AUTO", "Brings any voice to the same working level, so every module reacts the same way to a quiet or a loud take." },
        { Params::pitch, "PITCH", "How far the voice is shifted, in semitones. -2 to -4: a bigger human; -5 to -9: a creature; -10 to -14: a monster (add Size, or Formant Keep to hold the words). Below -15 the words blur with Follow; above +6 the throat stops following. In Mono mode it sets the note held (0 st = C3)." },
        { Params::pitchSize, "SIZE", "Makes the throat bigger: lowers the formants and adds body. With Pitch below -18 st and Follow, the throat is already at its lowest, so Size only adds body." },
        { Params::pitchDrive, "DRIVE", "Tube drive after the shift: up to about 25 % warmth, 35-70 % grit, 100 % heavy." },
        { Params::pitchMode, "MODE", "Shift moves the voice by the set interval. Snap also lands each note on the nearest semitone. Mono holds the voice on one note, set by Pitch (0 st = C3)." },
        { Params::pitchFormant, "FORMANT", "Follow moves the throat with the pitch, for a bigger or smaller creature. Keep leaves the throat where it is, so only the pitch changes." },
        { Params::cleanContain, "CONTAIN", "Evens out the level and catches peaks before the creature stages. It starts working at the bottom of the travel." },
        { Params::cleanGate, "GATE", "Closes on room noise between words; higher settings close further and harder. 25-40 % suits dialogue." },
        { Params::cleanTone, "TONE", "The voice EQ. Flat leaves the voice as it is, Voice cleans up the low mids, Heavy cuts the boom and low mids hard for a tight, forward chest (it thins the lows), Radio is a narrow, lo-fi band." },
        { Params::deess, "AMOUNT", "Softens harsh S and T sounds: more travel takes them down further." },
        { Params::deessMode, "MODE", "Split turns down only the hiss above about 3.5 kHz on each S. Wide turns down the whole voice for the length of the S, which also tames lisps and mouth noise but dips the words." },
        { Params::detailLow, "LOW", "Lifts the quiet parts (breaths, consonants, tails) while the loud syllables stay where they are. 40-60 % is a clear lift; it never lifts the room tone the gate is taking out." },
        { Params::detailHigh, "HIGH", "Pushes the loud parts up into a soft ceiling: denser and more forward. 30-60 % is a strong, even level." },
        { Params::detailSpeed, "SPEED", "How quickly the level follows the voice. Natural is smooth; Snappy recovers faster and sounds more lively." },
        { Params::gravel, "GRAVEL", "How much of the pulse-carrier vocoder is heard. About 30 % is gritty with the words fully intact; about 70 % is a dominant buzz." },
        { Params::vocoderOctave, "OCTAVE", "Register of the vocoder's pulse carrier: -1 Oct is low and buzzy, -2 Oct lower and rougher. The filter bank keeps its weight above the deepest bass." },
        { Params::ring, "AMOUNT", "How much of the ring-modulated voice is heard. About 20 % adds a metallic edge, 50 % is an equal metallic blend, 100 % is pure ring." },
        { Params::ringFreq, "FREQ", "The carrier frequency. 110-450 Hz gives a metallic voice; below about 50 Hz it becomes a tremolo; above about 1.3 kHz it squeaks." },
        { Params::ringRatio, "RATIO", "The carrier as a multiple of the voice's pitch. Whole-number ratios stay in tune with the voice; in-between ratios clang; below x0.35 it chops." },
        { Params::ringCarrier, "CARRIER", "The carrier waveform. Sine is smooth, Triangle a softened sine, Square buzzy and hard." },
        { Params::ringTrack, "TRACK", "Fixed holds the carrier at one frequency. Follow moves it with the voice's pitch." },
        { Params::ringMode, "MODE", "Ring removes the original pitch and leaves only the metallic sidebands. AM keeps the original voice and adds the sidebands around it." },
        { Params::ringBody, "BODY", "Puts the voice inside a small resonant box. Box is a hard, hollow enclosure; Can is smaller and more metallic." },
        { Params::shimmer, "AMOUNT", "A liquid wobble: a copy of the voice moved a few hertz beats against the voice. 15-30 % is a subtle layer; 90-100 % a full wobble." },
        { Params::shimmerRate, "RATE", "How fast the wobble beats. 0.5-2 Hz is a liquid swell, 3-9 Hz a warble, 13 Hz and up rough." },
        { Params::shimmerMode, "MODE", "Which way the copy is moved. Up and Down wobble differently, Up+Down uses both, Stereo puts one on each side (summed to mono, Stereo wobbles less)." },
        { Params::growl, "DEPTH", "How deeply the voice is chopped. 15-35 % adds texture; 45-100 % is an obvious growl." },
        { Params::growlRate, "RATE", "How fast the voice is chopped. 5-7 Hz stutters, 20-35 Hz growls, 60-100 Hz buzzes. Not used by the Throat shape." },
        { Params::growlShape, "SHAPE", "The chop's shape. Sine is smooth, Opto pulses like a lamp, Square switches hard, Throat locks to half the voice's pitch (Rate is not used), Rattle speeds up as the voice gets louder." },
        { Params::growlCharacter, "CHARACTER", "What the chop does to the tone. Clean only changes the level, Warm rounds the low end, Grind adds a gritty edge." },
        { Params::swirl, "AMOUNT", "How much of the sweeping phaser is mixed in." },
        { Params::swirlRate, "RATE", "How fast the phaser sweeps. 0.15-0.5 Hz is slow and liquid, 1-2.5 Hz animated, 4 Hz and up a warble." },
        { Params::swirlCircuit, "CIRCUIT", "The phaser's circuit: how many notches it has and how deep they cut, from a liquid four-stage to a dense twelve-stage sweep." },
        { Params::swirlMotion, "MOTION", "What moves the sweep. LFO is steady, LFO+Level sweeps further on loud words, Random wanders." },
        { Params::roar, "DRIVE", "Tube saturation. 25-45 % gives warmth and presence; 70-100 % grit." },
        { Params::roarTone, "TONE", "Tilts what the tube drives harder: the bass or the treble. It works at any Drive." },
        { Params::roarVoicing, "VOICING", "Line drives the tube flat. Mic first trims the boom and the air, as from a close microphone." },
        { Params::ensemble, "AMOUNT", "How much of the chorus of voices is mixed in." },
        { Params::ensembleWidth, "WIDTH", "How far the chorus spreads to the sides. Past about 100 % the chorus thins when the mix is summed to mono." },
        { Params::ensembleVoices, "VOICES", "How many voices sing along: 3 is tight, 6 is a crowd." },
        { Params::ensembleMotion, "MOTION", "How the voices drift. Slow, Medium and Fast set the movement; Vibrato makes them waver in pitch." },
        { Params::tame, "AMOUNT", "Holds back harsh top end when it gets loud; higher settings start lower and hold back harder." },
        { Params::sub, "LEVEL", "Level of the octave underneath the voice. 40-65 % is a solid octave. With Overtone in Replace, Sub Level also adds weight through the overtones." },
        { Params::subDrive, "DRIVE", "Drive on the octave. Up to about 20 % it stays clean; above that it gets thicker and growlier." },
        { Params::subRegister, "REGISTER", "Where the octave sits. Follow tracks the voice an octave down, Tight keeps it at 55 Hz or above, Deep goes a full octave down to about 32 Hz." },
        { Params::subCharacter, "CHARACTER", "The octave's colour. Clean is a pure low tone, Resonant rings and drives harder, Thick adds a second, fuller layer." },
        { Params::overtone, "AMOUNT", "How strongly the low end is echoed as overtones. They let the voice's weight be heard on small speakers without adding more bass. In Lean and Replace the original low end is cut more as Amount rises." },
        { Params::overtoneFreq, "FREQ", "Moves the overtone bands up or down together. Higher translates better to small speakers; lower is warmer." },
        { Params::overtonePattern, "PATTERN", "Which low regions are turned into overtones. Chest: body and octave. Throat: gruff upper growl. Beast: hollow, odd-heavy snarl. Speaker: high overtones for small or radio-like creatures. Broad: one wide low band. Tracked: follows the voice's pitch exactly." },
        { Params::overtoneMode, "MODE", "Add keeps the original low end. Lean lowers it. Replace removes it, so the low end is heard only through its overtones." },
        { Params::outPush, "PUSH", "Drives the voice into the ceiling. Push 8-12 is clean and dense; for loud, Push 14-18 with Clip 6-9. The peaks never pass the ceiling." },
        { Params::outCeiling, "CEILING", "The highest level the output can reach, true-peak safe. It sets the output level only: Push decides how hard the limiter works." },
        { Params::outClip, "CLIP", "How many dB of each peak above the ceiling are clipped instead of limited. Raise Push to drive into it." },
        { Params::outClipShape, "SHAPE", "Clip shape. Soft rounds the peaks gently; Hard clips them flat, which is brighter and more aggressive. It needs Clip above 0." },
        { Params::outAttack, "ATTACK", "How early the limiter starts turning down before a peak. With Auto off and a short Release, 0.3-3 ms crunches the low end. With Auto on it changes little." },
        { Params::outRelease, "RELEASE", "How fast the limiter recovers. With Auto off: 2-10 ms crunch, 25-60 ms grit, 200 ms and up clean." },
        { Params::outAuto, "AUTO", "Auto release: adapts the recovery to the material and keeps the low end clean. Release sets how quickly it breathes." },
        { Params::mix, "MIX", "Blends the processed voice with the dry voice at the same loudness, so only the character changes." },
        { Params::chamber, "AMOUNT", "How much of the voice played into the room is blended under the dry voice. 15-35 % adds a subtle edge, 45-65 % is a clear chamber; around 80 % the two are equal; at 100 % the room leads." },
        { Params::chamberResonance, "RESON", "How much the room rings after each sound. Lower is tight and boxy; higher lets its resonances sustain longer. The level stays the same." },
        { Params::chamberDrive, "EDGE", "Drives the small speaker into breakup: grit at 15-35 %, a cranked speaker at 50-70 %, torn and buzzing towards 100 %. Heard once Amount is about 60 % or more." },
        { Params::chamberWidth, "WIDTH", "How far the room spreads to the sides. 0 % keeps it centred; 100 % is the room's full stereo. Summed to mono it sounds the same at any Width." },
        { Params::chamberRoom, "ROOM", "The room the speaker plays into. Steel Box: bright and metallic. Resonant Chamber: long and ringing. Concrete Room: hard and dense. Tin Booth: thin, with a sharp ring. Padded Cell: dark and short. Wood Chamber: warm. Timber Room: sparse and soft. Soft Bloom: a diffuse swell. Party Wall: a big concrete room through the wall, dark and booming. Tower Block: further down the hall, darker still." },
        { Params::chamberSpeaker, "SPEAKER", "The speaker the voice is played through. Tiny is thin and nasal, Small a compact full-range box, Horn a narrow, honking paging horn." },
        { Params::chamberBoom, "BOOM", "Low-end weight added after the whole chain, a shelf at 110 Hz: Light +3 dB, Medium +6 dB, Heavy +9 dB." },
    };

    const Control* findControl(std::string_view id)
    {
        for (const auto& c : c_controls)
            if (id == c.id)
                return &c;
        return nullptr;
    }

    juce::String captionFor(const char* id)
    {
        const auto* c = findControl(id);
        return c != nullptr ? juce::String(c->caption) : juce::String(Params::spec(id).name).toUpperCase();
    }

    // Short forms the mode chip falls back to when the full names do not fit.
    juce::String shortChoice(const juce::String& name)
    {
        static const std::pair<const char*, const char*> table[] {
            { "TRIANGLE", "TRI" }, { "SQUARE", "SQR" }, { "FOLLOW", "FLW" }, { "FIXED", "FIX" },
            { "LFO+LEVEL", "LFO+LVL" }, { "UP+DOWN", "UP+DN" }, { "RESONANT", "RESON" }, { "REPLACE", "REPL" },
            { "SPEAKER", "SPKR" }, { "TRACKED", "TRACK" }, { "NATURAL", "NATUR" }, { "MEDIUM", "MED" },
            { "VIBRATO", "VIB" }, { "SMOOTH 4", "SMTH 4" },
            { "STEEL BOX", "STEEL" }, { "RESONANT CHAMBER", "RESONANT" }, { "CONCRETE ROOM", "CONCRETE" }, { "TIN BOOTH", "TIN" },
            { "PADDED CELL", "PADDED" }, { "WOOD CHAMBER", "WOOD" }, { "TIMBER ROOM", "TIMBER" }, { "SOFT BLOOM", "BLOOM" },
            { "PARTY WALL", "PARTY" }, { "TOWER BLOCK", "TOWER" },
            { "HEAVY", "HVY" },
        };
        for (const auto& [full, abbreviation] : table)
            if (name == full)
                return abbreviation;
        return name;
    }

    /** A still shorter form of a short choice name, for a chip that cannot fit the short ones. */
    juce::String compactChoice(const juce::String& shortName)
    {
        static const std::pair<const char*, const char*> table[] {
            { "RESONANT", "RESO" }, { "CONCRETE", "CONC" }, { "PADDED", "PAD" }, { "TIMBER", "TIMB" },
            { "SMALL", "SML" }, { "LIGHT", "LGT" }, { "BLOOM", "BLM" }, { "STEEL", "STL" },
            { "PARTY", "PRTY" }, { "TOWER", "TOWR" },
        };
        for (const auto& [from, to] : table)
            if (shortName == from)
                return to;
        return shortName;
    }

    float textWidth(const juce::Font& font, const juce::String& text)
    {
        return juce::GlyphArrangement::getStringWidth(font, text + " ") - juce::GlyphArrangement::getStringWidth(font, " ");
    }

    float load(const std::atomic<float>& a)
    {
        const float v = a.load(std::memory_order_relaxed);
        return std::isfinite(v) ? v : 0.0f;
    }

    // A readout value with its sign: no sign on a zero (so it never reads "-0.0"), "+" or "-"
    // in front otherwise. sign: "+" / "-" forces that sign on the magnitude, nullptr uses v's own.
    juce::String signedReadout(float v, int decimals, const char* sign = nullptr)
    {
        const float scale = std::pow(10.0f, static_cast<float>(decimals));
        const float rounded = std::round(v * scale) / scale;
        if (!std::isfinite(rounded) || std::abs(rounded) < 0.5f / scale)
            return juce::String(0.0f, decimals);
        const char* s = sign != nullptr ? sign : (rounded > 0.0f ? "+" : "-");
        return s + juce::String(std::abs(rounded), decimals);
    }

    // "0.0 dB" for nothing (never "-0.0"), otherwise the signed value.
    juce::String grText(float db)
    {
        const float v = std::min(std::abs(db), 99.9f);
        return v < 0.05f ? juce::String("0.0 dB") : "-" + juce::String(v, 1) + " dB";
    }

    juce::String hzText(float hz)
    {
        if (hz >= 1000.0f)
            return juce::String(hz / 1000.0f, 2) + " kHz";
        return juce::String(juce::roundToInt(hz)) + " Hz";
    }

    void drawTrack(juce::Graphics& g, juce::Rectangle<float> r)
    {
        g.setColour(outline);
        g.fillRoundedRectangle(r, std::min(2.0f, r.getHeight() * 0.5f));
    }

    /** A horizontal bar, 0..1, filled from the left. */
    void drawBar(juce::Graphics& g, juce::Rectangle<float> r, float fraction, juce::Colour colour)
    {
        drawTrack(g, r);
        const float f = juce::jlimit(0.0f, 1.0f, fraction);
        if (f > 0.0f)
        {
            g.setColour(colour);
            g.fillRoundedRectangle(r.withWidth(std::max(r.getHeight(), r.getWidth() * f)), std::min(2.0f, r.getHeight() * 0.5f));
        }
    }

    /** A horizontal bar, -1..1, filled from the centre. */
    void drawCentredBar(juce::Graphics& g, juce::Rectangle<float> r, float value, juce::Colour colour)
    {
        drawTrack(g, r);
        const float v = juce::jlimit(-1.0f, 1.0f, value);
        const float half = r.getWidth() * 0.5f;
        const float x0 = r.getCentreX(), x1 = x0 + v * half;
        g.setColour(colour);
        g.fillRect(juce::Rectangle<float>(std::min(x0, x1), r.getY(), std::max(1.0f, std::abs(x1 - x0)), r.getHeight()));
        g.setColour(mark);
        g.fillRect(juce::Rectangle<float>(x0 - 0.5f, r.getY() - 1.5f, 1.0f, r.getHeight() + 3.0f));
    }
}

//==============================================================================
struct SignalFlowStrip::Knob
{
    const char* paramId = nullptr;
    juce::Slider slider;
    juce::Label caption;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
};

//==============================================================================
/** The move chip: three dots in a pill, the house's chip look (no glyphs). */
class SignalFlowStrip::MoveChip : public juce::Button
{
public:
    MoveChip() : juce::Button("Move") {}

    void paintButton(juce::Graphics& g, bool isMouseOver, bool isDown) override
    {
        const auto r = getLocalBounds().toFloat().reduced(0.5f);
        const float corner = r.getHeight() * 0.5f;
        g.setColour(panelRaised.brighter(isDown ? 0.2f : isMouseOver ? 0.1f : 0.0f));
        g.fillRoundedRectangle(r, corner);
        if (RoarschachLookAndFeel::shouldShowFocusRing(*this))
        {
            g.setColour(output);
            g.drawRoundedRectangle(r.reduced(0.25f), corner, 1.5f);
        }
        else
        {
            g.setColour(isMouseOver ? textDim : outline);
            g.drawRoundedRectangle(r, corner, 1.0f);
        }

        g.setColour(isMouseOver || isDown ? Theme::Colours::text : textDim);
        const float d = 2.4f;
        for (int k = -1; k <= 1; ++k)
            g.fillEllipse(juce::Rectangle<float>(d, d).withCentre(r.getCentre().translated(static_cast<float>(k) * 4.6f, 0.0f)));
    }
};

//==============================================================================
/** The soft shadow under a tile that is being dragged (it sits right behind that tile). */
class SignalFlowStrip::DragShadow : public juce::Component
{
public:
    static constexpr int c_margin = 22;

    DragShadow()
    {
        setInterceptsMouseClicks(false, false);
    }

    void paint(juce::Graphics& g) override
    {
        const auto inner = getLocalBounds().reduced(c_margin);
        juce::Path p;
        p.addRoundedRectangle(inner.toFloat().translated(0.0f, 8.0f), c_radius);
        juce::DropShadow(juce::Colours::black.withAlpha(0.95f), 24, { 0, 0 }).drawForPath(g, p);
    }
};

//==============================================================================
/** A panel in the strip with knobs (the base of IN, OUTPUT and the module tiles). */
class SignalFlowStrip::Tile : public juce::Component
{
public:
    Tile(SignalFlowStrip& owner, juce::String title, juce::Colour accent)
        : m_owner(owner), m_title(std::move(title)), m_accent(accent)
    {
        setTitle(m_title);
    }

    Knob& addKnob(const char* id)
    {
        auto knob = std::make_unique<Knob>();
        knob->paramId = id;
        auto& slider = knob->slider;
        slider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
        slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 84, 18);
        slider.setRotaryParameters(juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
        slider.setColour(juce::Slider::rotarySliderFillColourId, m_accent);
        slider.setTooltip(tooltipFor(id));
        slider.setTitle(Params::spec(id).name);
        RoarschachLookAndFeel::setValueFontHeight(slider, c_tileValueFont);
        knob->attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(m_owner.m_parameters, id, slider);

        if (auto* param = m_owner.m_parameters.getParameter(id))
        {
            // The value box shows the parameter's own text, and typed values go through its parser,
            // so the box reads exactly as the host does.
            slider.textFromValueFunction = [param](double v) { return param->getText(param->convertTo0to1(static_cast<float>(v)), 0); };
            slider.valueFromTextFunction = [param](const juce::String& t)
            {
                return static_cast<double>(param->convertFrom0to1(param->getValueForText(t)));
            };
            slider.setDoubleClickReturnValue(true, param->convertFrom0to1(param->getDefaultValue()));
        }
        slider.updateText();
        addAndMakeVisible(slider);
        // The value box was made before the slider had a parent (with the default LookAndFeel):
        // setting a colour makes the slider rebuild it with ours.
        slider.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);

        knob->caption.setText(captionFor(id), juce::dontSendNotification);
        knob->caption.setFont(m_owner.m_fonts.label(c_captionFont));
        knob->caption.setJustificationType(juce::Justification::centred);
        knob->caption.setColour(juce::Label::textColourId, textDim);
        knob->caption.setInterceptsMouseClicks(false, false);
        knob->caption.setMinimumHorizontalScale(1.0f);
        knob->caption.setBorderSize({ 0, 0, 0, 0 });
        addAndMakeVisible(knob->caption);

        m_knobs.push_back(std::move(knob));
        return *m_knobs.back();
    }

    static void placeKnob(Knob& knob, juce::Rectangle<int> cell)
    {
        knob.caption.setBounds(cell.removeFromTop(c_captionHeight));
        knob.slider.setBounds(cell);
    }

    /** After the knobs are placed: one caption size for the tile, the largest (up to c_captionFont)
        at which every caption, centred over its knob, clears the captions beside it and the sides of the tile. A
        caption may be wider than its knob's cell (CHAMBER's four knobs are narrower than their words). */
    void fitCaptions()
    {
        const auto widthAt = [this](const Knob& knob, float height)
        {
            return juce::GlyphArrangement::getStringWidth(m_owner.m_fonts.label(height), knob.caption.getText()) + 2.0f;
        };
        const auto fits = [&](float height)
        {
            for (const auto& a : m_knobs)
            {
                const float ca = static_cast<float>(a->slider.getBounds().getCentreX()), wa = widthAt(*a, height);
                if (ca - wa * 0.5f < 2.0f || ca + wa * 0.5f > static_cast<float>(getWidth()) - 2.0f)
                    return false;
                for (const auto& b : m_knobs)
                {
                    // (two knobs in one place, RING's Freq and Ratio, never show together)
                    const float cb = static_cast<float>(b->slider.getBounds().getCentreX());
                    const bool sameRow = a->slider.getY() == b->slider.getY();
                    if (sameRow && cb > ca + 0.5f && ca + wa * 0.5f + 3.0f > cb - widthAt(*b, height) * 0.5f)
                        return false;
                }
            }
            return true;
        };

        float height = c_captionFont;
        while (height > c_minCaptionFont && !fits(height))
            height -= 0.25f;

        for (auto& knob : m_knobs)
        {
            const int w = static_cast<int>(std::ceil(widthAt(*knob, height))) + 2;
            knob->caption.setFont(m_owner.m_fonts.label(height));
            knob->caption.setBounds(knob->slider.getBounds().getCentreX() - w / 2, knob->caption.getY(), w, c_captionHeight);
        }
    }

    /** How much the current settings use a knob: fully, a little (it is half-dimmed), not at all
        (dimmed like an Off tile). A dimmed knob still turns. */
    enum class KnobUse { full, weak, none };

    void setKnobUse(const char* paramId, KnobUse use)
    {
        for (size_t k = 0; k < m_knobs.size() && k < m_knobUse.size(); ++k)
            if (std::string_view(m_knobs[k]->paramId) == paramId && m_knobUse[k] != use)
            {
                m_knobUse[k] = use;
                applyAlpha();
            }
    }

    /** The tile's own alpha (an Off module tile is dimmed as a whole). */
    virtual float tileAlpha() const { return 1.0f; }

    virtual void applyAlpha()
    {
        const float alpha = tileAlpha();
        for (size_t i = 0; i < m_knobs.size(); ++i)
        {
            const auto use = i < m_knobUse.size() ? m_knobUse[i] : KnobUse::full;
            const float a = std::min(alpha, use == KnobUse::none ? c_offAlpha : use == KnobUse::weak ? c_weakAlpha : 1.0f);
            m_knobs[i]->slider.setAlpha(a);
            m_knobs[i]->caption.setAlpha(a);
        }
    }

    void paintPanel(juce::Graphics& g, bool lifted, float titleAlpha, bool dot)
    {
        const auto r = getLocalBounds().toFloat();
        g.setColour(lifted ? panel.brighter(0.06f) : panel);
        g.fillRoundedRectangle(r, c_radius);
        g.setColour(lifted ? m_accent.withAlpha(0.8f) : outlineSoft);
        g.drawRoundedRectangle(r.reduced(0.5f), c_radius, lifted ? 1.5f : 1.0f);

        g.setColour(outlineSoft);
        g.fillRect(juce::Rectangle<float>(12.0f, static_cast<float>(c_headerHeight), r.getWidth() - 24.0f, 1.0f));

        if (dot)
        {
            auto header = r.withHeight(static_cast<float>(c_headerHeight)).reduced(12.0f, 0.0f);
            g.setColour(m_accent);
            g.fillEllipse(header.removeFromLeft(6.0f).withSizeKeepingCentre(6.0f, 6.0f));
            header.removeFromLeft(8.0f);
            g.setFont(m_owner.m_fonts.heading(11.0f));
            g.setColour(m_accent.withMultipliedAlpha(titleAlpha));
            g.drawText(m_title, header, juce::Justification::centredLeft, false);
        }
    }

    SignalFlowStrip& m_owner;
    const juce::String m_title;
    const juce::Colour m_accent;
    std::vector<std::unique_ptr<Knob>> m_knobs;
    std::array<KnobUse, 8> m_knobUse {};
};

//==============================================================================
class SignalFlowStrip::ModuleTile : public Tile, public juce::SettableTooltipClient
{
public:
    ModuleTile(SignalFlowStrip& owner, int module)
        : Tile(owner, Params::modules[static_cast<size_t>(module)].uiName, juce::Colour(Params::modules[static_cast<size_t>(module)].accent))
        , m_module(module)
        , m_spec(Params::modules[static_cast<size_t>(module)])
    {
        const juce::String name(m_spec.uiName);

        m_onButton.setTooltip("Turns " + name + " on or off. Off passes the voice through unchanged.");
        m_onButton.setTitle(name + " On");
        m_onButton.setColour(juce::ToggleButton::tickColourId, m_accent);
        addAndMakeVisible(m_onButton);
        m_onAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(m_owner.m_parameters, m_spec.onId, m_onButton);

        for (const char* id : m_spec.knobIds)
            if (id != nullptr)
                addKnob(id);

        if (m_spec.chipIds[0] != nullptr)
        {
            juce::StringArray tips;
            for (const char* id : m_spec.chipIds)
                if (id != nullptr)
                    tips.add(juce::String(Params::spec(id).name) + ": " + tooltipFor(id));
            m_modeChip.setTooltip(tips.joinIntoString("\n"));
            m_modeChip.setTitle(name + " Modes");
            m_modeChip.setColour(juce::PopupMenu::highlightedBackgroundColourId, m_accent); // the menu's accent
            m_modeChip.setColour(juce::TextButton::textColourOffId, text.withAlpha(0.85f));
            m_modeChip.onClick = [this] { m_owner.showModeMenu(m_module); };
            addAndMakeVisible(m_modeChip);
        }

        m_moveChip.setTooltip("Moves " + name + " in the chain, or turns it off. You can also drag the tile by its header, "
                              "or press Alt+Left or Alt+Right. The tile numbers count all fifteen modules; a host's "
                              "position parameters count the fourteen other than CHAMBER, so after CHAMBER they read one lower.");
        m_moveChip.setTitle("Move " + name);
        m_moveChip.setColour(juce::PopupMenu::highlightedBackgroundColourId, m_accent);
        m_moveChip.onClick = [this] { m_owner.showMoveMenu(m_module); };
        addAndMakeVisible(m_moveChip);

        m_dice.setTooltip("Rolls new settings for " + name + ": its knobs and modes, each inside its useful range. "
                          "Its On switch and its place in the chain stay as they are. " + diceUndoHint());
        m_dice.setTitle("Randomise " + name);
        m_dice.setAccent(m_accent);
        m_dice.setPillSize(c_diceSize);
        m_dice.onClick = [this] { if (m_owner.onRollModule) m_owner.onRollModule(m_module); };
        m_dice.onSecondaryClick = [this] { if (m_owner.onDiceMenu) m_owner.onDiceMenu(m_dice); };
        m_dice.onUndoKey = [this](bool redo) { return m_owner.onDiceUndoKey != nullptr && m_owner.onDiceUndoKey(redo); };
        addAndMakeVisible(m_dice);

        m_dragTip = "Drag the header to move " + name + " in the chain.";
        m_footerTip = footerTooltip(static_cast<Params::ModuleId>(m_module));
    }

    /** The drag hint over the header, what the readout shows over the footer, nothing elsewhere. */
    juce::String getTooltip() override
    {
        const auto p = getMouseXYRelative();
        if (p.y < c_headerHeight)
            return m_dragTip;
        if (footerBounds().expanded(0, 3).contains(p))
            return m_on ? m_footerTip : juce::String("Off: the module passes the sound through untouched.");
        return {};
    }

    static juce::String footerTooltip(Params::ModuleId m)
    {
        switch (m)
        {
            case Params::ModuleId::pitch:    return "SHIFT: the pitch shift applied right now, in semitones. With Snap or Mono it moves with the voice.";
            case Params::ModuleId::clean:    return "GATE: whether the gate is open or shut, with the gain reduction Contain applies right now (the lower bar).";
            case Params::ModuleId::deess:    return "GR: how far the sibilance is turned down right now.";
            case Params::ModuleId::tame:     return "GR: how far the harsh top end is held back right now.";
            case Params::ModuleId::detail:   return "LIFT: how far the quiet parts are lifted (+) and the loud parts held under the ceiling (-) right now.";
            case Params::ModuleId::vocoder:  return "The level of each vocoder band right now.";
            case Params::ModuleId::ring:     return "CARRIER: the ring carrier's frequency (with its ratio to the voice when Track is Follow). The dot blinks with the carrier, slowed until you can see it.";
            case Params::ModuleId::shimmer:  return "WOBBLE: where the shimmer's movement is in its cycle.";
            case Params::ModuleId::growl:    return "CHOP: how deep the growl is cutting into the voice right now.";
            case Params::ModuleId::swirl:    return "NOTCH: where the swirl's sweep is right now, from low to high.";
            case Params::ModuleId::roar:     return "HEAT: how hard the drive is working right now.";
            case Params::ModuleId::ensemble: return "WIDTH: how wide the ensemble makes the sound right now.";
            case Params::ModuleId::sub:      return "SUB: the level and frequency of the octave below right now.";
            case Params::ModuleId::overtone: return "BANDS: the centre and level of the two overtone bands right now (TRACK when they follow the voice). R: Replace, the original low end is taken out.";
            case Params::ModuleId::chamber:  return "The room in use and how long it rings (its decay with Resonance). The bar shows how strongly it is ringing right now.";
        }
        return {};
    }

    bool isRing() const { return m_module == static_cast<int>(Params::ModuleId::ring); }

    /** RING's second knob shows Freq (Track = Fixed) or Ratio (Track = Follow), in one place. */
    void showRingKnob(bool ratio)
    {
        if (!isRing() || m_knobs.size() < 3)
            return;
        auto& freq = *m_knobs[1];
        auto& rat = *m_knobs[2];
        if (rat.slider.isVisible() == ratio && freq.slider.isVisible() == !ratio)
            return;

        const bool hadFocus = freq.slider.hasKeyboardFocus(true) || rat.slider.hasKeyboardFocus(true);
        freq.slider.setVisible(!ratio);
        freq.caption.setVisible(!ratio);
        rat.slider.setVisible(ratio);
        rat.caption.setVisible(ratio);
        if (hadFocus)
            (ratio ? rat.slider : freq.slider).grabKeyboardFocus();
    }

    void setOn(bool on)
    {
        if (on == m_on)
            return;
        m_on = on;
        applyAlpha();
        repaint();
    }

    float tileAlpha() const override { return m_on ? 1.0f : c_offAlpha; }

    void applyAlpha() override
    {
        Tile::applyAlpha();
        m_modeChip.setAlpha(tileAlpha());
        m_dice.setAlpha(tileAlpha());   // still clickable, like the chip
    }

    void setSlot(int slot)
    {
        if (slot != m_slot)
        {
            m_slot = slot;
            repaint(0, 0, getWidth(), c_headerHeight);
        }
    }

    void setLifted(bool lifted)
    {
        if (lifted != m_lifted)
        {
            m_lifted = lifted;
            repaint();
        }
    }

    juce::Rectangle<int> footerBounds() const { return { 10, getHeight() - 6 - c_footerHeight, getWidth() - 20, c_footerHeight }; }
    /** The row above the footer: the mode chip, then the module's die at its right end. */
    juce::Rectangle<int> chipRowBounds() const
    {
        return footerBounds().translated(0, -(c_chipHeight + 4)).withHeight(c_chipHeight);
    }
    juce::Rectangle<int> chipBounds() const { return chipRowBounds().withTrimmedRight(c_diceSize + c_diceGap); }
    /** The die's clickable area: its pill at the right end of the chip row, and a margin around it. */
    juce::Rectangle<int> diceBounds() const { return chipRowBounds().removeFromRight(c_diceSize).withSizeKeepingCentre(c_diceHit, c_diceHit); }

    void resized() override
    {
        layoutControls();
        fitCaptions();
    }

    void layoutControls()
    {
        auto header = getLocalBounds().removeFromTop(c_headerHeight);
        m_onButton.setBounds(header.withX(7).withWidth(18).withSizeKeepingCentre(18, 18).withX(7));
        m_moveChip.setBounds(header.withTrimmedRight(c_pad).removeFromRight(26).withSizeKeepingCentre(26, 16));

        const auto chip = chipBounds();
        m_modeChip.setBounds(chip);
        m_dice.setBounds(diceBounds());

        auto body = getLocalBounds().withTop(c_headerHeight + 4).withBottom(chip.getY() - 2).reduced(6, 0);
        const int count = isRing() ? 2 : static_cast<int>(m_knobs.size());
        if (count == 1)
        {
            placeKnob(*m_knobs[0], body.withSizeKeepingCentre(body.getWidth() / 2, body.getHeight()));
            return;
        }

        if (count == 4)
        {
            // CHAMBER: four equal knobs.
            body = body.expanded(3, 0);
            const int w = body.getWidth() / 4;
            for (int k = 0; k < 4; ++k)
                placeKnob(*m_knobs[static_cast<size_t>(k)], k < 3 ? body.removeFromLeft(w) : body);
            return;
        }

        if (count == 3)
        {
            // PITCH: the shift is the main knob (and has the longest value); Size and Drive sit beside it.
            body = body.expanded(3, 0);
            const int main = body.getWidth() * 2 / 5;
            placeKnob(*m_knobs[0], body.removeFromLeft(main));
            placeKnob(*m_knobs[1], body.removeFromLeft(body.getWidth() / 2));
            placeKnob(*m_knobs[2], body);
            return;
        }

        const int w = body.getWidth() / count;
        for (int k = 0; k < static_cast<int>(m_knobs.size()); ++k)
        {
            const int cell = isRing() ? std::min(k, 1) : k;
            placeKnob(*m_knobs[static_cast<size_t>(k)], body.withX(body.getX() + cell * w).withWidth(w));
        }
    }

    void paint(juce::Graphics& g) override
    {
        paintPanel(g, m_lifted, 1.0f, false);

        auto header = getLocalBounds().removeFromTop(c_headerHeight).withTrimmedLeft(29).withTrimmedRight(c_pad + 30).toFloat();
        g.setFont(m_owner.m_fonts.label(c_smallFont));
        g.setColour(textFaint);
        g.drawText(juce::String(m_slot + 1).paddedLeft('0', 2), header.removeFromLeft(18.0f), juce::Justification::centredLeft, false);
        header.removeFromLeft(2.0f);
        g.setFont(m_owner.m_fonts.heading(11.0f));
        g.setColour(m_on ? m_accent : textDim);
        g.drawText(m_title, header, juce::Justification::centredLeft, false);

        if (m_spec.chipIds[0] == nullptr)
        {
            // No modes: a quiet note where the chip would be, so the tiles line up.
            g.setFont(m_owner.m_fonts.label(c_smallFont));
            g.setColour(textFaint.withMultipliedAlpha(m_on ? 1.0f : c_offAlpha));
            g.drawText("NO MODES", chipBounds().toFloat(), juce::Justification::centred, false);
        }

        paintFooter(g, footerBounds().toFloat());
    }

    void paintFooter(juce::Graphics& g, juce::Rectangle<float> area)
    {
        const auto captionFont = m_owner.m_fonts.label(c_smallFont);
        const auto valueFont = m_owner.m_fonts.body(c_smallFont);
        const auto& v = m_values;

        if (!m_on)
        {
            g.setFont(captionFont);
            g.setColour(textFaint);
            g.drawText("OFF", area, juce::Justification::centred, false);
            return;
        }

        const auto caption = [&](const juce::String& t)
        {
            g.setFont(captionFont);
            g.setColour(textFaint);
            g.drawText(t, area.removeFromLeft(std::max(30.0f, textWidth(captionFont, t) + 6.0f)), juce::Justification::centredLeft, false);
        };
        const auto value = [&](const juce::String& t, juce::Colour c = textDim)
        {
            g.setFont(valueFont);
            g.setColour(c);
            const float w = textWidth(valueFont, t) + 2.0f;
            g.drawText(t, area.removeFromRight(w), juce::Justification::centredRight, false);
            area.removeFromRight(6.0f);
        };
        const auto bar = [&]() { return area.withSizeKeepingCentre(area.getWidth(), 4.0f); };

        switch (static_cast<Params::ModuleId>(m_module))
        {
            case Params::ModuleId::pitch:
            {
                caption("SHIFT");
                value(signedReadout(v[0], 2) + " st");
                drawCentredBar(g, bar(), v[0] / 24.0f, m_accent);
                break;
            }
            case Params::ModuleId::clean:
            {
                caption("GATE");
                const bool open = v[0] > 0.5f;
                value(grText(v[1]));
                value(open ? "OPEN" : "SHUT", open ? textDim : textFaint);
                // The gate's opening, and under it Contain's gain reduction (0..-12 dB).
                const auto top = area.withSizeKeepingCentre(area.getWidth(), 9.0f);
                drawBar(g, top.withHeight(3.5f), v[0], m_accent);
                drawBar(g, top.withTrimmedTop(5.5f), -v[1] / 12.0f, m_accent.withAlpha(0.55f));
                break;
            }
            case Params::ModuleId::deess:
            case Params::ModuleId::tame:
            {
                caption("GR");
                value(grText(v[0]));
                drawBar(g, bar(), -v[0] / 12.0f, m_accent);
                break;
            }
            case Params::ModuleId::detail:
            {
                // Same rule as the creature view's DETAIL readout: no sign on a zero.
                caption("LIFT");
                value(signedReadout(std::min(std::abs(v[0]), 99.9f), 1, "+") + " / " + signedReadout(std::min(std::abs(v[1]), 99.9f), 1, "-") + " dB");
                const auto top = area.withSizeKeepingCentre(area.getWidth(), 9.0f);
                drawBar(g, top.withHeight(3.5f), std::abs(v[0]) / 24.0f, m_accent);
                drawBar(g, top.withTrimmedTop(5.5f), std::abs(v[1]) / 18.0f, m_accent.withAlpha(0.55f));
                break;
            }
            case Params::ModuleId::vocoder:
            {
                const int n = roar::MonsterFeatures::numBands;
                const float w = area.getWidth() / static_cast<float>(n);
                for (int b = 0; b < n; ++b)
                {
                    const float h = std::max(1.0f, area.getHeight() * juce::jlimit(0.0f, 1.0f, v[static_cast<size_t>(b)]));
                    g.setColour(b % 2 == 0 ? m_accent : m_accent.withAlpha(0.75f));
                    g.fillRect(juce::Rectangle<float>(area.getX() + w * static_cast<float>(b) + 0.5f, area.getBottom() - h, std::max(1.0f, w - 1.5f), h));
                }
                break;
            }
            case Params::ModuleId::ring:
            {
                caption("CARRIER");
                const bool follow = v[2] > 0.5f;
                const auto hz = v[0] > 0.0f ? hzText(v[0]) : juce::String("--");
                value(follow ? "x" + juce::String(v[3], 2) + " " + hz : hz, textDim);
                // The dot blinks at a slowed copy of the carrier (the rate halved until it is visible).
                const float d = 6.0f;
                const auto dot = juce::Rectangle<float>(d, d).withCentre({ area.getRight() - d * 0.5f, area.getCentreY() });
                g.setColour(outline);
                g.fillEllipse(dot);
                if (v[1] > 0.5f)
                {
                    g.setColour(m_accent);
                    g.fillEllipse(dot);
                    g.setColour(m_accent.withAlpha(0.25f));
                    g.fillEllipse(dot.expanded(2.5f));
                }
                break;
            }
            case Params::ModuleId::shimmer:
            {
                caption("WOBBLE");
                auto track = bar();
                drawTrack(g, track.withSizeKeepingCentre(track.getWidth(), 2.0f));
                const float x = track.getCentreX() + std::sin(v[0] * juce::MathConstants<float>::twoPi) * (track.getWidth() * 0.5f - 4.0f);
                g.setColour(m_accent);
                g.fillEllipse(juce::Rectangle<float>(7.0f, 7.0f).withCentre({ x, track.getCentreY() }));
                break;
            }
            case Params::ModuleId::growl:
            {
                caption("CHOP");
                value(juce::String(juce::roundToInt(std::abs(v[0]) * 100.0f)) + "%");
                drawCentredBar(g, bar(), v[0], m_accent);
                break;
            }
            case Params::ModuleId::swirl:
            {
                caption("NOTCH");
                auto track = bar();
                drawTrack(g, track);
                const float x = track.getX() + juce::jlimit(0.0f, 1.0f, v[0]) * track.getWidth();
                g.setColour(m_accent);
                g.fillRoundedRectangle(juce::Rectangle<float>(3.0f, 10.0f).withCentre({ x, track.getCentreY() }), 1.5f);
                break;
            }
            case Params::ModuleId::roar:
            {
                caption("HEAT");
                value(juce::String(juce::roundToInt(v[0] * 100.0f)) + "%");
                drawBar(g, bar(), v[0], m_accent);
                break;
            }
            case Params::ModuleId::ensemble:
            {
                caption("WIDTH");
                value(juce::String(juce::roundToInt(v[0] * 100.0f)) + "%");
                drawBar(g, bar(), v[0], m_accent);
                break;
            }
            case Params::ModuleId::sub:
            {
                caption("SUB");
                value(v[1] > 0.0f ? hzText(v[1]) : juce::String("--"));
                drawBar(g, bar(), v[0], m_accent);
                break;
            }
            case Params::ModuleId::overtone:
            {
                caption(v[4] > 0.5f ? "TRACK" : "BANDS");
                if (v[5] > 0.5f)
                {
                    // Replace: a small "R" badge, the dry low end is gone.
                    const auto badge = area.removeFromRight(13.0f).withSizeKeepingCentre(13.0f, 12.0f);
                    g.setColour(m_accent.withAlpha(0.25f));
                    g.fillRoundedRectangle(badge, 3.0f);
                    g.setColour(m_accent);
                    g.setFont(m_owner.m_fonts.heading(8.5f));
                    g.drawText("R", badge, juce::Justification::centred, false);
                    area.removeFromRight(4.0f);
                }
                juce::String centres = v[0] > 0.0f ? juce::String(juce::roundToInt(v[0])) : juce::String("--");
                if (v[1] > 0.0f)
                    centres << juce::String::fromUTF8("\xc2\xb7") << juce::roundToInt(v[1]);
                value(centres + " Hz");
                for (int b = 0; b < 2; ++b)
                {
                    const auto r = area.removeFromLeft(5.0f).withSizeKeepingCentre(5.0f, 12.0f);
                    area.removeFromLeft(3.0f);
                    g.setColour(outline);
                    g.fillRect(r);
                    const float h = r.getHeight() * juce::jlimit(0.0f, 1.0f, v[static_cast<size_t>(2 + b)]);
                    g.setColour(b == 1 && v[1] <= 0.0f ? outline : m_accent);
                    g.fillRect(r.withTop(r.getBottom() - h));
                }
                break;
            }
            case Params::ModuleId::chamber:
            {
                // The room (first word of its name), its decay, and a bar for the ring right now.
                const int room = juce::jlimit(0, static_cast<int>(Params::chamberRoomNames.size()) - 1, juce::roundToInt(v[2]));
                caption(juce::String(Params::chamberRoomNames[static_cast<size_t>(room)]).upToFirstOccurrenceOf(" ", false, false).toUpperCase());
                value(juce::String(v[1], 2) + " s");
                drawBar(g, bar(), v[0], m_accent);
                break;
            }
        }
    }

    /** New readout values (from the timer); repaints the footer when they changed. */
    void setValues(const std::array<float, 34>& values)
    {
        if (values != m_values)
        {
            m_values = values;
            repaint(footerBounds().expanded(2));
        }
    }

    // Dragging by the header
    void mouseMove(const juce::MouseEvent& e) override
    {
        setMouseCursor(e.y < c_headerHeight ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        m_pressInHeader = e.y < c_headerHeight && e.mods.isLeftButtonDown();
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (!m_pressInHeader)
            return;
        if (!m_owner.isDragging())
        {
            if (e.getDistanceFromDragStart() < 4)
                return;
            m_owner.beginDrag(m_module, e.getEventRelativeTo(&m_owner).getMouseDownPosition().toFloat());
        }
        if (m_owner.m_drag.module == m_module)
            m_owner.dragTo(e.getEventRelativeTo(&m_owner).position);
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        if (m_pressInHeader && m_owner.m_drag.module == m_module)
            m_owner.endDrag(true);
        m_pressInHeader = false;
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        if (key.getModifiers().isAltDown() && (key.isKeyCode(juce::KeyPress::leftKey) || key.isKeyCode(juce::KeyPress::rightKey)))
        {
            m_owner.applyMove(m_module, key.isKeyCode(juce::KeyPress::leftKey) ? moveEarlier : moveLater);
            return true;
        }
        return false;
    }

    const int m_module;
    const Params::ModuleSpec& m_spec;
    int m_slot = -1;
    bool m_on = true, m_lifted = false, m_pressInHeader = false;
    juce::String m_dragTip, m_footerTip;
    float m_blinkPhase = 0.0f;

    juce::ToggleButton m_onButton;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> m_onAttachment;
    juce::TextButton m_modeChip;
    MoveChip m_moveChip;
    DiceButton m_dice { "Randomise" };
    std::array<float, 34> m_values {};
};

//==============================================================================
class SignalFlowStrip::InTile : public Tile
{
public:
    explicit InTile(SignalFlowStrip& owner) : Tile(owner, "IN", juce::Colour(Params::accentIn))
    {
        addKnob(Params::inTrim);

        m_auto.setButtonText("AUTO");
        m_auto.setClickingTogglesState(true);
        m_auto.setTooltip(tooltipFor(Params::inAuto));
        m_auto.setTitle("Auto Level");
        m_auto.setColour(juce::TextButton::buttonOnColourId, m_accent.withAlpha(0.28f));
        m_auto.setColour(juce::TextButton::textColourOnId, text);
        addAndMakeVisible(m_auto);
        m_autoAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(m_owner.m_parameters, Params::inAuto, m_auto);
    }

    juce::Rectangle<float> meterBounds() const
    {
        return { static_cast<float>(getWidth()) * 0.5f + 4.0f, 174.0f, 10.0f, static_cast<float>(getHeight()) - 174.0f - 56.0f };
    }

    void resized() override
    {
        placeKnob(*m_knobs[0], { 6, c_headerHeight + 6, getWidth() - 12, c_captionHeight + 92 });
        m_auto.setBounds(juce::Rectangle<int>(getWidth() / 2 - 32, c_headerHeight + 6 + c_captionHeight + 92 + 6, 64, 22));
        fitCaptions();
    }

    void paint(juce::Graphics& g) override
    {
        paintPanel(g, false, 1.0f, true);

        // Input meter, -48..0 dBFS, with a scale on its left.
        const auto m = meterBounds();
        g.setFont(m_owner.m_fonts.label(c_smallFont));
        for (const int db : { 0, -12, -24, -36, -48 })
        {
            const float y = m.getY() + m.getHeight() * static_cast<float>(-db) / 48.0f;
            g.setColour(textFaint);
            g.drawText(juce::String(db), juce::Rectangle<float>(m.getX() - 34.0f, y - 6.0f, 28.0f, 12.0f), juce::Justification::centredRight, false);
            g.setColour(outlineSoft);
            g.fillRect(juce::Rectangle<float>(m.getX() - 4.0f, y - 0.5f, 3.0f, 1.0f));
        }
        g.setColour(outline);
        g.fillRoundedRectangle(m, 2.0f);
        const float db = m_level > 1.0e-6f ? 20.0f * std::log10(m_level) : -100.0f;
        const float f = juce::jlimit(0.0f, 1.0f, 1.0f + db / 48.0f);
        if (f > 0.0f)
        {
            g.setGradientFill(juce::ColourGradient(m_accent, 0.0f, m.getBottom(), output, 0.0f, m.getY(), false));
            g.fillRoundedRectangle(m.withTop(m.getBottom() - m.getHeight() * f), 2.0f);
        }

        // F0 readout.
        auto f0 = juce::Rectangle<float>(10.0f, static_cast<float>(getHeight()) - 46.0f, static_cast<float>(getWidth()) - 20.0f, 36.0f);
        g.setColour(outlineSoft);
        g.fillRect(f0.removeFromTop(1.0f));
        g.setFont(m_owner.m_fonts.label(c_smallFont));
        g.setColour(textFaint);
        g.drawText("PITCH", f0.removeFromTop(16.0f), juce::Justification::centredLeft, false);
        g.setFont(m_owner.m_fonts.body(12.0f));
        g.setColour(m_f0 > 0.0f ? text : textFaint);
        g.drawText(m_f0 > 0.0f ? juce::String(juce::roundToInt(m_f0)) + " Hz" : juce::String("--"), f0, juce::Justification::centredLeft, false);
        const auto dot = juce::Rectangle<float>(6.0f, 6.0f).withCentre({ f0.getRight() - 3.0f, f0.getCentreY() });
        g.setColour(m_f0 > 0.0f ? good : outline);
        g.fillEllipse(dot);
    }

    void update(float level, float f0)
    {
        const float roundedF0 = std::round(f0);
        if (std::abs(level - m_level) > 1.0e-4f || std::abs(roundedF0 - m_f0) > 0.5f)
        {
            m_level = level;
            m_f0 = roundedF0;
            repaint(meterBounds().toNearestInt().expanded(2));
            repaint(0, getHeight() - 48, getWidth(), 48);
        }
    }

    juce::TextButton m_auto;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> m_autoAttachment;
    float m_level = 0.0f, m_f0 = 0.0f;
};

//==============================================================================
class SignalFlowStrip::OutputTile : public Tile
{
public:
    explicit OutputTile(SignalFlowStrip& owner) : Tile(owner, "OUTPUT", juce::Colour(Params::accentOutput))
    {
        for (const char* id : Params::outputIds)
            if (std::string_view(id) != Params::outAuto)
                addKnob(id);

        m_auto.setButtonText("AUTO");
        m_auto.setClickingTogglesState(true);
        m_auto.setTooltip(tooltipFor(Params::outAuto));
        m_auto.setTitle("Auto Release");
        m_auto.setColour(juce::TextButton::buttonOnColourId, m_accent.withAlpha(0.28f));
        m_auto.setColour(juce::TextButton::textColourOnId, text);
        addAndMakeVisible(m_auto);
        m_autoAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(m_owner.m_parameters, Params::outAuto, m_auto);
    }

    static constexpr int c_rowHeight = c_captionHeight + 86;

    juce::Rectangle<int> meterArea() const
    {
        return { 14, c_headerHeight + 8 + 2 * c_rowHeight + 14, getWidth() - 28, 3 * 24 };
    }

    void resized() override
    {
        auto knobs = juce::Rectangle<int>(8, c_headerHeight + 6, getWidth() - 16, 2 * c_rowHeight + 4);
        const int w = knobs.getWidth() / 4;
        // Row 1: Push, Ceiling, Clip, Shape. Row 2: Attack, Release, the Auto switch, Mix.
        for (int k = 0; k < 4; ++k)
            placeKnob(*m_knobs[static_cast<size_t>(k)], { knobs.getX() + k * w, knobs.getY(), w, c_rowHeight });
        const int y2 = knobs.getY() + c_rowHeight + 4;
        placeKnob(*m_knobs[4], { knobs.getX(), y2, w, c_rowHeight });
        placeKnob(*m_knobs[5], { knobs.getX() + w, y2, w, c_rowHeight });
        placeKnob(*m_knobs[6], { knobs.getX() + 3 * w, y2, w, c_rowHeight });
        const int knobCentreY = y2 + c_captionHeight + (c_rowHeight - c_captionHeight - 20) / 2;
        m_auto.setBounds(juce::Rectangle<int>(knobs.getX() + 2 * w + (w - 54) / 2, knobCentreY - 11, 54, 22));
        fitCaptions();
    }

    void paint(juce::Graphics& g) override
    {
        paintPanel(g, false, 1.0f, true);

        auto area = meterArea().toFloat();
        g.setColour(outlineSoft);
        g.fillRect(juce::Rectangle<float>(12.0f, area.getY() - 8.0f, static_cast<float>(getWidth()) - 24.0f, 1.0f));

        const auto captionFont = m_owner.m_fonts.label(c_smallFont);
        const auto valueFont = m_owner.m_fonts.body(10.5f);
        const auto row = [&](const juce::String& caption, const juce::String& value, juce::Colour valueColour)
        {
            auto r = area.removeFromTop(24.0f);
            g.setFont(captionFont);
            g.setColour(textFaint);
            g.drawText(caption, r.removeFromLeft(40.0f), juce::Justification::centredLeft, false);
            g.setFont(valueFont);
            g.setColour(valueColour);
            g.drawText(value, r.removeFromRight(64.0f), juce::Justification::centredRight, false);
            r.removeFromRight(8.0f);
            return r.withSizeKeepingCentre(r.getWidth(), 6.0f);
        };

        // Output level, -36..0 dBFS, with the ceiling marked.
        {
            const float db = m_out > 1.0e-6f ? 20.0f * std::log10(m_out) : -100.0f;
            auto r = row("OUT", db > -99.0f ? (std::abs(db) < 0.05f ? juce::String("0.0") : juce::String(db, 1)) + " dB" : juce::String("--"), textDim);
            drawBar(g, r, 1.0f + db / 36.0f, master.withAlpha(0.85f));
            const float x = r.getX() + r.getWidth() * juce::jlimit(0.0f, 1.0f, 1.0f + m_ceiling / 36.0f);
            g.setColour(m_accent);
            g.fillRect(juce::Rectangle<float>(x - 1.0f, r.getY() - 3.0f, 2.0f, r.getHeight() + 6.0f));
        }
        // Gain reduction, 0..-12 dB, with a 1 s peak hold.
        {
            auto r = row("GR", grText(m_heldGr), m_heldGr < -0.05f ? text : textDim);
            drawBar(g, r, -m_gr / 12.0f, m_accent);
            if (m_heldGr < -0.05f)
            {
                const float x = r.getX() + r.getWidth() * juce::jlimit(0.0f, 1.0f, -m_heldGr / 12.0f);
                g.setColour(text);
                g.fillRect(juce::Rectangle<float>(x - 1.0f, r.getY() - 2.0f, 2.0f, r.getHeight() + 4.0f));
            }
        }
        // Clipped, 0..12 dB.
        {
            auto r = row("CLIP", m_clip < 0.05f ? juce::String("0.0 dB") : juce::String(m_clip, 1) + " dB", m_clip >= 0.05f ? text : textDim);
            drawBar(g, r, m_clip / 12.0f, Theme::Colours::roar);
        }
    }

    void update(float out, float gr, float clip, float ceiling, double nowMs)
    {
        gr = std::min(0.0f, gr);
        if (gr <= m_heldGr || nowMs - m_holdStartMs > c_grHoldSeconds * 1000.0)
        {
            m_heldGr = gr;
            m_holdStartMs = nowMs;
        }
        const std::array<float, 5> v { out, gr, clip, ceiling, m_heldGr };
        if (v != m_last)
        {
            m_last = v;
            m_out = out;
            m_gr = gr;
            m_clip = std::max(0.0f, clip);
            m_ceiling = ceiling;
            repaint(meterArea().expanded(4));
        }
    }

    juce::TextButton m_auto;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> m_autoAttachment;
    float m_out = 0.0f, m_gr = 0.0f, m_heldGr = 0.0f, m_clip = 0.0f, m_ceiling = -1.0f;
    double m_holdStartMs = 0.0;
    std::array<float, 5> m_last {};
};

//==============================================================================
SignalFlowStrip::SignalFlowStrip(juce::AudioProcessorValueTreeState& parameters, const roar::MonsterFeatures& features,
                                 const Theme::Fonts& fonts, roar::PositionWrites* positionWrites)
    : m_parameters(parameters), m_features(features), m_positionWrites(positionWrites), m_fonts(fonts)
{
    setTitle("Signal Chain");

    m_inTile = std::make_unique<InTile>(*this);
    addAndMakeVisible(*m_inTile);

    // ROLL ALL, before the tiles so a dragged tile passes over it.
    m_rollAll = std::make_unique<DiceButton>("Randomise All");
    m_rollAll->setTitle("Randomise All");
    m_rollAll->setTooltip("Rolls a new creature: turns three to seven modules on (the rest off) and gives every module's knobs "
                          "and modes new settings, each inside its useful range. The module order, IN and OUTPUT stay as they are, "
                          "so the level stays safe. " + diceUndoHint());
    m_rollAll->setAccent(output);
    m_rollAll->onClick = [this] { if (onRollAll) onRollAll(); };
    m_rollAll->onSecondaryClick = [this] { if (onDiceMenu) onDiceMenu(*m_rollAll); };
    m_rollAll->onUndoKey = [this](bool redo) { return onDiceUndoKey != nullptr && onDiceUndoKey(redo); };
    // The caption is part of the button: it clicks, hovers and shows the tooltip with the die.
    m_rollAll->setPillSize(c_rollAllSize);
    m_rollAll->setCaption(c_rollAllCaption, m_fonts.label(c_smallFont), c_captionHeight);
    addAndMakeVisible(*m_rollAll);

    for (int m = 0; m < Params::numModules; ++m)
    {
        m_tiles[static_cast<size_t>(m)] = std::make_unique<ModuleTile>(*this, m);
        addAndMakeVisible(*m_tiles[static_cast<size_t>(m)]);
    }

    m_outTile = std::make_unique<OutputTile>(*this);
    addAndMakeVisible(*m_outTile);

    m_shadow = std::make_unique<DragShadow>();
    addChildComponent(*m_shadow);

    m_order = getOrder();
    m_shown = m_order;
    m_split = stereoSplitSlot(m_order);
    timerCallback();
    startTimerHz(30);
}

SignalFlowStrip::~SignalFlowStrip()
{
    stopTimer();
    m_animationTimer.stopTimer();
}

//==============================================================================
juce::String SignalFlowStrip::tooltipFor(const char* paramId)
{
    const auto* c = findControl(paramId);
    return c != nullptr ? juce::String(c->tip) : juce::String();
}

float SignalFlowStrip::rawValue(const char* id) const
{
    const auto* p = m_parameters.getRawParameterValue(id);
    const float v = p != nullptr ? p->load(std::memory_order_relaxed) : Params::defaultOf(id);
    return std::isfinite(v) ? v : Params::defaultOf(id);
}

bool SignalFlowStrip::isModuleOn(int module) const
{
    return rawValue(Params::modules[static_cast<size_t>(module)].onId) >= 0.5f;
}

void SignalFlowStrip::setParameter(const char* id, float plainValue)
{
    if (auto* p = m_parameters.getParameter(id))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost(p->convertTo0to1(plainValue));
        p->endChangeGesture();
    }
}

juce::Component* SignalFlowStrip::getModuleTile(int module) const
{
    return juce::isPositiveAndBelow(module, Params::numModules) ? m_tiles[static_cast<size_t>(module)].get() : nullptr;
}

juce::Component* SignalFlowStrip::getInTile() const { return m_inTile.get(); }
juce::Component* SignalFlowStrip::getOutputTile() const { return m_outTile.get(); }

juce::Button* SignalFlowStrip::getModeChip(int module) const
{
    auto* t = m_tiles[static_cast<size_t>(module)].get();
    return t->m_spec.chipIds[0] != nullptr ? &t->m_modeChip : nullptr;
}

juce::Button* SignalFlowStrip::getMoveChip(int module) const { return &m_tiles[static_cast<size_t>(module)]->m_moveChip; }
juce::Button* SignalFlowStrip::getOnButton(int module) const { return &m_tiles[static_cast<size_t>(module)]->m_onButton; }

std::vector<std::pair<juce::Slider*, const char*>> SignalFlowStrip::getKnobs(juce::Component* tile) const
{
    std::vector<std::pair<juce::Slider*, const char*>> result;
    if (auto* t = dynamic_cast<Tile*>(tile))
        for (auto& k : t->m_knobs)
            result.emplace_back(&k->slider, k->paramId);
    return result;
}

//==============================================================================
Params::Order SignalFlowStrip::getOrder() const
{
    Params::Order positions {};
    for (int m = 0; m < Params::numModules; ++m)
        positions[static_cast<size_t>(m)] = juce::roundToInt(rawValue(Params::modules[static_cast<size_t>(m)].posId));
    return Params::resolveOrder(positions);
}

void SignalFlowStrip::setOrder(const Params::Order& order)
{
    // 1.0's modules: their place among those 14; CHAMBER: its slot (see Params::resolveOrder).
    const auto slotOf = Params::positionsFor(order);

    std::array<juce::RangedAudioParameter*, Params::numModules> changed {};
    for (int m = 0; m < Params::numModules; ++m)
    {
        auto* p = m_parameters.getParameter(Params::modules[static_cast<size_t>(m)].posId);
        if (p != nullptr && juce::roundToInt(p->convertFrom0to1(p->getValue())) != slotOf[static_cast<size_t>(m)])
            changed[static_cast<size_t>(m)] = p;
    }
    // The whole permutation is one write: the audio thread keeps the previous order until it is done.
    const roar::PositionWrites::Scope positionScope(m_positionWrites);
    for (auto* p : changed)
        if (p != nullptr)
            p->beginChangeGesture();
    for (int m = 0; m < Params::numModules; ++m)
        if (auto* p = changed[static_cast<size_t>(m)])
            p->setValueNotifyingHost(p->convertTo0to1(static_cast<float>(slotOf[static_cast<size_t>(m)])));
    for (auto* p : changed)
        if (p != nullptr)
            p->endChangeGesture();
}

Params::Order SignalFlowStrip::orderAfterMove(const Params::Order& order, int module, int item)
{
    if (item == resetOrder)
        return Params::referenceOrder();

    std::vector<int> list(order.begin(), order.end());
    const auto it = std::find(list.begin(), list.end(), module);
    if (it == list.end())
        return order;
    const int slot = static_cast<int>(it - list.begin());
    list.erase(it);

    int target = slot;
    if (item == moveToStart)
        target = 0;
    else if (item == moveEarlier)
        target = std::max(0, slot - 1);
    else if (item == moveLater)
        target = std::min(Params::numModules - 1, slot + 1);
    else if (item == moveToEnd)
        target = Params::numModules - 1;
    else if (item >= moveAfterBase && item < moveAfterBase + Params::numModules && item - moveAfterBase != module)
        target = static_cast<int>(std::find(list.begin(), list.end(), item - moveAfterBase) - list.begin()) + 1;
    else
        return order;

    list.insert(list.begin() + target, module);
    Params::Order result {};
    for (int k = 0; k < Params::numModules; ++k)
        result[static_cast<size_t>(k)] = list[static_cast<size_t>(k)];
    return result;
}

juce::PopupMenu SignalFlowStrip::buildMoveMenu(int module) const
{
    const auto order = getOrder();
    const int slot = static_cast<int>(std::find(order.begin(), order.end(), module) - order.begin());
    const int before = slot > 0 ? order[static_cast<size_t>(slot - 1)] : -1;

    juce::PopupMenu after;
    for (int k = 0; k < Params::numModules; ++k)
    {
        const int m = order[static_cast<size_t>(k)];
        if (m == module)
            continue;
        const juce::String name = juce::String(k + 1).paddedLeft('0', 2) + "  " + Params::modules[static_cast<size_t>(m)].uiName;
        after.addItem(moveAfterBase + m, name, m != before, m == before);
    }

    const bool isReference = order == Params::referenceOrder();

    juce::PopupMenu menu;
    menu.setLookAndFeel(&getLookAndFeel());
    menu.addSectionHeader(juce::String(Params::modules[static_cast<size_t>(module)].uiName) + " (" + juce::String(slot + 1) + " of "
                          + juce::String(Params::numModules) + ")");
    menu.addItem(moveToStart, "Move to Start", slot > 0);
    menu.addItem(moveEarlier, "Move Earlier", slot > 0);
    menu.addItem(moveLater, "Move Later", slot < Params::numModules - 1);
    menu.addItem(moveToEnd, "Move to End", slot < Params::numModules - 1);
    menu.addSubMenu("Move After", after);
    menu.addSeparator();
    menu.addItem(toggleOnOff, isModuleOn(module) ? "Turn Off" : "Turn On");
    menu.addItem(resetOrder, "Reset Chain Order", !isReference);
    return menu;
}

void SignalFlowStrip::applyMove(int module, int item)
{
    if (!juce::isPositiveAndBelow(module, Params::numModules))
        return;

    if (item == toggleOnOff)
    {
        setParameter(Params::modules[static_cast<size_t>(module)].onId, isModuleOn(module) ? 0.0f : 1.0f);
        return;
    }

    const auto order = getOrder();
    const auto next = orderAfterMove(order, module, item);
    if (next == order)
        return;

    if (isDragging())
        endDrag(false);
    setOrder(next);
    m_order = getOrder();
    m_shown = m_order;
    m_split = stereoSplitSlot(m_order);
    layoutTiles(true, c_slideMs);
    repaint();
}

juce::PopupMenu SignalFlowStrip::buildModeMenu(int module, std::vector<std::pair<const char*, int>>& itemMap) const
{
    const auto& spec = Params::modules[static_cast<size_t>(module)];
    itemMap.assign(1, { nullptr, 0 });   // item IDs start at 1

    int numChoices = 0;
    for (const char* id : spec.chipIds)
        numChoices += id != nullptr ? 1 : 0;

    juce::PopupMenu menu;
    menu.setLookAndFeel(&getLookAndFeel());

    for (const char* id : spec.chipIds)
    {
        if (id == nullptr)
            continue;
        const auto& s = Params::spec(id);
        const int current = juce::jlimit(0, s.numChoices - 1, juce::roundToInt(rawValue(id)));
        const auto caption = captionFor(id);

        if (numChoices == 1)
        {
            // One choice parameter: its items straight in the menu, under its name.
            menu.addSectionHeader(juce::String(spec.uiName) + " " + caption);
            for (int c = 0; c < s.numChoices; ++c)
            {
                menu.addItem(static_cast<int>(itemMap.size()), s.choices[c], true, c == current);
                itemMap.emplace_back(id, c);
            }
            break;
        }

        // Several: one submenu each, named with its current choice.
        juce::PopupMenu items;
        items.addSectionHeader(caption);
        for (int c = 0; c < s.numChoices; ++c)
        {
            items.addItem(static_cast<int>(itemMap.size()), s.choices[c], true, c == current);
            itemMap.emplace_back(id, c);
        }
        const auto title = caption.substring(0, 1) + caption.substring(1).toLowerCase();
        menu.addSubMenu(title + " (" + s.choices[current] + ")", items);
    }
    return menu;
}

juce::String SignalFlowStrip::modeChipText(int module, float maxWidth) const
{
    const auto& spec = Params::modules[static_cast<size_t>(module)];
    juce::StringArray full, shortened, compact;
    for (const char* id : spec.chipIds)
    {
        if (id == nullptr)
            continue;
        const auto& s = Params::spec(id);
        const int c = juce::jlimit(0, s.numChoices - 1, juce::roundToInt(rawValue(id)));
        const auto name = juce::String(s.choices[c]).toUpperCase();
        // RING's Body and CHAMBER's Boom say nothing while they are Off.
        if ((std::string_view(id) == Params::ringBody || std::string_view(id) == Params::chamberBoom) && c == 0)
            continue;
        full.add(name);
        shortened.add(shortChoice(name));
        compact.add(compactChoice(shortChoice(name)));
    }

    const auto separator = juce::String::fromUTF8(" \xc2\xb7 ");
    const auto font = m_fonts.label(9.0f);
    // Full names, then short ones, then short ones packed tight, then shorter ones packed tight (the
    // button cuts anything longer).
    const auto tight = juce::String::fromUTF8("\xc2\xb7");
    for (const auto& candidate : { full.joinIntoString(separator), shortened.joinIntoString(separator), shortened.joinIntoString(tight) })
        if (textWidth(font, candidate) <= maxWidth)
            return candidate;
    return compact.joinIntoString(tight);
}

juce::PopupMenu::Options SignalFlowStrip::menuOptions(juce::Component& target) const
{
    auto options = RoarschachLookAndFeel::menuOptionsFor(target);
    if (m_menuParent != nullptr)
        options = options.withParentComponent(m_menuParent);
    return options;
}

void SignalFlowStrip::showMoveMenu(int module)
{
    auto& chip = m_tiles[static_cast<size_t>(module)]->m_moveChip;
    buildMoveMenu(module).showMenuAsync(menuOptions(chip),
                                        [safe = juce::Component::SafePointer<SignalFlowStrip>(this), module](int item)
                                        {
                                            if (safe != nullptr && item > 0)
                                                safe->applyMove(module, item);
                                        });
}

void SignalFlowStrip::showModeMenu(int module)
{
    auto* chip = getModeChip(module);
    if (chip == nullptr)
        return;

    std::vector<std::pair<const char*, int>> map;
    auto menu = buildModeMenu(module, map);
    menu.showMenuAsync(menuOptions(*chip),
                       [safe = juce::Component::SafePointer<SignalFlowStrip>(this), map](int item)
                       {
                           if (safe != nullptr && item > 0 && item < static_cast<int>(map.size()) && map[static_cast<size_t>(item)].first != nullptr)
                               safe->setParameter(map[static_cast<size_t>(item)].first, static_cast<float>(map[static_cast<size_t>(item)].second));
                       });
}

//==============================================================================
int SignalFlowStrip::stereoSplitSlot(const Params::Order& order) const
{
    // The same rule as the chain: the first ENSEMBLE that is On, SHIMMER On in Stereo mode, or
    // CHAMBER On with some Width.
    for (int k = 0; k < Params::numModules; ++k)
    {
        const auto m = static_cast<Params::ModuleId>(order[static_cast<size_t>(k)]);
        if ((m == Params::ModuleId::ensemble && isModuleOn(static_cast<int>(m)))
            || (m == Params::ModuleId::shimmer && isModuleOn(static_cast<int>(m))
                && juce::roundToInt(rawValue(Params::shimmerMode)) == static_cast<int>(Params::ShimmerMode::stereo))
            || (m == Params::ModuleId::chamber && isModuleOn(static_cast<int>(m)) && rawValue(Params::chamberWidth) > 0.0f))
            return k;
    }
    return Params::numModules;
}

juce::Rectangle<int> SignalFlowStrip::getSlotBounds(int slot) const
{
    const auto area = getLocalBounds().withTrimmedLeft(c_inWidth + c_endGap).withTrimmedRight(c_outWidth + c_endGap);
    const int tileW = (area.getWidth() - (c_columns - 1) * c_gap) / c_columns;
    const int tileH = (area.getHeight() - c_rowGap) / 2;
    const int row = slot / c_columns, col = slot % c_columns;
    return { area.getX() + col * (tileW + c_gap), area.getY() + row * (tileH + c_rowGap), tileW, tileH };
}

int SignalFlowStrip::nearestSlot(juce::Point<float> centre) const
{
    int best = 0;
    float bestDistance = std::numeric_limits<float>::max();
    for (int k = 0; k < Params::numModules; ++k)
    {
        const float d = getSlotBounds(k).toFloat().getCentre().getDistanceSquaredFrom(centre);
        if (d < bestDistance)
        {
            bestDistance = d;
            best = k;
        }
    }
    return best;
}

juce::String SignalFlowStrip::diceUndoHint()
{
   #if JUCE_MAC
    return "Right-click any die to undo a roll (or press Cmd+Z straight after clicking it).";
   #else
    return "Right-click any die to undo a roll (or press Ctrl+Z straight after clicking it).";
   #endif
}

DiceButton* SignalFlowStrip::getModuleDice(int module) const
{
    return juce::isPositiveAndBelow(module, Params::numModules) ? &m_tiles[static_cast<size_t>(module)]->m_dice : nullptr;
}

void SignalFlowStrip::setDiceFace(int face)
{
    m_rollAll->setFace(face);
    for (auto& tile : m_tiles)
        tile->m_dice.setFace(face);
}

void SignalFlowStrip::resized()
{
    m_inTile->setBounds(getLocalBounds().removeFromLeft(c_inWidth));
    m_outTile->setBounds(getLocalBounds().removeFromRight(c_outWidth));
    {
        // The empty slot after the last module: the die above the wire into OUTPUT, its caption under it.
        const auto slot = getSlotBounds(Params::numModules);
        const int captionWidth = juce::roundToInt(std::ceil(juce::GlyphArrangement::getStringWidth(m_fonts.label(c_smallFont), c_rollAllCaption))) + 8;
        const int width = std::max(c_rollAllSize, captionWidth);
        m_rollAll->setBounds(slot.getCentreX() - width / 2, slot.getY() + 22, width,
                             c_rollAllSize + DiceButton::c_captionGap + c_captionHeight);
    }
    for (auto& a : m_animations)
        a.active = false;
    layoutTiles(false, 0.0);
}

void SignalFlowStrip::layoutTiles(bool animate, double durationMs)
{
    for (int k = 0; k < Params::numModules; ++k)
    {
        const int m = m_shown[static_cast<size_t>(k)];
        auto& tile = *m_tiles[static_cast<size_t>(m)];
        tile.setSlot(k);
        if (m == m_drag.module)
            continue;
        const auto target = getSlotBounds(k);
        if (animate && durationMs > 0.0 && !tile.getBounds().isEmpty())
            animateTile(m, target, durationMs);
        else
        {
            m_animations[static_cast<size_t>(m)].active = false;
            tile.setBounds(target);
        }
    }
}

void SignalFlowStrip::animateTile(int module, juce::Rectangle<int> target, double durationMs)
{
    auto& a = m_animations[static_cast<size_t>(module)];
    auto& tile = *m_tiles[static_cast<size_t>(module)];
    if (a.active && a.to.toNearestInt() == target)
        return;
    if (!a.active && tile.getBounds() == target)
        return;

    a.from = tile.getBounds().toFloat();
    a.to = target.toFloat();
    a.startMs = juce::Time::getMillisecondCounterHiRes();
    a.durationMs = durationMs;
    a.active = true;
    if (!m_animationTimer.isTimerRunning())
        m_animationTimer.startTimerHz(60);
}

void SignalFlowStrip::stepAnimations()
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    bool any = false;
    for (int m = 0; m < Params::numModules; ++m)
    {
        auto& a = m_animations[static_cast<size_t>(m)];
        if (!a.active)
            continue;
        const float t = static_cast<float>(juce::jlimit(0.0, 1.0, (now - a.startMs) / std::max(1.0, a.durationMs)));
        const float e = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);   // ease out
        const auto r = juce::Rectangle<float>(a.from.getX() + (a.to.getX() - a.from.getX()) * e,
                                              a.from.getY() + (a.to.getY() - a.from.getY()) * e,
                                              a.to.getWidth(), a.to.getHeight());
        m_tiles[static_cast<size_t>(m)]->setBounds(r.toNearestInt());
        a.active = t < 1.0f;
        any = any || a.active;
    }
    if (!any)
        m_animationTimer.stopTimer();
}

void SignalFlowStrip::finishAnimations()
{
    for (int m = 0; m < Params::numModules; ++m)
    {
        auto& a = m_animations[static_cast<size_t>(m)];
        if (a.active)
            m_tiles[static_cast<size_t>(m)]->setBounds(a.to.toNearestInt());
        a.active = false;
    }
    m_animationTimer.stopTimer();
}

//==============================================================================
void SignalFlowStrip::beginDrag(int module, juce::Point<float> position)
{
    if (isDragging() || !juce::isPositiveAndBelow(module, Params::numModules))
        return;

    auto& tile = *m_tiles[static_cast<size_t>(module)];
    m_animations[static_cast<size_t>(module)].active = false;
    m_drag.module = module;
    m_drag.grabOffset = position - tile.getPosition().toFloat();
    m_drag.slot = static_cast<int>(std::find(m_shown.begin(), m_shown.end(), module) - m_shown.begin());

    tile.toFront(false);
    tile.setLifted(true);
    m_shadow->setBounds(tile.getBounds().expanded(DragShadow::c_margin));
    m_shadow->setVisible(true);
    m_shadow->toBehind(&tile);

    // Escape cancels: the strip takes the keyboard focus while the tile is held.
    setWantsKeyboardFocus(true);
    grabKeyboardFocus();
    repaint();
}

void SignalFlowStrip::dragTo(juce::Point<float> position)
{
    if (!isDragging())
        return;

    auto& tile = *m_tiles[static_cast<size_t>(m_drag.module)];
    const auto area = getLocalBounds().toFloat();
    auto topLeft = position - m_drag.grabOffset;
    topLeft.x = juce::jlimit(area.getX() - static_cast<float>(tile.getWidth()) * 0.5f, area.getRight() - static_cast<float>(tile.getWidth()) * 0.5f, topLeft.x);
    topLeft.y = juce::jlimit(area.getY() - static_cast<float>(tile.getHeight()) * 0.5f, area.getBottom() - static_cast<float>(tile.getHeight()) * 0.5f, topLeft.y);
    tile.setTopLeftPosition(topLeft.toInt());
    m_shadow->setBounds(tile.getBounds().expanded(DragShadow::c_margin));

    const int slot = nearestSlot(tile.getBounds().toFloat().getCentre());
    if (slot != m_drag.slot)
    {
        m_drag.slot = slot;
        std::vector<int> list;
        for (const int m : m_order)
            if (m != m_drag.module)
                list.push_back(m);
        list.insert(list.begin() + slot, m_drag.module);
        for (int k = 0; k < Params::numModules; ++k)
            m_shown[static_cast<size_t>(k)] = list[static_cast<size_t>(k)];
        layoutTiles(true, c_slideMs);
        repaint();
    }
}

void SignalFlowStrip::endDrag(bool commit)
{
    if (!isDragging())
        return;

    const int module = m_drag.module;
    if (commit && m_shown != m_order)
        setOrder(m_shown);   // the whole permutation, one gesture

    m_order = getOrder();
    m_shown = m_order;
    m_split = stereoSplitSlot(m_order);
    m_drag = {};

    auto& tile = *m_tiles[static_cast<size_t>(module)];
    // A drag ended while the button is still down (Escape, or the host changed the order) stays
    // ended until the button is released: the next mouse move must not pick the tile up again,
    // and the release must not commit the order that was cancelled.
    tile.m_pressInHeader = false;
    tile.setLifted(false);
    m_shadow->setVisible(false);
    layoutTiles(true, c_slideMs);

    setWantsKeyboardFocus(false);
    if (hasKeyboardFocus(false))
        tile.m_moveChip.grabKeyboardFocus();
    repaint();
}

bool SignalFlowStrip::keyPressed(const juce::KeyPress& key)
{
    if (isDragging() && key.isKeyCode(juce::KeyPress::escapeKey))
    {
        endDrag(false);
        return true;
    }
    return false;
}

//==============================================================================
void SignalFlowStrip::paint(juce::Graphics& g)
{
    // Connectors: IN -> slot 0, along each row, the return from the end of row 1 to the start of
    // row 2, and the last slot -> OUTPUT. They follow the slots (not the tiles), so they hold still
    // while tiles slide.
    const auto chevron = [&g](juce::Point<float> c, juce::Colour colour)
    {
        juce::Path p;
        p.startNewSubPath(c.x - 2.0f, c.y - 4.0f);
        p.lineTo(c.x + 2.0f, c.y);
        p.lineTo(c.x - 2.0f, c.y + 4.0f);
        g.setColour(colour);
        g.strokePath(p, juce::PathStrokeType(1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    };

    const auto first = getSlotBounds(0).toFloat();
    const auto rowEnd = getSlotBounds(c_columns - 1).toFloat();
    const auto rowStart = getSlotBounds(c_columns).toFloat();
    const auto last = getSlotBounds(Params::numModules - 1).toFloat();
    const float halfEnd = static_cast<float>(c_endGap) * 0.5f;

    g.setColour(outline);
    juce::Path wire;
    // IN into slot 0
    wire.startNewSubPath(static_cast<float>(c_inWidth), first.getCentreY());
    wire.lineTo(first.getX() - 3.0f, first.getCentreY());
    // the return: out of the end of row 1, down into the row gap, back, and into row 2
    const float midY = (rowEnd.getBottom() + rowStart.getY()) * 0.5f;
    wire.startNewSubPath(rowEnd.getRight() + 1.0f, rowEnd.getCentreY());
    wire.lineTo(rowEnd.getRight() + halfEnd, rowEnd.getCentreY());
    wire.lineTo(rowEnd.getRight() + halfEnd, midY);
    wire.lineTo(rowStart.getX() - halfEnd, midY);
    wire.lineTo(rowStart.getX() - halfEnd, rowStart.getCentreY());
    wire.lineTo(rowStart.getX() - 3.0f, rowStart.getCentreY());
    // the last slot into OUTPUT
    wire.startNewSubPath(last.getRight() + 1.0f, last.getCentreY());
    wire.lineTo(static_cast<float>(getWidth() - c_outWidth), last.getCentreY());
    g.strokePath(wire, juce::PathStrokeType(1.0f, juce::PathStrokeType::mitered, juce::PathStrokeType::butt));

    // While a tile is held the chevrons give way to the drop markers (below).
    if (!isDragging())
    {
        chevron({ first.getX() - halfEnd + 1.0f, first.getCentreY() }, mark);
        chevron({ rowStart.getX() - halfEnd + 4.0f, rowStart.getCentreY() }, mark);
        chevron({ last.getRight() + halfEnd, last.getCentreY() }, mark);
        for (int k = 0; k + 1 < Params::numModules; ++k)
        {
            if (k % c_columns == c_columns - 1)
                continue;
            const auto r = getSlotBounds(k).toFloat();
            chevron({ r.getRight() + static_cast<float>(c_gap) * 0.5f, r.getCentreY() }, mark);
        }
    }

    // While a tile is held: a drop marker in every gap and at both ends of each row (the two
    // either side of the landing slot lit), and the landing slot outlined in the tile's accent.
    if (isDragging())
    {
        const auto accent = m_tiles[static_cast<size_t>(m_drag.module)]->m_accent;
        const auto marker = [&](int slot, bool before, bool lit)
        {
            const auto r = getSlotBounds(slot).toFloat();
            const int col = slot % c_columns;
            const bool rowEdge = before ? col == 0 : (col == c_columns - 1 || slot == Params::numModules - 1);
            const float half = (rowEdge ? static_cast<float>(c_endGap) : static_cast<float>(c_gap)) * 0.5f;
            const float x = before ? r.getX() - half : r.getRight() + half;
            const auto tick = juce::Rectangle<float>(lit ? 3.0f : 2.0f, r.getHeight() * 0.56f).withCentre({ x, r.getCentreY() });
            g.setColour(lit ? accent.withAlpha(0.9f) : mark.withAlpha(0.55f));
            g.fillRoundedRectangle(tick, 1.0f);
        };
        for (int k = 0; k < Params::numModules; ++k)
        {
            marker(k, true, k == m_drag.slot);
            const int col = k % c_columns;
            // The gap after a slot is the next slot's gap before, except at a row's end.
            if (col == c_columns - 1 || k == Params::numModules - 1)
                marker(k, false, k == m_drag.slot);
        }
        if (m_drag.slot % c_columns != c_columns - 1 && m_drag.slot + 1 < Params::numModules)
            marker(m_drag.slot + 1, true, true);

        const auto slot = getSlotBounds(m_drag.slot).toFloat().reduced(1.5f);
        g.setColour(accent.withAlpha(0.08f));
        g.fillRoundedRectangle(slot, c_radius);
        juce::Path outlinePath;
        outlinePath.addRoundedRectangle(slot, c_radius);
        juce::Path dashed;
        const float dashes[] { 6.0f, 4.0f };
        juce::PathStrokeType(1.5f).createDashedStroke(dashed, outlinePath, dashes, 2);
        g.setColour(accent.withAlpha(0.7f));
        g.fillPath(dashed);
    }
}

void SignalFlowStrip::paintOverChildren(juce::Graphics& g)
{
    // The ST badge on the connector out of the module that makes the chain stereo.
    if (m_split >= Params::numModules || isDragging())
        return;

    const auto r = getSlotBounds(m_split).toFloat();
    juce::Point<float> at;
    if (m_split == Params::numModules - 1)
        at = { r.getRight() + static_cast<float>(c_endGap) * 0.5f, r.getCentreY() + 16.0f };
    else if (m_split % c_columns == c_columns - 1)
        at = { r.getRight() + static_cast<float>(c_endGap) * 0.5f, r.getBottom() + static_cast<float>(c_rowGap) * 0.5f };
    else
        at = { r.getRight() + static_cast<float>(c_gap) * 0.5f, r.getCentreY() + 16.0f };

    const auto badge = juce::Rectangle<float>(20.0f, 12.0f).withCentre(at);
    g.setColour(background);
    g.fillRoundedRectangle(badge.expanded(1.5f), 4.0f);
    g.setColour(output.withAlpha(0.22f));
    g.fillRoundedRectangle(badge, 3.0f);
    g.setColour(output.withAlpha(0.8f));
    g.drawRoundedRectangle(badge.reduced(0.5f), 3.0f, 1.0f);
    g.setFont(m_fonts.heading(7.5f));
    g.setColour(output);
    g.drawText("ST", badge.translated(0.5f, 0.0f), juce::Justification::centred, false);
}

//==============================================================================
void SignalFlowStrip::updateKnobUse(int module)
{
    // Knobs the current settings do not use are dimmed (they still turn). Each case here is an exact
    // null in the DSP (the module's output does not change with the knob), except those marked weak.
    using Use = Tile::KnobUse;
    auto& tile = *m_tiles[static_cast<size_t>(module)];
    const auto unusedAtZero = [&](const char* amountId, std::initializer_list<const char*> knobs)
    {
        const auto use = rawValue(amountId) <= 0.0f ? Use::none : Use::full;
        for (const char* id : knobs)
            tile.setKnobUse(id, use);
        return use;
    };

    switch (static_cast<Params::ModuleId>(module))
    {
        case Params::ModuleId::growl:
        {
            // Throat follows the voice, so Rate is not used; at Depth 0 nothing is chopped.
            const bool throat = juce::roundToInt(rawValue(Params::growlShape)) == static_cast<int>(Params::GrowlShape::throat);
            tile.setKnobUse(Params::growlRate, throat || rawValue(Params::growl) <= 0.0f ? Use::none : Use::full);
            break;
        }
        case Params::ModuleId::shimmer:  unusedAtZero(Params::shimmer, { Params::shimmerRate }); break;
        case Params::ModuleId::swirl:    unusedAtZero(Params::swirl, { Params::swirlRate }); break;
        case Params::ModuleId::ensemble: unusedAtZero(Params::ensemble, { Params::ensembleWidth }); break;
        case Params::ModuleId::ring:     unusedAtZero(Params::ring, { Params::ringFreq, Params::ringRatio }); break;
        case Params::ModuleId::sub:      unusedAtZero(Params::sub, { Params::subDrive }); break;
        case Params::ModuleId::overtone:
        {
            // At Amount 0 nothing is generated. In Lean and Replace the low end still passes through
            // the split that Freq moves, which turns its phase a little and nothing else (weak).
            const bool add = juce::roundToInt(rawValue(Params::overtoneMode)) == 0;   // Add, Lean, Replace
            tile.setKnobUse(Params::overtoneFreq, rawValue(Params::overtone) > 0.0f ? Use::full : add ? Use::none : Use::weak);
            break;
        }
        case Params::ModuleId::chamber:
        {
            // With no room blended in, Reson and Edge have nothing to work on (Width still decides
            // where the chain turns stereo, so it stays lit). Edge is the speaker's breakup inside the
            // room's share, so it does little until Amount is high (weak).
            const auto use = unusedAtZero(Params::chamber, { Params::chamberResonance });
            tile.setKnobUse(Params::chamberDrive, use == Use::none ? Use::none
                                                 : rawValue(Params::chamber) < c_chamberEdgeFrom ? Use::weak : Use::full);
            break;
        }
        case Params::ModuleId::pitch:
        case Params::ModuleId::clean:
        case Params::ModuleId::deess:
        case Params::ModuleId::detail:
        case Params::ModuleId::vocoder:
        case Params::ModuleId::roar:
        case Params::ModuleId::tame:
            break;
    }
}

//==============================================================================
void SignalFlowStrip::timerCallback()
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    const float dt = m_lastTickMs > 0.0 ? static_cast<float>(juce::jlimit(0.0, 0.2, (now - m_lastTickMs) / 1000.0)) : 0.0f;
    m_lastTickMs = now;

    // Positions: the host, a preset or another editor may have changed them. Follow, never write.
    const auto order = getOrder();
    const int split = stereoSplitSlot(order);
    if (order != m_order)
    {
        if (isDragging())
            endDrag(false);
        m_order = order;
        m_shown = order;
        layoutTiles(true, c_settleMs);
        repaint();
    }
    if (split != m_split)
    {
        m_split = split;
        repaint();
    }

    const auto& f = m_features;
    for (int m = 0; m < Params::numModules; ++m)
    {
        auto& tile = *m_tiles[static_cast<size_t>(m)];
        tile.setOn(isModuleOn(m));
        updateKnobUse(m);

        if (tile.m_modeChip.isVisible())
        {
            // The LookAndFeel narrows its padding for a label that needs the room: keep 5 px each side.
            const auto t = modeChipText(m, static_cast<float>(tile.m_modeChip.getWidth() - 10));
            if (t != tile.m_modeChip.getButtonText())
                tile.m_modeChip.setButtonText(t);
        }

        std::array<float, 34> v {};
        switch (static_cast<Params::ModuleId>(m))
        {
            case Params::ModuleId::pitch:    v[0] = std::round(load(f.pitchSt) * 100.0f) / 100.0f; break;
            case Params::ModuleId::clean:    v[0] = juce::jlimit(0.0f, 1.0f, load(f.gateOpen));
                                             v[1] = std::round(load(f.cleanGrDb) * 10.0f) / 10.0f; break;
            case Params::ModuleId::deess:    v[0] = std::round(load(f.deessGrDb) * 10.0f) / 10.0f; break;
            case Params::ModuleId::tame:     v[0] = std::round(load(f.tameGrDb) * 10.0f) / 10.0f; break;
            case Params::ModuleId::detail:   v[0] = std::round(load(f.detailBoostDb) * 10.0f) / 10.0f;
                                             v[1] = std::round(load(f.detailCutDb) * 10.0f) / 10.0f; break;
            case Params::ModuleId::vocoder:
                for (int b = 0; b < roar::MonsterFeatures::numBands; ++b)
                    v[static_cast<size_t>(b)] = std::round(juce::jlimit(0.0f, 1.0f, load(f.vocoderBands[static_cast<size_t>(b)])) * 32.0f) / 32.0f;
                break;
            case Params::ModuleId::ring:
            {
                const bool follow = juce::roundToInt(rawValue(Params::ringTrack)) == 1;
                tile.showRingKnob(follow);
                const float hz = load(f.ringHz);
                // The dot blinks at the carrier halved until it is 8 Hz or less (a visible copy of it).
                float rate = hz;
                while (rate > 8.0f)
                    rate *= 0.5f;
                tile.m_blinkPhase = std::fmod(tile.m_blinkPhase + dt * std::max(rate, 0.0f), 1.0f);
                v[0] = std::round(hz);
                v[1] = hz > 0.0f && tile.m_blinkPhase < 0.5f ? 1.0f : 0.0f;
                v[2] = follow ? 1.0f : 0.0f;
                v[3] = rawValue(Params::ringRatio);
                break;
            }
            case Params::ModuleId::shimmer:  v[0] = load(f.shimmerPhase); break;
            case Params::ModuleId::growl:    v[0] = std::round(load(f.growlMod) * 50.0f) / 50.0f; break;
            case Params::ModuleId::swirl:    v[0] = std::round(load(f.swirlNotch) * 100.0f) / 100.0f; break;
            case Params::ModuleId::roar:     v[0] = std::round(load(f.roarHeat) * 100.0f) / 100.0f; break;
            case Params::ModuleId::ensemble: v[0] = std::round(load(f.ensembleWidth) * 100.0f) / 100.0f; break;
            case Params::ModuleId::sub:      v[0] = std::round(load(f.subLevel) * 50.0f) / 50.0f;
                                             v[1] = std::round(load(f.subHz)); break;
            case Params::ModuleId::overtone:
                v[0] = std::round(load(f.overtoneBandHz[0]));
                v[1] = std::round(load(f.overtoneBandHz[1]));
                v[2] = std::round(load(f.overtoneBandLevel[0]) * 24.0f) / 24.0f;
                v[3] = std::round(load(f.overtoneBandLevel[1]) * 24.0f) / 24.0f;
                v[4] = juce::roundToInt(rawValue(Params::overtonePattern)) == 5 ? 1.0f : 0.0f;
                v[5] = juce::roundToInt(rawValue(Params::overtoneMode)) == 2 ? 1.0f : 0.0f;
                break;
            case Params::ModuleId::chamber:
                v[0] = std::round(juce::jlimit(0.0f, 1.0f, load(f.chamberRing)) * 50.0f) / 50.0f;
                v[1] = std::round(load(f.chamberDecay) * 100.0f) / 100.0f;
                v[2] = static_cast<float>(juce::roundToInt(rawValue(Params::chamberRoom)));
                break;
        }
        tile.setValues(v);
    }

    // OUTPUT: Shape shapes the clipper, which is out at Clip 0; with Auto on, Attack changes little.
    m_outTile->setKnobUse(Params::outClipShape, rawValue(Params::outClip) <= 0.0f ? Tile::KnobUse::none : Tile::KnobUse::full);
    m_outTile->setKnobUse(Params::outAttack, rawValue(Params::outAuto) >= 0.5f ? Tile::KnobUse::weak : Tile::KnobUse::full);

    const float f0 = load(f.f0Hz);
    m_inTile->update(juce::jlimit(0.0f, 4.0f, load(f.inputLevel)), load(f.voiced) > 0.5f && f0 > 0.0f && f0 < 20000.0f ? f0 : 0.0f);
    m_outTile->update(juce::jlimit(0.0f, 4.0f, load(f.outputLevel)), std::round(load(f.limiterGrDb) * 10.0f) / 10.0f,
                      std::round(load(f.clipDb) * 10.0f) / 10.0f, rawValue(Params::outCeiling), now);
}
