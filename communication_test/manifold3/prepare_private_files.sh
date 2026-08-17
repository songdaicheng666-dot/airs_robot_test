#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/../.." && pwd)"
app_dir="$repo_root/Payload-SDK-master/samples/sample_c/platform/linux/manifold3"

app_info="$app_dir/application/dji_sdk_app_info_private.h"
relay_config="$app_dir/app_json/m4t_relay_config.json"

if [[ ! -e "$app_info" ]]; then
    install -m 600 "$app_dir/application/dji_sdk_app_info_private.example.h" "$app_info"
    printf 'Created %s\n' "$app_info"
fi
if [[ ! -e "$relay_config" ]]; then
    install -m 600 "$app_dir/app_json/m4t_relay_config.example.json" "$relay_config"
    printf 'Created %s\n' "$relay_config"
fi

printf 'Fill both private files before building. They are ignored by the repository.\n'
