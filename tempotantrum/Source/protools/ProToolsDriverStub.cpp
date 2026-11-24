#include "ProToolsDriver.h"

// The driver on systems other than macOS: every step fails and says why. The macOS driver is
// ProToolsDriver.mm; CMakeLists.txt builds one or the other.
namespace tt::pt
{

namespace
{
    Result onlyOnMac()
    {
        return Result::bad ("driving Pro Tools from outside is only built for macOS");
    }
}

struct Driver::State
{
    Log log;
};

Driver::Driver (Log log, Cancelled)
    : state (std::make_unique<State>())
{
    state->log = std::move (log);
}

Driver::~Driver() = default;

bool Driver::accessibilityAllowed()                                    { return false; }
void Driver::requestAccessibility()                                    {}
bool Driver::keyEventsAllowed()                                        { return false; }
std::string Driver::systemVersion()                                    { return "not macOS"; }
bool Driver::isRunning (const std::string&)                            { return false; }
void Driver::setBundleIdentifier (std::string)                         {}
AppInfo Driver::findProTools()                                         { return {}; }
Result Driver::bringToFront()                                          { return onlyOnMac(); }
Result Driver::requireFront()                                          { return onlyOnMac(); }
std::string Driver::describe (const Element&, int)                     { return {}; }
void Driver::logWindows()                                              {}

Result Driver::findMenuItem (const std::vector<std::string>&, Element& item, bool& enabled)
{
    item = {};
    enabled = false;
    return onlyOnMac();
}

Result Driver::readMainCounter (std::string& value)
{
    value.clear();
    return onlyOnMac();
}

Result Driver::rememberWindows()                                       { return onlyOnMac(); }
Result Driver::goToSample (long long)                                  { return onlyOnMac(); }

Result Driver::openIdentifyBeat (const Element&, Element& dialog)
{
    dialog = {};
    return onlyOnMac();
}

Result Driver::setIdentifyBeatLocation (const Element&, const std::string&, bool) { return onlyOnMac(); }

Result Driver::confirmIdentifyBeat (const Element&, bool& okPressed)
{
    okPressed = false;
    return onlyOnMac();
}


Result Driver::cancelDialog (const Element&)                           { return onlyOnMac(); }

} // namespace tt::pt
