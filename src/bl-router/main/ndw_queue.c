#include "ndw_queue.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "ndw_uplink.h"

static const char *TAG = "ndw-queue";

/*
 * Flush triggers.
 *
 * Whichever comes first, and the timer only runs when something is waiting —
 * an idle router does not wake up every five seconds to send nothing.
 *
 * Fifty is a batch worth the wakeup without being a batch worth losing: at the
 * ~65 frames a single press used to produce before dedupe, it is comfortably
 * more than one sensor's burst, so a busy moment fills it on count and a quiet
 * one goes out on the timer. Five seconds bounds how stale the freshest
 * reading can be when it finally lands, which matters because the server
 * stamps arrival — a longer window would widen the gap between when a press
 * happened and when the platform thinks it did.
 */
#define FLUSH_COUNT 50
#define FLUSH_MS 5000

/*
 * How many messages can wait.
 *
 * Deliberately deeper than the flush threshold, so a flush that cannot
 * complete — the broker is down — leaves room for the radio to keep feeding
 * the queue rather than dropping from the moment the link does. Two hundred
 * items is a couple of minutes of a busy site, or many hours of a quiet one.
 *
 * Bounded because the alternative is not "no loss", it is a heap exhaustion
 * that takes the whole box down and loses everything including the ability to
 * report that it happened.
 */
#define QUEUE_DEPTH 200

/*
 * An item is a pointer, not the message.
 *
 * A fixed 1KB slot per item would reserve 200KB of a ~320KB heap for a queue
 * that is nearly always empty. Bodies are allocated at their actual length —
 * a frame envelope is around 150 bytes, a telemetry document up to a
 * kilobyte — so the idle cost is the ring of pointers and nothing else.
 */
typedef struct {
    char topic[NDW_QUEUE_TOPIC_MAX];
    char *body;
    int len;
} item_t;

static QueueHandle_t s_queue;
static volatile uint32_t s_dropped;

static void free_item(item_t *item)
{
    free(item->body);
    item->body = NULL;
}

bool ndw_queue_push(const char *topic, const char *body, int len)
{
    if (s_queue == NULL || topic == NULL || body == NULL || len <= 0) {
        return false;
    }

    if (len > NDW_QUEUE_BODY_MAX) {
        /* Producers build into buffers this size or smaller, so this is a
           programming error rather than a runtime condition. */
        ESP_LOGE(TAG, "message of %d bytes exceeds the limit; dropping", len);
        s_dropped++;
        return false;
    }

    item_t item = {.len = len};
    strlcpy(item.topic, topic, sizeof(item.topic));

    item.body = malloc((size_t)len);
    if (item.body == NULL) {
        s_dropped++;
        return false;
    }
    memcpy(item.body, body, (size_t)len);

    /*
     * Never blocks. The callers are the NimBLE host task and the telemetry
     * task; blocking the first stalls the radio, and either one waiting on a
     * network that is already the reason the queue is full would deadlock
     * progress against itself.
     */
    if (xQueueSend(s_queue, &item, 0) == pdTRUE) {
        return true;
    }

    /*
     * Full. The oldest goes, not the newest.
     *
     * A queue that refuses new messages when full preserves the least useful
     * ones: during an outage the head is the stalest reading and the tail is
     * what just happened. Dropping from the front means a long outage
     * degrades to "the most recent two hundred messages", which is the subset
     * worth having.
     */
    item_t oldest;
    if (xQueueReceive(s_queue, &oldest, 0) == pdTRUE) {
        free_item(&oldest);
        s_dropped++;
    }

    if (xQueueSend(s_queue, &item, 0) != pdTRUE) {
        free_item(&item);
        s_dropped++;
        return false;
    }

    return false;
}

size_t ndw_queue_depth(void)
{
    return s_queue == NULL ? 0 : (size_t)uxQueueMessagesWaiting(s_queue);
}

uint32_t ndw_queue_dropped(void)
{
    return s_dropped;
}

/*
 * Sends what is waiting, stopping at the first failure.
 *
 * A failed publish means the link went down mid-flush, and the message that
 * failed is put back at the head rather than discarded — it has not been sent
 * and the queue's whole premise is that unsent messages wait. Returns the
 * number that went out.
 */
static int drain(void)
{
    int sent = 0;

    for (;;) {
        item_t item;
        if (xQueueReceive(s_queue, &item, 0) != pdTRUE) {
            break;
        }

        if (!ndw_uplink_send(item.topic, item.body, item.len)) {
            /*
             * Back to the front, so ordering survives a mid-flush drop. If
             * even that fails the queue filled behind us, and this one message
             * is lost rather than the newer ones that displaced it.
             */
            if (xQueueSendToFront(s_queue, &item, 0) != pdTRUE) {
                free_item(&item);
                s_dropped++;
            }
            break;
        }

        free_item(&item);
        sent++;
    }

    return sent;
}

static void sender_task(void *arg)
{
    (void)arg;

    for (;;) {
        /*
         * Sleeps until something arrives rather than polling. The peeked item
         * stays queued — this is only a wakeup, and taking it here would mean
         * carrying it separately through everything below.
         */
        item_t peek;
        if (xQueuePeek(s_queue, &peek, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /*
         * Something is waiting, so the clock starts now. This is where "every
         * 5s when the queue has anything" lives: the deadline is measured from
         * the first message of a batch, not from the last flush, so a message
         * arriving into an empty queue is never held longer than FLUSH_MS no
         * matter how long the router was idle beforehand.
         */
        int64_t deadline = esp_timer_get_time() + (int64_t)FLUSH_MS * 1000;

        while (uxQueueMessagesWaiting(s_queue) < FLUSH_COUNT &&
               esp_timer_get_time() < deadline) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        if (!ndw_uplink_connected()) {
            /*
             * Nothing to send into. Wait rather than drain into a closed
             * socket — the messages stay queued, which is the point, and the
             * next pass tries again. A second between attempts keeps a long
             * outage from spinning.
             */
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        int sent = drain();
        if (sent > 0) {
            ESP_LOGD(TAG, "flushed %d, %u waiting", sent,
                     (unsigned)uxQueueMessagesWaiting(s_queue));
        }
    }
}

void ndw_queue_start(void)
{
    if (s_queue != NULL) {
        return;
    }

    s_queue = xQueueCreate(QUEUE_DEPTH, sizeof(item_t));
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "could not create the queue");
        return;
    }

    xTaskCreate(sender_task, "ndw-send", 4096, NULL, 4, NULL);
    ESP_LOGI(TAG, "queue holds %d, flushing at %d or %dms", QUEUE_DEPTH, FLUSH_COUNT, FLUSH_MS);
}
