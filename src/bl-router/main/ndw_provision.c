#include "ndw_provision.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ndw_router.h"
#include "ndw_status.h"
#include "ndw_uplink.h"

static const char *TAG = "ndw-prov";

/*
 * One line in, one line out.
 *
 * A command arrives as a single JSON object terminated by a newline, and every
 * reply is one line. That is what lets the browser read the stream a chunk at
 * a time without a framing layer: it accumulates until it sees a newline, and
 * a line that does not start with the marker is log output to be shown or
 * ignored, never parsed.
 *
 * The limit is generous for a command and small enough that a stream of
 * garbage — a board printing a core dump into the same pipe — cannot grow the
 * buffer without bound. An over-long line is dropped to the next newline
 * rather than truncated and parsed, because half a JSON object that happens to
 * close its braces is worse than no object at all.
 */
#define COMMAND_MAX 512

/*
 * Sized for the reply that carries a network list.
 *
 * Sixteen networks at roughly forty bytes each, plus the envelope. Beyond that
 * the scan reports what fitted and says how many it saw, which is the honest
 * shape: a person picking their own network finds it near the top, and a list
 * that silently ended is indistinguishable from a quiet neighbourhood.
 */
#define REPLY_MAX 1024

/* How many access points to report. */
#define NETWORKS_MAX 16

/*
 * Long enough for the radio to hear a network that is not shouting.
 *
 * The C3 shares one antenna between WiFi and BLE, and the router's BLE scan
 * never stops — so an access point can be missed on a fast pass for reasons
 * that have nothing to do with range. This is slower than the driver's default
 * and finds more.
 */
#define SCAN_DWELL_MS 300

static void reply(const char *json)
{
    /*
     * printf, not ESP_LOGx.
     *
     * A reply is not a log line: it carries no level, no timestamp and no tag,
     * and it must survive a log level that hides everything. Writing it
     * straight to stdout also keeps it on one line, which is the whole framing
     * contract.
     */
    printf(NDW_REPLY_PREFIX "%s\n", json);
    fflush(stdout);
}

/* An error reply, when there is no structure worth returning. */
static void reply_error(const char *code, const char *detail)
{
    char body[256];
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\",\"detail\":\"%s\"}", code,
             detail != NULL ? detail : "");
    reply(body);
}

/*
 * One problem with one setting, named so a console can put it where the
 * person typed it.
 *
 * `field` is the wire name of the setting — "ssid", "password", "broker" —
 * rather than a section, because a section is a decision about layout and
 * this end should not be making it. A console that groups two fields into one
 * panel can group their problems too; one that lays them out flat still knows
 * which input to mark.
 */
typedef struct {
    const char *field;
    const char *message;
} ndw_fault_t;

/*
 * Every problem at once, rather than the first.
 *
 * Reporting one at a time means a person fixes it, saves, and is told about
 * the next — which for a form with a name, a passphrase and a broker is three
 * round trips through a board that takes twenty seconds to answer. Collecting
 * them costs nothing here and is the difference between one correction and
 * several.
 */
static void reply_faults(const char *code, const ndw_fault_t *faults, int count)
{
    char body[512];
    int n = snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\",\"faults\":[", code);

    for (int i = 0; i < count && n > 0 && n < (int)sizeof(body); i++) {
        int w = snprintf(body + n, sizeof(body) - n, "%s{\"field\":\"%s\",\"message\":\"%s\"}",
                         i == 0 ? "" : ",", faults[i].field, faults[i].message);
        if (w <= 0 || n + w >= (int)sizeof(body)) {
            break;
        }
        n += w;
    }

    if (n > 0 && n + 4 < (int)sizeof(body)) {
        /* `detail` as well, so a reader that knows nothing of faults still has
           something to show rather than an empty error. */
        snprintf(body + n, sizeof(body) - n, "],\"detail\":\"%s\"}",
                 count > 0 ? faults[0].message : "");
        reply(body);
    } else {
        reply_error(code, count > 0 ? faults[0].message : NULL);
    }
}

