#include "ndw_uplink.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "mqtt_client.h"

static const char *TAG = "ndw-uplink";

/*
 * Where the broker lives.
 *
 * A compiled-in default rather than a provisioned setting, for now. It is the
 * one piece of configuration a box cannot discover for itself, and putting it
 * in the BLE provisioning payload is the right answer — but that is a change
 * to the contract on both sides, and this needs to work first.
 */
#ifndef NDW_MQTT_URI
#define NDW_MQTT_URI "mqtt://emqx.ndw.ai:1883"
#endif

/*
 * QoS 0.
 *
 * A sensor repeats its counter for ten seconds, so a lost frame is followed
 * by dozens more carrying the same reading — and ingest dedups on
 * (device, counter), so the duplicates cost nothing. Paying for delivery
 * guarantees on a stream that is already redundant would buy reliability
 * twice and add a broker round trip per frame.
 */
#define UPLINK_QOS 0

static esp_mqtt_client_handle_t s_client;
static volatile bool s_connected;
static char s_gateway[17];

static void on_mqtt_event(void *handler_args, esp_event_base_t base, int32_t event_id, void *data)
{
    (void)handler_args;
    (void)base;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "connected to the broker");
        break;

    case MQTT_EVENT_DISCONNECTED:
        /*
         * Expected, not exceptional. A router lives somewhere with imperfect
         * WiFi; the client reconnects on its own and frames are dropped
         * meanwhile rather than queued.
         */
        s_connected = false;
        ESP_LOGW(TAG, "lost the broker; will reconnect");
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "mqtt error");
        break;

    default:
        break;
    }
}

void ndw_uplink_init(const char *gateway_eui)
{
    strlcpy(s_gateway, gateway_eui, sizeof(s_gateway));

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = NDW_MQTT_URI,
        /*
         * The client id is the gateway's EUI, which is unique by
         * construction. It matters: a broker closes the older connection when
         * a duplicate id appears, so two routers sharing one would evict each
         * other in a loop — the same failure the API hit with its fixed
         * subscriber id, which EMQX's flapping detector then banned for
         * thirty minutes.
         */
        .credentials.client_id = s_gateway,
        .session.keepalive = 60,
    };

    s_client = esp_mqtt_client_init(&cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "could not create the mqtt client");
        return;
    }

    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_mqtt_event, NULL);

    /*
     * Started before WiFi is necessarily up. The client handles that: it
     * retries until the network exists rather than failing once and staying
     * down, which is what a box that boots faster than its AP needs.
     */
    esp_err_t err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not start the mqtt client: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "uplink to %s as %s", NDW_MQTT_URI, s_gateway);
}

/*
 * Base64, six bits at a time.
 *
 * Written out rather than pulled from mbedtls: the payload is eight bytes and
 * the alternative is a dependency plus an allocation for something that fits
 * in a dozen lines.
 */
static void base64(const uint8_t *in, size_t len, char *out, size_t out_len)
{
    static const char *ALPHABET =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    size_t o = 0;
    for (size_t i = 0; i < len && o + 4 < out_len; i += 3) {
        uint32_t block = (uint32_t)in[i] << 16;
        if (i + 1 < len) block |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < len) block |= in[i + 2];

        out[o++] = ALPHABET[(block >> 18) & 0x3f];
        out[o++] = ALPHABET[(block >> 12) & 0x3f];
        out[o++] = (i + 1 < len) ? ALPHABET[(block >> 6) & 0x3f] : '=';
        out[o++] = (i + 2 < len) ? ALPHABET[block & 0x3f] : '=';
    }
    out[o] = '\0';
}

void ndw_uplink_publish(const ndw_frame_t *frame)
{
    if (s_client == NULL || !s_connected) {
        /* Dropped rather than queued. See the header for why. */
        return;
    }

    /*
     * The payload the application layer decodes: the counter, then uptime and
     * battery. The network layer does not interpret any of it — that is the
     * whole point of the layering, and it is why the counter travels in the
     * payload as well as in fCnt.
     */
    uint8_t raw[9];
    raw[0] = frame->kind;
    for (int i = 0; i < 4; i++) {
        raw[1 + i] = (uint8_t)((frame->counter >> (8 * i)) & 0xff);
    }
    for (int i = 0; i < 3; i++) {
        raw[5 + i] = (uint8_t)((frame->uptime >> (8 * i)) & 0xff);
    }
    raw[8] = frame->battery;

    char payload[16];
    base64(raw, sizeof(raw), payload, sizeof(payload));

    /*
     * ChirpStack's envelope, with the LoRaWAN-only fields absent. No time
     * field: this box has no clock it can trust — it never sets one from NTP
     * — and a fabricated timestamp is worse than none, because ingest would
     * take it as the as-of key for attribution. Absent means the server
     * stamps arrival, which is honest about what is actually known.
     */
    char body[320];
    int n = snprintf(body, sizeof(body),
                     "{\"deviceInfo\":{\"devEui\":\"%s\"},"
                     "\"fCnt\":%lu,"
                     "\"data\":\"%s\","
                     "\"rxInfo\":[{\"gatewayId\":\"%s\",\"rssi\":%d}]}",
                     frame->eui, (unsigned long)frame->counter, payload, s_gateway, frame->rssi);
    if (n <= 0 || n >= (int)sizeof(body)) {
        ESP_LOGW(TAG, "envelope did not fit; dropping the frame");
        return;
    }

    /* ndw/<account>/ble/<device>/up. The gateway EUI stands in for the
       account until the broker authenticates per-account credentials — the
       pattern the API subscribes to matches either way. */
    char topic[80];
    snprintf(topic, sizeof(topic), "ndw/%s/ble/%s/up", s_gateway, frame->eui);

    int msg = esp_mqtt_client_publish(s_client, topic, body, n, UPLINK_QOS, 0);
    if (msg < 0) {
        ESP_LOGW(TAG, "publish failed");
    }
}

bool ndw_uplink_connected(void)
{
    return s_connected;
}
