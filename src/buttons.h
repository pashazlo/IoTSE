#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BTN_UP = 0,
    BTN_DOWN,
    BTN_LEFT,
    BTN_RIGHT,
    BTN_SELECT,
    BTN_COUNT   // keep last
} button_id_t;

typedef enum {
    BTN_EVENT_PRESSED,   // fired once on press (debounced)
    BTN_EVENT_RELEASED,  // fired once on release (debounced)
} button_event_kind_t;

typedef struct {
    button_id_t button;
    button_event_kind_t kind;
} button_event_t;

/**
 * @brief Configure the 5 GPIOs as debounced inputs and start the polling
 *        task that emits button_event_t into an internal queue.
 *
 * Assumes buttons wired active-low with internal pull-up (button press
 * pulls the pin to GND) — the common, simplest wiring. If yours are
 * active-high, flip the polarity check in buttons.c (see comment there).
 */
esp_err_t buttons_init(void);

/**
 * @brief Get the event queue. Call xQueueReceive() on it — with a
 *        timeout if you're combining button polling with other work in
 *        the same task loop (e.g. ui_task's periodic redraw), or
 *        portMAX_DELAY if a task exists solely to react to input.
 */
QueueHandle_t buttons_get_queue(void);

/**
 * @brief Poll the current (debounced) held/released state of a button
 *        directly, without going through the queue. Useful for "is SELECT
 *        currently held" checks outside the event stream.
 */
bool buttons_is_pressed(button_id_t button);

#ifdef __cplusplus
}
#endif
