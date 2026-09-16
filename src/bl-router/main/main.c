/*
 * NDW BL Router — ESP32-C3.
 *
 * What it does, in order:
 *
 *   1. Derives an EUI from the chip's factory MAC, so the box has a stable
 *      identity nobody has to program in.
 *   2. If WiFi credentials are already stored, joins and stays joined.
 *   3. Advertises the NDW provisioning service over BLE either way, so a
 *      technician can always re-provision a box that is on the wrong network.
 *   4. Accepts credentials written over BLE, tries them, reports the outcome
 *      on the status characteristic, and only stores them once they work.
 *
 * That last point is the one worth stating plainly: credentials are proved
 * before they are persisted. Storing first would mean a typo survives a
 * reboot, and the box comes back up retrying a password that was never right
 * — with no way in except BLE, which is exactly the state a technician cannot
 * diagnose from the outside.
 *
 * Once provisioned it scans continuously and relays what it hears. Everything
 * it sends — relayed readings and its own telemetry — goes through one queue
 * (ndw_queue.h) rather than straight to the broker, so the radio never waits
 * on the network and an outage holds messages instead of discarding them.
 */

#include <ctype.h>
#include <inttypes.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "esp_nimble_hci.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "cJSON.h"
#include "ndw_button.h"
#include "ndw_command.h"
#include "ndw_contract.h"
#include "ndw_queue.h"
#include "ndw_scan.h"
#include "ndw_telemetry.h"
#include "ndw_uplink.h"
#include "ndw_provision.h"
#include "ndw_router.h"
#include "ndw_status.h"

static const char *TAG = "ndw-router";

/*
 * Set by the build (-DNDW_VERSION). A local build has none, and says so
 * rather than claiming a release number it does not have — a bench board
 * reporting "0.1.0" when it was built from an uncommitted tree is a lie
 * nobody can detect afterwards.
 */
#ifndef NDW_VERSION
#define NDW_VERSION "0.0.0-dev"
#endif


/* Where proven credentials live. */
#define NVS_NAMESPACE "ndw"
#define NVS_KEY_SSID  "ssid"
#define NVS_KEY_PASS  "pass"

/* How long to wait for a join before calling it failed. */
#define JOIN_TIMEOUT_MS 20000

/*
 * Retries per attempt.
 *
 * A first association often fails on a busy band for reasons that have
 * nothing to do with the password, so one refusal is not evidence. Three is
 * enough to ride out noise without leaving a technician watching a spinner.
 */
#define JOIN_ATTEMPTS 3

static EventGroupHandle_t s_wifi_events;
#define WIFI_JOINED BIT0
#define WIFI_FAILED BIT1

/*
 * Serialises everything that drives the radio.
 *
 * The join path keeps its state in file statics — the attempt count, the last
 * failure, the reconfiguring guard — and none of it is per-caller. That was
 * safe while one task at a time could ask: BLE provisioning ran on the host
 * task, and the boot path had finished before the host task existed.
 *
 * The console broke that. app_main sits inside a join for up to twenty seconds
 * while rejoining a stored network, and the console task is answering commands
 * throughout. Two joins interleaved would share one attempt counter and one
 * failure cause, and report each other's outcome.
 *
 * A scan takes it too: the driver will not scan during a join, and a caller
 * that waits is better than one that gets an error it cannot act on.
 */
static SemaphoreHandle_t s_radio_lock;

static char s_eui[NDW_EUI_LEN + 1];

/* What this box calls itself on the network. Defaults to the EUI suffix. */
static char s_hostname[33];
static uint16_t s_status_handle;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static int s_join_attempts;

/*
 * Set while a join is being set up, cleared once the new config is in.
 *
 * esp_wifi_disconnect() raises STA_DISCONNECTED, and the handler's job is to
 * retry — so without this it reconnected using the *old* config, racing the
 * set_config on the next line and spending a retry on the network we were
 * leaving. A box given new credentials would keep trying the previous ones.
 */
