/*
 * Provisioning over the USB cable.
 *
 * A box is flashed over USB and then has to be told which network to join.
 * Doing that over the same cable means one connection for the whole job: the
 * browser holds the port open after writing the firmware, the board reboots
 * into it, and the conversation continues without the person unplugging
 * anything, pressing anything, or meeting a second pairing mechanism.
 *
 * The protocol is deliberately plain. One JSON object per line in, one per
 * line out, each reply prefixed so it can be told apart from log output on the
 * same stream:
 *
 *   {"cmd":"hello"}                        -> #NDW {"ok":true,"eui":...}
 *   {"cmd":"scan"}                         -> #NDW {"ok":true,"networks":[...]}
 *   {"cmd":"wifi","ssid":"..","password":".."}
 *                                          -> #NDW {"ok":true,"state":"connected"}
 *   {"cmd":"status"}                       -> #NDW {"ok":true,...}
 *
 * Sharing the stream with logs rather than silencing them is the point: a
 * board that fails to join is exactly when someone wants to see why, and a
 * transcript that holds both the reply and the driver's own account of the
 * failure is worth more than a clean one.
 */

#pragma once

/* The marker every reply carries. A reader ignores lines without it. */
#define NDW_REPLY_PREFIX "#NDW "

/*
 * Starts the task that reads the console.
 *
 * Call once, after the uplink exists — a status reply that cannot say whether
 * the broker is reachable is not worth much — and before any blocking join, so
 * the console gets answers while a stored network is still being tried.
 */
void ndw_provision_start(void);
