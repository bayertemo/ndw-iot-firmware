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

## One image, programmed over USB

The workflow builds it into `builds/ndw/heltec/wifi-lora-32-v3/lora/probe`
and the manifest, like the other firmware. Nothing about a device is compiled
in: after flashing, flash.meterfax.com's Program tab makes the fleet —
random DevEUIs and AppKeys — writes it over the same cable, and downloads the
CSV that MeterFax's Devices → Import reads, each row on its own profile.

The host writes one JSON command per line; every answer is a line starting
`#NDW ` followed by JSON — the protocol the NDW router firmware speaks.

| Command | Does |
| --- | --- |
| `{"cmd":"hello"}` | `eui`, `firmware`, `kind` (`lorawan-probe`), `state`, `fleet`, `maxFleet` |
| `{"cmd":"status"}` | the fleet: size, water/power/gas, joined, reports sent, devices in an anomaly (`faults.high`), clock source, signal |
| `{"cmd":"fleet-begin","count":200,"interval":900,"anomalies":20,"epoch":…,"tzOffset":…}` | starts receiving a fleet; the running one stops |
| `{"cmd":"fleet-add","devices":[["<devEui>","<appKey>","water"\|"power"\|"gas","<joinEui>"],…]}` | a chunk of devices, each chunk answered |
| `{"cmd":"fleet-commit"}` | saves the fleet and reboots to join it |
| `{"cmd":"lorawan","appKey":"…","devEui":"…"}` | a fleet of one water meter, reporting the board's real battery (`tools.sh provision`) |
| `{"cmd":"interval","seconds":900}` | report interval, 60–86400 s |
| `{"cmd":"time","epoch":…,"tzOffset":…}` | sets the clock; `tzOffset` is minutes east of UTC |
| `{"cmd":"forget"}` | drops the fleet |
| `{"cmd":"reboot"}` | |

A refusal is `{"ok":false,"error":…,"detail":…,"faults":[{"field","message"}]}`.
AppKeys are never sent back.

## What the fleet does

- Every device joins by OTAA as LoRaWAN 1.0.x on **US915 sub-band 2**, the
  sub-band the NDW gateway bridge serves, one at a time. A join nobody answers
  (a device not registered yet) is retried after 2 minutes, doubling to an
  hour.
- Reports are spread across the interval. **Water** sends MeterFax's
  `ndw-water-v1` on port 10: uint32 BE decilitres, uint8 battery %.
  **Electricity** sends `ndw-power-v1` on port 11: uint32 BE watt-hours, uint16
  BE watts. **Gas** sends `ndw-gas-v1` on port 12: uint32 BE decilitres, uint8
  battery %. Registers start at a few years' use and only ever rise.
- Water is drawn the way households draw it — mostly nothing overnight, the
  morning's showers, the evening's cooking and washing, a little more at
  weekends. Electricity is a base load with the day's use on top and now and
  then a kettle or an oven. Gas follows the heating — a morning and an evening
  run, a night setback — and the season, several times more in January than
  in July. Each device has its own size of household.
- About `anomalies`% of each device's reports are an **anomaly**: a stretch
  of 4 to 24 reports — an hour to six at 15 minutes — consuming 4 to 10 times
  the device's normal, water drawn in every interval, gas burnt, electricity
  demanded. The one kind of fault there is, and the pattern MeterFax's alerts
  look for, so devices trip them at random.
- The clock comes from the browser, then from the network's DeviceTimeAns,
  and is saved so a reboot away from both keeps roughly the day.
- Every eighth report asks for a LinkCheck; three unanswered in a row and the
  device joins again — which is how it recovers when ChirpStack forgets it.

One report holds the radio about three seconds, so 200 devices take about ten
minutes a round: the interval should be longer. The OLED shows the fleet,
joined count, the device on air, reports sent and faults showing.

## Building and flashing locally

```sh
./tools.sh flash                         # build and write, keeping saved state
./tools.sh status                        # what the fleet is doing
./tools.sh provision <appkey> [deveui]   # a fleet of one: the board itself
./tools.sh monitor
./tools.sh mac                           # the board's MAC and the DevEUI it makes
```

`tools.sh` installs a pinned PlatformIO into `.venv` here on first use.

## Things that bite

- **ChirpStack rejects a reused DevNonce, and frames whose counter went
  back.** Each device is an NVS entry with its RadioLib nonces and session,
  saved after every join and report, so neither goes backwards. Programming
  the same DevEUIs again keeps their entries; a device dropped from the fleet
  loses its entry, and if it is added back ChirpStack refuses its joins until
  its dev-nonces are flushed there. `./tools.sh erase` loses them all.
- **The fleet is in NVS, in its own partition** (`fleet`, in
  `partitions.csv`, cut from the unused second app slot). 0.2.0 kept it in
  LittleFS, which took 0.6 s to open each of 200 files: a full fleet took two
  minutes to boot, and a host asking `hello` gave up first. A board upgraded
  from 0.2.0 copies its fleet across on the first boot — about 80 s for 200 —
  and wipes the old files.
- **The single-device firmware's keys carry over.** A board upgraded from
  0.1.x keeps its device as a fleet of one, with its nonces and total.
- **The null NwkKey is deliberate.** `beginOTAA(joinEUI, devEUI, nullptr,
  appKey)` is what makes RadioLib join as 1.0.x on the AppKey alone.
- **The battery-sense switch** (GPIO37) is active-low on some V3 revisions and
  active-high on others; the firmware tries both. On USB with no cell fitted,
  the charger holds the pin near 4.2 V and it reads full.
- **Meshtastic goes back on** from flasher.meshtastic.org.