static volatile bool s_reconfiguring;
/* Set from the disconnect reason, so the reported failure names a cause. */
static const char *s_last_failure = NDW_STATUS_FAILED;

static void ndw_advertise(void);
static void ndw_pairing_close(void);
static bool ndw_wifi_join_locked(const char *ssid, const char *password);

/*
 * How long a pairing window stays open.
 *
 * Long enough to open the console, scan, connect and type a passphrase;
 * short enough that a forgotten press does not leave the box discoverable
 * all afternoon. Pressing again restarts it.
 */
#define PAIRING_WINDOW_MS (2 * 60 * 1000)

/* Non-NULL while a window is open. */
static TimerHandle_t s_pairing_timer;
static bool s_pairing_open;

/* Whether the radio currently holds an IP. Decides what the light shows
   once a pairing window closes. */
static bool s_joined;

/*
 * The network this box is on, or is trying. Held in RAM so status reads do
 * not hit NVS, and so the console can show which SSID is configured without
 * being told — that is what lets it mask the fields rather than presenting
 * empty ones to a box that is already provisioned.
 *
 * The passphrase is deliberately not kept here. Nothing reads it back: it is
 * written to NVS once proven and handed to the driver, and a copy that can be
 * read over BLE would undo the point of never sending it to our API.
 */
static char s_ssid[SSID_MAX];

/* ---------------------------------------------------------------- identity */

/*
 * The EUI, derived from the factory MAC.
 *
 * A MAC is six bytes and an EUI-64 is eight, so the usual EUI-48 to EUI-64
 * expansion applies: split the OUI from the device id and insert fffe. That
 * keeps the vendor prefix meaningful and guarantees uniqueness, since the MAC
 * is already unique and burned in at manufacture.
 *
 * Nothing has to be programmed per unit, which matters: a provisioning step
 * that requires writing a serial number is a step that gets skipped.
 */
static void ndw_derive_eui(void)
{
    uint8_t mac[6] = {0};
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_BASE));

    snprintf(s_eui, sizeof(s_eui), "%02x%02x%02xfffe%02x%02x%02x", mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5]);

    ESP_LOGI(TAG, "eui %s", s_eui);
    /* Until a label arrives, the last six of the EUI — the same suffix the
       BLE advertisement uses, so the two agree. */
    snprintf(s_hostname, sizeof(s_hostname), "NDW-%s", s_eui + NDW_EUI_LEN - 6);
}

/*
 * What this box calls itself on the network.
 *
 * Defaults to the last six of the EUI, so an unnamed box is still
 * identifiable rather than appearing as "espressif". A label supplied at
 * provisioning replaces it: DHCP tables and router UIs are where people
 * actually look for a device, and "NDW-basement-riser" is findable in a way
 * that a hex identifier is not.
 */
/*
 * DHCP hostnames are not free text. RFC 1123 allows letters, digits and
 * hyphens, and nothing else — a label with a space or an apostrophe would be
 * rejected or silently mangled by the router, so it is folded here rather
 * than trusted.
 */
void ndw_set_hostname(const char *label)
{
    size_t out = 0;
    out += (size_t)snprintf(s_hostname, sizeof(s_hostname), "NDW-");

    bool last_was_dash = true;
    for (const char *c = label; *c != '\0' && out < sizeof(s_hostname) - 1; c++) {
        if (isalnum((unsigned char)*c)) {
            s_hostname[out++] = (char)tolower((unsigned char)*c);
            last_was_dash = false;
        } else if (!last_was_dash) {
            /* Runs of punctuation collapse to one hyphen rather than
               producing "a--b" or a trailing dash. */
            s_hostname[out++] = '-';
            last_was_dash = true;
        }
    }
    while (out > 0 && s_hostname[out - 1] == '-') {
        out--;
    }
    s_hostname[out] = '\0';

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != NULL) {
        esp_netif_set_hostname(netif, s_hostname);
    }
    ESP_LOGI(TAG, "hostname %s", s_hostname);
}

