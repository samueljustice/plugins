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

    /** What a page has in common: making its controls, reading its parameters, and the resting look. */
    class Page : public LayerPage
    {
    public:
        explicit Page(const Context& c) : LayerPage(c) {}

    protected:
        APVTS& state() const { return context.processor.getState(); }
        float value(const char* id) const
        {
            auto* raw = state().getRawParameterValue(id);
            return raw != nullptr ? raw->load() : 0.0f;
        }
        int choice(const char* id) const { return juce::roundToInt(value(id)); }
        bool isOn(const char* id) const { return value(id) > 0.5f; }

        /** The heading every tooltip on this page starts with ("Bus A Swirl", "Auto Swell"). */
        virtual juce::String group() const = 0;

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
            m_tips.push_back({ knob.get(), heading.isNotEmpty() ? heading : group() + ": " + caption, Params::tipFor(id), {} });
            m_wheels.push_back(std::move(knob));
            return *m_wheels.back();
        }

        /** A box of choices bound to `id`; its menu is headed by `heading`. */
        juce::ComboBox& box(const char* id, const char* const* names, int count, const juce::String& heading)
        {
            auto b = std::make_unique<juce::ComboBox>();
            b->addSectionHeading(heading);
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

        /** A key that switches `id` on and off, in the panel's colour when on. */
        juce::TextButton& key(const char* id, const juce::String& text, const juce::String& heading)
        {
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

    private:
        struct Tip
        {
            juce::Component* component;
            juce::String heading, body, reason;
        };
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
            m_type = &box(ids[SwirlType], swirlTypeNames, numSwirlTypes, group() + ": Type");

            m_classic = { &wheel(ids[SwirlFeedback], "Feedback"), &wheel(ids[SwirlSpeed], "Speed"),
                          &wheel(ids[SwirlDepth], "Depth"), &wheel(ids[SwirlFollow], "Follow") };

            const auto tape = bus() + " Tape Flanger";
            m_motion = &box(ids[TapeMotion], tapeMotionNames, numTapeMotions, tape + ": Motion");
            m_invert = &key(ids[TapeInvert], "Invert", tape + ": Invert");
            m_tapeDepth = &wheel(ids[TapeDepth], "Depth", tape + ": Depth");
            m_tapeRate = &wheel(ids[TapeRate], "Rate", tape + ": Rate");
            m_tapeFeedback = &wheel(ids[TapeFeedback], "Feedback", tape + ": Feedback");
            m_tapeWobble = &wheel(ids[TapeWobble], "Wobble", tape + ": Wobble");
            {
                auto sprung = std::make_unique<SprungWheel>(c.fonts, c.accent, WheelKnob::Size::Secondary);
                auto* raw = sprung.get();
                m_varispeed = &adopt(std::move(sprung), ids[Varispeed], "Varispeed", tape + ": Varispeed");
                raw->parameter = state().getParameter(ids[Varispeed]);
                raw->setTypedParameter(nullptr, c.fonts);
                raw->getProperties().set("howTo", "Drag it to lean; it springs back to the middle when you let go. Drag a Mod jack "
                                                  "onto it to lean it with a modulator.");
            }
            m_meter = std::make_unique<OffsetMeter>(c.processor, m_bus); // its tooltip and double-click are its own
            addAndMakeVisible(*m_meter);

            const auto doubler = bus() + " Doubler";
            m_wave = &box(ids[DoublerWave], doublerWaveNames, numDoublerWaves, doubler + ": Wave");
            m_thicken = &key(ids[DoublerThicken], "Thicken", doubler + ": Thicken");
            m_spread = &key(ids[DoublerSpread], "Spread", doubler + ": Spread");
            // Side by side in one place of the column, a lamp slot would leave their names no room: they are chips,
            // whose plate says on or off.
            SwooshLookAndFeel::setStyle(*m_thicken, SwooshLookAndFeel::ButtonStyle::Chip);
            SwooshLookAndFeel::setStyle(*m_spread, SwooshLookAndFeel::ButtonStyle::Chip);
            m_doubler = { &wheel(ids[DoublerDelay], "Delay", doubler + ": Delay"), &wheel(ids[DoublerWander], "Wander", doubler + ": Wander"),
                          &wheel(ids[DoublerRate], "Rate", doubler + ": Rate"), &wheel(ids[DoublerFeedback], "Feedback", doubler + ": Feedback") };

            refresh();
        }

        juce::String group() const override { return bus() + " Swirl"; }

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
            rest(*m_tapeDepth, motion == TapeByHand ? "It rests: Motion is By Hand, so only Varispeed moves the second copy." : "");
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
                m_type->setBounds(DL::choiceBox(t, 0, 3));
                m_motion->setBounds(DL::choiceBox(t, 1, 3));
                m_invert->setBounds(DL::choiceBox(t, 2, 3));
                placeWheel(*m_tapeDepth, 0);
                placeWheel(*m_tapeRate, 1);
                placeWheel(*m_tapeFeedback, 2);
                placeWheel(*m_tapeWobble, 3);
                placeWheel(*m_varispeed, 4);
                m_meter->setBounds(DL::meterBox(t, 4));
            }
            else if (type == ParamIDs::SwirlDoubler)
            {
                m_type->setBounds(DL::choiceBox(t, 0, 3));
                m_wave->setBounds(DL::choiceBox(t, 1, 3));
                m_thicken->setBounds(DL::choiceHalf(t, 2, 3, false));
                m_spread->setBounds(DL::choiceHalf(t, 2, 3, true));
                for (int i = 0; i < 4; ++i)
                    placeWheel(*m_doubler[static_cast<size_t>(i)], i);
            }
            else
            {
                m_type->setBounds(DL::choiceBox(t, 0, 1));
                for (int i = 0; i < 4; ++i)
                    placeWheel(*m_classic[static_cast<size_t>(i)], i);
            }
        }

    private:
        juce::String bus() const { return juce::String("Bus ") + ParamIDs::busNames[m_bus]; }

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
            m_type = &box(ids[FilterType], filterTypeNames, numFilterTypes, group() + ": Type");
            m_steep = &key(ids[FilterSteep], "24 dB", group() + ": 24 dB");
            m_shape = &box(ids[FilterShape], filterShapeNames, numFilterShapes, group() + ": Shape");
            m_wheels = { &wheel(ids[FilterClosedAt], "Closed At"), &wheel(ids[FilterResonance], "Resonance"),
                         &wheel(ids[FilterFollow], "Follow"), &wheel(ids[FilterSweep], "Sweep"), &wheel(ids[FilterRate], "Rate") };
            refresh();
        }

        juce::String group() const override { return juce::String("Bus ") + ParamIDs::busNames[m_bus] + " Filter"; }

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
            const auto t = context.target;
            m_type->setBounds(DL::choiceBox(t, 0, 3));
            m_steep->setBounds(DL::choiceBox(t, 1, 3));
            m_shape->setBounds(DL::choiceBox(t, 2, 3));
            for (int i = 0; i < 5; ++i)
                placeWheel(*m_wheels[static_cast<size_t>(i)], i);
        }

    private:
        const int m_bus;
        juce::ComboBox *m_type = nullptr, *m_shape = nullptr;
        juce::TextButton* m_steep = nullptr;
        std::array<WheelKnob*, 5> m_wheels {};
    };

    //==================================================================================================================
    // Ride / Together: Hit Every; Peak At and Stagger. All of it works only in Faders Together.
    class TogetherPage final : public Page
    {
    public:
        explicit TogetherPage(const Context& c) : Page(c)
        {
            using namespace ParamIDs;
            m_hitEvery = &box(togetherHitEvery, hitEveryNames, numHitEvery, "Hit Every");
            m_peakAt = &wheel(togetherPeakAt, "Peak At");
            m_stagger = &wheel(togetherStagger, "Stagger");
            refresh();
        }

        juce::String group() const override { return "Faders Together"; }

        void refresh() override
        {
            using namespace ParamIDs;
            const int mode = juce::jlimit(0, numRideModes - 1, choice(rideMode));
            const juce::String why = mode == RideTogether ? juce::String()
                                                          : "It rests: Ride Mode is " + juce::String(rideModeNames[mode]) + ". It works in Faders Together.";
            rest(*m_peakAt, why);
            rest(*m_stagger, why);
            rest(*m_hitEvery, why.isNotEmpty() ? why : (isOn(hitAim) ? juce::String() : "It rests: Aim is off, so no hits are aimed at the peaks."));
        }

        void resized() override
        {
            const auto t = context.target;
            m_hitEvery->setBounds(DL::choiceBox(t, 0, 1));
            placeWheel(*m_peakAt, 0);
            placeWheel(*m_stagger, 1);
        }

    private:
        juce::ComboBox* m_hitEvery = nullptr;
        WheelKnob *m_peakAt = nullptr, *m_stagger = nullptr;
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

        juce::String group() const override { return "Auto Swell"; }

        void refresh() override
        {
            using namespace ParamIDs;
            const int mode = juce::jlimit(0, numAutoSwells - 1, choice(swellAuto));
            const bool together = choice(rideMode) == RideTogether;
            juce::String why;
            if (! isOn(rideOn))
                why = "It rests: Auto Ride is off. Auto Swell runs only while the faders ride.";
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
            const auto t = context.target;
            m_auto->setBounds(DL::choiceBox(t, 0, 1));
            placeWheel(*m_every, 0);
            placeWheel(*m_hold, 1);
            placeWheel(*m_chance, 2);
            placeWheel(*m_loose, 3);
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
