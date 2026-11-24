#include "CaptureInbox.h"

#include <mutex>

namespace tt::capture
{

namespace
{
    std::mutex receiverLock;
    std::shared_ptr<std::function<void (const Capture&)>> currentReceiver;
}

void setReceiver (std::function<void (const Capture&)> receiver)
{
    const std::lock_guard<std::mutex> hold (receiverLock);
    currentReceiver = receiver ? std::make_shared<std::function<void (const Capture&)>> (std::move (receiver)) : nullptr;
}

bool deliver (const Capture& capture)
{
    std::shared_ptr<std::function<void (const Capture&)>> receiver;

    {
        const std::lock_guard<std::mutex> hold (receiverLock);
        receiver = currentReceiver;
    }

    if (receiver == nullptr)
        return false;

    // Handed over later on the message thread, to whichever window is registered by then.
    juce::MessageManager::callAsync ([capture]
    {
        std::shared_ptr<std::function<void (const Capture&)>> now;

        {
            const std::lock_guard<std::mutex> hold (receiverLock);
            now = currentReceiver;
        }

        if (now != nullptr)
            (*now) (capture);
    });

    return true;
}

} // namespace tt::capture
