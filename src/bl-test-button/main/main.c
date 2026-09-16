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

#include "mbedtls/ccm.h"

#include "ndw_beacon.h"
#include "ndw_console.h"

static const char *TAG = "ndw-button";

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
 * How many agreeing polls make a level real.
 *
 * Five at 20ms is 100ms of the line holding still — longer than any contact
 * bounce and far shorter than a finger. Two was the original value and was
 * thin: it believed a level after 40ms, which a noisy line can supply.
 */
#define STABLE_POLLS 5

/*
 * The shortest gap between two presses that can both be real.
 *
 * Nobody presses a button more than a few times a second, so anything closer
 * than this is the hardware talking rather than a person. It is the backstop
 * that makes over-counting impossible rather than merely unlikely: even if
 * the level filter is defeated, a second count inside this window is refused.
 *
 * Worth having because the failure it prevents is unrecoverable. The counter
 * is the CCM nonce, so a count that races ahead of the presses burns those
 * numbers — the platform will not accept them again, and the device cannot go
 * back.
 *
 * A board with no button wired to BUTTON_GPIO will float and can log presses
 * that nobody made. This does not fix that, and is not meant to: a floating
 * input is a wiring fault, and the honest signal is that the count climbs
 * while the board sits untouched.
 */
#define PRESS_GAP_MS 250

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
/* The same identifier as text, which is what the console reports and what the
   platform registers. Kept beside the bytes rather than formatted on demand:
   both forms are read often and neither ever changes. */
static char s_eui_text[17];
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

    snprintf(s_eui_text, sizeof(s_eui_text), "%02x%02x%02xfffe%02x%02x%02x", mac[0], mac[1],
             mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "eui %s", s_eui_text);
}

/*
 * Read by the console, which reports both and owns neither.
 *
 * The console is a separate module because it has a separate job — a cable,
 * a line protocol, a key store — and it should not be reaching into this
 * file's statics to do it.
 */
const char *ndw_device_eui(void)
{
    return s_eui_text;
}

