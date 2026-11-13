# lorawan-stack Specification

## Purpose
The platform-free LoRaWAN 1.0.3 Class A stack shared by every radio.

## Requirements

### Requirement: One LoRaWAN 1.0.3 Class A stack for every radio
The firmware SHALL work as follows. The core library `ndw-lorawan` implements LoRaWAN 1.0.3 Class A without a radio: join request and accept, session keys, data frames both ways and MAC command answers. The LoRa and Bluetooth boards use the same stack, so a meter behaves the same over both.

#### Scenario: A join request and its accept match an independent implementation
- **WHEN** a join request is built and a join accept (with or without a channel list) is parsed
- **THEN** the bytes agree with frames made by `lora-packet`, and an accept meant for another device is refused
<!-- TEST: src/host-tests/test/test_classa/test_classa.cpp › test_join_request, test_join_accept, test_join_accept_with_cflist, test_join_accept_for_another_device -->
<!-- proved (unit): src/host-tests/test/test_classa/test_classa.cpp -->

#### Scenario: Data frames and MAC answers
- **WHEN** an uplink carries MAC commands and an acknowledgement, or a downlink arrives
- **THEN** the frame encodes and decodes as the specification says, and the device answers the MAC commands the network sends
<!-- TEST: src/host-tests/test/test_classa/test_classa.cpp › test_uplink_with_mac_commands_and_ack, test_downlink, test_mac_answers -->
<!-- proved (unit): src/host-tests/test/test_classa/test_classa.cpp -->

### Requirement: Frame counters and ports are handled correctly
The firmware SHALL work as follows. The stack extends the frame counter past 16 bits, handles payloads of several blocks, and refuses port 0 for application data and a buffer too small for the frame.

#### Scenario: Counter beyond 16 bits and refused input
- **WHEN** the counter passes 65535, or a frame is built on port 0 or into a small buffer
- **THEN** the counter keeps counting and the last two are refused
<!-- TEST: src/host-tests/test/test_lorawan/test_lorawan.cpp › test_counter_past_16_bits, test_two_blocks, test_refuses_port_zero_and_a_small_buffer -->
<!-- proved (unit): src/host-tests/test/test_lorawan/test_lorawan.cpp -->