/* -------------------------------------------------------------------- wifi */

static void ndw_notify_status(const char *status);

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev = data;

        /*
         * Our own teardown, not a failed association. Retrying here would
         * reconnect to the network being replaced and consume an attempt.
         */
        if (s_reconfiguring) {
            return;
        }

        /*
         * Losing a network the box was actually on, rather than failing to
         * join one. Reported as an event because the gap it opens is the
         * thing worth explaining: without it, a box that dropped off at 2am
         * and came back at 6 is indistinguishable from one that was never
         * asked. The report carries uptime, so the server can place the
         * outage even though the box has no clock.
         *
         * Only on the transition. This handler runs once per retry, and
         * notifying each time would queue three reports for one failed join
         * — all of them saying the same thing.
         */
        if (s_joined) {
            s_joined = false;
            ndw_telemetry_notify(NDW_TELEMETRY_OFFLINE);
        }

        /*
         * The reason code is the only signal that separates "wrong password"
         * from "no such network" from "the AP is just busy". Without it every
         * failure reads the same to a technician, who then re-types a
         * password that was correct.
         */
        /*
         * Keep the most specific reason across attempts, not the last one.
         *
         * A first attempt often reports NO_AP_FOUND simply because the scan
         * has not settled, and a later one then reports the real problem. The
         * log that prompted this read "reason 201, retry 1" followed by
         * "reason 15" — no-such-network followed by wrong-passphrase — and
         * reporting whichever arrived last would have sent a technician to
         * check the wrong thing.
         *
         * An auth failure is evidence the AP was found and answered, so it
         * outranks not-found however the attempts are ordered.
         */
        switch (ev->reason) {
        case WIFI_REASON_NO_AP_FOUND:
            /* strcmp, not pointer comparison: these are string literals and
               the compiler is right that comparing their addresses is
               unspecified. */
            if (strcmp(s_last_failure, NDW_STATUS_BAD_AUTH) != 0) {
                s_last_failure = NDW_STATUS_NOT_FOUND;
            }
            break;
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            s_last_failure = NDW_STATUS_BAD_AUTH;
            break;
        default:
            /* Only when nothing more specific has been seen. */
            if (strcmp(s_last_failure, NDW_STATUS_NOT_FOUND) != 0 &&
                strcmp(s_last_failure, NDW_STATUS_BAD_AUTH) != 0) {
                s_last_failure = NDW_STATUS_FAILED;
            }
            break;
        }

        if (++s_join_attempts < JOIN_ATTEMPTS) {
            ESP_LOGW(TAG, "join failed (reason %d), retry %d", ev->reason, s_join_attempts);
            esp_wifi_connect();
            return;
        }

        ESP_LOGE(TAG, "join failed (reason %d), giving up", ev->reason);
        s_joined = false;
        xEventGroupSetBits(s_wifi_events, WIFI_FAILED);
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "joined, ip " IPSTR, IP2STR(&ev->ip_info.ip));
        s_joined = true;
        /* An event, not a wait for the next heartbeat: a join five minutes
           ago and a join just now mean different things. */
        ndw_telemetry_notify(NDW_TELEMETRY_NETWORK);
        /* Green unless a pairing window is open — the window is the more
           urgent thing to show while it lasts. */
        if (!s_pairing_open && s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
            ndw_status_set(NDW_LED_ONLINE);
        }
        xEventGroupSetBits(s_wifi_events, WIFI_JOINED);
    }
}

static void ndw_wifi_init(void)
{
    s_wifi_events = xEventGroupCreate();
    s_radio_lock = xSemaphoreCreateMutex();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    /*
     * RAM, not flash. The driver's own credential store would race with ours,
     * and we deliberately do not persist anything until it is proven.
     */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_start());
}

/*
 * Tries a set of credentials and waits for the outcome.
 *
 * Blocks, which is fine: it runs on the NimBLE host task after a write, and
 * the console is waiting on the status notification anyway. Returns true only
 * once an IP has actually arrived — association alone is not a working
 * network.
 */
