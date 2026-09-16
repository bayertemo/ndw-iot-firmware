/*
 * The provisioning core, shared between main.c and whatever is driving it.
 *
 * These were file-local to main.c while BLE was the only way in. They are not
 * BLE-shaped and never were — joining a network, proving credentials before
 * keeping them, and rendering what the box currently thinks is true are the
 * same operations whatever asked for them. What was BLE-shaped was the
 * envelope around them, and that has gone.
 *
 * Deliberately not a public interface: this is one firmware image split across
 * two translation units, not a library. Nothing outside this directory should
 * include it.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

/* WPA2 limits: 32 bytes of SSID, 63 of passphrase, plus terminators. */
#define SSID_MAX 33
#define PASS_MAX 64

/**
 * Joins a network, and says whether an address arrived.
 *
 * Blocks for up to twenty seconds. Association alone is not success — a box
 * that associates and never gets a lease is not on the network, and reporting
 * otherwise would send someone looking for a fault at the wrong end.
 *
 * Serialised internally: the boot path and the console can both reach it, and
 * the retry state underneath is shared. A second caller waits.
 */
bool ndw_wifi_join(const char *ssid, const char *password);

/**
 * Keeps credentials across a reboot.
 *
 * Only ever called after a join has proven them. Storing first would leave a
 * box that reboots into a network it has never reached, which is worse than
 * one that forgot: it looks configured.
 */
void ndw_store_credentials(const char *ssid, const char *pass);

/** Reads back what was stored. False if either half is missing. */
bool ndw_load_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len);

/**
 * Forgets the stored network.
 *
 * For a board being handed to someone else, or reflashed for a different site.
 * The flasher deliberately does not erase NVS — a reflash that silently
 * dropped a working network would be a support call — so forgetting is an
 * explicit act.
 */
void ndw_forget_credentials(void);

/**
 * Renders the current state as JSON, into a caller-supplied buffer.
 *
 * Returns what snprintf would: the length that was wanted, which may exceed
 * `len`. Pure apart from reading the radio and the uplink, so it can be called
 * from any task.
 */
int ndw_status_json(char *out, size_t len, const char *state);

/** The word describing where provisioning currently stands. */
const char *ndw_provision_state(void);

/** Sets the DHCP hostname from a human label. */
void ndw_set_hostname(const char *label);

/** This box's EUI-64, lowercase hex, sixteen characters. */
const char *ndw_eui(void);

/**
 * Taken by anything that drives the radio.
 *
 * ndw_wifi_join takes it itself. A scan has to take it explicitly, because the
 * driver refuses to scan during a join and a caller that waits is better than
 * one handed an error it cannot act on.
 */
void ndw_radio_lock(void);
void ndw_radio_unlock(void);

/*
 * The running firmware version.
 *
 * NDW_VERSION comes from the build (see main/CMakeLists.txt, which turns the
 * CMake variable into a compiler define — a plain -D of the variable does not
 * reach the code, which is how every board once reported 0.0.0-dev). A local
 * build has none and says so, rather than claiming a release number it does
 * not have: a bench board reporting "0.1.0" when it was built from an
 * uncommitted tree is a lie nobody can detect afterwards.
 */
#ifdef NDW_VERSION
#define NDW_FIRMWARE_VERSION NDW_VERSION
#else
#define NDW_FIRMWARE_VERSION "0.0.0-dev"
#endif
