#include "ProToolsDriver.h"

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <optional>
#include <unistd.h>

namespace tt::pt
{

namespace
{

// How long one Accessibility call may wait for the other application to answer.
constexpr float messagingTimeoutSeconds = 2.0f;

// Virtual key codes. The keypad keys are the same on every keyboard layout.
constexpr int keypadStarKey = 67;
constexpr int keypadEnterKey = 76;
constexpr int escapeKey = 53;
constexpr int periodKey = 47;
constexpr int zKey = 6;
constexpr int topRowDigitKeys[10] = { 29, 18, 19, 20, 21, 23, 22, 26, 28, 25 };

using Clock = std::chrono::steady_clock;

int millisecondsSince (Clock::time_point start)
{
    return (int) std::chrono::duration_cast<std::chrono::milliseconds> (Clock::now() - start).count();
}

//==============================================================================
// Text
std::string toStd (CFStringRef text)
{
    if (text == nullptr)
        return {};

    const CFIndex size = CFStringGetMaximumSizeForEncoding (CFStringGetLength (text), kCFStringEncodingUTF8) + 1;
    std::string out ((size_t) size, '\0');

    if (! CFStringGetCString (text, out.data(), size, kCFStringEncodingUTF8))
        return {};

    out.resize (std::strlen (out.c_str()));
    return out;
}

std::string toStd (NSString* text)
{
    const char* utf8 = text != nil ? [text UTF8String] : nullptr;
    return utf8 != nullptr ? std::string (utf8) : std::string();
}

std::string lower (std::string text)
{
    for (auto& c : text)
        if (c >= 'A' && c <= 'Z')
            c = (char) (c - 'A' + 'a');

    return text;
}

void replaceAll (std::string& text, const std::string& from, const std::string& to)
{
    for (size_t at = 0; (at = text.find (from, at)) != std::string::npos; at += to.size())
        text.replace (at, from.size(), to);
}

std::string trim (const std::string& text)
{
    const auto first = text.find_first_not_of (" \t\r\n");

    if (first == std::string::npos)
        return {};

    return text.substr (first, text.find_last_not_of (" \t\r\n") - first + 1);
}

// A title as it is compared: the ellipsis character as three dots, no-break spaces as spaces,
// nothing at the ends, lower case.
std::string normalise (std::string title)
{
    replaceAll (title, "\xe2\x80\xa6", "...");
    replaceAll (title, "\xc2\xa0", " ");
    return lower (trim (title));
}

std::string withoutTrailingDots (std::string title)
{
    while (! title.empty() && (title.back() == '.' || title.back() == ' '))
        title.pop_back();

    return title;
}

bool contains (const std::string& text, const char* part)
{
    return text.find (part) != std::string::npos;
}

// For the log: in quotes, on one line, cut to `limit` characters.
std::string quoted (std::string text, size_t limit = 60)
{
    replaceAll (text, "\n", "\\n");
    replaceAll (text, "\r", "\\r");

    if (text.size() > limit)
    {
        // Not in the middle of a UTF-8 character.
        while (limit > 0 && (((unsigned char) text[limit]) & 0xc0) == 0x80)
            --limit;

        text = text.substr (0, limit) + "...";
    }

    return "\"" + text + "\"";
}

bool allDigits (const std::string& text)
{
    return ! text.empty() && std::all_of (text.begin(), text.end(), [] (char c) { return c >= '0' && c <= '9'; });
}

// The digits of a sample count as a counter shows it: spaces and thousands separators removed.
std::string counterDigits (std::string text)
{
    replaceAll (text, "\xc2\xa0", "");
    text.erase (std::remove_if (text.begin(), text.end(), [] (char c) { return c == ' ' || c == ',' || c == '\t'; }), text.end());
    return text;
}

// "46|1|000" or "46| 1| 000" into its three numbers.
bool parseBarBeat (const std::string& text, long long out[3])
{
    size_t start = 0;

    for (int part = 0; part < 3; ++part)
    {
        const size_t bar = text.find ('|', start);

        if ((part < 2) == (bar == std::string::npos))
            return false;

        const auto piece = trim (text.substr (start, bar == std::string::npos ? std::string::npos : bar - start));

        if (! allDigits (piece) || piece.size() > 9)
            return false;

        out[part] = std::stoll (piece);
        start = bar + 1;
    }

    return true;
}

std::string errorName (AXError error)
{
    switch (error)
    {
        case kAXErrorSuccess:                           return "ok";
        case kAXErrorFailure:                           return "failure";
        case kAXErrorIllegalArgument:                   return "illegal argument";
        case kAXErrorInvalidUIElement:                  return "the element no longer exists";
        case kAXErrorInvalidUIElementObserver:          return "invalid observer";
        case kAXErrorCannotComplete:                    return "no answer in time";
        case kAXErrorAttributeUnsupported:              return "attribute not supported";
        case kAXErrorActionUnsupported:                 return "action not supported";
        case kAXErrorNotificationUnsupported:           return "notification not supported";
        case kAXErrorNotImplemented:                    return "not implemented";
        case kAXErrorNotificationAlreadyRegistered:     return "notification already registered";
        case kAXErrorNotificationNotRegistered:         return "notification not registered";
        case kAXErrorAPIDisabled:                       return "Accessibility is not allowed";
        case kAXErrorNoValue:                           return "no value";
        case kAXErrorParameterizedAttributeUnsupported: return "parameterised attribute not supported";
        case kAXErrorNotEnoughPrecision:                return "not enough precision";
    }

    return "error " + std::to_string ((int) error);
}

//==============================================================================
// Elements
AXUIElementRef ax (const Element& element)
{
    return (AXUIElementRef) element.handle.get();
}

// Takes over one reference.
Element adopt (CFTypeRef owned)
{
    Element e;

    if (owned != nullptr)
        e.handle = std::shared_ptr<const void> (owned, [] (const void* p) { CFRelease (p); });

    return e;
}

Element retained (CFTypeRef borrowed)
{
    return adopt (borrowed != nullptr ? CFRetain (borrowed) : nullptr);
}

bool same (const Element& a, const Element& b)
{
    return a && b && CFEqual (ax (a), ax (b));
}

bool isIn (const Element& element, const std::vector<Element>& list)
{
    return std::any_of (list.begin(), list.end(), [&] (const Element& other) { return same (element, other); });
}

std::string valueText (CFTypeRef value)
{
    if (value == nullptr || value == kCFNull)
        return {};

    const auto type = CFGetTypeID (value);

    if (type == CFStringGetTypeID())
        return toStd ((CFStringRef) value);

    if (type == CFBooleanGetTypeID())
        return CFBooleanGetValue ((CFBooleanRef) value) ? "true" : "false";

    if (type == CFNumberGetTypeID())
    {
        double number = 0.0;
        CFNumberGetValue ((CFNumberRef) value, kCFNumberDoubleType, &number);
        char text[48];

        if (std::abs (number - std::round (number)) < 1.0e-9 && std::abs (number) < 1.0e15)
            std::snprintf (text, sizeof (text), "%lld", (long long) number);
        else
            std::snprintf (text, sizeof (text), "%g", number);

        return text;
    }

    if (type == CFArrayGetTypeID())
        return "<" + std::to_string ((long) CFArrayGetCount ((CFArrayRef) value)) + " items>";

    if (type == AXUIElementGetTypeID())
        return "<element>";

    return "<other>";
}

bool isErrorValue (CFTypeRef value)
{
    return value == nullptr || value == kCFNull
           || (CFGetTypeID (value) == AXValueGetTypeID() && AXValueGetType ((AXValueRef) value) == kAXValueTypeAXError);
}

struct Info
{
    std::string role, subrole, title, description, value, help, identifier;
    bool answered = true;       // false when the other application gave no answer in time
    bool hasValue = false, hasEnabled = false, enabled = true, focused = false, hasFrame = false;
    CGPoint position {};
    CGSize size {};

