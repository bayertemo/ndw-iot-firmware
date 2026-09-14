/*
 * The pairing button.
 *
 * GPIO9 is the BOOT button on the ESP32-C3 DevKitM-1, active low with an
 * internal pull-up. Verified on hardware rather than taken from the
 * schematic: GPIO_IN reads 0x00327344 released and 0x00327144 held, so bit 9
 * genuinely tracks the button.
 *
 * Worth knowing that this pin has a second, unrelated job: holding it while
 * the chip powers up puts the ROM into download mode. That is silicon
 * behaviour and firmware cannot change it, so the two gestures overlap —
 * a long press at runtime pairs, a press during reset flashes.
 */

#pragma once

/** Called from a task context when the button has been held long enough. */
typedef void (*ndw_button_cb_t)(void);

/**
 * Watches the button and calls back on a long press.
 *
 * The callback fires once when the hold threshold is reached, not repeatedly
 * while the button stays down — a technician holding it a beat too long
 * should not open two windows.
 */
void ndw_button_init(ndw_button_cb_t on_long_press);
