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
unit_path=/etc/systemd/system/commento-kiosk.service
updating_existing_install=false

if [[ -f ${unit_path} || -L ${unit_path} ]]; then
    updating_existing_install=true
fi

if [[ ! -x ${commento_binary} ]]; then
    echo "Binario Release non trovato: ${commento_binary}" >&2
    echo "Compila prima Commento nella cartella build-pi." >&2
    exit 1
fi

apt-get update
apt-get install -y xserver-xorg-core xserver-xorg-input-libinput \
    xinit x11-xserver-utils unclutter-xfixes gldriver-test

install -d -m 0755 /opt/commento/bin
install -d -m 0700 "${state_directory}"

# Stage files beside their destinations, then rename them atomically. This
# keeps a running kiosk on its old executable until systemd restarts it.
binary_stage=$(mktemp /opt/commento/bin/.Commento.XXXXXX)
launcher_stage=$(mktemp /opt/commento/bin/.start-commento-kiosk.XXXXXX)
unit_file=$(mktemp)
trap 'rm -f "${binary_stage}" "${launcher_stage}" "${unit_file}"' EXIT

install -m 0755 "${commento_binary}" "${binary_stage}"
install -m 0755 "${project_root}/deploy/raspberry-pi/start-commento-kiosk.sh" \
    "${launcher_stage}"
mv -f "${binary_stage}" /opt/commento/bin/Commento
mv -f "${launcher_stage}" /opt/commento/bin/start-commento-kiosk

for hardware_group in audio video input render; do
    if getent group "${hardware_group}" >/dev/null; then
        usermod -aG "${hardware_group}" "${commento_user}"
    fi
done

if [[ ! -f ${state_directory}/previous-target ]]; then
    systemctl get-default > "${state_directory}/previous-target"
fi

sed -e "s|@USER@|${commento_user}|g" \
    -e "s|@GROUP@|${commento_group}|g" \
    -e "s|@HOME@|${commento_home}|g" \
    "${project_root}/deploy/raspberry-pi/commento-kiosk.service.in" \
    > "${unit_file}"
install -m 0644 "${unit_file}" "${unit_path}"

systemctl set-default multi-user.target
systemctl mask getty@tty1.service
systemctl daemon-reload
systemctl enable commento-kiosk.service

if [[ ${updating_existing_install} == true ]]; then
    systemctl restart commento-kiosk.service
fi

echo
if [[ ${updating_existing_install} == true ]]; then
    echo "Kiosk aggiornato e servizio riavviato."
else
    echo "Kiosk installato. Commento partira' automaticamente al prossimo riavvio."
fi
echo "Avvio immediato da SSH: sudo systemctl start commento-kiosk.service"
echo "Log: journalctl -u commento-kiosk.service -f"
echo "Rimozione: sudo ./deploy/raspberry-pi/remove-kiosk-service.sh"
