/*
 * Commands from the platform.
 *
 * The other direction from everything else here. A router publishes what it
 * hears and what it is doing; this is the one path where the platform tells
 * it something.
 *
 * It exists because of where these boxes end up. BLE reaches a gateway only
 * while it is advertising, and an installed one never is — the pairing window
 * opens for two minutes after a three-second button press and closes again,
 * which is what stops a stranger in the car park re-provisioning a box on a
 * roof. That is the right trade, but it means a mounted gateway has exactly
 * one remote channel, and this is it.
 *
 * Deliberately narrow. Only settings that are safe to change without someone
 * present travel this way:
 *
 *   broker  — not a secret, and the change is reversible by the box itself.
 *
 * WiFi credentials deliberately do NOT. Two reasons, either sufficient: the
 * passphrase would transit our broker, which is the boundary BLE provisioning
 * exists to keep; and new credentials arriving over WiFi are self-defeating —
 * wrong ones drop the network they arrived on and strand the box behind a
 * button press it may be three metres up a wall.
 *
 * Topic is ndw/<gw>/gateway/<gw>/cmd, the downlink mirror of the telemetry
 * topic, so a gateway subscribes to exactly one thing addressed to itself.
 */

#pragma once

/** Records which gateway this box is, for the topic it listens on. */
void ndw_command_init(const char *gateway_eui);

/**
 * Subscribes to this gateway's command topic.
 *
 * Called after the MQTT client exists. Re-subscribes on every reconnect,
 * because a broker that dropped us has forgotten our subscriptions.
 */
void ndw_command_subscribe(void);

/**
 * Handles one command payload.
 *
 * Called from the MQTT event handler, which runs on the event loop — so this
 * parses and stores, and never blocks on the network. Anything that needs to
 * reconnect is left to the uplink's own machinery.
 */
void ndw_command_handle(const char *data, int len);

/** The topic this gateway takes commands on. Filled into `out`. */
void ndw_command_topic(char *out, int len);
