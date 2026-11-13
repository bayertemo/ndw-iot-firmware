# ble-transport Specification

## Purpose
How LoRaWAN frames travel in BLE 5 extended advertisements between the C3 meter fleet and gateway, and how the NimBLE scan is kept safe.

## Requirements

### Requirement: LoRaWAN frames in BLE 5 extended advertisements
The firmware SHALL work as follows. A frame travels with company identifier `0xFFFF`, a format byte `'L'` for an uplink or `'D'` for a downlink, then the PHYPayload. The gateway reports every frame at US915 channel 8, DR3, and advertises downlinks back until the device's RX2 closes.

#### Scenario: A frame is wrapped
- **WHEN** a frame is wrapped for the air
- **THEN** it carries `0xFFFF`, `0x4C` or `0x44`, then the payload
<!-- proved: src/radio/ble-nimble/src/ndw_ble_frame.h:15-33 -->

### Requirement: The scan is cancelled and resumed, never stopped
The firmware SHALL work as follows. The scan is cancelled with `ble_gap_disc_cancel()` and resumed with `start(0, nullptr, true)`, since `NimBLEScan::stop()` and a fresh `start()` clear results while NimBLE's task may be delivering one, which hung the fleet.

#### Scenario: An uplink
- **WHEN** a device sends during a scan
- **THEN** the scan is cancelled through the GAP call and resumed, unless paused
<!-- proved: src/radio/ble-nimble/src/ndw_radio_ble.cpp:99-109 (cancelScan, resumeScan), :247-248 -->

### Requirement: A device scans only in its receive windows
The firmware SHALL work as follows. On the C3's one radio, scanning while advertising starves the board's own advertisements, so a device scans only during its receive windows.

#### Scenario: Between windows
- **WHEN** no receive window is open
- **THEN** the device does not scan
<!-- proved: src/radio/ble-nimble/src/ndw_radio_ble.cpp:130-137, :159, :170 (scan cancelled outside receive windows) -->
