#!/usr/bin/env bash
#
# Build, flash and watch the LoRaWAN probe.
#
# PlatformIO is installed into .venv here, pinned by requirements.txt, rather
# than asked of the machine: a toolchain that moves on its own turns an
# unrelated rebuild into a different binary. The first run takes a few
# minutes while PlatformIO fetches the ESP32 platform; after that it is quick.
#
# Usage: ./tools.sh <command> [args]
#   setup                      install PlatformIO into .venv
#   build                      compile only
#   flash [port]               compile and write to the board, keeping its saved state
#   erase [port]               wipe the board entirely — keys, total and join nonces
#   monitor [port]             print the board's serial log
#   mac [port]                 print the board's MAC, and the DevEUI it makes of it
#   status [port]              ask the board what it knows (the AppKey is never shown)
#   provision APPKEY [DEVEUI] [port]
#                              write its keys over USB; it reboots and joins.
#                              DEVEUI defaults to the one the board makes of its MAC.
#
# provision and status speak the same line protocol a browser does: one JSON
# command per line, answers on lines starting "#NDW ".
#
# The port is found automatically when one USB serial adapter is plugged in;
# pass it as the last argument otherwise.
set -euo pipefail

cd "$(dirname "$0")"

PIO=.venv/bin/pio

# The first Python 3 that actually runs. "python3" on PATH is not always
# one: on a Mac that migrated from Intel it can be a framework build that
# fails with "Bad CPU type in executable".
python() {
  local c
  for c in python3 /opt/homebrew/bin/python3 python3.13 python3.12 python3.11 python3.10; do
    if command -v "$c" >/dev/null 2>&1 && "$c" -c 'import sys; sys.exit(sys.version_info < (3, 8))' 2>/dev/null; then
      echo "$c"
      return
    fi
  done
  echo "No working Python 3.8+ found." >&2
  exit 1
}

setup() {
  if [ ! -x "$PIO" ]; then
    "$(python)" -m venv .venv
    .venv/bin/pip install --quiet -r requirements.txt
  fi
}

port() {
  if [ -n "${1:-}" ]; then
    echo "$1"
    return
  fi
  local found
  found=$(ls /dev/cu.usbserial-* /dev/ttyUSB* 2>/dev/null || true)
  if [ "$(echo "$found" | grep -c .)" -ne 1 ]; then
    echo "Need exactly one serial port; found: ${found:-none}. Pass it explicitly." >&2
    exit 1
  fi
  echo "$found"
}

# Sends one command and prints the board's answer. Opening the port resets
# the board on these adapters, so it waits for the boot banner first.
ask() {
  local port="$1" command="$2"
  .venv/bin/python - "$port" "$command" <<'PY'
import json, sys, time
import serial
port, command = sys.argv[1], sys.argv[2]
s = serial.Serial(port, 115200, timeout=0.2)
time.sleep(2.5)
s.reset_input_buffer()
s.write((command + "\n").encode())
deadline = time.time() + 20
while time.time() < deadline:
    line = s.readline().decode(errors="replace").strip()
    if line.startswith("#NDW "):
        reply = json.loads(line[5:])
        print(json.dumps(reply, indent=2))
        sys.exit(0 if reply.get("ok") else 1)
print("The board did not answer.", file=sys.stderr)
sys.exit(1)
PY
}

case "${1:-}" in
  setup)   setup ;;
  build)   setup; "$PIO" run ;;
  flash)   setup; "$PIO" run -t upload --upload-port "$(port "${2:-}")" ;;
  erase)   setup; "$PIO" run -t erase --upload-port "$(port "${2:-}")" ;;
  monitor) setup; "$PIO" device monitor --port "$(port "${2:-}")" --baud 115200 ;;
  status)  setup; ask "$(port "${2:-}")" '{"cmd":"status"}' ;;
  provision)
    setup
    key="${2:?the AppKey, 32 hex characters}"
    eui="${3:-}"
    command=$(printf '{"cmd":"lorawan","appKey":"%s"%s}' "$key" "${eui:+,\"devEui\":\"$eui\"}")
    ask "$(port "${4:-}")" "$command"
    ;;
  mac)
    setup
    # PlatformIO's own esptool, which it fetched with the platform.
    mac=$("$PIO" pkg exec -p tool-esptoolpy -- esptool.py --port "$(port "${2:-}")" read_mac 2>/dev/null | awk '/^MAC:/{print $2; exit}')
    echo "MAC     $mac"
    echo "DevEUI  $(echo "$mac" | awk -F: '{printf "%s%s%sfffe%s%s%s\n",$1,$2,$3,$4,$5,$6}')"
    ;;
  *) sed -n '3,30p' "$0"; exit 1 ;;
esac
