# NDW LoRaWAN probe — Heltec WiFi LoRa 32 V3

A test device that behaves like a water meter, for proving a gateway hears it
and the readings reach MeterFax — and for carrying to where a meter would sit,
to see whether the radio reaches from there.

**Hardware:** Heltec WiFi LoRa 32 V3 (also sold as MakerHawk) — ESP32-S3,
SX1262, SSD1306 OLED. The pins in `src/main.cpp` are that board's. The
browser only learns the chip, so it offers this image to any ESP32-S3; on
another board it will not work.

## One image, programmed over USB

The workflow builds it into `builds/ndw/heltec/wifi-lora-32-v3/lora/probe`
and the manifest, like the other firmware. Nothing about a device is compiled
in: after flashing, its keys are written over the same USB cable, from a
browser or from `tools.sh`, and kept in flash across reboots and upgrades.

The host writes one JSON command per line; every answer is a line starting
`#NDW ` followed by JSON — the protocol the NDW router firmware speaks.

| Command | Does |
| --- | --- |
| `{"cmd":"hello"}` | `eui`, `firmware`, `state` (`unprovisioned`, `joining`, `joined`) |
| `{"cmd":"status"}` | everything the screen shows: DevEUI, JoinEUI, band, interval, total, uplinks, signal, battery |
| `{"cmd":"lorawan","appKey":"…","devEui":"…","joinEui":"…"}` | saves the keys, reboots and joins. `devEui` defaults to the board's MAC widened with FF:FE; `joinEui` to zeros |
| `{"cmd":"interval","seconds":60}` | how often to report, 15–3600 s |
| `{"cmd":"forget"}` | drops the keys and join nonces, keeps the running total |
| `{"cmd":"reboot"}` | |

A refusal is `{"ok":false,"error":…,"detail":…,"faults":[{"field","message"}]}`.
The AppKey is never sent back; `status` says only whether the board is
provisioned.

## What it does once provisioned

- Joins by OTAA as LoRaWAN 1.0.x on **US915 sub-band 2** (channels 8–15 plus
  65), the sub-band the NDW gateway bridge serves.
- Every interval, sends MeterFax's `ndw-water-v1` payload on port 10: a
  big-endian uint32 running total in decilitres, then a uint8 battery percent.
  The total only rises and survives reboots and re-provisioning — a register
  that restarts from zero reads as a meter running backwards.
- Shows its state on the OLED: joining or sent, total and countdown, uplinks
  sent and answered, the last downlink's RSSI and SNR, and the battery. Until
  it is provisioned, the screen shows its DevEUI instead.

Register the same DevEUI and AppKey in MeterFax (Devices → Add device). For
its readings to decode as water the device must be on the ChirpStack profile
named **`ndw-water-v1`** — MeterFax chooses its decoder by profile name.

## Building and flashing locally

```sh
./tools.sh flash                         # build and write, keeping saved state
./tools.sh status                        # what the board knows
./tools.sh provision <appkey> [deveui]   # write its keys; it reboots and joins
./tools.sh monitor
./tools.sh mac                           # the board's MAC and the DevEUI it makes
```

`tools.sh` installs a pinned PlatformIO into `.venv` here on first use.

## Things that bite

- **ChirpStack rejects a reused DevNonce.** The probe saves its join nonces
  and keeps them for the keys they were counted under, so a reboot rejoins. A
  board re-provisioned as a different device starts counting again. After
  `./tools.sh erase`, or a RadioLib upgrade that changes the saved format, the
  count restarts too, and ChirpStack refuses joins until it passes the old
  count or the device's nonces are flushed there.
- **The null NwkKey is deliberate.** `beginOTAA(joinEUI, devEUI, nullptr,
  appKey)` is what makes RadioLib join as 1.0.x on the AppKey alone.
- **The battery-sense switch** (GPIO37) is active-low on some V3 revisions and
  active-high on others; the firmware tries both. On USB with no cell fitted,
  the charger holds the pin near 4.2 V and it reads full.
- **Meshtastic goes back on** from flasher.meshtastic.org.
