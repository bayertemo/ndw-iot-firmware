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
 * Queues one received beacon.
 *
 * Returns as soon as the frame is copied — it goes out with the next flush,
 * not now. Safe to call from the scan callback, which is the point: decoding
 * a beacon must not wait on a socket.
 *
 * Frames queued while the broker is unreachable are held, not dropped. The
 * counter is cumulative and the server stamps arrival itself, so a reading
 * that lands after a reconnect is still evidence of a press that happened —
 * the server decides what to make of the delay. See ndw_queue.h for the
 * bound on how much is held.
 */
void ndw_uplink_publish(const ndw_frame_t *frame);

/**
 * Sets the broker, persists it, and reconnects.
 *
 * Provisioned beside the WiFi credentials: it is a property of the site, not
 * of the firmware, and needing a rebuild to change it would mean a site visit
 * with a laptop and a cable.
 *
 * Applied outright, with no going back — for the BLE path, where somebody is
 * standing at the box and can correct a mistake on the spot.
 */
/*
 * The longest broker address the box will hold.
 *
 * In the header because provisioning checks a candidate against it before
 * handing it over: a truncated URL is worse than a refused one, since it
 * would look accepted and connect to nothing.
 */
#define NDW_BROKER_MAX 128

void ndw_uplink_set_broker(const char *uri);

/**
 * Tries a broker, and undoes it if the box cannot connect.
 *
 * For the remote path, where nobody is present. Changing a broker over the
 * broker is inherently self-endangering: the command arrives on the very
 * connection it replaces, so a wrong address leaves the box unreachable by
 * the only channel that could correct it — and an installed gateway does not
 * advertise over BLE, so the recovery is a ladder.
 *
 * So a remote change is a trial. The previous address is kept, the new one is
 * tried, and if no connection is established within the trial window the box
 * goes back on its own and says so in its next telemetry. The cost of a typo
 * becomes a few minutes of silence rather than a site visit.
 *
 * The new address is persisted immediately regardless, so a reboot mid-trial
 * still comes up on the address someone asked for — with the fallback intact
 * to catch it if that address is wrong.
 */
void ndw_uplink_try_broker(const char *uri);

/**
 * Reverts a broker trial whose window has closed without connecting.
 *
 * Polled rather than run off a timer, because the telemetry task already
 * wakes every second and a dedicated timer for a five-minute deadline would
 * be a second thing to reason about for no gain.
 */
void ndw_uplink_check_broker_trial(void);

/**
 * Whether a broker trial is currently running.
 *
 * Reported in telemetry so the platform can show a change as pending rather
 * than applied: the box is the only thing that knows whether the new address
 * actually worked.
 */
bool ndw_uplink_broker_pending(void);

/**
 * Queues a telemetry document on the gateway's own topic.
 *
 * Separate from publish() because it is a different kind of message: about
 * the box rather than relayed on behalf of a device, on a topic ingest routes
 * elsewhere, into a table that is not billing evidence. It shares the same
 * queue, so a heartbeat waits its turn behind readings already in line.
 */
void ndw_uplink_publish_telemetry(const char *body, int len);

/**
 * Publishes immediately, bypassing the queue.
 *
 * For the queue's own sender task and nothing else — it is the bottom of the
 * stack the rest of the firmware sits on top of. Returns false if the message
 * did not reach the broker, which the sender treats as "put it back".
 */
bool ndw_uplink_send(const char *topic, const char *body, int len);

/**
 * Subscribes to a topic on the shared client.
 *
 * Exposed so the command module does not need its own MQTT connection: a
 * second client would mean a second client id, and a broker evicts the older
 * connection when an id repeats — the flapping loop that got the API's
 * subscriber banned for thirty minutes.
 */
bool ndw_uplink_subscribe(const char *topic, int qos);

/** Whether the broker link is currently up. */
bool ndw_uplink_connected(void);

/**
 * The broker this box is using, provisioned or default.
 *
 * Read over BLE so the console can show it beside the WiFi details. An
 * installer standing at a box that is joined to WiFi but silent needs to see
 * which broker it is failing to reach — otherwise the two failures look
 * identical from the outside, and the address is the one they can fix.
 *
 * Never NULL; empty before init.
 */
const char *ndw_uplink_broker(void);

/**
 * Why the link is down, as a short token, or "" when it is up.
 *
 * Deliberately coarse. The console shows it to a person who can act on it,
 * and "the name does not resolve" and "the certificate was rejected" lead to
 * different fixes, while the mbedtls error code behind them leads to neither.
 */
const char *ndw_uplink_last_error(void);
