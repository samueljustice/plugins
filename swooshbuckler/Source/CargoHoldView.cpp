#include "CargoHoldView.h"
#include "LookAndFeel.h"
#include "SampleClassifier.h"

namespace TC = Theme::Colours;

namespace
{
    constexpr int c_peakBlock = 256;
    constexpr int c_headerWidth = 282; // room for the pitch boxes and VARI next to the fader
    constexpr int c_skipWidth = 96;
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
        g.setColour(TC::scope);
        g.fillRoundedRectangle(r, 3.0f);
        g.setColour(zero ? (hot ? TC::outline.brighter(0.5f) : TC::outline) : m_colour.withAlpha(hot ? 1.0f : 0.8f));
        g.drawRoundedRectangle(r, 3.0f, 1.0f);
        g.setFont(m_fonts.label(10.5f));
        g.setColour(zero ? TC::textDim : TC::text);
        g.drawFittedText(getTextFromValue(getValue()), getLocalBounds().reduced(2, 0), juce::Justification::centred, 1, 0.8f);

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
                g.fillEllipse(juce::Rectangle<float>(4.0f, 4.0f).withCentre({ x0 + w * m_modLive, r.getBottom() - 2.0f }));
            }
        }
        if (! m_wireHover.isTransparent())
        {
            g.setColour(m_wireHover.withAlpha(0.85f));
            g.drawRoundedRectangle(r.expanded(1.5f), 4.0f, 2.0f);
        }
    }

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
          m_semis(owner.m_fonts, Theme::trackColour(track), 240), m_cents(owner.m_fonts, Theme::trackColour(track), 400),
          m_levelAttachment(owner.m_processor.getState(), ParamIDs::cargoTrack[track], m_level),
          m_semisAttachment(owner.m_processor.getState(), ParamIDs::trackSemis[track], m_semis),
          m_centsAttachment(owner.m_processor.getState(), ParamIDs::trackCents[track], m_cents),
          m_variAttachment(owner.m_processor.getState(), ParamIDs::trackVari[track], m_vari)
    {
        const auto colour = Theme::trackColour(track);

        // This track's own pitch, on top of the Ride's Pitch knob.
        m_semis.caption = "Track " + juce::String(track + 1) + " Pitch";
        m_semis.setTooltip("Semitones\nThis track's own pitch, up to two octaves up or down, on top of the Ride's Pitch knob. "
                           "With VARI on it works like tape, so higher is also faster; with VARI off the speed stays the same. "
                           "Drag a MOD jack onto it to modulate the track's pitch (smoothly, not in steps); a 100% wire swings two octaves.\n"
                           "Drag up or down. Double-click to reset. Cmd-drag (Ctrl on Windows) when it's modulated to set the wire's depth, up or down."); 
        m_cents.setTooltip("Cents\nFine tuning for this track, up to a semitone either way (100 cents), on top of the semitones.\n"
                           "Drag up or down. Double-click to reset.");
        m_vari.setClickingTogglesState(true);
        m_vari.setColour(juce::TextButton::buttonOnColourId, colour);
        m_vari.setWantsKeyboardFocus(false);
        m_vari.setTooltip("Vari\nOn (the default): varispeed, like tape. The pitch and the speed change together, as with the Ride's Pitch knob. "
                          "Off: the pitch changes and the speed stays the same, through a pitch shifter. "
                          "That delays the track by about 43 ms; synced hits allow for it.\nClick to switch.");
        for (auto* c : std::initializer_list<juce::Component*> { &m_semis, &m_cents, &m_vari })
            addAndMakeVisible(c);

        m_level.setSliderStyle(juce::Slider::LinearHorizontal);
        m_level.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        m_level.setColour(juce::Slider::trackColourId, colour);
        m_level.setTooltip("Track Level\nThis track's fader, in dB (30 dB of travel, then silence at the bottom). With Auto Ride on it moves by itself and this sets how high it can go; "
                           "the strip next to the waveform shows where it is right now.\nDouble-click to reset it.");
        addAndMakeVisible(m_level);

        m_type.setColour(juce::TextButton::buttonOnColourId, colour);
        m_type.setClickingTogglesState(false);
        m_type.setToggleState(true, juce::dontSendNotification);
        m_type.onClick = [this] { showTypeMenu(); };
        addAndMakeVisible(m_type);


        for (auto* b : { &m_mute, &m_solo, &m_remove })
        {
            b->setColour(juce::TextButton::buttonOnColourId, b == &m_solo ? TC::flight : (b == &m_mute ? TC::record : TC::textDim));
            b->setWantsKeyboardFocus(false);
            addAndMakeVisible(*b);
        }
        m_mute.setTooltip("Mute\nSilences this track. It keeps its place in the ride, so it comes straight back in when you unmute it.");
        m_solo.setTooltip("Solo\nPlays only this track (and any others soloed), to hear what it brings to the whoosh.");
        m_remove.setTooltip("Remove\nThrows this track overboard and frees its slot. Cmd+Z fishes it back out.");
        const int lane = CargoHold::c_numBuiltIns + track;
        m_mute.onClick = [this, lane] { m_owner.m_hold.setMute(lane, ! m_owner.m_hold.getMute(lane)); };
        m_solo.onClick = [this, lane] { m_owner.m_hold.setSolo(lane, ! m_owner.m_hold.getSolo(lane)); };
        m_remove.onClick = [this] { m_owner.m_hold.clear(m_track); m_owner.status("Removed. Cmd+Z brings it back."); };

        m_skip.setClickingTogglesState(true);
        m_skip.setColour(juce::TextButton::buttonOnColourId, colour);
        m_skip.setWantsKeyboardFocus(false);
        m_skip.setTooltip("Skip Silence\nOn: only the sounds play and the gaps between them are skipped, so a recording full of pauses "
                          "becomes a steady source. Off: the whole file loops as it is, silence and all. "
                          "With SYNC HITS on, off fires the whole file as one hit, lined up on its first transient.");
        m_skip.onClick = [this] { m_owner.m_hold.setSkipSilence(m_track, m_skip.getToggleState()); };
        addAndMakeVisible(m_skip);

        m_nameEditor.setFont(owner.m_fonts.label(12.0f));
        m_nameEditor.setColour(juce::TextEditor::backgroundColourId, TC::scope);
        m_nameEditor.onReturnKey = [this] { commitRename(); };
        m_nameEditor.onFocusLost = [this] { commitRename(); };
        m_nameEditor.onEscapeKey = [this] { m_nameEditor.setVisible(false); };
        addChildComponent(m_nameEditor);
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
        m_type.setButtonText(CargoHold::typeName(hold.getRole(m_track)).toUpperCase() + (hold.getRoleOverride(m_track) < 0 ? "" : " *"));
        m_type.setTooltip("Sound Type: " + CargoHold::typeName(hold.getRole(m_track)) + "\n" + roleTip(hold.getRole(m_track))
                          + " Plays on " + busLabel(hold.getBus(m_track)) + "."
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
        auto top = header.removeFromTop(18);
        m_remove.setBounds(top.removeFromRight(22));
        top.removeFromRight(4);
        m_solo.setBounds(top.removeFromRight(22));
        top.removeFromRight(4);
        m_mute.setBounds(top.removeFromRight(22));
        m_nameEditor.setBounds(top.withTrimmedLeft(10));
        header.removeFromTop(4);
        // Type and skip silence side by side, always visible; the fader underneath.
        auto row = header.removeFromTop(18);
        m_skip.setBounds(row.removeFromRight(c_skipWidth));
        row.removeFromRight(4);
        m_type.setBounds(row);
        header.removeFromTop(4);
        // The fader, then this track's pitch: semitones, cents and VARI.
        auto bottom = header.removeFromTop(juce::jlimit(12, 16, header.getHeight()));
        m_vari.setBounds(bottom.removeFromRight(36));
        bottom.removeFromRight(3);
        m_cents.setBounds(bottom.removeFromRight(40));
        bottom.removeFromRight(3);
        m_semis.setBounds(bottom.removeFromRight(40));
        bottom.removeFromRight(6);
        m_level.setBounds(bottom);
    }

    juce::Rectangle<int> waveArea() const { return getLocalBounds().withTrimmedLeft(c_headerWidth + 14).reduced(0, 6).withTrimmedRight(10); }

    void paint(juce::Graphics& g) override
    {
        const auto& fonts = m_owner.m_fonts;
        const bool active = m_owner.m_active == m_track;
        const auto colour = Theme::trackColour(m_track);
        auto bounds = getLocalBounds().toFloat().reduced(0.0f, 3.0f);

        g.setColour(active ? TC::panelRaised : TC::panel);
        g.fillRoundedRectangle(bounds, 6.0f);
        g.setColour(active ? colour.withAlpha(0.7f) : TC::outline);
        g.drawRoundedRectangle(bounds.reduced(0.5f), 6.0f, 1.0f);
        g.setColour(colour);
        g.fillRoundedRectangle(bounds.getX() + 6.0f, bounds.getY() + 8.0f, 3.0f, bounds.getHeight() - 16.0f, 1.5f);

        auto header = headerArea(getLocalBounds());
        auto nameRow = header.removeFromTop(18).withTrimmedLeft(10).withTrimmedRight(86);
        if (hasData())
        {
            // Where the track plays: a tag in that section's colour, right of the name. Follows the type.
            const int bus = m_owner.m_hold.getBus(m_track);
            const auto tagColour = busColour(bus);
            const auto tagText = busTag(bus);
            g.setFont(fonts.label(10.0f));
            const int tagW = juce::roundToInt(juce::GlyphArrangement::getStringWidth(g.getCurrentFont(), tagText)) + 10;
            const auto tag = nameRow.removeFromRight(tagW).reduced(0, 2).toFloat();
            nameRow.removeFromRight(6);
            m_busTagArea = tag.toNearestInt();
            g.setColour(tagColour.withAlpha(0.16f));
            g.fillRoundedRectangle(tag, 3.0f);
            g.setColour(tagColour.withAlpha(0.8f));
            g.drawRoundedRectangle(tag.reduced(0.5f), 3.0f, 1.0f);
            g.setColour(tagColour);
            g.drawText(tagText, tag, juce::Justification::centred, false);
        }
        else
        {
            m_busTagArea = {};
        }
        g.setFont(fonts.heading(12.0f));
        g.setColour(TC::text);
        g.drawText(m_owner.m_hold.getDisplayName(m_track), nameRow, juce::Justification::centredLeft, true);

        const auto wave = waveArea();
        const auto wf = wave.toFloat();
        if (! hasData())
        {
            g.setColour(TC::scope);
            g.fillRoundedRectangle(wf, 4.0f);
            return;
        }

        // The waveform is drawn once into an image and only redrawn when the sound or the view changes;
        // every frame just adds the live parts (ride, start marker, playheads) on top.
        const float scale = g.getInternalContext().getPhysicalPixelScaleFactor();
        const bool skip = m_owner.m_hold.getSkipSilence(m_track);
        if (m_cacheData != m_data.get() || m_cacheSkip != skip || m_cacheScale != scale
            || m_cacheW0 != m_w0 || m_cacheW1 != m_w1
            || m_cache.getWidth() != juce::roundToInt(wf.getWidth() * scale) || m_cache.getHeight() != juce::roundToInt(wf.getHeight() * scale))
            renderCache(wf, scale, skip, colour);
        g.drawImage(m_cache, wf);

        const float gainNow = m_history[static_cast<size_t>((m_historyHead + c_history - 1) % c_history)];
        // The waveform brightens and dims with the fader as the ride moves it.
        g.setColour(TC::scope.withAlpha(0.55f * (1.0f - juce::jlimit(0.0f, 1.0f, gainNow))));
        g.fillRoundedRectangle(wf, 4.0f);

        // Ride meter beside the waveform
        {
            const auto strip = juce::Rectangle<float>(wf.getX() - 12.0f, wf.getY() + 2.0f, 5.0f, wf.getHeight() - 4.0f);
            g.setColour(TC::scope);
            g.fillRoundedRectangle(strip, 2.0f);
            g.setColour(colour);
            g.fillRoundedRectangle(strip.withTop(strip.getBottom() - strip.getHeight() * juce::jlimit(0.0f, 1.0f, gainNow)), 2.0f);
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
        g.setFont(fonts.body(10.5f));
        if (m_align > 0.5f)
        {
            g.setColour(TC::record.brighter(0.3f).withAlpha(m_align));
            const bool aimed = m_owner.m_processor.getState().getRawParameterValue(ParamIDs::hitAim)->load() > 0.5f;
            const juce::String mode = aimed ? "AIMED" : "SYNCED";
            g.drawText(m_syncCount == 0 ? mode + "   no hits found (try Skip Silence off)"
                                        : mode + "   hit " + juce::String(m_syncIndex + 1) + " of " + juce::String(m_syncCount)
                                              + (m_owner.m_hold.getSkipSilence(m_track) ? "" : "  (whole file)"),
                       wave.reduced(16, 4), juce::Justification::topLeft);
        }
        else
        {
            g.setColour(TC::textDim);
            g.drawText(juce::String(d.length() / d.sampleRate, 2) + " s   " + juce::String(static_cast<int>(d.regions.size()))
                           + (d.regions.size() == 1 ? " sound" : " sounds")
                           + (m_owner.m_hold.getSkipSilence(m_track) ? "" : "   silence kept"),
                       wave.reduced(16, 4), juce::Justification::topLeft);
        }
    }

    void renderCache(juce::Rectangle<float> wf, float scale, bool skip, juce::Colour colour)
    {
        m_cacheData = m_data.get();
        m_cacheSkip = skip;
        m_cacheScale = scale;
        m_cacheW0 = m_w0;
        m_cacheW1 = m_w1;
        const int iw = juce::jmax(1, juce::roundToInt(wf.getWidth() * scale));
        const int ih = juce::jmax(1, juce::roundToInt(wf.getHeight() * scale));
        if (m_cache.getWidth() != iw || m_cache.getHeight() != ih)
            m_cache = juce::Image(juce::Image::ARGB, iw, ih, true);
        else
            m_cache.clear(m_cache.getBounds());
        juce::Graphics g(m_cache);
        const float w = static_cast<float>(iw), h = static_cast<float>(ih);

        g.setColour(TC::scope);
        g.fillRoundedRectangle(0.0f, 0.0f, w, h, 4.0f * scale);

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
        auto header = headerArea(getLocalBounds());
        if (hasData() && m_busTagArea.contains(pos))
        {
            const int bus = m_owner.m_hold.getBus(m_track);
            return busTag(bus) + "\nWhere this track plays: "
                   + (bus == CargoHold::c_hitsBus ? juce::String("the hits, which only open at the biggest moments of buses A and B.")
                                                   : busLabel(bus) + (bus == 0 ? ", the bus that leads and ducks bus B." : ", the bus that sits under bus A."))
                   + " It follows the sound type.\nClick the type badge underneath to change the type, and so the bus.";
        }
        if (header.removeFromTop(18).contains(pos))
            return "Track Name\nNamed after its sound type, until you rename it.\nDouble-click to rename it.";
        if (hasData() && waveArea().contains(pos))
            return "Waveform\nThe flag marks where this track starts playing; it loops from there and the part before it is dimmed. "
                   "With Skip Silence on, the bright parts are the sounds it plays and the faint parts are the gaps it skips.\n"
                   "Click or drag to move the start. Double-click to put it back at the top.";
        return {};
    }

    // Mouse: click or drag on the waveform to place the start marker -----------------
    void mouseDown(const juce::MouseEvent& e) override
    {
        m_owner.setActiveTrack(m_track);
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

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (hasData() && e.mouseDownPosition.x >= waveArea().getX())
            m_owner.m_hold.setStart(m_track, fractionForX(static_cast<float>(e.x)));
    }

    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        auto header = headerArea(getLocalBounds());
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
    }

private:
    static constexpr int c_history = 240; // 8 s at 30 Hz
    std::array<float, c_history> m_history {};
    juce::Image m_cache;
    const CargoData* m_cacheData = nullptr;
    bool m_cacheSkip = true;
    float m_cacheScale = 1.0f;
    double m_cacheW0 = -1.0, m_cacheW1 = -1.0;

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

    static juce::String busTag(int bus)
    {
        if (bus == CargoHold::c_hitsBus) return "HITS";
        return juce::String("BUS ") + ParamIDs::busNames[juce::jlimit(0, CargoHold::c_maxBuses - 1, bus)];
    }

    static juce::Colour busColour(int bus)
    {
        // The colours of the sections' knobs: bus A coral, bus B turquoise, hits red.
        if (bus == CargoHold::c_hitsBus) return TC::record;
        return bus == 0 ? TC::movement : TC::sources;
    }

    static juce::String busLabel(int bus)
    {
        if (bus == CargoHold::c_hitsBus) return "the hits";
        return juce::String("bus ") + ParamIDs::busNames[juce::jlimit(0, CargoHold::c_maxBuses - 1, bus)];
    }

    void showTypeMenu()
    {
        auto& hold = m_owner.m_hold;
        const auto data = hold.get(m_track);
        const auto detected = data != nullptr ? data->detectedRole : CargoRole::Generic;
        juce::PopupMenu menu;
        menu.addSectionHeader("WHAT IS IT?  (this also decides where it plays)");
        menu.addItem(1, "Auto (Heard as " + CargoHold::typeName(detected) + ")", true, hold.getRoleOverride(m_track) < 0);
        menu.addSeparator();
        for (int r = 1; r < c_numCargoRoles; ++r)
            menu.addItem(10 + r, CargoHold::typeName(static_cast<CargoRole>(r)) + "   -> " + busLabel(CargoHold::busForRole(static_cast<CargoRole>(r))),
                         true, hold.getRoleOverride(m_track) == r);
        menu.addItem(10, CargoHold::typeName(CargoRole::Generic) + "   -> " + busLabel(CargoHold::busForRole(CargoRole::Generic)),
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

    juce::Slider m_level;
    PitchBox m_semis, m_cents;
    juce::TextButton m_vari { "VARI" };
    juce::TextButton m_type, m_mute { "M" }, m_solo { "S" }, m_remove { "x" }, m_skip { "SKIP SILENCE" };
    juce::TextEditor m_nameEditor;
    juce::Rectangle<int> m_busTagArea; // set in paint(), for the tooltip
    juce::AudioProcessorValueTreeState::SliderAttachment m_levelAttachment, m_semisAttachment, m_centsAttachment;
    juce::AudioProcessorValueTreeState::ButtonAttachment m_variAttachment;
};

//==============================================================================
CargoHoldView::CargoHoldView(SwooshbucklerAudioProcessor& processor, const Theme::Fonts& fonts)
    : m_processor(processor), m_hold(processor.getCargo()), m_fonts(fonts)
{
    setWantsKeyboardFocus(true);

    // No tools: tracks are drag and drop, they loop, and a click on a waveform sets where it starts.
    const std::pair<Action, const char*> buttons[] = { { Action::Undo, "UNDO" } };
    const char* tips[] = {
        "Load sounds into new tracks (or drop them anywhere here)", "Cut the selection (Cmd+X)", "Copy the selection (Cmd+C)",
        "Paste at the cursor (Cmd+V)", "Duplicate the selection right after itself (Cmd+D)",
        "Keep only the selection (T)", "Delete the selection (Backspace)", "Reverse the selection (or the whole track)",
        "Normalise the selection (or the whole track) to -1 dBFS", "Fade the selection in", "Fade the selection out",
        "Undo (Cmd+Z)", "Redo (Shift+Cmd+Z)",
    };

    int i = 0;
    for (const auto& [action, name] : buttons)
    {
        auto b = std::make_unique<juce::TextButton>(name);
        b->setColour(juce::TextButton::buttonOnColourId, TC::sources);
        b->setTooltip(tips[i++]);
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

void CargoHoldView::paint(juce::Graphics& g)
{
    SwooshLookAndFeel::drawPanel(g, getLocalBounds().toFloat(), "Tracks",
                                 "every track loops; click a waveform to set where it starts", TC::sources, m_fonts);

    g.setFont(m_fonts.body(11.0f));
    g.setColour(m_status.startsWith("!") ? TC::record : TC::textDim);
    g.drawText(m_status.startsWith("!") ? m_status.substring(1) : m_status, m_statusArea, juce::Justification::centredRight, true);

    if (! m_dropZone.isEmpty())
    {
        const auto r = m_dropZone.toFloat();
        g.setColour(m_dragOver ? TC::sources.withAlpha(0.12f) : TC::scope.withAlpha(0.6f));
        g.fillRoundedRectangle(r, 6.0f);
        juce::Path p;
        p.addRoundedRectangle(r.reduced(1.0f), 6.0f);
        juce::Path dashed;
        const float dashes[] = { 6.0f, 5.0f };
        juce::PathStrokeType(1.2f).createDashedStroke(dashed, p, dashes, 2);
        g.setColour(m_dragOver ? TC::sources : TC::outline.brighter(0.2f));
        g.fillPath(dashed);

        const int loaded = static_cast<int>(std::count_if(m_lanes.begin(), m_lanes.end(), [](const auto& l) { return l->hasData(); }));
        g.setFont(m_fonts.label(13.0f));
        g.setColour(m_dragOver ? TC::sources : TC::textDim);
        g.drawText(loaded == 0 ? "DROP SOUNDS HERE" : "DROP MORE SOUNDS HERE", m_dropZone.withTrimmedBottom(m_dropZone.getHeight() / 2 - 6),
                   juce::Justification::centredBottom);
        g.setFont(m_fonts.body(11.0f));
        g.setColour(TC::textFaint);
        g.drawText(loaded == 0 ? "growls, tones, wind, engines, cloth, hits... or click to browse. Each sound is named by type and routed"
                               : juce::String(CargoHold::c_numTracks - loaded) + (CargoHold::c_numTracks - loaded == 1 ? " more track fits" : " more tracks fit")
                                     + " (or click to browse)",
                   m_dropZone.withTrimmedTop(m_dropZone.getHeight() / 2 + 4), juce::Justification::centredTop);
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
    auto r = getLocalBounds().reduced(16, 0);
    r.removeFromTop(40);
    m_toolbar = {};
    auto bar = m_toolbar;
    for (auto& [action, b] : m_buttons)
    {
        const int w = juce::GlyphArrangement::getStringWidthInt(m_fonts.label(10.0f), b->getButtonText()) + 12;
        b->setBounds(bar.removeFromLeft(w).reduced(1, 0));
        if (action == Action::Load || action == Action::Delete || action == Action::FadeOut)
            bar.removeFromLeft(6);
    }
    m_statusArea = getLocalBounds().reduced(20, 0).withTrimmedTop(8).withHeight(22).withTrimmedLeft(460);

    r.removeFromTop(10);
    r.removeFromBottom(14);

    std::vector<Lane*> visible;
    for (auto& lane : m_lanes)
        if (lane->hasData())
            visible.push_back(lane.get());

    // Lanes stack at a comfortable height and scroll past the bottom (up to 10, like a Pro Tools session).
    const bool roomForMore = static_cast<int>(visible.size()) < CargoHold::c_numTracks;
    const int dropHeight = roomForMore ? (visible.empty() ? r.getHeight() : 44) : 0;
    const int laneArea = r.getHeight() - dropHeight - (roomForMore && ! visible.empty() ? 6 : 0);
    const int laneHeight = visible.empty() ? 0 : juce::jlimit(72, 110, laneArea / static_cast<int>(visible.size()));

    m_viewport.setBounds(r.removeFromTop(visible.empty() ? 0 : laneArea));
    const int holderWidth = m_viewport.getWidth() - (laneHeight * static_cast<int>(visible.size()) > laneArea ? 10 : 0);
    m_laneHolder.setSize(holderWidth, laneHeight * static_cast<int>(visible.size()));
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
        if (! visible.empty()) r.removeFromTop(6);
        m_dropZone = r.removeFromTop(dropHeight);
    }
    else
    {
        m_dropZone = {};
    }
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
            lane->repaint(lane->waveArea().withLeft(lane->waveArea().getX() - 14));
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

void CargoHoldView::mouseUp(const juce::MouseEvent& e)
{
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