/*
 * Taken by anything that drives the radio, and held across the whole
 * operation rather than around each driver call: the state being protected is
 * the retry bookkeeping between them, not the calls themselves.
 */
void ndw_radio_lock(void)
{
    if (s_radio_lock != NULL) {
        xSemaphoreTake(s_radio_lock, portMAX_DELAY);
    }
}

void ndw_radio_unlock(void)
{
    if (s_radio_lock != NULL) {
        xSemaphoreGive(s_radio_lock);
    }
}

/*
 * Where provisioning stands, in one word.
 *
 * Three states rather than two: a box that has never been told anything is
 * different from one that was told and could not get on, and a console that
 * cannot tell them apart would offer to retry credentials that do not exist.
 */
const char *ndw_provision_state(void)
{
    if (s_joined) {
        return NDW_STATUS_CONNECTED;
    }
    return s_ssid[0] != '\0' ? s_last_failure : "unprovisioned";
}

const char *ndw_eui(void)
{
    return s_eui;
}

void ndw_forget_credentials(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGE(TAG, "could not open nvs to forget credentials");
        return;
    }

    /* Erasing the keys rather than the namespace: the press counter and the
       provisioned broker live here too, and forgetting a network is not the
       same as forgetting everything. */
    nvs_erase_key(nvs, NVS_KEY_SSID);
    nvs_erase_key(nvs, NVS_KEY_PASS);
    nvs_commit(nvs);
    nvs_close(nvs);

    s_ssid[0] = '\0';
    ESP_LOGI(TAG, "credentials forgotten");
}

bool ndw_wifi_join(const char *ssid, const char *password)
{
    ndw_radio_lock();
    bool joined = ndw_wifi_join_locked(ssid, password);
    ndw_radio_unlock();
    return joined;
}

/* The join itself, with the lock already held. */
static bool ndw_wifi_join_locked(const char *ssid, const char *password)
{
    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password));

    s_join_attempts = 0;
    s_last_failure = NDW_STATUS_FAILED;
    xEventGroupClearBits(s_wifi_events, WIFI_JOINED | WIFI_FAILED);

    /*
     * Not ESP_ERROR_CHECK: disconnect fails when there is nothing to
     * disconnect from, which is the normal case for a box being provisioned
     * for the first time. Aborting the whole board over it would turn "no
     * network yet" into a reboot loop.
     */
    /*
     * Re-applied here because esp_netif_set_hostname only sticks while the
     * interface exists, and the name may have been set before it came up.
     */
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != NULL) {
        esp_netif_set_hostname(netif, s_hostname);
    }

    s_reconfiguring = true;
    esp_wifi_disconnect();

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    s_reconfiguring = false;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not set the wifi config: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not start the join: %s", esp_err_to_name(err));
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_JOINED | WIFI_FAILED, pdFALSE,
                                           pdFALSE, pdMS_TO_TICKS(JOIN_TIMEOUT_MS));

    if ((bits & WIFI_JOINED) != 0) {
        return true;
    }

    /* A timeout with neither bit set is its own failure, not a success. */
    if ((bits & WIFI_FAILED) == 0) {
        ESP_LOGE(TAG, "join to '%s' timed out after %dms", ssid, JOIN_TIMEOUT_MS);
        s_last_failure = NDW_STATUS_FAILED;
    }
    return false;
}

/* --------------------------------------------------------------- storage */

bool ndw_load_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }

    bool ok = nvs_get_str(nvs, NVS_KEY_SSID, ssid, &ssid_len) == ESP_OK &&
              nvs_get_str(nvs, NVS_KEY_PASS, pass, &pass_len) == ESP_OK;

    nvs_close(nvs);
    return ok;
}

