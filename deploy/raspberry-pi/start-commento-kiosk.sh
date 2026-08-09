#!/usr/bin/env bash
set -u

display_mode=${COMMENTO_DISPLAY_MODE:-1920x1200}
display_output=

for _ in $(seq 1 20); do
    display_output=$(xrandr --query 2>/dev/null \
        | awk '$2 == "connected" { print $1; exit }')
    if [[ -n ${display_output} ]]; then
        break
    fi
    sleep 0.1
done

if [[ -n ${display_output} ]]; then
    if xrandr --output "${display_output}" --mode "${display_mode}"; then
        echo "Commento: display ${display_output} impostato a ${display_mode}."
    else
        echo "Commento: mode ${display_mode} non disponibile; uso quello corrente." >&2
        xrandr --query >&2 || true
    fi
else
    echo "Commento: nessun display X11 rilevato." >&2
fi

xset s off
xset -dpms
xset s noblank

if command -v unclutter >/dev/null 2>&1; then
    unclutter --timeout 0 --jitter 0 --hide-on-touch --start-hidden &
fi

# Let devices already connected at boot finish their udev enumeration before
# JUCE performs its one-shot ALSA scan. No mixer name is hardcoded: the touch UI
# can configure any interface exposed by ALSA. An installation that must wait
# for one particular interface may set COMMENTO_AUDIO_CARD_PATTERN to an ERE,
# for example 'MODEL ?12|TASCAM'.
if command -v udevadm >/dev/null 2>&1; then
    udevadm settle --timeout=10 || true
fi

audio_card_pattern=${COMMENTO_AUDIO_CARD_PATTERN:-}
if [[ -n ${audio_card_pattern} ]]; then
    audio_card_ready=false
    for _ in $(seq 1 50); do
        if grep -Eqi -- "${audio_card_pattern}" /proc/asound/cards 2>/dev/null; then
            audio_card_ready=true
            break
        fi
        sleep 0.2
    done

    if [[ ${audio_card_ready} != true ]]; then
        echo "Commento: nessuna scheda corrisponde a COMMENTO_AUDIO_CARD_PATTERN; riprovo tramite systemd." >&2
        if [[ -r /proc/asound/cards ]]; then
            cat /proc/asound/cards >&2
        fi
        exit 75
    fi
fi

exec /opt/commento/bin/Commento "$@"
