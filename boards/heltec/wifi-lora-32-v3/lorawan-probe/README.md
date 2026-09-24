# LoRaWAN probe — Heltec WiFi LoRa 32 V3

A test device that behaves like a water meter, for proving a gateway hears it
and the readings reach MeterFax. Not an NDW product image: it is built and
flashed locally, and nothing in `builds/` or the manifest refers to it.

**Hardware:** Heltec WiFi LoRa 32 V3 (also sold as MakerHawk) — ESP32-S3,
SX1262, SSD1306 OLED. The pins in `src/main.cpp` are that board's; another
board needs its own directory beside this one.

## What it does

- Joins by OTAA as a LoRaWAN 1.0.x device on **US915 sub-band 2** (channels
  8–15 plus 65), the sub-band the NDW gateway bridge serves.
- Every minute, sends MeterFax's `ndw-water-v1` payload on port 10: a
  big-endian uint32 running total in decilitres, then a uint8 battery percent.
  The total only rises, and is kept across reboots — a register that restarts
  from zero reads as a meter running backwards.
- Shows its state on the OLED: joining or sent, the total and a countdown to
  the next uplink, uplinks sent and answered, the last downlink's RSSI and
  SNR, and the battery.

## Using it

```sh
cp include/credentials.example.h include/credentials.h   # then fill it in
./tools.sh mac        # the board's MAC and a DevEUI made from it
./tools.sh flash
./tools.sh monitor
```

Register the same DevEUI and AppKey in MeterFax (Devices → Add device) on a
LoRaWAN 1.0.3 OTAA US915 profile. For its readings to decode as water, the
device must be on the ChirpStack profile named **`ndw-water-v1`** — MeterFax
chooses its decoder by profile name.

## Things that bite

- **ChirpStack rejects a reused DevNonce,** and RadioLib starts from zero on
  every boot. The probe saves its nonces to flash, so a reboot rejoins. After
  `./tools.sh erase` they are gone: the next join is refused until the device
  is deleted and re-added in ChirpStack, or its nonces cleared there.
- **The null NwkKey is deliberate.** `beginOTAA(joinEUI, devEUI, nullptr,
  appKey)` is what makes RadioLib join as 1.0.x on the AppKey alone.
- **The battery-sense switch** (GPIO37) is active-low on some V3 revisions and
  active-high on others; the firmware tries both. On USB with no cell fitted,
  the charger holds the pin near 4.2 V and it reads full.
- **Meshtastic goes back on** from flasher.meshtastic.org.
