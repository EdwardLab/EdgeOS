#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
PYTHON_DIR="${ROOT_DIR}/userspace/third_party/python3"
PYTHON_VER="${PYTHON_VER:-3.12.8}"
PYTHON_TARBALL="${ROOT_DIR}/tools/ports/distfiles/Python-${PYTHON_VER}.tgz"
PY_STAGE="${ROOT_DIR}/out/python3-stage"
PY_ROOTFS_OVERRIDE="${ROOT_DIR}/userspace/third_party/python3-rootfs-override"

MUSL_PREFIX="${MUSL_PREFIX:-/tmp/edgeos-musl}"
CC_BIN="${MUSL_PREFIX}/bin/musl-gcc"
TOOLWRAP_DIR="${ROOT_DIR}/out/python3-toolwrap"

if [ ! -x "${CC_BIN}" ]; then
  echo "missing ${CC_BIN}; run tools/ports/build_musl.sh first" >&2
  exit 1
fi
if [ ! -f "${MUSL_PREFIX}/lib/musl-gcc.specs" ]; then
  echo "missing ${MUSL_PREFIX}/lib/musl-gcc.specs; rerun tools/ports/build_musl.sh" >&2
  exit 1
fi

PY_MULTIARCH="${PY_MULTIARCH:-x86_64-linux-musl}"

if [ ! -d "${PYTHON_DIR}" ]; then
  if [ -f "${PYTHON_TARBALL}" ]; then
    mkdir -p "$(dirname "${PYTHON_DIR}")"
    rm -rf "${PYTHON_DIR}.tmp"
    mkdir -p "${PYTHON_DIR}.tmp"
    tar -xzf "${PYTHON_TARBALL}" -C "${PYTHON_DIR}.tmp"
    srcdir="$(find "${PYTHON_DIR}.tmp" -mindepth 1 -maxdepth 1 -type d | head -n1)"
    if [ -z "${srcdir}" ]; then
      echo "failed to unpack ${PYTHON_TARBALL}" >&2
      exit 1
    fi
    mv "${srcdir}" "${PYTHON_DIR}"
    rmdir "${PYTHON_DIR}.tmp" 2>/dev/null || true
  else
    git clone --depth 1 --branch "v${PYTHON_VER}" https://github.com/python/cpython.git "${PYTHON_DIR}"
  fi
fi

cd "${PYTHON_DIR}"

make distclean >/dev/null 2>&1 || true
rm -f Modules/Setup.local

mkdir -p "${TOOLWRAP_DIR}"
cat > "${TOOLWRAP_DIR}/x86_64-linux-musl-gcc" <<EOF
#!/bin/sh
if [ "x\$1" = "x--print-multiarch" ]; then
  echo "${PY_MULTIARCH}"
  exit 0
fi
# MUSL_PREFIX defaults to /tmp, which is commonly mounted noexec.
# Invoke the musl-gcc wrapper through sh so configure can use it reliably.
exec /bin/sh "${CC_BIN}" "\$@"
EOF
chmod +x "${TOOLWRAP_DIR}/x86_64-linux-musl-gcc"
export PATH="${TOOLWRAP_DIR}:$PATH"
export CC="${TOOLWRAP_DIR}/x86_64-linux-musl-gcc"
export CXX="${TOOLWRAP_DIR}/x86_64-linux-musl-gcc"
export AR="${AR:-$(command -v ar)}"
export RANLIB="${RANLIB:-$(command -v ranlib)}"
export READELF="${READELF:-$(command -v readelf)}"
export OPT="${OPT:--O2}"
export CFLAGS="${CFLAGS:- -O2 }"
export CPPFLAGS="${CPPFLAGS:-}"
# Use static link only for configure test executables so they can run on the host.
# The final Python binary should be dynamic musl (EdgeOS already installs musl loader/libc).
ORIG_LDFLAGS="${LDFLAGS:-}"
export LDFLAGS="${ORIG_LDFLAGS} -static"

# Minimize optional features/deps to reduce ABI surface and build failures.
cat > Modules/Setup.local <<'EOF'
*disabled*
_ssl
_hashlib
zlib
_tkinter
readline
_bz2
_lzma
_sqlite3
_gdbm
_dbm
nis
ossaudiodev
spwd
grp
resource
termios
fcntl
mmap
_ctypes
_curses
_curses_panel
_decimal
binascii
EOF

