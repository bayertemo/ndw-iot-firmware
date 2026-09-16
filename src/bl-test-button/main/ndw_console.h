/*
 * The console this device answers on, over the cable that flashed it.
 *
 * The same line protocol the router speaks — one JSON object per line each
 * way, replies prefixed so they can be told apart from log output sharing the
 * stream — but a much smaller vocabulary. A sensor has no network to join and
 * no broker to reach; the only thing anybody needs to tell it is the key it
 * authenticates its readings with.
 *
 *   {"cmd":"hello"}                    -> #NDW {"ok":true,"eui":..,"provisioned":..}
 *   {"cmd":"status"}                   -> #NDW {"ok":true,...}
 *   {"cmd":"key","key":"<32 hex>"}     -> #NDW {"ok":true,"provisioned":true}
 *   {"cmd":"press"}                    -> #NDW {"ok":true,"count":..}
 *   {"cmd":"forget"}                   -> #NDW {"ok":true,"provisioned":false}
 *
 * The key arrives from the platform, which derived it from this device's EUI
 * and a master nobody else holds. It goes down the cable and into NVS, and is
 * never read back out — a console that could ask for it would undo the point
 * of deriving it server-side.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The version idf.py was told to build, in one place. Defined by CMake when
   the release workflow passes it and left as a marker when a developer runs
   idf.py by hand — which is the honest answer for a build nobody tagged. */
#ifdef NDW_VERSION
#define NDW_FIRMWARE_VERSION NDW_VERSION
#else
#define NDW_FIRMWARE_VERSION "0.0.0-dev"
#endif

/** The marker every reply carries. A reader ignores lines without it. */
#define NDW_REPLY_PREFIX "#NDW "

/** AES-128, so sixteen bytes. */
#define NDW_KEY_LEN 16

/** Starts the task that reads the console. Call once, after NVS is up. */
void ndw_console_start(void);

/**
 * The provisioned key, or NULL when this device has none.
 *
 * A device without one broadcasts in the clear on the older beacon version,
 * which is what lets a board work the moment it is flashed and before anybody
 * has claimed it.
 */
const uint8_t *ndw_console_key(void);

/** Whether a key has been provisioned. */
bool ndw_console_provisioned(void);
