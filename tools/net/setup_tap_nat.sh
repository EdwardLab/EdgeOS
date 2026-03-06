#!/usr/bin/env bash
set -euo pipefail

if [[ "${EUID}" -ne 0 ]]; then
    echo "run as root: sudo $0 [tap_if]"
    exit 1
fi

TAP_IF="${1:-tap0}"
UP_IF="${EDGEOS_UPLINK_IF:-}"
IPV6_PREFIX="${EDGEOS_IPV6_PREFIX:-fd00:2::/64}"
IPV6_GW="${EDGEOS_IPV6_GW:-fd00:2::1}"
IPV6_PLEN="${IPV6_PREFIX##*/}"

if [[ -z "${UP_IF}" ]]; then
    UP_IF="$(ip route show default 2>/dev/null | awk '/default/ {print $5; exit}')"
fi

if [[ -z "${UP_IF}" ]]; then
    echo "could not detect uplink interface; set EDGEOS_UPLINK_IF and re-run"
    exit 1
fi

if ! ip link show dev "${TAP_IF}" >/dev/null 2>&1; then
    ip tuntap add dev "${TAP_IF}" mode tap user "${SUDO_USER:-root}"
fi

ip addr flush dev "${TAP_IF}" || true
ip addr add 10.0.2.2/24 dev "${TAP_IF}"
ip -6 addr flush dev "${TAP_IF}" scope global || true
ip -6 addr add "${IPV6_GW}/${IPV6_PLEN}" dev "${TAP_IF}" || true
ip link set dev "${TAP_IF}" up

sysctl -w net.ipv4.ip_forward=1 >/dev/null
sysctl -w net.ipv6.conf.all.forwarding=1 >/dev/null || true

iptables -t nat -C POSTROUTING -s 10.0.2.0/24 -o "${UP_IF}" -j MASQUERADE 2>/dev/null || \
iptables -t nat -A POSTROUTING -s 10.0.2.0/24 -o "${UP_IF}" -j MASQUERADE

iptables -C FORWARD -i "${TAP_IF}" -o "${UP_IF}" -j ACCEPT 2>/dev/null || \
iptables -A FORWARD -i "${TAP_IF}" -o "${UP_IF}" -j ACCEPT

iptables -C FORWARD -i "${UP_IF}" -o "${TAP_IF}" -m state --state ESTABLISHED,RELATED -j ACCEPT 2>/dev/null || \
iptables -A FORWARD -i "${UP_IF}" -o "${TAP_IF}" -m state --state ESTABLISHED,RELATED -j ACCEPT

RA_STATUS="disabled"
if command -v radvd >/dev/null 2>&1; then
    RADVD_CONF="/run/edgeos-radvd-${TAP_IF}.conf"
    RADVD_PID="/run/edgeos-radvd-${TAP_IF}.pid"
    cat > "${RADVD_CONF}" <<EOF
interface ${TAP_IF}
{
    AdvSendAdvert on;
    MinRtrAdvInterval 3;
    MaxRtrAdvInterval 10;
    prefix ${IPV6_PREFIX}
    {
        AdvOnLink on;
        AdvAutonomous on;
    };
};
EOF
    if [[ -f "${RADVD_PID}" ]] && kill -0 "$(cat "${RADVD_PID}" 2>/dev/null)" 2>/dev/null; then
        RA_STATUS="radvd-running"
    else
        radvd -C "${RADVD_CONF}" -p "${RADVD_PID}"
        RA_STATUS="radvd-started"
    fi
elif command -v dnsmasq >/dev/null 2>&1; then
    DNSMASQ_PID="/run/edgeos-dnsmasq-ra-${TAP_IF}.pid"
    if [[ -f "${DNSMASQ_PID}" ]] && kill -0 "$(cat "${DNSMASQ_PID}" 2>/dev/null)" 2>/dev/null; then
        RA_STATUS="dnsmasq-running"
    else
        dnsmasq \
            --conf-file= \
            --port=0 \
            --interface="${TAP_IF}" \
            --bind-interfaces \
            --enable-ra \
            --dhcp-range=::,constructor:"${TAP_IF}",ra-only,64,30m \
            --pid-file="${DNSMASQ_PID}" >/dev/null 2>&1
        RA_STATUS="dnsmasq-started"
    fi
fi

echo "tap setup complete:"
echo "  tap interface: ${TAP_IF}"
echo "  guest network: 10.0.2.0/24"
echo "  guest gateway: 10.0.2.2"
echo "  guest ipv6 prefix: ${IPV6_PREFIX}"
echo "  guest ipv6 gateway: ${IPV6_GW}"
echo "  router advertisements: ${RA_STATUS}"
echo "  uplink iface:  ${UP_IF}"
