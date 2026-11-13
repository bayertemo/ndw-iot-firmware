## 1. LoRa receive windows
- [ ] 1.1 Flash a Heltec fleet and a Heltec gateway; program the fleet with the one-channel option (channel 8)
- [ ] 1.2 Watch every device join and report (`LoraEndDevice::receive`, `src/radio/lora-sx1262`)
- [ ] 1.3 Note the result and any timing fix

## 2. Hot C3 fleet board
- [ ] 2.1 Repeat the flash at 115200 and at the default rate on a second board and supply
- [ ] 2.2 Measure the temperature running the fleet; fix the firmware only if it is the cause