    // Everything an element may be named by, lower case.
    std::string names() const              { return lower (title + " | " + description + " | " + help + " | " + identifier); }
    bool isText() const                    { return role == "AXTextField" || role == "AXTextArea" || role == "AXComboBox"; }
};

// One round trip to the other application for everything the log shows about an element.
Info readInfo (AXUIElementRef element)
{
    Info info;

    if (element == nullptr)
        return info;

    const void* names[] = { kAXRoleAttribute, kAXSubroleAttribute, kAXTitleAttribute, kAXDescriptionAttribute,
                            kAXValueAttribute, kAXHelpAttribute, kAXIdentifierAttribute, kAXEnabledAttribute,
                            kAXFocusedAttribute, kAXPositionAttribute, kAXSizeAttribute };
    const CFIndex count = (CFIndex) (sizeof (names) / sizeof (names[0]));
    CFArrayRef attributes = CFArrayCreate (kCFAllocatorDefault, names, count, &kCFTypeArrayCallBacks);
    CFArrayRef values = nullptr;

    const AXError error = AXUIElementCopyMultipleAttributeValues (element, attributes, 0, &values);
    info.answered = error != kAXErrorCannotComplete;

    if (error == kAXErrorSuccess && values != nullptr)
    {
        const auto at = [values] (CFIndex index) -> CFTypeRef
        {
            if (index >= count || index >= CFArrayGetCount (values))
                return nullptr;

            CFTypeRef v = CFArrayGetValueAtIndex (values, index);
            return isErrorValue (v) ? nullptr : v;
        };

        info.role = valueText (at (0));
        info.subrole = valueText (at (1));
        info.title = valueText (at (2));
        info.description = valueText (at (3));
        info.hasValue = at (4) != nullptr;
        info.value = valueText (at (4));
        info.help = valueText (at (5));
        info.identifier = valueText (at (6));

        if (CFTypeRef v = at (7); v != nullptr && CFGetTypeID (v) == CFBooleanGetTypeID())
        {
            info.hasEnabled = true;
            info.enabled = CFBooleanGetValue ((CFBooleanRef) v);
        }

        if (CFTypeRef v = at (8); v != nullptr && CFGetTypeID (v) == CFBooleanGetTypeID())
            info.focused = CFBooleanGetValue ((CFBooleanRef) v);

        CFTypeRef p = at (9), z = at (10);

        if (p != nullptr && z != nullptr && CFGetTypeID (p) == AXValueGetTypeID() && CFGetTypeID (z) == AXValueGetTypeID())
            info.hasFrame = AXValueGetValue ((AXValueRef) p, kAXValueTypeCGPoint, &info.position)
                            && AXValueGetValue ((AXValueRef) z, kAXValueTypeCGSize, &info.size);
    }

    if (values != nullptr)
        CFRelease (values);

    CFRelease (attributes);
    return info;
}

// An attribute that holds one element. `out` stays empty unless the answer is an element.
AXError copyElementChecked (AXUIElementRef element, CFStringRef attribute, Element& out)
{
    out = {};
    CFTypeRef value = nullptr;

    if (element == nullptr)
        return kAXErrorIllegalArgument;

    const AXError error = AXUIElementCopyAttributeValue (element, attribute, &value);

    if (error == kAXErrorSuccess && value != nullptr && CFGetTypeID (value) == AXUIElementGetTypeID())
        out = adopt (value);
    else if (value != nullptr)
        CFRelease (value);

    return error;
}

Element copyElement (AXUIElementRef element, CFStringRef attribute)
{
    Element out;
    copyElementChecked (element, attribute, out);
    return out;
}

// An attribute that holds a list of elements.
AXError copyElementsChecked (AXUIElementRef element, CFStringRef attribute, std::vector<Element>& out)
{
    out.clear();
    CFTypeRef value = nullptr;

    if (element == nullptr)
        return kAXErrorIllegalArgument;

    const AXError error = AXUIElementCopyAttributeValue (element, attribute, &value);

    if (error != kAXErrorSuccess || value == nullptr)
        return error;

    if (CFGetTypeID (value) == CFArrayGetTypeID())
    {
        const CFIndex n = CFArrayGetCount ((CFArrayRef) value);
        out.reserve ((size_t) n);

        for (CFIndex i = 0; i < n; ++i)
        {
            CFTypeRef item = CFArrayGetValueAtIndex ((CFArrayRef) value, i);

            if (item != nullptr && CFGetTypeID (item) == AXUIElementGetTypeID())
                out.push_back (retained (item));
        }
    }

    CFRelease (value);
    return error;
}

std::vector<Element> copyElements (AXUIElementRef element, CFStringRef attribute)
{
    std::vector<Element> out;
    copyElementsChecked (element, attribute, out);
    return out;
}

std::vector<Element> childrenOf (const Element& element)
{
    return copyElements (ax (element), kAXChildrenAttribute);
}

std::vector<std::string> actionsOf (AXUIElementRef element)
{
    std::vector<std::string> out;
    CFArrayRef names = nullptr;

    if (AXUIElementCopyActionNames (element, &names) == kAXErrorSuccess && names != nullptr)
    {
        for (CFIndex i = 0; i < CFArrayGetCount (names); ++i)
            out.push_back (toStd ((CFStringRef) CFArrayGetValueAtIndex (names, i)));
    }

    if (names != nullptr)
        CFRelease (names);

    return out;
}

bool isSettable (AXUIElementRef element, CFStringRef attribute)
{
    Boolean settable = false;
    return AXUIElementIsAttributeSettable (element, attribute, &settable) == kAXErrorSuccess && settable;
}

// One line for the log.
std::string oneLine (AXUIElementRef element, const Info& info, bool withActions)
{
    std::string line = info.role.empty() ? std::string ("(no role)") : info.role;

    if (! info.subrole.empty())       line += " (" + info.subrole + ")";
    if (! info.title.empty())         line += " title=" + quoted (info.title);
    if (! info.description.empty())   line += " desc=" + quoted (info.description);
    if (info.hasValue)                line += " value=" + quoted (info.value);
    if (! info.help.empty())          line += " help=" + quoted (info.help);
    if (! info.identifier.empty())    line += " id=" + quoted (info.identifier);
    if (info.hasEnabled && ! info.enabled) line += " disabled";
    if (info.focused)                 line += " focused";

    if (info.hasFrame)
    {
        char frame[96];
        std::snprintf (frame, sizeof (frame), " at %d,%d size %dx%d", (int) info.position.x, (int) info.position.y,
                       (int) info.size.width, (int) info.size.height);
        line += frame;
    }

    if (withActions)
    {
        if (info.isText())
            line += isSettable (element, kAXValueAttribute) ? " value-settable" : " value-not-settable";

        const auto actions = actionsOf (element);

        if (! actions.empty())
        {
            line += " actions=";

            for (size_t i = 0; i < actions.size(); ++i)
                line += (i > 0 ? "," : "") + actions[i];
        }
    }

    return line;
}

std::string oneLine (const Element& element, bool withActions = true)
{
    return element ? oneLine (ax (element), readInfo (ax (element)), withActions) : std::string ("(nothing)");
}

// Ends a walk over many elements: when the run is stopped, when its time is up, or once the
// other application has given no answer in time (every further read would wait as long again).
struct Walk
{
    std::function<bool()> stopped;
    Clock::time_point deadline;
    bool noAnswer = false;

    bool over() const
    {
        return noAnswer || Clock::now() >= deadline || (stopped && stopped());
    }
};

void describeInto (const Element& element, int depth, int indent, int& budget, Walk& walk, std::string& out)
{
    const std::string pad ((size_t) indent * 2, ' ');

    if (budget <= 0 || walk.over())
    {
        if (budget >= 0)
            out += pad + (budget == 0 ? "(more elements not shown)\n" : "(the rest was not read)\n");

        budget = -1;
        return;
    }

    --budget;
    const auto info = readInfo (ax (element));

    if (! info.answered)
    {
        walk.noAnswer = true;
        out += pad + "(no answer in time)\n";
        return;
    }

    out += pad + oneLine (ax (element), info, true) + "\n";
    const auto children = childrenOf (element);

    if (depth <= 0)
    {
        if (! children.empty())
            out += pad + "  (" + std::to_string (children.size()) + " elements inside not shown)\n";

        return;
    }

    for (const auto& child : children)
        describeInto (child, depth - 1, indent + 1, budget, walk, out);
}

// Every element below `root` (and root itself), parents before children, at most `limit`.
void collect (const Element& root, std::vector<Element>& out, size_t limit, const Walk& walk)
{
    if (! root || out.size() >= limit || walk.over())
        return;

    out.push_back (root);

    for (const auto& child : childrenOf (root))
        collect (child, out, limit, walk);
}

struct Read
{
    Element element;
    Info info;
};

// The elements below `root` and what each one says about itself. Shorter than the whole when
// the walk ends early.
std::vector<Read> readAll (const Element& root, size_t limit, Walk& walk)
{
    std::vector<Element> all;
    collect (root, all, limit, walk);
    std::vector<Read> out;
    out.reserve (all.size());

    for (const auto& e : all)
    {
        if (walk.over())
            break;

        auto info = readInfo (ax (e));

        if (! info.answered)
        {
            walk.noAnswer = true;
            break;
        }

        out.push_back ({ e, std::move (info) });
    }

    return out;
}

std::vector<Element> notIn (const std::vector<Element>& now, const std::vector<Element>& earlier)
{
    std::vector<Element> out;

    for (const auto& w : now)
        if (! isIn (w, earlier))
            out.push_back (w);

    return out;
}

// Whether `element` is in `window`: by its AXWindow, or by going up through its parents.
bool isInside (const Element& element, const Element& window)
{
    if (! element || ! window)
        return false;

    if (same (copyElement (ax (element), kAXWindowAttribute), window))
        return true;

    auto at = element;

    for (int level = 0; level < 40 && at; ++level)
    {
        if (same (at, window))
            return true;

        at = copyElement (ax (at), kAXParentAttribute);
    }

    return false;
}

} // namespace

//==============================================================================
struct Driver::State
{
    using Windows = std::optional<std::vector<Element>>;

    enum class Front { yes, no, unknown };
    enum class Open { yes, no, unknown };
    enum class Closed { closed, stillOpen, noAnswer, stopped };

    Log log;
    Cancelled cancelled;
    std::string bundleIdentifier = Driver::defaultBundleIdentifier;
    std::string appName = "Pro Tools";
    pid_t pid = 0;
    Element app;
    Element counter;            // the Main Counter element once found, to read it again quickly
    bool counterNotFound = false;   // the search was made and found nothing: it is not made again
    Windows baseline;           // the windows open when rememberWindows was called
    bool dialogDescribed = false;

    // The quick pace (see Driver::setQuick).
    bool quick = false;
    int keyHoldMicroseconds = 30000;    // between a key going down and coming up, and after it
    int keyGapMilliseconds = 50;        // after each key
    bool counterFocusLogged = false;

    // The parts of the Identify Beat dialog that is open, read once each time it opens.
    struct Parts
    {
        Element dialog, field, ok, cancel;
    };

    Parts parts;

    // How long a wait for something to change sleeps between two looks.
    int poll() const
    {
        return quick ? 10 : 50;
    }

    void say (const std::string& text) const
    {
        if (log)
            log (text);
    }

    bool stopped() const
    {
        return cancelled && cancelled();
    }

    // False when the run was stopped during the wait.
    bool wait (int milliseconds) const
    {
        const auto start = Clock::now();

        while (millisecondsSince (start) < milliseconds)
        {
            if (stopped())
                return false;

            usleep (quick ? 2000 : 10000);
        }

        return ! stopped();
    }

    static Result stoppedResult()
    {
        return Result::bad ("the run was stopped");
    }

    Result noAnswer (const std::string& when) const
    {
        return Result::bad (appName + " did not answer " + when);
    }

    // A walk over elements that ends when the run is stopped or after this long.
    Walk walk (int milliseconds) const
    {
        return { [this] { return stopped(); }, Clock::now() + std::chrono::milliseconds (milliseconds) };
    }

    std::string describe (const Element& element, int depth) const
    {
        if (! element)
            return "(nothing)";

        std::string out;
        int budget = 600;
        auto limits = walk (8000);
        describeInto (element, depth, 0, budget, limits, out);

        while (! out.empty() && out.back() == '\n')
            out.pop_back();

        return out;
    }

    // The application that is in front, from the window server and from Accessibility. Both must
    // say it is Pro Tools. When Accessibility gives no answer the result is unknown, which is
    // never taken as yes. `why` gets a sentence for the log when the result is not yes.
    Front front (std::string* why = nullptr) const
    {
        @autoreleasepool
        {
            NSRunningApplication* frontApp = [[NSWorkspace sharedWorkspace] frontmostApplication];

            if (frontApp == nil || pid == 0 || frontApp.processIdentifier != pid)
            {
                if (why != nullptr)
                    *why = "The front application is " + (frontApp != nil ? toStd (frontApp.localizedName) : std::string ("nothing"))
                           + ", not " + appName + ".";

                return Front::no;
            }

            AXUIElementRef systemWide = AXUIElementCreateSystemWide();
            Element focusedApp;
            const AXError error = copyElementChecked (systemWide, kAXFocusedApplicationAttribute, focusedApp);
            CFRelease (systemWide);
            pid_t focusedPid = 0;

            if (! focusedApp || AXUIElementGetPid (ax (focusedApp), &focusedPid) != kAXErrorSuccess)
            {
                if (why != nullptr)
                    *why = "macOS did not say which application has the keyboard (" + errorName (error) + ").";

                return Front::unknown;
            }

            if (focusedPid != pid)
            {
                if (why != nullptr)
                    *why = "The keyboard is with another application (process " + std::to_string ((int) focusedPid)
                           + "), not " + appName + ".";

                return Front::no;
            }

            return Front::yes;
        }
    }

