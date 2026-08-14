#pragma once

#include <JuceHeader.h>
#include <functional>
#include <memory>

// JUCE's Linux/X11 peer receives the pointer emulated by Xorg, but not the
// additional contacts reported by a type-B multitouch touchscreen.  This
// bridge reads those contacts passively from evdev.  It never grabs the device:
// the first finger therefore continues through JUCE exactly as before, while
// only the secondary fingers are forwarded here.
class LinuxMultitouchBridge final
{
public:
    enum class Phase
    {
        down,
        up
    };

    struct Contact
    {
        int id = -1;
        juce::Point<float> normalisedPosition;
        Phase phase = Phase::down;
    };

    using Callback = std::function<void(const Contact&)>;

    explicit LinuxMultitouchBridge(Callback callback);
    ~LinuxMultitouchBridge();

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LinuxMultitouchBridge)
};
