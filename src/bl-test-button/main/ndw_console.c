#include "ndw_console.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ndw-console";

/* Shared with main.c, which owns the identity, the counter and the radio. */
extern const char *ndw_device_eui(void);
extern uint32_t ndw_device_counter(void);
extern void ndw_device_press(void);

#define NVS_NAMESPACE "ndw"
#define NVS_KEY_SECRET "key"

/*
 * One line in, one line out.
 *
 * Generous for a command and small enough that a stream of garbage — a board
 * printing a core dump into the same pipe — cannot grow the buffer without
 * bound. An over-long line is dropped to the next newline rather than
 * truncated and parsed.
 */
#define COMMAND_MAX 256
#define REPLY_MAX 256

/* Held in RAM once read, because the beacon path cannot afford an NVS read
   per press and the value never changes while the board is running. */
static uint8_t s_key[NDW_KEY_LEN];
static bool s_provisioned;

static void reply(const char *json)
{
    /* printf, not ESP_LOGx: a reply carries no level, no timestamp and no
       tag, and it must survive a log level that hides everything. */
    printf(NDW_REPLY_PREFIX "%s\n", json);
    fflush(stdout);
}

static void reply_fault(const char *field, const char *message)
{
    char body[REPLY_MAX];
    snprintf(body, sizeof(body),
             "{\"ok\":false,\"error\":\"key\",\"faults\":[{\"field\":\"%s\",\"message\":\"%s\"}],"
             "\"detail\":\"%s\"}",
             field, message, message);
    reply(body);
}

/* ---------------------------------------------------------------- storage */

static void load_key(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }

    size_t len = sizeof(s_key);
    s_provisioned = nvs_get_blob(nvs, NVS_KEY_SECRET, s_key, &len) == ESP_OK && len == NDW_KEY_LEN;
    nvs_close(nvs);

    if (s_provisioned) {
        ESP_LOGI(TAG, "key restored");
    }
}

static bool store_key(const uint8_t *key)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return false;
    }

    esp_err_t err = nvs_set_blob(nvs, NVS_KEY_SECRET, key, NDW_KEY_LEN);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (err != ESP_OK) {
        return false;
    }

    memcpy(s_key, key, NDW_KEY_LEN);
    s_provisioned = true;
    return true;
}

static void erase_key(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_key(nvs, NVS_KEY_SECRET);
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    memset(s_key, 0, sizeof(s_key));
    s_provisioned = false;
}

const uint8_t *ndw_console_key(void)
{
    return s_provisioned ? s_key : NULL;
}

bool ndw_console_provisioned(void)
{
    return s_provisioned;
}

/* --------------------------------------------------------------- commands */

static void on_status(void)
{
    char body[REPLY_MAX];
    /*
     * No key in here, in any form.
     *
     * Not the value, not a prefix of it, not a hash. The platform derives it
     * from a master and this device's EUI, so anything readable back would
     * make a board somebody borrowed for a minute into a way to forge its
     * readings afterwards.
     */
    snprintf(body, sizeof(body),
             "{\"ok\":true,\"eui\":\"%s\",\"firmware\":\"%s\",\"kind\":\"test-button\","
             "\"provisioned\":%s,\"count\":%lu}",
             ndw_device_eui(), NDW_FIRMWARE_VERSION, s_provisioned ? "true" : "false",
             (unsigned long)ndw_device_counter());
    reply(body);
}

/** One hex digit, or -1. */
static int unhex(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static void on_key(const cJSON *doc)
{
    const cJSON *hex = cJSON_GetObjectItemCaseSensitive(doc, "key");
    if (!cJSON_IsString(hex)) {
        reply_fault("key", "A key is required.");
        return;
    }

    const char *text = hex->valuestring;
    if (strlen(text) != NDW_KEY_LEN * 2) {
        reply_fault("key", "A key is 32 hex characters.");
        return;
    }

    uint8_t key[NDW_KEY_LEN];
    for (int i = 0; i < NDW_KEY_LEN; i++) {
        int hi = unhex(text[i * 2]);
        int lo = unhex(text[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            reply_fault("key", "That key is not hexadecimal.");
            return;
        }
        key[i] = (uint8_t)((hi << 4) | lo);
    }

    if (!store_key(key)) {
        reply_fault("key", "Could not store the key.");
        return;
    }

    ESP_LOGI(TAG, "provisioned");
    on_status();
}

/*
 * A press, without a button.
 *
 * The same path a real press takes — the counter moves, the reading is
 * sealed, the beacon goes out — reached over the cable instead of through a
 * switch. Two things need it. A board with nothing wired to BUTTON_GPIO
 * cannot be pressed at all, and even on a board that can, testing the radio
 * path should not depend on somebody being in the room.
 *
 * Not a way to inflate a reading: anyone who can send this already has the
 * board in their hands and could press it. It moves the counter by exactly
 * one, like the switch does.
 */
static void on_press(void)
{
    ndw_device_press();
    on_status();
}

static void on_forget(void)
{
    erase_key();
    ESP_LOGI(TAG, "key forgotten");
    on_status();
}

/* --------------------------------------------------------------- dispatch */

static void dispatch(char *line)
{
    cJSON *doc = cJSON_Parse(line);
    if (doc == NULL) {
        /* Silent: anything may be typed into a serial console, and a board
           that answered every stray keystroke would make its own transcript
           unreadable. */
        return;
    }

    const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(doc, "cmd");
    if (!cJSON_IsString(cmd)) {
        reply("{\"ok\":false,\"error\":\"command\",\"detail\":\"cmd is required\"}");
        cJSON_Delete(doc);
        return;
    }

    if (strcmp(cmd->valuestring, "hello") == 0 || strcmp(cmd->valuestring, "status") == 0) {
        /* One answer for both. A sensor's identity and its state are the same
           handful of fields, and two shapes would be two things to keep in
           step for no gain. */
        on_status();
    } else if (strcmp(cmd->valuestring, "key") == 0) {
        on_key(doc);
    } else if (strcmp(cmd->valuestring, "press") == 0) {
        on_press();
    } else if (strcmp(cmd->valuestring, "forget") == 0) {
        on_forget();
    } else {
        reply("{\"ok\":false,\"error\":\"command\",\"detail\":\"unknown command\"}");
    }

    cJSON_Delete(doc);
}

static void console_task(void *arg)
{
    (void)arg;

    static char line[COMMAND_MAX];
    size_t used = 0;
    bool overrun = false;

    ESP_LOGI(TAG, "console ready");

    for (;;) {
        int c = getchar();
        if (c == EOF) {
            /* Nothing attached. stdin is non-blocking on USB-Serial-JTAG when
               no host is reading, so this is the idle path rather than an
               error — sleep instead of spinning a core on it. */
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (c == '\r') {
            continue;
        }

        if (c == '\n') {
            if (overrun) {
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

void ndw_console_start(void)
{
    load_key();
    /* Small stack: cJSON parses into the heap, and the reply buffers are the
       only sizeable things on this one. */
    xTaskCreate(console_task, "ndw-console", 3584, NULL, 2, NULL);
}
