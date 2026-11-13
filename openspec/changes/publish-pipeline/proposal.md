## Why
CI commits to `main` four "Publish ..." commits a run, in racing jobs. One lost the push race on 30 Sep 2026 and needed a rerun, and local `main` falls behind after every push.

## What Changes
- One publish job a run instead of one per board, so there is one commit and no race.
- Optionally publish to a separate `builds` branch instead of `main`.

## Scope
In: `.github/workflows/build.yml` and `scripts/build-manifest.py`. Not parked, but the second option is blocked on other repos: the flasher's manifest URL and iot.ndw.ai (meterfax-flash, the console) must change first, in rollout order, a service that is asked for something new shipping before the one that asks.

## Capabilities
Modified: `firmware-publishing`.
