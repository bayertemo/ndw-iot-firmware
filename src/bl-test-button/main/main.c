/*
 * NDW Test Button — ESP32-C3.
 *
 * Stands in for a meter. It broadcasts a counter that goes up by one every
 * time the button is pressed, and nothing else: no pairing, no connection, no
 * configuration. Press it, and a reading appears wherever the network is
 * listening.
 *
 * That is the whole point of it. A gateway, an ingest pipeline and a
 * dashboard are hard to trust until something real has travelled end to end,
 * and a button whose count you can see with your own eyes is the cheapest
 * honest test of that path. If the number on the screen matches the number of
 * times you pressed, the whole chain works.
 *
 * Connectionless by design. A meter sends a small reading occasionally and
 * sleeps; opening a connection for each one would cost more energy than the
 * reading is worth, and would mean the gateway had to know every device
 * before it could hear any of them.
 */

#include <inttypes.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#include "ndw_beacon.h"

static const char *TAG = "ndw-button";

#ifndef NDW_VERSION
#define NDW_VERSION "0.0.0-dev"
#endif

#define BUTTON_GPIO 9
#define LED_GPIO 8

/*
 * Debounce interval.
 *
 * A tactile switch bounces for a few milliseconds on contact, and each bounce
 * looks like a press to a naive reader — the counter would jump by three or
 * four for one push, which in a device whose entire job is counting would be
 * the only bug that matters. Polling at 20ms is longer than the bounce and
 * far shorter than a human finger.
 */
#define POLL_MS 20

/*
 * How long the counter is broadcast after a press.
 *
 * Long enough that a gateway scanning on a duty cycle cannot miss it — a
 * scanner listening 10% of the time would otherwise drop most readings — and
 * short enough that the radio is quiet the rest of the time, which is what a
 * battery device would need.
 */
#define BROADCAST_MS 10000

/* Where the count lives across a reboot. */
#define NVS_NAMESPACE "ndw"
#define NVS_KEY_COUNT "count"

static uint8_t s_eui[8];
static uint32_t s_counter;
static int64_t s_broadcast_until;

/* ---------------------------------------------------------------- identity */

/*
 * The EUI, from the factory MAC.
 *
 * Same derivation the router uses — the EUI-48 to EUI-64 expansion, inserting
 * fffe — so a button and a router on the same bench produce identifiers of
 * the same shape, and one rule explains both.
 */
static void derive_eui(void)
{
    uint8_t mac[6] = {0};
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_BASE));

    s_eui[0] = mac[0];
    s_eui[1] = mac[1];
    s_eui[2] = mac[2];
    s_eui[3] = 0xff;
    s_eui[4] = 0xfe;
    s_eui[5] = mac[3];
    s_eui[6] = mac[4];
    s_eui[7] = mac[5];

    ESP_LOGI(TAG, "eui %02x%02x%02xfffe%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4],
             mac[5]);
}

/* ----------------------------------------------------------------- counter */

/*
 * The counter survives a reboot.
 *
 * Not a nicety. A counter that restarted at zero would look to the pipeline
 * exactly like a replayed frame — ingest dedups on (device, counter), so
 * every reading after a power cycle would be silently dropped as one it had
 * already seen. Persisting it is what makes the count mean "presses since the
 * device existed" rather than "presses since it last had power".
 */
static void load_counter(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    if (nvs_get_u32(nvs, NVS_KEY_COUNT, &s_counter) != ESP_OK) {
        s_counter = 0;
    }
    nvs_close(nvs);
}

static void store_counter(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGW(TAG, "could not open nvs; the count will not survive a reboot");
        return;
    }
    nvs_set_u32(nvs, NVS_KEY_COUNT, s_counter);
    nvs_commit(nvs);
    nvs_close(nvs);
}

/* ------------------------------------------------------------------ beacon */

