# meter-simulation Specification

## Purpose
How the simulated water, electricity and gas meters behave and what they send.

## Requirements

### Requirement: Meter payloads on ports 10, 11 and 12
The firmware SHALL work as follows. Water sends `ndw-water-v1` on port 10 (uint32 big-endian decilitres, uint8 battery percent). Electricity sends `ndw-power-v1` on port 11 (uint32 watt-hours, uint16 watts). Gas sends `ndw-gas-v1` on port 12 (uint32 decilitres, uint8 battery percent).

#### Scenario: Each kind encodes on its own port
- **WHEN** a water, electricity or gas reading is encoded
- **THEN** it is five or six bytes on port 10, 11 or 12, big-endian
<!-- TEST: src/host-tests/test/test_meters/test_meters.cpp › test_water_is_port_10_five_bytes_big_endian, test_power_is_port_11_with_demand, test_gas_is_port_12 -->
<!-- proved (unit): src/host-tests/test/test_meters/test_meters.cpp -->

### Requirement: Registers start in use and only rise
The firmware SHALL work as follows. A new meter's register starts at a few years of use, and over a week of simulated reports it never goes down.

#### Scenario: A week of readings
- **WHEN** a meter is seeded and run for a week
- **THEN** the starting register is a few years in and every later reading is at least the one before
<!-- TEST: src/host-tests/test/test_meters/test_meters.cpp › test_seed_starts_a_few_years_in, test_registers_only_rise_over_a_week -->
<!-- proved (unit): src/host-tests/test/test_meters/test_meters.cpp -->

### Requirement: Anomalies of 4 to 24 reports
The firmware SHALL work as follows. About the configured percent (0 to 90) of a device's reports start an anomaly, a stretch of 4 to 24 reports consuming 4 to 10 times the device's normal: the pattern MeterFax alerts look for.

#### Scenario: A fleet is programmed with anomalies
- **WHEN** `fleet-begin` is sent with `anomalies` above 90
- **THEN** it is refused; otherwise devices enter anomalies at about that rate
<!-- proved: src/core/ndw-fleet/src/ndw_fleet.cpp:645-654 (range), src/core/ndw-meters/src/ndw_meters.cpp:97 (stepAnomaly) -->
