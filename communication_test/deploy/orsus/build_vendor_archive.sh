#!/usr/bin/env bash
set -euo pipefail

output="${1:-/tmp/orsus-python-vendor.tar.gz}"
package_root=/usr/lib/python3/dist-packages
packages=(requests urllib3 certifi chardet idna yaml _yaml)
user_package_root="$(python3 -c 'import site; print(site.getusersitepackages())')"
staging="$(mktemp -d)"
trap 'rm -rf -- "$staging"' EXIT

for package in "${packages[@]}"; do
    if [[ ! -d "$package_root/$package" ]]; then
        printf 'Required Ubuntu Python package is missing: %s/%s\n' "$package_root" "$package" >&2
        exit 1
    fi
done

if [[ ! -d "$user_package_root/websocket" ]]; then
    printf 'Required Python package is missing: websocket-client in %s\n' "$user_package_root" >&2
    printf 'Install requirements.txt with /usr/bin/pip3 before building the vendor archive.\n' >&2
    exit 1
fi

for package in "${packages[@]}"; do
    cp -a "$package_root/$package" "$staging/"
done
cp -a "$user_package_root/websocket" "$staging/"
chmod -R a+rX "$staging"

PYTHONPATH="$staging" python3 -c \
    'import certifi, chardet, idna, requests, urllib3, websocket, yaml; print("requests", requests.__version__, "urllib3", urllib3.__version__, "websocket-client", websocket.__version__, "yaml", yaml.__version__)'
tar -czf "$output" --exclude='*/__pycache__' -C "$staging" .
chmod 600 "$output"
printf 'Wrote isolated Orsus Python dependencies to %s\n' "$output"
