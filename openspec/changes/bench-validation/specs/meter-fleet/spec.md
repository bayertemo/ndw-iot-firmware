## ADDED Requirements

### Requirement: A Heltec fleet joins through the Heltec gateway
A Heltec LoRa fleet programmed for channel 8 SHALL join and report through the NDW Heltec LoRa gateway, with the Class A receive windows answered on the board.

#### Scenario: Bench join
- **WHEN** a Heltec fleet is powered near a Heltec gateway connected to a network server
- **THEN** every device joins and each report reaches the server
<!-- unproved -->

### Requirement: A C3 fleet board runs within its limits
A C3 BLE fleet board SHALL run the fleet without overheating or stopping responding.

#### Scenario: A flash and a run
- **WHEN** a board is flashed and runs a fleet for an hour
- **THEN** it answers `status` throughout
<!-- unproved -->