uint32_t ndw_device_counter(void)
{
    return s_counter;
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

/*
 * Encrypts the reading in place, and appends the tag.
 *
 * The header stays readable and is fed in as associated data, so it is
 * authenticated without being hidden: a router filters on it holding no key,
 * and the platform reads the EUI from it to know whose key to try. Changing
 * one byte of it — relabelling a captured frame as a different device —
 * invalidates the tag, so "readable" does not mean "editable".
 *
 * The nonce is the EUI and the counter, read straight out of the header. CCM
 * needs a value that never repeats under one key, and that pair is exactly
 * that: unique per device, and unique per frame because the counter is
 * monotonic and persisted across reboots. Both are already on the wire, so
 * the nonce costs no bytes.
 *
 * The counter stays in the clear rather than being encrypted with the rest.
 * It has to: it is the nonce, so a receiver needs it before it can decrypt,
 * and the router keys its repeat suppression on it while holding no key at
 * all. Authenticated, not hidden — which is the right trade for a number that
 * says how many times a button was pressed.
 *
 * Returns false when the board has no key, which is not a failure: it is an
 * unclaimed board, and it falls back to broadcasting version 1 in the clear.
 */
static bool seal_payload(uint8_t *payload)
{
    const uint8_t *key = ndw_console_key();
    if (key == NULL) {
        return false;
    }

    mbedtls_ccm_context ccm;
    mbedtls_ccm_init(&ccm);

    bool ok = mbedtls_ccm_setkey(&ccm, MBEDTLS_CIPHER_ID_AES, key, NDW_KEY_LEN * 8) == 0;
    if (ok) {
        /* In place: the plaintext and the ciphertext are the same nine bytes,
           and a scratch buffer would only be a second thing to get wrong. */
        ok = mbedtls_ccm_encrypt_and_tag(&ccm, NDW_SEALED_LEN, &payload[NDW_OFF_NONCE],
                                         NDW_NONCE_LEN, payload, NDW_CLEAR_LEN,
                                         &payload[NDW_OFF_UPTIME], &payload[NDW_OFF_UPTIME],
                                         &payload[NDW_OFF_TAG], NDW_TAG_LEN) == 0;
    }

    mbedtls_ccm_free(&ccm);

    if (!ok) {
        /* The payload is now neither plaintext nor valid ciphertext. Saying so
           and sending nothing beats sending a frame the platform will count as
           a forgery attempt. */
        ESP_LOGE(TAG, "could not seal the reading");
    }
    return ok;
}

static void advertise(void)
{
    /* Sized for the larger of the two: a version 1 frame simply sends fewer
       of these bytes. */
    uint8_t payload[NDW_BEACON_SEALED_LEN] = {0};
    bool sealed = ndw_console_provisioned();

    payload[NDW_OFF_COMPANY] = NDW_COMPANY_ID & 0xff;
    payload[NDW_OFF_COMPANY + 1] = (NDW_COMPANY_ID >> 8) & 0xff;
    payload[NDW_OFF_VERSION] = sealed ? NDW_BEACON_VERSION_SEALED : NDW_BEACON_VERSION;
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

    if (sealed) {
        if (!seal_payload(payload)) {
            return;
        }
    }

    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.mfg_data = payload;
    fields.mfg_data_len = sealed ? NDW_BEACON_SEALED_LEN : NDW_BEACON_LEN;

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

/*
 * One press: count it, remember it, and broadcast it.
 *
 * Shared by the switch and the console's `press` command so the two cannot
 * drift — a test that exercised a different path from the real one would
 * prove nothing about the real one.
 */
void ndw_device_press(void)
{
    s_counter++;
    store_counter();
    s_broadcast_until = esp_timer_get_time() + (int64_t)BROADCAST_MS * 1000;
    ESP_LOGI(TAG, "press %" PRIu32, s_counter);
    advertise();
    gpio_set_level(LED_GPIO, 1);
}

/* ------------------------------------------------------------------ button */

static void button_task(void *arg)
{
    (void)arg;

    /* The level the last poll saw, and how many polls in a row have agreed
       with it. A level is believed only once it has held still. */
    bool seen = false;
    int held = 0;

    /* The level currently believed, which is what a press is measured
       against. */
    bool was_down = false;
    int64_t last_press = 0;

    for (;;) {
        bool down = gpio_get_level(BUTTON_GPIO) == 0;
        int64_t now = esp_timer_get_time();

        /*
         * Counts on the press, not the release: a button that responds when
         * you push it feels immediate, and one that waits for release feels
         * broken.
         *
         * Two filters. A level is believed only after STABLE_POLLS
         * consecutive polls have agreed with it, and a count is then refused
         * if the last one was less than PRESS_GAP_MS ago. The first rejects
         * contact bounce; the second bounds how fast the counter can climb
         * whatever the line does.
         *
         * The run length counts polls that agree with each other, not polls
         * that disagree with the believed level. Counting the latter — which
         * this did — means one bouncing poll resets the run, so the run never
         * accumulates and no press is ever seen. It happened to work only
         * because the threshold was two.
         */
        if (down != seen) {
            seen = down;
            held = 1;
        } else if (held < STABLE_POLLS) {
            held++;
        }

        if (held >= STABLE_POLLS && seen != was_down) {
            was_down = seen;
            if (was_down) {
                if (last_press != 0 && now - last_press < (int64_t)PRESS_GAP_MS * 1000) {
                    /* Debug, not warn: on a chattering switch this is the
                       common case, and at warn level it would bury the log in
                       exactly the situation where the log matters. */
                    ESP_LOGD(TAG, "ignoring a bounce %lldms after the last press",
                             (long long)((now - last_press) / 1000));
                } else {
                    last_press = now;
                    ndw_device_press();
                }
            }
        }

        /* The light follows the broadcast window, so it shows what the radio
           is doing rather than just that a press registered. */
        if (s_broadcast_until != 0 && now > s_broadcast_until) {
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

    ESP_LOGI(TAG, "NDW Test Button %s", NDW_FIRMWARE_VERSION);

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

    /* After derive_eui and load_counter: the console reports both, and a
       hello arriving in the gap would answer with zeros. */
    ndw_console_start();

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);

    xTaskCreate(button_task, "ndw-btn", 3072, NULL, 3, NULL);
}
