#!/usr/bin/env bash
set -euo pipefail

if [[ ${EUID} -ne 0 ]]; then
    echo "Esegui questo script con sudo." >&2
    exit 1
fi

commento_user=${SUDO_USER:-}
if [[ -z ${commento_user} || ${commento_user} == root ]]; then
    echo "Esegui con: sudo ./deploy/raspberry-pi/install-kiosk-service.sh" >&2
    exit 1
fi
if [[ ! ${commento_user} =~ ^[a-z_][a-z0-9_-]*$ ]]; then
    echo "Nome utente non valido." >&2
    exit 1
fi

commento_group=$(id -gn "${commento_user}")
commento_home=$(getent passwd "${commento_user}" | cut -d: -f6)
project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
commento_binary=${COMMENTO_BINARY:-${project_root}/build-pi/Commento_artefacts/Release/Commento}
state_directory=/var/lib/commento-kiosk

if [[ ! -x ${commento_binary} ]]; then
    echo "Binario Release non trovato: ${commento_binary}" >&2
    echo "Compila prima Commento nella cartella build-pi." >&2
    exit 1
fi

apt-get update
apt-get install -y xserver-xorg-core xserver-xorg-input-libinput \
    xinit x11-xserver-utils unclutter-xfixes

install -d -m 0755 /opt/commento/bin
install -m 0755 "${commento_binary}" /opt/commento/bin/Commento
install -m 0755 "${project_root}/deploy/raspberry-pi/start-commento-kiosk.sh" \
    /opt/commento/bin/start-commento-kiosk
install -d -m 0700 "${state_directory}"

for hardware_group in audio video input render; do
    if getent group "${hardware_group}" >/dev/null; then
        usermod -aG "${hardware_group}" "${commento_user}"
    fi
done

if [[ ! -f ${state_directory}/previous-target ]]; then
    systemctl get-default > "${state_directory}/previous-target"
fi

unit_file=$(mktemp)
trap 'rm -f "${unit_file}"' EXIT
sed -e "s|@USER@|${commento_user}|g" \
    -e "s|@GROUP@|${commento_group}|g" \
    -e "s|@HOME@|${commento_home}|g" \
    "${project_root}/deploy/raspberry-pi/commento-kiosk.service.in" \
    > "${unit_file}"
install -m 0644 "${unit_file}" /etc/systemd/system/commento-kiosk.service

systemctl set-default multi-user.target
systemctl mask --now getty@tty1.service
systemctl daemon-reload
systemctl enable commento-kiosk.service

echo
echo "Kiosk installato. Commento partira' automaticamente al prossimo riavvio."
echo "Avvio immediato da SSH: sudo systemctl start commento-kiosk.service"
echo "Log: journalctl -u commento-kiosk.service -f"
echo "Rimozione: sudo ./deploy/raspberry-pi/remove-kiosk-service.sh"
