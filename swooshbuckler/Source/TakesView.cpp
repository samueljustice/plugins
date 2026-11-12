#include "TakesView.h"
#include "LookAndFeel.h"

namespace TC = Theme::Colours;

TakesView::TakesView(SwooshbucklerAudioProcessor& processor, const Theme::Fonts& fonts)
    : m_processor(processor), m_fonts(fonts)
{
    m_formats.registerBasicFormats();
    for (auto* b : { &m_close, &m_folder })
    {
        b->setColour(juce::TextButton::buttonOnColourId, TC::flight);
        b->setWantsKeyboardFocus(false);
        addAndMakeVisible(*b);
    }
    m_close.onClick = [this] { setVisible(false); };
    m_folder.setTooltip(Theme::localiseShortcuts("Show Folder\nShows the folder recordings are saved to in the Finder / Explorer."));
    m_folder.onClick = [this] { m_processor.getRecordFolder().revealToUser(); };
    setMouseCursor(juce::MouseCursor::NormalCursor);
}

TakesView::~TakesView()
{
    for (auto& [id, thumb] : m_thumbs)
        thumb->removeChangeListener(this);
}

void TakesView::visibilityChanged()
{
    if (isVisible())
    {
        syncTakes();
        startTimerHz(30);
    }
    else
        stopTimer();
}

void TakesView::syncTakes()
{
    m_generation = m_processor.getTakesGeneration();
    const auto& takes = m_processor.getTakes();
    // A waveform for every take in the list; the ones that have left it are dropped.
    for (auto it = m_thumbs.begin(); it != m_thumbs.end();)
    {
        const bool listed = std::any_of(takes.begin(), takes.end(), [&](const auto& t) { return t.id == it->first; });
        if (! listed)
        {
            it->second->removeChangeListener(this);
            it = m_thumbs.erase(it);
        }
        else
            ++it;
    }
    for (const auto& take : takes)
        if (m_thumbs.find(take.id) == m_thumbs.end())
        {
            auto thumb = std::make_unique<juce::AudioThumbnail>(256, m_formats, m_cache);
            thumb->addChangeListener(this);
            thumb->setSource(new juce::FileInputSource(take.file));
            m_thumbs[take.id] = std::move(thumb);
        }
    repaint();
}

void TakesView::timerCallback()
{
    if (m_generation != m_processor.getTakesGeneration())
        syncTakes();
    const int playing = m_processor.getAuditionTake();
    if (playing >= 0 || playing != m_playing)
        repaint(m_list); // the playhead
    m_playing = playing;
}

void TakesView::resized()
{
    const int rows = SwooshbucklerAudioProcessor::c_maxTakes;
    m_panel = getLocalBounds().withSizeKeepingCentre(860, 56 + rows * c_rowHeight + 62);
    m_list = m_panel.reduced(24, 0).withTop(m_panel.getY() + 56).withHeight(rows * c_rowHeight);
    auto bottom = m_panel.reduced(24, 16).removeFromBottom(30);
    m_close.setBounds(bottom.removeFromRight(100));
    m_folder.setBounds(bottom.removeFromLeft(120));
    m_hint = bottom.reduced(12, 0);
}

TakesView::Row TakesView::rowBounds(int index) const
{
    Row r;
    r.all = m_list.withY(m_list.getY() + index * c_rowHeight).withHeight(c_rowHeight);
    auto a = r.all.reduced(4, 2);
    r.star = a.removeFromLeft(22);
    a.removeFromLeft(4);
    r.wave = a.removeFromLeft(220);
    a.removeFromLeft(10);
    r.bin = a.removeFromRight(26);
    a.removeFromRight(6);
    r.show = a.removeFromRight(50);
    a.removeFromRight(10);
    r.length = a.removeFromRight(54);
    a.removeFromRight(6);
    r.kind = a.removeFromRight(70);
    a.removeFromRight(8);
    r.name = a;
    return r;
}

int TakesView::rowAt(juce::Point<int> p, Zone& zone) const
{
    zone = Zone::None;
    if (! m_list.contains(p))
        return -1;
    const int index = (p.y - m_list.getY()) / c_rowHeight;
    if (index < 0 || index >= static_cast<int>(m_processor.getTakes().size()))
        return -1;
    const auto r = rowBounds(index);
    zone = r.star.contains(p) ? Zone::Star : r.show.contains(p) ? Zone::Show : r.bin.contains(p) ? Zone::Bin : Zone::Body;
    return index;
}

