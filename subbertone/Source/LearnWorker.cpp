#include "LearnWorker.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>

namespace subbertone::learn
{
    namespace
    {
        constexpr double c_pollMs = 25.0;
        constexpr double c_garbageMs = 1000.0;
        constexpr double c_onsetSeconds = 0.035;       // a note's pitch reaches back this far to its first audible frame
        constexpr float c_presentConfidence = 0.1f;    // a candidate line counts as present in a frame from this level
        constexpr double c_alignSearchSeconds = 0.050; // how far an alignment looks either way
        constexpr double c_alignWarmSeconds = 0.100;   // the job's band filters start from silence: skip this much
        constexpr double c_alignMatchShare = 0.9;
        constexpr int c_alignMinHops = 20;

        std::int64_t floorDiv(std::int64_t a, std::int64_t b) noexcept
        {
            const std::int64_t q = a / b;
            return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
        }

        std::int64_t ceilDiv(std::int64_t a, std::int64_t b) noexcept
        {
            return -floorDiv(-a, b);
        }

        bool same(double a, double b) noexcept
        {
            return ! (a < b) && ! (b < a);
        }

        offline::Method toOffline(Method method) noexcept
        {
            switch (method)
            {
                case Method::Engine: return offline::Method::Engine;
                case Method::Tonal:  return offline::Method::Tonal;
                case Method::Auto:   break;
            }

            return offline::Method::Auto;
        }

        // Where the analysis of a capture calls the audio silent (dBFS): the Threshold when it was captured, less a
        // margin. The region keeps it, so playback knows when the Threshold has since moved below it.
        double silenceLevelOf(const CaptureBuffer& capture) noexcept
        {
            return static_cast<double>(capture.thresholdDb) - static_cast<double>(c_silenceBelowThresholdDb);
        }

        // Frames before a note's first pitched frame, back to the silence before it (no more than c_onsetSeconds),
        // take its first pitch. The analysis needs about a period of a new note before it can name its pitch, but
        // the sub should start on that pitch as soon as the Threshold lets it sound.
        void voiceOnsets(std::vector<offline::Frame>& frames, double frameSeconds)
        {
            const int limit = static_cast<int>(std::lround(c_onsetSeconds / frameSeconds));
            const int numFrames = static_cast<int>(frames.size());

            for (int f = 1; f < numFrames; ++f)
            {
                const auto& first = frames[static_cast<std::size_t>(f)];

                if (! (first.hz > 0.0f) || frames[static_cast<std::size_t>(f - 1)].hz > 0.0f)
                    continue;

                int g = f - 1;

                while (g >= 0 && ! (frames[static_cast<std::size_t>(g)].hz > 0.0f) && ! frames[static_cast<std::size_t>(g)].silent
                       && f - g <= limit)
                    --g;

                // Only an onset: the audible run before the note starts after silence (or the capture's start) and is
                // short. A longer unpitched run, or one after another note, is left to gap filling.
                if (g >= 0 && ! frames[static_cast<std::size_t>(g)].silent)
                    continue;

                for (int k = g + 1; k < f; ++k)
                {
                    auto& frame = frames[static_cast<std::size_t>(k)];
                    frame.hz = first.hz;
                    frame.confidence = 0.0f;
                    frame.bridged = true;
                }
            }
        }

        // The candidate lines of a region and where each is present.
        void setCandidates(const offline::Analysis& analysis, Region& region)
        {
            struct Line
            {
                float ratio = 1.0f;
                double score = 0.0;
                float maxHz = 0.0f;
                const std::vector<offline::Frame>* frames = nullptr;   // null: present wherever the region has a pitch
            };

            float maxHz = 0.0f;

            for (const auto& frame : region.frames)
                if (frame.isVoiced())
                    maxHz = std::max(maxHz, std::exp(frame.logLineHz));

            std::vector<Line> lines;

            if (analysis.usedMethod == offline::Method::Engine)
            {
                // The analysis' own line and the parallel lines it scored on the same spectrogram.
                lines.push_back({ 1.0f, static_cast<double>(analysis.line.score), maxHz, nullptr });

                for (const auto& line : analysis.parallel)
                {
                    if (! (line.ratio >= c_minLineRatio && line.ratio <= c_maxLineRatio) || ! std::isfinite(line.score))
                        continue;

                    float lineMax = 0.0f;

                    for (const auto& frame : line.frames)
                        if (frame.hz > 0.0f)
                            lineMax = std::max(lineMax, frame.hz);

                    lines.push_back({ line.ratio, static_cast<double>(line.score), lineMax, &line.frames });
                }
            }
            else
            {
                // A note's octaves, unscored (score 0): the sub may be wanted an octave lower or higher.
                lines.push_back({ 1.0f, 1.0, maxHz, nullptr });
                lines.push_back({ 0.5f, 0.0, maxHz * 0.5f, nullptr });
                lines.push_back({ 2.0f, 0.0, maxHz * 2.0f, nullptr });
            }

            std::stable_sort(lines.begin(), lines.end(), [](const Line& a, const Line& b) { return a.score > b.score; });

            if (lines.size() > static_cast<std::size_t>(c_maxCandidates))
                lines.resize(static_cast<std::size_t>(c_maxCandidates));

            double best = 0.0;

            for (const auto& line : lines)
                best = std::max(best, line.score);

            region.candidates.clear();

            for (const auto& line : lines)
            {
                Candidate candidate;
                candidate.ratio = line.ratio;
                candidate.score = best > 0.0 ? static_cast<float>(std::clamp(line.score / best, 0.0, 1.0)) : 0.0f;
                candidate.maxHz = std::clamp(line.maxHz, 0.0f, c_maxCandidateHz);
                region.candidates.push_back(candidate);
            }

            region.presence.assign(region.frames.size(), 0);

            for (std::size_t f = 0; f < region.frames.size(); ++f)
            {
                unsigned bits = 0;

                for (std::size_t k = 0; k < lines.size(); ++k)
                {
                    const auto* frames = lines[k].frames;
                    const bool present = frames == nullptr
                                       ? region.frames[f].isVoiced()
                                       : (f < frames->size() && (*frames)[f].hz > 0.0f && (*frames)[f].confidence >= c_presentConfidence);

                    if (present)
                        bits |= 1u << k;
                }

                region.presence[f] = static_cast<std::uint8_t>(bits);
            }
        }

