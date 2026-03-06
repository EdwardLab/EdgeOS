#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
USR_DIR="${ROOT_DIR}/userspace"
TINYX_DIR="${TINYX_DIR:-${USR_DIR}/third_party/tinyx}"
MUSL_PREFIX="${MUSL_PREFIX:-/opt/musl}"
CC_BIN="${MUSL_PREFIX}/bin/musl-gcc"
PREFIX="${TINYX_PREFIX:-${ROOT_DIR}/out/tinyx-install}"
TINYX_TARGET="${TINYX_TARGET:-xvfb}"   # xfbdev | xephyr | xvfb
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}"

if [[ ! -d "${TINYX_DIR}" ]]; then
  echo "[tinyx] missing source tree: ${TINYX_DIR}" >&2
  echo "[tinyx] place TinyX/Xorg kdrive source there (or export TINYX_DIR=...)" >&2
  exit 1
fi
if [[ ! -x "${CC_BIN}" ]] && [[ -x "/opt/musl/bin/musl-gcc" ]]; then
  CC_BIN="/opt/musl/bin/musl-gcc"
fi
if [[ ! -x "${CC_BIN}" ]]; then
  echo "[tinyx] missing musl-gcc; run tools/ports/build_musl.sh first" >&2
  exit 1
fi

echo "[tinyx] source=${TINYX_DIR}"
echo "[tinyx] prefix=${PREFIX}"
echo "[tinyx] target=${TINYX_TARGET}"
echo "[tinyx] mode=musl cc=${CC_BIN}"

mkdir -p "${PREFIX}"

pushd "${TINYX_DIR}" >/dev/null

export CC="${CC_BIN}"
export CXX="${CC_BIN}"
export LD="${CC_BIN}"
export AR="${AR:-ar}"
export RANLIB="${RANLIB:-ranlib}"
export STRIP="${STRIP:-strip}"
USER_CFLAGS="${CFLAGS:-}"
USER_CPPFLAGS="${CPPFLAGS:-}"
USER_LDFLAGS="${LDFLAGS:-}"
export CFLAGS="${USER_CFLAGS} -D_GNU_SOURCE"
export CPPFLAGS="${USER_CPPFLAGS}"
export LDFLAGS="${USER_LDFLAGS}"

# Meson-based modern xserver tree.
if [[ -f "./meson.build" ]]; then
  if [[ "${CFLAGS} ${CPPFLAGS} ${LDFLAGS}" == *"/usr/include"* ]]; then
    echo "[tinyx] refusing meson+musl build with host include paths in flags." >&2
    echo "[tinyx] unset CFLAGS/CPPFLAGS/LDFLAGS and use musl-built dependencies only." >&2
    exit 1
  fi
  if [[ -n "${PKG_CONFIG_LIBDIR:-}" || -n "${PKG_CONFIG_SYSROOT_DIR:-}" ]]; then
    echo "[tinyx] note: PKG_CONFIG_LIBDIR/PKG_CONFIG_SYSROOT_DIR are set; ensure they point to musl-built Xorg deps." >&2
  fi
  if ! command -v pkg-config >/dev/null 2>&1; then
    echo "[tinyx] pkg-config required for meson-based xserver tree" >&2
    exit 1
  fi
  if [[ "${TINYX_TARGET}" == "xfbdev" ]]; then
    if [[ ! -d "hw/kdrive/fbdev" ]]; then
      echo "[tinyx] this xserver tree does not include hw/kdrive/fbdev (Xfbdev)." >&2
      echo "[tinyx] It contains kdrive only for Xephyr in this version." >&2
      echo "[tinyx] Use an older TinyX/Xorg tree with Xfbdev sources, or set TINYX_TARGET=xvfb/xephyr." >&2
      exit 1
    fi
  fi
  if ! command -v meson >/dev/null 2>&1 || ! command -v ninja >/dev/null 2>&1; then
    echo "[tinyx] meson/ninja required for this source tree" >&2
    exit 1
  fi
  BUILD_DIR="${ROOT_DIR}/out/tinyx-build"
  rm -rf "${BUILD_DIR}"
  mkdir -p "${BUILD_DIR}"
  xephyr_opt=false
  xvfb_opt=false
  if [[ "${TINYX_TARGET}" == "xephyr" ]]; then xephyr_opt=true; fi
  if [[ "${TINYX_TARGET}" == "xvfb" ]]; then xvfb_opt=true; fi
  meson setup "${BUILD_DIR}" . \
    --prefix="${PREFIX}" \
    --default-library=static \
    -Dxorg=false \
    -Dxwayland=false \
    -Dxnest=false \
    -Dxvfb=${xvfb_opt} \
    -Dxwin=false \
    -Dxquartz=false \
    -Ddocs=false \
    -Ddevel-docs=false \
    -Dglamor=false \
    -Dlisten_unix=true \
    -Dlisten_tcp=false \
    -Dxephyr=${xephyr_opt} \
    || { echo "[tinyx] meson setup failed" >&2; exit 1; }
  ninja -C "${BUILD_DIR}" -j"${JOBS}"
  ninja -C "${BUILD_DIR}" install
  echo "[tinyx] meson build complete"
  if [[ "${TINYX_TARGET}" == "xephyr" ]]; then echo "[tinyx] expected binary: ${PREFIX}/bin/Xephyr"; fi
  if [[ "${TINYX_TARGET}" == "xvfb" ]]; then echo "[tinyx] expected binary: ${PREFIX}/bin/Xvfb"; fi
  popd >/dev/null
  exit 0
fi

# Autotools TinyX/Xorg tree (older trees with Xfbdev).
export PKG_CONFIG="${PKG_CONFIG:-false}"
if [[ -x "./configure" ]]; then
  :
elif [[ -x "./autogen.sh" ]]; then
  echo "[tinyx] running autogen.sh"
  ./autogen.sh
else
  echo "[tinyx] no meson.build, ./configure, or ./autogen.sh found" >&2
  echo "[tinyx] expected TinyX/Xorg source tree with kdrive/fbdev support" >&2
  exit 1
fi

echo "[tinyx] configuring for kdrive fbdev (autotools tree)"
./configure \
  --host=x86_64-linux-musl \
  --prefix="${PREFIX}" \
  --disable-shared \
  --enable-static \
  --enable-kdrive \
  --enable-xfbdev \
  --disable-xorg \
  --disable-xephyr \
  --disable-xnest \
  --disable-xvfb \
  --disable-xwin \
  --disable-xquartz \
  --disable-dmx \
  --disable-docs \
  --without-dtrace \
  --without-launchd \
  --without-systemd-logind \
  --without-hal \
  --without-udev \
  || {
    echo "[tinyx] configure failed. Check userspace/third_party/tinyx/PORTING_NOTES.md" >&2
    exit 1
  }

echo "[tinyx] building"
make -j"${JOBS}"
echo "[tinyx] installing to ${PREFIX}"
make install
echo "[tinyx] done. Expected server binary: ${PREFIX}/bin/Xfbdev"
popd >/dev/null
