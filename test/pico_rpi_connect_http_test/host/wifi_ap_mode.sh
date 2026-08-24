#!/bin/bash
# Switch this Pi's WiFi into AP mode for pico_rpi_connect_http_test, and back.
#
# Uses a NetworkManager hotspot in shared mode: the Pi serves DHCP and is
# reachable at 10.42.0.1 (the default HTTP_TEST_SERVER in the test). Stopping
# the hotspot deletes the profile so NetworkManager reconnects to the regular
# WiFi network. The SSID/password here match the test's build-time defaults.
#
# Usage: wifi_ap_mode.sh start|stop|status

set -e

SSID="pico-http-test"
PASSWORD="pico-http-test-pw"
IFACE="${WIFI_AP_IFACE:-wlan0}"
CON_NAME="pico-http-ap"

case "$1" in
start)
    # band bg: the Pico W is 2.4GHz-only.
    nmcli device wifi hotspot ifname "$IFACE" con-name "$CON_NAME" \
        ssid "$SSID" band bg password "$PASSWORD"
    echo "AP '$SSID' up on $IFACE; server address 10.42.0.1"
    ;;
stop)
    nmcli connection down "$CON_NAME" || true
    nmcli connection delete "$CON_NAME" || true
    echo "AP stopped; NetworkManager will reconnect $IFACE to the regular network"
    ;;
status)
    nmcli -f NAME,DEVICE,STATE connection show --active | grep "$CON_NAME" ||
        echo "AP not active"
    ;;
*)
    echo "Usage: $0 start|stop|status" >&2
    exit 1
    ;;
esac