        // Worth a look in the view: the analysis could not be sure of the line.
        bool isUncertain(const Region& region, const offline::Analysis& analysis)
        {
            const Quality quality = measure(region);
            const bool engine = region.used == Method::Engine;

            if (quality.heardShare < 0.98f || quality.voicedShare < 0.5f || quality.meanConfidence < (engine ? 0.25f : 0.5f))
                return true;

            // Engine climbs to the octave above whenever that line keeps enough of the score (the perceived firing
            // rate), so a stronger line an octave below is expected, not a doubt. A doubt is a line that left the
            // analysis grid, or Auto taking Engine on a narrow agreement with the Tonal pass.
            if (! engine)
                return false;

            return ! analysis.engineOnGrid
                || (region.requested == Method::Auto && analysis.octaveAgreement >= 0.0f && analysis.octaveAgreement < 0.75f);
        }
    } // namespace

    //==============================================================================
    LearnWorker::LearnWorker()
        : juce::Thread("Subbertone Learn")
    {
        startThread(juce::Thread::Priority::low);
    }

    LearnWorker::~LearnWorker()
    {
        signalThreadShouldExit();
        notify();
        stopThread(10000);
    }

    void LearnWorker::addClient(Client& client)
    {
        const std::lock_guard<std::mutex> lock(m_lock);

        if (std::find(m_clients.begin(), m_clients.end(), &client) == m_clients.end())
            m_clients.push_back(&client);
    }

    void LearnWorker::removeClient(Client& client)
    {
        std::unique_lock<std::mutex> lock(m_lock);
        m_clients.erase(std::remove(m_clients.begin(), m_clients.end(), &client), m_clients.end());

        // Its queued jobs never run (unlike cancelled ones of a client that stays): what they would hand back goes
        // with the client.
        for (auto it = m_queue.begin(); it != m_queue.end();)
        {
            if (it->owner == &client)
            {
                it->state->cancel.store(true);
                it->state->finished.store(true);
                it = m_queue.erase(it);
            }
            else
            {
                ++it;
            }
        }

        if (m_busyWith == &client && m_running != nullptr)
            m_running->cancel.store(true);

        // A poll or job of the client's that is under way may still post (a capture it just picked up, an analysis
        // that starts itself again): post() refuses it, as the client is off the list, so once nothing of the client's
        // runs, nothing of it is left to run.
        m_idle.wait(lock, [this, &client] { return m_busyWith != &client; });
        jassert(std::none_of(m_queue.begin(), m_queue.end(), [&client](const Job& job) { return job.owner == &client; }));
    }

    void LearnWorker::post(Client& owner, std::shared_ptr<JobState> state, std::function<void(JobState&)> job)
    {
        {
            const std::lock_guard<std::mutex> lock(m_lock);

            // An instance being removed: its job would outlive the buffers and the store it works on.
            if (std::find(m_clients.begin(), m_clients.end(), &owner) == m_clients.end())
            {
                state->cancel.store(true);
                state->finished.store(true);
                return;
            }

            m_queue.push_back({ &owner, std::move(state), std::move(job) });
        }

        notify();
    }

    void LearnWorker::run()
    {
        double nextPoll = 0.0;

        while (! threadShouldExit())
        {
            if (juce::Time::getMillisecondCounterHiRes() >= nextPoll)
            {
                pollClients();
                nextPoll = juce::Time::getMillisecondCounterHiRes() + c_pollMs;
            }

            if (runOneJob())
                continue;

            const double left = nextPoll - juce::Time::getMillisecondCounterHiRes();
            wait(static_cast<int>(std::clamp(left, 1.0, c_pollMs)));
        }
    }

    void LearnWorker::pollClients()
    {
        std::vector<Client*> clients;

        try
        {
            const std::lock_guard<std::mutex> lock(m_lock);
            clients = m_clients;
        }
        catch (...)
        {
            return;   // out of memory for the copy: the next round polls them
        }

        for (auto* client : clients)
        {
            {
                const std::lock_guard<std::mutex> lock(m_lock);

                if (std::find(m_clients.begin(), m_clients.end(), client) == m_clients.end())
                    continue;

                m_busyWith = client;
            }

            try
            {
                client->pollFromWorker();
            }
            catch (...)
            {
                // As for a job: nothing may take the shared thread down, and removeClient() waits for this poll to
                // end. A poll that runs out of memory gives back what it took, so the next round tries again.
            }

            {
                const std::lock_guard<std::mutex> lock(m_lock);
                m_busyWith = nullptr;
            }

            m_idle.notify_all();
        }
    }

    bool LearnWorker::runOneJob()
    {
        Job job;

        {
            const std::lock_guard<std::mutex> lock(m_lock);

            if (m_queue.empty())
                return false;

            job = std::move(m_queue.front());
            m_queue.pop_front();
            m_busyWith = job.owner;
            m_running = job.state;
        }

        // A job cancelled while it waited still runs: it sees its flag at once and only hands back what it holds (its
        // capture buffer, the alignment hand-off), which nothing else would.
        try
        {
            job.run(*job.state);
        }
        catch (...)
        {
            // A job reports its own failures; nothing may take the shared thread down.
        }

        job.state->finished.store(true);

        {
            const std::lock_guard<std::mutex> lock(m_lock);
            m_busyWith = nullptr;
            m_running = nullptr;
        }

        m_idle.notify_all();
        return true;
    }

    //==============================================================================
    Learner::Learner(LearnStore& store, LearnAudio& audio)
        : m_store(store),
          m_audio(audio)
    {
        m_worker->addClient(*this);
    }

    Learner::~Learner()
    {
        // Nothing of this instance's may run once its buffers and store are gone.
        m_worker->removeClient(*this);
    }

    void Learner::prepare()
    {
        const std::lock_guard<std::mutex> lock(m_lock);

        if (m_audio.getSampleRate() <= 0.0)
            return;

        if (! m_store.getSnapshot()->empty())
            m_audio.ensureListening();

        if (m_armWhenPrepared)
        {
            armLocked(m_armMethod);
            return;
        }

        // A capture that was wanted at another rate was dropped by LearnAudio::prepare(): ask again.
        const auto generation = m_audio.getRequestGeneration();
        const auto capture = m_audio.getCaptureStatus();

        if (m_audio.isCaptureRequested() && capture.completedGeneration != generation)
        {
            const bool held = std::any_of(m_buffers.begin(), m_buffers.end(), [](const auto& buffer)
            {
                const int state = buffer != nullptr ? buffer->state.load() : CaptureBuffer::free;
                return state == CaptureBuffer::offered || state == CaptureBuffer::capturing;
            });

            if (! held)
            {
                const auto* record = findRecordLocked(generation);
                armLocked(record != nullptr ? record->method : Method::Auto);
            }
        }
    }

    void Learner::stateLoaded()
    {
        const std::lock_guard<std::mutex> lock(m_lock);
        stopLocked(false);
        m_outcome = Status::Outcome::none;
        m_failure = Status::Failure::none;
        m_retainedRegion = 0;
        m_retainedBuffer = nullptr;

        if (! m_store.getSnapshot()->empty())
            m_audio.ensureListening();
    }

