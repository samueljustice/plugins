#include "LayerPages.h"
#include "LookAndFeel.h"
#include "OffsetMeter.h"
#include "PluginProcessor.h"

#include <array>
#include <initializer_list>
#include <utility>

namespace
{
    using APVTS = juce::AudioProcessorValueTreeState;
    using DL = DetailLayer;

    const juce::String c_tipBreak = "\n";

    /** The name of a box, in small type over it: a box shows only what it is set to ("Sine", "1 Peak", "Off"). */
    class BoxCaption final : public juce::Component
    {
    public:
        BoxCaption(const Theme::Fonts& fonts, juce::String text) : m_fonts(fonts), m_text(std::move(text)) { setInterceptsMouseClicks(false, false); }

        void setResting(bool resting) { if (resting != m_resting) { m_resting = resting; repaint(); } }

        void paint(juce::Graphics& g) override
        {
            g.setColour(m_resting ? Theme::Colours::textFaint : Theme::Colours::textDim);
            Theme::fitText(g, m_text, getLocalBounds().toFloat().reduced(2.0f, 0.0f), m_fonts.caption(), juce::Justification::centredLeft);
        }

    private:
        const Theme::Fonts& m_fonts;
        const juce::String m_text;
        bool m_resting = false;
    };

    /** What a page has in common: making its controls, reading its parameters, and the resting look. */
    class Page : public LayerPage
    {
    public:
        explicit Page(const Context& c) : LayerPage(c)
        {
            // Only its controls take clicks: under a Ride page the panel's own row of keys (Auto Ride, Ride Mode,
            // Throw) stays in reach.
            setInterceptsMouseClicks(false, true);
        }

    protected:
        APVTS& state() const { return context.processor.getState(); }
        float value(const char* id) const
        {
            auto* raw = state().getRawParameterValue(id);
            return raw != nullptr ? raw->load() : 0.0f;
        }
        int choice(const char* id) const { return juce::roundToInt(value(id)); }
        bool isOn(const char* id) const { return value(id) > 0.5f; }

        /** The heading every tooltip on this page starts with: the layer's title ("Bus A / Swirl", "Ride / Swell"). */
        juce::String group() const { return Layers::title(context.target); }

        /** A Secondary wheel bound to `id`, with its caption and its tooltip (the heading, then tipFor). */
        WheelKnob& wheel(const char* id, const juce::String& caption, juce::String heading = {})
        {
            return adopt(std::make_unique<WheelKnob>(context.fonts, context.accent, WheelKnob::Size::Secondary), id, caption, std::move(heading));
        }
        WheelKnob& adopt(std::unique_ptr<WheelKnob> knob, const char* id, const juce::String& caption, juce::String heading = {})
        {
            knob->caption = caption;
            addAndMakeVisible(*knob);
            m_sliders.push_back(std::make_unique<APVTS::SliderAttachment>(state(), id, *knob));
            if (auto* param = state().getParameter(id))
            {
                knob->setDoubleClickReturnValue(true, param->convertFrom0to1(param->getDefaultValue()));
                knob->setTypedParameter(param, context.fonts);
            }
            // (A page's wheels take no wire, but for Varispeed, which says so itself.)
            knob->getProperties().set("howTo", "Double-click to reset it. Right-click (or Alt-double-click) to type a value.");
            m_tips.push_back({ knob.get(), heading.isNotEmpty() ? heading : group() + ": " + caption, Params::tipFor(id), {} });
            m_wheels.push_back(std::move(knob));
            return *m_wheels.back();
        }

        /** A box of choices bound to `id`, named `name` (its menu's heading, and the end of its tooltip's). */
        juce::ComboBox& box(const char* id, const char* const* names, int count, const juce::String& name)
        {
            const juce::String heading = group() + ": " + name;
            auto b = std::make_unique<juce::ComboBox>();
            b->addSectionHeading(name);
            for (int i = 0; i < count; ++i)
                b->addItem(names[i], i + 1);
            b->setWantsKeyboardFocus(false);
            b->setTitle(heading);
            addAndMakeVisible(*b);
            m_boxAttachments.push_back(std::make_unique<APVTS::ComboBoxAttachment>(state(), id, *b));
            m_tips.push_back({ b.get(), heading, Params::tipFor(id), {} });
            m_boxes.push_back(std::move(b));
            return *m_boxes.back();
        }

