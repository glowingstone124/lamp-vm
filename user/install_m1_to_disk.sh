#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
INSTALL_ONE="${REPO_ROOT}/user/install_user_to_disk.sh"

usage() {
  cat <<'EOF'
Usage:
  bash user/install_m1_to_disk.sh [--disk <path> | --rootfs <path>]

Installs M1 userspace test binaries into ext4:
  /bin/hello
  /bin/echo
  /bin/vfork_exec

Notes:
  - Requires the ELF files built by `bash user/build.sh`.
  - Pass-through options are forwarded to install_user_to_disk.sh.
EOF
}

ARGS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --disk|--rootfs)
      if [[ $# -lt 2 ]]; then
        echo "error: missing value for $1" >&2
        exit 1
      fi
      ARGS+=("$1" "$2")
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "error: unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

install_one() {
  local input="$1"
  local dest="$2"
  if [[ ${#ARGS[@]} -gt 0 ]]; then
    bash "${INSTALL_ONE}" "${ARGS[@]}" --input "${input}" --dest "${dest}"
  else
    bash "${INSTALL_ONE}" --input "${input}" --dest "${dest}"
  fi
}

install_one "${REPO_ROOT}/build-user/hello.elf" /bin/hello
install_one "${REPO_ROOT}/build-user/echo.elf" /bin/echo
install_one "${REPO_ROOT}/build-user/vfork_exec.elf" /bin/vfork_exec

echo "installed M1 userspace set: /bin/hello /bin/echo /bin/vfork_exec"