    //==============================================================================
    bool Learner::startLearning(Method method)
    {
        const std::lock_guard<std::mutex> lock(m_lock);
        return armLocked(method);
    }

    bool Learner::armLocked(Method method)
    {
        m_interrupted = Interrupted::nothing;
        m_resumeWhenReleased = false;
        m_outcome = Status::Outcome::none;
        m_failure = Status::Failure::none;
        const double rate = m_audio.getSampleRate();

        if (rate <= 0.0 || ! m_audio.ensureListening())
        {
            // Not prepared yet: arm as soon as there is a sample rate.
            m_armWhenPrepared = true;
            m_armMethod = method;
            return true;
        }

        m_armWhenPrepared = false;

        // A capture already under way only changes its method.
        const auto generation = m_audio.getRequestGeneration();
        const auto capture = m_audio.getCaptureStatus();

        if (m_audio.isCaptureRequested() && capture.completedGeneration != generation
            && ! (capture.generation == generation && capture.noBuffer))
        {
            const bool held = std::any_of(m_buffers.begin(), m_buffers.end(), [](const auto& buffer)
            {
                const int state = buffer != nullptr ? buffer->state.load() : CaptureBuffer::free;
                return state == CaptureBuffer::offered || state == CaptureBuffer::capturing;
            });

            if (held)
            {
                if (auto* record = findRecordLocked(generation))
                    record->method = method;

                return true;
            }
        }

        CaptureBuffer* buffer = takeBufferLocked(rate);

        if (buffer == nullptr)
            return false;

        buffer->state.store(CaptureBuffer::offered, std::memory_order_release);

        ArmRecord record;
        record.method = method;
        record.epoch = m_store.getEpoch();
        record.valid = true;
        record.generation = m_audio.requestCapture(*buffer);
        m_records[m_recordWrite] = record;
        m_recordWrite = (m_recordWrite + 1) % m_records.size();
        return true;
    }

    CaptureBuffer* Learner::takeBufferLocked(double rate)
    {
        auto unused = [](const std::unique_ptr<CaptureBuffer>& buffer)
        {
            const int state = buffer->state.load(std::memory_order_acquire);
            return state == CaptureBuffer::free || state == CaptureBuffer::retained;
        };

        auto take = [this](CaptureBuffer* buffer)
        {
            if (buffer == m_retainedBuffer)
            {
                m_retainedBuffer = nullptr;
                m_retainedRegion = 0;
            }

            if (buffer == m_interruptedBuffer)
                m_interruptedBuffer = nullptr;

            return buffer;
        };

        // A free buffer at this rate first.
        for (auto& buffer : m_buffers)
            if (buffer != nullptr && same(buffer->sampleRate, rate) && buffer->state.load(std::memory_order_acquire) == CaptureBuffer::free)
                return take(buffer.get());

        bool outOfMemory = false;

        // Then a new one, while there is room for two: the audio the newest region was learned from stays kept (METHOD
        // in the view works the region out again from it), also when this capture is cancelled before it hears anything.
        for (auto& buffer : m_buffers)
        {
            if (buffer == nullptr)
            {
                try
                {
                    buffer = std::make_unique<CaptureBuffer>(rate);
                    return buffer.get();
                }
                catch (const std::bad_alloc&)
                {
                    outOfMemory = true;   // give up kept audio instead (the slot stays empty)
                    break;
                }
            }
        }

        // Then a retained one at this rate (its audio is given up): an older region's first, the newest region's last.
        for (int pass = 0; pass < 2; ++pass)
            for (auto& buffer : m_buffers)
                if (buffer != nullptr && same(buffer->sampleRate, rate)
                    && buffer->state.load(std::memory_order_acquire) == CaptureBuffer::retained
                    && (pass == 1 || buffer.get() != m_retainedBuffer))
                    return take(buffer.get());

        try
        {
            // One that nobody uses, at another rate: make it anew. (A slot may be empty here: one could not be made.)
            // The audio thread may still be looking at it, offered before a rate change: freed once it no longer is.
            for (auto& buffer : m_buffers)
            {
                if (buffer != nullptr && unused(buffer))
                {
                    take(buffer.get());
                    m_audio.withdrawOffer(*buffer);
                    buffer.reset();
                    buffer = std::make_unique<CaptureBuffer>(rate);
                    return buffer.get();
                }
            }
        }
        catch (const std::bad_alloc&)
        {
            outOfMemory = true;
        }

        // None to be had: say why when it was memory (otherwise every buffer is busy).
        if (outOfMemory)
        {
            m_outcome = Status::Outcome::failed;
            m_failure = Status::Failure::outOfMemory;
        }

        return nullptr;
    }

    Learner::ArmRecord* Learner::findRecordLocked(std::uint32_t generation)
    {
        for (auto& record : m_records)
            if (record.valid && record.generation == generation)
                return &record;

        return nullptr;
    }

    const Learner::ArmRecord* Learner::findRecordLocked(std::uint32_t generation) const
    {
        for (const auto& record : m_records)
            if (record.valid && record.generation == generation)
                return &record;

        return nullptr;
    }

    void Learner::stopLearning()
    {
        const std::lock_guard<std::mutex> lock(m_lock);
        stopLocked(true);
    }

    void Learner::stopLocked(bool keepForResume)
    {
        m_armWhenPrepared = false;
        m_resumeWhenReleased = false;
        Interrupted interrupted = Interrupted::nothing;
        const auto generation = m_audio.getRequestGeneration();
        const auto capture = m_audio.getCaptureStatus();

        if (m_audio.isCaptureRequested() && capture.completedGeneration != generation)
        {
            const auto* record = findRecordLocked(generation);
            m_interruptedMethod = record != nullptr ? record->method : Method::Auto;
            interrupted = Interrupted::capture;
            m_audio.requestStop();

            // A buffer the audio thread has not taken yet comes straight back.
            for (auto& buffer : m_buffers)
            {
                int offered = CaptureBuffer::offered;

                if (buffer != nullptr)
                    buffer->state.compare_exchange_strong(offered, CaptureBuffer::free, std::memory_order_acq_rel);
            }
        }

        // A capture complete but not picked up yet is kept, not analysed.
        for (auto& buffer : m_buffers)
        {
            int captured = CaptureBuffer::captured;

            if (buffer != nullptr && buffer->state.compare_exchange_strong(captured, CaptureBuffer::retained, std::memory_order_acq_rel))
            {
                if (const auto* record = findRecordLocked(buffer->generation))
                {
                    interrupted = Interrupted::analysis;
                    m_interruptedBuffer = buffer.get();
                    m_interruptedRecord = *record;
                    m_interruptedGapFill = static_cast<float>(c_defaultGapFillSeconds);
                }
            }
        }

        if (m_analysisJob != nullptr && ! m_analysisJob->finished.load())
        {
            m_analysisJob->cancel.store(true);
            interrupted = Interrupted::analysis;
            m_interruptedBuffer = m_analysisBuffer;

            if (! keepForResume)
                m_analysisJob = nullptr;   // its end is not news: nothing reports it
        }

        if (m_alignmentJob != nullptr && ! keepForResume)
            m_alignmentJob->cancel.store(true);

        m_interrupted = keepForResume ? interrupted : Interrupted::nothing;
    }