        /** The box's name in small type, to go over it (placeCaptioned). */
        void caption(juce::ComboBox& b, const juce::String& text)
        {
            auto c = std::make_unique<BoxCaption>(context.fonts, text);
            addChildComponent(*c);
            m_captions.push_back({ std::move(c), &b });
        }

        /** A key that switches `id` on and off, in the panel's colour when on. */
        juce::TextButton& key(const char* id, const juce::String& text)
        {
            const juce::String heading = group() + ": " + text;
            auto k = std::make_unique<juce::TextButton>(text);
            k->setClickingTogglesState(true);
            k->setColour(juce::TextButton::buttonOnColourId, context.accent);
            k->setWantsKeyboardFocus(false);
            k->setTitle(heading);
            addAndMakeVisible(*k);
            m_keyAttachments.push_back(std::make_unique<APVTS::ButtonAttachment>(state(), id, *k));
            m_tips.push_back({ k.get(), heading, Params::tipFor(id), {} });
            m_keys.push_back(std::move(k));
            return *m_keys.back();
        }

        /** Says why `c` does nothing now (an "It rests: ..." sentence), or that it works (empty): its look and its
            tooltip follow. Cheap when nothing changed. */
        void rest(juce::Component& c, const juce::String& reason)
        {
            for (auto& t : m_tips)
                if (t.component == &c)
                {
                    if (t.reason == reason && c.getProperties().contains("tipSet"))
                        return;
                    t.reason = reason;
                    c.getProperties().set("tipSet", true);
                    Theme::setLook(c, reason.isEmpty() ? Theme::Look::Normal : Theme::Look::Resting);
                    for (auto& cap : m_captions)
                        if (cap.second == &c)
                            cap.first->setResting(reason.isNotEmpty());
                    if (auto* client = dynamic_cast<juce::SettableTooltipClient*>(&c))
                        client->setTooltip(t.heading + c_tipBreak + t.body + (reason.isNotEmpty() ? c_tipBreak + reason : juce::String()));
                    return;
                }
        }

        /** A wheel in the index-th cell: the cell's whole width (room for its caption and value), the grid's height. */
        void placeWheel(juce::Component& c, int index) const
        {
            const auto cell = DL::cell(context.target, index);
            const auto wb = DL::wheelBox(context.target, index);
            c.setBounds(cell.getX(), wb.getY(), cell.getWidth(), wb.getHeight());
        }

        /** The choices column, top to bottom: `rows` boxes or keys, the ones in `captioned` with a line for their
            name over them, the stack centred in `height`. Each row's place. */
        std::vector<juce::Rectangle<int>> stack(int rows, std::initializer_list<int> captioned, int height = DL::c_rowHeight) const
        {
            auto has = [&captioned](int row) { return std::find(captioned.begin(), captioned.end(), row) != captioned.end(); };
            int total = 0;
            for (int i = 0; i < rows; ++i)
                total += DL::c_choiceHeight + (has(i) ? c_captionHeight + c_captionGap : 0) + (i > 0 ? (has(i) ? c_captionedGap : DL::c_choiceGap) : 0);
            const auto column = DL::choiceBox(context.target, 0, 1);
            std::vector<juce::Rectangle<int>> places;
            int y = (height - total) / 2;
            for (int i = 0; i < rows; ++i)
            {
                if (i > 0)
                    y += has(i) ? c_captionedGap : DL::c_choiceGap;
                if (has(i))
                    y += c_captionHeight + c_captionGap;
                places.push_back({ column.getX(), y, column.getWidth(), DL::c_choiceHeight });
                y += DL::c_choiceHeight;
            }
            return places;
        }

        /** Puts each box's caption over it, shown while the box is. After the boxes are placed. */
        void placeCaptions()
        {
            for (auto& cap : m_captions)
            {
                const auto b = cap.second->getBounds();
                cap.first->setBounds(b.getX(), b.getY() - c_captionGap - c_captionHeight, b.getWidth(), c_captionHeight);
                cap.first->setVisible(cap.second->isVisible());
            }
        }

        static constexpr int c_captionHeight = 13, c_captionGap = 1, c_captionedGap = 3;
        // Ride's pages: the wheels' part of the row (the panel's keys are under it).
        static constexpr int c_rideHeight = DL::c_wheelY + DL::c_wheelHeight;

