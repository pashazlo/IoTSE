#include "buttons.h"
#include "driver/gpio.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "buttons";

// ============================================================================
// Pin assignment — EDIT THESE to match your actual wiring.
// ============================================================================
#define BTN_UP_GPIO      41
#define BTN_DOWN_GPIO    40
#define BTN_LEFT_GPIO    39
#define BTN_RIGHT_GPIO   38
#define BTN_SELECT_GPIO  0

// Polling interval and debounce threshold. A transition must hold stable
// for DEBOUNCE_STABLE_COUNT consecutive polls before it's considered real —
// simple, robust, no interrupts needed. 20ms * 3 = 60ms worst-case latency,
// imperceptible for menu navigation.
#define POLL_INTERVAL_MS       20
#define DEBOUNCE_STABLE_COUNT  3

static const int s_gpio_for_button[BTN_COUNT] = {
    [BTN_UP]     = BTN_UP_GPIO,
    [BTN_DOWN]   = BTN_DOWN_GPIO,
    [BTN_LEFT]   = BTN_LEFT_GPIO,
    [BTN_RIGHT]  = BTN_RIGHT_GPIO,
    [BTN_SELECT] = BTN_SELECT_GPIO,
};

static QueueHandle_t s_event_queue = NULL;

// Debounced (confirmed) state, one bit per button — this is what
// buttons_is_pressed() reads.
static volatile bool s_confirmed_pressed[BTN_COUNT] = { false };

static void buttons_task(void *arg)
{
    bool raw_pressed[BTN_COUNT] = { false };
    uint8_t stable_count[BTN_COUNT] = { 0 };

    while (1) {
        for (int i = 0; i < BTN_COUNT; i++) {
            // Active-low with pull-up: level LOW == pressed.
            // If your buttons are wired active-high instead, swap this to:
            //   bool level_now = gpio_get_level(s_gpio_for_button[i]) == 1;
            bool level_now = gpio_get_level(s_gpio_for_button[i]) == 0;

            if (level_now == raw_pressed[i]) {
                // Same raw reading as last poll — no bounce happening,
                // but we only act once stable_count crosses the threshold
                // (handles the case where it was already stable before).
                if (stable_count[i] < DEBOUNCE_STABLE_COUNT) {
                    stable_count[i]++;
                }
            } else {
                // Raw level changed — restart the stability count.
                raw_pressed[i] = level_now;
                stable_count[i] = 1;
            }

            if (stable_count[i] == DEBOUNCE_STABLE_COUNT &&
                s_confirmed_pressed[i] != raw_pressed[i]) {
                // Confirmed transition — commit state and emit an event.
                s_confirmed_pressed[i] = raw_pressed[i];

                button_event_t evt = {
                    .button = (button_id_t)i,
                    .kind = raw_pressed[i] ? BTN_EVENT_PRESSED : BTN_EVENT_RELEASED,
                };
                // Non-blocking send: if the queue is full (consumer stalled),
                // drop the event rather than block the polling task.
                xQueueSend(s_event_queue, &evt, 0);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

esp_err_t buttons_init(void)
{
    s_event_queue = xQueueCreate(16, sizeof(button_event_t));
    ESP_RETURN_ON_FALSE(s_event_queue != NULL, ESP_ERR_NO_MEM, TAG, "queue alloc failed");

    uint64_t pin_mask = 0;
    for (int i = 0; i < BTN_COUNT; i++) {
        pin_mask |= (1ULL << s_gpio_for_button[i]);
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = pin_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,     // active-low wiring assumption
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,       // polling, not interrupt-driven
    };
    esp_err_t err = gpio_config(&io_conf);
    ESP_RETURN_ON_ERROR(err, TAG, "gpio_config failed");

    BaseType_t task_ok = xTaskCreate(buttons_task, "buttons", 2048, NULL, 5, NULL);
    ESP_RETURN_ON_FALSE(task_ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task create failed");

    ESP_LOGI(TAG, "Buttons initialized (UP=%d DOWN=%d LEFT=%d RIGHT=%d SELECT=%d)",
             BTN_UP_GPIO, BTN_DOWN_GPIO, BTN_LEFT_GPIO, BTN_RIGHT_GPIO, BTN_SELECT_GPIO);
    return ESP_OK;
}

QueueHandle_t buttons_get_queue(void)
{
    return s_event_queue;
}

bool buttons_is_pressed(button_id_t button)
{
    if (button >= BTN_COUNT) return false;
    return s_confirmed_pressed[button];
}