void ndw_store_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGE(TAG, "could not open nvs to store credentials");
        return;
    }

    ESP_ERROR_CHECK(nvs_set_str(nvs, NVS_KEY_SSID, ssid));
    ESP_ERROR_CHECK(nvs_set_str(nvs, NVS_KEY_PASS, pass));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    ESP_LOGI(TAG, "credentials stored");
}

/* ------------------------------------------------------------------- gatt */

/*
 * Renders the current state as the JSON the contract describes.
 *
 * Assembled by hand rather than with cJSON: five fields of known shape, and
 * the allocation-free version is both shorter and cannot fail part-way
 * through a notify.
 */
int ndw_status_json(char *out, size_t len, const char *state)
{
    char ip[16] = "";
    int8_t rssi = 0;

    if (s_joined) {
        esp_netif_ip_info_t info;
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif != NULL && esp_netif_get_ip_info(netif, &info) == ESP_OK) {
            snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
        }
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            rssi = ap.rssi;
        }
    }

    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);

    /*
     * The uplink, reported beside the WiFi details.
     *
     * A box on WiFi that publishes nothing is the failure an installer cannot
     * diagnose: the light is green, the network is fine, and the problem is a
     * broker address or a certificate they cannot see. Showing which broker it
     * is using, whether it is connected, why not, and how much is waiting to
     * be sent turns that into something readable while they are still there.
     *
     * Queue depth is the part that distinguishes "nothing to send" from
     * "sending is failing" — a growing queue on a connected box means the link
     * is up but not keeping up.
     */
    return snprintf(out, len,
                    "{\"state\":\"%s\",\"eui\":\"%s\",\"firmware\":\"%s\","
                    "\"ssid\":\"%s\",\"ip\":\"%s\","
                    "\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"rssi\":%d,"
                    "\"uplink\":{\"broker\":\"%s\",\"connected\":%s,"
                    "\"error\":\"%s\",\"queued\":%u,\"sent\":%lu}}",
                    /*
                     * The EUI and the version, because the console needs both
                     * and neither was here. It claims the box by the EUI, and
                     * the version is the one question a flasher cannot answer
                     * for itself: whether the image that booted is the image
                     * that was just written.
                     */
                    state, s_eui, NDW_FIRMWARE_VERSION, s_ssid, ip,
                    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], rssi,
                    ndw_uplink_broker(), ndw_uplink_connected() ? "true" : "false",
                    ndw_uplink_last_error(), (unsigned)ndw_queue_depth(),
                    (unsigned long)ndw_scan_count());
}

static void ndw_notify_status(const char *status)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || s_status_handle == 0) {
        return;
    }

    /* Fits the broker URL (up to 128) plus the rest, and stays under the
       517-byte ATT MTU Chrome negotiates. */
    char body[384];
    int n = ndw_status_json(body, sizeof(body), status);
    if (n <= 0) {
        return;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(body, (uint16_t)n);
    if (om != NULL) {
        ble_gatts_notify_custom(s_conn_handle, s_status_handle, om);
    }
}

