## ADDED Requirements

### Requirement: A fleet of up to 200 devices on one board
The firmware SHALL work as follows. A board runs up to 200 simulated devices, each joining by OTAA on its own keys, one at a time, and reporting on its own schedule spread across the interval. `hello` reports `maxFleet`.

#### Scenario: A fleet joins, then reports with the counter moving on
- **WHEN** a fleet is programmed and the network answers its joins
- **THEN** each device joins, reports, and its frame counter moves on with every report
<!-- TEST: src/host-tests/test/test_fleet/test_fleet.cpp › test_joins_then_reports_with_the_counter_moving_on -->
<!-- proved (unit): src/host-tests/test/test_fleet/test_fleet.cpp -->

#### Scenario: A fleet larger than the board allows
- **WHEN** `fleet-begin` asks for more than 200 devices or fewer than 1
- **THEN** it is refused
<!-- proved: src/core/ndw-fleet/src/ndw_fleet.cpp:646 -->

### Requirement: Programmed over USB with `#NDW` commands
The firmware SHALL work as follows. The host writes one JSON command per line; every answer is a line starting `#NDW `. Commands: `hello`, `status`, `fleet-begin`, `fleet-add`, `fleet-commit`, `lorawan`, `interval` (60 to 86400 s), `channel`, `time`, `forget`, `reboot`. A refusal carries `ok:false`, `error`, `detail` and `faults`. AppKeys are never sent back.

#### Scenario: An interval out of range
- **WHEN** the interval is below 60 or above 86400 seconds
- **THEN** the command is refused with the field named
<!-- proved: src/core/ndw-fleet/src/ndw_fleet.cpp:651 -->

### Requirement: DevNonce and counters never repeat
The firmware SHALL work as follows. A device's DevNonce and counters are saved before anything goes on the air, so neither repeats after a reboot. A new device's DevNonces start at a random point. Programming the same DevEUIs with the same keys keeps their sessions.

#### Scenario: Reprogramming the same keys
- **WHEN** a fleet is programmed again with the same DevEUIs and keys
- **THEN** each device keeps its session and does not join again
<!-- TEST: src/host-tests/test/test_fleet/test_fleet.cpp › test_reprogramming_the_same_keys_keeps_the_session -->
<!-- proved (unit): src/host-tests/test/test_fleet/test_fleet.cpp -->

### Requirement: Unanswered joins back off
The firmware SHALL work as follows. A join nobody answers is retried after 2 minutes, doubling up to an hour.

#### Scenario: The network stays silent
- **WHEN** joins go unanswered
- **THEN** the wait between attempts doubles from 120 s to at most 3600 s
<!-- TEST: src/host-tests/test/test_fleet/test_fleet.cpp › test_backs_off_while_its_joins_go_unanswered -->
<!-- proved (unit): src/host-tests/test/test_fleet/test_fleet.cpp -->

### Requirement: Link check and rejoin
The firmware SHALL work as follows. Every eighth report asks for a LinkCheck; three unanswered in a row and the device joins again, which is how it recovers when the network forgets it.

#### Scenario: Three link checks go unanswered
- **WHEN** three LinkCheck requests get no answer
- **THEN** the device joins again
<!-- TEST: src/host-tests/test/test_fleet/test_fleet.cpp › test_joins_again_after_three_unanswered_link_checks -->
<!-- proved (unit): src/host-tests/test/test_fleet/test_fleet.cpp -->

### Requirement: The clock comes from the host, then the network
The firmware SHALL work as follows. The clock is set by the host's `time` command, then by the network's DeviceTimeAns, and is saved so a reboot keeps roughly the day. `tzOffset` is minutes east of UTC.

#### Scenario: The network answers with the time
- **WHEN** a DeviceTimeAns arrives
- **THEN** the fleet's clock source becomes the network
<!-- TEST: src/host-tests/test/test_fleet/test_fleet.cpp › test_takes_the_network_time -->
<!-- proved (unit): src/host-tests/test/test_fleet/test_fleet.cpp -->

### Requirement: One-channel option for the NDW gateway
The firmware SHALL work as follows. A fleet may be kept to one US915 channel (0 to 63) at DR3 with ADR off, for the NDW LoRaWAN Gateway; `null` frees it. The channel survives a reprogram of the same devices.

#### Scenario: A channel is given
- **WHEN** a fleet is programmed with a channel
- **THEN** its devices use only that channel, and an out-of-range or non-number channel is refused
<!-- TEST: src/host-tests/test/test_fleet/test_fleet.cpp › test_keeps_the_channel_it_was_given -->
<!-- proved (unit): src/host-tests/test/test_fleet/test_fleet.cpp -->

### Requirement: A watchdog on every board
The firmware SHALL work as follows. Fleet boards restart a hung program after 30 seconds, gateway boards after 60, since their `wifi` command blocks up to 30.

#### Scenario: A board starts
- **WHEN** a board's main starts
- **THEN** it arms the watchdog with its limit
<!-- proved: src/boards/esp32-c3/ble-fleet/src/main.cpp:39, src/boards/heltec-wifi-lora-32-v3/lora-fleet/src/main.cpp:167, src/boards/esp32-c3/ble-gateway/src/main.cpp:43, src/boards/heltec-wifi-lora-32-v3/lora-gateway/src/main.cpp:105 -->