    bool isFront() const
    {
        return front() == Front::yes;
    }

    // Before every key and every press. A missing answer is asked for again for a quarter of a
    // second, then counts as not in front.
    Result frontGuard() const
    {
        if (stopped())
            return stoppedResult();

        if (pid == 0)
            return Result::bad (appName + " has not been found");

        std::string why;
        auto now = front (&why);

        for (int again = 0; again < 5 && now == Front::unknown; ++again)
        {
            if (! wait (50))
                return stoppedResult();

            now = front (&why);
        }

        if (now == Front::yes)
            return Result::good();

        say (why + " Nothing more is sent.");
        return Result::bad (now == Front::unknown ? "macOS did not confirm that " + appName + " has the keyboard"
                                                  : appName + " stopped being the front application");
    }

    // One key down and up, sent only while Pro Tools is in front. `character` is the text the key
    // types, so a digit is a digit on every keyboard layout (zero: leave it to the layout).
    Result pressKey (int keyCode, CGEventFlags flags, UniChar character, const std::string& name) const
    {
        if (auto guard = frontGuard(); ! guard.ok)
            return guard;

        CGEventSourceRef source = CGEventSourceCreate (kCGEventSourceStatePrivate);
        bool posted = true;

        for (const bool down : { true, false })
        {
            CGEventRef event = CGEventCreateKeyboardEvent (source, (CGKeyCode) keyCode, down);

            if (event == nullptr)
            {
                posted = false;
                break;
            }

            CGEventSetFlags (event, flags);

            if (character != 0)
                CGEventKeyboardSetUnicodeString (event, 1, &character);

            CGEventPost (kCGHIDEventTap, event);
            CFRelease (event);
            usleep ((useconds_t) keyHoldMicroseconds);
        }

        if (source != nullptr)
            CFRelease (source);

        if (! posted)
            return Result::bad ("macOS would not make the key event for " + name);

        if (! name.empty())
            say ("Key: " + name + " (key code " + std::to_string (keyCode) + ")");

        return wait (keyGapMilliseconds) ? Result::good() : stoppedResult();
    }

    // The digits on the top row of the keyboard, one key each. (The keypad's digits are transport
    // keys in Pro Tools whenever no number field is being typed in, so they are not used.)
    Result typeDigits (const std::string& digits) const
    {
        for (const char c : digits)
        {
            if (c < '0' || c > '9')
                return Result::bad ("there was something other than a digit to type");

            if (auto r = pressKey (topRowDigitKeys[c - '0'], 0, (UniChar) c, {}); ! r.ok)
                return r;
        }

        say ("Typed " + digits + " with the top-row digit keys");
        return Result::good();
    }

    Element focusedElement() const
    {
        return copyElement (ax (app), kAXFocusedUIElementAttribute);
    }

    // The application's windows, the focused one first (a dialog is not always in the window
    // list). Nothing when Pro Tools did not answer, which is not the same as having no windows.
    Windows windows() const
    {
        Element focused;
        const AXError focusError = copyElementChecked (ax (app), kAXFocusedWindowAttribute, focused);

        if (focusError != kAXErrorSuccess && focusError != kAXErrorNoValue && focusError != kAXErrorAttributeUnsupported)
            return std::nullopt;

        std::vector<Element> listed;

        if (copyElementsChecked (ax (app), kAXWindowsAttribute, listed) != kAXErrorSuccess)
            return std::nullopt;

        std::vector<Element> out;

        if (focused)
            out.push_back (focused);

        for (auto& w : listed)
            if (! isIn (w, out))
                out.push_back (w);

        return out;
    }

    // Asks again for up to this long while Pro Tools does not answer.
    Windows windowsPatiently (int milliseconds) const
    {
        const auto start = Clock::now();

        do
        {
            if (auto now = windows())
                return now;
        }
        while (wait (50) && millisecondsSince (start) < milliseconds);

        return std::nullopt;
    }

    Open windowIsOpen (const Element& window) const
    {
        const auto now = windows();
        return ! now ? Open::unknown : (isIn (window, *now) ? Open::yes : Open::no);
    }

    // For after "Pro Tools has".
    std::string windowList() const
    {
        const auto all = windows();

        if (! all)
            return "windows that could not be read: it did not answer";

        std::string out = std::to_string (all->size()) + (all->size() == 1 ? " window" : " windows");

        for (const auto& w : *all)
            out += "\n  " + oneLine (w, false);

        return out;
    }

    // The words in a window, for reporting an alert.
    std::string textIn (const Element& window) const
    {
        auto limits = walk (4000);
        std::string out;

        for (const auto& [element, info] : readAll (window, 200, limits))
        {
            if (info.role != "AXStaticText")
                continue;

            const auto words = trim (info.value.empty() ? info.title : info.value);

            if (! words.empty())
                out += (out.empty() ? "" : " ") + words;
        }

        return out;
    }

    // Closed only when Pro Tools answered and the window was not among its windows. A read that
    // got no answer says nothing either way, so the wait goes on.
    Closed waitUntilClosed (const Element& window, int milliseconds) const
    {
        const auto start = Clock::now();
        auto last = Open::unknown;

        do
        {
            last = windowIsOpen (window);

            if (last == Open::no)
                return Closed::closed;

            if (! wait (poll()))
                return Closed::stopped;
        }
        while (millisecondsSince (start) < milliseconds);

        return last == Open::unknown ? Closed::noAnswer : Closed::stillOpen;
    }

    // Reads the dialog once and keeps its location field and its OK and Cancel buttons. The
    // location field is the text field whose value reads as bar|beat|ticks, or the one with
    // the identifier LOCA.
    Result readParts (const Element& dialog)
    {
        if (same (parts.dialog, dialog))
            return Result::good();

        parts = {};
        auto limits = walk (4000);

        for (const auto& [element, info] : readAll (dialog, 400, limits))
        {
            if (info.isText())
            {
                long long got[3] = {};

                if (! parts.field && (parseBarBeat (info.value, got) || lower (trim (info.identifier)) == "loca"))
                    parts.field = element;
            }
            else if (info.role == "AXButton")
            {
                const auto title = normalise (info.title);

                if (title == "ok" && ! parts.ok)
                    parts.ok = element;
                else if (title == "cancel" && ! parts.cancel)
                    parts.cancel = element;
            }
        }

        if (stopped())
            return stoppedResult();

        if (limits.noAnswer)
            return noAnswer ("while the Identify Beat dialog was read");

        if (! parts.ok)
            parts.ok = copyElement (ax (dialog), kAXDefaultButtonAttribute);

        if (! parts.cancel)
            parts.cancel = copyElement (ax (dialog), kAXCancelButtonAttribute);

        parts.dialog = dialog;
        return Result::good();
    }