/* ------------------------------------------------------------------- hello */

/*
 * Who this board is, before anything has been asked of it.
 *
 * The EUI is what the console claims the box by, and the version is what tells
 * someone whether the firmware they just wrote is the firmware that booted —
 * the one question a flasher cannot answer for itself.
 */
static void on_hello(void)
{
    char body[REPLY_MAX];
    int n = snprintf(body, sizeof(body), "{\"ok\":true,\"eui\":\"%s\",\"firmware\":\"%s\",\"state\":\"%s\"}",
                     ndw_eui(), NDW_FIRMWARE_VERSION, ndw_provision_state());
    if (n > 0 && n < (int)sizeof(body)) {
        reply(body);
    }
}

/* ------------------------------------------------------------------ status */

static void on_status(void)
{
    char body[REPLY_MAX];
    /*
     * The status document, wrapped so every reply has the same envelope. A
     * reader should be able to check `ok` without knowing which command it
     * answered.
     */
    char inner[REPLY_MAX - 32];
    int n = ndw_status_json(inner, sizeof(inner), ndw_provision_state());
    if (n <= 0 || n >= (int)sizeof(inner)) {
        reply_error("status", "could not render status");
        return;
    }

    /* Splice `"ok":true` into the object rather than nesting it: one flat
       shape is easier to read by eye on a serial monitor. */
    if (inner[0] != '{') {
        reply_error("status", "unexpected status shape");
        return;
    }
    n = snprintf(body, sizeof(body), "{\"ok\":true,%s", inner + 1);
    if (n > 0 && n < (int)sizeof(body)) {
        reply(body);
    }
}

/* -------------------------------------------------------------------- scan */

/* Names the protection a network advertises, so the console can say so. */
static const char *auth_name(wifi_auth_mode_t mode)
{
    switch (mode) {
    case WIFI_AUTH_OPEN:
        return "open";
    case WIFI_AUTH_WEP:
        return "wep";
    case WIFI_AUTH_WPA_PSK:
    case WIFI_AUTH_WPA2_PSK:
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "wpa2";
    case WIFI_AUTH_WPA3_PSK:
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "wpa3";
    case WIFI_AUTH_WPA2_ENTERPRISE:
        return "enterprise";
    default:
        return "unknown";
    }
}

/*
 * Escapes an SSID into a JSON string body.
 *
 * An SSID is arbitrary bytes, not text. Quotes and backslashes would break the
 * document, control characters would make it unparseable, and a name that is
 * not valid UTF-8 is entirely legal on the air. Anything outside printable
 * ASCII becomes an escape, so the reply is always well-formed even when the
 * network next door is called something unrepresentable.
 */
static void escape_ssid(const uint8_t *ssid, char *out, size_t len)
{
    size_t o = 0;
    for (size_t i = 0; i < 32 && ssid[i] != '\0' && o + 7 < len; i++) {
        uint8_t c = ssid[i];
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c >= 0x20 && c < 0x7f) {
            out[o++] = (char)c;
        } else {
            o += (size_t)snprintf(out + o, len - o, "\\u%04x", c);
        }
    }
    out[o] = '\0';
}

/*
 * Lists what the radio can hear.
 *
 * The scan is blocking and the driver will not run one while a join is in
 * flight, so this is serialised against joining by the same lock — a console
 * that asks for a list mid-join waits rather than getting an error it cannot
 * act on.
 */
