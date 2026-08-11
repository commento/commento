#if defined(__linux__)

#include <algorithm>
#include <cerrno>
#include <limits>
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
struct _snd_pcm_sw_params;

using CommentoAlsaFrames = unsigned long;

extern "C" int snd_pcm_stream(_snd_pcm*);
extern "C" int snd_pcm_get_params(_snd_pcm*, CommentoAlsaFrames*,
                                    CommentoAlsaFrames*);
extern "C" int __real_snd_pcm_sw_params_set_start_threshold(
    _snd_pcm*, _snd_pcm_sw_params*, CommentoAlsaFrames);

extern "C" int __wrap_snd_pcm_link(_snd_pcm*, _snd_pcm*)
{
    static constexpr char message[] =
        "Commento ALSA: snd_pcm_link bypass attivo; capture e playback "
        "partono indipendenti\n";
    (void) ::write(STDERR_FILENO, message, sizeof(message) - 1);
    return -ENOSYS;
}

extern "C" int __wrap_snd_pcm_sw_params_set_start_threshold(
    _snd_pcm* pcm, _snd_pcm_sw_params* parameters,
    CommentoAlsaFrames requestedThreshold)
{
    // With the link bypass active JUCE starts capture first, renders one
    // callback, and only then writes the first playback period. JUCE normally
    // starts playback after that single write. The output therefore keeps
    // almost no scheduling reserve: a later callback that is merely slower
    // than the first one can reach an empty ring even while its DSP load is
    // comfortably below one full period.
    //
    // ALSA stream 0 is playback. Delay its automatic start until two real
    // periods are queued. This adds one period of fixed output latency but
    // leaves a complete period of jitter headroom during full-duplex use.
    // Capture keeps JUCE's original threshold. Querying the actual hardware
    // geometry lets us clamp safely to the real ring size. Thresholds above
    // the ring are explicit-start semantics and are deliberately preserved.
    constexpr auto playbackStream = 0;
    CommentoAlsaFrames bufferSize = 0;
    CommentoAlsaFrames periodSize = 0;
    auto appliedThreshold = requestedThreshold;

    if (pcm != nullptr && snd_pcm_stream(pcm) == playbackStream
        && snd_pcm_get_params(pcm, &bufferSize, &periodSize) >= 0
        && periodSize > 0 && requestedThreshold <= bufferSize
        && periodSize <= std::numeric_limits<CommentoAlsaFrames>::max() / 2
        && bufferSize >= periodSize * 2)
    {
        appliedThreshold = std::min(
            bufferSize, std::max(requestedThreshold, periodSize * 2));
        if (appliedThreshold != requestedThreshold)
        {
            static constexpr char message[] =
                "Commento ALSA: playback prefill attivo; avvio dopo 2 periodi\n";
            (void) ::write(STDERR_FILENO, message, sizeof(message) - 1);
        }
    }

    return __real_snd_pcm_sw_params_set_start_threshold(
        pcm, parameters, appliedThreshold);
}

#endif
