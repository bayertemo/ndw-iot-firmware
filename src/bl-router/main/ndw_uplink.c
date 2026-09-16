#include "ndw_uplink.h"

#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "esp_timer.h"
#include "nvs.h"

#include "ndw_command.h"
#include "ndw_queue.h"

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
/*
 * MQTT over WebSocket, not raw TCP.
 *
 * emqx.ndw.ai resolves to Cloudflare, which proxies HTTP and will not carry
 * MQTT on 1883 — the meterfax README documents that trap. But EMQX already
 * runs a WebSocket listener on 8083 and the existing ingress already routes
 * to it, so wss://emqx.ndw.ai/mqtt works today with no new port, no
 * LoadBalancer change and no DNS record that bypasses the proxy.
 *
 * Verified by hand: the handshake returns 101 Switching Protocols with
 * sec-websocket-protocol: mqtt.
 *
 * It also keeps Cloudflare's TLS and DDoS protection in front of a broker
 * that currently has authentication = [], which matters more than the few
 * bytes of WebSocket framing it costs per message.
 */
#define NDW_MQTT_URI "wss://emqx.ndw.ai/mqtt"
#endif

/* Long enough for mqtts://host.example.com:8883 and room to spare. */
#define BROKER_MAX NDW_BROKER_MAX

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

/*
 * Recently published readings, so a repeat is not sent twice.
 *
 * A sensor rebroadcasts one counter for ten seconds and the scanner sees it
 * roughly every 150ms, so forwarding every copy meant about 65 publishes for
 * a single press. Ingest dedups on (device, counter) and discarded 64 of
 * them — correct, but the traffic was already paid for. This drops them at
 * the point they are created.
 *
 * Sized for a bench and a small site. Full is not an error: the oldest entry
 * is replaced, and the worst case is a duplicate the server discards exactly
 * as it does today. Being wrong here costs bandwidth, never a reading.
 */
#define SEEN_MAX 32

typedef struct {
    char eui[17];
    uint32_t counter;
    int64_t at;
} seen_t;

static seen_t s_seen[SEEN_MAX];
static uint8_t s_seen_next;

/*
 * How long a reading is remembered.
 *
 * Longer than the sensor's ten-second broadcast, so every copy of one press
 * is suppressed. Not much longer: a counter that wraps, or a device reset
 * back to a value it has used before, should be publishable again rather
 * than silently dropped forever.
 */
#define SEEN_TTL_US (60LL * 1000 * 1000)

/* True if this exact reading was published recently; records it if not. */
static bool already_sent(const ndw_frame_t *frame)
{
    int64_t now = esp_timer_get_time();

    for (int i = 0; i < SEEN_MAX; i++) {
        if (s_seen[i].eui[0] == '\0') {
            continue;
        }
        if (now - s_seen[i].at > SEEN_TTL_US) {
            /* Expired. Free it rather than letting stale entries crowd out
               live sensors in a small table. */
            s_seen[i].eui[0] = '\0';
            continue;
        }
        if (s_seen[i].counter == frame->counter &&
            strcmp(s_seen[i].eui, frame->eui) == 0) {
            return true;
        }
    }

    seen_t *slot = &s_seen[s_seen_next];
    s_seen_next = (uint8_t)((s_seen_next + 1) % SEEN_MAX);
    strlcpy(slot->eui, frame->eui, sizeof(slot->eui));
    slot->counter = frame->counter;
    slot->at = now;
    return false;
}

static esp_mqtt_client_handle_t s_client;
static volatile bool s_connected;
static char s_gateway[17];
static char s_broker[BROKER_MAX];

/*
 * A broker change made remotely, on trial.
 *
 * Remote is the dangerous direction: the command arrives over the very
 * connection it replaces, so a wrong address leaves the box unreachable by
 * the only channel that could fix it — and an installed gateway does not
 * advertise over BLE, so recovery means a ladder.
 *
 * So the previous address is kept here while the new one is tried. Connect
 * within the window and the trial ends; fail and the box puts the old one
 * back itself.
 */
static char s_broker_previous[BROKER_MAX];
static int64_t s_trial_deadline;

