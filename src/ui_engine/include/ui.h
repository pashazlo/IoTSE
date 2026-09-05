#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// UI Initialization
// ============================================================================

/**
 * @brief Creates the internal UI event queue.
 *
 * Must be called before ui_task() and before any task sends UI events.
 */
esp_err_t ui_init(void);


// ============================================================================
// UI Task
// ============================================================================

/**
 * @brief Main UI FreeRTOS task.
 *
 * Handles rendering and UI events.
 */
void ui_task(void *arg);


// ============================================================================
// UI Event API
// ============================================================================

/**
 * @brief Send UI event from a normal FreeRTOS task.
 */
BaseType_t ui_send_event(ui_event_t evt);


/**
 * @brief Send UI event from an ISR.
 *
 * Currently buttons use polling, so this is reserved for future use.
 */
BaseType_t ui_send_event_from_isr(
    ui_event_t evt,
    BaseType_t *hp_task_woken
);


#ifdef __cplusplus
}
#endif
