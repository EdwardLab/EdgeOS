#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
DROPBEAR_DIR="${ROOT_DIR}/userspace/third_party/dropbearssh"

MUSL_PREFIX="${MUSL_PREFIX:-/tmp/edgeos-musl}"
CC_BIN="${MUSL_PREFIX}/bin/musl-gcc"
if [ ! -x "${CC_BIN}" ] && [ -x "/opt/musl/bin/musl-gcc" ]; then
  CC_BIN="/opt/musl/bin/musl-gcc"
fi

if [ ! -x "${CC_BIN}" ]; then
  echo "missing musl-gcc; run tools/ports/build_musl.sh first" >&2
  exit 1
fi

if [ ! -d "${DROPBEAR_DIR}" ]; then
  git clone --depth 1 https://github.com/mkj/dropbear.git "${DROPBEAR_DIR}"
fi

cd "${DROPBEAR_DIR}"

make distclean >/dev/null 2>&1 || true

CC="${CC_BIN}" ./configure \
  --host=x86_64-linux-musl \
  --disable-zlib \
  --disable-syslog \
  --disable-lastlog \
  --disable-utmp \
  --disable-utmpx \
  --disable-wtmp \
  --disable-wtmpx \
  --disable-pam

make -j"$(nproc)" PROGRAMS="dropbear dropbearkey"

DROPBEAR_BIN=""
if [ -x "${DROPBEAR_DIR}/dropbear" ]; then
  DROPBEAR_BIN="${DROPBEAR_DIR}/dropbear"
elif [ -x "${DROPBEAR_DIR}/src/dropbear" ]; then
  DROPBEAR_BIN="${DROPBEAR_DIR}/src/dropbear"
fi

if [ -z "${DROPBEAR_BIN}" ]; then
  echo "dropbear binary not produced" >&2
  exit 1
fi

readelf -l "${DROPBEAR_BIN}" | grep -q '/lib/ld-musl-x86_64.so.1' || {
  echo "dropbear is not dynamically linked with musl loader" >&2
  exit 1
}
readelf -d "${DROPBEAR_BIN}" | grep -q 'Shared library: \[libc.so\]' || {
  echo "dropbear is not linked against musl libc.so" >&2
  exit 1
}

echo "built ${DROPBEAR_BIN}"
