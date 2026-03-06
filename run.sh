#!/usr/bin/env bash
set -euo pipefail

clear

OBJ_DIR="${EDGEOS_OBJ:-/tmp/edge_obj}"
OUT_DIR="${EDGEOS_OUT:-/tmp/edge_out}"
TINYX_INSTALL_DIR="${OUT_DIR}/tinyx-install"
TINYX_INSTALL_FALLBACK="$(pwd)/out/tinyx-install"
TINYX_INSTALL_SAVE=""

if [[ -d "${TINYX_INSTALL_DIR}" ]]; then
    TINYX_INSTALL_SAVE="$(mktemp -d /tmp/edge_tinyx_save.XXXXXX)"
    cp -a "${TINYX_INSTALL_DIR}" "${TINYX_INSTALL_SAVE}/"
fi

rm -rf "${OBJ_DIR}" "${OUT_DIR}"

if [[ -n "${TINYX_INSTALL_SAVE}" ]] && [[ -d "${TINYX_INSTALL_SAVE}/tinyx-install" ]]; then
    mkdir -p "${OUT_DIR}"
    cp -a "${TINYX_INSTALL_SAVE}/tinyx-install" "${OUT_DIR}/"
    rm -rf "${TINYX_INSTALL_SAVE}"
elif [[ -d "${TINYX_INSTALL_FALLBACK}" ]]; then
    mkdir -p "${OUT_DIR}"
    cp -a "${TINYX_INSTALL_FALLBACK}" "${OUT_DIR}/tinyx-install"
    echo "[run] seeded tinyx-install from ${TINYX_INSTALL_FALLBACK}"
fi

make OBJ="${OBJ_DIR}" OUT="${OUT_DIR}"

NET_MODE="${EDGEOS_NET:-tap}"       # tap | user
TAP_IF="${EDGEOS_TAP_IF:-tap0}"

check_tap_backend() {
    if ! command -v ip >/dev/null 2>&1; then
        echo "[run] error: 'ip' command not found; cannot validate TAP backend"
        exit 1
    fi
    if ! ip link show dev "${TAP_IF}" >/dev/null 2>&1; then
        echo "[run] error: TAP interface '${TAP_IF}' not found"
        echo "[run] create/setup with: sudo tools/net/setup_tap_nat.sh ${TAP_IF}"
        exit 1
    fi
    if ! ip -o link show dev "${TAP_IF}" | grep -q "UP"; then
        echo "[run] error: TAP interface '${TAP_IF}' is down"
        echo "[run] bring it up with: sudo ip link set ${TAP_IF} up"
        exit 1
    fi
    if ! ip -4 -o addr show dev "${TAP_IF}" | grep -q "10\\.0\\.2\\.2/24"; then
        echo "[run] error: '${TAP_IF}' must have host gateway IP 10.0.2.2/24"
        echo "[run] set it with: sudo ip addr add 10.0.2.2/24 dev ${TAP_IF}"
        echo "[run] or run full setup: sudo tools/net/setup_tap_nat.sh ${TAP_IF}"
        exit 1
    fi
}

NET_ARGS=()
USB_MODE="${EDGEOS_USB:-xhci-mouse}"   # off | uhci-mouse | xhci-mouse
USB_ARGS=()

if [[ "${NET_MODE}" == "tap" ]]; then
    if command -v ip >/dev/null 2>&1 && ip link show dev "${TAP_IF}" >/dev/null 2>&1; then
        if ip -o link show dev "${TAP_IF}" | grep -q "UP"; then
            echo "[run] using TAP networking: ${TAP_IF}"
            NET_ARGS=(-netdev "tap,id=net0,ifname=${TAP_IF},script=no,downscript=no" -device e1000,netdev=net0)
        else
            echo "[run] TAP ${TAP_IF} exists but is down, disabling networking"
        fi
    else
        echo "[run] TAP ${TAP_IF} not found, networking disabled"
    fi

elif [[ "${NET_MODE}" == "user" ]]; then
    echo "[run] using QEMU user networking (SLiRP)"
    NET_ARGS=(-netdev user,id=net0 -device e1000,netdev=net0)

else
    echo "[run] unknown EDGEOS_NET=${NET_MODE}, defaulting to user networking"
    NET_ARGS=(-netdev user,id=net0 -device e1000,netdev=net0)
fi

case "${USB_MODE}" in
    off)
        echo "[run] USB disabled"
        ;;
    uhci-mouse)
        echo "[run] USB: piix3 UHCI + usb-mouse"
        USB_ARGS=(-device piix3-usb-uhci,id=usb0 -device usb-mouse,bus=usb0.0)
        ;;
    xhci-mouse)
        echo "[run] USB: qemu-xhci + usb-mouse"
        USB_ARGS=(-device qemu-xhci,id=usb0 -device usb-mouse,bus=usb0.0)
        ;;
    *)
        echo "[run] unknown EDGEOS_USB=${USB_MODE}, using xhci-mouse"
        USB_ARGS=(-device qemu-xhci,id=usb0 -device usb-mouse,bus=usb0.0)
        ;;
esac

qemu-system-x86_64 \
    -enable-kvm \
    -cpu host,migratable=off \
    -machine pc,accel=kvm \
    -smp 4,sockets=1,cores=4,threads=1 \
    -m 2048M \
    -rtc base=utc,clock=host \
    -no-reboot \
    -no-shutdown \
    -serial stdio \
    -device ich9-ahci,id=ahci \
    -drive file="${OUT_DIR}/rootfs.img",format=raw,if=none,id=rootfsdisk,cache=none,aio=native,discard=unmap \
    -device ide-hd,drive=rootfsdisk,bus=ahci.0 \
    -cdrom "${OUT_DIR}/edgeos.iso" \
    "${NET_ARGS[@]}" \
    "${USB_ARGS[@]}" \
    -boot d