    // What the location field shows, asked again for up to `milliseconds` until it reads as
    // bar|beat|ticks. False when it never does.
    bool readLocation (std::string& shown, long long got[3], int milliseconds) const
    {
        const auto start = Clock::now();

        do
        {
            const auto info = readInfo (ax (parts.field));
            shown = info.value;

            if (parseBarBeat (shown, got))
                return true;
        }
        while (wait (poll()) && millisecondsSince (start) < milliseconds);

        return false;
    }
};

//==============================================================================
Driver::Driver (Log log, Cancelled cancelled)
    : state (std::make_unique<State>())
{
    state->log = std::move (log);
    state->cancelled = std::move (cancelled);
}

Driver::~Driver() = default;

bool Driver::accessibilityAllowed()
{
    return AXIsProcessTrusted();
}

void Driver::requestAccessibility()
{
    const void* keys[] = { kAXTrustedCheckOptionPrompt };
    const void* values[] = { kCFBooleanTrue };
    CFDictionaryRef options = CFDictionaryCreate (kCFAllocatorDefault, keys, values, 1,
                                                  &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    AXIsProcessTrustedWithOptions (options);
    CFRelease (options);
}

bool Driver::keyEventsAllowed()
{
    return CGPreflightPostEventAccess();
}

std::string Driver::systemVersion()
{
    @autoreleasepool
    {
        const NSOperatingSystemVersion v = [[NSProcessInfo processInfo] operatingSystemVersion];
        return std::to_string ((long) v.majorVersion) + "." + std::to_string ((long) v.minorVersion)
               + "." + std::to_string ((long) v.patchVersion);
    }
}

bool Driver::isRunning (const std::string& bundleIdentifier)
{
    @autoreleasepool
    {
        NSString* identifier = [NSString stringWithUTF8String: bundleIdentifier.c_str()];
        return identifier != nil && [[NSRunningApplication runningApplicationsWithBundleIdentifier: identifier] count] > 0;
    }
}

void Driver::setBundleIdentifier (std::string bundleIdentifier)
{
    state->bundleIdentifier = std::move (bundleIdentifier);
}

//==============================================================================
AppInfo Driver::findProTools()
{
    @autoreleasepool
    {
        auto& s = *state;
        AppInfo info;
        s.pid = 0;
        s.app = {};
        s.counter = {};
        s.counterNotFound = false;
        s.baseline.reset();

        NSString* identifier = [NSString stringWithUTF8String: s.bundleIdentifier.c_str()];
        NSArray<NSRunningApplication*>* running = identifier != nil
            ? [NSRunningApplication runningApplicationsWithBundleIdentifier: identifier] : nil;

        if ([running count] == 0)
        {
            s.say ("No running application has the bundle identifier " + s.bundleIdentifier);
            return info;
        }

        if ([running count] > 1)
            s.say (std::to_string ((long) [running count]) + " running applications have the bundle identifier "
                   + s.bundleIdentifier + ". The first is used.");

        NSRunningApplication* app = running[0];
        info.running = true;
        info.pid = (int) app.processIdentifier;
        info.name = toStd (app.localizedName);
        info.frontmost = app.isActive;

        if (NSURL* url = app.bundleURL)
            if (NSBundle* bundle = [NSBundle bundleWithURL: url])
            {
                const auto shortVersion = toStd ((NSString*) [bundle objectForInfoDictionaryKey: @"CFBundleShortVersionString"]);
                const auto build = toStd ((NSString*) [bundle objectForInfoDictionaryKey: @"CFBundleVersion"]);
                info.version = shortVersion.empty() ? build : shortVersion;

                if (! build.empty() && build != shortVersion && ! shortVersion.empty())
                    info.version += " (" + build + ")";
            }

        s.pid = app.processIdentifier;
        s.appName = info.name.empty() ? std::string ("Pro Tools") : info.name;
        s.app = adopt (AXUIElementCreateApplication (s.pid));

        // A busy application cannot hold a call for longer than this.
        AXUIElementRef systemWide = AXUIElementCreateSystemWide();
        AXUIElementSetMessagingTimeout (systemWide, messagingTimeoutSeconds);
        CFRelease (systemWide);
        AXUIElementSetMessagingTimeout (ax (s.app), messagingTimeoutSeconds);

        s.say ("Found " + s.appName + " " + info.version + ", process " + std::to_string (info.pid)
               + (info.frontmost ? ", in front" : ", not in front"));
        return info;
    }
}

Result Driver::requireFront()
{
    return state->frontGuard();
}

Result Driver::bringToFront()
{
    @autoreleasepool
    {
        auto& s = *state;

        if (s.pid == 0)
            return Result::bad (s.appName + " has not been found");

        if (s.stopped())
            return State::stoppedResult();

        if (s.isFront())
        {
            s.say (s.appName + " is already the front application");
            return Result::good();
        }

        NSRunningApplication* app = [NSRunningApplication runningApplicationWithProcessIdentifier: s.pid];

        if (app == nil)
            return Result::bad (s.appName + " is no longer running");

        // From macOS 14 the front application has to hand its place over.
        if (@available (macOS 14.0, *))
        {
            const pid_t target = s.pid;
            dispatch_async (dispatch_get_main_queue(), ^
            {
                if (NSRunningApplication* other = [NSRunningApplication runningApplicationWithProcessIdentifier: target])
                    [NSApp yieldActivationToApplication: other];
            });
            usleep (50000);
        }

        const BOOL asked = [app activateWithOptions: NSApplicationActivateAllWindows];
        s.say (std::string ("Asked macOS to bring ") + s.appName + " to the front: " + (asked ? "accepted" : "refused"));

        const auto waitForFront = [&s] (int milliseconds)
        {
            const auto start = Clock::now();

            while (millisecondsSince (start) < milliseconds)
            {
                if (s.isFront())
                    return true;

                if (! s.wait (50))
                    return false;
            }

            return s.isFront();
        };

        if (! waitForFront (1500) && ! s.stopped())
        {
            const AXError error = AXUIElementSetAttributeValue (ax (s.app), kAXFrontmostAttribute, kCFBooleanTrue);
            s.say ("Still not in front. Set its AXFrontmost attribute: " + errorName (error));
            waitForFront (1500);
        }

        if (s.stopped())
            return State::stoppedResult();

        if (! s.isFront())
        {
            std::string why;
            s.front (&why);
            s.say (why);
            return Result::bad (s.appName + " could not be brought to the front");
        }

        s.say (s.appName + " is the front application");

        // A moment for its windows to take the keyboard.
        return s.wait (200) ? Result::good() : State::stoppedResult();
    }
}

//==============================================================================
std::string Driver::describe (const Element& element, int depth)
{
    @autoreleasepool
    {
        return state->describe (element, depth);
    }
}

Result Driver::rememberWindows()
{
    @autoreleasepool
    {
        auto& s = *state;

        if (! s.app)
            return Result::bad (s.appName + " has not been found");

        auto now = s.windowsPatiently (2000);

        if (s.stopped())
            return State::stoppedResult();

        if (! now)
            return s.noAnswer ("when asked for its windows");

        s.say ("Remembered the " + std::to_string (now->size()) + (now->size() == 1 ? " window" : " windows")
               + " open at this point. No key is sent while any other window is open.");
        s.baseline = std::move (now);
        return Result::good();
    }
}

void Driver::logWindows()
{
    @autoreleasepool
    {
        if (state->app)
            state->say (state->appName + " has " + state->windowList());
    }
}

//==============================================================================
Result Driver::findMenuItem (const std::vector<std::string>& path, Element& item, bool& enabled)
{
    @autoreleasepool
    {
        auto& s = *state;
        item = {};
        enabled = false;

        if (! s.app)
            return Result::bad (s.appName + " has not been found");

        std::string pathText;

        for (const auto& p : path)
            pathText += (pathText.empty() ? "" : " > ") + p;

        const auto menuBar = copyElement (ax (s.app), kAXMenuBarAttribute);

        if (! menuBar)
            return Result::bad ("the menu bar of " + s.appName + " could not be read");

        // The items of a menu bar item or of a menu item with a submenu sit inside one AXMenu.
        const auto itemsOf = [] (const Element& parent)
        {
            auto children = childrenOf (parent);

            if (children.size() == 1 && readInfo (ax (children[0])).role == "AXMenu")
                return childrenOf (children[0]);

            return children;
        };

        const auto titlesOf = [] (const std::vector<Element>& items)
        {
            std::string out;

            for (const auto& i : items)
            {
                const auto info = readInfo (ax (i));

                if (info.title.empty())
                    continue;       // separators

                out += "\n  " + quoted (info.title, 80) + (info.hasEnabled && ! info.enabled ? " (switched off)" : "");
            }

            return out.empty() ? std::string ("\n  (none)") : out;
        };

        const auto topLevel = childrenOf (menuBar);
        auto level = topLevel;
        Element found;

        for (size_t depth = 0; depth < path.size(); ++depth)
        {
            if (s.stopped())
                return State::stoppedResult();

            const auto wanted = normalise (path[depth]);
            found = {};

            // An exact match first, then one that differs only in dots at the end.
            for (int pass = 0; pass < 2 && ! found; ++pass)
                for (const auto& candidate : level)
                {
                    if (s.stopped())
                        return State::stoppedResult();

                    const auto candidateInfo = readInfo (ax (candidate));

                    if (! candidateInfo.answered)
                        return s.noAnswer ("while its menus were read");

                    const auto title = normalise (candidateInfo.title);

                    if (pass == 0 ? title == wanted
                                  : (! title.empty() && withoutTrailingDots (title) == withoutTrailingDots (wanted)))
                    {
                        found = candidate;
                        break;
                    }
                }

            if (! found)
            {
                s.say ("Did not find " + quoted (path[depth]) + " while looking for " + pathText + ".");
                s.say ("The top-level menus are:" + titlesOf (topLevel));

                if (depth > 0)
                    s.say ("The items in " + quoted (path[depth - 1]) + " are:" + titlesOf (level));

                return Result::bad (depth == 0 ? "the " + path[depth] + " menu was not found in " + s.appName
                                               : "the menu item " + pathText + " was not found");
            }

            if (depth + 1 < path.size())
            {
                level = itemsOf (found);

                if (level.empty())
                    s.say ("The " + path[depth] + " menu reports no items");
            }
        }

        const auto info = readInfo (ax (found));
        enabled = ! info.hasEnabled || info.enabled;
        item = found;

        // The shortcut, to compare with the one the manual gives.
        std::string shortcut;
        CFTypeRef key = nullptr, modifiers = nullptr;

        if (AXUIElementCopyAttributeValue (ax (found), kAXMenuItemCmdCharAttribute, &key) == kAXErrorSuccess && key != nullptr)
            shortcut = " shortcut key=" + quoted (valueText (key));

        if (AXUIElementCopyAttributeValue (ax (found), kAXMenuItemCmdModifiersAttribute, &modifiers) == kAXErrorSuccess && modifiers != nullptr)
            shortcut += " modifiers=" + valueText (modifiers);

        if (key != nullptr)       CFRelease (key);
        if (modifiers != nullptr) CFRelease (modifiers);

        s.say ("Found the menu item " + pathText + ": " + oneLine (ax (found), info, true) + shortcut);
        s.say (std::string ("The menu item is ") + (enabled ? "switched on" : "switched off"));
        return Result::good();
    }
}

//==============================================================================
Result Driver::readMainCounter (std::string& value)
{
    @autoreleasepool
    {
        auto& s = *state;
        value.clear();

        if (! s.app)
            return Result::bad (s.appName + " has not been found");

        // Found on an earlier call: read the same element again.
        if (s.counter)
        {
            const auto info = readInfo (ax (s.counter));

            if (info.hasValue && ! info.role.empty())
            {
                value = info.value;
                return Result::good();
            }

            s.counter = {};
        }

        // Looked for on an earlier call and not found: the search and its long report are not repeated.
        if (s.counterNotFound)
            return Result::bad ("the Main Counter was not found");

        const auto open = s.windowsPatiently (1000);

        if (s.stopped())
            return State::stoppedResult();

        if (! open)
            return s.noAnswer ("when asked for its windows");

        struct Visit
        {
            Element element;
            int depth = 0;
            std::string inside;     // names of the nearest counter element above this one
        };

        std::deque<Visit> queue;

        for (auto& w : *open)
            queue.push_back ({ w, 0, {} });

        const size_t maxElements = 6000;
        const int maxMilliseconds = 6000;
        const auto start = Clock::now();
        size_t visited = 0;
        std::map<std::string, int> roles;
        std::vector<std::string> textSeen;
        Element best;
        int bestRank = 0;
        int candidates = 0;
        bool gaveUp = false, unanswered = false;

        s.say ("Looking for the Main Counter in the windows of " + s.appName + ": a text element whose title, "
               "description, help or identifier contains \"Counter\", or that sits inside such an element");

        while (! queue.empty() && bestRank < 3)
        {
            if (s.stopped())
                return State::stoppedResult();

            if (visited >= maxElements || millisecondsSince (start) > maxMilliseconds)
            {
                gaveUp = true;
                break;
            }

            auto visit = queue.front();
            queue.pop_front();
            ++visited;

            const auto info = readInfo (ax (visit.element));

            if (! info.answered)
            {
                unanswered = true;
                break;
            }

            ++roles[info.role.empty() ? std::string ("(no role)") : info.role];

            const auto names = info.names();
            const bool named = contains (names, "counter");
            const auto inside = named ? names : visit.inside;
            const bool isTextKind = info.isText() || info.role == "AXStaticText";

            if (! inside.empty())
            {
                // Only text can be the counter: a checkbox, pop-up or slider inside a counter has
                // a value as well. A single digit is believed only from an element named
                // Main Counter, since a 0 or a 1 is as likely to be a switch.
                const auto digits = counterDigits (info.value);
                const bool hasText = isTextKind && info.hasValue && ! info.value.empty();
                const bool mainCounter = contains (inside, "main counter");
                const bool oneDigit = allDigits (digits) && digits.size() == 1;
                int rank = 0;

                if (hasText && mainCounter)
                    rank = 3;
                else if (hasText && ! oneDigit)
                    rank = allDigits (digits) ? 2 : 1;

                ++candidates;
                s.say (std::string ("  candidate") + (named ? "" : " (inside a counter element)") + ": "
                       + oneLine (ax (visit.element), info, false)
                       + (rank > 0 ? "" : ! isTextKind ? "  [not text, not used]"
                                        : ! hasText ? "  [no value, not used]" : "  [a single digit, not used]"));

                if (rank > bestRank)
                {
                    bestRank = rank;
                    best = visit.element;
                }
            }
            else if (info.hasValue && ! info.value.empty() && isTextKind && textSeen.size() < 60)
            {
                textSeen.push_back (oneLine (ax (visit.element), info, false));
            }

            if (visit.depth < 14)
                for (auto& child : childrenOf (visit.element))
                    queue.push_back ({ child, visit.depth + 1, inside });
        }

        s.say ("Looked at " + std::to_string (visited) + " elements in " + std::to_string (millisecondsSince (start)) + " ms"
               + (gaveUp ? " and stopped at the limit before seeing them all" : "")
               + (unanswered ? " and stopped when " + s.appName + " gave no answer in time" : "") + ". "
               + std::to_string (candidates) + (candidates == 1 ? " candidate." : " candidates."));

        if (! best)
        {
            std::string summary = "No counter with a value was found. Kinds of element seen:";

            for (const auto& [role, count] : roles)
                summary += "\n  " + role + " x " + std::to_string (count);

            summary += "\nText elements with a value (the first " + std::to_string (textSeen.size()) + "):";

            for (const auto& line : textSeen)
                summary += "\n  " + line;

            s.say (summary);

            if (unanswered)
                return s.noAnswer ("while the Main Counter was looked for");

            s.counterNotFound = true;
            return Result::bad ("the Main Counter was not found");
        }

        s.counter = best;
        value = readInfo (ax (best)).value;
        s.say ("Using " + oneLine (best, false) + " as the Main Counter. It shows " + quoted (value));
        return Result::good();
    }
}

//==============================================================================
Result Driver::goToSample (long long sample)
{
    @autoreleasepool
    {
        auto& s = *state;

        if (! s.app)
            return Result::bad (s.appName + " has not been found");

        if (sample < 0)
            return Result::bad ("the sample to go to is below zero");

        if (s.quick)
            return goToSampleQuick (sample);

        const auto wanted = std::to_string (sample);
        const auto before = s.windowsPatiently (2000);

        if (s.stopped())
            return State::stoppedResult();

        if (! before)
            return s.noAnswer ("when asked for its windows, so no key was sent");

        // A window that was not open when the run began (an alert, a dialog left open) would
        // get the keys, and keypad Enter would press its default button.
        if (s.baseline)
            if (const auto extra = notIn (*before, *s.baseline); ! extra.empty())
            {
                s.say ("A window is open that was not there at the start of the run. No key is sent:\n" + s.describe (extra.front(), 6));
                return Result::bad ("a window titled " + quoted (readInfo (ax (extra.front())).title) + " saying "
                                    + quoted (s.textIn (extra.front()), 300) + " is open in " + s.appName
                                    + ", so the keys for sample " + wanted + " were not sent. Nothing was pressed in it");
            }

        s.say ("Going to sample " + wanted);

        if (auto r = s.pressKey (keypadStarKey, kCGEventFlagMaskNumericPad, 0, "keypad *"); ! r.ok)
            return r;

        if (! s.wait (120))
            return State::stoppedResult();

        s.say ("Keyboard focus after keypad *: " + oneLine (s.focusedElement()));

        if (auto r = s.typeDigits (wanted); ! r.ok)
            return r;

        if (auto r = s.pressKey (keypadEnterKey, kCGEventFlagMaskNumericPad, 0, "keypad Enter"); ! r.ok)
            return r;

        if (! s.wait (150))
            return State::stoppedResult();

        // Enter outside a number field opens a dialog in Pro Tools, so a window that was not there
        // before means the digits did not go into the counter.
        const auto after = s.windowsPatiently (2000);

        if (s.stopped())
            return State::stoppedResult();

        if (! after)
            return s.noAnswer ("after the keys for sample " + wanted + " were sent, so it is not known whether a window opened");

        if (const auto opened = notIn (*after, *before); ! opened.empty())
        {
            s.say ("A window opened after the keys:\n" + s.describe (opened.front(), 5));
            return Result::bad ("a window titled " + quoted (readInfo (ax (opened.front())).title)
                                + " opened when the sample was typed, so the keys did not go to the Main Counter. "
                                  "Nothing was pressed in it");
        }

        // The counter may take a moment to show the new position.
        std::string shown;
        Result read = Result::bad ({});
        const auto start = Clock::now();

        do
        {
            read = readMainCounter (shown);

            if (! read.ok || counterDigits (shown) == wanted)
                break;

            if (! s.wait (100))
                return State::stoppedResult();
        }
        while (millisecondsSince (start) < 1000);

        if (s.stopped())
            return State::stoppedResult();

        if (! read.ok)
        {
            s.say ("Could not confirm the position: " + read.message);
            return Result::unconfirmed ("the keys for sample " + wanted + " were sent, but the Main Counter could not be read to confirm it");
        }

        if (counterDigits (shown) != wanted)
            return Result::bad ("the Main Counter shows " + quoted (shown) + " after going to sample " + wanted);

        s.say ("The Main Counter shows " + shown);
        return Result::good();
    }
}

//==============================================================================
Result Driver::openIdentifyBeat (const Element& menuItem, Element& dialog)
{
    @autoreleasepool
    {
        auto& s = *state;
        dialog = {};

        if (! s.app || ! menuItem)
            return Result::bad ("the Identify Beat menu item has not been found");

        const auto isIdentifyBeat = [] (const Element& window)
        {
            const auto title = normalise (readInfo (ax (window)).title);
            return contains (title, "bar") && contains (title, "beat");
        };

        const auto before = s.windowsPatiently (2000);

        if (s.stopped())
            return State::stoppedResult();

        if (! before)
            return s.noAnswer ("when asked for its windows, so the menu item was not pressed");

        for (const auto& w : *before)
            if (isIdentifyBeat (w))
            {
                s.say ("A window with Bar and Beat in its title is already open:\n" + s.describe (w, 3));
                return Result::bad ("a window titled " + quoted (readInfo (ax (w)).title) + " was already open before the menu item was pressed");
            }

        if (auto guard = s.frontGuard(); ! guard.ok)
            return guard;

        // A menu item that opens a dialog may not answer until the dialog closes, so no answer
        // in time is not a failure here: the window is looked for either way.
        const auto start = Clock::now();
        const AXError error = AXUIElementPerformAction (ax (menuItem), kAXPressAction);
        s.say ("Pressed the menu item (AXPress): " + errorName (error) + ", " + std::to_string (millisecondsSince (start)) + " ms");

        if (error != kAXErrorSuccess && error != kAXErrorCannotComplete)
            return Result::bad ("the Identify Beat menu item could not be pressed (" + errorName (error) + ")");

        const auto waitStart = Clock::now();

        do
        {
            if (const auto now = s.windows())
                for (auto& w : *now)
                    if (isIdentifyBeat (w))
                    {
                        dialog = w;
                        break;
                    }

            if (dialog)
                break;

            if (! s.wait (s.poll()))
                return State::stoppedResult();
        }
        while (millisecondsSince (waitStart) < 3000);

        if (! dialog)
        {
            s.say ("No window with Bar and Beat in its title appeared in 3 seconds. " + s.appName + " has " + s.windowList());
            const auto now = s.windows();

            if (! now)
                return s.noAnswer ("after the Identify Beat menu item was pressed, so it is not known what opened. Nothing more was pressed");

            const auto opened = notIn (*now, *before);

            for (const auto& w : opened)
                s.say ("This window was not there before:\n" + s.describe (w, 5));

            if (! opened.empty())
                return Result::bad ("the Identify Beat dialog did not appear. A window titled "
                                    + quoted (readInfo (ax (opened.front())).title) + " appeared instead, and nothing was pressed in it");

            return Result::bad ("the Identify Beat dialog did not appear");
        }

        s.say ("The dialog " + quoted (readInfo (ax (dialog)).title) + " appeared after "
               + std::to_string (millisecondsSince (start)) + " ms");

        if (! s.dialogDescribed && ! s.quick)
        {
            s.dialogDescribed = true;
            s.say ("What the dialog contains:\n" + s.describe (dialog, 8));
        }

        return Result::good();
    }
}

//==============================================================================
Result Driver::setIdentifyBeatLocation (const Element& dialog, const std::string& barBeat, bool typeIt)
{
    @autoreleasepool
    {
        auto& s = *state;
        long long wanted[3] = {};

        if (! dialog)
            return Result::bad ("there is no Identify Beat dialog to fill in");

        if (! parseBarBeat (barBeat, wanted))
            return Result::bad ("the location " + quoted (barBeat) + " is not bar|beat|ticks");

        if (wanted[0] < 1 || wanted[1] < 1)
            return Result::bad ("the location " + quoted (barBeat) + " is before bar 1 beat 1");

        if (s.quick)
            return enterLocationQuick (dialog, barBeat);

        // The dialog's text fields with their values as they are at the time of the call.
        const auto textFields = [&dialog, &s]
        {
            auto limits = s.walk (4000);
            auto all = readAll (dialog, 400, limits);
            all.erase (std::remove_if (all.begin(), all.end(), [] (const Read& r) { return ! r.info.isText(); }), all.end());
            return all;
        };

        // What the dialog shows: +1 it holds the wanted location, -1 it holds another one,
        // 0 nothing in it can be read as a location.
        const auto readBack = [&] (std::string& shown)
        {
            const auto fields = textFields();
            shown.clear();

            for (const auto& f : fields)
            {
                long long got[3] = {};

                if (parseBarBeat (f.info.value, got))
                {
                    shown = f.info.value;
                    return (got[0] == wanted[0] && got[1] == wanted[1] && got[2] == wanted[2]) ? 1 : -1;
                }
            }

            if (fields.size() >= 3)
            {
                const std::string parts[3] = { trim (fields[0].info.value), trim (fields[1].info.value), trim (fields[2].info.value) };

                if (allDigits (parts[0]) && allDigits (parts[1]) && allDigits (parts[2])
                    && parts[0].size() <= 9 && parts[1].size() <= 9 && parts[2].size() <= 9)
                {
                    shown = parts[0] + "|" + parts[1] + "|" + parts[2];
                    return (std::stoll (parts[0]) == wanted[0] && std::stoll (parts[1]) == wanted[1]
                            && std::stoll (parts[2]) == wanted[2]) ? 1 : -1;
                }
            }

            return 0;
        };

        const auto fields = textFields();
        std::string list = "The dialog has " + std::to_string (fields.size()) + (fields.size() == 1 ? " text field" : " text fields");

        for (const auto& f : fields)
            list += "\n  " + oneLine (ax (f.element), f.info, true);

        s.say (list);

        if (s.stopped())
            return State::stoppedResult();

        if (fields.empty())
            return Result::bad ("the Identify Beat dialog shows no text field through Accessibility");

        // Whether a location could be read before anything was entered. If it could, it has to
        // be readable afterwards as well.
        std::string shownBefore;
        const bool readableBefore = readBack (shownBefore) != 0;
        s.say (readableBefore ? "Before entry the dialog's location reads as " + quoted (shownBefore)
                              : std::string ("Before entry no field's value reads as bar|beat|ticks"));

        const auto setValue = [&s] (const Element& field, const std::string& text)
        {
            CFStringRef value = CFStringCreateWithCString (kCFAllocatorDefault, text.c_str(), kCFStringEncodingUTF8);
            const AXError error = AXUIElementSetAttributeValue (ax (field), kAXValueAttribute, value);
            CFRelease (value);
            s.say ("Set AXValue to " + quoted (text) + ": " + errorName (error));
            return error == kAXErrorSuccess;
        };

        const auto cancelledNote = [&] ()
        {
            const auto cancelled = cancelDialog (dialog);
            return cancelled.ok ? std::string (". The dialog was cancelled")
                                : ". The dialog could not be cancelled: " + cancelled.message;
        };

        std::string strategy;
        std::string shown;
        int match = 0;

        if (! typeIt)
        {
            // (a) One field that already holds a bar|beat|ticks value: set its value.
            for (const auto& f : fields)
            {
                long long current[3] = {};

                if (! parseBarBeat (f.info.value, current))
                    continue;

                // Pro Tools reports its location field as not settable, and asking anyway can
                // move the keyboard off the field. So a field that says no is left alone.
                if (! isSettable (ax (f.element), kAXValueAttribute))
                {
                    s.say ("The location field (" + quoted (f.info.value) + ") does not allow its value to be set. Not trying.");
                    break;
                }

                s.say ("Strategy a: one field holds bar|beat|ticks (" + quoted (f.info.value) + "). Setting its value.");

                if (auto guard = s.frontGuard(); ! guard.ok)
                    return guard;

                if (setValue (f.element, barBeat) && s.wait (80))
                {
                    match = readBack (shown);
                    s.say ("The field then shows " + quoted (readInfo (ax (f.element)).value));

                    if (match == 1)
                        strategy = "setting the value of the location field (strategy a)";
                }

                break;
            }

            // (b) Three number fields in a row, the third with the three digits of the ticks.
            if (strategy.empty() && fields.size() >= 3)
            {
                const std::string parts[3] = { trim (fields[0].info.value), trim (fields[1].info.value), trim (fields[2].info.value) };

                if (allDigits (parts[0]) && allDigits (parts[1]) && allDigits (parts[2]) && parts[2].size() >= 3)
                {
                    s.say ("Strategy b: the first three fields hold numbers (" + parts[0] + ", " + parts[1] + ", " + parts[2]
                           + "). Setting bar, beat and ticks one by one.");

                    if (auto guard = s.frontGuard(); ! guard.ok)
                        return guard;

                    char ticks[16];
                    std::snprintf (ticks, sizeof (ticks), "%03lld", wanted[2]);
                    const bool allSet = setValue (fields[0].element, std::to_string (wanted[0]))
                                        && setValue (fields[1].element, std::to_string (wanted[1]))
                                        && setValue (fields[2].element, ticks);

                    if (allSet && s.wait (80))
                    {
                        match = readBack (shown);

                        if (match == 1)
                            strategy = "setting the bar, beat and tick fields (strategy b)";
                    }
                }
            }

            if (s.stopped())
                return State::stoppedResult();

            if (strategy.empty())
                s.say ("Setting values did not give the wanted location. Typing it instead.");
        }

        // (c) Type it: focus the first field, select what is in it, then bar . beat . ticks
        // (the period moves to the next part of a Pro Tools counter).
        if (strategy.empty())
        {
            s.say (std::string ("Strategy c: typing the location into the first text field")
                   + (typeIt ? " (typing was chosen in the panel)" : ""));

            const auto& first = fields.front().element;

            if (auto guard = s.frontGuard(); ! guard.ok)
                return guard;

            // Pro Tools opens the dialog with the keyboard already in the location field, so the
            // focus is only touched when it is somewhere else: first by pressing the field,
            // then by asking for the focus.
            // Pro Tools names its OK button as the application's focused element while the
            // keyboard is in the location field, so the field's own focused state is what counts.
            const auto keyboardInFirst = [&first]
            {
                return readInfo (ax (first)).focused;
            };

            if (keyboardInFirst())
            {
                s.say ("The keyboard is already in the location field. Leaving the focus alone.");
            }
            else
            {
                AXError focusError = AXUIElementPerformAction (ax (first), kAXPressAction);
                s.say ("The keyboard is not in the location field. Pressed the field (AXPress): " + errorName (focusError));

                if (! s.wait (120))
                    return State::stoppedResult();

                if (! keyboardInFirst())
                {
                    focusError = AXUIElementSetAttributeValue (ax (first), kAXFocusedAttribute, kCFBooleanTrue);
                    s.say ("Still not there. Set AXFocused on the field: " + errorName (focusError));

                    if (! s.wait (120))
                        return State::stoppedResult();
                }
            }

            // The keys go wherever the keyboard is, and in the Edit window digits and periods
            // are commands. So nothing is typed unless Accessibility shows the keyboard
            // in a text field of this dialog and the dialog as the focused window.
            const auto focused = s.focusedElement();
            Element focusedWindow;
            const AXError windowError = copyElementChecked (ax (s.app), kAXFocusedWindowAttribute, focusedWindow);
            const bool fieldSaysFocused = keyboardInFirst();
            const bool inField = fieldSaysFocused
                                 || (focused && readInfo (ax (focused)).isText() && isInside (focused, dialog));
            s.say (std::string ("The location field says it ") + (fieldSaysFocused ? "has" : "does not have") + " the keyboard");
            const bool dialogHasKeys = same (focusedWindow, dialog);

            s.say ("Keyboard focus before typing: " + oneLine (focused));
            s.say ("Focused window before typing: " + (focusedWindow ? oneLine (focusedWindow, false)
                                                                     : "not given (" + errorName (windowError) + ")"));

            if (! inField || ! dialogHasKeys)
            {
                s.say (std::string ("Nothing is typed: ")
                       + (! dialogHasKeys ? "the Identify Beat dialog is not the focused window"
                                          : "the keyboard focus is not in a text field of the Identify Beat dialog")
                       + ". Cancelling the dialog.");
                return Result::bad ("the keyboard was not in a text field of the Identify Beat dialog, so the location was not typed"
                                    + cancelledNote());
            }

            if (! fieldSaysFocused)
                s.say ("The keyboard is in a text field of the dialog other than the first one. Typing there.");

            char ticks[16];
            std::snprintf (ticks, sizeof (ticks), "%03lld", wanted[2]);

            // The field opens with its bar part selected: digits replace the selected part and
            // a period moves to the next one. What the field shows is logged after each part.
            const auto showField = [&s, &first] (const char* after)
            {
                s.wait (60);
                s.say (std::string ("After ") + after + " the field shows " + quoted (readInfo (ax (first)).value));
            };

            Result typed = s.typeDigits (std::to_string (wanted[0]));
            if (typed.ok) showField ("the bar");
            if (typed.ok) typed = s.pressKey (periodKey, 0, (UniChar) '.', "period");
            if (typed.ok) typed = s.typeDigits (std::to_string (wanted[1]));
            if (typed.ok) showField ("the beat");
            if (typed.ok) typed = s.pressKey (periodKey, 0, (UniChar) '.', "period");
            if (typed.ok) typed = s.typeDigits (ticks);
            if (typed.ok) showField ("the ticks");

            if (! typed.ok)
                return typed;

            if (! s.wait (120))
                return State::stoppedResult();

            match = readBack (shown);
            strategy = "typing it (strategy c)";
        }

        // What every field holds afterwards, whichever way it was filled in.
        std::string after = "The text fields afterwards:";

        for (const auto& f : textFields())
            after += "\n  " + oneLine (ax (f.element), f.info, false);

        s.say (after);

        if (s.stopped())
            return State::stoppedResult();

        if (match == 1)
        {
            s.say ("The dialog shows " + quoted (shown) + ", as wanted, after " + strategy);
            return Result::good ("the location was entered by " + strategy);
        }

        if (match == 0 && ! readableBefore)
        {
            s.say ("Could not confirm the location: no field's value reads as bar|beat|ticks, before or after " + strategy);
            return Result::unconfirmed ("the location was entered by " + strategy + ", but it could not be read back");
        }

        if (match == 0)
        {
            // Readable before and not after: the entry went wrong, it is not merely unreadable.
            s.say ("The dialog's location read as " + quoted (shownBefore) + " before entry and cannot be read after " + strategy
                   + ". Cancelling the dialog.");
            return Result::bad ("the location in the Identify Beat dialog no longer reads as bar|beat|ticks after " + strategy
                                + ", so " + barBeat + " did not go in as wanted" + cancelledNote());
        }

        s.say ("The dialog shows " + quoted (shown) + " and " + quoted (barBeat) + " was wanted. Cancelling the dialog.");
        return Result::bad ("the Identify Beat dialog showed " + quoted (shown) + " after " + strategy + ", not " + barBeat
                            + cancelledNote());
    }
}

//==============================================================================
namespace
{
    Element findButton (const Element& dialog, const char* normalisedTitle, Walk walk)
    {
        for (const auto& [element, info] : readAll (dialog, 400, walk))
            if (info.role == "AXButton" && normalise (info.title) == normalisedTitle)
                return element;

        return {};
    }
}

Result Driver::confirmIdentifyBeat (const Element& dialog, bool& okPressed)
{
    @autoreleasepool
    {
        auto& s = *state;
        okPressed = false;

        if (! dialog)
            return Result::bad ("there is no Identify Beat dialog to confirm");

        Element button;

        if (s.quick)
        {
            if (auto read = s.readParts (dialog); ! read.ok)
                return read;

            button = s.parts.ok;
        }
        else if (button = findButton (dialog, "ok", s.walk (4000)); button)
        {
            s.say ("Found the OK button by its title: " + oneLine (button));
        }
        else
        {
            button = copyElement (ax (dialog), kAXDefaultButtonAttribute);

            if (button)
                s.say ("No button is titled OK. Using the dialog's default button: " + oneLine (button));
        }

        if (s.stopped())
            return State::stoppedResult();

        if (! button)
        {
            s.say ("No OK button and no default button. The dialog:\n" + s.describe (dialog, 8));
            return Result::bad ("the Identify Beat dialog has no OK button that can be found");
        }

        if (const auto info = readInfo (ax (button)); info.hasEnabled && ! info.enabled)
            return Result::bad ("the OK button of the Identify Beat dialog is switched off");

        const auto before = s.windowsPatiently (2000);

        if (s.stopped())
            return State::stoppedResult();

        if (! before)
            return s.noAnswer ("when asked for its windows, so OK was not pressed");

        if (auto guard = s.frontGuard(); ! guard.ok)
            return guard;

        const AXError error = AXUIElementPerformAction (ax (button), kAXPressAction);
        s.say ("Pressed OK (AXPress): " + errorName (error));

        if (error != kAXErrorSuccess && error != kAXErrorCannotComplete)
            return Result::bad ("the OK button could not be pressed (" + errorName (error) + ")");

        okPressed = true;
        const auto closed = s.waitUntilClosed (dialog, 3000);

        if (closed == State::Closed::stopped)
            return State::stoppedResult();

        if (closed == State::Closed::noAnswer)
            return s.noAnswer ("for 3 seconds after OK was pressed, so it is not known whether the dialog closed. Nothing more was pressed");

        // An alert can come up a moment after the dialog goes, so once the dialog has closed the
        // windows are watched for half a second more. At the quick pace it is a fifth of a
        // second: the next move looks at the windows before its first key and again before its
        // keypad Enter, which comes more than half a second after this OK.
        std::vector<Element> opened;
        bool answered = false;
        const auto watchStart = Clock::now();
        const int watchMilliseconds = s.quick ? 200 : 500;

        do
        {
            if (const auto now = s.windows())
            {
                answered = true;
                opened = notIn (*now, *before);

                if (! opened.empty())
                    break;
            }

            if (closed != State::Closed::closed)
                break;

            if (! s.wait (s.poll()))
                return State::stoppedResult();
        }
        while (millisecondsSince (watchStart) < watchMilliseconds);

        if (! opened.empty())
        {
            const auto& alert = opened.front();
            s.say ("Another window appeared after OK. Nothing is pressed in it:\n" + s.describe (alert, 6));
            return Result::bad ("after OK, " + s.appName + " showed a window titled " + quoted (readInfo (ax (alert)).title)
                                + " saying " + quoted (s.textIn (alert), 300) + ". Nothing was pressed in it");
        }

        if (closed != State::Closed::closed)
        {
            s.say ("The dialog is still open 3 seconds after OK:\n" + s.describe (dialog, 8));
            return Result::bad ("the Identify Beat dialog was still open 3 seconds after OK was pressed. It was left open");
        }

        if (! answered)
            return s.noAnswer ("after the dialog closed, so it is not known whether another window appeared");

        s.say (s.quick ? "The dialog closed and no other window appeared in the 200 ms after"
                       : "The dialog closed and no other window appeared in the half second after");
        return Result::good();
    }
}

Result Driver::cancelDialog (const Element& dialog)
{
    @autoreleasepool
    {
        auto& s = *state;

        if (! dialog)
            return Result::good();

        // Closed only if Pro Tools says so. Without an answer nothing is known about the dialog.
        auto open = State::Open::unknown;
        const auto askStart = Clock::now();

        do
        {
            open = s.windowIsOpen (dialog);
        }
        while (open == State::Open::unknown && s.wait (50) && millisecondsSince (askStart) < 2000);

        if (open == State::Open::no)
        {
            s.say ("The dialog is already closed");
            return Result::good();
        }

        if (s.stopped())
            return State::stoppedResult();

        if (open == State::Open::unknown)
            return s.noAnswer ("when asked for its windows, so it is not known whether the Identify Beat dialog is open. Nothing was pressed");

        Element button;

        if (s.quick && s.readParts (dialog).ok)
            button = s.parts.cancel;
        else
            button = findButton (dialog, "cancel", s.walk (4000));

        if (! button)
            button = copyElement (ax (dialog), kAXCancelButtonAttribute);

        if (auto guard = s.frontGuard(); ! guard.ok)
            return guard;

        if (button)
        {
            const AXError error = AXUIElementPerformAction (ax (button), kAXPressAction);
            s.say ("Pressed Cancel (AXPress on " + oneLine (button, false) + "): " + errorName (error));

            if (error != kAXErrorSuccess && error != kAXErrorCannotComplete)
                return Result::bad ("the Cancel button could not be pressed (" + errorName (error) + "). The dialog was left open");
        }
        else
        {
            s.say ("No Cancel button was found. Pressing Escape.");

            if (auto r = s.pressKey (escapeKey, 0, 0, "Escape"); ! r.ok)
                return r;
        }

        switch (s.waitUntilClosed (dialog, 3000))
        {
            case State::Closed::closed:
                break;

            case State::Closed::stopped:
                return State::stoppedResult();

            case State::Closed::noAnswer:
                return s.noAnswer ("for 3 seconds after Cancel, so it is not known whether the Identify Beat dialog closed");

            case State::Closed::stillOpen:
                s.say ("The dialog is still open 3 seconds after Cancel:\n" + s.describe (dialog, 8));
                return Result::bad ("the Identify Beat dialog was still open 3 seconds after Cancel. It was left open");
        }

        s.say ("The dialog was cancelled and has closed");
        return Result::good();
    }
}

//==============================================================================
void Driver::setQuick (bool quick, bool slowKeys)
{
    state->quick = quick;
    state->keyHoldMicroseconds = (quick && ! slowKeys) ? 8000 : 30000;
    state->keyGapMilliseconds = (quick && ! slowKeys) ? 8 : 50;
}

Result Driver::pause (int milliseconds)
{
    return state->wait (milliseconds) ? Result::good() : State::stoppedResult();
}

void Driver::keepInViewEverywhere (void* nativeView)
{
    @autoreleasepool
    {
        if (nativeView == nullptr)
            return;

        NSWindow* window = [(__bridge NSView*) nativeView window];

        if (window != nil)
        {
            [window setCollectionBehavior: [window collectionBehavior] | NSWindowCollectionBehaviorCanJoinAllSpaces
                                           | NSWindowCollectionBehaviorFullScreenAuxiliary];
            [window setHidesOnDeactivate: NO];
        }
    }
}

Result Driver::checkWindowsAtStart()
{
    @autoreleasepool
    {
        auto& s = *state;

        if (! s.app)
            return Result::bad (s.appName + " has not been found");

        const auto all = s.windowsPatiently (2000);

        if (s.stopped())
            return State::stoppedResult();

        if (! all)
            return s.noAnswer ("when asked for its windows");

        std::string list = s.appName + " has " + std::to_string (all->size()) + (all->size() == 1 ? " window" : " windows");
        std::string editTitle, editDocument, modalTitle, identifyTitle;
        bool editFound = false, modalFound = false, identifyFound = false;

        for (const auto& w : *all)
        {
            const auto info = readInfo (ax (w));

            if (! info.answered)
                return s.noAnswer ("while its windows were read");

            std::string document;
            CFTypeRef value = nullptr;

            if (AXUIElementCopyAttributeValue (ax (w), kAXDocumentAttribute, &value) == kAXErrorSuccess && value != nullptr)
                document = valueText (value);

            if (value != nullptr)
                CFRelease (value);

            bool modal = false;
            value = nullptr;

            if (AXUIElementCopyAttributeValue (ax (w), kAXModalAttribute, &value) == kAXErrorSuccess && value != nullptr
                && CFGetTypeID (value) == CFBooleanGetTypeID())
                modal = CFBooleanGetValue ((CFBooleanRef) value);

            if (value != nullptr)
                CFRelease (value);

            list += "\n  " + oneLine (ax (w), info, false) + (modal ? " modal" : "")
                    + (document.empty() ? std::string (" no AXDocument") : " AXDocument=" + quoted (document, 400));

            const auto title = normalise (info.title);

            if (! editFound && title.rfind ("edit:", 0) == 0)
            {
                editFound = true;
                editTitle = info.title;
                editDocument = document;
            }

            if (! identifyFound && contains (title, "bar") && contains (title, "beat"))
            {
                identifyFound = true;
                identifyTitle = info.title;
            }
            else if (! modalFound && modal)
            {
                modalFound = true;
                modalTitle = info.title;
            }
        }

        s.say (list);

        if (! editFound)
            s.say ("Session document: no window has a title that starts with \"Edit:\"");
        else if (! editDocument.empty())
            s.say ("Session document: the Edit window gives it as " + quoted (editDocument, 400));
        else
            s.say ("Session document: the Edit window gives no AXDocument. Its title is " + quoted (editTitle, 200));

        if (identifyFound)
            return Result::bad ("a window titled " + quoted (identifyTitle) + " is already open in " + s.appName
                                + ". Close it and try again");

        if (modalFound)
            return Result::bad ("a dialog titled " + quoted (modalTitle) + " is open in " + s.appName + ". Close it and try again");

        return Result::good();
    }
}

//==============================================================================
Result Driver::goToSampleQuick (long long sample)
{
    @autoreleasepool
    {
        auto& s = *state;

        if (! s.counter)
            return Result::bad ("the Main Counter has not been found, so the cursor cannot be moved");

        const auto wanted = std::to_string (sample);
        const auto start = Clock::now();
        const auto before = s.windowsPatiently (2000);

        if (s.stopped())
            return State::stoppedResult();

        if (! before)
            return s.noAnswer ("when asked for its windows, so no key was sent");

        // A window that was not open when the run began would get the keys.
        if (s.baseline)
            if (const auto extra = notIn (*before, *s.baseline); ! extra.empty())
            {
                s.say ("A window is open that was not there at the start of the run. No key is sent:\n" + s.describe (extra.front(), 6));
                return Result::bad ("a window titled " + quoted (readInfo (ax (extra.front())).title) + " saying "
                                    + quoted (s.textIn (extra.front()), 300) + " is open in " + s.appName
                                    + ", so the keys for sample " + wanted + " were not sent. Nothing was pressed in it");
            }

        if (auto r = s.pressKey (keypadStarKey, kCGEventFlagMaskNumericPad, 0, "keypad *"); ! r.ok)
            return r;

        // The counter takes the keyboard. It is given up to 120 ms to say so; the digits are
        // typed either way, as they were when this wait was a fixed one.
        bool counterHasKeys = false;

        for (const auto asked = Clock::now(); millisecondsSince (asked) < 120;)
        {
            if (readInfo (ax (s.counter)).focused)
            {
                counterHasKeys = true;
                break;
            }

            if (! s.wait (5))
                return State::stoppedResult();
        }

        if (! s.counterFocusLogged)
        {
            s.counterFocusLogged = true;
            s.say (counterHasKeys ? "After keypad * the Main Counter says it has the keyboard"
                                  : "After keypad * the Main Counter does not say it has the keyboard. Waited 120 ms");
        }

        if (auto r = s.typeDigits (wanted); ! r.ok)
            return r;

        // Keypad Enter presses the default button of whatever window has the keyboard, so the
        // windows are looked at once more before it is sent.
        const auto beforeEnter = s.windowsPatiently (2000);

        if (s.stopped())
            return State::stoppedResult();

        if (! beforeEnter)
            return s.noAnswer ("when asked for its windows after the digits of sample " + wanted + ", so keypad Enter was not sent");

        if (const auto opened = notIn (*beforeEnter, *before); ! opened.empty())
        {
            s.say ("A window opened while the sample was typed. Keypad Enter is not sent:\n" + s.describe (opened.front(), 5));
            return Result::bad ("a window titled " + quoted (readInfo (ax (opened.front())).title)
                                + " opened while sample " + wanted + " was typed, so keypad Enter was not sent. "
                                  "Nothing was pressed in it");
        }

        if (auto r = s.pressKey (keypadEnterKey, kCGEventFlagMaskNumericPad, 0, "keypad Enter"); ! r.ok)
            return r;

        // The counter must show the sample. It is asked until it does, for as long as the fixed
        // waits it replaces.
        std::string shown;
        bool arrived = false, answered = true;
        const auto entered = Clock::now();

        do
        {
            const auto info = readInfo (ax (s.counter));
            answered = info.answered && info.hasValue;
            shown = info.value;

            if (answered && counterDigits (shown) == wanted)
            {
                arrived = true;
                break;
            }
        }
        while (s.wait (s.poll()) && millisecondsSince (entered) < 1150);

        if (s.stopped())
            return State::stoppedResult();

        const auto after = s.windowsPatiently (2000);

        if (s.stopped())
            return State::stoppedResult();

        if (! after)
            return s.noAnswer ("after the keys for sample " + wanted + " were sent, so it is not known whether a window opened");

        if (const auto opened = notIn (*after, *before); ! opened.empty())
        {
            s.say ("A window opened after the keys:\n" + s.describe (opened.front(), 5));
            return Result::bad ("a window titled " + quoted (readInfo (ax (opened.front())).title)
                                + " opened when the sample was typed, so the keys did not go to the Main Counter. "
                                  "Nothing was pressed in it");
        }

        if (! arrived)
            return Result::bad (answered ? "the Main Counter shows " + quoted (shown) + " after going to sample " + wanted
                                         : "the Main Counter could not be read after going to sample " + wanted);

        s.say ("The Main Counter shows " + shown + ", " + std::to_string (millisecondsSince (start)) + " ms for the move");
        return Result::good();
    }
}

//==============================================================================
Result Driver::readIdentifyBeatLocation (const Element& dialog, std::string& shown)
{
    @autoreleasepool
    {
        auto& s = *state;
        shown.clear();

        if (! dialog)
            return Result::bad ("there is no Identify Beat dialog to read");

        if (auto read = s.readParts (dialog); ! read.ok)
            return read;

        if (! s.parts.field)
            return Result::bad ("the Identify Beat dialog shows no location field through Accessibility");

        long long got[3] = {};

        if (! s.readLocation (shown, got, 300))
            return s.stopped() ? State::stoppedResult()
                               : Result::bad ("the location field of the Identify Beat dialog shows " + quoted (shown)
                                              + ", which is not bar|beat|ticks");

        return Result::good();
    }
}

Result Driver::enterLocationQuick (const Element& dialog, const std::string& barBeat)
{
    @autoreleasepool
    {
        auto& s = *state;
        long long wanted[3] = {};

        if (! parseBarBeat (barBeat, wanted))
            return Result::bad ("the location " + quoted (barBeat) + " is not bar|beat|ticks");

        const auto cancelledNote = [&]
        {
            const auto cancelled = cancelDialog (dialog);
            return cancelled.ok ? std::string (". The dialog was cancelled")
                                : ". The dialog could not be cancelled: " + cancelled.message;
        };

        if (auto read = s.readParts (dialog); ! read.ok)
            return read;

        if (! s.parts.field)
        {
            s.say ("No location field was found in the dialog:\n" + s.describe (dialog, 8));
            return Result::bad ("the Identify Beat dialog shows no location field through Accessibility" + cancelledNote());
        }

        const auto& field = s.parts.field;
        std::string suggested;
        long long before[3] = {};

        if (! s.readLocation (suggested, before, 300))
        {
            if (s.stopped())
                return State::stoppedResult();

            return Result::bad ("the location field of the Identify Beat dialog shows " + quoted (suggested)
                                + " before anything is typed, which is not bar|beat|ticks" + cancelledNote());
        }

        s.say ("The dialog suggests " + quoted (suggested));

        // Pro Tools opens the dialog with the keyboard already in the location field, and names
        // its OK button as the application's focused element meanwhile, so the field's own
        // focused state is what counts. The focus is only touched when it is somewhere else.
        const auto keyboardInField = [&field]
        {
            return readInfo (ax (field)).focused;
        };

        const auto waitForKeyboard = [&] (int milliseconds)
        {
            const auto asked = Clock::now();

            while (! keyboardInField() && millisecondsSince (asked) < milliseconds)
                if (! s.wait (s.poll()))
                    return false;

            return true;
        };

        if (! keyboardInField())
        {
            if (auto guard = s.frontGuard(); ! guard.ok)
                return guard;

            AXError focusError = AXUIElementPerformAction (ax (field), kAXPressAction);
            s.say ("The keyboard is not in the location field. Pressed the field (AXPress): " + errorName (focusError));

            if (! waitForKeyboard (120))
                return State::stoppedResult();

            if (! keyboardInField())
            {
                focusError = AXUIElementSetAttributeValue (ax (field), kAXFocusedAttribute, kCFBooleanTrue);
                s.say ("Still not there. Set AXFocused on the field: " + errorName (focusError));

                if (! waitForKeyboard (120))
                    return State::stoppedResult();
            }
        }

        // In the Edit window digits and periods are commands, so nothing is typed unless the
        // field says it has the keyboard and the dialog is the focused window.
        Element focusedWindow;
        const AXError windowError = copyElementChecked (ax (s.app), kAXFocusedWindowAttribute, focusedWindow);
        const bool fieldHasKeys = keyboardInField();
        const bool dialogHasKeys = same (focusedWindow, dialog);

        if (! fieldHasKeys || ! dialogHasKeys)
        {
            s.say (std::string ("Nothing is typed: ")
                   + (! dialogHasKeys ? "the Identify Beat dialog is not the focused window ("
                                            + (focusedWindow ? oneLine (focusedWindow, false) : "not given, " + errorName (windowError)) + ")"
                                      : std::string ("the location field does not say it has the keyboard"))
                   + ". Cancelling the dialog.");
            return Result::bad ("the keyboard was not in the location field of the Identify Beat dialog, so the location was not typed"
                                + cancelledNote());
        }

        char ticks[16];
        std::snprintf (ticks, sizeof (ticks), "%03lld", wanted[2]);

        // The field opens with its bar part selected: digits replace the selected part and a
        // period moves to the next one.
        Result typed = s.typeDigits (std::to_string (wanted[0]));
        if (typed.ok) typed = s.pressKey (periodKey, 0, (UniChar) '.', "period");
        if (typed.ok) typed = s.typeDigits (std::to_string (wanted[1]));
        if (typed.ok) typed = s.pressKey (periodKey, 0, (UniChar) '.', "period");
        if (typed.ok) typed = s.typeDigits (ticks);

        if (! typed.ok)
            return typed;

        // The field must show what was typed. It is asked until it does, for as long as the
        // fixed waits it replaces.
        std::string shown;
        long long got[3] = {};
        bool readable = false, matches = false;
        const auto asked = Clock::now();

        do
        {
            shown = readInfo (ax (field)).value;
            readable = parseBarBeat (shown, got);
            matches = readable && got[0] == wanted[0] && got[1] == wanted[1] && got[2] == wanted[2];
        }
        while (! matches && s.wait (s.poll()) && millisecondsSince (asked) < 300);

        if (s.stopped())
            return State::stoppedResult();

        if (matches)
        {
            s.say ("The dialog shows " + quoted (shown) + ", as typed");
            return Result::good ("the location was typed and read back");
        }

        s.say ("The dialog shows " + quoted (shown) + " and " + quoted (barBeat) + " was wanted. Cancelling the dialog.");
        return Result::bad ("the Identify Beat dialog showed " + quoted (shown) + " after " + barBeat + " was typed"
                            + cancelledNote());
    }
}

//==============================================================================
Result Driver::sendUndo()
{
    @autoreleasepool
    {
        auto& s = *state;

        if (! s.app)
            return Result::bad (s.appName + " has not been found");

        const auto now = s.windowsPatiently (2000);

        if (s.stopped())
            return State::stoppedResult();

        if (! now)
            return s.noAnswer ("when asked for its windows, so Cmd+Z was not sent");

        if (s.baseline)
            if (const auto extra = notIn (*now, *s.baseline); ! extra.empty())
            {
                s.say ("A window is open that was not there at the start. Cmd+Z is not sent:\n" + s.describe (extra.front(), 5));
                return Result::bad ("a window titled " + quoted (readInfo (ax (extra.front())).title) + " is open in " + s.appName
                                    + ", so Cmd+Z was not sent. Nothing was pressed in it");
            }

        return s.pressKey (zKey, kCGEventFlagMaskCommand, 0, "Cmd+Z");
    }
}

} // namespace tt::pt
