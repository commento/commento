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

# JUCE's ALSA device list is scanned once. Wait for the dedicated mixer before
# starting the app so a late USB enumeration cannot leave MODEL 12 invisible
# until the next process restart.
model12_ready=false
for _ in $(seq 1 50); do
    if grep -Eqi 'MODEL ?12|TASCAM' /proc/asound/cards 2>/dev/null; then
        model12_ready=true
        break
    fi
    sleep 0.2
done

if [[ ${model12_ready} != true ]]; then
    echo "Commento: MODEL 12 non presente in /proc/asound/cards; riprovo tramite systemd." >&2
    if [[ -r /proc/asound/cards ]]; then
        cat /proc/asound/cards >&2
    fi
    exit 75
fi

exec /opt/commento/bin/Commento "$@"
