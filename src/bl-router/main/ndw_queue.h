/*
 * The one place outbound messages wait.
 *
 * Everything this router sends — relayed readings and its own telemetry —
 * goes through here rather than straight to the broker. Two reasons, and the
 * second is the one that matters:
 *
 *   Batching. A press produces a burst, and a site produces bursts from many
 *   sensors at once. Publishing each one the moment it is decoded means a
 *   socket write per frame, each with its own MQTT header, on a link that is
 *   often a marginal WiFi signal in a plant room.
 *
 *   A single drain point. When the producers published directly, "what
 *   happens when the broker is down" had to be answered separately in each of
 *   them, and it was answered differently: frames were dropped in publish(),
 *   telemetry was skipped in report(). One queue means one answer.
 *
 * That answer is: keep queuing. A reading is evidence and the counter is
 * cumulative, so a frame that arrives late is still worth having; the server
 * decides what a late message means, which it can do because it stamps
 * arrival itself and the payload carries the sensor's own uptime. The queue is
 * bounded, so an outage costs a fixed amount of memory rather than the heap.
 *
 * Producers never block and never publish. ndw_queue_push() is safe from the
 * NimBLE host task and from the system event loop — it copies into a
 * FreeRTOS queue and returns, so a slow or dead network cannot stall the
 * radio or wedge every other event handler behind a socket write.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Longest message the queue carries.
 *
 * Sized for a telemetry document with sixteen neighbours, which is the
 * largest thing this firmware produces; a frame envelope is about a third of
 * it. One size for both rather than a union, because the waste is bounded and
 * a variable-length queue would mean allocation on the radio's task.
 */
#define NDW_QUEUE_BODY_MAX 1024

/** Longest topic. ndw/<16>/gateway/<16>/telemetry plus room. */
#define NDW_QUEUE_TOPIC_MAX 80

/**
 * Starts the sender.
 *
 * Safe before WiFi or the broker exist: messages accumulate and go out when
 * the link comes up.
 */
void ndw_queue_start(void);

/**
 * Queues one message for publication.
 *
 * Returns false if the queue is full, having dropped the oldest message to
 * make room — the caller's message is still queued. False is worth logging as
 * a sign the link cannot keep up, but it is not a failure the caller can do
 * anything about.
 */
bool ndw_queue_push(const char *topic, const char *body, int len);

/** Messages waiting to be sent. */
size_t ndw_queue_depth(void);

/** Messages dropped for want of room since boot, reported in telemetry. */
uint32_t ndw_queue_dropped(void);
