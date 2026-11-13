# ndw-iot-firmware

Firmware images for NDW hardware, and the manifest the console reads to
decide what it can flash onto the board in front of you.

The console at [iot.ndw.ai](https://iot.ndw.ai) fetches `manifest.json` from
this repo on every visit to its flash page, then downloads the binaries a
chosen build names. Both come from `raw.githubusercontent.com`.

## Why this repository is public

Because a browser has to be able to read it.

GitHub release assets and Actions artifacts both look like the natural home
for build output, and neither works here: their download responses carry no
`Access-Control-Allow-Origin` header, so a `fetch()` from `iot.ndw.ai` is
blocked before it starts. `raw.githubusercontent.com` returns `*`, which is
what makes this arrangement work at all.

Nothing is lost by it. Firmware ships on hardware that anyone holding a board
can read back over the same USB cable used to write it, so a private
repository would be protecting something already public. No key, credential
or account data belongs in an image here.

## Layout

```
builds/ndw/<vendor>/<family>/<variant>/<radio>/<role>/
    build.json          what it is, and which chips it fits
    bootloader.bin      \
    partition-table.bin  }  committed by the firmware build
    app.bin             /
```

So `builds/ndw/espressif/esp32/c3/bl/lorawan-gateway/` is the NDW BLE Gateway
for the ESP32-C3. That path is also the build's `kind`, which is what the
manifest publishes and the console fetches against — the two cannot drift,
because `scripts/build-manifest.py` refuses a `build.json` whose `kind`
disagrees with where it sits.

## Three layers

The source is split by what changes independently:

```
src/
  core/        what a device is, on any hardware and any radio — platform-free C++
  hal/         what it runs on: the hardware layer, ESP32 for now
  radio/       how it talks: LoRa or Bluetooth
  boards/      the builds: a board picks one hardware layer and one radio
  host-tests/  the core on the host
```

| Layer | Library | |
|---|---|---|
| core | `ndw-lorawan` | LoRaWAN 1.0.3 Class A without a radio: join, session keys, data frames both ways, MAC commands |
| core | `ndw-meters` | How the simulated meters behave, and their payloads on ports 10, 11 and 12 |
| core | `ndw-fleet` | A fleet of up to 200 simulated meters on one board, OTAA and Class A, and the `#NDW` protocol that programs it |
| core | `ndw-station` | A LoRa Basics Station gateway: the server link, settings, and the `#NDW` protocol that programs it |
| core | `ndw-hal`, `ndw-radio` | The interfaces the core is written against: clock, RNG, store, console, board, network; an end device's radio and a gateway's |
| hal | `ndw-hal-esp32`, `ndw-hal-esp32-net` | Those interfaces on an ESP32 over Arduino-ESP32; the network half only for gateways |
| radio | `ndw-radio-lora` | US915 through an SX1262: an end device's Class A windows to the microsecond, and a single-channel gateway |
| radio | `ndw-radio-ble` | LoRaWAN frames in BLE 5 extended advertisements, both ways, over NimBLE |

The LoRaWAN stack is the same above both radios, so a meter behaves the same
over LoRa and over Bluetooth: it joins, reports, asks the time, checks its
link and answers MAC commands. Another MCU is a new `hal/` directory; another
radio a new `radio/` one; neither touches the core.

The builds, each a PlatformIO project finding the layers through its
`lib_extra_dirs`:

| | |
|---|---|
| `src/boards/heltec-wifi-lora-32-v3/lora-fleet` | NDW LoRaWAN meter fleet: `ndw-fleet` on the Heltec's SX1262, with its screen and battery |
| `src/boards/heltec-wifi-lora-32-v3/lora-gateway` | NDW LoRaWAN Gateway: `ndw-station` on the Heltec's SX1262, one channel |
| `src/boards/esp32-c3/ble-fleet` | NDW BLE meter fleet: `ndw-fleet` over Bluetooth, any 4MB ESP32-C3 |
| `src/boards/esp32-c3/ble-gateway` | NDW BLE Gateway: `ndw-station` over Bluetooth, any 4MB ESP32-C3 |

`pio test` in `src/host-tests` runs the core on the host: the LoRaWAN stack
against frames made by `lora-packet`, an independent implementation, and the
fleet and gateway engines against a fake radio, network and hardware. CI runs
it before any board is built.

The C3 builds need Espressif's RISC-V toolchain, which PlatformIO ships for macOS as an Intel binary only; see `AGENTS.md`.

Nothing about a device is compiled into any image here. Firmware that needs
keys takes them over USB after it is flashed.

## Behavior

What the firmware, the boards and the manifest do is specified in `openspec/specs/` (`firmware-publishing` for the manifest, the one-chip-per-build rule and CI; `lorawan-stack`, `meter-simulation`, `meter-fleet`, `lorawan-gateway`, `ble-transport` for the firmware). Wanted but unbuilt work is in `openspec/changes/`. Working rules are in `AGENTS.md`.

`manifest.json` is generated: run `python3 scripts/build-manifest.py`; a hand edit is overwritten by the next push.