    bool Learner::resumeLearning()
    {
        const std::lock_guard<std::mutex> lock(m_lock);
        const auto interrupted = m_interrupted;
        m_interrupted = Interrupted::nothing;

        if (interrupted == Interrupted::capture)
            return armLocked(m_interruptedMethod);

        if (interrupted == Interrupted::analysis && m_interruptedBuffer != nullptr)
        {
            // Back to the same audio, once the cancelled job has let go of it (and if nothing reused it since).
            int retained = CaptureBuffer::retained;

            if (m_interruptedBuffer->state.compare_exchange_strong(retained, CaptureBuffer::analysing, std::memory_order_acq_rel))
            {
                if (m_interruptedBuffer->generation == m_interruptedRecord.generation)
                {
                    postAnalysisLocked(*m_interruptedBuffer, m_interruptedRecord, m_interruptedGapFill);
                    return true;
                }

                m_interruptedBuffer->state.store(CaptureBuffer::retained, std::memory_order_release);
            }
            else if (m_interruptedBuffer == m_analysisBuffer && m_interruptedBuffer->state.load() == CaptureBuffer::analysing)
            {
                // The cancelled job has not let go of it yet: it starts again as it ends.
                m_resumeWhenReleased = true;
                return true;
            }
        }

        return false;
    }

    bool Learner::forgetAll()
    {
        const std::lock_guard<std::mutex> lock(m_lock);
        stopLocked(false);
        m_outcome = Status::Outcome::none;
        m_failure = Status::Failure::none;
        return m_store.clear();   // the epoch moves on: whatever was still being learned is refused
    }

    bool Learner::forgetRegion(std::uint32_t id)
    {
        const std::lock_guard<std::mutex> lock(m_lock);   // as forgetAll(): the Learner's lock, then the store's

        if (! m_store.forget(id))
            return false;

        // What an earlier try to learn found is no news after a forget: the prompt offers its UNDO instead.
        m_outcome = Status::Outcome::none;
        m_failure = Status::Failure::none;
        return true;
    }

    bool Learner::canUndoForget() const
    {
        return m_store.canUndo();
    }

    bool Learner::undoForget()
    {
        return m_store.undo();
    }

    void Learner::setFollow(bool follow)
    {
        m_store.setFollow(follow);
    }

    bool Learner::getFollow() const
    {
        return m_store.getSnapshot()->follow;
    }

    bool Learner::setLine(std::uint32_t id, float ratio)
    {
        if (! (ratio >= c_minLineRatio && ratio <= c_maxLineRatio))
            return false;

        return m_store.update(id, [ratio](Region& region) { region.lineRatio = ratio; });
    }

    bool Learner::setGapFill(std::uint32_t id, float seconds)
    {
        if (! std::isfinite(seconds))
            return false;

        const float gapFill = std::clamp(seconds, 0.0f, static_cast<float>(c_maxGapFillSeconds));
        std::vector<std::uint32_t> ids;

        if (id != 0)
        {
            ids.push_back(id);
        }
        else
        {
            const auto snapshot = m_store.getSnapshot();   // held for the loop, as in LearnStore::update()

            for (const auto& region : snapshot->regions)
                ids.push_back(region->id);
        }

        bool any = false;

        for (const auto regionId : ids)
            any = m_store.update(regionId, [gapFill](Region& region) { refillGaps(region, gapFill); }) || any;

        return any;
    }

    bool Learner::relearn(std::uint32_t id, Method method)
    {
        const std::lock_guard<std::mutex> lock(m_lock);
        const auto snapshot = m_store.getSnapshot();
        const Region* region = snapshot->findId(id);

        if (region != nullptr && m_retainedBuffer != nullptr && m_retainedRegion == id
            && (m_analysisJob == nullptr || m_analysisJob->finished.load()))
        {
            int retained = CaptureBuffer::retained;

            if (m_retainedBuffer->state.compare_exchange_strong(retained, CaptureBuffer::analysing, std::memory_order_acq_rel))
            {
                ArmRecord record;
                record.generation = m_retainedBuffer->generation;
                record.method = method;
                record.epoch = m_store.getEpoch();
                record.valid = true;
                m_interrupted = Interrupted::nothing;
                postAnalysisLocked(*m_retainedBuffer, record, region->gapFillSeconds);
                return true;
            }
        }

        armLocked(method);
        return false;
    }

    //==============================================================================
    void Learner::postAnalysisLocked(CaptureBuffer& buffer, const ArmRecord& record, float gapFillSeconds)
    {
        // What can run out of memory comes first: if it throws, nothing here has changed, and the caller hands the
        // buffer back. (The job only looks at what follows under m_lock, which the caller holds until it is all set.)
        auto job = std::make_shared<JobState>();
        CaptureBuffer* target = &buffer;
        m_worker->post(*this, job, [this, target, record, gapFillSeconds](JobState& state)
        {
            analyse(*target, record, gapFillSeconds, state);
        });

        m_analysisJob = std::move(job);
        m_analysisBuffer = &buffer;
        m_analysisMethod = record.method;
        m_outcome = Status::Outcome::none;
        m_failure = Status::Failure::none;
        m_interruptedRecord = record;
        m_interruptedGapFill = gapFillSeconds;
    }

