#include "ndw_telemetry.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ndw_queue.h"
#include "ndw_scan.h"
#include "ndw_uplink.h"

static const char *TAG = "ndw-telem";

/*
 * The same define main.c reports at boot, rather than esp_app_get_description().
 * That would pull in app_update for a string the build already has, and it
 * reads the project version from the image header — which is not the version
 * CI stamps into a build.
 */
#ifndef NDW_VERSION
#define NDW_VERSION "0.0.0-dev"
#endif

/*
 * How often the heartbeat fires.
 *
 * Five minutes. Long enough that a thousand gateways cost the broker a few
 * messages a second, short enough that a box which died is noticed within one
 * missed interval rather than an hour. Anything urgent arrives as an event
 * instead, so this does not have to be fast.
 */
#define PERIOD_MS (5 * 60 * 1000)

/*
 * Neighbours remembered between reports.
 *
 * Bounded because a busy site has hundreds of BLE devices and a gateway has
 * no business holding a list that grows with the room. Overflow is counted
 * rather than silently dropped — "we saw more than we could report" is
 * itself worth knowing, and a census that quietly truncates would understate
 * exactly the dense sites where coverage questions matter most.
 */
#define NEIGHBOURS_MAX 16

typedef struct {
    char eui[17];
    int8_t rssi;
    uint16_t seen;
    bool is_ndw;
} neighbour_t;

static neighbour_t s_neighbours[NEIGHBOURS_MAX];
static uint16_t s_overflow;
static uint32_t s_total_seen;
static uint32_t s_ndw_seen;

static char s_gateway[17];
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/*
 * A pending event report, picked up by the telemetry task.
 *
 * Set rather than sent directly, because the callers are event handlers:
 * ndw_telemetry_report() publishes over the network, and doing that on the
 * system event loop would block every other handler behind a socket write.
 */
static volatile bool s_event_pending;
static volatile ndw_telemetry_reason_t s_event_reason;

void ndw_telemetry_saw(const char *eui, int8_t rssi, bool is_ndw)
{
    /*
     * Called from the NimBLE host task on every advertisement, which on a
     * busy site is hundreds a second — so this holds a spinlock briefly
     * rather than taking a mutex, and does no allocation and no I/O.
     */
    taskENTER_CRITICAL(&s_lock);

    s_total_seen++;
    if (is_ndw) {
        s_ndw_seen++;
    }

    for (int i = 0; i < NEIGHBOURS_MAX; i++) {
        if (s_neighbours[i].eui[0] == '\0') {
            strlcpy(s_neighbours[i].eui, eui, sizeof(s_neighbours[i].eui));
            s_neighbours[i].rssi = rssi;
            s_neighbours[i].seen = 1;
            s_neighbours[i].is_ndw = is_ndw;
            taskEXIT_CRITICAL(&s_lock);
            return;
        }
        if (strcmp(s_neighbours[i].eui, eui) == 0) {
            /*
             * Strongest wins rather than most recent. RSSI varies several dB
             * between packets from a stationary device, and the best
             * observation is the better estimate of how well this gateway can
             * actually hear it.
             */
            if (rssi > s_neighbours[i].rssi) {
                s_neighbours[i].rssi = rssi;
            }
            s_neighbours[i].seen++;
            taskEXIT_CRITICAL(&s_lock);
            return;
        }
    }

    s_overflow++;
    taskEXIT_CRITICAL(&s_lock);
}

static const char *reason_name(ndw_telemetry_reason_t r)
{
    switch (r) {
    case NDW_TELEMETRY_BOOT:
        return "boot";
    case NDW_TELEMETRY_NETWORK:
        return "network";
    case NDW_TELEMETRY_OFFLINE:
        return "offline";
    default:
        return "periodic";
    }
}