static int on_eui_read(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    (void)attr;
    (void)arg;

    /*
     * ASCII rather than the eight raw bytes. Both are valid per the console's
     * parser, but only the text form is unambiguous: eight raw bytes that all
     * land on ASCII hex digits are indistinguishable from a truncated string,
     * and the console rejects that case rather than guessing.
     */
    return os_mbuf_append(ctxt->om, s_eui, NDW_EUI_LEN) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

/*
 * Current state, on demand.
 *
 * The console reads this before showing the form: a box that already has an
 * SSID gets masked fields and an explicit override, rather than empty ones
 * that invite retyping credentials the box is already using.
 */
static int on_status_read(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt,
                          void *arg)
{
    (void)conn;
    (void)attr;
    (void)arg;

    const char *state = s_joined         ? NDW_STATUS_CONNECTED
                        : s_ssid[0]      ? NDW_STATUS_FAILED
                                         : "unprovisioned";

    /* Fits the broker URL (up to 128) plus the rest, and stays under the
       517-byte ATT MTU Chrome negotiates. */
    char body[384];
    int n = ndw_status_json(body, sizeof(body), state);
    if (n <= 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    return os_mbuf_append(ctxt->om, body, (uint16_t)n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int on_wifi_write(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    (void)attr;
    (void)arg;

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    /* SSID and passphrase plus JSON overhead; anything larger is not ours. */
    if (len == 0 || len > 256) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    char body[257] = {0};
    if (ble_hs_mbuf_to_flat(ctxt->om, body, sizeof(body) - 1, &len) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    cJSON *doc = cJSON_Parse(body);
    if (doc == NULL) {
        ESP_LOGW(TAG, "provisioning payload was not json");
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(doc, "ssid");
    const cJSON *pass = cJSON_GetObjectItemCaseSensitive(doc, "password");
    /*
     * Optional. The label is a cloud attribute the box has no need for, with
     * one exception: it makes a far better DHCP hostname than a hex EUI, so
     * a router shows up on the network as something a person recognises.
     */
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(doc, "name");
    /*
     * Optional, and provisioned for the same reason the SSID is: where the
     * broker lives is a property of the site, not of the firmware.
     */
    const cJSON *broker = cJSON_GetObjectItemCaseSensitive(doc, "broker");

    if (!cJSON_IsString(ssid) || ssid->valuestring[0] == '\0' || !cJSON_IsString(pass)) {
        ESP_LOGW(TAG, "provisioning payload missing ssid or password");
        cJSON_Delete(doc);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    char ssid_buf[SSID_MAX];
    char pass_buf[PASS_MAX];
    strlcpy(ssid_buf, ssid->valuestring, sizeof(ssid_buf));
    strlcpy(pass_buf, pass->valuestring, sizeof(pass_buf));
    if (cJSON_IsString(name) && name->valuestring[0] != '\0') {
        ndw_set_hostname(name->valuestring);
    }
    if (cJSON_IsString(broker) && broker->valuestring[0] != '\0') {
        ndw_uplink_set_broker(broker->valuestring);
    }
    cJSON_Delete(doc);

    strlcpy(s_ssid, ssid_buf, sizeof(s_ssid));
    ESP_LOGI(TAG, "provisioning for ssid '%s'", ssid_buf);
    ndw_notify_status(NDW_STATUS_CONNECTING);
    ndw_status_set(NDW_LED_JOINING);

    if (ndw_wifi_join(ssid_buf, pass_buf)) {
        /* Proven, so now it is worth keeping across a reboot. */
        ndw_store_credentials(ssid_buf, pass_buf);
        ndw_notify_status(NDW_STATUS_CONNECTED);
        ndw_status_set(NDW_LED_ONLINE);
    } else {
        ndw_notify_status(s_last_failure);
        ndw_status_set(NDW_LED_FAILED);
    }

    return 0;
}

static const struct ble_gatt_svc_def s_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &(ble_uuid128_t)NDW_SERVICE_UUID.u,
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {
                    .uuid = &(ble_uuid128_t)NDW_EUI_UUID.u,
                    .access_cb = on_eui_read,
                    .flags = BLE_GATT_CHR_F_READ,
                },
                {
                    .uuid = &(ble_uuid128_t)NDW_WIFI_UUID.u,
                    .access_cb = on_wifi_write,
                    /*
                     * With response: the console wants to know the write
                     * landed, and this one is worth waiting on.
                     */
                    .flags = BLE_GATT_CHR_F_WRITE,
                },
                {
                    .uuid = &(ble_uuid128_t)NDW_STATUS_UUID.u,
                    .access_cb = on_status_read,
                    .val_handle = &s_status_handle,
                    .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ,
                },
                {0},
            },
    },
    {0},
};

/* -------------------------------------------------------------------- gap */

static int on_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "console connected");
            ndw_status_set(NDW_LED_LINKED);
        } else if (s_pairing_open) {
            /* Failed to establish; the window is still open, so keep
               advertising rather than making the technician press again. */
            ndw_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "console disconnected");
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        /*
         * Only back to advertising if the window has not expired. Previously
         * this restarted unconditionally, which meant one press made the box
         * discoverable forever — the window would have been decorative.
         */
        if (s_pairing_open) {
            ndw_advertise();
        } else if (ndw_status_get() == NDW_LED_FAILED) {
            /*
             * Hold a failure on the light after the console goes away. The
             * console disconnects the moment provisioning finishes, and
             * dropping straight to idle threw away the one signal a
             * technician standing at the box could still read.
             */
            ESP_LOGI(TAG, "holding the failure on the light");
        } else {
            ndw_status_set(s_joined ? NDW_LED_ONLINE : NDW_LED_IDLE);
        }
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_pairing_open) {
            ndw_advertise();
        }
        return 0;

    default:
        return 0;
    }
}