    void Learner::pollFromWorker()
    {
        const std::lock_guard<std::mutex> lock(m_lock);

        // A capture the audio thread completed: analyse it (one at a time per instance).
        if (m_analysisJob == nullptr || m_analysisJob->finished.load())
        {
            for (auto& buffer : m_buffers)
            {
                int captured = CaptureBuffer::captured;

                if (buffer == nullptr || ! buffer->state.compare_exchange_strong(captured, CaptureBuffer::analysing, std::memory_order_acq_rel))
                    continue;

                if (const auto* record = findRecordLocked(buffer->generation))
                {
                    try
                    {
                        postAnalysisLocked(*buffer, *record, static_cast<float>(c_defaultGapFillSeconds));
                    }
                    catch (...)
                    {
                        // Out of memory: nothing was posted, so the capture waits, complete, for the next poll.
                        buffer->state.store(CaptureBuffer::captured, std::memory_order_release);
                    }

                    break;
                }

                buffer->state.store(CaptureBuffer::free, std::memory_order_release);   // a request nobody knows
            }
        }

        // The audio thread could not take the buffer it was offered (it was reused meanwhile): offer another.
        const auto generation = m_audio.getRequestGeneration();
        const auto capture = m_audio.getCaptureStatus();

        if (m_audio.isCaptureRequested() && capture.generation == generation && capture.noBuffer
            && capture.completedGeneration != generation)
        {
            const auto* record = findRecordLocked(generation);
            armLocked(record != nullptr ? record->method : Method::Auto);
        }

        // The loop that plays is too short to learn: the capture would never complete. Stop listening and say why
        // (nothing to take up again: the loop has to change first).
        if (m_audio.isCaptureRequested() && capture.generation == generation && capture.loopTooShort
            && capture.completedGeneration != generation)
        {
            stopLocked(true);
            m_interrupted = Interrupted::nothing;
            m_outcome = Status::Outcome::failed;
            m_failure = Status::Failure::loopTooShort;
        }

        // An alignment the audio thread asked for.
        if (m_alignmentJob == nullptr || m_alignmentJob->finished.load())
        {
            if (auto request = m_audio.getAlignmentRequest())
            {
                int filled = AlignmentRequest::filled;

                if (request->state.compare_exchange_strong(filled, AlignmentRequest::taken, std::memory_order_acq_rel))
                {
                    try
                    {
                        // m_alignmentJob only once posted: a job that never runs would hold up every later request.
                        auto job = std::make_shared<JobState>();
                        m_worker->post(*this, job, [this, request](JobState& state) { align(request, state); });
                        m_alignmentJob = std::move(job);
                    }
                    catch (...)
                    {
                        // Out of memory: nothing was posted, so the request waits, filled, for the next poll.
                        request->state.store(AlignmentRequest::filled, std::memory_order_release);
                    }
                }
            }
        }

        // Memory of replaced snapshots and of what UNDO no longer offers, also while no editor is open.
        const double now = juce::Time::getMillisecondCounterHiRes();

        if (now - m_lastGarbageMs >= c_garbageMs)
        {
            m_store.collectGarbage();
            m_lastGarbageMs = now;
        }
    }

    void Learner::analyse(CaptureBuffer& buffer, ArmRecord record, float gapFillSeconds, JobState& job)
    {
        auto outcome = Status::Outcome::failed;
        auto failure = Status::Failure::none;
        std::uint32_t id = 0;

        try
        {
            AssembledCapture assembled;

            if (job.cancel.load())
            {
                failure = Status::Failure::cancelled;   // stopped while it waited its turn: only the hand-back below
            }
            else if (! assembleCapture(buffer, assembled))
            {
                failure = Status::Failure::nothingPitched;
            }
            else
            {
                offline::Settings settings;
                settings.method = toOffline(record.method);
                settings.lowestHz = c_lowestHz;
                settings.highestHz = c_highestHz;
                settings.hopSeconds = c_frameSeconds;
                settings.silenceDbfs = silenceLevelOf(buffer);   // as the Threshold, on the same signal
                settings.bridgeSeconds = gapFillSeconds;

                const auto analysis = offline::analyse(assembled.audio.data(), static_cast<int>(assembled.audio.size()),
                                                       assembled.captureRate, settings, &job.cancel,
                                                       [&job](float progress) { job.progress.store(progress); });

                if (analysis.cancelled || job.cancel.load())
                {
                    failure = Status::Failure::cancelled;
                }
                else
                {
                    Region region;

                    if (! regionFromAnalysis(buffer, assembled, analysis, record.method, gapFillSeconds, region))
                    {
                        failure = Status::Failure::nothingPitched;
                    }
                    else
                    {
                        const bool uncertain = region.uncertain;
                        const auto result = m_store.add(std::move(region), record.epoch);

                        if (result.id == 0)
                        {
                            failure = Status::Failure::notKept;
                        }
                        else
                        {
                            outcome = uncertain ? Status::Outcome::uncertain : Status::Outcome::learned;
                            id = result.id;
                        }
                    }
                }
            }
        }
        catch (...)
        {
            // Out of memory, or anything else the analysis could not get past: reported as out of memory, the nearest
            // failure LEARN shows (try again). Either way the buffer is handed back below, or it would stay busy.
            failure = Status::Failure::outOfMemory;
        }

        const std::lock_guard<std::mutex> lock(m_lock);

        if (failure == Status::Failure::cancelled && m_resumeWhenReleased && &buffer == m_interruptedBuffer
            && m_analysisJob.get() == &job)
        {
            // Taken up again while it was being stopped: start over on the same audio.
            m_resumeWhenReleased = false;

            try
            {
                postAnalysisLocked(buffer, m_interruptedRecord, m_interruptedGapFill);
                return;
            }
            catch (...)
            {
                // Out of memory: nothing was posted, so the buffer is handed back below, as from an analysis that ran
                // out of memory.
                failure = Status::Failure::outOfMemory;
            }
        }

        buffer.state.store(CaptureBuffer::retained, std::memory_order_release);

        if (id != 0)
        {
            m_retainedBuffer = &buffer;
            m_retainedRegion = id;
        }

        if (m_analysisJob.get() == &job)
        {
            m_outcome = outcome;
            m_failure = failure;
            m_analysisJob = nullptr;
            m_analysisBuffer = nullptr;
        }
    }

    void Learner::align(const std::shared_ptr<AlignmentRequest>& request, JobState& job)
    {
        // A request this job does not get to the end of (stopped, out of memory, its region gone or moved meanwhile) is
        // reported as not made, so the audio thread may ask for that region and alignment again (say after UNDO).
        const int attempt = request->attempt;
        bool slotHeld = true;
        bool made = false;

        try
        {
            if (! job.cancel.load())   // else stopped while it waited its turn
            {
                // Take the audio and give the slot back at once, so the audio thread can use it again.
                const std::vector<float> audio(request->audio.begin(),
                                               request->audio.begin() + std::clamp(request->numSamples, 0,
                                                                                   static_cast<int>(request->audio.size())));
                const auto id = request->regionId;
                const double was = request->regionAlignmentSeconds;
                const auto firstPosition = request->firstPosition;
                const int decimation = request->decimation;
                const double rate = request->sampleRate;
                request->state.store(AlignmentRequest::idle, std::memory_order_release);
                slotHeld = false;

                const auto snapshot = m_store.getSnapshot();
                const Region* region = snapshot->findId(id);

                if (region != nullptr && same(region->alignmentSeconds, was))   // else changed meanwhile
                {
                    double seconds = 0.0;
                    float share = 0.0f;
                    const bool found = findAlignment(*region, audio, firstPosition, decimation, rate, &job.cancel, seconds, share);

                    // No offset that matches is an answer too; one that does counts once it is kept.
                    if (! job.cancel.load())
                        made = ! found || m_store.update(id, [seconds, was](Region& edited)
                        {
                            if (same(edited.alignmentSeconds, was))
                                edited.alignmentSeconds = seconds;
                        });
                }
            }
        }
        catch (...)
        {
            // Out of memory: as if it had not been asked for. (No failure is shown for an alignment: the region plays
            // on where it was learned until one is made.)
        }

        if (! made && attempt >= 0 && attempt < 32)
            request->notMade.fetch_or(1u << attempt, std::memory_order_release);

        // The slot after the report, so that the audio thread, seeing it free, sees the report too.
        if (slotHeld)
            request->state.store(AlignmentRequest::idle, std::memory_order_release);
    }

