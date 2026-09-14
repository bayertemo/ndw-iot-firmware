#include "ndw_button.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ndw-button";

#define BUTTON_GPIO 9

/* How long the button must be held to open a pairing window. */
#define HOLD_MS 5000

/*
 * Polled rather than interrupt-driven.
 *
 * An interrupt would fire on every contact bounce and still need a timer to
 * measure a five-second hold, so it would be more moving parts for the same
 * answer. At 50ms a press cannot be missed by a human hand, and a bounce
 * lasting under one interval is invisible by construction.
 */
#define POLL_MS 50
#define HOLD_TICKS (HOLD_MS / POLL_MS)

static ndw_button_cb_t s_callback;

static void button_task(void *arg)
{
    (void)arg;

    int held = 0;
    bool fired = false;

    for (;;) {
        /* Active low: the pull-up holds it high, pressing grounds it. */
        bool down = gpio_get_level(BUTTON_GPIO) == 0;

        if (!down) {
            if (held > 0 && !fired) {
                ESP_LOGI(TAG, "released after %dms, too short", held * POLL_MS);
            }
            held = 0;
            fired = false;
        } else {
            held++;
            /*
             * Fires once on crossing the threshold. Latched until release, so
             * holding for ten seconds opens one window rather than two —
             * which also means the technician gets no feedback for keeping it
             * down, and that is fine: the LED changes the moment it fires.
             */
            if (held >= HOLD_TICKS && !fired) {
                fired = true;
                ESP_LOGI(TAG, "held %dms — opening a pairing window", HOLD_MS);
                if (s_callback != NULL) {
                    s_callback();
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void ndw_button_init(ndw_button_cb_t on_long_press)
{
    s_callback = on_long_press;

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        /*
         * The DevKitM-1 fits an external pull-up, but a bare module may not,
         * and a floating input would read as random presses. Enabling the
         * internal one costs nothing and makes the firmware work on both.
         */
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));

    xTaskCreate(button_task, "ndw-button", 2560, NULL, 3, NULL);
    ESP_LOGI(TAG, "hold GPIO%d for %dms to pair", BUTTON_GPIO, HOLD_MS);
}
