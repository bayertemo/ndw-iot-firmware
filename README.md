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

The C3 builds need Espressif's RISC-V toolchain, which PlatformIO ships for
macOS as an Intel binary only: on Apple silicon, build in a container
(`docker run --rm -v "$PWD":/repo -w /repo/src/boards/esp32-c3/ble-fleet
python:3.12-slim sh -c 'pip install platformio==6.2.0 && pio run -e esp32c3'`)
or install Rosetta.

Nothing about a device is compiled into any image here. Firmware that needs
keys takes them over USB after it is flashed.

## One build, one chip

Each `build.json` targets exactly one chip family, and says so with a regex
tested against what esptool-js reports after connecting:

```json
{
  "chipMatch": "^ESP32-C3(?!\\d)",
  "parts": [{ "path": "bootloader.bin", "address": 0 }]
}
```

The split is not bureaucracy. The bootloader offset differs by family —
`0x1000` on the original ESP32, `0x0` on the C3 and S3 — so one image cannot
serve both, and a wrong offset leaves a board that does not boot until it is
reflashed. The `(?!\d)` matters too: without it `^ESP32-C3` also matches an
ESP32-C61.

Adding a chip means adding a directory, not widening a regex.

## The manifest

`manifest.json` is generated. Edit `builds/**/build.json` and let the
workflow rebuild it; a hand edit is overwritten by the next push.

For each part it records the size and SHA-256 of the committed binary, so the
console can check what it received before writing anything — a truncated
download is otherwise discovered as an unbootable board. A build whose
binaries are missing is published with `"published": false` and listed in the
picker as unavailable, which is the state every build is in until real
firmware exists.

Run it locally with:

```sh
python3 scripts/build-manifest.py
```

It fails rather than publishing something the console cannot use: an invalid
regex, two parts at one address, a `kind` that disagrees with its path, or two
builds of the same role claiming the same chips on the same vendor's board.
