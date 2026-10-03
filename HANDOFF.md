# AI assistant, BLE meters end to end, and the firmware in layers — handoff

30 September – 1 October 2026. Five repos, all pushed to `main` and deployed
or published. The same file is in `meterfax-web`, `meterfax-api` and
`ndw-iot-firmware`.

| Repo | Session range | Deployed |
| --- | --- | --- |
| `meterfax-web` | `84b8f31..462bd28` | yes, one pod |
| `meterfax-api` | `6aafe69..f0dde90` | yes, four pods, ConfigMap regenerated |
| `meterfax-flash` | `02571e8..aadac05` | yes, one pod |
| `ndw-iot-firmware` | `059b2cb..15be688` | published to the manifest by CI |
| `meterfax-chirpstack` | none | — (an ABP profile was tried and reverted) |

This is a dated snapshot of where a session stopped, not a spec. The code,
`CLAUDE.md` and the guard specs are what stay true. The previous handoff is in
git history (`meterfax-web` at `84b8f31`, `meterfax-api` at `6aafe69`).

**Read "Open items" first.** One gateway still needs reflashing.

---

## What changed

### Password reset email (api)

- `vendors.cybermall.email.templates.en.password-reset` is
  `646fe43303139a7ace0cfcf3`, in `config/prod.yaml` and the ConfigMap. The
  reset link is emailed now, not only logged.

### AI assistant, unburied (web)

- **One name everywhere: "AI assistant"** (was Connected apps / Integrations /
  With your AI). The Settings tab id stays `apps`.
- **The header's call to action** in the console and in My homes
  (`core/nav.ts` `AI_ASSISTANT`, an `action` CTA in `layout/app-header`) opens
  the AI assistant dialog (`blocks/ai-assistant/`, moved out of
  `pages/console/meter-points` so the owner area may import it).
- **The dialog has three states:**
  - **no fleet:** an upsell to My fleet;
  - **fleet on a plan without MCP:** Choose a plan;
  - **otherwise:** the apps — connected first, each with **Open** — then the
    rest, and a few prompts to try.
- **Apps open in a pop-up window at the right edge of the screen**
  (`openAgentWindow` in `connect-links.ts`), not a new tab: Open, Add to
  Claude, the ChatGPT/Grok steps dialog, and the Add points menu.
  Cursor and VS Code keep their `cursor:` / `vscode:` links.
- **Overview:** the card is "AI assistant" and opens the dialog.
- The account menu no longer lists Connected apps; the header button and the
  Settings tab replace it.
- ChirpStack/channel traces are gone from the UI: no Channel column in
  Devices, no Channel row in a device's facts, the device form says "Created
  on the network". Gateway EUIs are plain text, like meter IDs.
- **Site:** the plans welcome is shorter, with a larger illustration
  (landscape images fill their column, `blocks/welcome.ts`); the CTA band can
  carry a firmware-flasher note (`[flasher]`), shown on home and platform.

### BLE meters, end to end (firmware, flash)

- **NDW BLE meter fleet** and **NDW BLE Gateway**, ESP32-C3, both 0.1.1 in the
  manifest. LoRaWAN over BLE 5 extended advertising:
  - **Format:** company `0xFFFF`, format byte `'L'` up / `'D'` down, then the
    PHYPayload.
  - **Device:** full Class A, as the LoRa fleet is — OTAA, receive windows,
    DeviceTime, LinkCheck, rejoin, MAC answers.
  - **Gateway:** Basics Station, reporting every frame at US915 ch 8 / DR3,
    and advertising downlinks back until the device's RX2 closes.
- **MeterFax needs nothing new for them:** the BLE meters are OTAA on "NDW
  Simulator / v1", the same CSV and import as the LoRa fleet.
- **The flasher** knows `ble-fleet` and `ble-gateway` on its Program tab.
  The BLE fleet form has no single-channel option and reckons about 3 s a
  report.
- **On the bench:**
  - a 150-meter BLE fleet (`test-1`..`test-50` × water, electricity, gas)
    joined fully through gateway `aca704fffebce8f0`;
  - every device has reported (`list_devices` silent: 0);
  - every meter's clock is "network";
  - the gateway forwards essentially every uplink.

### The firmware in layers (firmware)

```
src/core/     ndw-lorawan  ndw-meters  ndw-fleet  ndw-station  ndw-hal  ndw-radio
src/hal/      esp32  esp32-net
src/radio/    lora-sx1262  ble-nimble
src/boards/   heltec-wifi-lora-32-v3/{lora-fleet,lora-gateway}  esp32-c3/{ble-fleet,ble-gateway}
src/host-tests/
```

- **The core is platform-free** and tested on the host (`pio test` in
  `src/host-tests`, 25 tests):
  - frames and join, against lora-packet;
  - the fleet engine, against a fake network server;
  - the gateway, through Basics Station against a fake network stack.
