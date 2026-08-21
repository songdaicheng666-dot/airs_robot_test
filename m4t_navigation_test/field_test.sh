#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd -- "${script_dir}/.." && pwd)"
private_env="${project_root}/communication_test/.private/m4t-relay.env"

if [[ ! -r "${private_env}" ]]; then
  echo "missing private operator configuration: ${private_env}" >&2
  exit 2
fi

cd "${project_root}"
set -a
# shellcheck disable=SC1090
source "${private_env}"
set +a

python3 -m m4t_navigation_test.client --allow-insecure-http startup
exec python3 -m m4t_navigation_test.client --allow-insecure-http run
