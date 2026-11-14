#include "CargoHoldView.h"
#include "LookAndFeel.h"
#include "SampleClassifier.h"

namespace TC = Theme::Colours;

namespace
{
    constexpr int c_peakBlock = 256;
    constexpr int c_headerWidth = 300; // a lane's controls, left of its waveform
    constexpr int c_skipWidth = 98;
    constexpr int c_buttonsWidth = 100; // the star, M, S and the cross at the right of a lane's name row
    constexpr int c_dropLine = 20;      // the drop zone once tracks are loaded: one slim line
    // CAF and M4A are read through Core Audio, so only on macOS.
   #if JUCE_MAC
    const juce::String c_audioExtensions = "wav;wave;aif;aiff;flac;ogg;mp3;caf;m4a";
   #else
    const juce::String c_audioExtensions = "wav;wave;aif;aiff;flac;ogg;mp3";
   #endif
    const juce::String c_audioWildcard = "*." + c_audioExtensions.replace(";", ";*.");

    juce::String roleTip(CargoRole role)
    {
        switch (role)
        {
            // What each type is for. The type picks the bus; every track loops the same way.
            case CargoRole::Bed:     return "Steady sound with body, such as wind, drones, engines or rain, that rolls under the whoosh.";
            case CargoRole::Tonal:   return "Sounds with a pitch or a voice, such as growls, squeals, creatures or tones.";
            case CargoRole::Impact:  return "Hits and transients. They only play when the hit gate opens.";
            case CargoRole::Air:     return "Air movement, such as swishes, cloth, flaps or sword swings.";
            case CargoRole::Crackle: return "Busy, broken-up texture, such as fire, debris or sparks.";
            case CargoRole::Rumble:  return "Low end, such as sub, thunder or rumble.";
            case CargoRole::Generic:
            default:                 return "A bit of everything, or no single clear type.";
        }
    }
}

bool CargoHoldView::isAudioFile(const juce::String& path)
{
    return juce::File(path).hasFileExtension(c_audioExtensions);
}

//==============================================================================
/** A small number box (semitones or cents): drag up or down to change it, double-click to reset. The Semitones box
    is also a modulation target: each wire's reach is a bar under the number (dashed when negative), the live pitch
    a dot, and a Cmd-drag (Ctrl on Windows) sets the wire's depth (see ModTarget). */
class PitchBox : public ModTarget
{
public:
    PitchBox(const Theme::Fonts& fonts, juce::Colour colour, int pixelsForFullRange) : m_fonts(fonts), m_colour(colour)
    {
        setSliderStyle(juce::Slider::RotaryVerticalDrag); // a vertical drag over a set distance, not over the box's 16 px
        setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        setMouseDragSensitivity(pixelsForFullRange);
        setDoubleClickReturnValue(true, 0.0);
        setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
        setWantsKeyboardFocus(false);
        setScrollWheelEnabled(false); // the wheel scrolls the track list, it never retunes a track by accident
    }

    void paint(juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced(0.5f);
        const bool zero = std::abs(getValue()) < 0.5;
        const bool hot = isMouseOverOrDragging();
        // A well; its edge is in the track's colour when it holds something other than zero, lighter under the mouse.
        SwooshLookAndFeel::drawWell(g, r, Theme::Cut::small);
        if (hot || ! zero)
        {
            g.setColour(zero ? TC::stroke : m_colour.withAlpha(hot ? 1.0f : 0.8f));
            Theme::Plate::stroke(g, r, Theme::Cut::small);
        }
        g.setColour(zero ? TC::textDim : TC::text);
        Theme::fitText(g, getTextFromValue(getValue()), getLocalBounds().reduced(2, 0), m_fonts.caption(), juce::Justification::centred);

        // Modulation: each wire's reach along the bottom edge, from the set pitch (both ways for LFO / Random,
        // one way for Follower / Macro), dashed when negative; the live pitch as a dot.
        if (! m_modArcs.empty())
        {
            const float base = static_cast<float>(valueToProportionOfLength(getValue()));
            const float x0 = r.getX() + 3.0f, w = r.getWidth() - 6.0f;
            float y = r.getBottom() - 2.0f;
            for (const auto& m : m_modArcs)
            {
                const float lo = m.bipolar ? base - 0.5f * std::abs(m.depth) : juce::jmin(base, base + m.depth);
                const float hi = m.bipolar ? base + 0.5f * std::abs(m.depth) : juce::jmax(base, base + m.depth);
                juce::Path p;
                p.startNewSubPath(x0 + w * juce::jlimit(0.0f, 1.0f, lo), y);
                p.lineTo(x0 + w * juce::jlimit(0.0f, 1.0f, hi), y);
                strokeModPath(g, p, m.colour, m.depth, 1.6f);
                y -= 2.2f;
            }
            if (m_modLive >= 0.0f)
            {
                g.setColour(TC::text);
                g.fillRect(juce::Rectangle<float>(3.0f, 4.0f).withCentre({ x0 + w * m_modLive, r.getBottom() - 2.5f }));
            }
        }
        if (! m_wireHover.isTransparent())
        {
            g.setColour(m_wireHover.withAlpha(0.85f));
            Theme::Plate::stroke(g, getLocalBounds().toFloat(), Theme::Cut::small, 2.0f);
        }
    }

    void setColour(juce::Colour c) { if (c != m_colour) { m_colour = c; repaint(); } }

    juce::Point<float> getDialCentre() const override { return getLocalBounds().toFloat().getCentre(); }
    // A flat box: its plugs sit in a row just under it (not around a dial, where they'd cover the buttons above).
    juce::Point<float> getPlugPosition(int index) const override
    {
        return { 7.0f + 11.0f * static_cast<float>(index), static_cast<float>(getHeight()) + 4.0f };
    }
    float getDialRadius() const override { return static_cast<float>(getHeight()) * 0.5f + 2.0f; }
    bool isReachable() const override
    {
        // Inside the scrolling track list: only when fully in view.
        if (! ModTarget::isReachable())
            return false;
        if (const auto* vp = findParentComponentOfClass<juce::Viewport>())
            return vp->getLocalBounds().contains(vp->getLocalArea(this, getLocalBounds()));
        return true;
    }

private:
    const Theme::Fonts& m_fonts;
    juce::Colour m_colour;
};