    //==============================================================================
    Status Learner::getStatus() const
    {
        Status status;
        const auto snapshot = m_store.getSnapshot();
        const auto capture = m_audio.getCaptureStatus();
        const auto generation = m_audio.getRequestGeneration();
        const bool requested = m_audio.isCaptureRequested();

        {
            const std::lock_guard<std::mutex> lock(m_lock);
            const auto* record = findRecordLocked(generation);

            if (m_armWhenPrepared)
            {
                status.activity = Status::Activity::armed;
                status.method = m_armMethod;
            }
            else if (m_analysisJob != nullptr && ! m_analysisJob->finished.load())
            {
                status.activity = Status::Activity::analysing;
                status.progress = m_analysisJob->progress.load();
                status.method = m_analysisMethod;
            }
            else if (requested && capture.completedGeneration != generation)
            {
                status.method = record != nullptr ? record->method : Method::Auto;

                if (capture.generation == generation && capture.phase == LearnAudio::CapturePhase::capturing)
                {
                    status.activity = Status::Activity::capturing;
                    status.progress = capture.coverage;
                    status.heardSeconds = capture.heardSeconds;
                    status.loopSeconds = capture.loopSeconds;
                }
                else
                {
                    status.activity = Status::Activity::armed;
                }
            }
            else if (requested && std::any_of(m_buffers.begin(), m_buffers.end(), [generation](const auto& buffer)
                     {
                         // the state first: once it reads captured, the audio thread has written its last
                         return buffer != nullptr && buffer->state.load(std::memory_order_acquire) == CaptureBuffer::captured
                             && buffer->generation == generation;
                     }))
            {
                // Complete, about to be picked up.
                status.activity = Status::Activity::analysing;
                status.progress = 0.0f;
                status.method = record != nullptr ? record->method : Method::Auto;
            }

            status.outcome = m_outcome;
            status.failure = m_failure;
            status.canResume = m_interrupted != Interrupted::nothing;
        }

        status.timeline = m_audio.getTimelineState();

        if (const auto playhead = m_audio.getPlayheadSeconds())
        {
            status.playheadKnown = true;
            status.playheadSeconds = *playhead;
        }

        status.numRegions = static_cast<int>(snapshot->regions.size());
        status.learnedSeconds = snapshot->learnedSeconds();
        status.follow = snapshot->follow;
        status.canUndo = m_store.canUndo();
        status.regionHere = m_audio.getRegionHere();
        status.followingHere = m_audio.isFollowing();
        status.confidence = m_audio.getLearnedConfidence();

        const Region* shown = snapshot->findId(status.regionHere);

        if (shown == nullptr)
            for (const auto& region : snapshot->regions)
                if (shown == nullptr || shown->learnedBefore(*region))
                    shown = region.get();

        if (shown != nullptr)
        {
            status.region = shown->id;
            status.regionMethod = shown->used;
            status.regionLineRatio = shown->lineRatio;
            status.regionUncertain = shown->uncertain;
            status.regionSeconds = shown->durationSeconds();

            // The pass under way, or the one before if that fell back more: a pass starts again from none at every
            // wrap, and what no longer matches should not seem to come right at every loop start.
            const auto invalid = std::max(shown->live->invalidHops.load(std::memory_order_relaxed),
                                          shown->live->lastInvalidHops.load(std::memory_order_relaxed));
            status.regionFallbackSeconds = static_cast<double>(invalid) * static_cast<double>(shown->hopSamples) / shown->learnedRate;
            status.partlyInvalid = status.regionFallbackSeconds >= 0.2;
        }

        return status;
    }