/*
 * How long a new broker has to prove itself.
 *
 * Five minutes. Long enough to ride out a slow DNS answer, a captive portal
 * still waking up, or a broker mid-restart — none of which mean the address
 * is wrong. Short enough that a genuine typo costs one missed heartbeat
 * rather than an afternoon.
 */
#define BROKER_TRIAL_US (5LL * 60 * 1000 * 1000)

static void apply_broker(void);

/*
 * Why the link is down, in words a person can act on.
 *
 * Kept because the console asks for it: a box that is on WiFi but publishing
 * nothing looks the same from the outside whether the address is wrong, the
 * name does not resolve, or the certificate was rejected — and those have
 * three different fixes, only one of which is "retype the broker".
 */
static const char *s_error = "";

static void on_mqtt_event(void *handler_args, esp_event_base_t base, int32_t event_id, void *data)
{
    (void)handler_args;
    (void)base;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        s_error = "";
        /*
         * A connection is the proof a trial was waiting for. Cleared here
         * rather than on a timer expiring successfully, because reaching the
         * broker is the only evidence that matters — everything else is a
         * guess about whether the address was right.
         */
        if (s_trial_deadline != 0) {
            ESP_LOGI(TAG, "broker %s accepted; keeping it", s_broker);
            s_trial_deadline = 0;
            s_broker_previous[0] = '\0';
        }
        ESP_LOGI(TAG, "connected to the broker");
        ndw_command_subscribe();
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

    case MQTT_EVENT_ERROR: {
        /*
         * Reduced to one of a few tokens rather than passed through. The
         * console shows this to an installer, and an mbedtls error code is
         * not something they can act on — "the name does not resolve" sends
         * them to the address field, "the certificate was rejected" does not.
         */
        const esp_mqtt_event_handle_t ev = data;
        if (ev != NULL && ev->error_handle != NULL) {
            const esp_mqtt_error_codes_t *e = ev->error_handle;
            if (e->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                s_error = "refused";
            } else if (e->esp_tls_cert_verify_flags != 0) {
                s_error = "certificate";
            } else if (e->esp_tls_last_esp_err != 0 || e->esp_tls_stack_err != 0) {
                s_error = "tls";
            } else if (e->esp_transport_sock_errno != 0) {
                s_error = "unreachable";
            } else {
                s_error = "failed";
            }
        } else {
            s_error = "failed";
        }
        ESP_LOGW(TAG, "mqtt error: %s", s_error);
        break;
    }

    case MQTT_EVENT_DATA: {
        /*
         * The only thing this box subscribes to is its own command topic, so
         * no routing is needed — but the payload is handed on rather than
         * acted on here, because this runs on the system event loop and a
         * handler that reconnects would block every other event behind it.
         */
        const esp_mqtt_event_handle_t ev = data;
        if (ev != NULL && ev->data_len > 0) {
            ndw_command_handle(ev->data, ev->data_len);
        }
        break;
    }

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

    /*
     * No trial on this path. Someone is at the box with a Bluetooth link
     * open: they can see it fail and correct it, and a revert underneath them
     * would undo what they just typed.
     */
    s_trial_deadline = 0;
    s_broker_previous[0] = '\0';

    if (changed) {
        apply_broker();
    }
}

/* Points the client at whatever s_broker now holds. */
static void apply_broker(void)
{
    if (s_client == NULL) {
        return;
    }

    esp_mqtt_client_stop(s_client);
    /* Same bundle as init: a broker typed into the console is exactly the
       case that needs it, and omitting it here would mean the default works
       while a provisioned wss:// address silently does not. */
    esp_mqtt_set_config(s_client, &(esp_mqtt_client_config_t){
                                      .broker.address.uri = s_broker,
                                      .credentials.client_id = s_gateway,
                                      .session.keepalive = 60,
                                      .broker.verification.crt_bundle_attach =
                                          esp_crt_bundle_attach,
                                  });
    esp_mqtt_client_start(s_client);
}

/* Writes the broker to NVS, so a reboot comes up on it. */
static void store_broker(const char *uri)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGW(TAG, "could not store the broker address");
        return;
    }
    nvs_set_str(nvs, NVS_KEY_BROKER, uri);
    nvs_commit(nvs);
    nvs_close(nvs);
}

