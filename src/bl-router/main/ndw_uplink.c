#include "ndw_uplink.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "mqtt_client.h"
#include "nvs.h"

static const char *TAG = "ndw-uplink";

/*
 * Where the broker lives.
 *
 * Provisioned, not compiled in. It is the one piece of configuration a box
 * cannot discover for itself, and it belongs beside the WiFi credentials for
 * the same reason they do: it is a property of the site, set by whoever is
 * installing the box, and a firmware rebuild to change it would mean a site
 * visit with a laptop and a cable.
 *
 * The compiled value is only a fallback for a box provisioned before the
 * field existed, or one whose installer left it blank.
 */
#ifndef NDW_MQTT_URI
#define NDW_MQTT_URI "mqtt://emqx.ndw.ai:1883"
#endif

/* Long enough for mqtts://host.example.com:8883 and room to spare. */
#define BROKER_MAX 128

#define NVS_NAMESPACE "ndw"
#define NVS_KEY_BROKER "broker"

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
static char s_broker[BROKER_MAX];

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

/* Reads the provisioned broker, falling back to the compiled default. */
static void load_broker(void)
{
    nvs_handle_t nvs;
    size_t len = sizeof(s_broker);

    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        if (nvs_get_str(nvs, NVS_KEY_BROKER, s_broker, &len) == ESP_OK && s_broker[0] != '\0') {
            nvs_close(nvs);
            return;
        }
        nvs_close(nvs);
    }

    strlcpy(s_broker, NDW_MQTT_URI, sizeof(s_broker));
}

void ndw_uplink_set_broker(const char *uri)
{
    if (uri == NULL || uri[0] == '\0') {
        return;
    }

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGW(TAG, "could not store the broker address");
        return;
    }
    nvs_set_str(nvs, NVS_KEY_BROKER, uri);
    nvs_commit(nvs);
    nvs_close(nvs);

    ESP_LOGI(TAG, "broker set to %s", uri);

    /*
     * Reconnect rather than wait for a reboot. A technician who has just
     * typed an address expects the box to try it while they are still
     * standing there — and if it is wrong, to say so before they leave.
     */
    bool changed = strcmp(uri, s_broker) != 0;
    strlcpy(s_broker, uri, sizeof(s_broker));

    if (changed && s_client != NULL) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_set_config(s_client, &(esp_mqtt_client_config_t){
                                          .broker.address.uri = s_broker,
                                          .credentials.client_id = s_gateway,
                                          .session.keepalive = 60,
                                      });
        esp_mqtt_client_start(s_client);
    }
}

void ndw_uplink_init(const char *gateway_eui)
{
    strlcpy(s_gateway, gateway_eui, sizeof(s_gateway));
    load_broker();

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = s_broker,
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

    ESP_LOGI(TAG, "uplink to %s as %s", s_broker, s_gateway);
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
