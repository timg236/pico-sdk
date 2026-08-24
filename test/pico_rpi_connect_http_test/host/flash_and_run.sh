#!/bin/bash
# Flash pico_rpi_connect_http_test and watch the serial console for the
# result. Spawns the harness if it is not already up: the access point
# (left running afterwards) and the HTTP test server (stopped on exit if
# this run started it). Flashes over SWD with openocd by default (works
# while an app is running); -p uses picotool instead (device must be in
# BOOTSEL / USB reachable). The serial capture starts before the reset so
# no output is lost, and is always killed on exit.
#
# Usage: flash_and_run.sh [-p] [path/to/pico_rpi_connect_http_test.elf|.uf2]
#
# With no image argument the test is located in the build directory under
# PICO_SDK_PATH. Environment: SERIAL (default /dev/ttyACM0), RUN_TIMEOUT
# seconds (default 300, covers the soak test).

set -e

SERIAL="${SERIAL:-/dev/ttyACM0}"
RUN_TIMEOUT="${RUN_TIMEOUT:-300}"
HTTP_SERVER="${HTTP_SERVER:-10.42.0.1}"
HTTP_PORT="${HTTP_PORT:-8080}"
LOG="pico_rpi_connect_http_test.log"

USE_PICOTOOL=0
if [ "$1" = "-p" ]; then
    USE_PICOTOOL=1
    shift
fi
IMAGE="$1"
if [ -z "$IMAGE" ]; then
    if [ -z "$PICO_SDK_PATH" ]; then
        echo "Set PICO_SDK_PATH or pass an image path" >&2
        exit 1
    fi
    IMAGE="$PICO_SDK_PATH/build/test/pico_rpi_connect_http_test/pico_rpi_connect_http_test.elf"
fi
if [ ! -f "$IMAGE" ]; then
    echo "Image not found: $IMAGE" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

capture_pid=
server_pid=
cleanup() {
    # Never leave a background reader stealing bytes from the port, and stop
    # the server if this run spawned it.
    [ -n "$capture_pid" ] && kill "$capture_pid" 2>/dev/null
    [ -n "$server_pid" ] && kill "$server_pid" 2>/dev/null
}
trap cleanup EXIT

# Spawn the harness if it is not already up. The AP is left running for the
# next iteration: wifi_ap_mode.sh stop restores the regular WiFi network.
if ! curl -sf -m 2 -o /dev/null "http://$HTTP_SERVER:$HTTP_PORT/get"; then
    if [ "$HTTP_SERVER" = "10.42.0.1" ] &&
            ! nmcli -f NAME connection show --active | grep -q pico-http-ap; then
        "$SCRIPT_DIR/wifi_ap_mode.sh" start
    fi
    "$SCRIPT_DIR/http_test_server.py" -q -p "$HTTP_PORT" >"$SCRIPT_DIR/http_test_server.out" 2>&1 &
    server_pid=$!
    for ((i = 0; i < 10; i++)) do
        curl -sf -m 2 -o /dev/null "http://$HTTP_SERVER:$HTTP_PORT/get" && break
        sleep 1
    done
    if ! curl -sf -m 2 -o /dev/null "http://$HTTP_SERVER:$HTTP_PORT/get"; then
        echo "Test server not reachable at http://$HTTP_SERVER:$HTTP_PORT" >&2
        exit 1
    fi
fi

# Start the capture before the reset: the test prints from boot.
stty -F "$SERIAL" 115200 raw -echo
: > "$LOG"
cat "$SERIAL" >> "$LOG" &
capture_pid=$!

if [ "$USE_PICOTOOL" = 1 ]; then
    picotool load -f -x "$IMAGE"
else
    killall openocd 2>/dev/null || true
    openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg \
        -c "adapter speed 5000" -c "program $IMAGE verify reset exit"
fi

echo "Running (timeout ${RUN_TIMEOUT}s), log in $LOG"
for ((i = 0; i < RUN_TIMEOUT; i++)) do
    # Serial lines end \r\n: allow trailing whitespace.
    if grep -q '^PASSED[[:space:]]*$' "$LOG"; then
        echo "PASSED"
        exit 0
    fi
    if grep -q 'Failed' "$LOG"; then
        echo "FAILED:"
        grep 'Failed' "$LOG"
        exit 1
    fi
    sleep 1
done
echo "TIMEOUT: no result after ${RUN_TIMEOUT}s; last output:"
tail -5 "$LOG"
exit 1
