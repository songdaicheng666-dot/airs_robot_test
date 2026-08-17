#!/usr/bin/env bash
set -euo pipefail

archive="${1:-/tmp/m4t-relay-source.tar.gz}"
registry_source="${2:-}"
source_root=/opt/m4t-relay-source
app_root=/opt/m4t-relay
state_root=/var/lib/m4t-relay
requirements_file="$source_root/requirements.txt"
env_file=/etc/m4t-relay.env
registry_file=/etc/m4t-relay-devices.json

if [[ ! -f "$archive" ]]; then
    printf 'Deployment archive not found: %s\n' "$archive" >&2
    exit 1
fi

if ! id -u m4trelay >/dev/null 2>&1; then
    useradd --system --home "$app_root" --shell /usr/sbin/nologin m4trelay
fi

install -d -m 755 -o root -g root "$source_root" "$app_root" "$app_root/communication_test"
install -d -m 750 -o m4trelay -g m4trelay "$state_root"
tar -xzf "$archive" -C "$source_root"
if [[ ! -f "$requirements_file" ]]; then
    printf 'Requirements file not found in deployment archive: %s\n' "$requirements_file" >&2
    exit 1
fi
cp -a "$source_root/communication_test/." "$app_root/communication_test/"

if [[ ! -x "$app_root/venv/bin/python" ]]; then
    python3 -m venv "$app_root/venv"
fi
"$app_root/venv/bin/pip" install --disable-pip-version-check \
    -r "$requirements_file"

if [[ ! -f "$env_file" ]]; then
    umask 077
    operator_token="$(openssl rand -hex 32)"
    device_token="$(openssl rand -hex 32)"
    {
        printf 'M4T_OPERATOR_TOKEN=%s\n' "$operator_token"
        printf 'M4T_DEVICE_TOKEN=%s\n' "$device_token"
        printf 'M4T_DEVICE_ID=M4T-001\n'
        printf 'M4T_DATABASE_PATH=/var/lib/m4t-relay/relay.db\n'
        printf 'M4T_ONLINE_THRESHOLD_SECONDS=15\n'
        printf 'M4T_COMMAND_LEASE_SECONDS=35\n'
        printf 'M4T_COMMAND_TTL_SECONDS=300\n'
        printf 'M4T_MAX_POLL_SECONDS=30\n'
    } >"$env_file"
    unset operator_token device_token
fi
chmod 600 "$env_file"

if [[ -n "$registry_source" ]]; then
    if [[ ! -f "$registry_source" ]]; then
        printf 'Device registry not found: %s\n' "$registry_source" >&2
        exit 1
    fi
    install -m 640 -o root -g m4trelay "$registry_source" "$registry_file"
fi
if [[ -f "$registry_file" ]] && ! grep -q '^RELAY_DEVICE_CONFIG_PATH=' "$env_file"; then
    printf 'RELAY_DEVICE_CONFIG_PATH=%s\n' "$registry_file" >>"$env_file"
fi

install -m 644 "$app_root/communication_test/deploy/cloud/m4t-relay.service" \
    /etc/systemd/system/m4t-relay.service
install -m 644 "$app_root/communication_test/deploy/cloud/nginx-m4t-relay.conf" \
    /etc/nginx/sites-available/m4t-relay
ln -sfn /etc/nginx/sites-available/m4t-relay /etc/nginx/sites-enabled/m4t-relay
if [[ -e /etc/nginx/sites-enabled/default || -L /etc/nginx/sites-enabled/default ]]; then
    unlink /etc/nginx/sites-enabled/default
fi

nginx -t
systemctl daemon-reload
systemctl enable --now m4t-relay nginx
systemctl restart m4t-relay nginx

printf 'M4T relay deployed. Tokens remain in %s.\n' "$env_file"
