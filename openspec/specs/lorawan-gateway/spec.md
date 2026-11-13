# lorawan-gateway Specification

## Purpose
What the NDW LoRaWAN gateways (Heltec LoRa, ESP32-C3 BLE) do: a LoRa Basics Station client programmed over USB.

## Requirements

### Requirement: A LoRa Basics Station gateway
The firmware SHALL work as follows. The gateway asks `/router-info` where to connect, connects there, sends `version`, receives `router_config`, forwards each frame heard as `jreq` or `updf` with an `xtime`, and carries downlinks back, answering `dntxed`.

#### Scenario: Programming and discovery
- **WHEN** WiFi and the server are programmed
- **THEN** it connects through discovery and the status shows the link
<!-- TEST: src/host-tests/test/test_station/test_station.cpp › test_programs_and_connects_through_discovery -->
<!-- proved (unit): src/host-tests/test/test_station/test_station.cpp -->

#### Scenario: Uplink and downlink
- **WHEN** the radio hears a frame, and later the server sends a `dnmsg`
- **THEN** the frame goes up where the radio reported it, and the downlink is handed to the radio with its window (RX1 or RX2)
<!-- TEST: src/host-tests/test/test_station/test_station.cpp › test_forwards_a_frame_where_the_radio_reports_it, test_hands_a_downlink_to_the_radio_with_its_window -->
<!-- proved (unit): src/host-tests/test/test_station/test_station.cpp -->

### Requirement: Downlink acknowledgements carry the server's id
The firmware SHALL work as follows. The `diid` of a `dnmsg` is read as an unsigned 32-bit number and echoed in `dntxed`, since ChirpStack's ids run past a signed int.

#### Scenario: A large diid
- **WHEN** a `dnmsg` has a `diid` above 2147483647
- **THEN** `dntxed` repeats it, not 0
<!-- proved: src/core/ndw-station/src/ndw_station.cpp:335, :359 -->

### Requirement: A downlink from an older connection is refused
The firmware SHALL work as follows. The top byte of `xtime` is a per-connection session; a Class A downlink carrying another session's byte is not sent.

#### Scenario: A reconnect
- **WHEN** a downlink arrives with an `xtime` from the previous connection
- **THEN** it is refused
<!-- proved: src/core/ndw-station/src/ndw_station.cpp:185, :328 -->

### Requirement: Commands and the reason the link is down
The firmware SHALL work as follows. Commands: `hello`, `status`, `scan`, `wifi` (answers `connected`, `bad-auth`, `not-found` or `failed`), `lns`, `forget`, `reboot`. The token is never sent back, only its last four characters. `lns.error` is one word: `tls`, `refused`, `unreachable`, `discovery`, `region`, `dropped` or `failed`.

#### Scenario: A turned-away token
- **WHEN** the server refuses the token
- **THEN** `status` shows `lns.error` as `refused` and `tokenHint` as the last four characters
<!-- proved: src/core/ndw-station/src/ndw_station.cpp:214, :559 -->

### Requirement: US915 only
The firmware SHALL work as follows. A server that runs another region is refused with `region`.

#### Scenario: A non-US915 server
- **WHEN** `router_config` names another region
- **THEN** the link fails with `region`
<!-- proved: src/core/ndw-station/src/ndw_station.cpp:406-408 -->

### Requirement: The Heltec gateway hears one channel
The firmware SHALL work as follows. It hears US915 channel 8 (903.9 MHz) at DR3, which a real eight-channel gateway would not; the meter fleet's one-channel option matches it.

#### Scenario: Both set to channel 8
- **WHEN** a Heltec fleet is pinned to channel 8 near a Heltec gateway
- **THEN** the gateway can hear it
<!-- proved: src/radio/lora-sx1262/src/ndw_radio_lora.cpp:86-89 -->

### Requirement: A WiFi scan pauses the radio
The firmware SHALL work as follows. A scan pauses the gateway's radio and resumes it after, so the Program tab can list networks on the BLE gateway.

#### Scenario: Scan on the BLE gateway
- **WHEN** `scan` is sent
- **THEN** the radio is paused and then resumed
<!-- proved: src/core/ndw-station/src/ndw_station.cpp:597-599, src/radio/ble-nimble/src/ndw_radio_ble.cpp:292 -->