juce::String TakesView::getTooltip()
{
    Zone zone = Zone::None;
    if (rowAt(getMouseXYRelative(), zone) < 0)
        return {};
    switch (zone)
    {
        case Zone::Star: return "Keep\nStarred takes stay in this list when it is full. Without a star, the oldest take leaves the list first; its file stays where it is.";
        case Zone::Show: return Theme::localiseShortcuts("Show\nShows this file in the Finder / Explorer.");
        case Zone::Bin:  return "Delete\nMoves this file to the system trash and takes it off the list.";
        case Zone::Body: return Theme::localiseShortcuts("Take\nClick to hear it: it plays over the rig and is not recorded. Click again to stop. "
                                                         "Drag it out to the Finder / Explorer, or straight onto a track in your DAW.");
        case Zone::None: break;
    }
    return {};
}

void TakesView::mouseMove(const juce::MouseEvent& e)
{
    Zone zone = Zone::None;
    const int row = rowAt(e.getPosition(), zone);
    if (row != m_hoverRow || zone != m_hoverZone)
    {
        m_hoverRow = row;
        m_hoverZone = zone;
        setMouseCursor(row >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
        repaint(m_list);
    }
}

void TakesView::mouseExit(const juce::MouseEvent&)
{
    m_hoverRow = -1;
    m_hoverZone = Zone::None;
    repaint(m_list);
}

void TakesView::mouseDown(const juce::MouseEvent& e)
{
    m_dragged = false;
    m_pressRow = rowAt(e.getPosition(), m_pressZone);
    if (! m_panel.contains(e.getPosition()))
        setVisible(false);
}

void TakesView::mouseDrag(const juce::MouseEvent& e)
{
    // Drag a take out: to the Finder / Explorer, or onto a DAW's track. The file is copied there, never moved.
    if (m_dragged || m_pressRow < 0 || m_pressZone != Zone::Body || e.getDistanceFromDragStart() < 8)
        return;
    const auto& takes = m_processor.getTakes();
    if (m_pressRow >= static_cast<int>(takes.size()) || ! takes[static_cast<size_t>(m_pressRow)].file.existsAsFile())
        return;
    m_dragged = true;
    juce::DragAndDropContainer::performExternalDragDropOfFiles({ takes[static_cast<size_t>(m_pressRow)].file.getFullPathName() }, false, this);
}

void TakesView::mouseUp(const juce::MouseEvent& e)
{
    Zone zone = Zone::None;
    const int row = rowAt(e.getPosition(), zone);
    const int pressed = m_pressRow;
    m_pressRow = -1;
    if (m_dragged || row < 0 || row != pressed || zone != m_pressZone)
        return;
    const auto take = m_processor.getTakes()[static_cast<size_t>(row)]; // a copy: the list may change below
    auto status = [this](const juce::String& text) { if (onStatus) onStatus(text); };
    switch (zone)
    {
        case Zone::Star:
            m_processor.setTakeStarred(take.id, ! take.starred);
            break;
        case Zone::Show:
            if (take.file.existsAsFile())
                take.file.revealToUser();
            else
                status(take.file.getFileName() + " is no longer there");
            break;
        case Zone::Bin:
            if (m_processor.deleteTake(take.id))
                status("Moved " + take.file.getFileName() + " to the trash");
            else
                status("Couldn't move " + take.file.getFileName() + " to the trash");
            break;
        case Zone::Body:
            if (m_processor.getAuditionTake() == take.id)
                m_processor.stopAudition();
            else
            {
                juce::String error;
                if (! m_processor.auditionTake(take.id, error))
                    status(error);
            }
            break;
        case Zone::None:
            break;
    }
    syncTakes();
}

void TakesView::paint(juce::Graphics& g)
{
    g.fillAll(TC::background.withAlpha(0.8f));
    const auto& takes = m_processor.getTakes();
    SwooshLookAndFeel::drawPanel(g, m_panel.toFloat(), "Takes",
                                 "the last " + juce::String(SwooshbucklerAudioProcessor::c_maxTakes) + " recordings of this run, newest first",
                                 TC::flight, m_fonts);

    g.setColour(TC::scope.withAlpha(0.6f));
    g.fillRoundedRectangle(m_list.toFloat().expanded(4.0f, 2.0f), 5.0f);

    if (takes.empty())
    {
        g.setFont(m_fonts.body(13.0f));
        g.setColour(TC::textDim);
        g.drawFittedText("Nothing recorded yet. Arm the tape or press LAST 60s, and the files appear here.",
                         m_list.reduced(40, 0).withHeight(120), juce::Justification::centred, 2);
    }

    const int playing = m_processor.getAuditionTake();
    const double progress = m_processor.getAuditionProgress();
    for (int i = 0; i < static_cast<int>(takes.size()); ++i)
    {
        const auto& take = takes[static_cast<size_t>(i)];
        const auto r = rowBounds(i);
        const bool hover = i == m_hoverRow, isPlaying = take.id == playing;
        const bool missing = ! take.file.existsAsFile();
        if (isPlaying || hover)
        {
            g.setColour(isPlaying ? TC::flight.withAlpha(0.14f) : TC::hover.withAlpha(0.7f));
            g.fillRoundedRectangle(r.all.toFloat().reduced(0.0f, 1.0f), 3.0f);
        }

        // Star
        {
            juce::Path star;
            const auto c = r.star.toFloat().getCentre();
            star.addStar(c, 5, 3.4f, 7.6f);
            if (take.starred)
            {
                g.setColour(TC::flight);
                g.fillPath(star);
            }
            g.setColour(take.starred ? TC::flight : (hover && m_hoverZone == Zone::Star ? TC::text : TC::textFaint));
            g.strokePath(star, juce::PathStrokeType(1.1f));
        }

        // Waveform, with the playhead while it plays
        {
            const auto w = r.wave.toFloat();
            g.setColour(TC::scope);
            g.fillRoundedRectangle(w, 3.0f);
            const auto it = m_thumbs.find(take.id);
            if (it != m_thumbs.end() && it->second->getTotalLength() > 0.0)
            {
                g.setColour(isPlaying ? TC::flight : TC::sources.withAlpha(missing ? 0.3f : 0.85f));
                it->second->drawChannels(g, r.wave.reduced(2, 1), 0.0, it->second->getTotalLength(), 1.0f);
            }
            if (isPlaying)
            {
                g.setColour(TC::text);
                g.fillRect(juce::Rectangle<float>(w.getX() + 2.0f + (w.getWidth() - 4.0f) * static_cast<float>(progress), w.getY() + 1.0f, 1.5f, w.getHeight() - 2.0f));
            }
            g.setColour(TC::outline);
            g.drawRoundedRectangle(w.reduced(0.5f), 3.0f, 1.0f);
        }

        // Name, what it is, how long
        g.setFont(m_fonts.body(12.5f));
        g.setColour(missing ? TC::textFaint : TC::text);
        g.drawText(take.file.getFileName(), r.name, juce::Justification::centredLeft, true);
        g.setFont(m_fonts.label(9.5f));
        g.setColour(TC::textDim);
        g.drawText(missing ? juce::String("Missing") : take.kind, r.kind, juce::Justification::centredLeft, true);
        const int whole = static_cast<int>(take.seconds);
        g.setFont(m_fonts.body(12.0f));
        g.drawText(take.seconds < 60.0 ? juce::String(take.seconds, 1) + " s" : juce::String::formatted("%d:%02d", whole / 60, whole % 60),
                   r.length, juce::Justification::centredRight, true);

        // Show
        {
            const bool over = hover && m_hoverZone == Zone::Show;
            const auto b = r.show.toFloat().reduced(0.0f, 1.0f);
            g.setColour(over ? TC::panelRaised.brighter(0.1f) : TC::panelRaised);
            g.fillRoundedRectangle(b, 3.0f);
            g.setColour(TC::outline);
            g.drawRoundedRectangle(b.reduced(0.5f), 3.0f, 1.0f);
            g.setFont(m_fonts.label(10.0f));
            g.setColour(over ? TC::text : TC::textDim);
            g.drawText("Show", r.show, juce::Justification::centred, false);
        }

        // Bin
        {
            const bool over = hover && m_hoverZone == Zone::Bin;
            const auto c = r.bin.toFloat().getCentre();
            g.setColour(over ? TC::record : TC::textFaint);
            juce::Path bin;
            bin.addRoundedRectangle(c.x - 4.5f, c.y - 3.0f, 9.0f, 9.5f, 1.2f);       // the can
            bin.addRectangle(c.x - 6.0f, c.y - 5.2f, 12.0f, 1.2f);                   // the lid
            bin.addRectangle(c.x - 2.0f, c.y - 7.0f, 4.0f, 1.2f);                    // its handle
            g.strokePath(bin, juce::PathStrokeType(1.1f));
            g.drawLine(c.x - 1.6f, c.y - 0.6f, c.x - 1.6f, c.y + 4.2f, 1.0f);
            g.drawLine(c.x + 1.6f, c.y - 0.6f, c.x + 1.6f, c.y + 4.2f, 1.0f);
        }
    }

    g.setFont(m_fonts.body(11.5f));
    g.setColour(TC::textFaint);
    g.drawFittedText(Theme::localiseShortcuts("Click a take to hear it. Drag it out to the Finder / Explorer or into a DAW."),
                     m_hint, juce::Justification::centred, 1);
}
