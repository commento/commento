#include "LinuxMultitouchBridge.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <utility>

#if JUCE_LINUX
 #include <array>
 #include <cerrno>
 #include <cstdio>
 #include <cstring>
 #include <fcntl.h>
 #include <linux/input.h>
 #include <poll.h>
 #include <unistd.h>
 #include <sys/ioctl.h>
#endif

struct LinuxMultitouchBridge::Implementation
{
    explicit Implementation(Callback callbackToUse)
        : callback(std::move(callbackToUse))
    {
       #if JUCE_LINUX
        worker = std::thread([this] { run(); });
       #endif
    }

    ~Implementation()
    {
       #if JUCE_LINUX
        shouldStop.store(true, std::memory_order_relaxed);
        if (worker.joinable())
            worker.join();
       #endif
    }

private:
    Callback callback;

   #if JUCE_LINUX
    static constexpr int maximumSlots = 32;

    struct Slot
    {
        int trackingId = -1;
        int endingId = -1;
        int x = 0;
        int y = 0;
        bool began = false;
        bool ended = false;
        bool forwarded = false;
    };

    template <std::size_t size>
    static bool bitIsSet(const std::array<unsigned long, size>& bits,
                         int bit) noexcept
    {
        constexpr auto bitsPerWord = static_cast<int>(
            sizeof(unsigned long) * 8u);
        const auto word = bit / bitsPerWord;
        return juce::isPositiveAndBelow(word, static_cast<int>(size))
            && (bits[static_cast<std::size_t>(word)]
                & (1ul << (bit % bitsPerWord))) != 0ul;
    }

    static float normalise(int value, const input_absinfo& range) noexcept
    {
        const auto span = range.maximum - range.minimum;
        return span > 0
            ? juce::jlimit(0.0f, 1.0f,
                static_cast<float>(value - range.minimum)
                    / static_cast<float>(span))
            : 0.0f;
    }

    void postContact(int id, const Slot& slot, Phase phase)
    {
        Contact contact;
        contact.id = id;
        contact.normalisedPosition = {
            normalise(slot.x, xRange), normalise(slot.y, yRange)
        };
        contact.phase = phase;

        // Copying the callback makes queued releases safe if the bridge is
        // destroyed before the message thread has drained its queue.  The UI
        // callback itself uses a Component::SafePointer.
        auto callbackCopy = callback;
        juce::MessageManager::callAsync(
            [callbackCopy = std::move(callbackCopy), contact]
            {
                if (callbackCopy)
                    callbackCopy(contact);
            });
    }

    void releaseForwardedContacts()
    {
        for (auto& slot : slots)
        {
            if (slot.forwarded)
                postContact(slot.trackingId >= 0
                                ? slot.trackingId : slot.endingId,
                            slot, Phase::up);
            slot = {};
        }
        primaryTrackingId = noPrimaryContact;
        contactsInGesture = 0;
    }