    private:
        struct Tip
        {
            juce::Component* component;
            juce::String heading, body, reason;
        };
        std::vector<std::pair<std::unique_ptr<BoxCaption>, juce::ComboBox*>> m_captions;
        std::vector<std::unique_ptr<WheelKnob>> m_wheels;
        std::vector<std::unique_ptr<juce::ComboBox>> m_boxes;
        std::vector<std::unique_ptr<juce::TextButton>> m_keys;
        // After the controls, so they go first.
        std::vector<std::unique_ptr<APVTS::SliderAttachment>> m_sliders;
        std::vector<std::unique_ptr<APVTS::ComboBoxAttachment>> m_boxAttachments;
        std::vector<std::unique_ptr<APVTS::ButtonAttachment>> m_keyAttachments;
        std::vector<Tip> m_tips;
    };

    //==================================================================================================================
    /** Varispeed: a hand on the second tape machine. Lean it and it springs back to the middle when let go (the held
        offset stays: that is the OffsetMeter's). Nothing to type: a typed lean would never spring back. */
    class SprungWheel : public WheelKnob
    {
    public:
        using WheelKnob::WheelKnob;

        void mouseDown(const juce::MouseEvent& e) override
        {
            // A right-click is not a lean, nor is a Cmd-drag that sets a wire's depth.
            m_leaning = ! (e.mods.isPopupMenu() || (e.mods.isCommandDown() && isModulated()));
            WheelKnob::mouseDown(e);
        }
        void mouseUp(const juce::MouseEvent& e) override
        {
            WheelKnob::mouseUp(e);
            if (! std::exchange(m_leaning, false))
                return;
            if (parameter != nullptr && std::abs(parameter->getValue() - parameter->getDefaultValue()) > 1.0e-6f)
            {
                parameter->beginChangeGesture();
                parameter->setValueNotifyingHost(parameter->getDefaultValue());
                parameter->endChangeGesture();
            }
        }

        juce::RangedAudioParameter* parameter = nullptr;

    private:
        bool m_leaning = false;
    };

    //==================================================================================================================
    // Swirl: the Type box, then each type's own controls.
    class SwirlPage final : public Page
    {
    public:
        explicit SwirlPage(const Context& c)
            : Page(c), m_bus(juce::jlimit(0, ParamIDs::numBuses - 1, c.target.bus))
        {
            using namespace ParamIDs;
            const auto* ids = busDetail[m_bus];
            m_type = &box(ids[SwirlType], swirlTypeNames, numSwirlTypes, "Type");

            m_classic = { &wheel(ids[SwirlFeedback], "Feedback"), &wheel(ids[SwirlSpeed], "Speed"),
                          &wheel(ids[SwirlDepth], "Depth"), &wheel(ids[SwirlFollow], "Follow") };

            m_motion = &box(ids[TapeMotion], tapeMotionNames, numTapeMotions, "Motion");
            caption(*m_motion, "Motion");
            m_invert = &key(ids[TapeInvert], "Invert");
            m_tapeDepth = &wheel(ids[TapeDepth], "Depth");
            m_tapeRate = &wheel(ids[TapeRate], "Rate");
            m_tapeFeedback = &wheel(ids[TapeFeedback], "Feedback");
            m_tapeWobble = &wheel(ids[TapeWobble], "Wobble");
            {
                auto sprung = std::make_unique<SprungWheel>(c.fonts, c.accent, WheelKnob::Size::Secondary);
                auto* raw = sprung.get();
                m_varispeed = &adopt(std::move(sprung), ids[Varispeed], "Varispeed");
                raw->parameter = state().getParameter(ids[Varispeed]);
                raw->setTypedParameter(nullptr, c.fonts);
                raw->getProperties().set("howTo", "Drag it to lean; it springs back to the middle when you let go. Drag a Mod jack "
                                                  "onto it to lean it with a modulator.");
            }
            m_meter = std::make_unique<OffsetMeter>(c.processor, m_bus); // its tooltip and double-click are its own
            addAndMakeVisible(*m_meter);

            m_wave = &box(ids[DoublerWave], doublerWaveNames, numDoublerWaves, "Wave");
            caption(*m_wave, "Wave");
            m_thicken = &key(ids[DoublerThicken], "Thicken");
            m_spread = &key(ids[DoublerSpread], "Spread");
            // Side by side in one place of the column, a lamp slot would leave their names no room: they are chips,
            // whose plate says on or off.
            SwooshLookAndFeel::setStyle(*m_thicken, SwooshLookAndFeel::ButtonStyle::Chip);
            SwooshLookAndFeel::setStyle(*m_spread, SwooshLookAndFeel::ButtonStyle::Chip);
            m_doubler = { &wheel(ids[DoublerDelay], "Delay"), &wheel(ids[DoublerWander], "Wander"),
                          &wheel(ids[DoublerRate], "Rate"), &wheel(ids[DoublerFeedback], "Feedback") };

            refresh();
        }

