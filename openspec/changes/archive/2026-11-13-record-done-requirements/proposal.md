## Why
The firmware's behavior lived in READMEs and a dated handoff. This records what the code on `main` already does as OpenSpec requirements, each proved by a host test or a file and line, so nothing requirement-like stays outside OpenSpec.

## What Changes
- Adds six capabilities as baseline requirements: `lorawan-stack`, `meter-simulation`, `meter-fleet`, `lorawan-gateway`, `ble-transport`, `firmware-publishing`.
- No code, config or test changes.

## Scope
In: behavior that exists on `main` today. Out: anything not built (see the open changes) and other repos' behavior (flasher, MeterFax, ChirpStack).

## Capabilities
New: all six above.
