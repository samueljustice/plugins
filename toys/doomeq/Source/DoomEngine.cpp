#include "DoomEngine.h"
#include "BinaryData.h"
#include "DoomAudio.h"

#include <deque>
#include <mutex>

extern "C"
{
#include "doomgeneric.h"

int dg_safe_create (int argc, char** argv);
int dg_safe_tick (void);
extern const char* dg_config_dir;
int dg_game_flags (void);
void dg_request_pause (int wanted);
}

static_assert (DOOMGENERIC_RESX == DoomEngine::width && DOOMGENERIC_RESY == DoomEngine::height);

namespace
{
    struct Shared
    {
        struct Key { int pressed; unsigned char key, typed; };
        std::mutex keyLock;
        std::deque<Key> keys;

        std::mutex frameLock;
        std::vector<uint32_t> frame = std::vector<uint32_t> ((size_t) (DoomEngine::width * DoomEngine::height), 0xff000000u);
        std::atomic<uint32_t> serial { 0 };

        std::atomic<int> state { (int) DoomEngine::State::Idle };
        juce::CriticalSection statusLock;
        juce::String status;

        // Game clock: only advances while the doom thread is running, so closing the
        // editor pauses the game instead of making it fast-forward on reopen.
        // Touched only from the doom thread.
        double accumulatedMs = 1000.0;
        double runStartMs = 0.0;
        bool clockRunning = false;

        void startClock()
        {
            if (! clockRunning)
            {
                runStartMs = juce::Time::getMillisecondCounterHiRes();
                clockRunning = true;
            }
        }

        void stopClock()
        {
            if (clockRunning)
            {
                accumulatedMs += juce::Time::getMillisecondCounterHiRes() - runStartMs;
                clockRunning = false;
            }
        }

        double nowMs() const
        {
            return accumulatedMs + (clockRunning ? juce::Time::getMillisecondCounterHiRes() - runStartMs : 0.0);
        }

        void setStatus (const juce::String& s)
        {
            const juce::ScopedLock sl (statusLock);
            status = s;
        }
    };

    // Leaked on purpose: DOOM's own globals live for the whole process too.
    Shared& shared()
    {
        static Shared* s = new Shared();
        return *s;
    }

    juce::File dataDirectory()
    {
       #if JUCE_MAC
        auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Application Support");
       #else
        auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
       #endif
        return base.getChildFile ("SammyJs DOOM EQ");
    }

    /** Unpacks the embedded Freedoom IWAD next to the config, once. */
    juce::File ensureWad()
    {
        static std::mutex wadLock;
        std::lock_guard<std::mutex> guard (wadLock);

        auto dir = dataDirectory();
        dir.createDirectory();

        juce::ZipFile zip (new juce::MemoryInputStream (BinaryData::freedoom1_zip, (size_t) BinaryData::freedoom1_zipSize, false), true);
        const auto* entry = zip.getEntry ("freedoom1.wad");
        if (entry == nullptr)
            return {};

        auto wad = dir.getChildFile ("freedoom1.wad");
        if (wad.existsAsFile() && wad.getSize() == entry->uncompressedSize)
            return wad;

        // A random temp name, so two processes unpacking at once (a DAW and the standalone
        // app, say) can't trip over each other. Whichever finishes second uses the first copy.
        {
            std::unique_ptr<juce::InputStream> in (zip.createStreamForEntry (*entry));
            juce::TemporaryFile temp (wad);
            bool written = false;
            {
                juce::FileOutputStream out (temp.getFile());
                if (in != nullptr && out.openedOk())
                    written = out.writeFromInputStream (*in, -1) == entry->uncompressedSize && out.getStatus().wasOk();
            }
            if (written)
                temp.overwriteTargetFileWithTemporary();
        }

        return (wad.existsAsFile() && wad.getSize() == entry->uncompressedSize) ? wad : juce::File();
    }
}

