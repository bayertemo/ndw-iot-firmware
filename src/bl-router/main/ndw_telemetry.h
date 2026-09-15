/*
 * What a gateway says about itself.
 *
 * Distinct from the frames it relays, and deliberately so. A relayed frame is
 * a reading that belongs to somebody's account and may end up on an invoice;
 * telemetry is infrastructure data about the box doing the relaying. They
 * have different audiences, different retention, and different consequences
 * when wrong — so they travel on separate topics and land in separate tables.
 *
 * Published to ndw/<gateway>/gateway/<gateway>/telemetry, which deliberately
 * does not match the ndw/+/ble/+/up pattern ingest routes device frames on.
 * A new radio is a new pattern; a new *kind* of message is a new path.
 *
 * Two triggers, because the questions they answer are different:
 *
 *   periodic  — "is this box alive and healthy", answerable only by a
 *               message that keeps arriving. Silence is the signal.
 *   event     — "something changed just now": a join, a broker reconnect.
 *               A five-minute heartbeat would report these too late to act
 *               on and without saying what happened.
 *
 * Everything reported is already in memory. Nothing here turns on a radio,
 * takes a measurement, or costs airtime it would not otherwise spend — a
 * gateway that degraded its own scanning to talk about scanning would be
 * reporting on a problem it created.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/** Why this telemetry is being sent. Carried in the payload. */
typedef enum {
    /** The regular heartbeat. */
    NDW_TELEMETRY_PERIODIC,
    /** First message after boot, so a restart is visible as an event. */
    NDW_TELEMETRY_BOOT,
    /** The network came up: joined, rejoined, or the broker came back. */
    NDW_TELEMETRY_NETWORK,
    /**
     * The network went away while the box was on it.
     *
     * Distinct from NETWORK rather than folded into it, because the two are
     * read differently: a report saying the link is up arrives over that link
     * and is current, while this one is queued and delivered later, after the
     * link returns. Its wifi block is empty by definition — that is the
     * point, not missing data — and its uptime is what lets the server bound
     * when the gap began.
     */
    NDW_TELEMETRY_OFFLINE,
} ndw_telemetry_reason_t;

/**
 * Starts the heartbeat.
 *
 * Safe before WiFi or the broker exist: a report that cannot be sent is
 * skipped rather than queued, on the same reasoning as a dropped frame —
 * stale telemetry describes a state the box is no longer in.
 */
void ndw_telemetry_start(const char *gateway_eui);

/**
 * Sends one report now. Publishes on the calling task, so not for use from an
 * event handler — see ndw_telemetry_notify().
 */
void ndw_telemetry_report(ndw_telemetry_reason_t reason);

/**
 * Asks for a report soon, without publishing on this task.
 *
 * For event handlers. The WiFi and MQTT callbacks run on the system event
 * loop, and a network write there blocks every other handler behind it.
 */
void ndw_telemetry_notify(ndw_telemetry_reason_t reason);

/**
 * Records that a beacon was heard, for the neighbour census.
 *
 * Called from the scan callback on every advertisement — including ones that
 * are not ours. Knowing how much non-NDW traffic a site carries is what
 * distinguishes "no sensors in range" from "the band is saturated", and the
 * two need very different responses.
 */
void ndw_telemetry_saw(const char *eui, int8_t rssi, bool is_ndw);