//==============================================================================
class CargoHoldView::Lane : public juce::Component, public juce::TooltipClient
{
public:
    Lane(CargoHoldView& owner, int track)
        : m_owner(owner), m_track(track),
          m_semis(owner.m_fonts, TC::busB, 240), m_cents(owner.m_fonts, TC::busB, 400),
          m_levelAttachment(owner.m_processor.getState(), ParamIDs::cargoTrack[track], m_level),
          m_semisAttachment(owner.m_processor.getState(), ParamIDs::trackSemis[track], m_semis),
          m_centsAttachment(owner.m_processor.getState(), ParamIDs::trackCents[track], m_cents),
          m_variAttachment(owner.m_processor.getState(), ParamIDs::trackVari[track], m_vari)
    {
        // This track's own pitch, on top of the Ride's Pitch knob.
        m_semis.caption = "Track " + juce::String(track + 1) + " Pitch";
        m_semis.setTooltip("Semitones\nThis track's own pitch, up to two octaves up or down, on top of the Ride's Pitch knob. "
                           "With Vari on it works like tape, so higher is also faster; with Vari off the speed stays the same. "
                           "Drag a Mod jack onto it to modulate the track's pitch (smoothly, not in steps); a 100% wire swings two octaves.\n"
                           "Drag up or down. Double-click to reset. Right-click (or Alt-double-click) to type a value. "
                           "Cmd-drag (Ctrl on Windows) when it's modulated to set the wire's depth, up or down.");
        m_cents.setTooltip("Cents\nFine tuning for this track, up to a semitone either way (100 cents), on top of the semitones.\n"
                           "Drag up or down. Double-click to reset. Right-click (or Alt-double-click) to type a value.");
        m_semis.setTypedParameter(owner.m_processor.getState().getParameter(ParamIDs::trackSemis[track]), owner.m_fonts);
        m_cents.setTypedParameter(owner.m_processor.getState().getParameter(ParamIDs::trackCents[track]), owner.m_fonts);
        m_vari.setClickingTogglesState(true);
        m_vari.setWantsKeyboardFocus(false);
        m_vari.setTooltip("Vari\nOn (the default): varispeed, like tape. The pitch and the speed change together, as with the Ride's Pitch knob. "
                          "Off: the pitch changes and the speed stays the same, through a pitch shifter. "
                          "That delays the track by about 43 ms; synced hits allow for it.\nClick to switch.");
        for (auto* c : std::initializer_list<juce::Component*> { &m_semis, &m_cents, &m_vari })
            addAndMakeVisible(c);

        m_level.setSliderStyle(juce::Slider::LinearHorizontal);
        m_level.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        m_level.setTooltip("Track Level\nThis track's fader, in dB (30 dB of travel, then silence at the bottom). With Auto Ride on it moves by itself and this sets how high it can go; "
                           "the strip next to the waveform shows where it is right now.\nRight-click for Learn MIDI CC, to have a fader or knob on a MIDI controller move it.");
        m_level.setTypedParameter(owner.m_processor.getState().getParameter(ParamIDs::cargoTrack[track]), owner.m_fonts);
        m_level.onContextMenu = [this] { m_owner.showControlMenu(m_track, m_level); };
        addAndMakeVisible(m_level);

        SwooshLookAndFeel::setStyle(m_type, SwooshLookAndFeel::ButtonStyle::Menu);
        m_type.setClickingTogglesState(false);
        m_type.onClick = [this] { showTypeMenu(); };
        addAndMakeVisible(m_type);


        m_master.setClickingTogglesState(false);
        m_master.setTooltip("Master Track\nMakes this track the Master: the one sound the others can follow. Only one track is the Master at a time. "
                            "A Follower modulator set to Master, Master Pitch or Master Brightness then moves with this sound's loudness, pitch or brightness "
                            "as it plays, and Follow Master in the Ride panel makes every other track swell with it.\n"
                            "Click to choose it, click again to have no Master. Right-click another lane for Match Pitch to Master.");
        m_master.onClick = [this]
        {
            const bool was = m_owner.m_hold.getMaster() == m_track;
            m_owner.m_processor.setMasterTrack(was ? -1 : m_track);
            m_owner.status(was ? "No Master track." : "\"" + m_owner.m_hold.getDisplayName(m_track) + "\" is the Master track.");
        };
        addAndMakeVisible(m_master);

        // M and S are chips (states); the cross is an action, drawn, with no plate until the mouse is over it.
        for (auto* b : { &m_mute, &m_solo })
        {
            b->setColour(juce::TextButton::buttonOnColourId, b == &m_solo ? TC::brass : TC::hits);
            SwooshLookAndFeel::setStyle(*b, SwooshLookAndFeel::ButtonStyle::Chip);
            b->setWantsKeyboardFocus(false);
            addAndMakeVisible(*b);
        }
        addAndMakeVisible(m_remove);
        m_mute.setTooltip("Mute\nSilences this track. It keeps its place in the ride, so it comes straight back in when you unmute it.");
        m_solo.setTooltip("Solo\nPlays only this track (and any others soloed), to hear what it brings to the whoosh.");
        m_remove.setTooltip("Remove\nRemoves this track and frees its slot. Undo brings it back.");
        const int lane = CargoHold::c_numBuiltIns + track;
        m_mute.onClick = [this, lane] { m_owner.m_hold.setMute(lane, ! m_owner.m_hold.getMute(lane)); };
        m_solo.onClick = [this, lane] { m_owner.m_hold.setSolo(lane, ! m_owner.m_hold.getSolo(lane)); };
        m_remove.onClick = [this] { m_owner.m_hold.clear(m_track); m_owner.status("Removed. Undo brings it back."); };

        m_skip.setClickingTogglesState(true);
        m_skip.setWantsKeyboardFocus(false);
        m_skip.setTooltip("Skip Silence\nOn: only the sounds play and the gaps between them are skipped, so a recording full of pauses "
                          "becomes a steady source. Off: the whole file loops as it is, silence and all. "
                          "With Sync Hits on, off fires the whole file as one hit, lined up on its first transient.");
        m_skip.onClick = [this] { m_owner.m_hold.setSkipSilence(m_track, m_skip.getToggleState()); };
        addAndMakeVisible(m_skip);

        m_nameEditor.setFont(owner.m_fonts.control());
        m_nameEditor.onReturnKey = [this] { commitRename(); };
        m_nameEditor.onFocusLost = [this] { commitRename(); };
        m_nameEditor.onEscapeKey = [this] { m_nameEditor.setVisible(false); };
        addChildComponent(m_nameEditor);
        applyColour();
    }

    /** The lane's colour (a shade of its bus), given to everything in the lane that carries it. */
    void applyColour()
    {
        const auto colour = m_owner.laneColour(m_track);
        if (colour == m_colour)
            return;
        m_colour = colour;
        for (auto* b : { &m_vari, &m_type, &m_skip })
            b->setColour(juce::TextButton::buttonOnColourId, colour);
        m_level.setColour(juce::Slider::trackColourId, colour);
        m_semis.setColour(colour);
        m_cents.setColour(colour);
        repaint();
    }

    int getTrack() const { return m_track; }
    ModTarget* getSemisBox() { return &m_semis; }
    bool hasData() const { return m_data != nullptr && m_data->length() > 0; }
    bool hasSelection() const { return selEnd != selStart; }
    int selectionStart() const { return juce::jmin(selStart, selEnd); }
    int selectionEnd() const { return juce::jmax(selStart, selEnd); }
    int length() const { return m_data != nullptr ? m_data->length() : 0; }
    void select(int s, int e) { selStart = s; selEnd = e; cursor = s; repaint(); }

    void refresh()
    {
        auto& hold = m_owner.m_hold;
        const auto data = hold.get(m_track);
        if (data != m_data)
        {
            m_data = data;
            buildPeaks();
            computeWindow();
            selStart = juce::jlimit(0, length(), selStart);
            selEnd = juce::jlimit(0, length(), selEnd);
            cursor = juce::jlimit(0, length(), cursor);
        }
        const int lane = CargoHold::c_numBuiltIns + m_track;
        m_mute.setToggleState(hold.getMute(lane), juce::dontSendNotification);
        m_solo.setToggleState(hold.getSolo(lane), juce::dontSendNotification);
        m_skip.setToggleState(hold.getSkipSilence(m_track), juce::dontSendNotification);
        m_master.setToggleState(hold.getMaster() == m_track, juce::dontSendNotification);
        applyColour();
        m_type.setButtonText(CargoHold::typeName(hold.getRole(m_track)) + (hold.getRoleOverride(m_track) < 0 ? "" : " *"));
        m_type.setTooltip("Sound Type: " + CargoHold::typeName(hold.getRole(m_track)) + "\n" + roleTip(hold.getRole(m_track))
                          + " Plays on " + busLabel(hold.getBus(m_track)) + (hold.getBusOverride(m_track) < 0 ? "." : " (chosen by hand on the bus tag).")
                          + (hold.getRoleOverride(m_track) < 0 ? " Guessed from the sound." : " Set by you (marked *).")
                          + "\nWrong guess? Click to tell it what the sound is.");
        repaint();
    }

    /** Hit sync view, called at the UI rate: synced hit tracks slide and zoom into one shared beat timeline with
        the transient of the hit they fired last on the same line (the sync line), like regions aligned by hand. */
    void updateSync(bool synced, int region)
    {
        const float target = synced ? 1.0f : 0.0f;
        m_align += (target - m_align) * 0.22f;
        if (std::abs(target - m_align) < 0.002f)
            m_align = target;
        if (synced && region >= 0)
            m_syncRegion = region; // keep showing the last hit between beats
        if (! synced && m_align <= 0.0f)
            m_syncRegion = -1;
        computeWindow();
    }

    bool isAligned() const { return m_align > 0.0f; }

    static juce::Rectangle<int> headerArea(juce::Rectangle<int> bounds) { return bounds.removeFromLeft(c_headerWidth).reduced(14, 6); }

    void resized() override
    {
        auto header = headerArea(getLocalBounds());
        // Taller lanes (fewer than four tracks) keep their three rows together, in the middle.
        header = header.withSizeKeepingCentre(header.getWidth(), juce::jmin(header.getHeight(), 60));
        m_rows = header;
        auto top = header.removeFromTop(18);
        m_remove.setBounds(top.removeFromRight(20));
        top.removeFromRight(6);
        m_solo.setBounds(top.removeFromRight(22));
        top.removeFromRight(3);
        m_mute.setBounds(top.removeFromRight(22));
        top.removeFromRight(5);
        m_master.setBounds(top.removeFromRight(22));
        m_nameEditor.setBounds(top.withTrimmedLeft(10));
        header.removeFromTop(4);
        // Type and skip silence side by side, always visible; the fader underneath.
        auto row = header.removeFromTop(18);
        m_skip.setBounds(row.removeFromRight(c_skipWidth));
        row.removeFromRight(4);
        m_type.setBounds(row);
        header.removeFromTop(4);
        // The fader, then this track's pitch: semitones, cents and Vari.
        auto bottom = header.removeFromTop(juce::jlimit(12, 16, header.getHeight()));
        m_vari.setBounds(bottom.removeFromRight(46));
        bottom.removeFromRight(4);
        m_cents.setBounds(bottom.removeFromRight(38));
        bottom.removeFromRight(3);
        m_semis.setBounds(bottom.removeFromRight(40));
        bottom.removeFromRight(8);
        m_level.setBounds(bottom);
    }

    juce::Rectangle<int> waveArea() const { return getLocalBounds().withTrimmedLeft(c_headerWidth + 12).reduced(0, 6).withTrimmedRight(8); }

