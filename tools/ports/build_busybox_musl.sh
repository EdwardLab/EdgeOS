#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BUSYBOX_DIR="${ROOT_DIR}/userspace/third_party/busybox"
CONFIG_FILE="${ROOT_DIR}/tools/ports/config/busybox-edgeos.config"

MUSL_PREFIX="${MUSL_PREFIX:-/tmp/edgeos-musl}"
CC_BIN="${MUSL_PREFIX}/bin/musl-gcc"

if [ ! -x "${CC_BIN}" ]; then
  echo "missing ${CC_BIN}; run tools/ports/build_musl.sh first" >&2
  exit 1
fi

if [ ! -d "${BUSYBOX_DIR}" ]; then
  git clone --depth 1 https://git.busybox.net/busybox "${BUSYBOX_DIR}"
fi

cd "${BUSYBOX_DIR}"

make distclean >/dev/null 2>&1 || true

# Copy your predefined config
cp "${CONFIG_FILE}" .config

# Update config if BusyBox version changed
yes "" | make oldconfig || true

# Build static musl version
make -j"$(nproc)" CC="${CC_BIN}"

echo "built ${BUSYBOX_DIR}/busybox"
