#pragma once

#include <JuceHeader.h>

#include <functional>
#include <map>
#include <vector>

/**
 * One undo history for everything you change: knob moves, presets, Randomise, the section dice, A/B, New Session
 * and the track edits, in the order they happened (a juce::UndoManager underneath).
 *
 *  - Parameters are recorded from their change gestures: what a parameter was when a gesture began and what it is
 *    when it ends. Gestures that are open together make one step, so a preset or a Randomise roll gliding thirty
 *    controls to their places is undone in one go. Loose single moves that follow each other within 400 ms (the
 *    mouse wheel, the morph pad's X and Y) are one step too.
 *  - Group makes everything inside it one named step, including gestures it leaves open (a glide that is still
 *    moving when the Group ends joins that step when it lands).
 *  - Anything else joins with add() (how to undo and redo it) or perform() (a juce::UndoableAction).
 *
 * Changes that arrive without a gesture (host automation, a restored session) are not recorded.
 * Message thread only; changes on other threads are ignored.
 */
class EditHistory : private juce::AudioProcessorListener
{
public:
    explicit EditHistory(juce::AudioProcessor& processor) : m_processor(processor)
    {
        m_manager.setMaxNumberOfStoredUnits(200000, 100);
        m_processor.addListener(this);
    }
    ~EditHistory() override { m_processor.removeListener(this); }

    /** Parameters (by index) that are never recorded: the transport is not an edit. */
    std::function<bool(int)> isRecorded;

    bool canUndo() const { return m_manager.canUndo(); }
    bool canRedo() const { return m_manager.canRedo(); }
    juce::String undoName() const { return m_manager.getUndoDescription(); }
    juce::String redoName() const { return m_manager.getRedoDescription(); }

    /** Takes the latest step back / does it again. `what` gets the step's name. False when there is none. */
    bool undo(juce::String* what = nullptr)
    {
        closeOpenGestures();
        m_looseAt = 0;
        if (! m_manager.canUndo())
            return false;
        if (what != nullptr)
            *what = m_manager.getUndoDescription();
        return m_manager.undo();
    }
    bool redo(juce::String* what = nullptr)
    {
        closeOpenGestures();
        m_looseAt = 0;
        if (! m_manager.canRedo())
            return false;
        if (what != nullptr)
            *what = m_manager.getRedoDescription();
        return m_manager.redo();
    }
    void clear()
    {
        m_open.clear();
        m_pending.clear();
        m_join = false;
        m_looseAt = 0;
        m_manager.clearUndoHistory();
    }
    /** True while an undo or redo is putting things back (nothing is recorded then). */
    bool isRestoring() const { return m_manager.isPerformingUndoRedo(); }

    /** Everything recorded until the matching end() is one step called `name`. */
    void begin(const juce::String& name)
    {
        if (isRestoring())
            return;
        if (m_depth++ == 0)
        {
            commit(); // what was already waiting is its own step
            m_manager.beginNewTransaction(name);
            m_join = false;
        }
    }
    void end()
    {
        if (isRestoring() || m_depth == 0)
            return;
        if (--m_depth == 0)
        {
            m_join = true;         // whatever the group recorded, or is still gliding to, belongs to its step
            if (m_open.empty())
                commit();
        }
    }
    struct Group
    {
        Group(EditHistory& h, const juce::String& name) : history(h) { history.begin(name); }
        ~Group() { history.end(); }
        EditHistory& history;
        JUCE_DECLARE_NON_COPYABLE(Group)
    };

    /** Something that has just been done, with how to take it back and how to do it again. */
    void add(const juce::String& name, std::function<void()> undoIt, std::function<void()> redoIt)
    {
        perform(new LambdaAction(std::move(undoIt), std::move(redoIt)), name);
    }
    /** An action that has just been done (its first perform() must do nothing). Takes ownership. */
    void perform(juce::UndoableAction* action, const juce::String& name)
    {
        std::unique_ptr<juce::UndoableAction> owned(action);
        if (isRestoring() || ! juce::MessageManager::existsAndIsCurrentThread())
            return;
        if (m_depth == 0 && ! m_join)
        {
            commit();
            m_manager.beginNewTransaction(name);
        }
        m_manager.perform(owned.release());
        m_looseAt = 0;
    }

private:
    struct Change
    {
        float before = 0.0f, after = 0.0f;
    };
    using Changes = std::map<int, Change>; // by parameter index

    class LambdaAction : public juce::UndoableAction
    {
    public:
        LambdaAction(std::function<void()> undoIt, std::function<void()> redoIt) : m_undo(std::move(undoIt)), m_redo(std::move(redoIt)) {}
        bool perform() override
        {
            if (m_done && m_redo)
                m_redo();
            m_done = true;
            return true;
        }
        bool undo() override
        {
            if (m_undo)
                m_undo();
            return true;
        }
    private:
        std::function<void()> m_undo, m_redo;
        bool m_done = false;
    };