    void paint(juce::Graphics& g) override
    {
        const auto& fonts = m_owner.m_fonts;
        const bool active = m_owner.m_active == m_track;
        const auto colour = m_colour;
        auto bounds = getLocalBounds().toFloat().reduced(0.0f, 2.0f);

        // A lane is a plate of its own; the one in hand is lighter and its edge is in its colour.
        g.setColour(active ? TC::raised : TC::raised.withAlpha(0.45f));
        Theme::Plate::fill(g, bounds, Theme::Cut::control);
        if (active)
        {
            g.setColour(colour.withAlpha(0.75f));
            Theme::Plate::stroke(g, bounds, Theme::Cut::control);
        }
        else
            Theme::Plate::edge(g, bounds, Theme::Cut::control, TC::edgeDark.withAlpha(0.8f), TC::edgeLight.withMultipliedAlpha(0.5f));
        // The colour stripe, breathing with the ride: 4 to 6 px wide, half to full strength.
        {
            const float ride = juce::jlimit(0.0f, 1.0f, m_stripe);
            g.setColour(colour.withAlpha(0.5f + 0.5f * ride));
            g.fillRect(juce::Rectangle<float>(bounds.getX() + 6.0f, bounds.getY() + 8.0f, 4.0f + 2.0f * ride, bounds.getHeight() - 16.0f));
        }

        auto header = m_rows;
        auto nameRow = header.removeFromTop(18).withTrimmedLeft(10).withTrimmedRight(c_buttonsWidth);
        if (hasData())
        {
            // Where the track plays: a tag in that section's colour, right of the name. It follows the type until
            // a bus is chosen by hand (then it is marked *). A click opens the menu: the little arrow says so.
            const int bus = m_owner.m_hold.getBus(m_track);
            // Sent to Stowaway while it is switched off: the track waits, and its tag rests (hollow) to say so.
            const bool waits = bus == CargoHold::c_stowBus && ! m_owner.m_processor.isStowawayOn();
            const auto tagColour = waits ? TC::textDim : busColour(bus);
            const auto tagText = busTag(bus) + (m_owner.m_hold.getBusOverride(m_track) < 0 ? "" : " *");
            // A tag: a small square plate with the bus's letter on a tally in the section's colour (H for the
            // hits), the bus's name beside it, and the chevron of a menu.
            const auto tagFont = fonts.caption();
            const int arrowW = 11, tally = 16;
            const int tagW = tally + juce::roundToInt(std::ceil(Theme::textWidth(tagFont, tagText))) + 10 + arrowW;
            const auto tag = nameRow.removeFromRight(tagW).toFloat();
            nameRow.removeFromRight(6);
            m_busTagArea = tag.toNearestInt();
            g.setColour(m_busTagHover ? TC::hover : TC::well.withAlpha(0.8f));
            g.fillRect(tag);
            g.setColour(tagColour.withAlpha(m_busTagHover ? 0.95f : 0.6f));
            Theme::Plate::stroke(g, tag, 0.0f);
            const auto letter = tag.withWidth(static_cast<float>(tally));
            g.setColour(tagColour);
            if (waits)
                g.drawRect(letter.reduced(0.5f), 1.0f);
            else
                g.fillRect(letter);
            g.setColour(waits ? TC::textDim : TC::window);
            Theme::fitText(g, busLetter(bus), letter, fonts.title(), juce::Justification::centred);
            g.setColour(waits ? TC::textDim : tagColour.brighter(0.2f));
            Theme::fitText(g, tagText, tag.withTrimmedLeft(static_cast<float>(tally) + 5.0f).withTrimmedRight(static_cast<float>(arrowW) + 3.0f), tagFont, juce::Justification::centredLeft);
            juce::Path arrow;
            const float ax = tag.getRight() - 8.5f, ay = tag.getCentreY();
            arrow.startNewSubPath(ax - 3.0f, ay - 1.5f);
            arrow.lineTo(ax, ay + 1.5f);
            arrow.lineTo(ax + 3.0f, ay - 1.5f);
            g.strokePath(arrow, juce::PathStrokeType(1.4f, juce::PathStrokeType::mitered, juce::PathStrokeType::butt));
        }
        else
        {
            m_busTagArea = {};
        }
        g.setColour(TC::text);
        Theme::fitText(g, m_owner.m_hold.getDisplayName(m_track), nameRow, fonts.title(), juce::Justification::centredLeft, Theme::Fit::Name);

        const auto wave = waveArea();
        const auto wf = wave.toFloat();
        if (! hasData())
        {
            SwooshLookAndFeel::drawWell(g, wf);
            return;
        }

        // The waveform is drawn once into an image and only redrawn when the sound or the view changes;
        // every frame just adds the live parts (ride, start marker, playheads) on top.
        const float scale = g.getInternalContext().getPhysicalPixelScaleFactor();
        const bool skip = m_owner.m_hold.getSkipSilence(m_track);
        if (m_cacheData != m_data.get() || m_cacheSkip != skip || m_cacheScale != scale
            || m_cacheW0 != m_w0 || m_cacheW1 != m_w1 || m_cacheColour != colour
            || m_cache.getWidth() != juce::roundToInt(wf.getWidth() * scale) || m_cache.getHeight() != juce::roundToInt(wf.getHeight() * scale))
            renderCache(wf, scale, skip, colour);
        g.drawImage(m_cache, wf);

        const float gainNow = m_history[static_cast<size_t>((m_historyHead + c_history - 1) % c_history)];
        // The waveform brightens and dims with the fader as the ride moves it.
        g.setColour(TC::scope.withAlpha(0.55f * (1.0f - juce::jlimit(0.0f, 1.0f, gainNow))));
        g.fillRect(wf);
        Theme::Plate::wellEdge(g, wf, 0.0f); // a window cut in the lane

        // Ride meter beside the waveform
        {
            const auto strip = juce::Rectangle<float>(wf.getX() - 12.0f, wf.getY() + 2.0f, 5.0f, wf.getHeight() - 4.0f);
            g.setColour(TC::scope);
            g.fillRect(strip);
            g.setColour(colour);
            g.fillRect(strip.withTop(strip.getBottom() - strip.getHeight() * juce::jlimit(0.0f, 1.0f, gainNow)));
        }

        // Everything before the start marker is played only once you move it back.
        const float sx = xForFraction(m_owner.m_hold.getStart(m_track));
        if (sx > wf.getX() + 1.0f)
        {
            g.setColour(TC::scope.withAlpha(0.6f));
            g.fillRect(juce::Rectangle<float>(wf.getX(), wf.getY(), juce::jmin(sx, wf.getRight()) - wf.getX(), wf.getHeight()));
        }

        // The ride, like a volume automation lane scrolling past (the last few seconds).
        {
            juce::Path ride;
            const float top = wf.getY() + 4.0f, bottom = wf.getBottom() - 6.0f;
            for (int k = 0; k < c_history; ++k)
            {
                const float v = juce::jlimit(0.0f, 1.0f, m_history[static_cast<size_t>((m_historyHead + k) % c_history)]);
                const float x = wf.getX() + wf.getWidth() * static_cast<float>(k) / static_cast<float>(c_history - 1);
                const float y = bottom - (bottom - top) * v;
                if (k == 0) ride.startNewSubPath(x, y); else ride.lineTo(x, y);
            }
            g.setColour(colour.brighter(0.4f).withAlpha(0.9f));
            g.strokePath(ride, juce::PathStrokeType(1.4f));
            const float yNow = bottom - (bottom - top) * juce::jlimit(0.0f, 1.0f, gainNow);
            g.fillEllipse(juce::Rectangle<float>(7.0f, 7.0f).withCentre({ wf.getRight() - 4.0f, yNow }));
        }

        // Hit sync: the shared line every synced hit's transient sits on; it flashes on each beat.
        if (m_align > 0.0f && m_syncCount > 0)
        {
            const float lx = static_cast<float>(xForSample(m_syncFocus));
            const float flash = m_owner.m_beatFlash;
            g.setColour(TC::record.withAlpha(m_align * (0.10f + 0.20f * flash)));
            g.fillRect(juce::Rectangle<float>(lx - 5.0f, wf.getY(), 10.0f, wf.getHeight()));
            g.setColour(TC::record.withAlpha(m_align * (0.55f + 0.45f * flash)));
            const float dashes[] = { 4.0f, 3.0f };
            g.drawDashedLine(juce::Line<float>(lx, wf.getY(), lx, wf.getBottom()), dashes, 2, 1.5f);
        }

        // Start marker: a flag on a line. Drag it (or click anywhere on the waveform) to choose where the track starts.
        if (sx >= wf.getX() - 1.0f && sx <= wf.getRight() + 1.0f)
        {
            g.setColour(TC::flight);
            g.drawLine(sx, wf.getY(), sx, wf.getBottom(), 1.5f);
            juce::Path flag;
            flag.startNewSubPath(sx, wf.getY());
            flag.lineTo(sx + 10.0f, wf.getY() + 5.0f);
            flag.lineTo(sx, wf.getY() + 10.0f);
            flag.closeSubPath();
            g.fillPath(flag);
        }

        for (int h = 0; h < 2; ++h)
        {
            const float pos = m_owner.m_hold.playheads[static_cast<size_t>(m_track)][static_cast<size_t>(h)].load();
            if (pos < 0.0f) continue;
            const float px = xForFraction(pos);
            if (px < wf.getX() || px > wf.getRight()) continue;
            g.setColour(TC::text.withAlpha(0.75f));
            g.drawVerticalLine(static_cast<int>(px), wf.getY(), wf.getBottom());
        }

        const auto& d = *m_data;
        const auto infoFont = fonts.caption();
        // The line over the waveform sits on a dark plate, so it reads over the sound, the hit marks and the ride.
        const auto infoLine = wave.reduced(16, 4).withHeight(14).toFloat();
        auto drawInfo = [&](const juce::String& text, juce::Colour textColour, Theme::Fit fit)
        {
            const float width = juce::jmin(infoLine.getWidth(), Theme::fitToWidth(text, infoFont, infoLine.getWidth(), fit).width);
            g.setColour(TC::well.withAlpha(0.62f));
            Theme::Plate::fill(g, infoLine.withWidth(width).expanded(5.0f, 0.0f), Theme::Cut::small);
            g.setColour(textColour);
            Theme::fitText(g, text, infoLine, infoFont, juce::Justification::centredLeft, fit);
        };
        if (m_align > 0.5f)
        {
            const bool aimed = m_owner.m_processor.getState().getRawParameterValue(ParamIDs::hitAim)->load() > 0.5f;
            const juce::String mode = aimed ? "Aimed" : "Synced";
            drawInfo(m_syncCount == 0 ? mode + ": no hits found (try Skip Silence off)"
                                      : mode + ": hit " + juce::String(m_syncIndex + 1) + " of " + juce::String(m_syncCount)
                                            + (m_owner.m_hold.getSkipSilence(m_track) ? "" : " (whole file)"),
                     TC::record.brighter(0.3f).withAlpha(m_align), Theme::Fit::Message);
        }
        else
        {
            // The length, the sounds found, and the file it came from (cut in the middle to fit the lane).
            const auto line = wave.reduced(16, 4);
            juce::String info = juce::String(d.length() / d.sampleRate, 2) + " s   " + juce::String(static_cast<int>(d.regions.size()))
                                + (d.regions.size() == 1 ? " sound" : " sounds")
                                + (m_owner.m_hold.getSkipSilence(m_track) ? "" : "   silence kept");
            const float room = static_cast<float>(line.getWidth()) - Theme::textWidth(infoFont, info + "      ");
            const auto file = Theme::fitMiddle(d.name, infoFont, room);
            if (file.isNotEmpty())
                info << "   " << file;
            drawInfo(info, TC::textDim, Theme::Fit::Name);
        }
    }