- **One LoRaWAN stack for both radios.** The Heltec fleet no longer uses
  RadioLib's `LoRaWANNode`; RadioLib is only the SX1262 driver.
- `bl-router` and `bl-test-button` are removed: source, the ESP-IDF CI job,
  and their published builds (iot.ndw.ai no longer offers them).
- **Versions:** BLE fleet / gateway 0.1.1, Heltec gateway 0.2.1, Heltec fleet
  0.6.1. A Heltec fleet must be programmed again once after 0.6.x (new record
  layout).
- **CI** runs the host tests before any board is built.
- **The C3 toolchain is Intel-only on macOS.** Build in Docker
  (README has the command) or install Rosetta.

### Bugs fixed on the way (firmware)

- **The BLE fleet hung after a few minutes.** NimBLE-Arduino's
  `NimBLEScan::stop()` / a fresh `start()` clear its results from the app task
  while NimBLE's task may be delivering one. The scan is now cancelled with
  `ble_gap_disc_cancel()` and resumed with `start(0, nullptr, true)`.
- **Scanning while advertising starved a board's own advertisements.** A
  device scans only during its receive windows.
- **Gateways acknowledged downlinks with id 0.** ChirpStack's `diid` runs past
  a signed int, so `in["diid"] | 0` read 0. It is now `as<uint32_t>()`.
  The Heltec gateway had the same bug.
- **A WiFi scan found nothing on the BLE gateway**, so the Program tab's
  dropdown was empty. The BLE scan pauses during it (`GatewayRadio::pause`).
- **A watchdog on every board:** 30 s for fleets, 60 s for gateways, whose
  `wifi` command blocks up to 30 s.

---

## Open items

1. **Reflash the Heltec LoRa gateway `f09e9efffe777af0`** ("NDW Firmware life
   test") to 0.2.1. Until then ChirpStack logs "Object does not exist (id: 0)"
   on its tx acks. Harmless, but noisy.
2. **The LoRa end device's receive windows are new code, never run on a
   board** (`radio/lora-sx1262`, `LoraEndDevice::receive`). Bench-test a
   Heltec fleet joining through the Heltec gateway before relying on it.
3. **CI commits to `main`** (four "Publish …" commits a run), so local `main`
   falls behind after every push. `git pull --rebase` before pushing.
   - Not done yet: one publish job a run instead of four racing (one lost
     the push race on 30 Sep and needed a rerun), and/or publishing to a
     separate `builds` branch. The second needs the flasher's manifest URL
     and iot.ndw.ai changed.
4. **Versions were not bumped between the two 0.1.1 pushes**, so two
   different C3 binaries both report 0.1.1. Bump on the next change.
5. **Utility data (Smart Meter Texas):**
   - The third-party portal in the 2016 guides (`private.smartmetertexas.com`)
     no longer exists.
   - A registration request to the SMT help desk was drafted, for the user
     to send.
   - An aggregator (Arcadia, UtilityAPI) is the faster route, and the one for
     coverage outside Texas, where it is an integration per utility.
6. **Carried over:**
   - 2700 Kennedy St, Houston: an electricity point with a water meter;
   - the two old `tsc` spec errors in the API;
   - the decisions offered and not taken;
   - alert rules as data, parked (`meterfax-api/DESIGN-alert-rules.md`).
   - ABP activation was built and reverted: nothing needs it; real ABP meters
     would.
7. **Plan limits are parked as a user story:** `meterfax-api/STORY-plan-limits.md`.
   - A device and gateway cap per plan, and a minimum ingest interval per
     device in a Redis token bucket, so free accounts running simulators
     cannot flood ingest with valid frames.
   - ChirpStack v4 cannot throttle per customer, and MeterFax has no caps
     today.
   - The plans page still promises Free "no limit on how many".
   - Do not build until asked.
8. **The C3 fleet board ran very hot** and once stopped responding mid-flash.
   A slower flash (115200) worked. Suspect the board or its power, not the
   firmware.

---

## How to check it

- **Firmware:**
  - `cd src/host-tests && pio test`: 25 pass.
  - A board's state over USB: `{"cmd":"status"}`, as the flasher sends it.
  - ChirpStack's view of the BLE gateway:
    `kubectl -n meterfax logs deploy/chirpstack-gateway-bridge-bs | grep aca704fffebce8f0`.
- **Updating a C3 without losing its fleet or settings:** write only the app,
  `esptool.py --chip esp32c3 write_flash 0x10000 app.bin`. The flasher
  rewrites the same layout without erasing, so it keeps them too.
- **MeterFax:** the MCP `list_devices` view `silent` should stay empty;
  `latest_readings` shows readings about 2 s after they are sent.
- **Tests:**
  - web: `ng test` (505 pass) and `npm run imports:check`;
  - flash: `ng test` (94 pass);
  - API: `npx vitest run` (525 pass).
