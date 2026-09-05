```c
#pragma once

#include <stdint.h>

#include "ui_event.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Focus IDs
// ============================================================================

typedef enum {
    UI_FOCUS_MAIN = 0,
    UI_FOCUS_IR,
    UI_FOCUS_WIFI,
    UI_FOCUS_RF,
    UI_FOCUS_NRF,
    UI_FOCUS_BT,
    UI_FOCUS_SETTINGS,
} ui_focus_id_t;


// ============================================================================
// Focus API
// ============================================================================

/**
 * @brief Move focus according to UI event.
 *
 * UI_EVT_UP   -> previous item
 * UI_EVT_DOWN -> next item
 *
 * Focus wraps around at the beginning/end of the list.
 */
void ui_focus_move(
    ui_focus_id_t focus,
    uint8_t count,
    ui_event_t event
);


/**
 * @brief Get current selected item.
 */
uint8_t ui_focus_get(
    ui_focus_id_t focus
);


/**
 * @brief Set current selected item.
 */
void ui_focus_set(
    ui_focus_id_t focus,
    uint8_t selected
);


/**
 * @brief Reset one focus to the first item.
 */
void ui_focus_reset(
    ui_focus_id_t focus
);


/**
 * @brief Reset all focus positions.
 */
void ui_focus_reset_all(void);

#ifdef __cplusplus
}
#endif
```