    void renderCache(juce::Rectangle<float> wf, float scale, bool skip, juce::Colour colour)
    {
        m_cacheData = m_data.get();
        m_cacheSkip = skip;
        m_cacheScale = scale;
        m_cacheW0 = m_w0;
        m_cacheW1 = m_w1;
        m_cacheColour = colour;
        const int iw = juce::jmax(1, juce::roundToInt(wf.getWidth() * scale));
        const int ih = juce::jmax(1, juce::roundToInt(wf.getHeight() * scale));
        if (m_cache.getWidth() != iw || m_cache.getHeight() != ih)
            m_cache = juce::Image(juce::Image::ARGB, iw, ih, true);
        else
            m_cache.clear(m_cache.getBounds());
        juce::Graphics g(m_cache);
        const float w = static_cast<float>(iw), h = static_cast<float>(ih);

        g.setColour(TC::scope);
        g.fillRect(0.0f, 0.0f, w, h);

        const auto& d = *m_data;
        const int len = d.length();
        const float* src = d.audio.getReadPointer(0);
        const float midY = h * 0.5f, amp = h * 0.44f;
        // The image shows the samples m_w0..m_w1 (the whole file, or the hit sync window).
        const double w0 = m_w0, span = juce::jmax(1.0, m_w1 - m_w0);
        auto xFor = [&](int sample) { return juce::jlimit(0.0f, w, static_cast<float>(w * (sample - w0) / span)); };

        if (skip)
            for (const auto& r : d.regions)
            {
                const float x0 = xFor(r.start), x1 = xFor(r.end);
                if (x1 <= x0) continue;
                g.setColour(colour.withAlpha(0.08f));
                g.fillRect(x0, 0.0f, x1 - x0, h);
                g.setColour(colour.withAlpha(0.75f));
                g.fillRect(x0, h - 3.0f * scale, x1 - x0, 3.0f * scale);
            }

        const double spp = span / juce::jmax(1, iw);
        size_t regionIndex = 0;
        for (int px = 0; px < iw; ++px)
        {
            const double sa = w0 + px * spp, sb = w0 + (px + 1) * spp;
            if (sb <= 0.0 || sa >= static_cast<double>(len) || len <= 0)
                continue; // outside the file (the sync window can reach past either end)
            const int a = juce::jlimit(0, juce::jmax(0, len - 1), static_cast<int>(sa));
            const int b = juce::jlimit(a + 1, len, static_cast<int>(std::ceil(sb)));
            float lo = 1.0f, hi = -1.0f;
            if (spp >= c_peakBlock && ! m_peaks.empty())
            {
                for (int k = a / c_peakBlock; k <= (b - 1) / c_peakBlock && k < static_cast<int>(m_peaks.size()); ++k)
                {
                    lo = juce::jmin(lo, m_peaks[static_cast<size_t>(k)].first);
                    hi = juce::jmax(hi, m_peaks[static_cast<size_t>(k)].second);
                }
            }
            else
            {
                for (int k = a; k < b; ++k) { lo = juce::jmin(lo, src[k]); hi = juce::jmax(hi, src[k]); }
            }
            bool inside = ! skip;
            if (skip)
            {
                while (regionIndex < d.regions.size() && d.regions[regionIndex].end <= a) ++regionIndex;
                inside = regionIndex < d.regions.size() && d.regions[regionIndex].start < b;
            }
            g.setColour(inside ? colour : TC::textFaint.withAlpha(0.6f));
            g.drawVerticalLine(px, midY - juce::jmax(hi, 0.0f) * amp - 0.5f, midY - juce::jmin(lo, 0.0f) * amp + 0.5f);
        }
    }

    juce::String getTooltip() override
    {
        const auto pos = getMouseXYRelative();
        auto header = m_rows;
        if (hasData() && m_busTagArea.contains(pos))
        {
            return busTooltip();
        }
        if (header.removeFromTop(18).contains(pos))
            return "Track Name\nNamed after its sound type, until you rename it."
                   + (hasData() && m_data->name.isNotEmpty() ? " The sound is \"" + Theme::middleTruncated(m_data->name, 60) + "\"." : juce::String())
                   + "\nDouble-click to rename it. Right-click for the track's menu: Master Track, Match Pitch to Master and the bus.";
        if (hasData() && waveArea().contains(pos))
            return "Waveform\nThe flag marks where this track starts playing; it loops from there and the part before it is dimmed. "
                   "With Skip Silence on, the bright parts are the sounds it plays and the faint parts are the gaps it skips.\n"
                   "Click or drag to move the start. Double-click to put it back at the top.";
        return {};
    }

    /** What the bus tag's tooltip says: where the track plays, and how to change it. */
    juce::String busTooltip() const
    {
        {
            const int bus = m_owner.m_hold.getBus(m_track);
            const bool byHand = m_owner.m_hold.getBusOverride(m_track) >= 0;
            const bool waits = bus == CargoHold::c_stowBus && ! m_owner.m_processor.isStowawayOn();
            return juce::String("Bus\nWhere this track plays: ")
                   + (bus == CargoHold::c_hitsBus ? juce::String("the hits, which only open at the biggest moments of buses A and B.")
                      : waits ? juce::String("Stowaway, which is off, so this track waits and is not heard. Switch Stowaway on (the On / Off switch on its plate) to hear it.")
                      : bus == CargoHold::c_stowBus ? juce::String("Stowaway, the extra bus beside A and B. Its Mode decides what it does with the sound.")
                                                   : busLabel(bus) + (bus == 0 ? ", the bus that leads and ducks bus B." : ", the bus that sits under bus A."))
                   + (byHand ? " Chosen by hand (marked *): the sound type and the track's name stay as they are."
                             : " On Auto it follows the sound type.")
                   + "\nClick to choose: Auto, Bus A, Bus B, Hits or Stowaway.";
        }
    }

    // Mouse: click or drag on the waveform to place the start marker -----------------
    void mouseDown(const juce::MouseEvent& e) override
    {
        m_owner.setActiveTrack(m_track);
        if (hasData() && e.mods.isPopupMenu())
        {
            showLaneMenu();
            return;
        }
        if (hasData() && m_busTagArea.contains(e.getPosition()))
        {
            showBusMenu();
            return;
        }
        if (hasData() && waveArea().contains(e.getPosition()))
        {
            m_gesture = true;
            m_owner.m_hold.setStart(m_track, fractionForX(static_cast<float>(e.x)));
        }
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        m_gesture = false;
        computeWindow();
    }

