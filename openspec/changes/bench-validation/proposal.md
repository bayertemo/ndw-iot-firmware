## Why
Two things were never proved on real boards: the Heltec LoRa end device's receive windows are new code that has never run on a board, and a C3 fleet board ran very hot and once stopped mid-flash.

## What Changes
- Bench-test a Heltec fleet joining through the Heltec gateway and record the result.
- Find out whether the hot C3 is the board, its power or the firmware.

## Scope
In: bench work, and a fix if the firmware is at fault. Out: the Heltec gateway reflash (an operation on one device, not a requirement). Not parked.

## Capabilities
Modified: `meter-fleet`.
