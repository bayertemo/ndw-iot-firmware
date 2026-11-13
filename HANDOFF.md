# ndw-iot-firmware — handoff

Session handoff only; behavior is in `openspec/specs/`, wanted work in `openspec/changes/`, rules in `AGENTS.md`.

## State (2026-11-13)
- OpenSpec set up; the code's behavior is recorded as six specs, and the open work is two changes: `publish-pipeline` (one publish job per run, optional `builds` branch) and `bench-validation` (Heltec receive windows on a board, hot C3).
- Branch `chore/openspec-fold`, pushed; no PR opened.

## How to ship
- CI commits "Publish ..." to `main` after a push: `git pull --rebase` before the next push; regenerate conflicts in `builds/**` or `manifest.json` with `python3 scripts/build-manifest.py`.
- Bump the board's `build.json` revision on every change.
- Update a C3 without losing its fleet or settings: write only the app, `esptool.py --chip esp32c3 write_flash 0x10000 app.bin`.
