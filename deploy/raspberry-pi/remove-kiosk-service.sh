#!/usr/bin/env bash
set -euo pipefail

if [[ ${EUID} -ne 0 ]]; then
    echo "Esegui questo script con sudo." >&2
    exit 1
fi

state_directory=/var/lib/commento-kiosk
systemctl disable --now commento-kiosk.service 2>/dev/null || true
systemctl unmask getty@tty1.service
rm -f /etc/systemd/system/commento-kiosk.service

if [[ -f ${state_directory}/previous-target ]]; then
    previous_target=$(tr -d '\n' < "${state_directory}/previous-target")
    if [[ ${previous_target} =~ ^[a-zA-Z0-9_.@-]+\.target$ ]]; then
        systemctl set-default "${previous_target}"
    fi
fi

systemctl daemon-reload
echo "Kiosk Commento rimosso. Il login testuale tornera' al prossimo riavvio."

