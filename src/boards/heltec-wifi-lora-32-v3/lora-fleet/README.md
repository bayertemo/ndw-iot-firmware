# NDW LoRaWAN meter fleet — Heltec WiFi LoRa 32 V3

Up to 200 simulated LoRaWAN meters on one board and one radio: water,
electricity and gas, each joining on its own keys and reporting on its own schedule,
with a household's daily rhythm and the faults real meters show. For loading
a gateway, ChirpStack and MeterFax with traffic that looks like a street's
worth of meters.

**Hardware:** Heltec WiFi LoRa 32 V3 (also sold as MakerHawk) — ESP32-S3,
SX1262, SSD1306 OLED. The pins in `src/main.cpp` are that board's. The
browser only learns the chip, so it offers this image to any ESP32-S3; on
another board it will not work.

## Behavior

The `#NDW` commands, joining, reporting, anomalies, the clock and the one-channel option are specified in `openspec/specs/meter-fleet` and `meter-simulation`. The build is `builds/ndw/heltec/wifi-lora-32-v3/lora/probe`; after flashing, flash.meterfax.com's Program tab programs the fleet over USB.

## Building and flashing locally

```sh
./tools.sh flash                         # build and write, keeping saved state
./tools.sh status                        # what the fleet is doing
./tools.sh provision <appkey> [deveui]   # a fleet of one: the board itself
./tools.sh monitor
./tools.sh mac                           # the board's MAC and the DevEUI it makes
```

`tools.sh` installs a pinned PlatformIO into `.venv` here on first use.

Not built yet: `openspec/changes/bench-validation`.
