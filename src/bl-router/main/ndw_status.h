/*
 * The status light.
 *
 * One addressable LED is the only output this box has, so it carries the
 * whole of what a technician can learn without a laptop. That makes the
 * vocabulary worth keeping small and conventional rather than expressive:
 *
 *   blue    pairing, or Bluetooth activity — the pairing-light convention
 *   green   joined and working
 *   amber   busy, outcome not yet known
 *   red     failed
 *   off     idle and provisioned; nothing to say
 *
 * Colour alone is not enough — red/green confusion affects roughly one man in
 * twelve — so every state that matters also differs in rhythm: pairing
 * blinks, failure is a long hold, success is steady.
 *
 * Named NDW_LED_* rather than NDW_STATUS_* because ndw_contract.h already
 * uses that prefix for the provisioning words sent to the console over BLE.
 * The two are different vocabularies — one is what a person sees, the other
 * is what the browser is told — and sharing a prefix made them collide.
 */

#pragma once

#include <stdbool.h>

typedef enum {
    /* Nothing to report. Provisioned and running, or waiting for a button. */
    NDW_LED_IDLE,
    /* Discoverable: a pairing window is open. Slow blue blink. */
    NDW_LED_PAIRING,
    /* A console is connected. Double blue flash, then holds dim blue. */
    NDW_LED_LINKED,
    /* Trying credentials. Amber, because the answer is not known yet. */
    NDW_LED_JOINING,
    /* On the network. Green. */
    NDW_LED_ONLINE,
    /* The join failed. Red, held long enough to be seen and understood. */
    NDW_LED_FAILED,
} ndw_led_t;

/** Brings up the LED. Safe to call once, before any status is set. */
void ndw_status_init(void);

/** Switches the light to a state. Takes effect on the next tick. */
void ndw_status_set(ndw_led_t status);

/**
 * One short blue pulse, overlaid on whatever the current state is.
 *
 * For a captured beacon: the point is that each frame is visible as a
 * discrete event, so it must not disturb the underlying state — a gateway
 * relaying steadily should still read as online between pulses.
 */
void ndw_status_pulse(void);
