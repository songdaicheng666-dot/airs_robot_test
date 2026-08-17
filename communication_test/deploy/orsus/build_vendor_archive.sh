#!/usr/bin/env bash
set -euo pipefail

output="${1:-/tmp/orsus-python-vendor.tar.gz}"
package_root=/usr/lib/python3/dist-packages
packages=(requests urllib3 certifi chardet idna yaml _yaml)

for package in "${packages[@]}"; do
    if [[ ! -d "$package_root/$package" ]]; then
        printf 'Required Ubuntu Python package is missing: %s/%s\n' "$package_root" "$package" >&2
        exit 1
    fi
done

PYTHONPATH="$package_root" python3 -c \
    'import certifi, chardet, idna, requests, urllib3, yaml; print("requests", requests.__version__, "urllib3", urllib3.__version__, "yaml", yaml.__version__)'
tar -czf "$output" --exclude='*/__pycache__' -C "$package_root" "${packages[@]}"
chmod 600 "$output"
printf 'Wrote isolated Orsus Python dependencies to %s\n' "$output"
