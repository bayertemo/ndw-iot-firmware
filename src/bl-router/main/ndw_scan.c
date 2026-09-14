#include "ndw_scan.h"

#include <string.h>

#include "esp_log.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"

#include "ndw_beacon.h"

static const char *TAG = "ndw-scan";

/*
 * Window and interval.
 *
 * Equal, so the radio listens continuously rather than in bursts. A sensor
 * broadcasts for ten seconds after a press and repeats every few hundred
 * milliseconds, so a duty-cycled scan would still catch it — but this router
 * is mains-powered and has no reason to save the energy, and continuous
 * listening is what keeps a one-shot beacon from a future battery sensor
 * from being missed.
 *
 * Units are 0.625ms, as the BLE spec counts them.
 */
#define SCAN_INTERVAL 0x0060 /* 60ms */
#define SCAN_WINDOW 0x0060   /* 60ms — equal, so 100% duty */

static ndw_frame_cb_t s_callback;
static volatile bool s_running;
static volatile uint32_t s_count;

/*
 * Pulls an NDW frame out of an advertisement, or reports that it is not one.
 *
 * Every check here is a way the payload could be somebody else's: the field
 * may be absent, too short, carry a different company identifier, or use a
 * beacon version this firmware predates. Treating any of those as ours would
 * mean forwarding garbage with a plausible EUI attached.
 */
static bool parse_frame(const struct ble_hs_adv_fields *fields, int8_t rssi, ndw_frame_t *out)
{
    if (fields->mfg_data == NULL || fields->mfg_data_len < NDW_BEACON_LEN) {
        return false;
    }

    const uint8_t *p = fields->mfg_data;

    uint16_t company = (uint16_t)p[NDW_OFF_COMPANY] | ((uint16_t)p[NDW_OFF_COMPANY + 1] << 8);
    if (company != NDW_COMPANY_ID) {
        return false;
    }

    /*
     * A newer sensor may lay the payload out differently, and reading it with
     * this firmware's offsets would produce a well-formed frame full of wrong
     * numbers — worse than dropping it, because nothing downstream could tell.
     */
    if (p[NDW_OFF_VERSION] != NDW_BEACON_VERSION) {
        ESP_LOGD(TAG, "ignoring beacon version %u", p[NDW_OFF_VERSION]);
        return false;
    }

    /* Big-endian on the wire, so it reads as the printed identifier. */
    for (int i = 0; i < 8; i++) {
        snprintf(&out->eui[i * 2], 3, "%02x", p[NDW_OFF_EUI + i]);
    }
    out->eui[16] = '\0';

    out->kind = p[NDW_OFF_KIND];
    out->counter = (uint32_t)p[NDW_OFF_COUNTER] | ((uint32_t)p[NDW_OFF_COUNTER + 1] << 8) |
                   ((uint32_t)p[NDW_OFF_COUNTER + 2] << 16) |
                   ((uint32_t)p[NDW_OFF_COUNTER + 3] << 24);
    out->uptime = (uint32_t)p[NDW_OFF_UPTIME] | ((uint32_t)p[NDW_OFF_UPTIME + 1] << 8) |
                  ((uint32_t)p[NDW_OFF_UPTIME + 2] << 16) |
                  ((uint32_t)p[NDW_OFF_UPTIME + 3] << 24);
    out->battery = p[NDW_OFF_BATTERY];
    out->rssi = rssi;

    return true;
}

static int on_scan_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields fields;
        if (ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data) != 0) {
            return 0;
        }

        ndw_frame_t frame;
        if (!parse_frame(&fields, event->disc.rssi, &frame)) {
            return 0;
        }

        s_count++;
        if (s_callback != NULL) {
            s_callback(&frame);
        }
        return 0;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE:
        /*
         * Should not happen — the scan is started with no duration — but a
         * router that quietly stopped listening would look identical to one
         * with nothing in range, which is the failure nobody notices.
         */
        ESP_LOGW(TAG, "scan ended unexpectedly (reason %d), restarting",
                 event->disc_complete.reason);
        s_running = false;
        ndw_scan_start(s_callback);
        return 0;

    default:
        return 0;
    }
}

void ndw_scan_start(ndw_frame_cb_t on_frame)
{
    s_callback = on_frame;

    if (s_running) {
        return;
    }

    struct ble_gap_disc_params params = {0};
    params.itvl = SCAN_INTERVAL;
    params.window = SCAN_WINDOW;
    /*
     * Passive. The router never sends a scan request: the whole payload is in
     * the advertisement, so there is nothing to ask for, and staying silent
     * halves the airtime this device costs everyone else on the channel.
     */
    params.passive = 1;
    /*
     * Duplicates are not filtered in the controller. A sensor repeats the
     * same counter for ten seconds and the pipeline dedups on (eui, counter)
     * anyway — but filtering here would also suppress a *new* reading from a
     * device that had just sent one, which for a counter is a lost press.
     */
    params.filter_duplicates = 0;
    params.filter_policy = BLE_HCI_SCAN_FILT_NO_WL;

    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &params, on_scan_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "could not start scanning: %d", rc);
        return;
    }

    s_running = true;
    ESP_LOGI(TAG, "scanning for beacons");
}

bool ndw_scan_running(void)
{
    return s_running;
}

uint32_t ndw_scan_count(void)
{
    return s_count;
}