void ndw_telemetry_report(ndw_telemetry_reason_t reason)
{
    /*
     * Built and queued whether or not the link is up.
     *
     * Reports made during an outage are the ones that explain it: a network
     * event recorded while disconnected is precisely the record of going
     * down. They carry uptime, so the server can place them in time despite
     * arriving late, and it decides what a delayed heartbeat means rather than
     * this box deciding for it by staying silent.
     */

    /* Copy under the lock so the scan callback is never blocked on I/O. */
    neighbour_t snapshot[NEIGHBOURS_MAX];
    uint16_t overflow;
    uint32_t total, ndw;

    taskENTER_CRITICAL(&s_lock);
    memcpy(snapshot, s_neighbours, sizeof(snapshot));
    overflow = s_overflow;
    total = s_total_seen;
    ndw = s_ndw_seen;
    memset(s_neighbours, 0, sizeof(s_neighbours));
    s_overflow = 0;
    s_total_seen = 0;
    s_ndw_seen = 0;
    taskEXIT_CRITICAL(&s_lock);

    /*
     * How many distinct devices were heard, as opposed to how many
     * advertisements arrived.
     *
     * The difference is large and it matters: a BLE device broadcasts several
     * times a second, so one phone in the room contributes hundreds to the
     * advertisement count over a five-minute window. Reporting only that left
     * a console saying "12226 devices in range" about a corridor with a
     * handful of laptops in it.
     *
     * Counted from the snapshot, outside the lock — the scan callback runs on
     * every advertisement and must not wait behind a loop that has all the
     * data it needs already copied.
     */
    uint16_t devices = 0;
    uint16_t ndw_devices = 0;
    for (int i = 0; i < NEIGHBOURS_MAX; i++) {
        if (snapshot[i].eui[0] != '\0') {
            devices++;
            if (snapshot[i].is_ndw) {
                ndw_devices++;
            }
        }
    }

    wifi_ap_record_t ap = {0};
    bool have_ap = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;

    char ip[16] = "";
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != NULL) {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(netif, &info) == ESP_OK) {
            snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
        }
    }

    /*
     * Sized for the header plus sixteen neighbours at ~40 bytes each. Built
     * with snprintf bounds throughout: a truncated JSON document would be
     * rejected by ingest as malformed, losing the whole report rather than
     * the one neighbour that did not fit.
     */
    static char body[1024];
    int n = snprintf(body, sizeof(body),
                     "{\"gatewayId\":\"%s\",\"reason\":\"%s\","
                     "\"uptime\":%llu,\"firmware\":\"%s\",\"heapFree\":%u,"
                     "\"wifi\":{\"ssid\":\"%s\",\"rssi\":%d,\"ip\":\"%s\"},"
                     /*
                      * Where this box is publishing, as it has it stored.
                      *
                      * Reported rather than assumed: the console prefills its
                      * own default when provisioning, but a box can have been
                      * given a different one, or be running a build with a
                      * different compiled fallback. Reading it back is the
                      * only way to be sure what a router is actually pointed
                      * at before changing it.
                      */
                     "\"broker\":\"%s\",\"brokerPending\":%s,"
                     /*
                      * Two different counts, deliberately both sent.
                      *
                      * seen/ndw are advertisements — traffic, useful for
                      * judging how busy the band is. devices/ndwDevices are
                      * distinct senders, which is what a person means by
                      * "what is in range". A device broadcasts several times
                      * a second, so the first pair is hundreds of times the
                      * second and reporting only it was misread as a device
                      * count.
                      *
                      * devices is a floor, not a total: the census table
                      * holds sixteen and overflow says how many arrivals it
                      * had no room for.
                      */
                     "\"radio\":{\"seen\":%lu,\"ndw\":%lu,"
                     "\"devices\":%u,\"ndwDevices\":%u,"
                     "\"published\":%lu,\"overflow\":%u},"
                     /*
                      * Queue health. A depth that is consistently non-trivial
                      * means this box is producing faster than its link can
                      * carry, and dropped counts messages already lost — both
                      * invisible from the server side, which by definition
                      * only sees what did arrive.
                      *
                      * Read at build time, so the depth is the queue before
                      * this report joins it.
                      */
                     "\"queue\":{\"depth\":%u,\"dropped\":%lu},"
                     "\"neighbours\":[",
                     s_gateway, reason_name(reason),
                     (unsigned long long)(esp_timer_get_time() / 1000000), NDW_VERSION,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
                     have_ap ? (const char *)ap.ssid : "", have_ap ? ap.rssi : 0, ip,
                     ndw_uplink_broker(), ndw_uplink_broker_pending() ? "true" : "false",
                     (unsigned long)total, (unsigned long)ndw, devices, ndw_devices,
                     (unsigned long)ndw_scan_count(), overflow,
                     (unsigned)ndw_queue_depth(), (unsigned long)ndw_queue_dropped());

    bool first = true;
    for (int i = 0; i < NEIGHBOURS_MAX && n > 0 && n < (int)sizeof(body); i++) {
        if (snapshot[i].eui[0] == '\0') {
            continue;
        }
        int w = snprintf(body + n, sizeof(body) - n, "%s{\"eui\":\"%s\",\"rssi\":%d,\"n\":%u%s}",
                         first ? "" : ",", snapshot[i].eui, snapshot[i].rssi, snapshot[i].seen,
                         snapshot[i].is_ndw ? ",\"ndw\":true" : "");
        if (w <= 0 || n + w >= (int)sizeof(body)) {
            /* Out of room. Stop cleanly rather than emit half an entry — the
               overflow count already says the list is incomplete. */
            break;
        }
        n += w;
        first = false;
    }

    if (n > 0 && n + 3 < (int)sizeof(body)) {
        n += snprintf(body + n, sizeof(body) - n, "]}");
        ndw_uplink_publish_telemetry(body, n);
        /* Both counts, because the gap between them is the thing worth
           seeing: hundreds of advertisements from a handful of devices is
           normal, and reading only the first number invites mistaking it for
           a device count. */
        ESP_LOGI(TAG, "telemetry (%s): %u devices (%u ndw), %lu adverts (%lu ndw)",
                 reason_name(reason), devices, ndw_devices, (unsigned long)total,
                 (unsigned long)ndw);
    }
}

