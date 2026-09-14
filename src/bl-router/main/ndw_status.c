#include "ndw_status.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

static const char *TAG = "ndw-status";

/*
 * The DevKitM-1 and DevKit-RUST both put a single WS2812 here. It is
 * addressable, not a plain LED: driving the pin high does nothing useful, the
 * colour arrives as serial data over RMT.
 */
#define LED_GPIO 8
#define LED_COUNT 1

/* 20ms gives smooth blinking without spending a core on it. */
#define TICK_MS 20

/*
 * Dim on purpose.
 *
 * A WS2812 at full scale is genuinely painful to look at from a bench, and
 * these are status lights rather than illumination. A twelfth of full scale
 * is still unmistakable across a plant room.
 */
#define DIM 20
#define BRIGHT 60

typedef struct {
    uint8_t r, g, b;
} rgb_t;

static const rgb_t OFF = {0, 0, 0};
static const rgb_t BLUE = {0, 0, BRIGHT};
static const rgb_t BLUE_DIM = {0, 0, DIM};
static const rgb_t GREEN = {0, DIM, 0};
/* Amber needs roughly twice as much red as green to not read as yellow. */
static const rgb_t AMBER = {BRIGHT, DIM, 0};
static const rgb_t RED = {BRIGHT, 0, 0};

static led_strip_handle_t s_strip;
static volatile ndw_led_t s_status = NDW_LED_IDLE;

/*
 * Pulse requests, counted rather than flagged.
 *
 * Beacons arrive in bursts, and a boolean would collapse five frames into one
 * blink — which is the opposite of what the light is for. A counter lets each
 * one be shown in turn, and caps the backlog so a flood does not leave the
 * LED blinking for a minute after the traffic stopped.
 */
#define PULSE_MAX 3
static volatile uint8_t s_pulses;

/* 0-100, how far through the hold. Drives the accelerating blink. */
static volatile uint8_t s_hold_percent;

static void show(rgb_t c)
{
    /* Null when init found no LED. The task is not started in that case, but
       guarding here too keeps this safe for any later caller. */
    if (s_strip == NULL) {
        return;
    }
    led_strip_set_pixel(s_strip, 0, c.r, c.g, c.b);
    led_strip_refresh(s_strip);
}

/*
 * Renders the current state for a given point in its cycle.
 *
 * `phase` is milliseconds since the state's cycle began, which keeps the
 * patterns declarative — each is a function of time rather than a sequence of
 * blocking sleeps, so a state change takes effect on the next tick instead of
 * after the current animation finishes.
 */
static rgb_t frame_for(ndw_led_t status, uint32_t phase)
{
    switch (status) {
    case NDW_LED_HOLDING: {
        /*
         * Speeds up from about 3Hz to 12Hz as the hold completes, so "nearly
         * there" is visible rather than implied. Solid at the threshold,
         * which is the moment the window opens.
         */
        if (s_hold_percent >= 100) {
            return BLUE;
        }
        uint32_t period = 320 - (s_hold_percent * 240 / 100);
        return (phase % period) < (period / 2) ? BLUE : OFF;
    }

    case NDW_LED_PAIRING:
        /* 1Hz: half a second on, half off. Reads as "looking for me". */
        return (phase % 1000) < 500 ? BLUE : OFF;

    case NDW_LED_LINKED:
        /*
         * Two quick flashes then a pause — the acknowledgement pattern a
         * paired device gives. Distinct from pairing's even blink at a
         * glance, which is the point: the technician needs to know the
         * console actually arrived.
         */
        if (phase < 120) return BLUE;
        if (phase < 240) return OFF;
        if (phase < 360) return BLUE;
        if (phase < 1400) return BLUE_DIM;
        return BLUE_DIM;

    case NDW_LED_JOINING:
        /* Faster than pairing: something is happening, wait. */
        return (phase % 400) < 200 ? AMBER : OFF;

    case NDW_LED_ONLINE:
        /* Steady. Nothing to attend to. */
        return GREEN;

    case NDW_LED_FAILED:
        /*
         * Long red, brief gap. Held rather than blinked quickly because a
         * fast red flash reads as activity; this needs to read as stopped.
         */
        return (phase % 2000) < 1600 ? RED : OFF;

    case NDW_LED_IDLE:
    default:
        return OFF;
    }
}

static void status_task(void *arg)
{
    (void)arg;

    ndw_led_t showing = s_status;
    uint32_t phase = 0;
    /* Counts down the remaining ticks of an in-flight pulse. */
    uint8_t pulse_ticks = 0;

    for (;;) {
        ndw_led_t now = s_status;
        if (now != showing) {
            /* Restart the cycle so a pattern always begins at its start. */
            showing = now;
            phase = 0;
        }

        if (pulse_ticks > 0) {
            pulse_ticks--;
            show(BLUE);
        } else if (s_pulses > 0) {
            s_pulses--;
            /* 60ms: long enough to register, short enough to count six a
               second without them merging into one. */
            pulse_ticks = 60 / TICK_MS;
            show(BLUE);
        } else {
            show(frame_for(showing, phase));
        }

        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
        phase += TICK_MS;
    }
}

void ndw_status_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        /* 2.5.5 spells this led_pixel_format; later releases renamed it to
           color_component_format. The dependency is pinned in
           idf_component.yml so this cannot change underneath us. */
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .flags = {.invert_out = false},
    };

    /*
     * RMT rather than SPI. Both can drive a WS2812, but the SPI backend
     * occupies a peripheral this board may want later, and one LED at 20ms
     * intervals is nothing to the RMT.
     */
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .flags = {.with_dma = false},
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        /*
         * Not fatal. A board without a WS2812 on GPIO8 should still route
         * frames — losing the status light is a worse diagnostic experience,
         * not a broken gateway.
         */
        ESP_LOGW(TAG, "no addressable LED on GPIO%d (%s); running dark", LED_GPIO,
                 esp_err_to_name(err));
        s_strip = NULL;
        return;
    }

    led_strip_clear(s_strip);

    /*
     * Logged on success, not only on failure. A silent success and a task
     * that never started look identical from the outside, and the first time
     * the LED stayed dark that ambiguity cost more to diagnose than this line
     * costs to print.
     */
    if (xTaskCreate(status_task, "ndw-status", 3072, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not start the status task; running dark");
        return;
    }
    ESP_LOGI(TAG, "status LED on GPIO%d", LED_GPIO);
}

static const char *led_name(ndw_led_t s)
{
    switch (s) {
    case NDW_LED_HOLDING: return "holding";
    case NDW_LED_PAIRING: return "pairing";
    case NDW_LED_LINKED:  return "linked";
    case NDW_LED_JOINING: return "joining";
    case NDW_LED_ONLINE:  return "online";
    case NDW_LED_FAILED:  return "failed";
    default:              return "idle";
    }
}

void ndw_status_set(ndw_led_t status)
{
    /*
     * Logged because the LED is the only thing this box can say without a
     * cable, which makes "it did not blink" impossible to diagnose otherwise
     * — there is no way to tell a state that was never set from one that was
     * set and not rendered.
     */
    if (status != s_status) {
        ESP_LOGI(TAG, "led -> %s", led_name(status));
    }
    s_status = status;
}

ndw_led_t ndw_status_get(void)
{
    return s_status;
}

void ndw_status_hold_progress(uint8_t percent)
{
    s_hold_percent = percent > 100 ? 100 : percent;
}

void ndw_status_pulse(void)
{
    if (s_pulses < PULSE_MAX) {
        s_pulses++;
    }
}
