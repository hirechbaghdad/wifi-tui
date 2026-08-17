#!/bin/sh
# Parser/UI smoke test. Runs wifi-tui against fake ifconfig/wpa_cli/netstat
# binaries inside a pty and checks what actually reached the screen. Nothing
# here touches the machine's network configuration.
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
output=$(mktemp -t wifi-tui-smoke)
trap 'rm -f "$output"' EXIT

if [ ! -x "$project_dir/wifi-tui" ]; then
    echo "smoke test: build wifi-tui first (make)" >&2
    exit 1
fi

# A checkout that lost the executable bits would fail in a confusing way.
chmod +x "$project_dir"/tests/fake-bin/* 2>/dev/null || true

PATH="$project_dir/tests/fake-bin:$PATH"
TERM=xterm
WIFI_TUI_RESOLV_CONF="$project_dir/tests/resolv.conf"
export PATH TERM WIFI_TUI_RESOLV_CONF

# script(1) gives the program a pty; the fresh pty has no window size, so set
# one before exec'ing. FreeBSD's script takes "script [-q] file command ...".
printf q | script -q "$output" \
    /bin/sh -c "stty rows 30 cols 120 2>/dev/null; exec '$project_dir/wifi-tui'" \
    >/dev/null 2>&1 || true

status=0
check() {
    if grep -q "$1" "$output"; then
        echo "  ok    $1"
    else
        echo "  FAIL  expected '$1' on screen" >&2
        status=1
    fi
}

check 'wpa_supplicant'   # backend detected through the control socket
check 'Lab WiFi'         # SSID from wpa_cli scan_results
check 'Cafe Guest'       # open network, space in the SSID
check 'Modern Net'       # 5 GHz network, frequency 5180 -> channel 36
check 'WPA2/3'           # WPA2+SAE transition mode
check '802.1X'           # enterprise network flagged, not offered
check '192.0.2.20'       # inet from ifconfig
check '255.255.255.0'    # netmask converted from 0xffffff00
check '192.0.2.1'        # default gateway from netstat
check '192.0.2.53'       # nameserver from resolv.conf

if [ "$status" -ne 0 ]; then
    echo 'smoke test failed' >&2
    exit 1
fi
echo 'smoke test passed'
