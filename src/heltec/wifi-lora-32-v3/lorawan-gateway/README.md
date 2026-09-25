# NDW test gateway — Heltec WiFi LoRa 32 V3

A single-channel LoRaWAN gateway on one Heltec board, connecting to a network
server as a LoRa Basics Station over WiFi. With a second board running
[NDW LoRaWAN meter fleet](../lorawan-probe) it makes a complete test network
on a bench, without a real gateway.

**Hardware:** Heltec WiFi LoRa 32 V3 — ESP32-S3, SX1262, SSD1306 OLED. The
pins in `src/main.cpp` are that board's.

## One channel

The SX1262 hears one channel at one spreading factor: **US915 channel 8,
903.9 MHz, SF7 on 125 kHz (DR3)**. A real gateway hears eight channels at
every spreading factor, so ordinary devices, which hop, are mostly missed.
The meter fleet has a one-channel option for exactly this: programmed for a
test gateway, every device joins and reports on channel 8 at DR3, with ADR
off. The two are set to the same channel in their sources; change both.

Downlinks go out as the server asks: RX1 or RX2, at the frequency and data
rate it names, timed from the uplink's `xtime`.

## Programmed over USB

The workflow builds it into `builds/ndw/heltec/wifi-lora-32-v3/lora/gateway`.
Nothing is compiled in. After flashing, flash.meterfax.com's Program tab asks
for the WiFi network and passphrase, then the server address
(`wss://lns.meterfax.com`), the trust file and the gateway's token from
MeterFax (Console → Gateways → Add a gateway, using the EUI this board
reports).

The host writes one JSON command per line; every answer is a line starting
`#NDW ` followed by JSON — the protocol the other NDW firmware speaks.

| Command | Does |
| --- | --- |
| `{"cmd":"hello"}` | `eui`, `firmware`, `kind` (`lorawan-gateway`), `state` |
| `{"cmd":"status"}` | WiFi (`state`, `ssid`, `ip`, `mac`, `rssi`), the server link (`lns`: `url`, `state`, `error`, `detail`, `up`, `down`, `late`, `tokenHint`) and `radio` |
| `{"cmd":"scan"}` | the networks it can hear |
| `{"cmd":"wifi","ssid":"…","password":"…"}` | joins, answering with where it got: `connected`, `bad-auth`, `not-found`, `failed` |
| `{"cmd":"lns","url":"wss://…","trust":"<PEM>","token":"Authorization: Bearer …"}` | the server, its trust file and the token; connects straight away |
| `{"cmd":"forget"}` | drops everything and reboots |
| `{"cmd":"reboot"}` | |

The token is never sent back, only its last four characters. `lns.error`
says why the server link is down, in one word: `tls` (the trust file does
not match the server), `refused` (the token was turned away), `unreachable`,
`discovery`, `region` (the server is not US915), `dropped` or `failed`.

## Basics Station

1. `wss://<server>/router-info`: sends `{"router":"<eui>"}`, gets the address to use.
2. That address: sends `version`, gets `router_config`.
3. Each frame heard goes up as `jreq` or `updf`, with `xtime` — the board's
   microsecond clock at the end of the frame, a per-connection session in the
   top byte.
4. A `dnmsg` carries that `xtime` back with `RxDelay`; the gateway sends into
   RX1 at `xtime + RxDelay`, or RX2 a second later, and answers `dntxed`.

The token rides on both connections as an `Authorization` header.
