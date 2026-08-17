#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export CMAKE_PREFIX_PATH="${HOME}/.local${CMAKE_PREFIX_PATH:+:${CMAKE_PREFIX_PATH}}"
echo "Using CMAKE_PREFIX_PATH: ${CMAKE_PREFIX_PATH}"

cd "${SCRIPT_DIR}"
dpkg-buildpackage -b -uc -us "$@"