void ndw_telemetry_notify(ndw_telemetry_reason_t reason)
{
    s_event_reason = reason;
    s_event_pending = true;
}

static void telemetry_task(void *arg)
{
    (void)arg;

    /*
     * A boot report, so a restart is visible as an event rather than inferred
     * from a gap in the heartbeat.
     *
     * The wait is for the network to settle, not for permission to send — the
     * queue would hold the report either way. It is so the report has an SSID
     * and an IP in it: a boot record that says the box came up with no network
     * is true for the first second of every boot and tells nobody anything.
     * Capped, so a box that never joins still reports that it booted.
     */
    for (int i = 0; i < 30 && !ndw_uplink_connected(); i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ndw_telemetry_report(NDW_TELEMETRY_BOOT);

    int64_t next = esp_timer_get_time() + (int64_t)PERIOD_MS * 1000;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        /*
         * Here rather than on its own timer: this task already wakes every
         * second, and a five-minute deadline does not justify a second thing
         * that has to be reasoned about during a reconnect.
         */
        ndw_uplink_check_broker_trial();

        if (s_event_pending) {
            s_event_pending = false;
            ndw_telemetry_report(s_event_reason);
            /* An event resets the clock: a heartbeat a second after a join
               report would say the same thing twice. */
            next = esp_timer_get_time() + (int64_t)PERIOD_MS * 1000;
            continue;
        }

        if (esp_timer_get_time() >= next) {
            next = esp_timer_get_time() + (int64_t)PERIOD_MS * 1000;
            ndw_telemetry_report(NDW_TELEMETRY_PERIODIC);
        }
    }
}

void ndw_telemetry_start(const char *gateway_eui)
{
    strlcpy(s_gateway, gateway_eui, sizeof(s_gateway));
    xTaskCreate(telemetry_task, "ndw-telem", 4096, NULL, 2, NULL);
    ESP_LOGI(TAG, "telemetry every %ds", PERIOD_MS / 1000);
}
