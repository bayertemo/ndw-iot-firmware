# ndw-iot-firmware

Baseline: `~/.claude/CLAUDE.md` (global). This file adds what is particular to ndw-iot-firmware and its overrides. NDW's open-source meter firmware: the boards under `src/boards/`, the radios under `radio/`. A public repository: a branch and a pull request, as the global rule says.

## Releases
- CI publishes each board by committing its binaries, `build.json` and `manifest.json` straight to `main`, one commit per board, in parallel jobs. After a push, `main` moves on by several commits: `git pull --rebase` before the next push, and where `builds/**` or `manifest.json` conflict, regenerate with `python3 scripts/build-manifest.py`. The flasher and iot.ndw.ai read `manifest.json` from `main`.
- Bump the board's `build.json` revision on every change, so each version is unique.

## Building on Apple silicon
- The ESP32-C3 (RISC-V) toolchain is Intel-only and fails here with "Bad CPU type in executable". Build in a linux/arm64 container with a cached volume (after the global disk check):
  `docker run --rm -v "$PWD":/repo -v ndw-pio-cache:/root/.platformio -w /repo/src/boards/esp32-c3/ble-fleet python:3.12-slim sh -c 'pip install platformio==6.2.0 && pio run -e esp32c3'`
  The S3 (Heltec) and native host-test builds run locally.
- Flash with esptool; writing only `0x10000 app.bin` keeps a board's NVS (fleet, Wi-Fi, token).

## BLE (NimBLE)
- Never `NimBLEScan::stop()` and `start(…, false)` around an uplink: both clear the results from the app task while NimBLE's host task may be delivering one, and the fleet hung within minutes. Cancel with `ble_gap_disc_cancel()` and resume with `start(0, nullptr, true)`.
- On the C3's one radio, scanning while advertising starves the board's own advertisements.