    void mouseMove(const juce::MouseEvent& e) override
    {
        const bool over = hasData() && m_busTagArea.contains(e.getPosition());
        if (over != m_busTagHover)
        {
            m_busTagHover = over;
            setMouseCursor(over ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
            repaint(m_busTagArea.expanded(2));
        }
    }

    void mouseExit(const juce::MouseEvent& e) override { mouseMove(e.withNewPosition(juce::Point<int>(-1, -1))); }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
            return;
        if (hasData() && e.mouseDownPosition.x >= waveArea().getX())
            m_owner.m_hold.setStart(m_track, fractionForX(static_cast<float>(e.x)));
    }

    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        auto header = m_rows;
        if (header.removeFromTop(18).contains(e.getPosition()))
        {
            m_nameEditor.setText(m_owner.m_hold.getDisplayName(m_track), false);
            m_nameEditor.setVisible(true);
            m_nameEditor.grabKeyboardFocus();
            m_nameEditor.selectAll();
            return;
        }
        if (hasData() && waveArea().contains(e.getPosition()))
            m_owner.m_hold.setStart(m_track, 0.0f); // back to the top
    }

    int selStart = 0, selEnd = 0, cursor = 0;

    /** Called at the UI rate with this track's current gain (fader x ride x mute/solo). */
    void pushRide(float gain)
    {
        m_history[static_cast<size_t>(m_historyHead)] = gain;
        m_historyHead = (m_historyHead + 1) % c_history;
        // The lane's colour stripe breathes with the ride, so the left edge of the track list reads as a row of
        // lanterns. It is only repainted when the drawn width or tone would actually change.
        m_stripe += (juce::jlimit(0.0f, 1.0f, gain) - m_stripe) * 0.35f;
    }

    /** Where the colour stripe is, and whether it has moved enough to be worth redrawing. */
    juce::Rectangle<int> stripeArea() const { return getLocalBounds().withX(4).withWidth(10); }
    bool stripeMoved()
    {
        const int step = static_cast<int>(m_stripe * 12.0f);
        if (step == m_stripeShown)
            return false;
        m_stripeShown = step;
        return true;
    }

private:
    friend class CargoHoldView; // applyBusChoice does what the bus menu does
    static constexpr int c_history = 240; // 8 s at 30 Hz
    std::array<float, c_history> m_history {};
    float m_stripe = 0.0f;      // the ride, smoothed, that the colour stripe breathes with
    int m_stripeShown = -1;     // the step of it the stripe was last drawn at
    juce::Image m_cache;
    const CargoData* m_cacheData = nullptr;
    bool m_cacheSkip = true;
    float m_cacheScale = 1.0f;
    double m_cacheW0 = -1.0, m_cacheW1 = -1.0;
    juce::Colour m_cacheColour;
    juce::Colour m_colour { juce::Colours::transparentBlack }; // the lane's shade of its bus's colour
    juce::Rectangle<int> m_rows;                              // the three rows of controls (set in resized)

    // View window in samples: the whole file, or (hit sync) the shared beat window around the last hit.
    double m_w0 = 0.0, m_w1 = 1.0;
    float m_align = 0.0f;       // 0 = whole file, 1 = aligned to the hit sync line (animated)
    int m_syncRegion = -1;      // regionAt index of the hit shown
    int m_syncIndex = 0, m_syncCount = 0;
    double m_syncFocus = 0.0;   // that hit's transient (samples)
    bool m_gesture = false;     // mouse down on the waveform

    void computeWindow()
    {
        if (m_gesture)
            return; // the start marker is being dragged: hold the view still under the mouse
        const double len = static_cast<double>(juce::jmax(1, length()));
        double w0 = 0.0, w1 = len;
        m_syncCount = 0;
        if (m_align > 0.0f && hasData())
        {
            const auto& d = *m_data;
            auto& hold = m_owner.m_hold;
            const bool skip = hold.getSkipSilence(m_track);
            const float start = hold.getStart(m_track);
            m_syncCount = CargoPlayer::countRegions(d, skip, start);
            if (m_syncCount > 0)
            {
                m_syncIndex = juce::jlimit(0, m_syncCount - 1, juce::jmax(0, m_syncRegion));
                const auto r = CargoPlayer::regionAt(d, skip, start, m_syncIndex);
                // The engine's shared beat window (source seconds), plus a margin.
                // Scaled by this track's own VARI speed: a faster track covers more of its file in the same beat.
                const auto& win = m_owner.m_hitWindow;
                const double speed = static_cast<double>(m_owner.m_processor.getEngine().getTrackSpeed(m_track));
                const double pre = juce::jmax(0.01, win.pre * speed) + 0.03;
                const double post = juce::jmax(0.05, win.post * speed) + 0.03;
                m_syncFocus = r.focus;
                const float e = m_align * m_align * (3.0f - 2.0f * m_align); // smoothstep
                w0 = juce::jmap(static_cast<double>(e), 0.0, r.focus - pre * d.sampleRate);
                w1 = juce::jmap(static_cast<double>(e), len, r.focus + post * d.sampleRate);
            }
        }
        m_w0 = w0;
        m_w1 = juce::jmax(w0 + 1.0, w1);
    }
    int m_historyHead = 0;

    void commitRename()
    {
        if (! m_nameEditor.isVisible()) return;
        m_nameEditor.setVisible(false);
        auto name = m_nameEditor.getText().trim();
        // Typing the automatic name back (or nothing) returns the track to automatic naming.
        if (name == CargoHold::typeName(m_owner.m_hold.getRole(m_track)))
            name.clear();
        m_owner.m_hold.rename(m_track, name);
    }

    static juce::String busTag(int bus) { return busTitle(bus); }

    /** The letter on a track's tag: A, B, H for the hits, S for Stowaway. */
    static juce::String busLetter(int bus)
    {
        if (bus == CargoHold::c_hitsBus) return "H";
        if (bus == CargoHold::c_stowBus) return "S";
        return ParamIDs::busNames[bus == 1 ? 1 : 0];
    }

    static juce::Colour busColour(int bus)
    {
        // The colours of the sections' knobs: bus A coral, bus B turquoise, hits red, Stowaway kelp.
        if (bus == CargoHold::c_hitsBus) return TC::record;
        if (bus == CargoHold::c_stowBus) return TC::stowaway;
        return bus == 1 ? TC::sources : TC::movement;
    }

    /** As a menu item: "Bus A", "Bus B", "Hits", "Stowaway". */
    static juce::String busTitle(int bus)
    {
        if (bus == CargoHold::c_hitsBus) return "Hits";
        if (bus == CargoHold::c_stowBus) return "Stowaway";
        return juce::String("Bus ") + ParamIDs::busNames[bus == 1 ? 1 : 0];
    }

    static juce::String busLabel(int bus)
    {
        if (bus == CargoHold::c_hitsBus) return "the hits";
        if (bus == CargoHold::c_stowBus) return "Stowaway";
        return juce::String("bus ") + ParamIDs::busNames[bus == 1 ? 1 : 0];
    }

    /** The bus choices, as menu items 100 (Auto) and 101.. (Bus A, Bus B, Hits, Stowaway). */
    void addBusItems(juce::PopupMenu& menu) const
    {
        auto& hold = m_owner.m_hold;
        const int chosen = hold.getBusOverride(m_track);
        menu.addItem(100, "Auto (" + busTitle(hold.autoBus(hold.getRole(m_track))) + ", by Type)", true, chosen < 0);
        for (int bus = 0; CargoHold::isBusChoice(bus); ++bus)
            menu.addItem(101 + bus, busTitle(bus), true, chosen == bus);
    }

    void applyBusItem(int result)
    {
        if (result != 100 && ! CargoHold::isBusChoice(result - 101))
            return;
        // Sent to Stowaway while it is off: it is switched on with the same step, so the track is never left
        // waiting on a bus that is not there.
        auto& processor = m_owner.m_processor;
        const bool switchesOn = result - 101 == CargoHold::c_stowBus && ! processor.isStowawayOn();
        EditHistory::Group group(processor.getHistory(), "Track Bus");
        processor.setTrackBus(m_track, result - 101);
        if (switchesOn)
            processor.setStowawayOn(true);
        const int bus = m_owner.m_hold.getBus(m_track);
        m_owner.status("\"" + m_owner.m_hold.getDisplayName(m_track) + "\" plays on " + busLabel(bus)
                       + (result == 100 ? " (by its type)." : switchesOn ? ". Stowaway is switched on." : "."));
    }

    void showBusMenu()
    {
        juce::PopupMenu menu;
        menu.addSectionHeader("Plays On");
        addBusItems(menu);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(localAreaToGlobal(m_busTagArea)),
                           [safe = juce::Component::SafePointer<Lane>(this)](int result) { if (safe != nullptr) safe->applyBusItem(result); });
    }

    /** The lane's own menu (right-click): the Master, matching its pitch, and the bus. */
    void showLaneMenu()
    {
        auto& hold = m_owner.m_hold;
        const int master = hold.getMaster();
        juce::PopupMenu menu;
        menu.addItem(1, "Master Track", true, master == m_track);
        menu.addItem(2, "Match Pitch to Master", master >= 0 && master != m_track);
        menu.addSeparator();
        juce::PopupMenu buses;
        addBusItems(buses);
        menu.addSubMenu("Plays On", buses);
        menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition(),
                           [safe = juce::Component::SafePointer<Lane>(this)](int result)
                           {
                               if (safe == nullptr)
                                   return;
                               auto& owner = safe->m_owner;
                               if (result == 1)
                                   safe->m_master.onClick();
                               else if (result == 2)
                                   owner.status(owner.m_processor.matchPitchToMaster(safe->m_track));
                               else
                                   safe->applyBusItem(result);
                           });
    }

    void showTypeMenu()
    {
        auto& hold = m_owner.m_hold;
        const auto data = hold.get(m_track);
        const auto detected = data != nullptr ? data->detectedRole : CargoRole::Generic;
        juce::PopupMenu menu;
        // The type picks the bus too, unless the bus was chosen by hand on the bus tag.
        const bool busByHand = hold.getBusOverride(m_track) >= 0;
        menu.addSectionHeader(busByHand ? "Sound Type" : "Sound Type and Bus");
        menu.addItem(1, "Auto (Heard as " + CargoHold::typeName(detected) + ")", true, hold.getRoleOverride(m_track) < 0);
        menu.addSeparator();
        for (int r = 1; r < c_numCargoRoles; ++r)
            menu.addItem(10 + r, CargoHold::typeName(static_cast<CargoRole>(r)) + (busByHand ? juce::String() : "   -> " + busTitle(hold.autoBus(static_cast<CargoRole>(r)))),
                         true, hold.getRoleOverride(m_track) == r);
        menu.addItem(10, CargoHold::typeName(CargoRole::Generic) + (busByHand ? juce::String() : "   -> " + busTitle(hold.autoBus(CargoRole::Generic))),
                     true, hold.getRoleOverride(m_track) == 0);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(m_type), [this](int result)
        {
            if (result == 1) m_owner.m_hold.setRoleOverride(m_track, -1);
            else if (result >= 10) m_owner.m_hold.setRoleOverride(m_track, result - 10);
        });
    }

    double xForSample(double sample) const
    {
        const auto wave = waveArea().toFloat();
        return wave.getX() + (sample - m_w0) / juce::jmax(1.0, m_w1 - m_w0) * wave.getWidth();
    }

    float xForFraction(float f) const
    {
        return static_cast<float>(xForSample(juce::jlimit(0.0f, 1.0f, f) * static_cast<double>(juce::jmax(1, length()))));
    }

    float fractionForX(float x) const
    {
        const auto wave = waveArea().toFloat();
        const double t = (x - wave.getX()) / juce::jmax(1.0f, wave.getWidth());
        const double sample = m_w0 + t * (m_w1 - m_w0);
        return juce::jlimit(0.0f, 0.98f, static_cast<float>(sample / juce::jmax(1, length())));
    }

    void buildPeaks()
    {
        m_peaks.clear();
        if (m_data == nullptr) return;
        const int len = m_data->length();
        const float* src = m_data->audio.getReadPointer(0);
        m_peaks.reserve(static_cast<size_t>(len / c_peakBlock + 1));
        for (int s = 0; s < len; s += c_peakBlock)
        {
            float lo = 1.0f, hi = -1.0f;
            for (int k = s; k < juce::jmin(len, s + c_peakBlock); ++k) { lo = juce::jmin(lo, src[k]); hi = juce::jmax(hi, src[k]); }
            m_peaks.emplace_back(lo, hi);
        }
    }

    CargoHoldView& m_owner;
    int m_track;
    std::shared_ptr<const CargoData> m_data;
    std::vector<std::pair<float, float>> m_peaks;

    ValueSlider m_level;
    PitchBox m_semis, m_cents;
    juce::TextButton m_vari { "Vari" };
    juce::TextButton m_type, m_mute { "M" }, m_solo { "S" }, m_skip { "Skip Silence" };
    IconButton m_remove { IconButton::Icon::Remove, TC::hits, true };
    IconButton m_master { IconButton::Icon::Star, TC::brass, true }; // lit on the Master track
    juce::TextEditor m_nameEditor;
    juce::Rectangle<int> m_busTagArea; // set in paint(), for the tooltip and the bus menu
    bool m_busTagHover = false;
    juce::AudioProcessorValueTreeState::SliderAttachment m_levelAttachment, m_semisAttachment, m_centsAttachment;
    juce::AudioProcessorValueTreeState::ButtonAttachment m_variAttachment;
};

