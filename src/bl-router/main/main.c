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
 * MQTT is deliberately not here yet. Getting a box onto WiFi and answering to
 * its EUI is the whole of what the console's claim flow needs, and it is
 * enough to prove that path against real silicon.
 */

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
#include "ndw_contract.h"
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

/* WPA2 limits: 32 bytes of SSID, 63 of passphrase, plus terminators. */
#define SSID_MAX 33
#define PASS_MAX 64

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

static char s_eui[NDW_EUI_LEN + 1];
static uint16_t s_status_handle;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static int s_join_attempts;
/* Set from the disconnect reason, so the reported failure names a cause. */
static const char *s_last_failure = NDW_STATUS_FAILED;

static void ndw_advertise(void);
static void ndw_pairing_close(void);

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
         * The reason code is the only signal that separates "wrong password"
         * from "no such network" from "the AP is just busy". Without it every
         * failure reads the same to a technician, who then re-types a
         * password that was correct.
         */
        switch (ev->reason) {
        case WIFI_REASON_NO_AP_FOUND:
            s_last_failure = NDW_STATUS_NOT_FOUND;
            break;
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            s_last_failure = NDW_STATUS_BAD_AUTH;
            break;
        default:
            s_last_failure = NDW_STATUS_FAILED;
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
static bool ndw_wifi_join(const char *ssid, const char *password)
{
    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password));

    s_join_attempts = 0;
    s_last_failure = NDW_STATUS_FAILED;
    xEventGroupClearBits(s_wifi_events, WIFI_JOINED | WIFI_FAILED);

    ESP_ERROR_CHECK(esp_wifi_disconnect());
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_connect());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_JOINED | WIFI_FAILED, pdFALSE,
                                           pdFALSE, pdMS_TO_TICKS(JOIN_TIMEOUT_MS));

    /* A timeout with neither bit set is its own failure, not a success. */
    return (bits & WIFI_JOINED) != 0;
}

/* --------------------------------------------------------------- storage */

static bool ndw_load_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
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

static void ndw_store_credentials(const char *ssid, const char *pass)
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

static void ndw_notify_status(const char *status)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || s_status_handle == 0) {
        return;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(status, strlen(status));
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

    if (!cJSON_IsString(ssid) || ssid->valuestring[0] == '\0' || !cJSON_IsString(pass)) {
        ESP_LOGW(TAG, "provisioning payload missing ssid or password");
        cJSON_Delete(doc);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    char ssid_buf[SSID_MAX];
    char pass_buf[PASS_MAX];
    strlcpy(ssid_buf, ssid->valuestring, sizeof(ssid_buf));
    strlcpy(pass_buf, pass->valuestring, sizeof(pass_buf));
    cJSON_Delete(doc);

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
                    .access_cb = on_eui_read, /* readable, but only notify matters */
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

static void on_host_sync(void)
{
    ESP_ERROR_CHECK(ble_hs_util_ensure_addr(0));
    /*
     * Deliberately does not advertise. The box is invisible until someone
     * holds the button, so an installed gateway cannot be re-provisioned by
     * a stranger with a laptop in the car park. That is the whole point of
     * the window, and it costs physical access to recover a box.
     */
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
     * Rejoin a known network before BLE comes up, so a box that has been
     * provisioned once is back online without anyone touching it.
     */
    char ssid[SSID_MAX] = {0};
    char pass[PASS_MAX] = {0};
    if (ndw_load_credentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
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
