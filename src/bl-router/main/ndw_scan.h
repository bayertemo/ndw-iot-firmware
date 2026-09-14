/*
 * Listening for sensor beacons.
 *
 * A router's actual job. Provisioning is what you do once; carrying other
 * devices' readings is what it does for the rest of its life.
 *
 * It listens for every NDW beacon in range, not only ones belonging to the
 * account that claimed it. That is deliberate and it is what makes the
 * network a network: a gateway relays for sensors it has never met, and who
 * owns what is resolved server-side from the EUI. A router that only carried
 * its owner's traffic would be a receiver, not a relay.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/** One beacon, as read off the air. */
typedef struct {
    /** Sixteen lowercase hex characters, matching the printed identifier. */
    char eui[17];
    /** What kind of device sent it. */
    uint8_t kind;
    /** The sensor's own count. Ingest dedups on (eui, counter). */
    uint32_t counter;
    /** Seconds since the sensor booted. */
    uint32_t uptime;
    /** 0-100, or 0xff when the device cannot measure it. */
    uint8_t battery;
    /** dBm as this router heard it — the only field the sensor cannot know. */
    int8_t rssi;
} ndw_frame_t;

/** Called from the NimBLE host task for each beacon received. */
typedef void (*ndw_frame_cb_t)(const ndw_frame_t *frame);

/**
 * Starts scanning, and keeps scanning.
 *
 * Passive: the router never sends a scan request. There is nothing to ask for
 * — the whole payload is in the advertisement — and staying quiet halves the
 * airtime this router costs everyone else on the channel.
 */
void ndw_scan_start(ndw_frame_cb_t on_frame);

/**
 * Whether the radio is currently scanning.
 *
 * Exposed because scanning and advertising share one antenna, and a caller
 * that opens a pairing window needs to know it is interrupting something.
 */
bool ndw_scan_running(void);

/** Frames accepted since boot. */
uint32_t ndw_scan_count(void);