//==============================================================================
CargoHoldView::CargoHoldView(SwooshbucklerAudioProcessor& processor, const Theme::Fonts& fonts)
    : m_processor(processor), m_hold(processor.getCargo()), m_fonts(fonts)
{
    setWantsKeyboardFocus(true);

    // No tools: tracks are drag and drop, they loop, and a click on a waveform sets where it starts.
    const std::pair<Action, const char*> buttons[] = { { Action::Undo, "Undo" } };
    for (const auto& [action, name] : buttons)
    {
        auto b = std::make_unique<juce::TextButton>(name);
        b->setColour(juce::TextButton::buttonOnColourId, TC::sources);
        b->setTooltip("Undo\nTakes back the last change.");
        const auto a = action;
        b->onClick = [this, a] { perform(a); };
        b->setWantsKeyboardFocus(false);
        addChildComponent(*b); // kept for the keyboard shortcuts; no visible toolbar
        m_buttons.emplace_back(action, std::move(b));
    }

    for (int t = 0; t < CargoHold::c_numTracks; ++t)
    {
        m_lanes[static_cast<size_t>(t)] = std::make_unique<Lane>(*this, t);
        m_laneHolder.addChildComponent(*m_lanes[static_cast<size_t>(t)]);
        m_lanes[static_cast<size_t>(t)]->refresh();
    }

    // The empty state's way to hear the rig at once: five made-up sounds, one of each main kind.
    m_demo.setColour(juce::TextButton::buttonOnColourId, TC::sources);
    m_demo.setWantsKeyboardFocus(false);
    m_demo.setTooltip("Load Demo Sounds\nFills five tracks with sounds made on the spot: a growl, a wind bed, air swishes, hits and "
                      "a rumble, one for each part of the rig. Press Play (or Return) and you have a whoosh to shape. "
                      "Undo takes them out again, and you can drop your own sounds in beside them or over them.");
    m_demo.onClick = [this] { loadDemo(); };
    addChildComponent(m_demo);

    m_viewport.setViewedComponent(&m_laneHolder, false);
    m_viewport.onMoved = [this] { if (onLanesMoved) onLanesMoved(); };
    m_viewport.setScrollBarsShown(true, false);
    m_viewport.setScrollBarThickness(8);
    addAndMakeVisible(m_viewport);

    m_hold.addChangeListener(this);
    updateButtons();
    startTimerHz(30);
}

CargoHoldView::~CargoHoldView()
{
    m_hold.removeChangeListener(this);
}

void CargoHoldView::status(const juce::String& text)
{
    m_status = Theme::localiseShortcuts(text);
    repaint(m_statusArea);
    if (onStatus)
        onStatus(text.startsWith("!") ? text.substring(1) : text);
}

bool CargoHoldView::takeNoteShowing() const
{
    return m_takeNote.isNotEmpty() && juce::Time::getMillisecondCounterHiRes() * 0.001 - m_takeNoteTime < c_takeNoteSeconds;
}

void CargoHoldView::showTakeNote(const juce::String& text)
{
    m_takeNote = Theme::localiseShortcuts(text);
    m_takeNoteTime = juce::Time::getMillisecondCounterHiRes() * 0.001;
    repaint(m_statusArea);
}

juce::Colour CargoHoldView::laneColour(int track) const
{
    const int bus = m_hold.getBus(track);
    const auto base = bus == CargoHold::c_hitsBus ? TC::hits : bus == CargoHold::c_stowBus ? TC::stowaway : (bus == 1 ? TC::busB : TC::busA);
    int step = 0;
    for (int t = 0; t < track; ++t)
        if (m_hold.get(t) != nullptr && m_hold.getBus(t) == bus)
            ++step;
    return Theme::laneShade(base, step);
}

void CargoHoldView::paint(juce::Graphics& g)
{
    SwooshLookAndFeel::drawPanel(g, getLocalBounds().toFloat(), "Tracks",
                                 "Every track loops. Click a waveform to set where it starts.", TC::busB, m_fonts, Icons::Icon::Tracks,
                                 static_cast<float>(m_statusArea.getX() - 8));

    // The title bar's message: a take that was just saved (in brass; a click opens Takes), else the last thing said.
    if (takeNoteShowing())
    {
        const float age = static_cast<float>(juce::Time::getMillisecondCounterHiRes() * 0.001 - m_takeNoteTime);
        g.setColour(TC::brass.withAlpha(juce::jlimit(0.0f, 1.0f, static_cast<float>(c_takeNoteSeconds) - age)));
        Theme::fitText(g, m_takeNote + "  (click for Takes)", m_statusArea, m_fonts.body(), juce::Justification::centredRight, Theme::Fit::Message);
    }
    else
    {
        g.setColour(m_status.startsWith("!") ? TC::hits.brighter(0.2f) : TC::textDim);
        Theme::fitText(g, m_status.startsWith("!") ? m_status.substring(1) : m_status, m_statusArea, m_fonts.body(), juce::Justification::centredRight,
                       Theme::Fit::Message);
    }

    if (! m_dropZone.isEmpty())
    {
        const auto r = m_dropZone.toFloat();
        // The one dashed edge in the window: it says "drop here".
        g.setColour(m_dragOver ? TC::sources.withAlpha(0.12f) : TC::well.withAlpha(0.45f));
        Theme::Plate::fill(g, r, Theme::Cut::panel);
        const auto& p = Theme::Plate::shape(r.reduced(1.0f), Theme::Cut::panel);
        juce::Path dashed;
        const float dashes[] = { 6.0f, 5.0f };
        juce::PathStrokeType(1.2f, juce::PathStrokeType::mitered, juce::PathStrokeType::butt).createDashedStroke(dashed, p, dashes, 2);
        g.setColour(m_dragOver ? TC::sources : TC::stroke);
        g.fillPath(dashed);

        const int loaded = static_cast<int>(std::count_if(m_lanes.begin(), m_lanes.end(), [](const auto& l) { return l->hasData(); }));
        if (loaded > 0)
        {
            // Tracks are loaded: one slim line.
            const int more = CargoHold::c_numTracks - loaded;
            g.setColour(m_dragOver ? TC::sources : TC::textDim);
            Theme::fitText(g, "Drop more sounds here, or click to browse. " + juce::String(more) + (more == 1 ? " more track fits." : " more tracks fit."),
                           m_dropZone.reduced(10, 0), m_fonts.body(), juce::Justification::centred, Theme::Fit::Sentence);
            return;
        }
        // Empty: two lines a little above the middle, with Load Demo Sounds under them.
        g.setColour(m_dragOver ? TC::sources : TC::text);
        const int middle = m_dropZone.getCentreY() - 22;
        Theme::fitText(g, "Drop Sounds Here", juce::Rectangle<int>(m_dropZone.getX() + 8, middle - 18, m_dropZone.getWidth() - 16, 18),
                       m_fonts.title(), juce::Justification::centred);
        g.setColour(TC::textFaint);
        Theme::fitText(g, "Growls, tones, wind, engines, cloth, hits. Or click to browse. Each sound is named and routed by its type.",
                       juce::Rectangle<int>(m_dropZone.getX() + 8, middle + 1, m_dropZone.getWidth() - 16, 16), m_fonts.body(), juce::Justification::centred,
                       Theme::Fit::Sentence);
    }
}