void ndw_uplink_try_broker(const char *uri)
{
    if (uri == NULL || uri[0] == '\0' || strcmp(uri, s_broker) == 0) {
        return;
    }

    /*
     * Remember where we were before moving. Only on the first trial — a
     * second command arriving mid-trial must not overwrite the address that
     * is known to work with one that is merely newer and also unproven.
     */
    if (s_trial_deadline == 0) {
        strlcpy(s_broker_previous, s_broker, sizeof(s_broker_previous));
    }

    strlcpy(s_broker, uri, sizeof(s_broker));
    /*
     * Persisted immediately, before it has proved anything. A reboot
     * mid-trial should come up on the address someone asked for rather than
     * silently ignoring the request — and the fallback below still catches it
     * if that address turns out to be wrong.
     */
    store_broker(s_broker);

    s_trial_deadline = esp_timer_get_time() + BROKER_TRIAL_US;
    ESP_LOGI(TAG, "trying broker %s (reverting to %s in %llds)", s_broker, s_broker_previous,
             (long long)(BROKER_TRIAL_US / 1000000));

    apply_broker();
}

bool ndw_uplink_broker_pending(void)
{
    return s_trial_deadline != 0;
}

void ndw_uplink_check_broker_trial(void)
{
    if (s_trial_deadline == 0 || esp_timer_get_time() < s_trial_deadline) {
        return;
    }

    /*
     * The window closed without a connection, so the address does not work
     * from where this box is standing. Going back is the only move it can
     * make on its own — the command channel it would need to be corrected on
     * is the very thing that is broken.
     */
    ESP_LOGW(TAG, "broker %s never connected; reverting to %s", s_broker, s_broker_previous);
    strlcpy(s_broker, s_broker_previous, sizeof(s_broker));
    store_broker(s_broker);

    s_trial_deadline = 0;
    s_broker_previous[0] = '\0';

    apply_broker();
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
        /*
         * The certificate bundle ESP-IDF ships, which includes the roots
         * Cloudflare chains to.
         *
         * Without it a wss:// broker fails before the handshake with "No
         * server verification option set" — the box joins WiFi, reports
         * itself healthy, and silently publishes nothing, which is the worst
         * shape a failure can take. Attaching the bundle rather than pinning
         * one certificate is what lets an installer point a box at their own
         * broker without a firmware change.
         *
         * Ignored for mqtt:// and ws:// brokers, so a plain bench broker is
         * unaffected.
         */
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
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
    /*
     * No connection check. The queue holds messages through an outage, so
     * refusing to enqueue while the link is down would throw away exactly the
     * readings the queue exists to keep.
     *
     * Dedupe still happens here rather than in the queue: it is about what the
     * radio heard, not about how messages are sent, and doing it at the point
     * frames are created keeps the queue ignorant of what it carries.
     */
    if (already_sent(frame)) {
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
    char topic[NDW_QUEUE_TOPIC_MAX];
    snprintf(topic, sizeof(topic), "ndw/%s/ble/%s/up", s_gateway, frame->eui);

    ndw_queue_push(topic, body, n);
}

void ndw_uplink_publish_telemetry(const char *body, int len)
{
    /*
     * A distinct topic, deliberately not matching ndw/+/ble/+/up. Telemetry
     * is infrastructure data about the box, not a reading belonging to an
     * account — routing it down the device path would put it in raw_frames
     * beside billing evidence.
     */
    char topic[NDW_QUEUE_TOPIC_MAX];
    snprintf(topic, sizeof(topic), "ndw/%s/gateway/%s/telemetry", s_gateway, s_gateway);

    ndw_queue_push(topic, body, len);
}

bool ndw_uplink_send(const char *topic, const char *body, int len)
{
    if (s_client == NULL || !s_connected) {
        return false;
    }

    return esp_mqtt_client_publish(s_client, topic, body, len, UPLINK_QOS, 0) >= 0;
}

bool ndw_uplink_subscribe(const char *topic, int qos)
{
    if (s_client == NULL || !s_connected) {
        return false;
    }
    return esp_mqtt_client_subscribe(s_client, topic, qos) >= 0;
}

bool ndw_uplink_connected(void)
{
    return s_connected;
}

const char *ndw_uplink_broker(void)
{
    return s_broker;
}

const char *ndw_uplink_last_error(void)
{
    return s_connected ? "" : s_error;
}