/*
 * Shuts the pairing window.
 *
 * Stops advertising unless a console is mid-session — cutting a technician
 * off at the two-minute mark while they are typing a passphrase would be its
 * own bug. The disconnect handler closes things properly once they are done.
 */
static void ndw_pairing_close(void)
{
    s_pairing_open = false;

    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGI(TAG, "pairing window expired, but a console is connected");
        return;
    }

    ble_gap_adv_stop();
    ESP_LOGI(TAG, "pairing window closed");
    ndw_led_t resting = s_joined ? NDW_LED_ONLINE : NDW_LED_IDLE;
    ndw_status_set(resting);
    ndw_button_set_idle_state(resting);
}

static void on_pairing_timeout(TimerHandle_t timer)
{
    (void)timer;
    ndw_pairing_close();
}

/*
 * Opens a pairing window. Called from the button task.
 *
 * Pressing again while one is open restarts the clock rather than opening a
 * second: a technician who is not sure whether the first press registered
 * should be able to press again without making things worse.
 */
static void ndw_pairing_open(void)
{
    if (s_pairing_timer == NULL) {
        s_pairing_timer = xTimerCreate("ndw-pair", pdMS_TO_TICKS(PAIRING_WINDOW_MS), pdFALSE,
                                       NULL, on_pairing_timeout);
        if (s_pairing_timer == NULL) {
            ESP_LOGE(TAG, "could not create the pairing timer");
            return;
        }
    }

    bool was_open = s_pairing_open;
    s_pairing_open = true;
    xTimerReset(s_pairing_timer, pdMS_TO_TICKS(100));

    if (!was_open && s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        ndw_advertise();
    }
    ndw_status_set(NDW_LED_PAIRING);
    ESP_LOGI(TAG, "pairing window open for %ds", PAIRING_WINDOW_MS / 1000);
}

static void ndw_advertise(void)
{
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    /*
     * The name carries the last six EUI characters. Two boxes on one bench
     * are otherwise identical in the browser's chooser, and the console shows
     * this name before it can read the EUI over GATT.
     */
    static char name[24];
    snprintf(name, sizeof(name), "NDW-%s", s_eui + NDW_EUI_LEN - 6);
    ble_svc_gap_device_name_set(name);
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields failed: %d", rc);
        return;
    }

    /*
     * The 128-bit service UUID goes in the scan response, not the advertising
     * packet. It alone is 16 of the 31 available bytes, and with the name and
     * flags it does not fit — but the console filters on it, so it has to be
     * discoverable. Chrome reads scan response data for filter matching.
     */
    struct ble_hs_adv_fields scan_rsp = {0};
    static const ble_uuid128_t service_uuid = NDW_SERVICE_UUID;
    scan_rsp.uuids128 = (ble_uuid128_t *)&service_uuid;
    scan_rsp.num_uuids128 = 1;
    scan_rsp.uuids128_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&scan_rsp);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_rsp_set_fields failed: %d", rc);
        return;
    }

    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &params, on_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_start failed: %d", rc);
    }
}