    //==============================================================================
    bool assembleCapture(const CaptureBuffer& capture, AssembledCapture& out)
    {
        out = {};
        const int hop = capture.hopSamples;
        const int decimation = capture.decimation;
        std::int64_t from = 0;
        std::int64_t to = 0;

        if (capture.onLoop)
        {
            from = capture.loopStart;
            to = capture.loopEnd;
        }
        else if (capture.segmentToLearn >= 0 && capture.segmentToLearn < capture.numSegments)
        {
            const auto& segment = capture.segments[static_cast<std::size_t>(capture.segmentToLearn)];
            from = segment.start;
            to = segment.end;
        }
        else
        {
            return false;
        }

        // Only what was heard: a gap at either end (the block before a wrap, dropped when the host may have
        // wrapped inside it) is left out rather than filled, so the region ends where the capture really did.
        std::int64_t heardFrom = std::numeric_limits<std::int64_t>::max();
        std::int64_t heardTo = std::numeric_limits<std::int64_t>::min();

        for (int i = 0; i < capture.numSegments; ++i)
        {
            const auto& segment = capture.segments[static_cast<std::size_t>(i)];
            const std::int64_t a = std::max(segment.start, from);
            const std::int64_t b = std::min(segment.end, to);

            if (b > a && segment.numSamples > 0)
            {
                heardFrom = std::min(heardFrom, a);
                heardTo = std::max(heardTo, b);
            }
        }

        if (heardTo <= heardFrom)
            return false;

        // A loop's region starts with the hop that holds the loop start, the part before it silent and the hop
        // marked partly heard, so that every wrap lands inside the region (and the sub jumps with the input there).
        // Otherwise whole hops only.
        const std::int64_t firstHop = capture.onLoop && heardFrom == from ? floorDiv(heardFrom, hop) : ceilDiv(heardFrom, hop);
        std::int64_t endHop = floorDiv(heardTo, hop);
        endHop = std::min(endHop, firstHop + maxHopsFor(capture.sampleRate));

        if (endHop <= firstHop)
            return false;

        const std::int64_t start = firstHop * hop;
        const std::int64_t end = endHop * hop;
        const std::int64_t firstPosition = ceilDiv(start, decimation) * decimation;

        if (firstPosition >= end)
            return false;

        const auto count = static_cast<std::size_t>((end - 1 - firstPosition) / decimation + 1);
        out.audio.assign(count, 0.0f);
        std::vector<std::uint8_t> filled(count, 0);

        // Samples, the earlier segment winning where two hold the same positions (a loop heard past its start).
        for (int i = 0; i < capture.numSegments; ++i)
        {
            const auto& segment = capture.segments[static_cast<std::size_t>(i)];

            for (int k = 0; k < segment.numSamples; ++k)
            {
                const std::int64_t position = segment.firstSample + static_cast<std::int64_t>(k) * decimation;

                if (position < firstPosition || position >= end)
                    continue;

                const auto j = static_cast<std::size_t>((position - firstPosition) / decimation);

                if (filled[j] == 0)
                {
                    out.audio[j] = capture.samples[static_cast<std::size_t>(segment.sampleOffset + k)];
                    filled[j] = 1;
                }
            }
        }

        // Fingerprints: heard whole where a segment kept one; partly heard where some of the hop's samples were;
        // not heard (a gap filled with silence) otherwise.
        const auto numHops = static_cast<std::size_t>(endHop - firstHop);
        out.prints.assign(numHops, Fingerprint {});

        for (int i = 0; i < capture.numSegments; ++i)
        {
            const auto& segment = capture.segments[static_cast<std::size_t>(i)];

            for (int k = 0; k < segment.numPrints; ++k)
            {
                const std::int64_t h = segment.firstHop + k - firstHop;

                if (h < 0 || h >= static_cast<std::int64_t>(numHops))
                    continue;

                auto& print = out.prints[static_cast<std::size_t>(h)];

                if ((print.flags & Fingerprint::complete) == 0)
                    print = capture.prints[static_cast<std::size_t>(segment.printOffset + k)];
            }
        }

        for (std::size_t h = 0; h < numHops; ++h)
        {
            auto& print = out.prints[h];

            if ((print.flags & Fingerprint::complete) != 0)
                continue;

            const std::int64_t hopStart = (firstHop + static_cast<std::int64_t>(h)) * hop;
            const auto j0 = static_cast<std::size_t>(std::max<std::int64_t>(0, ceilDiv(hopStart - firstPosition, decimation)));
            const auto j1 = static_cast<std::size_t>(std::clamp<std::int64_t>(ceilDiv(hopStart + hop - firstPosition, decimation),
                                                                              0, static_cast<std::int64_t>(count)));
            const bool any = std::any_of(filled.begin() + static_cast<std::ptrdiff_t>(std::min(j0, j1)),
                                         filled.begin() + static_cast<std::ptrdiff_t>(j1),
                                         [](std::uint8_t f) { return f != 0; });
            print = Fingerprint {};
            print.flags = any ? Fingerprint::partial : 0;
        }

        // Hops the host skipped after the input fell silent (it stopped calling the plug-in until audio returned) were
        // silence: they get the print of a silent hop, heard whole, so the passage counts as heard there and audio
        // played there later falls back to live tracking, as any change does.
        const Fingerprint silence = makeFingerprint(HopEnergy {});

        for (int s = 0; s < capture.numSilentSpans; ++s)
        {
            const auto& span = capture.silentSpans[static_cast<std::size_t>(s)];

            for (std::size_t h = 0; h < numHops; ++h)
            {
                const std::int64_t hopStart = (firstHop + static_cast<std::int64_t>(h)) * hop;
                auto& print = out.prints[h];

                if (hopStart >= span.start && hopStart + hop <= span.end && (print.flags & Fingerprint::complete) == 0)
                    print = silence;
            }
        }

        out.firstPosition = firstPosition;
        out.captureRate = capture.captureRate;
        out.firstHop = firstHop;
        return true;
    }

    std::vector<Frame> framesFromCurve(std::vector<offline::Frame> curve)
    {
        voiceOnsets(curve, c_frameSeconds);
        std::vector<Frame> frames(curve.size());

        for (std::size_t i = 0; i < curve.size(); ++i)
        {
            const auto& in = curve[i];
            auto& out = frames[i];

            if (in.hz > 0.0f && std::isfinite(in.hz))
            {
                out.logLineHz = std::log(std::clamp(in.hz, c_minLineHz, c_maxLineHz));
                out.confidence = in.bridged ? 0.0f : std::clamp(in.confidence, 0.0f, 1.0f);
                out.flags = static_cast<std::uint8_t>(Frame::voiced | (in.bridged ? Frame::bridged : 0));
            }
            else
            {
                out.flags = in.silent ? Frame::silent : 0;
            }
        }

        return frames;
    }

    void refillGaps(Region& region, float gapFillSeconds)
    {
        std::vector<offline::Frame> curve(region.frames.size());

        for (std::size_t i = 0; i < curve.size(); ++i)
        {
            const auto& frame = region.frames[i];
            auto& out = curve[i];
            out.hz = frame.isVoiced() ? std::exp(frame.logLineHz) : 0.0f;
            out.confidence = frame.confidence;
            out.bridged = (frame.flags & Frame::bridged) != 0;
            out.silent = (frame.flags & Frame::silent) != 0;
        }

        offline::clearBridges(curve);
        offline::bridgeGaps(curve, c_frameSeconds, gapFillSeconds, 0.0);
        region.frames = framesFromCurve(std::move(curve));
        region.gapFillSeconds = gapFillSeconds;

        // Where the curve's voicing changed, so did where the lines that follow it are present.
        unsigned followers = 0;

        for (std::size_t k = 0; k < region.candidates.size(); ++k)
            if (std::abs(region.candidates[k].ratio - 1.0f) < 1.0e-4f || region.candidates[k].score <= 0.0f)
                followers |= 1u << k;

        region.presence.resize(region.frames.size(), 0);

        for (std::size_t f = 0; f < region.frames.size(); ++f)
        {
            auto bits = static_cast<unsigned>(region.presence[f]) & ~followers;

            if (region.frames[f].isVoiced())
                bits |= followers;
            else
                bits = 0;

            region.presence[f] = static_cast<std::uint8_t>(bits);
        }
    }

    bool regionFromAnalysis(const CaptureBuffer& capture, const AssembledCapture& assembled,
                            const offline::Analysis& analysis, Method requested, float gapFillSeconds, Region& region)
    {
        region = {};
        region.learnedRate = capture.sampleRate;
        region.hopSamples = capture.hopSamples;
        region.firstHop = assembled.firstHop;
        region.prints = assembled.prints;
        region.firstFrameSeconds = static_cast<double>(assembled.firstPosition) / capture.sampleRate;
        region.frames = framesFromCurve(analysis.line.frames);

        if (std::none_of(region.frames.begin(), region.frames.end(), [](const Frame& frame) { return frame.isVoiced(); }))
            return false;

        region.requested = requested;
        region.used = analysis.usedMethod == offline::Method::Engine ? Method::Engine : Method::Tonal;
        region.lineRatio = 1.0f;
        region.gapFillSeconds = std::clamp(gapFillSeconds, 0.0f, static_cast<float>(c_maxGapFillSeconds));
        region.alignmentSeconds = 0.0;
        region.silenceDbfs = std::clamp(static_cast<float>(silenceLevelOf(capture)), c_minSilenceDbfs, c_maxSilenceDbfs);
        setCandidates(analysis, region);
        region.uncertain = isUncertain(region, analysis);
        return true;
    }

