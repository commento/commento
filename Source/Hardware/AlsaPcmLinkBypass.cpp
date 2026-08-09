#if defined(__linux__)

#include <cerrno>
#include <unistd.h>

// JUCE 8.0.13 links the playback and capture handles, prepares them, and then
// performs a blocking capture read before it has written any playback data.
// A read may start the entire linked group, including the still-empty playback
// stream.  On USB full-duplex devices this can produce a repeated recover/start
// cycle which JUCE does not include in getXRunCount() when the error is first
// reported by snd_pcm_avail_update().
//
// ALSA's own latency example primes playback before starting linked streams.
// Until JUCE does the same, leave the two handles unlinked.  They still use the
// same physical USB device, sample rate, period size, and hardware clock.  The
// linker --wrap option makes this workaround local to Commento and avoids
// editing the fetched JUCE checkout.

struct _snd_pcm;

extern "C" int __wrap_snd_pcm_link(_snd_pcm*, _snd_pcm*)
{
    static constexpr char message[] =
        "Commento ALSA: snd_pcm_link bypass attivo; capture e playback "
        "partono indipendenti\n";
    (void) ::write(STDERR_FILENO, message, sizeof(message) - 1);
    return -ENOSYS;
}

#endif