        void refresh() override
        {
            using namespace ParamIDs;
            const auto* ids = busDetail[m_bus];
            const int type = juce::jlimit(0, numSwirlTypes - 1, choice(ids[SwirlType]));
            if (type != m_shownType)
            {
                m_shownType = type;
                const bool classic = type == SwirlBoth || type == SwirlPhaser || type == SwirlFlanger;
                const bool tape = type == SwirlTape, doubler = type == SwirlDoubler;
                for (auto* w : m_classic)
                    w->setVisible(classic);
                const std::initializer_list<juce::Component*> tapeOnly { m_motion, m_invert, m_tapeDepth, m_tapeRate, m_tapeFeedback,
                                                                         m_tapeWobble, m_varispeed, m_meter.get() };
                for (auto* w : tapeOnly)
                    w->setVisible(tape);
                const std::initializer_list<juce::Component*> doublerOnly { m_wave, m_thicken, m_spread };
                for (auto* w : doublerOnly)
                    w->setVisible(doubler);
                for (auto* w : m_doubler)
                    w->setVisible(doubler);
                resized();
            }

            for (auto* w : m_classic)
                rest(*w, {});
            rest(*m_type, {});
            rest(*m_invert, {});
            rest(*m_tapeFeedback, {});
            rest(*m_tapeWobble, {});
            rest(*m_varispeed, {});
            rest(*m_wave, {});
            rest(*m_thicken, {});
            rest(*m_spread, {});
            for (int i = 0; i < 3; ++i)
                rest(*m_doubler[static_cast<size_t>(i)], {});

            const int motion = juce::jlimit(0, numTapeMotions - 1, choice(ids[TapeMotion]));
            rest(*m_motion, {});
            rest(*m_tapeDepth, {});
            rest(*m_tapeRate, motion == TapeOnItsOwn ? juce::String()
                                                     : "It rests: it sets the speed for On Its Own, and Motion is " + juce::String(tapeMotionNames[motion]) + ".");
            rest(*m_doubler[3], isOn(ids[DoublerThicken]) ? "It rests: Thicken is on and sets the feedback itself from the delay." : "");

            if (type == SwirlTape)
                m_meter->refresh();
        }

        std::vector<ModTarget*> wireTargets() override { return { m_varispeed }; }

        void resized() override
        {
            const auto t = context.target;
            const int type = m_shownType;
            if (type == ParamIDs::SwirlTape)
            {
                const auto rows = stack(3, { 1 });
                m_type->setBounds(rows[0]);
                m_motion->setBounds(rows[1]);
                m_invert->setBounds(rows[2]);
                placeWheel(*m_tapeDepth, 0);
                placeWheel(*m_tapeRate, 1);
                placeWheel(*m_tapeFeedback, 2);
                placeWheel(*m_tapeWobble, 3);
                placeWheel(*m_varispeed, 4);
                m_meter->setBounds(DL::meterBox(t, 4));
            }
            else if (type == ParamIDs::SwirlDoubler)
            {
                const auto rows = stack(3, { 1 });
                m_type->setBounds(rows[0]);
                m_wave->setBounds(rows[1]);
                const int half = (rows[2].getWidth() - DL::c_choiceGap) / 2;
                m_thicken->setBounds(rows[2].withWidth(half));
                m_spread->setBounds(rows[2].withLeft(rows[2].getRight() - half));
                for (int i = 0; i < 4; ++i)
                    placeWheel(*m_doubler[static_cast<size_t>(i)], i);
            }
            else
            {
                m_type->setBounds(DL::choiceBox(t, 0, 1));
                for (int i = 0; i < 4; ++i)
                    placeWheel(*m_classic[static_cast<size_t>(i)], i);
            }
            placeCaptions();
        }

    private:
        const int m_bus;
        int m_shownType = -1;
        juce::ComboBox* m_type = nullptr;
        std::array<WheelKnob*, 4> m_classic {};
        juce::ComboBox* m_motion = nullptr;
        juce::TextButton* m_invert = nullptr;
        WheelKnob *m_tapeDepth = nullptr, *m_tapeRate = nullptr, *m_tapeFeedback = nullptr, *m_tapeWobble = nullptr, *m_varispeed = nullptr;
        std::unique_ptr<OffsetMeter> m_meter;
        juce::ComboBox* m_wave = nullptr;
        juce::TextButton *m_thicken = nullptr, *m_spread = nullptr;
        std::array<WheelKnob*, 4> m_doubler {};
    };

