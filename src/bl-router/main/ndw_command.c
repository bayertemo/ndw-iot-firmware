#include "ndw_command.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "mqtt_client.h"

#include "ndw_uplink.h"

static const char *TAG = "ndw-cmd";

/*
 * QoS 1 for commands, unlike the uplink's QoS 0.
 *
 * The reasoning that makes QoS 0 right for frames is exactly what makes it
 * wrong here. A reading is one of dozens carrying the same counter, so losing
 * one costs nothing; a command is sent once and there is no second copy
 * behind it. A gateway that misses the only instruction it was ever sent is
 * indistinguishable from one that ignored it.
 */
#define COMMAND_QOS 1

/* The gateway whose commands we take, set at subscribe time. */
static char s_gateway[17];

void ndw_command_topic(char *out, int len)
{
    /*
     * The downlink mirror of the telemetry topic. A gateway subscribes to
     * exactly one thing, addressed to itself by the identity it derives from
     * its own chip — so a command meant for one box cannot be delivered to
     * another by a broker misconfiguration.
     */
    snprintf(out, len, "ndw/%s/gateway/%s/cmd", s_gateway, s_gateway);
}

void ndw_command_init(const char *gateway_eui)
{
    strlcpy(s_gateway, gateway_eui, sizeof(s_gateway));
}

void ndw_command_subscribe(void)
{
    if (s_gateway[0] == '\0') {
        return;
    }

    char topic[80];
    ndw_command_topic(topic, sizeof(topic));

    /*
     * On every connect, not once at startup. A broker that dropped us has
     * forgotten our subscriptions, and this client uses a clean session — so
     * a reconnect without re-subscribing leaves a box that looks healthy,
     * publishes normally, and silently takes no instructions.
     */
    if (ndw_uplink_subscribe(topic, COMMAND_QOS)) {
        ESP_LOGI(TAG, "listening on %s", topic);
    } else {
        ESP_LOGW(TAG, "could not subscribe to %s", topic);
    }
}

void ndw_command_handle(const char *data, int len)
{
    /*
     * Bounded before parsing. A command is a short JSON object; anything
     * larger is not ours, and cJSON would happily allocate whatever arrived.
     */
    if (data == NULL || len <= 0 || len > 512) {
        ESP_LOGW(TAG, "ignoring a command of %d bytes", len);
        return;
    }

    /* cJSON_ParseWithLength: the payload is not NUL-terminated. */
    cJSON *doc = cJSON_ParseWithLength(data, (size_t)len);
    if (doc == NULL) {
        ESP_LOGW(TAG, "command was not json");
        return;
    }

    const cJSON *broker = cJSON_GetObjectItemCaseSensitive(doc, "broker");
    if (cJSON_IsString(broker) && broker->valuestring[0] != '\0') {
        /*
         * try_ rather than set_. This arrived over the very connection it
         * replaces, with nobody at the box: a wrong address would leave it
         * reachable by neither MQTT nor BLE, since an installed gateway does
         * not advertise. The trial puts the old address back on its own.
         */
        ESP_LOGI(TAG, "command: broker -> %s", broker->valuestring);
        ndw_uplink_try_broker(broker->valuestring);
    }

    /*
     * No WiFi here, deliberately, and it is not an omission to be filled in
     * later. A passphrase would transit our broker, which is the boundary
     * that BLE-only provisioning exists to keep — and credentials delivered
     * over WiFi are self-defeating, because wrong ones drop the network they
     * arrived on and strand the box behind a button press that may be three
     * metres up a wall.
     */

    cJSON_Delete(doc);
}