/*
 * A beacon arrived.
 *
 * Runs on the NimBLE host task, so it does no I/O: publish() queues the frame
 * and returns, and the sender task puts it on the wire. A socket write here
 * would stall the radio behind the network, dropping the beacons that arrive
 * while it waited.
 */
static void on_frame(const ndw_frame_t *frame)
{
    ESP_LOGI(TAG, "beacon %s kind %u count %" PRIu32 " rssi %d", frame->eui, frame->kind,
             frame->counter, frame->rssi);
    ndw_uplink_publish(frame);
    /* One pulse per frame, overlaid on whatever the light is showing, so a
       relaying router still reads as online between beacons. */
    ndw_status_pulse();
}

static void on_host_sync(void)
{
    ESP_ERROR_CHECK(ble_hs_util_ensure_addr(0));
    /*
     * Deliberately does not advertise. The box is invisible until someone
     * holds the button, so an installed gateway cannot be re-provisioned by
     * a stranger with a laptop in the car park. That is the whole point of
     * the window, and it costs physical access to recover a box.
     */
    /*
     * Scanning starts here and never stops. Advertising is the exceptional
     * state — a two-minute pairing window — while listening is what the box
     * is for, so the two share the radio with listening as the default.
     */
    ndw_scan_start(on_frame);
    ndw_telemetry_start(s_eui);

    ESP_LOGI(TAG, "ready — hold the button for 3s to pair");
    ndw_led_t resting = s_joined ? NDW_LED_ONLINE : NDW_LED_IDLE;
    ndw_status_set(resting);
    ndw_button_set_idle_state(resting);
}

static void on_host_reset(int reason)
{
    ESP_LOGW(TAG, "nimble reset: %d", reason);
}

static void ndw_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ------------------------------------------------------------------- main */

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* A partition from an older layout; start clean rather than refuse. */
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_LOGI(TAG, "NDW BL Router %s", NDW_VERSION);

    ndw_status_init();
    ndw_derive_eui();
    ndw_wifi_init();
    /*
     * Before the uplink, because push() is a no-op until the queue exists and
     * a frame decoded in the gap would be silently lost. Nothing sends yet —
     * the sender waits on an empty queue.
     */
    ndw_queue_start();
    // Before the uplink, so the subscribe that runs on connect knows which
    // gateway it is listening for.
    ndw_command_init(s_eui);
    ndw_uplink_init(s_eui);

    /*
     * Before the rejoin below, which blocks for up to twenty seconds.
     *
     * The browser opens the port as soon as the board reboots into this image,
     * and a console that answered nothing for the first twenty seconds would
     * look like a board that had not booted. The task takes the radio lock
     * like any other caller, so a command that needs the radio waits for the
     * rejoin rather than racing it.
     */
    ndw_provision_start();

    /*
     * Rejoin a known network before BLE comes up, so a box that has been
     * provisioned once is back online without anyone touching it.
     */
    char ssid[SSID_MAX] = {0};
    char pass[PASS_MAX] = {0};
    if (ndw_load_credentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
        strlcpy(s_ssid, ssid, sizeof(s_ssid));
        ESP_LOGI(TAG, "rejoining stored network '%s'", ssid);
        if (!ndw_wifi_join(ssid, pass)) {
            /*
             * Not fatal, and deliberately not cause to forget them: the AP may
             * simply be down. BLE is up regardless, so the network can be
             * changed if it turns out to be gone for good.
             */
            ESP_LOGW(TAG, "stored credentials did not work this time");
        }
    } else {
        ESP_LOGI(TAG, "no stored credentials, waiting to be provisioned");
    }

    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.sync_cb = on_host_sync;
    ble_hs_cfg.reset_cb = on_host_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    ESP_ERROR_CHECK(ble_gatts_count_cfg(s_services));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(s_services));

    nimble_port_freertos_init(ndw_host_task);

    /* Last, so a press cannot open a window before the host is up. */
    ndw_button_init(ndw_pairing_open);
}
