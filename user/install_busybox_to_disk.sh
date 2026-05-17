#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
INSTALL_ONE="${REPO_ROOT}/user/install_user_to_disk.sh"

DISK_IMG="${REPO_ROOT}/disk.img"
ROOTFS_IMG=""
BUSYBOX_ELF=""
DEBUGFS_BIN=""
EXT4_LBA_BASE=2048

usage() {
  cat <<'EOF'
Usage:
  bash user/install_busybox_to_disk.sh --input <busybox-elf> [--disk <path> | --rootfs <path>]

Installs:
  /bin/busybox
  /bin/sh -> /bin/busybox

Notes:
  - The BusyBox binary must already be a static ELF for the LAMP user ABI.
  - Requires debugfs from e2fsprogs. Homebrew paths are auto-detected on macOS.
EOF
}

find_debugfs() {
  if command -v debugfs >/dev/null 2>&1; then
    DEBUGFS_BIN="$(command -v debugfs)"
    return
  fi
  for candidate in \
    /opt/homebrew/opt/e2fsprogs/sbin/debugfs \
    /usr/local/opt/e2fsprogs/sbin/debugfs \
    /opt/homebrew/sbin/debugfs \
    /usr/local/sbin/debugfs; do
    if [[ -x "${candidate}" ]]; then
      DEBUGFS_BIN="${candidate}"
      return
    fi
  done
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --disk)
      DISK_IMG="$2"
      shift 2
      ;;
    --rootfs)
      ROOTFS_IMG="$2"
      shift 2
      ;;
    --input)
      BUSYBOX_ELF="$2"
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

if [[ -z "${BUSYBOX_ELF}" ]]; then
  echo "error: --input is required" >&2
  usage
  exit 1
fi
if [[ ! -f "${BUSYBOX_ELF}" ]]; then
  echo "error: BusyBox ELF not found: ${BUSYBOX_ELF}" >&2
  exit 1
fi

find_debugfs
if [[ -z "${DEBUGFS_BIN}" ]]; then
  echo "error: debugfs not found. Please install e2fsprogs." >&2
  exit 1
fi

INSTALL_ARGS=()
if [[ -n "${ROOTFS_IMG}" ]]; then
  INSTALL_ARGS+=(--rootfs "${ROOTFS_IMG}")
elif [[ -n "${DISK_IMG}" ]]; then
  INSTALL_ARGS+=(--disk "${DISK_IMG}")
fi
bash "${INSTALL_ONE}" "${INSTALL_ARGS[@]}" --input "${BUSYBOX_ELF}" --dest /bin/busybox

TMP_DIR=""
FS_IMG=""
cleanup() {
  if [[ -n "${TMP_DIR}" && -d "${TMP_DIR}" ]]; then
    rm -rf "${TMP_DIR}"
  fi
}
trap cleanup EXIT

if [[ -n "${ROOTFS_IMG}" ]]; then
  FS_IMG="${ROOTFS_IMG}"
else
  if [[ ! -f "${DISK_IMG}" ]]; then
    echo "error: disk image not found: ${DISK_IMG}" >&2
    exit 1
  fi
  TMP_DIR="$(mktemp -d)"
  FS_IMG="${TMP_DIR}/rootfs.ext4"
  dd if="${DISK_IMG}" of="${FS_IMG}" bs=512 skip="${EXT4_LBA_BASE}" status=none
fi

"${DEBUGFS_BIN}" -w -R "rm /bin/sh" "${FS_IMG}" >/dev/null 2>&1 || true
"${DEBUGFS_BIN}" -w -R "symlink /bin/sh /bin/busybox" "${FS_IMG}" >/dev/null

if [[ -z "${ROOTFS_IMG}" ]]; then
  dd if="${FS_IMG}" of="${DISK_IMG}" bs=512 seek="${EXT4_LBA_BASE}" conv=notrunc status=none
fi

echo "installed BusyBox shell: /bin/busybox and /bin/sh -> /bin/busybox"
"${DEBUGFS_BIN}" -R "ls -l /bin" "${FS_IMG}"