std::map<juce::String, ModTarget*> CargoHoldView::getModTargets()
{
    std::map<juce::String, ModTarget*> targets;
    for (auto& lane : m_lanes)
        targets[ParamIDs::trackSemis[lane->getTrack()]] = lane->getSemisBox();
    return targets;
}

void CargoHoldView::resized()
{
    auto r = getLocalBounds().reduced(12, 0);
    r.removeFromTop(Theme::Space::titleBar);
    m_toolbar = {};
    for (auto& [action, b] : m_buttons)
        b->setBounds({});
    // The title bar's message: the right half, after the subtitle.
    m_statusArea = getLocalBounds().reduced(16, 0).withTrimmedTop(9).withHeight(22).withTrimmedLeft(getWidth() * 55 / 100);

    r.removeFromTop(4);
    r.removeFromBottom(10);

    std::vector<Lane*> visible;
    for (auto& lane : m_lanes)
        if (lane->hasData())
            visible.push_back(lane.get());

    // Up to four lanes share the room (never more than 110 high each); with more, four are in view, whole, and
    // the list scrolls a lane at a time (up to 10, like a Pro Tools session). Room for more: a slim drop line under them.
    constexpr int inView = 4, gap = 4;
    const int count = static_cast<int>(visible.size());
    const bool roomForMore = count < CargoHold::c_numTracks;
    const int dropHeight = roomForMore ? (visible.empty() ? r.getHeight() : c_dropLine) : 0;
    const int laneArea = r.getHeight() - dropHeight - (roomForMore && ! visible.empty() ? gap : 0);
    const int laneHeight = visible.empty() ? 0 : juce::jmin(110, laneArea / juce::jlimit(1, inView, count));
    const int shown = laneHeight * juce::jmin(count, inView);

    // Only the room the lanes actually take is given to the list, so with one or two tracks the drop line stays
    // directly under the last lane instead of floating at the panel's bottom with a hole above it.
    m_viewport.setBounds(r.removeFromTop(visible.empty() ? 0 : shown));
    m_viewport.setSingleStepSizes(16, juce::jmax(1, laneHeight));
    const int holderWidth = m_viewport.getWidth() - (count > inView ? 10 : 0);
    m_laneHolder.setSize(holderWidth, laneHeight * count);
    int y = 0;
    for (auto& lane : m_lanes)
        lane->setVisible(lane->hasData());
    for (auto* lane : visible)
    {
        lane->setBounds(0, y, holderWidth, laneHeight);
        y += laneHeight;
    }

    if (roomForMore)
    {
        if (! visible.empty()) r.removeFromTop(gap);
        m_dropZone = r.removeFromTop(dropHeight).reduced(visible.empty() ? 0 : 14, 0); // the slim line stays between the panel's bolts
    }
    else
    {
        m_dropZone = {};
    }
    // Load Demo Sounds: under the two lines of the empty drop zone.
    m_demo.setVisible(visible.empty() && m_dropZone.getHeight() >= 110);
    m_demo.setBounds(juce::Rectangle<int>(170, 26).withCentre({ m_dropZone.getCentreX(), m_dropZone.getCentreY() + 26 }));
    if (onLanesMoved)
        onLanesMoved(); // lanes appear, go and change size: their plugs move
}

void CargoHoldView::changeListenerCallback(juce::ChangeBroadcaster*)
{
    for (auto& lane : m_lanes)
        lane->refresh();
    if (! m_lanes[static_cast<size_t>(m_active)]->hasData())
        for (auto& lane : m_lanes)
            if (lane->hasData()) { m_active = lane->getTrack(); break; }
    resized();
    updateButtons();
    repaint();
}

void CargoHoldView::timerCallback()
{
    const auto& engine = m_processor.getEngine();

    // Stowaway's Mode moved the rumbles on Auto to or from it (the hold is only told a flag): the lanes show it.
    if (const bool rumbles = m_hold.getRumbleToStowaway(); rumbles != m_rumblesOnStowaway)
    {
        m_rumblesOnStowaway = rumbles;
        changeListenerCallback(nullptr);
    }
    // Stowaway switched on or off: the tags of the tracks sent to it rest or wake.
    if (const bool on = m_processor.isStowawayOn(); on != m_stowawayOn)
    {
        m_stowawayOn = on;
        m_laneHolder.repaint();
    }

    // Learn MIDI CC: the controller has moved.
    if (m_learnWaiting >= 0 && m_processor.getMidiLearnTarget() != m_learnWaiting)
    {
        const int cc = m_processor.getMidiControl(m_learnWaiting);
        if (cc >= 0)
            status(SwooshbucklerAudioProcessor::midiTargetName(m_learnWaiting) + " follows MIDI CC " + juce::String(cc) + ".");
        m_learnWaiting = -1;
    }

    // Hit sync view: the shared beat window over every hit track, and a flash on each beat.
    // Hit tracks line up their transients in the view whenever they're cued: SYNC HITS or AIM.
    const bool hitSync = m_processor.getState().getRawParameterValue(ParamIDs::hitSync)->load() > 0.5f
                      || m_processor.getState().getRawParameterValue(ParamIDs::hitAim)->load() > 0.5f;
    // The window the engine used for its last beat (pitch and caps applied), so the view matches what plays.
    m_hitWindow.pre = engine.getHitPre();
    m_hitWindow.post = engine.getHitPost();
    const int beats = engine.getHitBeats();
    m_beatFlash = beats != m_lastBeat && hitSync ? 1.0f : m_beatFlash * 0.82f;
    m_lastBeat = beats;
    for (auto& lane : m_lanes)
    {
        const int t = lane->getTrack();
        lane->updateSync(hitSync && lane->hasData() && m_hold.getBus(t) == CargoHold::c_hitsBus, engine.getHitRegion(t));
    }

    for (auto& lane : m_lanes)
    {
        const int t = lane->getTrack();
        const float level = Params::faderGain(m_processor.getState().getParameter(ParamIDs::cargoTrack[t])->getValue()); // the gain, not the fader position
        lane->pushRide(engine.getRide(t) * level * m_hold.laneGain(CargoHold::c_numBuiltIns + t));
    }
    for (auto& lane : m_lanes)
        if (lane->isVisible())
        {
            lane->repaint(lane->waveArea().withLeft(lane->waveArea().getX() - 14));
            if (lane->stripeMoved())
                lane->repaint(lane->stripeArea());
        }
    // A take's note fades out over its last second, then makes way for the last thing said.
    if (m_takeNote.isNotEmpty())
    {
        if (! takeNoteShowing())
            m_takeNote.clear();
        repaint(m_statusArea);
    }
}

void CargoHoldView::setActiveTrack(int track)
{
    if (m_active != track)
    {
        m_active = track;
        for (auto& lane : m_lanes)
            lane->repaint();
    }
    updateButtons();
    grabKeyboardFocus();
}

void CargoHoldView::updateButtons()
{
    auto& lane = *m_lanes[static_cast<size_t>(m_active)];
    const bool hasData = lane.hasData();
    const bool sel = hasData && lane.hasSelection();
    for (auto& [action, b] : m_buttons)
    {
        bool enabled = true;
        switch (action)
        {
            case Action::Cut: case Action::Copy: case Action::Duplicate: case Action::Trim: case Action::Delete:
            case Action::FadeIn: case Action::FadeOut: enabled = sel; break;
            case Action::Reverse: case Action::Normalise: enabled = hasData; break;
            case Action::Paste: enabled = m_hold.hasClipboard(); break;
            case Action::Undo: enabled = m_hold.canUndoLatest(); break;
            case Action::Redo: enabled = m_hold.canRedoLatest(); break;
            case Action::Load: break;
        }
        b->setEnabled(enabled);
    }
}

