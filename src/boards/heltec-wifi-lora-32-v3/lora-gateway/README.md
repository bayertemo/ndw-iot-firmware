# NDW LoRaWAN Gateway — Heltec WiFi LoRa 32 V3

A single-channel LoRaWAN gateway on one Heltec board, connecting to a network
server as a LoRa Basics Station over WiFi. With a second board running
[NDW LoRaWAN meter fleet](../lora-fleet) it makes a complete test network
on a bench, without a real gateway.

**Hardware:** Heltec WiFi LoRa 32 V3 — ESP32-S3, SX1262, SSD1306 OLED. The
pins in `src/main.cpp` are that board's.

## Behavior

The one-channel radio (US915 channel 8, DR3), the `#NDW` commands and Basics Station are specified in `openspec/specs/lorawan-gateway`. The build is `builds/ndw/heltec/wifi-lora-32-v3/lora/gateway`; after flashing, flash.meterfax.com's Program tab sets WiFi and the server over USB.