static void advertise(void)
{
    uint8_t payload[NDW_BEACON_LEN] = {0};

    payload[NDW_OFF_COMPANY] = NDW_COMPANY_ID & 0xff;
    payload[NDW_OFF_COMPANY + 1] = (NDW_COMPANY_ID >> 8) & 0xff;
    payload[NDW_OFF_VERSION] = NDW_BEACON_VERSION;
    payload[NDW_OFF_KIND] = NDW_KIND_TEST_BUTTON;

    /* Big-endian: this is the identifier printed on the box, and reversing it
       here would make every log disagree with the label. */
    memcpy(&payload[NDW_OFF_EUI], s_eui, sizeof(s_eui));

    for (int i = 0; i < 4; i++) {
        payload[NDW_OFF_COUNTER + i] = (uint8_t)((s_counter >> (8 * i)) & 0xff);
    }

    uint32_t uptime = (uint32_t)(esp_timer_get_time() / 1000000);
    for (int i = 0; i < 4; i++) {
        payload[NDW_OFF_UPTIME + i] = (uint8_t)((uptime >> (8 * i)) & 0xff);
    }

    /* No fuel gauge on a dev board, and a made-up number is worse than an
       admitted gap — a dashboard would chart it as if it meant something. */
    payload[NDW_OFF_BATTERY] = NDW_BATTERY_UNKNOWN;

    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.mfg_data = payload;
    fields.mfg_data_len = sizeof(payload);

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields failed: %d", rc);
        return;
    }

    /*
     * The name goes in the scan response, not the advertisement: 24 bytes of
     * payload plus flags leaves too little room, and the payload is the part
     * that carries the reading.
     *
     * Set directly rather than through ble_svc_gap_device_name_set, which
     * belongs to the peripheral role — compiled out here, because nothing
     * connects to a broadcaster and the GATT name would never be read.
     */
    static char name[24];
    snprintf(name, sizeof(name), "NDW-btn-%02x%02x%02x", s_eui[5], s_eui[6], s_eui[7]);

    struct ble_hs_adv_fields scan_rsp = {0};
    scan_rsp.name = (uint8_t *)name;
    scan_rsp.name_len = strlen(name);
    scan_rsp.name_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&scan_rsp);

    struct ble_gap_adv_params params = {0};
    /*
     * Non-connectable, undirected. There is nothing to connect to — the whole
     * payload is in the advertisement — and accepting connections would
     * invite a pairing attempt the device has no answer for.
     */
    params.conn_mode = BLE_GAP_CONN_MODE_NON;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    ble_gap_adv_stop();
    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &params, NULL, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_start failed: %d", rc);
    }
}

/* ------------------------------------------------------------------ button */

static void button_task(void *arg)
{
    (void)arg;

    bool was_down = false;
    int stable = 0;

    for (;;) {
        bool down = gpio_get_level(BUTTON_GPIO) == 0;

        /*
         * Counts on the press, not the release: a button that responds when
         * you push it feels immediate, and one that waits for release feels
         * broken. Two consecutive polls agreeing is what filters the contact
         * bounce that would otherwise count one push several times.
         */
        if (down == was_down) {
            stable = 0;
        } else if (++stable >= 2) {
            was_down = down;
            stable = 0;
            if (down) {
                s_counter++;
                store_counter();
                s_broadcast_until = esp_timer_get_time() + (int64_t)BROADCAST_MS * 1000;
                ESP_LOGI(TAG, "press %" PRIu32, s_counter);
                advertise();
                gpio_set_level(LED_GPIO, 1);
            }
        }

        /* The light follows the broadcast window, so it shows what the radio
           is doing rather than just that a press registered. */
        if (s_broadcast_until != 0 && esp_timer_get_time() > s_broadcast_until) {
            s_broadcast_until = 0;
            ble_gap_adv_stop();
            gpio_set_level(LED_GPIO, 0);
            ESP_LOGI(TAG, "quiet");
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

/* -------------------------------------------------------------------- main */

static void on_sync(void)
{
    ESP_ERROR_CHECK(ble_hs_util_ensure_addr(0));
    ESP_LOGI(TAG, "ready — press the button to send a reading");
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_LOGI(TAG, "NDW Test Button %s", NDW_VERSION);

    derive_eui();
    load_counter();
    ESP_LOGI(TAG, "count %" PRIu32 " (restored)", s_counter);

    gpio_config_t button = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&button));

    /*
     * A plain level on GPIO8, not the addressable LED the router drives. This
     * firmware has no colour to express — the light is on while the radio is
     * broadcasting and off otherwise — and pulling in the RMT driver for that
     * would be more moving parts than the signal is worth.
     */
    gpio_config_t led = {
        .pin_bit_mask = 1ULL << LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&led));
    gpio_set_level(LED_GPIO, 0);

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);

    xTaskCreate(button_task, "ndw-btn", 3072, NULL, 3, NULL);
}