void CargoHoldView::perform(Action action)
{
    auto& lane = *m_lanes[static_cast<size_t>(m_active)];
    const int t = m_active;
    const bool sel = lane.hasSelection();
    const int s = sel ? lane.selectionStart() : 0;
    const int e = sel ? lane.selectionEnd() : lane.length();

    switch (action)
    {
        case Action::Load:      chooseFiles(); return;
        case Action::Cut:       if (sel) { m_hold.cut(t, s, e); lane.select(s, s); } break;
        case Action::Copy:      if (sel) { m_hold.copy(t, s, e); status("Copied"); } break;
        case Action::Paste:
        {
            const int at = juce::jmin(lane.cursor, lane.length());
            const int before = lane.length();
            m_hold.paste(t, at);
            const int after = m_hold.get(t) != nullptr ? m_hold.get(t)->length() : 0;
            lane.select(at, at + juce::jmax(0, after - before));
            break;
        }
        case Action::Duplicate: if (sel) { m_hold.duplicate(t, s, e); lane.select(e, e + (e - s)); } break;
        case Action::Trim:      if (sel) { m_hold.trim(t, s, e); lane.select(0, 0); } break;
        case Action::Delete:    if (sel) { m_hold.erase(t, s, e); lane.select(s, s); } break;
        case Action::Reverse:   if (lane.hasData()) m_hold.reverse(t, s, e); break;
        case Action::Normalise: if (lane.hasData()) m_hold.normalise(t, s, e); break;
        case Action::FadeIn:    if (sel) m_hold.fade(t, s, e, true); break;
        case Action::FadeOut:   if (sel) m_hold.fade(t, s, e, false); break;
        // Undo / redo follow the order of the edits across every track (so x then Cmd+Z brings that track back,
        // wherever the selection went), and select the track they acted on.
        case Action::Undo:
        case Action::Redo:
        {
            // The rig's one history: track edits and settings, in the order they were made.
            if (onHistory)
            {
                onHistory(action == Action::Redo);
                return;
            }
            if (action == Action::Undo)
                m_processor.undoEdit();
            else
                m_processor.redoEdit();
            historyStepped();
            return;
        }
    }
    lane.refresh();
    updateButtons();
}

void CargoHoldView::chooseFiles()
{
    m_chooser = std::make_unique<juce::FileChooser>("Add Sounds", juce::File::getSpecialLocation(juce::File::userDesktopDirectory),
                                                    c_audioWildcard);
    m_chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                               | juce::FileBrowserComponent::canSelectMultipleItems,
                           [this](const juce::FileChooser& fc)
                           {
                               juce::StringArray paths;
                               for (const auto& f : fc.getResults())
                                   paths.add(f.getFullPathName());
                               if (! paths.isEmpty())
                                   loadFiles(paths);
                           });
}

juce::String CargoHoldView::loadFiles(const juce::StringArray& files, int track)
{
    // Every file is accounted for in the status line: added, no track left for it, couldn't be read, or cut short.
    const auto report = m_hold.loadFiles(files, track);
    for (const int target : report.tracks)
    {
        m_lanes[static_cast<size_t>(target)]->select(0, 0);
        m_active = target;
    }

    juce::String text = report.summary();
    if (text.isNotEmpty() && report.hasProblems())
        text = "!" + text;
    if (text.isNotEmpty())
        status(text);
    resized();
    updateButtons();
    return text.startsWith("!") ? text.substring(1) : text;
}

void CargoHoldView::loadDemo()
{
    const int loaded = m_processor.loadDemoSounds();
    for (auto& lane : m_lanes)
        lane->refresh();
    for (auto& lane : m_lanes)
        if (lane->hasData()) { m_active = lane->getTrack(); break; }
    status(loaded > 0 ? "Demo sounds loaded. Press Play to hear them, and switch Stowaway on for a sub. Undo takes them out."
                      : "!Every track is in use: remove one to make room for the demo sounds.");
    resized();
    updateButtons();
    repaint();
}

void CargoHoldView::showControlMenu(int target, ValueSlider& control)
{
    const int cc = m_processor.getMidiControl(target);
    const bool waiting = m_processor.getMidiLearnTarget() == target;
    juce::PopupMenu menu;
    menu.addSectionHeader(SwooshbucklerAudioProcessor::midiTargetName(target));
    menu.addItem(1, "Type a Value...");
    menu.addSeparator();
    menu.addItem(2, waiting ? "Cancel Learn MIDI CC" : "Learn MIDI CC");
    if (cc >= 0)
        menu.addItem(3, "Forget MIDI CC " + juce::String(cc));
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(control),
                       [this, target, waiting, safeView = juce::Component::SafePointer<CargoHoldView>(this),
                        safeControl = juce::Component::SafePointer<ValueSlider>(&control)](int result)
                       {
                           if (safeView == nullptr)
                               return;
                           const auto name = SwooshbucklerAudioProcessor::midiTargetName(target);
                           if (result == 1 && safeControl != nullptr)
                               safeControl->beginTyping();
                           else if (result == 2 && waiting)
                           {
                               m_processor.learnMidiControl(-1);
                               m_learnWaiting = -1;
                               status("Learn MIDI CC cancelled.");
                           }
                           else if (result == 2)
                           {
                               m_processor.learnMidiControl(target);
                               m_learnWaiting = target;
                               status("Move a fader or knob on your MIDI controller: " + name + " will follow it.");
                           }
                           else if (result == 3)
                           {
                               m_processor.setMidiControl(target, -1);
                               status(name + " no longer follows a MIDI controller.");
                           }
                       });
}

bool CargoHoldView::laneAnchorScreen(int track, juce::Point<int>& screenPos) const
{
    if (track < 0 || track >= CargoHold::c_numTracks)
        return false;
    const auto& lane = *m_lanes[static_cast<size_t>(track)];
    if (! lane.isVisible() || ! lane.hasData() || ! Theme::isShownInWindow(*this))
        return false;
    const auto wave = lane.waveArea();
    auto point = m_viewport.getLocalPoint(&lane, juce::Point<int>(wave.getRight() - 14, wave.getCentreY()));
    point.y = juce::jlimit(4, juce::jmax(4, m_viewport.getHeight() - 4), point.y); // scrolled away: the rope ends at the list's edge
    screenPos = m_viewport.localPointToGlobal(point);
    return true;
}

bool CargoHoldView::applyBusChoice(int track, int busOrMinusOne)
{
    if (track < 0 || track >= CargoHold::c_numTracks || ! m_lanes[static_cast<size_t>(track)]->hasData())
        return false;
    m_lanes[static_cast<size_t>(track)]->applyBusItem(101 + busOrMinusOne);
    return true;
}

juce::String CargoHoldView::busTagTooltip(int track) const
{
    if (track < 0 || track >= CargoHold::c_numTracks || ! m_lanes[static_cast<size_t>(track)]->hasData())
        return {};
    return m_lanes[static_cast<size_t>(track)]->busTooltip();
}

int CargoHoldView::trackAtScreenPosition(juce::Point<int> screenPos) const
{
    for (const auto& lane : m_lanes)
        if (lane->isVisible() && lane->getScreenBounds().contains(screenPos))
            return lane->getTrack();
    return -1;
}

bool CargoHoldView::isInterestedInFileDrag(const juce::StringArray& files)
{
    for (const auto& f : files)
        if (isAudioFile(f))
            return true;
    return false;
}

void CargoHoldView::fileDragEnter(const juce::StringArray&, int, int) { m_dragOver = true; repaint(); }
void CargoHoldView::fileDragExit(const juce::StringArray&) { m_dragOver = false; repaint(); }

void CargoHoldView::filesDropped(const juce::StringArray& files, int x, int y)
{
    m_dragOver = false;
    // Dropped onto a track: replace that one first. Anywhere else: new tracks.
    int target = -1;
    for (auto& lane : m_lanes)
        if (lane->isVisible() && lane->getScreenBounds().contains(localPointToGlobal(juce::Point<int>(x, y))))
            target = lane->getTrack();
    loadFiles(files, target);
    repaint();
}

void CargoHoldView::mouseMove(const juce::MouseEvent& e)
{
    setMouseCursor(takeNoteShowing() && m_statusArea.contains(e.getPosition()) ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
}

void CargoHoldView::mouseUp(const juce::MouseEvent& e)
{
    if (takeNoteShowing() && m_statusArea.contains(e.getPosition()) && e.mouseWasClicked())
    {
        if (onTakeNoteClicked)
            onTakeNoteClicked();
        return;
    }
    if (m_dropZone.contains(e.getPosition()) && e.mouseWasClicked() && m_hold.firstEmptyTrack() >= 0)
        chooseFiles();
}

void CargoHoldView::historyStepped()
{
    const int acted = m_processor.takeRestoredTrack();
    if (acted >= 0 && m_lanes[static_cast<size_t>(acted)]->hasData())
        setActiveTrack(acted);
    for (auto& l : m_lanes)
        l->refresh();
    updateButtons();
}

bool CargoHoldView::keyPressed(const juce::KeyPress& key)
{
    const bool cmd = key.getModifiers().isCommandDown();
    const auto c = juce::CharacterFunctions::toLowerCase(key.getTextCharacter());
    if (cmd && (c == 'z' || key.getKeyCode() == 'Z'))
        return false; // undo and redo belong to the window: one history for the tracks and the settings
    if (key == juce::KeyPress::spaceKey)
    {
        if (onSwell) onSwell();
        return true;
    }
    return false;
}
