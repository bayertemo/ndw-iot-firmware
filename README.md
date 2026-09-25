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

So `builds/ndw/espressif/esp32/c3/bl/router/` is the BLE router for the
ESP32-C3. That path is also the build's `kind`, which is what the manifest
publishes and the console fetches against — the two cannot drift, because
`scripts/build-manifest.py` refuses a `build.json` whose `kind` disagrees
with where it sits.

## Two toolchains

Most builds are ESP-IDF projects under `src/<project>`, built in Espressif's
pinned Docker image. Arduino-framework firmware is built with PlatformIO
instead, by the workflow's `build-platformio` job, and lives under
`src/<maker>/<board>/<project>` because it is written for one board rather
than one chip:

| | |
|---|---|
| `src/heltec/wifi-lora-32-v3/lorawan-probe` | NDW LoRaWAN meter fleet: up to 200 simulated water, electricity and gas meters, programmed over USB after flashing |
| `src/heltec/wifi-lora-32-v3/lorawan-gateway` | NDW test gateway: a single-channel LoRaWAN gateway speaking Basics Station to MeterFax over WiFi, programmed over USB after flashing |

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