static void on_scan(void)
{
    wifi_scan_config_t cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = {.active = {.min = 100, .max = SCAN_DWELL_MS}},
    };

    ndw_radio_lock();
    esp_err_t err = esp_wifi_scan_start(&cfg, true);
    if (err != ESP_OK) {
        ndw_radio_unlock();
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
        reply_error("scan", esp_err_to_name(err));
        return;
    }

    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);

    uint16_t wanted = found < NETWORKS_MAX ? found : NETWORKS_MAX;
    wifi_ap_record_t *records = NULL;
    if (wanted > 0) {
        records = calloc(wanted, sizeof(*records));
        if (records == NULL) {
            esp_wifi_scan_get_ap_records(&(uint16_t){0}, NULL);
            ndw_radio_unlock();
            reply_error("scan", "out of memory");
            return;
        }
        esp_wifi_scan_get_ap_records(&wanted, records);
    }
    ndw_radio_unlock();

    char body[REPLY_MAX];
    int n = snprintf(body, sizeof(body), "{\"ok\":true,\"found\":%u,\"networks\":[", found);

    for (uint16_t i = 0; i < wanted && n > 0 && n < (int)sizeof(body); i++) {
        char name[200];
        escape_ssid(records[i].ssid, name, sizeof(name));
        if (name[0] == '\0') {
            /* A hidden network answers with an empty name. Nothing can be
               typed to join it from a list, so it is left out rather than
               shown as a blank row. */
            continue;
        }

        int w = snprintf(body + n, sizeof(body) - n, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":\"%s\"}",
                         i == 0 ? "" : ",", name, records[i].rssi, auth_name(records[i].authmode));
        if (w <= 0 || n + w >= (int)sizeof(body)) {
            /* Out of room. Stop cleanly rather than emit half an entry — the
               `found` count already says the list is incomplete. */
            break;
        }
        n += w;
    }

    free(records);

    if (n > 0 && n + 3 < (int)sizeof(body)) {
        snprintf(body + n, sizeof(body) - n, "]}");
        reply(body);
    } else {
        reply_error("scan", "too many networks to report");
    }
}

/* -------------------------------------------------------------------- wifi */

/*
 * Joins the network the console picked, and keeps the credentials only if it
 * worked.
 *
 * The order is the whole point, and it is the same order the BLE path used:
 * try, then store. A box that stored first and failed would reboot into a
 * network it has never reached and report itself configured.
 */
static void on_wifi(const cJSON *doc)
{
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(doc, "ssid");
    const cJSON *pass = cJSON_GetObjectItemCaseSensitive(doc, "password");

    const cJSON *broker = cJSON_GetObjectItemCaseSensitive(doc, "broker");
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(doc, "name");

    /*
     * Checked together, and all of them, before anything is applied.
     *
     * Applying as we validate would leave a board carrying half a change when
     * the other half is refused — a new broker set against a network it was
     * never told how to reach. Nothing here touches the radio until every
     * field has passed.
     */
    ndw_fault_t faults[4];
    int count = 0;

    if (!cJSON_IsString(ssid) || ssid->valuestring[0] == '\0') {
        faults[count++] = (ndw_fault_t){"ssid", "A network name is required."};
    } else if (strlen(ssid->valuestring) >= SSID_MAX) {
        faults[count++] = (ndw_fault_t){"ssid", "That network name is too long for the radio."};
    }

    /* An open network has no passphrase, so an empty string is valid — but a
       missing key is a malformed command rather than an open network. */
    if (!cJSON_IsString(pass)) {
        faults[count++] = (ndw_fault_t){"password", "A passphrase is required, even if empty."};
    } else if (strlen(pass->valuestring) >= PASS_MAX) {
        faults[count++] = (ndw_fault_t){"password", "That passphrase is longer than WPA2 allows."};
    }

    /*
     * The broker, checked rather than taken on trust.
     *
     * A board given an address it cannot parse joins the network, reports
     * itself healthy and publishes nothing — the failure an installer cannot
     * see, because everything else looks right. Refusing it here costs one
     * round trip; accepting it costs a site visit.
     */
    if (cJSON_IsString(broker) && broker->valuestring[0] != '\0') {
        const char *url = broker->valuestring;
        if (strncmp(url, "ws://", 5) != 0 && strncmp(url, "wss://", 6) != 0 &&
            strncmp(url, "mqtt://", 7) != 0 && strncmp(url, "mqtts://", 8) != 0) {
            faults[count++] =
                (ndw_fault_t){"broker", "A broker address starts with wss://, ws://, mqtt:// or mqtts://."};
        } else if (strlen(url) >= NDW_BROKER_MAX) {
            faults[count++] = (ndw_fault_t){"broker", "That broker address is too long."};
        }
    }

    if (count > 0) {
        reply_faults("wifi", faults, count);
        return;
    }

    /* Optional, and applied before the join so the DHCP lease carries the
       name the person chose rather than the default. */
    if (cJSON_IsString(name) && name->valuestring[0] != '\0') {
        ndw_set_hostname(name->valuestring);
    }

    if (cJSON_IsString(broker) && broker->valuestring[0] != '\0') {
        ndw_uplink_set_broker(broker->valuestring);
    }

    char ssid_buf[SSID_MAX];
    char pass_buf[PASS_MAX];
    strlcpy(ssid_buf, ssid->valuestring, sizeof(ssid_buf));
    strlcpy(pass_buf, pass->valuestring, sizeof(pass_buf));

    ndw_status_set(NDW_LED_JOINING);

    if (ndw_wifi_join(ssid_buf, pass_buf)) {
        ndw_store_credentials(ssid_buf, pass_buf);
        ndw_status_set(NDW_LED_ONLINE);
    } else {
        ndw_status_set(NDW_LED_FAILED);
    }

    /* The full status either way: a failure that names the cause — bad-auth
       against not-found — is the difference between retyping a password and
       walking closer to the access point. */
    on_status();
}