    class ParameterAction : public juce::UndoableAction
    {
    public:
        ParameterAction(juce::AudioProcessor& p, Changes changes) : m_processor(p), m_changes(std::move(changes)) {}
        bool perform() override
        {
            if (m_done)
                apply(false);
            m_done = true;
            return true;
        }
        bool undo() override
        {
            apply(true);
            return true;
        }
        int getSizeInUnits() override { return 10 + static_cast<int>(m_changes.size()); }
        juce::UndoableAction* createCoalescedAction(juce::UndoableAction* next) override
        {
            auto* other = dynamic_cast<ParameterAction*>(next);
            if (other == nullptr)
                return nullptr;
            auto merged = m_changes;
            for (const auto& [index, change] : other->m_changes)
            {
                const auto it = merged.find(index);
                if (it != merged.end())
                    it->second.after = change.after;
                else
                    merged[index] = change;
            }
            auto* action = new ParameterAction(m_processor, std::move(merged));
            action->m_done = true;
            return action;
        }
    private:
        void apply(bool before)
        {
            const auto& params = m_processor.getParameters();
            for (const auto& [index, change] : m_changes)
                if (auto* param = params[index])
                {
                    const float value = before ? change.before : change.after;
                    if (std::abs(param->getValue() - value) < 1.0e-7f)
                        continue;
                    param->beginChangeGesture();
                    param->setValueNotifyingHost(value);
                    param->endChangeGesture();
                }
        }
        juce::AudioProcessor& m_processor;
        Changes m_changes;
        bool m_done = false;
    };

    bool listening(int index) const
    {
        return ! isRestoring() && juce::MessageManager::existsAndIsCurrentThread() && (! isRecorded || isRecorded(index));
    }

    void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override {}
    void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails&) override {}
    void audioProcessorParameterChangeGestureBegin(juce::AudioProcessor*, int index) override
    {
        if (! listening(index) || m_open.count(index) != 0)
            return;
        if (auto* param = m_processor.getParameters()[index])
            m_open[index] = param->getValue();
    }
    void audioProcessorParameterChangeGestureEnd(juce::AudioProcessor*, int index) override
    {
        if (! listening(index))
            return;
        const auto it = m_open.find(index);
        if (it == m_open.end())
            return;
        note(index, it->second);
        m_open.erase(it);
        if (m_open.empty() && m_depth == 0)
            commit();
    }

    void note(int index, float before)
    {
        auto* param = m_processor.getParameters()[index];
        if (param == nullptr)
            return;
        const float after = param->getValue();
        const auto it = m_pending.find(index);
        if (it != m_pending.end())
            it->second.after = after;
        else if (std::abs(after - before) > 1.0e-7f)
            m_pending[index] = { before, after };
    }

    /** A gesture still held when undo is asked for (a knob under the mouse, a glide): it counts as ended here. */
    void closeOpenGestures()
    {
        for (const auto& [index, before] : m_open)
            note(index, before);
        m_open.clear();
        commit();
    }

    void commit()
    {
        if (isRestoring())
            return;
        if (m_pending.empty())
        {
            if (m_depth == 0)
                m_join = false;
            return;
        }
        Changes changes;
        changes.swap(m_pending);
        const auto now = juce::Time::getMillisecondCounter();
        const bool loose = m_depth == 0 && ! m_join;
        if (loose)
        {
            // Quick moves in a row (the wheel, the pad's X then Y) stay one step; anything slower is a new one.
            const bool sameStep = m_looseAt != 0 && now - m_looseAt < 400;
            if (! sameStep)
            {
                juce::String name = "Change";
                if (changes.size() == 1)
                    if (auto* param = m_processor.getParameters()[changes.begin()->first])
                        name = param->getName(40);
                m_manager.beginNewTransaction(name);
            }
        }
        m_manager.perform(new ParameterAction(m_processor, std::move(changes)));
        m_looseAt = loose ? juce::jmax(static_cast<juce::uint32>(1), now) : 0;
        if (m_depth == 0)
            m_join = false;
    }

    juce::AudioProcessor& m_processor;
    juce::UndoManager m_manager;
    std::map<int, float> m_open;   // gestures in progress: parameter index -> its value when the gesture began
    Changes m_pending;             // ended gestures waiting for the others to end
    int m_depth = 0;               // open Groups
    bool m_join = false;           // the next commit belongs to the step a Group began
    juce::uint32 m_looseAt = 0;    // when the last loose parameter step was recorded (0 = the last step wasn't one)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EditHistory)
};
