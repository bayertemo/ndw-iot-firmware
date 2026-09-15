/*
 * Publishing received beacons to the platform.
 *
 * The last link. A router that hears a sensor and says nothing is a very
 * expensive LED.
 *
 * The envelope is ChirpStack's, deliberately — the same shape a LoRaWAN
 * uplink arrives in, with the LoRaWAN-only fields simply absent. The API's
 * interfaces.ts spells out why: the fields carry the same information under
 * either radio, so a second envelope would mean a second adapter, a second
 * thing for a UI to understand, and a second shape for an integrator to
 * learn. Matching it is not pretending BLE is LoRaWAN.
 *
 * Topic is ndw/<account>/ble/<device>/up. The account segment is proved by
 * the broker before ingest sees it, which is what lets the pipeline trust the
 * prefix rather than re-authenticating every frame.
 */

#pragma once

#include <stdbool.h>

#include "ndw_scan.h"

/**
 * Brings up the MQTT client.
 *
 * Safe to call before WiFi is up: the client connects when the network
 * arrives and reconnects on its own afterwards. A router spends its life
 * somewhere with unreliable WiFi, so treating a disconnection as an error
 * rather than a normal condition would mean a device that needs rebooting
 * every time the AP does.
 */
void ndw_uplink_init(const char *gateway_eui);

/**
 * Publishes one received beacon.
 *
 * Drops the frame if the link is down rather than queueing it. That is a real
 * decision: a queue deep enough to ride out an outage would also replay stale
 * readings on reconnect, and a counter that arrives an hour late is worse
 * than one that never arrives — the sensor will send its next press anyway,
 * and the count is cumulative, so nothing is permanently lost.
 */
void ndw_uplink_publish(const ndw_frame_t *frame);

/**
 * Sets the broker, persists it, and reconnects.
 *
 * Provisioned beside the WiFi credentials: it is a property of the site, not
 * of the firmware, and needing a rebuild to change it would mean a site visit
 * with a laptop and a cable.
 */
void ndw_uplink_set_broker(const char *uri);

/** Whether the broker link is currently up. */
bool ndw_uplink_connected(void);
