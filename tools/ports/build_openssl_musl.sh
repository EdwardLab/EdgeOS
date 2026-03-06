#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
OPENSSL_DIR="${ROOT_DIR}/userspace/third_party/openssl"
OPENSSL_TARBALL="${ROOT_DIR}/tools/ports/distfiles/openssl-3.0.16.tar.gz"

MUSL_PREFIX="${MUSL_PREFIX:-/tmp/edgeos-musl}"
CC_BIN="${MUSL_PREFIX}/bin/musl-gcc"
if [ ! -x "${CC_BIN}" ] && [ -x "/opt/musl/bin/musl-gcc" ]; then
  CC_BIN="/opt/musl/bin/musl-gcc"
fi

if [ ! -x "${CC_BIN}" ]; then
  echo "missing musl-gcc; run tools/ports/build_musl.sh first" >&2
  exit 1
fi

if [ ! -d "${OPENSSL_DIR}" ]; then
  if [ -f "${OPENSSL_TARBALL}" ]; then
    mkdir -p "$(dirname "${OPENSSL_DIR}")"
    rm -rf "${OPENSSL_DIR}.tmp"
    mkdir -p "${OPENSSL_DIR}.tmp"
    tar -xzf "${OPENSSL_TARBALL}" -C "${OPENSSL_DIR}.tmp"
    srcdir="$(find "${OPENSSL_DIR}.tmp" -mindepth 1 -maxdepth 1 -type d | head -n1)"
    if [ -z "${srcdir}" ]; then
      echo "failed to unpack ${OPENSSL_TARBALL}" >&2
      exit 1
    fi
    mv "${srcdir}" "${OPENSSL_DIR}"
    rmdir "${OPENSSL_DIR}.tmp" 2>/dev/null || true
  else
    git clone --depth 1 --branch openssl-3.0.16 https://github.com/openssl/openssl.git "${OPENSSL_DIR}"
  fi
fi

cd "${OPENSSL_DIR}"

make distclean >/dev/null 2>&1 || true

export CC="${CC_BIN}"
if [ -x "${MUSL_PREFIX}/bin/ar" ]; then
  export AR="${MUSL_PREFIX}/bin/ar"
else
  export AR="$(command -v ar)"
fi
if [ -x "${MUSL_PREFIX}/bin/ranlib" ]; then
  export RANLIB="${MUSL_PREFIX}/bin/ranlib"
else
  export RANLIB="$(command -v ranlib)"
fi
if [ -x "${MUSL_PREFIX}/bin/strip" ]; then
  export STRIP="${MUSL_PREFIX}/bin/strip"
else
  export STRIP="$(command -v strip)"
fi
unset CROSS_COMPILE

perl ./Configure linux-x86_64 \
  no-shared no-tests no-module no-dso no-engine no-async no-threads no-ui-console \
  --prefix=/usr \
  --openssldir=/etc/ssl \
  -DOPENSSL_NO_SECURE_MEMORY

# OpenSSL git trees need generated headers (e.g. include/openssl/opensslv.h)
# before app compilation. Building apps/openssl directly may skip or race this.
if [ ! -f "include/openssl/opensslv.h" ]; then
  make -j1 build_generated || true
fi
if [ ! -f "include/openssl/opensslv.h" ]; then
  make -j1 generated_files || true
fi
if [ ! -f "include/openssl/opensslv.h" ]; then
  echo "OpenSSL generated headers missing (include/openssl/opensslv.h)" >&2
  echo "Try: make -j1 in userspace/third_party/openssl and inspect generated targets" >&2
  exit 1
fi

make -j"$(nproc)" apps/openssl

if [ ! -x "${OPENSSL_DIR}/apps/openssl" ]; then
  echo "openssl binary not produced" >&2
  exit 1
fi

readelf -l "${OPENSSL_DIR}/apps/openssl" | grep -q '/lib/ld-musl-x86_64.so.1' || {
  echo "openssl is not dynamically linked with musl loader" >&2
  exit 1
}

echo "built ${OPENSSL_DIR}/apps/openssl"
