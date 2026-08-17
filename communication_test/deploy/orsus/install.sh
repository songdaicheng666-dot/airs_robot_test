#!/usr/bin/env bash
set -euo pipefail

agent_source="${1:-/tmp/agent.py}"
env_source="${2:-/tmp/orsus-ecs-agent.env}"
service_source="${3:-/tmp/orsus-ecs-agent.service}"
vendor_source="${4:-}"
app_root=/opt/orsus-ecs-agent
env_file=/etc/orsus-ecs-agent.env
service_file=/etc/systemd/system/orsus-ecs-agent.service

for source_file in "$agent_source" "$env_source" "$service_source"; do
    if [[ ! -f "$source_file" ]]; then
        printf 'Required deployment file not found: %s\n' "$source_file" >&2
        exit 1
    fi
done

if ! id -u gs >/dev/null 2>&1; then
    printf 'Required service account does not exist: gs\n' >&2
    exit 1
fi

install -d -m 755 -o root -g root "$app_root"
install -m 755 -o root -g root "$agent_source" "$app_root/agent.py"
install -m 600 -o root -g root "$env_source" "$env_file"
install -m 644 -o root -g root "$service_source" "$service_file"

if [[ -n "$vendor_source" ]]; then
    if [[ ! -f "$vendor_source" ]]; then
        printf 'Python vendor archive not found: %s\n' "$vendor_source" >&2
        exit 1
    fi
    install -d -m 755 -o root -g root "$app_root/vendor"
    tar -xzf "$vendor_source" -C "$app_root/vendor"
    chown -R root:root "$app_root/vendor"
elif ! /usr/bin/python3 -c 'import requests' >/dev/null 2>&1; then
    apt-get update
    apt-get install -y python3-requests
fi
/usr/bin/python3 -c 'import sys; sys.path.insert(0, "/opt/orsus-ecs-agent/vendor"); import requests; print("requests", requests.__version__)'

systemctl daemon-reload
systemctl enable --now orsus-ecs-agent.service
systemctl restart orsus-ecs-agent.service
systemctl --no-pager --full status orsus-ecs-agent.service
