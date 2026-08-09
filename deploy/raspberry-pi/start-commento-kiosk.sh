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

exec /opt/commento/bin/Commento "$@"