/* ------------------------------------------------------------------ forget */

static void on_forget(void)
{
    ndw_forget_credentials();
    reply("{\"ok\":true,\"state\":\"unprovisioned\"}");
}

/* ----------------------------------------------------------------- dispatch */

static void dispatch(char *line)
{
    cJSON *doc = cJSON_Parse(line);
    if (doc == NULL) {
        /*
         * Silent, deliberately.
         *
         * Anything may be typed into a serial console, and a board that
         * answered every stray keystroke with an error would make its own
         * transcript unreadable. A command that was meant will be well-formed.
         */
        return;
    }

    const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(doc, "cmd");
    if (!cJSON_IsString(cmd)) {
        reply_error("command", "cmd is required");
        cJSON_Delete(doc);
        return;
    }

    if (strcmp(cmd->valuestring, "hello") == 0) {
        on_hello();
    } else if (strcmp(cmd->valuestring, "status") == 0) {
        on_status();
    } else if (strcmp(cmd->valuestring, "scan") == 0) {
        on_scan();
    } else if (strcmp(cmd->valuestring, "wifi") == 0) {
        on_wifi(doc);
    } else if (strcmp(cmd->valuestring, "forget") == 0) {
        on_forget();
    } else {
        reply_error("command", "unknown command");
    }

    cJSON_Delete(doc);
}

/* --------------------------------------------------------------------- task */

static void provision_task(void *arg)
{
    (void)arg;

    static char line[COMMAND_MAX];
    size_t used = 0;
    bool overrun = false;

    ESP_LOGI(TAG, "console ready");

    for (;;) {
        int c = getchar();
        if (c == EOF) {
            /*
             * No input available. stdin is non-blocking on USB-Serial-JTAG
             * when nothing is attached, so this is the normal idle path rather
             * than an error — sleep instead of spinning a core on it.
             */
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (c == '\r') {
            continue;
        }

        if (c == '\n') {
            if (overrun) {
                /* The line was too long to hold. It was dropped as it
                   arrived; this is where the tail of it ends. */
                ESP_LOGW(TAG, "dropped an over-long line");
                overrun = false;
            } else if (used > 0) {
                line[used] = '\0';
                dispatch(line);
            }
            used = 0;
            continue;
        }

        if (used + 1 >= sizeof(line)) {
            overrun = true;
            used = 0;
            continue;
        }

        line[used++] = (char)c;
    }
}

void ndw_provision_start(void)
{
    /*
     * Small stack, but not tiny: cJSON parses into the heap, while the reply
     * buffers and a scan's worth of records are on this stack.
     */
    xTaskCreate(provision_task, "ndw-prov", 4608, NULL, 3, NULL);
}