    bool findAlignment(const Region& region, const std::vector<float>& audio, std::int64_t firstPosition, int decimation,
                       double sampleRate, const std::atomic<bool>* cancel, double& alignmentSeconds, float& matchShare)
    {
        matchShare = 0.0f;
        const auto count = static_cast<std::int64_t>(audio.size());

        // Positions line up hop for hop only at the rate it was learned at.
        if (count < 2 || ! same(region.learnedRate, sampleRate) || decimation < 1)
            return false;

        PrintFilters filters;
        filters.prepare(sampleRate);

        if (filters.getDecimation() != decimation)
            return false;

        // Each hop's energies, for any offset, from running sums.
        std::vector<PrintFilters::Bands> bands;
        filters.bandsOf(audio.data(), static_cast<int>(count), bands);
        const auto size = static_cast<std::size_t>(count) + 1;
        std::vector<double> total(size, 0.0);
        std::array<std::vector<double>, Fingerprint::numBands> bandTotal;

        for (auto& running : bandTotal)
            running.assign(size, 0.0);

        for (std::size_t i = 0; i < static_cast<std::size_t>(count); ++i)
        {
            total[i + 1] = total[i] + static_cast<double>(audio[i]) * static_cast<double>(audio[i]);

            for (std::size_t b = 0; b < bandTotal.size(); ++b)
                bandTotal[b][i + 1] = bandTotal[b][i] + bands[i][b] * bands[i][b];
        }

        const int hop = region.hopSamples;
        const std::int64_t lastPosition = firstPosition + (count - 1) * decimation;
        const std::int64_t warmUntil = firstPosition + static_cast<std::int64_t>(std::llround(c_alignWarmSeconds * sampleRate));
        const auto current = static_cast<std::int64_t>(std::llround(region.alignmentSeconds * sampleRate));
        const auto reach = static_cast<std::int64_t>(std::llround(c_alignSearchSeconds * sampleRate)) / decimation * decimation;
        const auto limit = static_cast<std::int64_t>(std::floor(c_maxAlignmentSeconds * sampleRate));

        // Each offset gets the share of hops that match (as playback would judge them), and a finer distance: how far
        // the measured level changes and band shares lie from the learned ones, before rounding. Offsets a few samples
        // apart often match equally well; the distance tells them apart.
        struct Score
        {
            std::int64_t shift = 0;
            double share = 0.0;
            double distance = 0.0;
        };

        std::vector<Score> scores;

        auto score = [&](std::int64_t shift, int& compared, double& distance)
        {
            int matched = 0;
            int previousHeard = -1;
            int previousLearned = -1;
            double previousLevel = 0.0;
            std::int64_t previousHop = -2;
            compared = 0;
            distance = 0.0;
            const std::int64_t k0 = ceilDiv(warmUntil - shift, hop);
            const std::int64_t k1 = floorDiv(lastPosition + 1 - shift, hop);

            for (std::int64_t k = k0; k < k1; ++k)
            {
                const std::int64_t h = k - region.firstHop;

                if (h < 0 || h >= region.numHops()
                    || (region.prints[static_cast<std::size_t>(h)].flags & Fingerprint::complete) == 0)
                {
                    previousHeard = -1;
                    continue;
                }

                const std::int64_t hopStart = k * hop + shift;
                const auto j0 = static_cast<std::size_t>(std::clamp<std::int64_t>(ceilDiv(hopStart - firstPosition, decimation), 0, count));
                const auto j1 = static_cast<std::size_t>(std::clamp<std::int64_t>(ceilDiv(hopStart + hop - firstPosition, decimation), 0, count));

                if (j1 <= j0)
                {
                    previousHeard = -1;
                    continue;
                }

                HopEnergy energy;
                energy.total = total[j1] - total[j0];
                energy.count = static_cast<int>(j1 - j0);

                for (std::size_t b = 0; b < bandTotal.size(); ++b)
                    energy.band[b] = bandTotal[b][j1] - bandTotal[b][j0];

                const Fingerprint heard = makeFingerprint(energy);
                const Fingerprint& stored = region.prints[static_cast<std::size_t>(h)];
                const bool chained = h == previousHop + 1 && previousHeard >= 0;
                const bool differs = printsDiffer(heard, stored, chained ? previousHeard : -1, chained ? previousLearned : -1);

                // the same comparison, unrounded
                const double mean = energy.total / energy.count + 1.0e-12;
                const double level = 10.0 * std::log10(mean);
                const double learnedLevel = static_cast<double>(stored.level) - 120.0;

                if (chained)
                    distance += std::abs((level - previousLevel) - (learnedLevel - static_cast<double>(previousLearned - 120)));

                for (std::size_t b = 0; b < bandTotal.size(); ++b)
                {
                    const double share = std::max(-60.0, 10.0 * std::log10(energy.band[b] / energy.count / mean + 1.0e-20));
                    const double learnedShare = -static_cast<double>(stored.band[b]);

                    if ((share > -30.0 || learnedShare > -30.0) && level + share > -85.0 && learnedLevel + learnedShare > -85.0)
                        distance += std::abs(share - learnedShare);
                }

                previousHeard = heard.level;
                previousLearned = stored.level;
                previousLevel = level;
                previousHop = h;
                ++compared;
                matched += differs ? 0 : 1;
            }

            if (compared > 0)
                distance /= compared;

            return compared > 0 ? static_cast<double>(matched) / compared : 0.0;
        };

        // Nearest offsets first, so that of two that score the same the smaller move wins.
        for (std::int64_t step = 0; step <= reach; step += decimation)
        {
            for (const int sign : { 1, -1 })
            {
                if (step == 0 && sign < 0)
                    continue;

                if (cancel != nullptr && cancel->load())
                    return false;

                const std::int64_t shift = current + sign * step;

                if (std::llabs(shift) > limit)
                    continue;

                int compared = 0;
                double distance = 0.0;
                const double share = score(shift, compared, distance);

                if (compared >= c_alignMinHops)
                    scores.push_back({ shift, share, distance });
            }
        }

        double bestShare = -1.0;

        for (const auto& s : scores)
            bestShare = std::max(bestShare, s.share);

        matchShare = static_cast<float>(std::max(0.0, bestShare));

        if (bestShare < c_alignMatchShare)
            return false;

        const Score* best = nullptr;

        for (const auto& s : scores)
            if (s.share >= bestShare - 0.01 && (best == nullptr || s.distance < best->distance - 1.0e-9))
                best = &s;

        alignmentSeconds = static_cast<double>(best->shift) / sampleRate;
        return true;
    }
} // namespace subbertone::learn
