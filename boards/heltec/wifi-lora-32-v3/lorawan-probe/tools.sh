#!/usr/bin/env bash
#
# Build, flash and watch the LoRaWAN probe.
#
# PlatformIO is installed into .venv here, pinned by requirements.txt, rather
# than asked of the machine: a toolchain that moves on its own turns an
# unrelated rebuild into a different binary. The first run takes a few
# minutes while PlatformIO fetches the ESP32 platform; after that it is quick.
#
# Usage: ./tools.sh <command> [serial port]
#   setup     install PlatformIO into .venv
#   build     compile only
#   flash     compile and write to the board, keeping its saved state
#   erase     wipe the board entirely — its running total and join nonces too
#   monitor   print the board's serial log
#   mac       print the board's MAC, and the DevEUI it suggests
#
# The port is found automatically when one USB serial adapter is plugged in;
# pass it as the second argument otherwise.
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

need_credentials() {
  if [ ! -f include/credentials.h ]; then
    echo "No include/credentials.h — copy include/credentials.example.h and fill it in." >&2
    exit 1
  fi
}

case "${1:-}" in
  setup)   setup ;;
  build)   setup; need_credentials; "$PIO" run ;;
  flash)   setup; need_credentials; "$PIO" run -t upload --upload-port "$(port "${2:-}")" ;;
  erase)   setup; "$PIO" run -t erase --upload-port "$(port "${2:-}")" ;;
  monitor) setup; "$PIO" device monitor --port "$(port "${2:-}")" --baud 115200 ;;
  mac)
    setup
    # PlatformIO's own esptool, which it fetched with the platform.
    mac=$("$PIO" pkg exec -p tool-esptoolpy -- esptool.py --port "$(port "${2:-}")" read_mac 2>/dev/null | awk '/^MAC:/{print $2; exit}')
    echo "MAC     $mac"
    echo "DevEUI  $(echo "$mac" | awk -F: '{printf "%s%s%sfffe%s%s%s\n",$1,$2,$3,$4,$5,$6}')"
    ;;
  *) sed -n '3,21p' "$0"; exit 1 ;;
esac