    //==================================================================================================================
    // Filter: Type, the 24 dB key and Shape; Closed At, Resonance, Follow, Sweep and Rate.
    class FilterPage final : public Page
    {
    public:
        explicit FilterPage(const Context& c)
            : Page(c), m_bus(juce::jlimit(0, ParamIDs::numBuses - 1, c.target.bus))
        {
            using namespace ParamIDs;
            const auto* ids = busDetail[m_bus];
            m_type = &box(ids[FilterType], filterTypeNames, numFilterTypes, "Type");
            m_steep = &key(ids[FilterSteep], "24 dB");
            m_shape = &box(ids[FilterShape], filterShapeNames, numFilterShapes, "Shape");
            caption(*m_shape, "Shape");
            m_wheels = { &wheel(ids[FilterClosedAt], "Closed At"), &wheel(ids[FilterResonance], "Resonance"),
                         &wheel(ids[FilterFollow], "Follow"), &wheel(ids[FilterSweep], "Sweep"), &wheel(ids[FilterRate], "Rate") };
            refresh();
        }

        void refresh() override
        {
            using namespace ParamIDs;
            const auto* ids = busDetail[m_bus];
            const bool still = value(ids[FilterSweep]) < 0.005f;
            const juce::String why = still ? "It rests: Sweep is 0%, so the filter has no rhythm of its own." : "";
            rest(*m_type, {});
            rest(*m_steep, {});
            rest(*m_shape, why);
            for (int i = 0; i < 4; ++i)
                rest(*m_wheels[static_cast<size_t>(i)], {});
            rest(*m_wheels[4], why);
        }

        void resized() override
        {
            const auto rows = stack(3, { 2 });
            m_type->setBounds(rows[0]);
            m_steep->setBounds(rows[1]);
            m_shape->setBounds(rows[2]);
            for (int i = 0; i < 5; ++i)
                placeWheel(*m_wheels[static_cast<size_t>(i)], i);
            placeCaptions();
        }

    private:
        const int m_bus;
        juce::ComboBox *m_type = nullptr, *m_shape = nullptr;
        juce::TextButton* m_steep = nullptr;
        std::array<WheelKnob*, 5> m_wheels {};
    };

    //==================================================================================================================
    // Ride / Together: Hit Every; Rate (the wheel the layer was opened from: the swell's length), Peak At and Stagger.
    // Rate works in every Ride Mode; the rest only in Faders Together (the Ride Mode box is right under the page).
    class TogetherPage final : public Page
    {
    public:
        explicit TogetherPage(const Context& c) : Page(c)
        {
            using namespace ParamIDs;
            m_hitEvery = &box(togetherHitEvery, hitEveryNames, numHitEvery, "Hit Every");
            caption(*m_hitEvery, "Hit Every");
            m_rate = &wheel(rate, "Rate", "Ride: Rate");
            // (As the front wheel: the length of one swell in Faders Together, the ride's own time in the other modes.)
            m_rate->textFromValueFunction = [this](double v) { return Params::rateText(static_cast<float>(v), choice(ParamIDs::rideMode) == ParamIDs::RideTogether); };
            m_rate->typedText = [this](float v) { return Params::rateText(v, choice(ParamIDs::rideMode) == ParamIDs::RideTogether); };
            m_rate->valueTextChanged();
            m_peakAt = &wheel(togetherPeakAt, "Peak At");
            m_stagger = &wheel(togetherStagger, "Stagger");
            refresh();
        }

        void refresh() override
        {
            using namespace ParamIDs;
            const int mode = juce::jlimit(0, numRideModes - 1, choice(rideMode));
            const juce::String why = mode == RideTogether ? juce::String()
                                                          : "It rests: Ride Mode is " + juce::String(rideModeNames[mode]) + ". It works in Faders Together: the Ride Mode box is under this page.";
            rest(*m_rate, {});
            if (mode != m_shownMode)
            {
                m_shownMode = mode;
                m_rate->valueTextChanged(); // its readout is the swell's length in Faders Together
            }
            rest(*m_peakAt, why);
            rest(*m_stagger, why);
            rest(*m_hitEvery, why.isNotEmpty() ? why : (isOn(hitAim) ? juce::String() : "It rests: Aim is off, so no hits are aimed at the peaks."));
        }