//==============================================================================
// doomgeneric platform hooks (all called on the doom thread)
extern "C"
{
    void DG_Init() {}

    void DG_DrawFrame()
    {
        auto& s = shared();
        bool changed = false;
        {
            std::lock_guard<std::mutex> sl (s.frameLock);
            const auto* src = reinterpret_cast<const uint32_t*> (DG_ScreenBuffer);
            for (size_t i = 0; i < s.frame.size(); ++i)
            {
                const uint32_t px = src[i] | 0xff000000u;
                changed |= (px != s.frame[i]);
                s.frame[i] = px;
            }
        }
        // A paused game or a still menu redraws the same picture: don't wake the UI for it.
        if (changed)
            s.serial.fetch_add (1);
    }

    void DG_SleepMs (uint32_t ms)
    {
        juce::Thread::sleep ((int) ms);
    }

    uint32_t DG_GetTicksMs()
    {
        return (uint32_t) shared().nowMs();
    }

    int DG_GetKeyEx (int* pressed, unsigned char* key, unsigned char* typed)
    {
        auto& s = shared();
        std::lock_guard<std::mutex> lock (s.keyLock);
        if (s.keys.empty())
            return 0;
        *pressed = s.keys.front().pressed;
        *key = s.keys.front().key;
        *typed = s.keys.front().typed;
        s.keys.pop_front();
        return 1;
    }

    int DG_GetKey (int* pressed, unsigned char* key)
    {
        unsigned char typed;
        return DG_GetKeyEx (pressed, key, &typed);
    }

    void DG_SetWindowTitle (const char*) {}
}

//==============================================================================
DoomEngine::DoomEngine() : juce::Thread ("DOOM")
{
    startThread();
}

DoomEngine::~DoomEngine()
{
    stopThread (4000);
}

void DoomEngine::pushKey (bool pressed, int doomKey, int typedChar)
{
    if (doomKey <= 0 || doomKey > 255)
        return;
    if (typedChar <= 0 || typedChar > 255)
        typedChar = doomKey;
    auto& s = shared();
    std::lock_guard<std::mutex> lock (s.keyLock);
    if (s.keys.size() < 256)
        s.keys.push_back ({ pressed ? 1 : 0, (unsigned char) doomKey, (unsigned char) typedChar });
}

void DoomEngine::requestPause (bool shouldPause)
{
    dg_request_pause (shouldPause ? 1 : 0);
}

bool DoomEngine::fetchFrame (juce::Image& image, uint32_t& lastSerial)
{
    auto& s = shared();
    const auto serial = s.serial.load();
    if (serial == lastSerial || ! image.isValid())
        return false;

    juce::Image::BitmapData bd (image, juce::Image::BitmapData::writeOnly);
    {
        std::lock_guard<std::mutex> sl (s.frameLock);
        for (int y = 0; y < height; ++y)
            std::memcpy (bd.getLinePointer (y), s.frame.data() + (size_t) (y * width), (size_t) width * sizeof (uint32_t));
    }
    lastSerial = serial;
    return true;
}

DoomEngine::State DoomEngine::getState()
{
    return (State) shared().state.load();
}

int DoomEngine::getGameFlags()
{
    return getState() == State::Running ? dg_game_flags() : 0;
}

juce::File DoomEngine::getWadFile()
{
    return ensureWad();
}

juce::String DoomEngine::getStatusText()
{
    auto& s = shared();
    const juce::ScopedLock sl (s.statusLock);
    return s.status;
}

void DoomEngine::run()
{
    auto& s = shared();

    if (getState() == State::Idle)
    {
        s.state = (int) State::Booting;
        s.setStatus ("Loading DOOM...");

        const auto wad = ensureWad();
        if (! wad.existsAsFile())
        {
            s.setStatus ("Couldn't unpack the Freedoom WAD.");
            s.state = (int) State::Dead;
            return;
        }

        // Both must outlive the game: doom keeps pointers to argv and the config dir.
        static std::string configDir = dataDirectory().getFullPathName().toStdString() + std::string (1, (char) juce::File::getSeparatorChar());
        // -nogui: a fatal error must not try to pop up a dialog by running a shell command.
        static std::string arg0 = "doomeq", arg1 = "-iwad", arg2 = wad.getFullPathName().toStdString(), arg3 = "-nogui";
        static char* argv[] = { arg0.data(), arg1.data(), arg2.data(), arg3.data(), nullptr };
        dg_config_dir = configDir.c_str();

        s.startClock();
        const bool failed = dg_safe_create (4, argv) != 0;
        s.stopClock();

        if (failed)
        {
            s.setStatus ("DOOM failed to start.");
            s.state = (int) State::Dead;
            return;
        }

        s.setStatus ({});
        s.state = (int) State::Running;
    }

    s.startClock();
    DoomAudio::setGameRunning (true);

    while (! threadShouldExit() && getState() == State::Running)
    {
        if (dg_safe_tick() != 0)
        {
            s.setStatus ("DOOM has left the building. The EQ still works.");
            s.state = (int) State::Dead;
        }
    }

    DoomAudio::setGameRunning (false);
    s.stopClock();
}
