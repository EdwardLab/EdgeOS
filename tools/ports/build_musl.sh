#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
MUSL_DIR="${ROOT_DIR}/userspace/third_party/musl"
MUSL_PREFIX="${MUSL_PREFIX:-/tmp/edgeos-musl}"

if [ ! -d "${MUSL_DIR}" ]; then
  git clone --depth 1 https://git.musl-libc.org/git/musl "${MUSL_DIR}"
fi

# Keep musl on the native x86_64 syscall ABI (`syscall` instruction with rcx/r11 clobbers).
# If this tree was previously patched for int 0x80, restore the defaults.
sed -i 's/"int $0x80"/"syscall"/g' "${MUSL_DIR}/arch/x86_64/syscall_arch.h"
sed -i 's/: "memory"/: "rcx", "r11", "memory"/g' "${MUSL_DIR}/arch/x86_64/syscall_arch.h"

cd "${MUSL_DIR}"
./configure --prefix="${MUSL_PREFIX}" CC=gcc
make -j"$(nproc)"
make install

# Some musl installs (or partial reruns) leave out musl-gcc.specs.
mkdir -p "${MUSL_PREFIX}/lib"
if [ ! -f "${MUSL_PREFIX}/lib/musl-gcc.specs" ]; then
  if [ -f "${MUSL_DIR}/lib/musl-gcc.specs" ]; then
    cp -f "${MUSL_DIR}/lib/musl-gcc.specs" "${MUSL_PREFIX}/lib/musl-gcc.specs"
  elif [ -f "${MUSL_DIR}/obj/musl-gcc" ]; then
    cp -f "${MUSL_DIR}/obj/musl-gcc" "${MUSL_PREFIX}/lib/musl-gcc.specs"
  else
    echo "missing musl-gcc.specs after install (${MUSL_PREFIX}/lib/musl-gcc.specs)" >&2
    exit 1
  fi
fi

# Fix wrapper when configure-time CC contains extra flags.
mkdir -p "${MUSL_PREFIX}/bin"
cat > "${MUSL_PREFIX}/bin/musl-gcc" <<EOS
#!/bin/sh
exec "\${REALGCC:-gcc}" "\$@" -specs "${MUSL_PREFIX}/lib/musl-gcc.specs"
EOS
chmod +x "${MUSL_PREFIX}/bin/musl-gcc"

echo "musl installed to ${MUSL_PREFIX}"