        void resized() override
        {
            m_hitEvery->setBounds(stack(1, { 0 }, c_rideHeight)[0]);
            placeWheel(*m_rate, 0);
            placeWheel(*m_peakAt, 1);
            placeWheel(*m_stagger, 2);
            placeCaptions();
        }

    private:
        juce::ComboBox* m_hitEvery = nullptr;
        WheelKnob *m_rate = nullptr, *m_peakAt = nullptr, *m_stagger = nullptr;
        int m_shownMode = -1;
    };

    //==================================================================================================================
    // Ride / Swell: Auto Swell; Every, Hold, Chance and Loose.
    class SwellPage final : public Page
    {
    public:
        explicit SwellPage(const Context& c) : Page(c)
        {
            using namespace ParamIDs;
            m_auto = &box(swellAuto, autoSwellNames, numAutoSwells, "Auto Swell");
            caption(*m_auto, "Auto Swell");
            m_every = &wheel(swellEvery, "Every");
            m_hold = &wheel(swellHold, "Hold");
            m_chance = &wheel(swellChance, "Chance");
            m_loose = &wheel(swellLoose, "Loose");
            // Hold reads what is held: never more than 60% of Every.
            m_hold->textFromValueFunction = [this](double v)
            {
                return Params::secondsText(juce::jmin(static_cast<float>(v), ParamIDs::swellHoldShareMax * value(ParamIDs::swellEvery)));
            };
            m_hold->valueTextChanged();
            refresh();
        }

        void refresh() override
        {
            using namespace ParamIDs;
            const int mode = juce::jlimit(0, numAutoSwells - 1, choice(swellAuto));
            const bool together = choice(rideMode) == RideTogether;
            juce::String why;
            if (! isOn(rideOn))
                why = "It rests: Auto Ride is off (its key is under this page). Auto Swell runs only while the faders ride.";
            else if (value(energy) < 0.005f)
                why = "It rests: Energy is 0%, so the faders do not swell.";
            const juce::String peaksWhy = mode == AutoSwellPeaks && together
                                              ? "It rests: in Faders Together the faders already rise to every peak. Choose On a Clock, or Faders Apart."
                                              : "";
            const juce::String offWhy = mode == AutoSwellOff ? "It rests: Auto Swell is Off." : "";
            const juce::String clockWhy = mode == AutoSwellClock ? juce::String() : offWhy.isNotEmpty() ? offWhy : "It rests: it is for On a Clock.";
            auto first = [](std::initializer_list<juce::String> reasons)
            {
                for (const auto& r : reasons)
                    if (r.isNotEmpty())
                        return r;
                return juce::String();
            };
            rest(*m_auto, first({ why, peaksWhy }));
            rest(*m_every, first({ why, clockWhy }));
            rest(*m_loose, first({ why, clockWhy }));
            rest(*m_hold, first({ why, offWhy, peaksWhy }));
            rest(*m_chance, first({ why, offWhy, peaksWhy }));

            // Hold's readout follows Every (the cap).
            if (const float every = value(swellEvery); std::abs(every - m_shownEvery) > 1.0e-6f)
            {
                m_shownEvery = every;
                m_hold->valueTextChanged();
            }
        }

        void resized() override
        {
            m_auto->setBounds(stack(1, { 0 }, c_rideHeight)[0]);
            placeWheel(*m_every, 0);
            placeWheel(*m_hold, 1);
            placeWheel(*m_chance, 2);
            placeWheel(*m_loose, 3);
            placeCaptions();
        }

    private:
        juce::ComboBox* m_auto = nullptr;
        WheelKnob *m_every = nullptr, *m_hold = nullptr, *m_chance = nullptr, *m_loose = nullptr;
        float m_shownEvery = -1.0f;
    };
}

namespace LayerPages
{
    std::unique_ptr<LayerPage> make(const LayerPage::Context& context)
    {
        switch (context.target.kind)
        {
            case Layers::Kind::Swirl:    return std::make_unique<SwirlPage>(context);
            case Layers::Kind::Filter:   return std::make_unique<FilterPage>(context);
            case Layers::Kind::Together: return std::make_unique<TogetherPage>(context);
            case Layers::Kind::Swell:    return std::make_unique<SwellPage>(context);
        }
        return std::make_unique<LayerPage>(context);
    }
}