configure_args=(
  --prefix=/usr
  --disable-shared
  --without-ensurepip
  --disable-ipv6
  --disable-test-modules
  --without-readline
  --without-static-libpython
  ac_cv_file__dev_ptmx=yes
  ac_cv_file__dev_ptc=no
  # Some shipped musl runtimes in this project do not export these wrappers.
  # Force Python to avoid referencing them at link/runtime.
  ac_cv_func_preadv2=no
  ac_cv_func_pwritev2=no
)

if ! ./configure "${configure_args[@]}"; then
  build_triplet="$(./config.guess 2>/dev/null || echo x86_64-pc-linux-gnu)"
  host_triplet="x86_64-linux-musl"
  echo "retrying configure in explicit cross mode (--build=${build_triplet} --host=${host_triplet})" >&2
  make distclean >/dev/null 2>&1 || true
  rm -f Modules/Setup.local
  cat > Modules/Setup.local <<'EOF'
*disabled*
_ssl
_hashlib
zlib
_tkinter
readline
_bz2
_lzma
_sqlite3
_gdbm
_dbm
nis
ossaudiodev
spwd
grp
resource
termios
fcntl
mmap
_ctypes
_curses
_curses_panel
_decimal
binascii
EOF
  ./configure \
    "${configure_args[@]}" \
    --build="${build_triplet}" \
    --host="${host_triplet}" \
    --with-build-python="$(command -v python3 || command -v python)"
fi

# Build final runtime dynamically (avoid static TLS startup requirements in kernel ELF path).
export LDFLAGS="${ORIG_LDFLAGS}"
if [ -f Makefile ]; then
  # configure may have baked -static into Makefile linker vars from the environment.
  sed -i 's/[[:space:]]-static[[:space:]]/ /g; s/[[:space:]]-static$/ /g' Makefile
fi
make -j"$(nproc)" python

if [ ! -x "${PYTHON_DIR}/python" ]; then
  echo "python binary not produced" >&2
  exit 1
fi

readelf -l "${PYTHON_DIR}/python" | grep -q '/lib/ld-musl-x86_64.so.1' || {
  echo "python is not dynamically linked with musl loader" >&2
  exit 1
}

# Install a minimal runtime tree for rootfs override application.
rm -rf "${PY_STAGE}" "${PY_ROOTFS_OVERRIDE}"
mkdir -p "${PY_ROOTFS_OVERRIDE}/bin" "${PY_ROOTFS_OVERRIDE}/usr/lib"
cp -f "${PYTHON_DIR}/python" "${PY_ROOTFS_OVERRIDE}/bin/python3"

# Prefer "make install" staging, but fall back to copying Lib/ directly if install is incomplete.
make -j"$(nproc)" install DESTDIR="${PY_STAGE}" || true

if [ -d "${PY_STAGE}/usr/lib/python${PYTHON_VER%.*}" ]; then
  mkdir -p "${PY_ROOTFS_OVERRIDE}/usr"
  cp -a "${PY_STAGE}/usr/lib" "${PY_ROOTFS_OVERRIDE}/usr/"
else
  pyminor="python${PYTHON_VER%.*}"
  mkdir -p "${PY_ROOTFS_OVERRIDE}/usr/lib/${pyminor}"
  cp -a "${PYTHON_DIR}/Lib/." "${PY_ROOTFS_OVERRIDE}/usr/lib/${pyminor}/"
fi

find "${PY_ROOTFS_OVERRIDE}/usr/lib" -type d \( -name test -o -name tests -o -name idlelib -o -name tkinter \) -prune -exec rm -rf {} +
find "${PY_ROOTFS_OVERRIDE}/usr/lib" -type d -name "__pycache__" -prune -exec rm -rf {} +

if [ ! -d "${PY_ROOTFS_OVERRIDE}/usr/lib/python${PYTHON_VER%.*}/encodings" ]; then
  echo "python stdlib override missing encodings/ after staging" >&2
  exit 1
fi

echo "built ${PYTHON_DIR}/python"
echo "rootfs override prepared at ${PY_ROOTFS_OVERRIDE}"
