#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

// Drives the Pro Tools window from outside with the macOS Accessibility API and keyboard events:
// finds the Event > Identify Beat menu item, opens its dialog, fills in a bar|beat and confirms it.
// Plain C++ here; the macOS code is in ProToolsDriver.mm. Nothing in it is specific to this app's
// window, and it is never used on the audio thread.
//
// Every function writes what it does to the log, and stops at the first thing that is not as
// expected. Before every key press and every button press it checks that Pro Tools is the front
// application, so nothing is typed into another application. When Pro Tools gives no answer to a
// question, that is never read as "no windows" or "closed": the step fails.
namespace tt::pt
{

struct Result
{
    bool ok = false;
    bool confirmed = true;      // false when the step was made but its effect could not be read back
    std::string message;        // one sentence, written to follow "Stopped: " or "Worked: "

    static Result good (std::string text = {})             { return { true, true, std::move (text) }; }
    static Result unconfirmed (std::string text)           { return { true, false, std::move (text) }; }
    static Result bad (std::string text)                   { return { false, false, std::move (text) }; }
};

// A user interface element of another application. Empty when nothing was found.
struct Element
{
    std::shared_ptr<const void> handle;

    explicit operator bool() const                         { return handle != nullptr; }
};

struct AppInfo
{
    bool running = false;
    bool frontmost = false;
    int pid = 0;
    std::string name;
    std::string version;
};

class Driver
{
public:
    using Log = std::function<void (const std::string&)>;
    using Cancelled = std::function<bool()>;

    // `log` receives one message per step (a message may hold several lines). `cancelled` is asked
    // between steps and during every wait; once it returns true the current function fails.
    Driver (Log log, Cancelled cancelled);
    ~Driver();

    //==============================================================================
    // Whether macOS lets this application read and control other applications. Never shows a prompt.
    static bool accessibilityAllowed();

    // Shows the system's prompt that leads to System Settings. Only for a button the user presses.
    static void requestAccessibility();

    // Whether this application may post keyboard events. Never shows a prompt.
    static bool keyEventsAllowed();

    static std::string systemVersion();

    // Whether an application with this bundle identifier is running.
    static bool isRunning (const std::string& bundleIdentifier = defaultBundleIdentifier);

    static constexpr const char* defaultBundleIdentifier = "com.avid.ProTools";

    //==============================================================================
    // The application to drive, for trying the read-only functions on another application.
    void setBundleIdentifier (std::string bundleIdentifier);

    AppInfo findProTools();

    // Fails if Pro Tools is not the front application three seconds later.
    Result bringToFront();

    // Fails unless Pro Tools is the front application right now.
    Result requireFront();

    // The element and everything below it down to `depth` levels, one line per element: role,
    // subrole, title, description, value, enabled, focused, position, size and actions.
    std::string describe (const Element& element, int depth);

    // Writes the title, role and subrole of every window of the application to the log.
    void logWindows();

    // Walks the menu bar by title, for example { "Event", "Identify Beat..." }. Three dots match the
    // ellipsis character, and spaces at the ends are ignored. Presses nothing. When an item is not
    // found, the titles of the top-level menus and of every item in the menu searched are logged.
    Result findMenuItem (const std::vector<std::string>& path, Element& item, bool& enabled);

    // Looks in the application's windows for a text element whose title, description, help or
    // identifier contains "Counter" (or that sits inside such an element), logs every candidate,
    // and gives the value of the best one. A search that finds nothing is not made again.
    Result readMainCounter (std::string& value);

    // Notes which windows are open, at the start of a run. After that goToSample sends no key
    // while a window is open that was not among them.
    Result rememberWindows();

    // Keypad *, the digits, keypad Enter. Then the Main Counter must show that sample; if the
    // counter cannot be read the result is unconfirmed. A window that opens instead is a failure.
    Result goToSample (long long sample);

    // Presses the menu item and waits up to three seconds for a window whose title contains
    // "Bar" and "Beat". The first dialog of a run is written to the log in full.
    Result openIdentifyBeat (const Element& menuItem, Element& dialog);

    // Puts "46|1|000" in the dialog's location. Sets the field's value when it can, otherwise
    // types it (always types when `typeIt` is set), and types only when the keyboard is in a text
    // field of the dialog. Reads the result back: when that differs from what was asked, or could
    // be read before and not after, the dialog is cancelled and the result is a failure. The
    // result is unconfirmed only when no location could be read before or after.
    Result setIdentifyBeatLocation (const Element& dialog, const std::string& barBeat, bool typeIt);

    // Presses OK, waits for the dialog to close and watches half a second more for an alert. If
    // another window appears, it is written to the log, nothing is pressed, and its text is the
    // failure. `okPressed` says whether OK was pressed, whatever happened after.
    Result confirmIdentifyBeat (const Element& dialog, bool& okPressed);

    // Presses Cancel (or Escape when there is no Cancel button) and waits for the dialog to close.
    Result cancelDialog (const Element& dialog);

private:
    struct State;
    std::unique_ptr<State> state;
};

} // namespace tt::pt
