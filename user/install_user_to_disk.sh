#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

DISK_IMG="${REPO_ROOT}/disk.img"
USER_ELF="${REPO_ROOT}/build-user/hello.elf"
DEST_PATH="/bin/hello"
ROOTFS_IMG=""
EXT4_LBA_BASE=2048

usage() {
  cat <<'EOF'
Usage:
  bash user/install_user_to_disk.sh [options]

Options:
  --disk <path>      VM disk image (default: ./disk.img)
  --rootfs <path>    Raw ext4 image to modify directly (skip disk partition extraction/writeback)
  --input <path>     User ELF file to install (default: ./build-user/hello.elf)
  --dest <path>      Target path inside ext4 (default: /bin/hello)
  -h, --help         Show this help

Notes:
  - Default mode edits the ext4 partition in disk.img at LBA 2048.
  - Requires debugfs (e2fsprogs).
EOF
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
      USER_ELF="$2"
      shift 2
      ;;
    --dest)
      DEST_PATH="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if ! command -v debugfs >/dev/null 2>&1; then
  echo "error: debugfs not found. Please install e2fsprogs." >&2
  exit 1
fi
if [[ ! -f "${USER_ELF}" ]]; then
  echo "error: user ELF not found: ${USER_ELF}" >&2
  exit 1
fi
if [[ "${DEST_PATH}" != /* ]]; then
  echo "error: --dest must be an absolute path inside ext4 (e.g. /bin/hello)" >&2
  exit 1
fi

TMP_DIR=""
FS_IMG=""
cleanup() {
  if [[ -n "${TMP_DIR}" && -d "${TMP_DIR}" ]]; then
    rm -rf "${TMP_DIR}"
  fi
}
trap cleanup EXIT

if [[ -n "${ROOTFS_IMG}" ]]; then
  if [[ ! -f "${ROOTFS_IMG}" ]]; then
    echo "error: rootfs image not found: ${ROOTFS_IMG}" >&2
    exit 1
  fi
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

ensure_parent_dirs() {
  local full_path="$1"
  local parent="${full_path%/*}"
  local cur=""
  local rest
  local next

  if [[ -z "${parent}" || "${parent}" == "/" ]]; then
    return
  fi

  rest="${parent#/}"
  while [[ -n "${rest}" ]]; do
    next="${rest%%/*}"
    if [[ "${rest}" == *"/"* ]]; then
      rest="${rest#*/}"
    else
      rest=""
    fi
    cur="${cur}/${next}"
    if ! debugfs -R "stat ${cur}" "${FS_IMG}" >/dev/null 2>&1; then
      debugfs -w -R "mkdir ${cur}" "${FS_IMG}" >/dev/null
    fi
  done
}

ensure_parent_dirs "${DEST_PATH}"

if debugfs -R "stat ${DEST_PATH}" "${FS_IMG}" >/dev/null 2>&1; then
  debugfs -w -R "rm ${DEST_PATH}" "${FS_IMG}" >/dev/null
fi
debugfs -w -R "write ${USER_ELF} ${DEST_PATH}" "${FS_IMG}" >/dev/null

if [[ -z "${ROOTFS_IMG}" ]]; then
  dd if="${FS_IMG}" of="${DISK_IMG}" bs=512 seek="${EXT4_LBA_BASE}" conv=notrunc status=none
fi

echo "installed: ${USER_ELF} -> ${DEST_PATH}"
debugfs -R "ls -l ${DEST_PATH%/*}" "${FS_IMG}"