    int openTouchscreen()
    {
        constexpr auto bitsPerWord = sizeof(unsigned long) * 8u;
        constexpr auto eventWords = (EV_MAX + bitsPerWord) / bitsPerWord;
        constexpr auto absoluteWords = (ABS_MAX + bitsPerWord) / bitsPerWord;

        for (int index = 0; index < 64; ++index)
        {
            const auto path = juce::String("/dev/input/event")
                + juce::String(index);
            const auto descriptor = ::open(path.toRawUTF8(),
                                           O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (descriptor < 0)
                continue;

            std::array<unsigned long, eventWords> eventBits {};
            std::array<unsigned long, absoluteWords> absoluteBits {};
            const auto hasEventBits = ::ioctl(descriptor,
                EVIOCGBIT(0, sizeof(eventBits)), eventBits.data()) >= 0;
            const auto hasAbsoluteBits = hasEventBits
                && bitIsSet(eventBits, EV_ABS)
                && ::ioctl(descriptor, EVIOCGBIT(EV_ABS,
                    sizeof(absoluteBits)), absoluteBits.data()) >= 0;
            const auto isTypeBTouchscreen = hasAbsoluteBits
                && bitIsSet(absoluteBits, ABS_MT_SLOT)
                && bitIsSet(absoluteBits, ABS_MT_TRACKING_ID)
                && bitIsSet(absoluteBits, ABS_MT_POSITION_X)
                && bitIsSet(absoluteBits, ABS_MT_POSITION_Y);

            input_absinfo candidateX {};
            input_absinfo candidateY {};
            const auto hasRanges = isTypeBTouchscreen
                && ::ioctl(descriptor, EVIOCGABS(ABS_MT_POSITION_X),
                           &candidateX) >= 0
                && ::ioctl(descriptor, EVIOCGABS(ABS_MT_POSITION_Y),
                           &candidateY) >= 0
                && candidateX.maximum > candidateX.minimum
                && candidateY.maximum > candidateY.minimum;
            if (! hasRanges)
            {
                ::close(descriptor);
                continue;
            }

            xRange = candidateX;
            yRange = candidateY;
            char name[128] {};
            if (::ioctl(descriptor, EVIOCGNAME(sizeof(name)), name) < 0)
                std::strncpy(name, "touchscreen", sizeof(name) - 1u);
            std::fprintf(stderr,
                "Commento touch: multitouch attivo su %s (%s)\n",
                path.toRawUTF8(), name);
            return descriptor;
        }

        return -1;
    }

    void finishFrame()
    {
        // The contact that begins an otherwise empty gesture is the pointer
        // already delivered by Xorg/JUCE.  Keep it as the primary until every
        // finger has left the glass; promoting another contact midway could
        // duplicate an event if Xorg changes its emulated pointer owner.
        for (auto& slot : slots)
        {
            if (! slot.began)
                continue;

            ++contactsInGesture;
            if (primaryTrackingId == noPrimaryContact
                && contactsInGesture == 1)
            {
                primaryTrackingId = slot.trackingId;
            }
            else if (slot.trackingId != primaryTrackingId)
            {
                slot.forwarded = true;
                postContact(slot.trackingId, slot, Phase::down);
            }
            slot.began = false;
        }

        for (auto& slot : slots)
        {
            if (! slot.ended)
                continue;

            if (slot.forwarded)
                postContact(slot.endingId, slot, Phase::up);
            slot.forwarded = false;
            slot.ended = false;
            slot.endingId = -1;
            contactsInGesture = juce::jmax(0, contactsInGesture - 1);
        }

        if (contactsInGesture == 0)
            primaryTrackingId = noPrimaryContact;
    }

    bool readEvents(int descriptor)
    {
        std::array<input_event, 32> events {};
        const auto bytes = ::read(descriptor, events.data(),
                                  sizeof(events));
        if (bytes < 0)
            return errno == EAGAIN || errno == EWOULDBLOCK;
        if (bytes == 0)
            return false;

        const auto count = static_cast<std::size_t>(bytes)
            / sizeof(input_event);
        for (std::size_t index = 0; index < count; ++index)
        {
            const auto& event = events[index];
            if (event.type == EV_SYN && event.code == SYN_DROPPED)
            {
                // Losing even one tracking-id edge could leave a momentary pad
                // held forever.  Release every forwarded contact and ignore
                // the incomplete frame; normal tracking resumes with the next
                // fresh contact after the synchronisation report.
                releaseForwardedContacts();
                discardingDroppedFrame = true;
                continue;
            }
            if (discardingDroppedFrame)
            {
                if (event.type == EV_SYN && event.code == SYN_REPORT)
                    discardingDroppedFrame = false;
                continue;
            }
            if (event.type == EV_ABS)
            {
                if (event.code == ABS_MT_SLOT)
                {
                    currentSlot = juce::jlimit(
                        0, maximumSlots - 1, event.value);
                    continue;
                }

                auto& slot = slots[static_cast<std::size_t>(currentSlot)];
                switch (event.code)
                {
                    case ABS_MT_TRACKING_ID:
                        if (event.value >= 0)
                        {
                            slot.trackingId = event.value;
                            slot.endingId = -1;
                            slot.began = true;
                            slot.ended = false;
                        }
                        else if (slot.trackingId >= 0)
                        {
                            slot.endingId = slot.trackingId;
                            slot.trackingId = -1;
                            slot.ended = true;
                            slot.began = false;
                        }
                        break;
                    case ABS_MT_POSITION_X: slot.x = event.value; break;
                    case ABS_MT_POSITION_Y: slot.y = event.value; break;
                    default: break;
                }
            }
            else if (event.type == EV_SYN && event.code == SYN_REPORT)
            {
                finishFrame();
            }
        }
        return true;
    }

    void run()
    {
        auto descriptor = -1;
        auto unavailableWasLogged = false;
        while (! shouldStop.load(std::memory_order_relaxed))
        {
            if (descriptor < 0)
            {
                descriptor = openTouchscreen();
                if (descriptor < 0)
                {
                    if (! unavailableWasLogged)
                    {
                        std::fputs(
                            "Commento touch: multitouch evdev non disponibile; "
                            "verifica touchscreen e gruppo input\n", stderr);
                        unavailableWasLogged = true;
                    }
                    for (int tenth = 0; tenth < 20
                         && ! shouldStop.load(std::memory_order_relaxed);
                         ++tenth)
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(100));
                    continue;
                }
                unavailableWasLogged = false;
            }

            pollfd pollDescriptor { descriptor, POLLIN, 0 };
            const auto result = ::poll(&pollDescriptor, 1, 100);
            if (result > 0 && (pollDescriptor.revents & POLLIN) != 0)
            {
                if (! readEvents(descriptor))
                {
                    releaseForwardedContacts();
                    ::close(descriptor);
                    descriptor = -1;
                }
            }
            else if (result < 0 && errno != EINTR)
            {
                releaseForwardedContacts();
                ::close(descriptor);
                descriptor = -1;
            }
            else if (result > 0
                     && (pollDescriptor.revents
                         & (POLLERR | POLLHUP | POLLNVAL)) != 0)
            {
                releaseForwardedContacts();
                ::close(descriptor);
                descriptor = -1;
            }
        }

        releaseForwardedContacts();
        if (descriptor >= 0)
            ::close(descriptor);
    }

    static constexpr int noPrimaryContact = -1;
    std::thread worker;
    std::atomic<bool> shouldStop { false };
    std::array<Slot, maximumSlots> slots {};
    input_absinfo xRange {};
    input_absinfo yRange {};
    int currentSlot = 0;
    int primaryTrackingId = noPrimaryContact;
    int contactsInGesture = 0;
    bool discardingDroppedFrame = false;
   #endif
};

LinuxMultitouchBridge::LinuxMultitouchBridge(Callback callback)
    : implementation(std::make_unique<Implementation>(std::move(callback)))
{
}

LinuxMultitouchBridge::~LinuxMultitouchBridge() = default;
